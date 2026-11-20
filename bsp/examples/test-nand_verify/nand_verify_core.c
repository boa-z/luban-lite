/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <string.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/mtd_nand.h>
#include <aic_common.h>
#include <aic_core.h>
#include <drv_qspi.h>
#include "aic_time.h"
#include "nand_verify_core.h"

static struct rt_mtd_nand_device *g_nand = NULL;
static uint8_t *read_ptr = NULL;
static uint8_t *write_ptr = NULL;
static int g_test_block = -1;

/* Multiple sampling statistics */
#define MAX_SAMPLES 64
static int g_iterations = 5;
static uint64_t g_samples[MAX_SAMPLES];
static uint64_t g_wr_samples[MAX_SAMPLES];
static uint64_t g_er_samples[MAX_SAMPLES];

/* Sampling statistics output: avg / min / max.
 * Note: RT_KLIBC_USING_VSNPRINTF_LONGLONG is not enabled in this
 * build, so %llu cannot be used; print as u32 (us), one iteration
 * is far below the u32 limit.
 */
static void show_speed_stats(const char *msg, u32 bytes,
                             uint64_t *samples, int count)
{
    uint64_t min = UINT64_MAX, max = 0, sum = 0;
    uint64_t avg, speed;
    int i;

    for (i = 0; i < count; i++) {
        if (samples[i] < min)
            min = samples[i];
        if (samples[i] > max)
            max = samples[i];
        sum += samples[i];
    }
    avg = sum / count;
    speed = (uint64_t)bytes * 1000000ULL / (avg ? avg : 1) / 1024;
    rt_kprintf("%s: %u byte, avg=%lu us (min=%lu max=%lu) -> %lu KB/s (%d samples)\n",
               msg, bytes, (u32)avg, (u32)min, (u32)max, (u32)speed, count);
}

/* mtd read returning >0 means data was corrected but hit the bitflip
 * threshold (SPINAND_ECC_LIMIT) - read succeeded, just warn; <0 is
 * failure.
 */
static int check_read_ret(const char *what, int page, int ret)
{
    if (ret < 0) {
        rt_kprintf("%s failed at page %d, ret %d\n", what, page, ret);
        return -1;
    } else if (ret > 0) {
        rt_kprintf("  (WARN) %s at page %d, bitflip status %d\n", what, page, ret);
    }
    return 0;
}

static int find_good_block(void)
{
    int b;

    for (b = (int)(g_nand->block_end - g_nand->block_start) - 1; b >= 0; b--) {
        if (!rt_mtd_nand_check_block(g_nand, b))
            return b;
    }
    return -1;
}

static int count_good_blocks(void)
{
    int i, cnt = 0;

    for (i = (int)(g_nand->block_end - g_nand->block_start) - 1; i >= 0; i--) {
        if (!rt_mtd_nand_check_block(g_nand, i))
            cnt++;
    }
    return cnt;
}

int nand_verify_init(const char *dev_name)
{
    if (g_nand) {
        nand_verify_deinit();
    }

    g_nand = RT_MTD_NAND_DEVICE(rt_device_find(dev_name));
    if (!g_nand) {
        rt_kprintf("no nand device found: %s\n", dev_name);
        return -1;
    }

    read_ptr = rt_malloc_align(g_nand->page_size + g_nand->oob_size, CACHE_LINE_SIZE);
    write_ptr = rt_malloc_align(g_nand->page_size + g_nand->oob_size, CACHE_LINE_SIZE);
    if (!read_ptr || !write_ptr) {
        rt_kprintf("out of memory\n");
        nand_verify_deinit();
        return -1;
    }

    g_test_block = find_good_block();
    if (g_test_block < 0) {
        rt_kprintf("no good block found\n");
        nand_verify_deinit();
        return -1;
    }

    rt_kprintf("device: %s, page=%d B, block=%d KB, test_block=%d\n",
        dev_name, (int)g_nand->page_size,
        (int)(g_nand->page_size * g_nand->pages_per_block) / 1024,
        g_test_block);

    return 0;
}

