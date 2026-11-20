/*
 * Copyright (C) 2022-2026, ArtInChip Technology Co.,Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: Dehuang Wu <dehuang.wu@artinchip.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aic_common.h>
#include <aic_image.h>
#include <boot_param.h>
#include <private_param.h>
#include <aic_log.h>
#include <aic_osal.h>

#define IMAGE_BACKUP_BLOCK_CNT 2
#define PAGE_CNT_PER_BLOCK     64
#define PAGE_TABLE_MAX_ENTRY     (PAGE_CNT_PER_BLOCK * 2 - 1) // 127 * 2k = 254k or 127 * 4k = 508k
#define PAGE_DEFAULT_SIZE      2048
#define FSBL_CANDIDATE_BLK_CNT 18

#define SLICE_DEFAULT_SIZE        2048
#define PAGE_TABLE_USE_SIZE       2048
#define PAGE_MAX_SIZE             4096
#define SPL_CANDIDATE_BLOCK_NUM   18
#define SPL_INVALID_BLOCK_IDX     0xFFFFFFFF
#define SPL_INVALID_PAGE_ADDR     0xFFFFFFFF

#define SPINAND_IMAGE_BACKUP_0 0
#define SPINAND_IMAGE_BACKUP_1 1
#define SPINAND_IMAGE_BACKUP_2 2

#define RTC_AP_BOOTINFO_REG       0x19030134
#define RTC_BOOTINFO_IMGID_MSK    (0xF << 4)
#define RTC_BOOTINFO_IMGID_OFF    (4)
#define AIC_PDAT_MAGIC            0x54414450      /* Magic is 'PDAT ' */

#define ROUNDUP(a, b) ((((a)-1) / (b) + 1) * (b))

struct nand_page_table_head {
    char magic[4]; /* AICP: AIC Page table */
    u16 entry_cnt;
    u16 page_size;
    u8  pages_per_block;
    u8  blocks; /* Block number in SPI NAND */
    u8  planes; /* Plane number in SPI NAND: Max 2 in market avaialbe devices */
    u8  pad; /* Padding it to fit size 12 bytes */
};

struct nand_page_table_entry {
    u32 pageaddr[IMAGE_BACKUP_BLOCK_CNT]; /* Page address in good block */
    u32 checksum;                         /* Page data checksum value */
};

struct nand_page_table {
    struct nand_page_table_head head;
    struct nand_page_table_entry entry[PAGE_TABLE_MAX_ENTRY];
};

struct private_data2_header {
    u32 magic;
    u32 cksum;
    u32 ver;
    u32 sign_offset;
    u8 pad[16];
};

static u32 spl_candidate_block_table[SPL_CANDIDATE_BLOCK_NUM] = {
    0,   1,   2,   3,   202, 32,  312, 296, 142,
    136, 392, 526, 452, 708, 810, 552, 906, 674,
};

static u32 calc_page_checksum(u8 *start, u32 size)
{
    u8 *p;
    u32 i, val, sum, rest, cnt;

    p = start;
    i = 0;
    sum = 0;
    cnt = size >> 2;

    for (i = 0; i < cnt; i++) {
        p = &start[i * 4];
        val = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        sum += val;
    }

    /* Calculate not 32 bit aligned part */
    rest = size - (cnt << 2);
    p = &start[cnt * 4];
    val = 0;
    for (i = 0; i < rest; i++)
        val += (p[i] << (i * 8));
    sum += val;

    return sum;
}

static s32 get_nand_page_table(nand_read fn, void *dev, unsigned long pagesize,
                               struct nand_page_table *pt)
{
    u32 i, blkidx, pa, sumval;
    s32 ret = -1;
    u32 *table = spl_candidate_block_table;
    u8 *page_data = NULL;
    ulong offset;

    page_data = malloc(PAGE_DEFAULT_SIZE);
    if (page_data == NULL)
        return -1;
    /* Search in alternative blocks */
    for (i = 0; i < FSBL_CANDIDATE_BLK_CNT; i++) {
        blkidx = table[i];
        pa = (blkidx << 6) + 0; /* First page in block */
        offset = pa * pagesize;
        ret = fn(dev, offset, page_data, PAGE_DEFAULT_SIZE, 1);
        if (ret < 0) {
            pr_err("Read from block = %d, pa = 0x%x failed.\n", blkidx, pa);
            continue;
        }
        sumval = calc_page_checksum(page_data, PAGE_DEFAULT_SIZE);
        if (sumval != 0xFFFFFFFF) {
            ret = -1;
            pr_err("Checksum of block = %d, pa = 0x%x failed.\n", blkidx, pa);
            continue;
        }

        memcpy(pt, page_data, sizeof(struct nand_page_table));
        if (strncmp(pt->head.magic, "AICP", 4) != 0) {
            ret = -1;
            pr_err("AIC Head of block = %d, pa = 0x%x failed.\n", blkidx, pa);
            continue;
        } else {
            ret = 0;
            goto out;
        }
    }

out:
    if (page_data)
        free(page_data);
    return (ret);
}

#define PRIVATE_TAG_MAGIC_MSK (0xFFFF0000)
#define PRIVATE_TAG_MAGIC_VAL (0x41490000)