void nand_verify_deinit(void)
{
    if (write_ptr) {
        rt_free_align(write_ptr);
        write_ptr = NULL;
    }
    if (read_ptr) {
        rt_free_align(read_ptr);
        read_ptr = NULL;
    }
    g_nand = NULL;
    g_test_block = -1;
}

int nand_verify_set_block(int block)
{
    int total_blocks;

    if (!g_nand)
        return -1;

    total_blocks = (int)(g_nand->block_end - g_nand->block_start);
    if (block < 0 || block >= total_blocks) {
        rt_kprintf("invalid block %d (total %d blocks)\n", block, total_blocks);
        return -1;
    }
    g_test_block = block;
    rt_kprintf("  using block %d (manual)\n", block);
    return 0;
}

int nand_verify_set_iterations(int n)
{
    if (n < 1)
        n = 1;
    if (n > MAX_SAMPLES)
        n = MAX_SAMPLES;
    g_iterations = n;
    return 0;
}

const char *nand_verify_dev_name(void)
{
    return g_nand ? g_nand->parent.parent.name : "N/A";
}

uint32_t nand_verify_page_size(void)
{
    return g_nand ? g_nand->page_size : 0;
}

uint32_t nand_verify_block_size(void)
{
    return g_nand ? g_nand->page_size * g_nand->pages_per_block : 0;
}

uint32_t nand_verify_capacity(void)
{
    if (!g_nand) return 0;
    return (uint32_t)(g_nand->block_end - g_nand->block_start)
        * g_nand->page_size * g_nand->pages_per_block;
}

/* --- single page --- */

int nand_verify_single_read(void)
{
    int i, n, base;

    if (!g_nand) return -1;

    n = g_nand->pages_per_block;
    base = g_test_block * n;

    for (i = 0; i < g_iterations; i++) {
        /* Read a different offset each time to avoid cache */
        int page = base + (i % n);
        int ret;
        uint64_t start_us = aic_get_time_us();

        ret = rt_mtd_nand_read(g_nand, page, read_ptr, g_nand->page_size, NULL, 0);
        if (check_read_ret("single page read", page, ret))
            return -1;
        g_samples[i] = aic_get_time_us() - start_us;
    }
    show_speed_stats("single page read", g_nand->page_size, g_samples, g_iterations);
    return 0;
}

int nand_verify_single_write(void)
{
    int i, page;

    if (!g_nand) return -1;

    page = g_test_block * g_nand->pages_per_block;

    for (i = 0; i < g_iterations; i++) {
        uint64_t start_us;

        memset(write_ptr, 0xA5, g_nand->page_size);
        if (rt_mtd_nand_erase_block(g_nand, g_test_block)) {
            rt_kprintf("single page write: erase prep failed at iter %d\n", i);
            return -1;
        }

        start_us = aic_get_time_us();
        if (rt_mtd_nand_write(g_nand, page, write_ptr, g_nand->page_size, NULL, 0)) {
            rt_kprintf("single page write failed\n");
            return -1;
        }
        g_samples[i] = aic_get_time_us() - start_us;
    }
    show_speed_stats("single page write", g_nand->page_size, g_samples, g_iterations);
    return 0;
}

/* --- multi page (one block) --- */

int nand_verify_multi_read(void)
{
    uint32_t i, n, base;
    int iter;

    if (!g_nand) return -1;

    n = g_nand->pages_per_block;
    base = g_test_block * n;

    for (iter = 0; iter < g_iterations; iter++) {
        uint64_t start_us = aic_get_time_us();

        for (i = 0; i < n; i++) {
            int ret = rt_mtd_nand_read(g_nand, base + i, read_ptr,
                                       g_nand->page_size, NULL, 0);
            if (check_read_ret("multi page read", (int)(base + i), ret))
                return -1;
        }
        g_samples[iter] = aic_get_time_us() - start_us;
    }
    show_speed_stats("multi page read", n * g_nand->page_size,
                     g_samples, g_iterations);
    return 0;
}

int nand_verify_multi_write(void)
{
    uint32_t i, n, base;
    int iter;

    if (!g_nand) return -1;

    n = g_nand->pages_per_block;
    base = g_test_block * n;

    memset(write_ptr, 0xA5, g_nand->page_size);

    for (iter = 0; iter < g_iterations; iter++) {
        uint64_t start_us;

        if (rt_mtd_nand_erase_block(g_nand, g_test_block)) {
            rt_kprintf("multi page write: erase prep failed at iter %d\n", iter);
            return -1;
        }

        start_us = aic_get_time_us();
        for (i = 0; i < n; i++) {
            if (rt_mtd_nand_write(g_nand, base + i, write_ptr,
                                  g_nand->page_size, NULL, 0)) {
                rt_kprintf("multi page write failed at page %d\n", (int)i);
                return -1;
            }
        }
        g_samples[iter] = aic_get_time_us() - start_us;
    }
    show_speed_stats("multi page write", n * g_nand->page_size,
                     g_samples, g_iterations);
    return 0;
}

/* --- block erase --- */

int nand_verify_erase(void)
{
    u32 size;
    int i;

    if (!g_nand) return -1;

    size = g_nand->page_size * g_nand->pages_per_block;

    for (i = 0; i < g_iterations; i++) {
        uint64_t start_us = aic_get_time_us();

        if (rt_mtd_nand_erase_block(g_nand, g_test_block)) {
            rt_kprintf("block erase failed\n");
            return -1;
        }
        g_samples[i] = aic_get_time_us() - start_us;
    }
    show_speed_stats("erase 1 block", size, g_samples, g_iterations);
    return 0;
}

/* --- 1MB R/W --- */

int nand_verify_large_read(void)
{
    u32 target = 1024 * 1024, block_size;
    int nblocks, iter, b, j;

    if (!g_nand) return -1;

    block_size = g_nand->page_size * g_nand->pages_per_block;
    nblocks = (int)(g_nand->block_end - g_nand->block_start);

    if ((u32)count_good_blocks() * block_size < target) {
        rt_kprintf("not enough good blocks for 1MB read\n");
        return -1;
    }

    for (iter = 0; iter < g_iterations; iter++) {
        u32 size = 0;
        uint64_t start_us = aic_get_time_us();

        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            for (j = 0; j < (int)g_nand->pages_per_block && size < target; j++) {
                int page = b * g_nand->pages_per_block + j;
                int ret = rt_mtd_nand_read(g_nand, page, read_ptr,
                                           g_nand->page_size, NULL, 0);
                if (check_read_ret("1MB read", page, ret))
                    return -1;
                size += g_nand->page_size;
            }
        }
        g_samples[iter] = aic_get_time_us() - start_us;
    }
    show_speed_stats("1MB read", target, g_samples, g_iterations);
    return 0;
}

int nand_verify_large_write(void)
{
    u32 target = 1024 * 1024, block_size;
    int nblocks, iter, b, j;

    if (!g_nand) return -1;

    block_size = g_nand->page_size * g_nand->pages_per_block;
    nblocks = (int)(g_nand->block_end - g_nand->block_start);

    if ((u32)count_good_blocks() * block_size < target) {
        rt_kprintf("not enough good blocks for 1MB write\n");
        return -1;
    }

    memset(write_ptr, 0xA5, g_nand->page_size);

    for (iter = 0; iter < g_iterations; iter++) {
        u32 size = 0;
        uint64_t start_us;

        /* Erase all needed blocks first (untimed, preparation only) */
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            if (rt_mtd_nand_erase_block(g_nand, b) == 0)
                size += block_size;
        }
        if (size < target) {
            rt_kprintf("not enough erasable blocks, got %u B\n", size);
            return -1;
        }

        /* Write (timed) */
        size = 0;
        start_us = aic_get_time_us();
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            for (j = 0; j < (int)g_nand->pages_per_block && size < target; j++) {
                if (rt_mtd_nand_write(g_nand, b * g_nand->pages_per_block + j,
                                      write_ptr, g_nand->page_size, NULL, 0)) {
                    rt_kprintf("1MB write failed\n");
                    return -1;
                }
                size += g_nand->page_size;
            }
        }
        g_samples[iter] = aic_get_time_us() - start_us;
    }
    show_speed_stats("1MB write", target, g_samples, g_iterations);
    return 0;
}