static bool private_data_quick_check(u8 *res, u32 rlen)
{
    u32 offset = 0, type, len, sumval;
    struct private_data2_header head;

    memcpy(&head, res, sizeof(head));
    if (head.magic != AIC_PDAT_MAGIC) {
        return false;
    }

    if (head.ver != 0x00010001) {
        return false;
    }

    if (head.cksum != 0) {
        sumval = calc_page_checksum(res, rlen);
        if (sumval != 0xFFFFFFFF) {
            return false;
        } else {
            return true;
        }
    }

    /* No cksum value, should has secure signature */
    if (head.sign_offset == 0) {
        return false;
    }

    /* But don't perform complex secure signature verification, just quick check TLV */
    offset = sizeof(head);
    while ((rlen - offset) >= 8) {
        type = *(u32 *)(res + offset);
        len = *(u32 *)(res + offset + 4);

        if ((type & PRIVATE_TAG_MAGIC_MSK) != PRIVATE_TAG_MAGIC_VAL) {
            return false;
        }
        if (type == DATA_SECT_TYPE_END) {
            return true;
        }

        if ((len & 3) || (len > (rlen - offset -8))) {
            return false;
        }
        offset += (8 + len);
    }

    return false;
}

static bool private_data_range_check(struct nand_page_table *pt, unsigned long pagesize,
                                     u32 offset, u32 len)
{
    ulong maxlen = (ulong)(pt->head.entry_cnt - 1) * pagesize;

    if (offset == 0 || len == 0)
        return false;
    if (offset >= maxlen)
        return false;
    if (len > maxlen - offset)
        return false;
    return true;
}

static void *get_boot_resource_by_image_id(void *dev, int img_id, unsigned long pagesize,
                                           nand_read fn)
{
    u32 pa, i, offset, cksum, sumval, rdofs, rdlen = 0;
    struct nand_page_table *pt;
    struct aic_image_header head;
    u8 *res = NULL, *p = NULL, *page = NULL;
    s32 ret;

    pt = malloc(sizeof(*pt));
    if (pt == NULL)
        return NULL;

    ret = get_nand_page_table(fn, dev, pagesize, pt);
    if (ret < 0) {
        goto out;
    }

    if (pt->head.entry_cnt < 2 || pt->head.entry_cnt > PAGE_TABLE_MAX_ENTRY) {
        goto out;
    }

    page = malloc(pagesize);
    if (page == NULL) {
        goto out;
    }

    /* Page Table in page0, read AIC header in page1 */
    pa = pt->entry[1].pageaddr[img_id];
    if (pa == SPL_INVALID_PAGE_ADDR)
        goto out;

    cksum = pt->entry[1].checksum;

    offset = pa * pagesize;
    ret = fn(dev, offset, page, pagesize, 0);
    if (ret < 0) {
        pr_err("Failed to read image %d page 1, pa 0x%x.\n", img_id, pa);
        goto out;
    }
    sumval = calc_page_checksum(page, pagesize);
    sumval += cksum;
    if (sumval != 0xFFFFFFFF) {
        pr_err("Page 1, pa 0x%x checksum 0x%x is wrong.\n", pa, sumval);
        goto out;
    }

    memcpy(&head, page, sizeof(head));

    if (head.magic != AIC_IMAGE_MAGIC) {
        pr_err("aic image head verify failure.");
        goto out;
    }

    /* No private data or it exceeds the page table range: both are
     * invalid, prevent ROUNDUP overflow and heap overflow.
     */
    if (!private_data_range_check(pt, pagesize, head.private_data_offset,
                                  head.private_data_len)) {
        pr_err("Invalid private data offset 0x%x, len 0x%x.\n",
               head.private_data_offset, head.private_data_len);
        goto out;
    }

    rdofs = head.private_data_offset & (~(pagesize - 1));
    rdlen = ROUNDUP(head.private_data_len, pagesize);
    if (head.private_data_offset & (pagesize - 1))
        rdlen += pagesize;

    res = (void *)malloc(rdlen);
    if (res == NULL) {
        pr_err("Failed to malloc resource buffer.\n");
        goto out;
    }

    p = res;
    while (rdlen) {
        i = 1 + rdofs / pagesize;
        if (i >= pt->head.entry_cnt) {
            pr_err("Private data page %d exceeds entry count %d.\n",
                   i, pt->head.entry_cnt);
            free(res);
            res = NULL;
            goto out;
        }
        pa = pt->entry[i].pageaddr[img_id];
        if (pa == SPL_INVALID_PAGE_ADDR) {
            free(res);
            res = NULL;
            goto out;
        }
        cksum = pt->entry[i].checksum;
        offset = pa * pagesize;
        ret = fn(dev, offset, p, pagesize, 0);
        if (ret < 0) {
            pr_err("Failed to read aic image head.");
            free(res);
            res = NULL;
            goto out;
        }

        sumval = calc_page_checksum(p, pagesize);
        sumval += cksum;
        if (sumval != 0xFFFFFFFF) {
            pr_err("Image %d page %d, pa 0x%x checksum 0x%x is wrong.\n",
                    img_id, i, pa, sumval);
            free(res);
            res = NULL;
            goto out;
        }

        rdlen -= pagesize;
        rdofs += pagesize;
        p += pagesize;
    }

    rdofs = head.private_data_offset & (pagesize - 1);
    if (rdofs) {
        memcpy(res, res + rdofs, head.private_data_len);
    }

out:
    if (res && (private_data_quick_check(res, head.private_data_len) == false)) {
        free(res);
        res = NULL;
    }

    if (pt)
        free(pt);
    if (page)
        free(page);

    return res;
}

void *aic_get_boot_resource_from_nand(void *dev, unsigned long pagesize,
                                      nand_read fn)
{
    void *res_addr = NULL;
    int img_id;
    u32 val;

    val = readl(RTC_AP_BOOTINFO_REG);
    /* Set the initial image id from BROM */
    img_id = (val & RTC_BOOTINFO_IMGID_MSK) >> RTC_BOOTINFO_IMGID_OFF;
    for (; img_id < SPINAND_IMAGE_BACKUP_2; img_id++) {
        res_addr = get_boot_resource_by_image_id(dev, img_id, pagesize, fn);
        if (res_addr) {
            return res_addr;
        }
    }

    return NULL;
}