/* --- data verify --- */

int nand_verify_data(void)
{
    int i, page, ret;

    if (!g_nand) return -1;

    for (i = 0; i < (int)g_nand->page_size; i++)
        write_ptr[i] = (uint8_t)(i * 0x11);

    rt_mtd_nand_erase_block(g_nand, g_test_block);
    page = g_test_block * g_nand->pages_per_block;
    if (rt_mtd_nand_write(g_nand, page, write_ptr, g_nand->page_size, NULL, 0)) {
        rt_kprintf("verify write failed\n");
        return -1;
    }

    memset(read_ptr, 0, g_nand->page_size);
    ret = rt_mtd_nand_read(g_nand, page, read_ptr, g_nand->page_size, NULL, 0);
    if (check_read_ret("verify read", page, ret))
        return -1;

    for (i = 0; i < (int)g_nand->page_size; i++) {
        if (write_ptr[i] != read_ptr[i]) {
            rt_kprintf("data check error at byte %d\n", i);
            return -1;
        }
    }
    rt_kprintf("data check OK\n");
    return 0;
}

/* ---------- Cycle test: write -> read+verify -> erase ---------- */

/* Single page cycle */
static int cycle_page(void)
{
    u32 page;
    int i;

    if (!g_nand) return -1;

    page = g_test_block * g_nand->pages_per_block;

    for (i = 0; i < g_iterations; i++) {
        uint64_t t;
        int ret;

        memset(write_ptr, 0xA5, g_nand->page_size);

        /* Prepare: erase block */
        if (rt_mtd_nand_erase_block(g_nand, g_test_block)) {
            rt_kprintf("page cycle: erase prep failed at iter %d\n", i);
            return -1;
        }

        /* Write 1 page (timed) */
        t = aic_get_time_us();
        if (rt_mtd_nand_write(g_nand, page, write_ptr,
                              g_nand->page_size, NULL, 0)) {
            rt_kprintf("page cycle: write failed at iter %d\n", i);
            return -1;
        }
        g_samples[i] = aic_get_time_us() - t;

        /* Read 1 page (timed) */
        t = aic_get_time_us();
        ret = rt_mtd_nand_read(g_nand, page, read_ptr,
                               g_nand->page_size, NULL, 0);
        if (check_read_ret("page cycle read", (int)page, ret))
            return -1;
        g_wr_samples[i] = aic_get_time_us() - t;

        /* Verify */
        if (memcmp(write_ptr, read_ptr, g_nand->page_size)) {
            rt_kprintf("page cycle: data mismatch at iter %d\n", i);
            return -1;
        }
    }

    rt_kprintf("--- Cycle: 1 Page ---\n");
    show_speed_stats("write", g_nand->page_size, g_samples, g_iterations);
    show_speed_stats("read",  g_nand->page_size, g_wr_samples, g_iterations);
    rt_kprintf("  data verify: OK\n");
    return 0;
}

/* Whole block cycle: write/read all pages */
static int cycle_block(void)
{
    u32 i, n, base;
    int iter;

    if (!g_nand) return -1;

    n = g_nand->pages_per_block;
    base = g_test_block * n;

    for (iter = 0; iter < g_iterations; iter++) {
        uint64_t t;
        int ret;

        memset(write_ptr, 0xA5, g_nand->page_size);

        /* Prepare: erase block */
        if (rt_mtd_nand_erase_block(g_nand, g_test_block)) {
            rt_kprintf("block cycle: erase prep failed at iter %d\n", iter);
            return -1;
        }

        /* Write all pages (timed) */
        t = aic_get_time_us();
        for (i = 0; i < n; i++) {
            if (rt_mtd_nand_write(g_nand, base + i, write_ptr,
                                  g_nand->page_size, NULL, 0)) {
                rt_kprintf("block cycle: write failed at iter %d\n", iter);
                return -1;
            }
        }
        g_samples[iter] = aic_get_time_us() - t;

        /* Read all pages (timed) */
        t = aic_get_time_us();
        for (i = 0; i < n; i++) {
            ret = rt_mtd_nand_read(g_nand, base + i, read_ptr,
                                   g_nand->page_size, NULL, 0);
            if (check_read_ret("block cycle read", (int)(base + i), ret))
                return -1;
        }
        g_wr_samples[iter] = aic_get_time_us() - t;

        /* Verify: all pages share pattern 0xA5, only check the last page */
        if (memcmp(write_ptr, read_ptr, g_nand->page_size)) {
            rt_kprintf("block cycle: data mismatch at iter %d\n", iter);
            return -1;
        }
    }

    rt_kprintf("--- Cycle: 1 Block (%u KB) ---\n",
               g_nand->page_size * n / 1024);
    show_speed_stats("write", g_nand->page_size * n, g_samples, g_iterations);
    show_speed_stats("read",  g_nand->page_size * n, g_wr_samples, g_iterations);
    rt_kprintf("  data verify: OK\n");
    return 0;
}

/* 1MB cycle */
static int cycle_1mb(void)
{
    u32 target = 1024 * 1024;
    u32 block_size = g_nand->page_size * g_nand->pages_per_block;
    int nblocks = (int)(g_nand->block_end - g_nand->block_start);
    int iter, b, j;

    if (!g_nand) return -1;

    if ((u32)count_good_blocks() * block_size < target) {
        rt_kprintf("not enough good blocks for 1MB cycle\n");
        return 0; /* Non-fatal, skip */
    }

    for (iter = 0; iter < g_iterations; iter++) {
        u32 size = 0;
        uint64_t t;

        /* Prepare: erase all needed blocks (untimed) */
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            if (rt_mtd_nand_erase_block(g_nand, b) == 0)
                size += block_size;
        }
        if (size < target) {
            rt_kprintf("not enough erasable blocks for 1MB\n");
            return -1;
        }

        /* Write 1MB (timed) */
        size = 0;
        t = aic_get_time_us();
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            for (j = 0; j < (int)g_nand->pages_per_block && size < target; j++) {
                if (rt_mtd_nand_write(g_nand, b * g_nand->pages_per_block + j,
                                      write_ptr, g_nand->page_size, NULL, 0)) {
                    rt_kprintf("1MB cycle: write failed at iter %d\n", iter);
                    return -1;
                }
                size += g_nand->page_size;
            }
        }
        g_samples[iter] = aic_get_time_us() - t;

        /* Read 1MB (timed) */
        size = 0;
        t = aic_get_time_us();
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            for (j = 0; j < (int)g_nand->pages_per_block && size < target; j++) {
                if (rt_mtd_nand_read(g_nand, b * g_nand->pages_per_block + j,
                                     read_ptr, g_nand->page_size, NULL, 0) < 0) {
                    rt_kprintf("1MB cycle: read failed at iter %d\n", iter);
                    return -1;
                }
                size += g_nand->page_size;
            }
        }
        g_wr_samples[iter] = aic_get_time_us() - t;

        /* Verify: all pages share pattern, only check one page */
        if (memcmp(write_ptr, read_ptr, g_nand->page_size)) {
            rt_kprintf("1MB cycle: data mismatch at iter %d\n", iter);
            return -1;
        }

        /* Erase 1MB data area (timed) */
        size = 0;
        t = aic_get_time_us();
        for (b = nblocks - 1; b >= 0 && size < target; b--) {
            if (rt_mtd_nand_check_block(g_nand, b))
                continue;
            if (rt_mtd_nand_erase_block(g_nand, b) == 0)
                size += block_size;
        }
        g_er_samples[iter] = aic_get_time_us() - t;
    }

    rt_kprintf("--- Cycle: 1 MB ---\n");
    show_speed_stats("write", target, g_samples, g_iterations);
    show_speed_stats("read",  target, g_wr_samples, g_iterations);
    show_speed_stats("erase", target, g_er_samples, g_iterations);
    rt_kprintf("  data verify: OK\n");
    return 0;
}

int nand_verify_cycle(void)
{
    if (!g_nand) return -1;

    if (cycle_page())
        return -1;
    rt_kprintf("\n");
    if (cycle_block())
        return -1;
    rt_kprintf("\n");
    cycle_1mb(); /* 1MB failure does not block other results */
    rt_kprintf("\n");
    return 0;
}

/* --- OOB verify --- */

/* Probe actual user OOB size — map_user fails if nbytes exceeds layout */
static int probe_oob_size(int oob_free)
{
    uint8_t probe_oob[64], probe_spare[32];

    memset(probe_spare, 0, sizeof(probe_spare));
    while (oob_free > 0) {
        memset(probe_oob, 0xFF, sizeof(probe_oob));
        if (rt_mtd_nand_map_user(g_nand, probe_oob, probe_spare, 0, oob_free) == 0)
            break;
        oob_free--;
    }
    return oob_free;
}

int nand_verify_oob(void)
{
    int i, page, oob_free;
    uint8_t *w_oob, *r_oob;
    uint8_t w_spare[32], r_spare[32];

    if (!g_nand) return -1;

    oob_free = g_nand->oob_free;
    if (oob_free > (int)sizeof(w_spare))
        oob_free = (int)sizeof(w_spare);
    if (oob_free == 0) {
        rt_kprintf("no OOB area available\n");
        return 0;
    }

    w_oob = write_ptr + g_nand->page_size;
    r_oob = read_ptr + g_nand->page_size;
    page = g_test_block * g_nand->pages_per_block;

    oob_free = probe_oob_size(oob_free);
    if (oob_free == 0) {
        rt_kprintf("no OOB area available\n");
        return 0;
    }

    /* Init entire OOB buffer to 0xFF so non-user bytes keep erased state */
    memset(w_oob, 0xFF, g_nand->oob_size);

    rt_mtd_nand_erase_block(g_nand, g_test_block);

    for (i = 0; i < oob_free; i++)
        w_spare[i] = (uint8_t)(i * 0x11);
    w_spare[0] = 0xFF;

    for (i = 0; i < 10; i++) {
        int p = page + i;
        int ret;

        ret = rt_mtd_nand_map_user(g_nand, w_oob, w_spare, 0, oob_free);
        if (ret) {
            rt_kprintf("OOB map_user failed at page %d\n", p);
            return -1;
        }
        ret = rt_mtd_nand_write(g_nand, p, write_ptr, g_nand->page_size,
                                w_oob, g_nand->oob_size);
        if (ret) {
            rt_kprintf("OOB write failed at page %d\n", p);
            return -1;
        }

        memset(r_oob, 0, g_nand->oob_size);
        memset(r_spare, 0, sizeof(r_spare));
        ret = rt_mtd_nand_read(g_nand, p, read_ptr, g_nand->page_size,
                               r_oob, g_nand->oob_size);
        if (ret < 0) {
            rt_kprintf("OOB read failed at page %d\n", p);
            return -1;
        } else if (ret > 0) {
            rt_kprintf("  (WARN) OOB read at page %d, bitflip status %d\n", p, ret);
        }

        ret = rt_mtd_nand_unmap_user(g_nand, r_spare, r_oob, 0, oob_free);
        if (ret) {
            rt_kprintf("OOB unmap_user failed at page %d\n", p);
            return -1;
        }
        for (int j = 0; j < oob_free; j++) {
            if (w_spare[j] != r_spare[j]) {
                rt_kprintf("OOB data check error at byte %d, page %d\n", j, p);
                return -1;
            }
        }
    }

    rt_kprintf("OOB data check OK (%d bytes x 10 pages)\n", oob_free);
    return 0;
}

/* --- phase scan --- */

#define INT_TO_DELAY_MODE(x) \
    (((x) == 0) ? "auto" : \
     ((x) == 1) ? "no delay" : \
     ((x) == 2) ? "delay half cycle" : \
     ((x) == 3) ? "delay one cycle" : "unknown")

static int g_phase_ok[4];

int nand_verify_phase(uint32_t phase)
{
    struct rt_qspi_device *qspi;
    struct rt_qspi_configuration qspi_cfg;
    struct rt_qspi_message msg;
    uint8_t data_r[3] = {0x01, 0x02, 0x03};
    int ret, i, j;

    if (!g_nand) return -1;
    if (phase >= ARRAY_SIZE(g_phase_ok)) return -1;

    qspi = (struct rt_qspi_device *)rt_device_find("flash_verify");
    if (!qspi) {
        ret = aic_qspi_bus_attach_device("qspi0", "flash_verify", 0, 4, RT_NULL, RT_NULL);
        if (ret != RT_EOK) {
            rt_kprintf("attach qspi failed\n");
            return -1;
        }
        qspi = (struct rt_qspi_device *)rt_device_find("flash_verify");
        if (!qspi) return -1;
    }

    rt_memset(&qspi_cfg, 0, sizeof(qspi_cfg));
    qspi_cfg.qspi_dl_width = 4;
    qspi_cfg.parent.mode = 0;
    qspi_cfg.parent.max_hz = 50000000;

    ret = rt_qspi_configure(qspi, &qspi_cfg);
    if (ret < 0) return -1;

    rt_spi_set_rx_delay_mode((struct rt_spi_device *)qspi, phase);

    rt_memset(&msg, 0, sizeof(msg));
    msg.instruction.content = 0x01;
    msg.instruction.qspi_lines = 1;
    msg.qspi_data_lines = 1;
    msg.parent.send_buf = data_r;
    msg.parent.length = 3;
    msg.parent.cs_take = 1;
    msg.parent.cs_release = 1;

    rt_spi_take_bus((struct rt_spi_device *)qspi);
    rt_qspi_transfer_message(qspi, &msg);
    rt_spi_release_bus((struct rt_spi_device *)qspi);

    for (i = 0; i < (int)g_nand->page_size; i++)
        write_ptr[i] = (uint8_t)((i * 3 + 0x5A) & 0xFF);

    rt_mtd_nand_erase_block(g_nand, g_test_block);
    rt_mtd_nand_write(g_nand, g_test_block * g_nand->pages_per_block,
        write_ptr, g_nand->page_size, NULL, 0);

    memset(read_ptr, 0, g_nand->page_size);
    rt_mtd_nand_read(g_nand, g_test_block * g_nand->pages_per_block,
        read_ptr, g_nand->page_size, NULL, 0);

    for (j = 0; j < (int)g_nand->page_size; j++) {
        if (read_ptr[j] != write_ptr[j]) {
            g_phase_ok[phase] = 0;
            return -1;
        }
    }

    g_phase_ok[phase] = 1;
    return 0;
}

int nand_verify_phase_scan(void)
{
    int i;

    for (i = 1; i <= 3; i++) {
        nand_verify_phase(i);
    }

    rt_kprintf("\n -------------------------------------------------------------- \n");
    for (i = 1; i <= 3; i++) {
        rt_kprintf("[phase scan] delay mode: [%s] test %s!\n",
            INT_TO_DELAY_MODE(i), g_phase_ok[i] ? "Success" : "Failed");
    }
    rt_kprintf("-------------------------------------------------------------- \n");

    nand_verify_phase(0);
    return 0;
}
