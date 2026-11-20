/*
 * Copyright (c) 2024-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <aic_core.h>
#include <hal_i2c.h>

#define MAX_TEST_LEN           256
#define SHORT_DELAY_MS         2
#define MEDIUM_DELAY_MS        5
#define DEFAULT_STRESS_LOOPS   250

#define STREQ(a, b)            (rt_strncmp((a), (b), rt_strlen(b) + 1) == 0)

/* Supported parameter sweep tables */
static const uint32_t g_speed_table[] = {100000, 400000};
static const uint32_t g_stuck_table[]  = {50, 100, 200, 400, 800, 1000, 2000};
static const uint32_t g_len_table[]    = {1, 2, 4, 8, 16, 32, 64, 128, 255};

#define SPEED_CNT  (sizeof(g_speed_table) / sizeof(g_speed_table[0]))
#define STUCK_CNT  (sizeof(g_stuck_table)  / sizeof(g_stuck_table[0]))
#define LEN_CNT    (sizeof(g_len_table)    / sizeof(g_len_table[0]))

/* I2C bus local structure (must match driver definition exactly) */
struct aic_i2c_bus_local {
    struct rt_i2c_bus_device bus;
    aic_i2c_ctrl aic_bus;
    struct rt_completion cmd_complete;
};
#define BUS_TO_CTRL(bus_ptr)  (&((struct aic_i2c_bus_local *)(bus_ptr))->aic_bus)
#define DEV_TO_BUS(dev_ptr)   (rt_container_of((dev_ptr), struct rt_i2c_bus_device, parent))

/* Centralized test context */
struct i2c_test_ctx {
    struct rt_i2c_bus_device *master;
    rt_device_t slave;
    struct aic_i2c_slave_info slave_info;

    /* Current active config (auto loaded from menuconfig) */
    uint32_t is_10bit;
    uint32_t cur_speed;
    uint32_t cur_stuck;
    uint16_t cur_addr;

    /* Slave buffer state (accessed in ISR) */
    volatile uint8_t slave_buf[MAX_TEST_LEN];
    volatile uint32_t buf_offset;
    volatile uint32_t slave_buf_len;
    volatile uint32_t read_active;

    /* Test data buffers */
    uint8_t tx_buf[MAX_TEST_LEN];
    uint8_t rx_buf[MAX_TEST_LEN];
};

static struct i2c_test_ctx g_ctx = {0};

/* Forward declarations */
static int set_master_speed(uint32_t hz);
static int set_bus_stuck(uint32_t cycles);
static void slave_buf_reset(uint32_t len);
static void slave_buf_fill(const uint8_t *src, uint32_t len);

/* Get controller handle from bus device */
static aic_i2c_ctrl *master_ctrl(void)
{
    if (!g_ctx.master)
        return RT_NULL;
    return BUS_TO_CTRL(g_ctx.master);
}

static aic_i2c_ctrl *slave_ctrl(void)
{
    if (!g_ctx.slave)
        return RT_NULL;
    return BUS_TO_CTRL(DEV_TO_BUS(g_ctx.slave));
}

/* Set I2C bus clock speed */
static int set_master_speed(uint32_t hz)
{
    rt_err_t ret;

    if (!g_ctx.master)
        return -RT_ERROR;

    ret = rt_i2c_control(g_ctx.master, RT_I2C_DEV_CTRL_CLK, hz);
    if (ret != RT_EOK) {
        rt_kprintf("[ERR] set speed %u fail: %d\n", hz, ret);
        return ret;
    }

    g_ctx.cur_speed = hz;
    return RT_EOK;
}

/* Set SDA/SCL stuck detection threshold for both master and slave */
static int set_bus_stuck(uint32_t cycles)
{
    aic_i2c_ctrl *mc;
    aic_i2c_ctrl *sc;

    mc = master_ctrl();
    sc = slave_ctrl();
    if (!mc || !sc)
        return -RT_ERROR;

    hal_i2c_module_disable(mc);
    mc->stuck_time = cycles;
    hal_i2c_set_sda_scl_stuck_time(mc);
    hal_i2c_module_enable(mc);

    hal_i2c_module_disable(sc);
    sc->stuck_time = cycles;
    hal_i2c_set_sda_scl_stuck_time(sc);
    hal_i2c_module_enable(sc);

    g_ctx.cur_stuck = cycles;
    return RT_EOK;
}

/* ISR safe slave buffer operations */
static void slave_buf_reset(uint32_t len)
{
    rt_base_t level;
    uint32_t i;
    uint32_t safe_len;

    safe_len = (len > MAX_TEST_LEN) ? MAX_TEST_LEN : len;
    level = rt_hw_interrupt_disable();
    for (i = 0; i < MAX_TEST_LEN; i++)
        g_ctx.slave_buf[i] = 0;
    g_ctx.buf_offset = 0;
    g_ctx.read_active = 0;
    g_ctx.slave_buf_len = safe_len;
    rt_hw_interrupt_enable(level);
}

static void slave_buf_fill(const uint8_t *src, uint32_t len)
{
    rt_base_t level;
    uint32_t i;
    uint32_t safe_len;

    safe_len = (len > MAX_TEST_LEN) ? MAX_TEST_LEN : len;
    level = rt_hw_interrupt_disable();
    for (i = 0; i < safe_len; i++)
        g_ctx.slave_buf[i] = src[i];
    g_ctx.buf_offset = 0;
    g_ctx.read_active = 0;
    g_ctx.slave_buf_len = safe_len;
    rt_hw_interrupt_enable(level);
}

/* Slave IRQ callback handler */
static int i2c_slave_cb(void *arg)
{
    struct slave_param *p;
    uint8_t *data;
    rt_base_t level;

    p = (struct slave_param *)arg;
    if (!p)
        return 0;

    switch (p->cmd) {
    case I2C_SLAVE_WRITE_REQUESTED:
        level = rt_hw_interrupt_disable();
        g_ctx.buf_offset = 0;
        g_ctx.read_active = 0;
        rt_hw_interrupt_enable(level);
        break;

    case I2C_SLAVE_WRITE_RECEIVED:
        data = (uint8_t *)p->arg;
        if (data) {
            level = rt_hw_interrupt_disable();
            if (g_ctx.buf_offset < g_ctx.slave_buf_len)
                g_ctx.slave_buf[g_ctx.buf_offset++] = *data;
            rt_hw_interrupt_enable(level);
        }
        break;

    case I2C_SLAVE_READ_REQUESTED:
        level = rt_hw_interrupt_disable();
        if (!g_ctx.read_active) {
            g_ctx.buf_offset = 0;
            g_ctx.read_active = 1;
        }
        rt_hw_interrupt_enable(level);

        data = (uint8_t *)p->arg;
        if (data) {
            level = rt_hw_interrupt_disable();
            if (g_ctx.buf_offset < g_ctx.slave_buf_len)
                *data = g_ctx.slave_buf[g_ctx.buf_offset++];
            else
                *data = 0xFF;
            rt_hw_interrupt_enable(level);
        }
        break;

    case I2C_SLAVE_READ_PROCESSED:
        break;

    case I2C_SLAVE_STOP:
        level = rt_hw_interrupt_disable();
        g_ctx.read_active = 0;
        rt_hw_interrupt_enable(level);
        break;

    default:
        break;
    }

    return 0;
}

/* Run single length transfer test */
static int run_test_len(uint16_t addr, uint32_t len, const char *tag, int verbose)
{
    struct rt_i2c_msg wmsg, rmsg;
    rt_err_t ret;
    uint32_t i;
    uint16_t flags;

    if (!g_ctx.master || !g_ctx.slave) {
        rt_kprintf("[ERR] Please run init first\n");
        return -RT_ERROR;
    }
    if (len == 0 || len > MAX_TEST_LEN) {
        rt_kprintf("[ERR] Invalid len %u, range 1~%d\n", len, MAX_TEST_LEN);
        return -RT_ERROR;
    }

    flags = g_ctx.is_10bit ? RT_I2C_ADDR_10BIT : 0;

    /* Fill test pattern */
    for (i = 0; i < len; i++)
        g_ctx.tx_buf[i] = (uint8_t)(i & 0xFF);
    rt_memset(g_ctx.rx_buf, 0, len);
    slave_buf_reset(len);

    if (verbose) {
        rt_kprintf("--- %s | %uHz | stuck=%u | addr=0x%02X | len=%u ---\n",
                   tag, g_ctx.cur_speed, g_ctx.cur_stuck, addr, len);
    }

    /* Write test */
    wmsg.addr  = addr;
    wmsg.flags = flags | RT_I2C_WR;
    wmsg.buf   = g_ctx.tx_buf;
    wmsg.len   = (rt_uint16_t)len;
    ret = rt_i2c_transfer(g_ctx.master, &wmsg, 1);
    if (ret != 1) {
        if (verbose) rt_kprintf("[FAIL] Write no ACK\n");
        return -RT_ERROR;
    }
    rt_thread_mdelay(MEDIUM_DELAY_MS);

    /* Read test */
    slave_buf_fill(g_ctx.tx_buf, len);
    rmsg.addr  = addr;
    rmsg.flags = flags | RT_I2C_RD;
    rmsg.buf   = g_ctx.rx_buf;
    rmsg.len   = (rt_uint16_t)len;
    ret = rt_i2c_transfer(g_ctx.master, &rmsg, 1);
    if (ret != 1) {
        if (verbose) rt_kprintf("[FAIL] Read no ACK\n");
        return -RT_ERROR;
    }

    /* Verify data */
    if (rt_memcmp(g_ctx.tx_buf, g_ctx.rx_buf, len) != 0) {
        if (verbose) {
            for (i = 0; i < len; i++) {
                if (g_ctx.tx_buf[i] != g_ctx.rx_buf[i]) {
                    rt_kprintf("[FAIL] Mismatch @%u: tx=0x%02X rx=0x%02X\n",
                               i, g_ctx.tx_buf[i], g_ctx.rx_buf[i]);
                    break;
                }
            }
        }
        return -RT_ERROR;
    }

    if (verbose) rt_kprintf("[PASS]\n");
    return RT_EOK;
}

/* Test combined write-then-read (restart) format */
static int run_test_writeread(void)
{
    struct rt_i2c_msg msgs[2];
    uint8_t reg;
    uint8_t rx[16];
    uint8_t expected[16];
    uint16_t flags;
    rt_err_t ret;
    int i;

    if (!g_ctx.master || !g_ctx.slave) {
        rt_kprintf("[ERR] Please run init first\n");
        return -RT_ERROR;
    }

    /* Prepare test data */
    reg = 0x00;
    expected[0] = reg;
    for (i = 1; i < 16; i++)
        expected[i] = (uint8_t)(0xAA + i);
    slave_buf_fill(expected, 16);

    rt_kprintf("--- Combined write-then-read (restart) ---\n");
    flags = g_ctx.is_10bit ? RT_I2C_ADDR_10BIT : 0;
    msgs[0].addr  = g_ctx.cur_addr;
    msgs[0].flags = flags | RT_I2C_WR;
    msgs[0].buf   = &reg;
    msgs[0].len   = 1;
    msgs[1].addr  = g_ctx.cur_addr;
    msgs[1].flags = flags | RT_I2C_RD;
    msgs[1].buf   = rx;
    msgs[1].len   = 16;

    ret = rt_i2c_transfer(g_ctx.master, msgs, 2);
    if (ret != 2) {
        rt_kprintf("[FAIL] Combined transfer error: %d\n", ret);
        return -RT_ERROR;
    }

    rt_kprintf("Read: ");
    for (i = 0; i < 16; i++)
        rt_kprintf("%02X ", rx[i]);
    rt_kprintf("\n");

    if (rt_memcmp(expected, rx, 16) == 0) {
        rt_kprintf("[PASS] Combined write-read OK\n");
        return RT_EOK;
    }

    rt_kprintf("[FAIL] Data mismatch\n");
    return -RT_ERROR;
}

/* Test all supported bus speeds */
static int test_sweep_speed(void)
{
    uint32_t save_speed;
    uint32_t i;
    int fail;
    int ret;

    save_speed = g_ctx.cur_speed;
    fail = 0;

    for (i = 0; i < SPEED_CNT; i++) {
        ret = set_master_speed(g_speed_table[i]);
        if (ret != RT_EOK) {
            fail++;
            continue;
        }
        ret = run_test_len(g_ctx.cur_addr, 32, "speed test", 0);
        if (ret == RT_EOK) {
            rt_kprintf("[OK] Speed %uHz passed\n", g_speed_table[i]);
        } else {
            rt_kprintf("[FAIL] Speed %uHz failed\n", g_speed_table[i]);
            fail++;
        }
        rt_thread_mdelay(MEDIUM_DELAY_MS);
    }

    set_master_speed(save_speed);
    return fail ? -RT_ERROR : RT_EOK;
}

/* Test all stuck threshold values */
static int test_sweep_stuck(void)
{
    uint32_t save_stuck;
    uint32_t i;
    int fail;
    int ret;

    save_stuck = g_ctx.cur_stuck;
    fail = 0;

    for (i = 0; i < STUCK_CNT; i++) {
        set_bus_stuck(g_stuck_table[i]);
        rt_thread_mdelay(MEDIUM_DELAY_MS);

        ret = run_test_len(g_ctx.cur_addr, 32, "stuck test", 0);
        if (ret == RT_EOK) {
            rt_kprintf("[OK] Stuck %u cycles passed\n", g_stuck_table[i]);
        } else {
            rt_kprintf("[FAIL] Stuck %u cycles failed\n", g_stuck_table[i]);
            fail++;
        }
        rt_thread_mdelay(SHORT_DELAY_MS);
    }

    set_bus_stuck(save_stuck);
    return fail ? -RT_ERROR : RT_EOK;
}

/* Test all supported packet lengths */
static int test_sweep_len(void)
{
    uint32_t i;
    int fail;
    int ret;

    fail = 0;
    for (i = 0; i < LEN_CNT; i++) {
        ret = run_test_len(g_ctx.cur_addr, g_len_table[i], "len test", 0);
        if (ret == RT_EOK) {
            rt_kprintf("[OK] Len %u passed\n", g_len_table[i]);
        } else {
            rt_kprintf("[FAIL] Len %u failed\n", g_len_table[i]);
            fail++;
        }
        rt_thread_mdelay(SHORT_DELAY_MS);
    }
    return fail ? -RT_ERROR : RT_EOK;
}

/* Initialize test environment */
static int cmd_init(void)
{
    rt_err_t ret;
    aic_i2c_ctrl *mc;
    aic_i2c_ctrl *sc;

    rt_memset(&g_ctx, 0, sizeof(g_ctx));

    /* Find slave device */
#ifdef CONFIG_I2C0_DEV_NAME
    g_ctx.slave = rt_device_find(CONFIG_I2C0_DEV_NAME);
#endif
    if (!g_ctx.slave)
        g_ctx.slave = rt_device_find("i2c0");
    if (!g_ctx.slave) {
        rt_kprintf("[ERR] Slave I2C device not found\n");
        return -RT_ERROR;
    }

    ret = rt_device_open(g_ctx.slave, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        rt_kprintf("[ERR] Open slave fail: %d\n", ret);
        return ret;
    }

    /* Register slave callback */
    slave_buf_reset(MAX_TEST_LEN);
    g_ctx.slave_info.slave_cb = i2c_slave_cb;
    g_ctx.slave_info.callback_param = RT_NULL;
    ret = rt_device_control(g_ctx.slave, RT_I2C_DEV_SLAVE_CONFIG, &g_ctx.slave_info);
    if (ret != RT_EOK) {
        rt_kprintf("[ERR] Slave config fail: %d\n", ret);
        rt_device_close(g_ctx.slave);
        g_ctx.slave = RT_NULL;
        return ret;
    }

    /* Find master bus */
#ifdef CONFIG_I2C1_DEV_NAME
    g_ctx.master = rt_i2c_bus_device_find(CONFIG_I2C1_DEV_NAME);
#endif
    if (!g_ctx.master)
        g_ctx.master = rt_i2c_bus_device_find("i2c1");
    if (!g_ctx.master) {
        rt_kprintf("[ERR] Master I2C bus not found\n");
        rt_device_close(g_ctx.slave);
        g_ctx.slave = RT_NULL;
        return -RT_ERROR;
    }

    mc = master_ctrl();
    sc = slave_ctrl();
    if (!mc || !sc) {
        rt_kprintf("[ERR] Get controller handler fail\n");
        return -RT_ERROR;
    }

    /* Load default config from driver */
    g_ctx.is_10bit  = (mc->addr_bit == I2C_10BIT_ADDR) ? 1 : 0;
    g_ctx.cur_speed = mc->target_rate;
    g_ctx.cur_stuck = mc->stuck_time;
    g_ctx.cur_addr  = (uint16_t)sc->slave_addr;

    /* Sync stuck config between master and slave */
    set_bus_stuck(g_ctx.cur_stuck);

    rt_kprintf("[OK] Init success: %s addr=0x%02X speed=%uHz stuck=%u cycles\n",
               g_ctx.is_10bit ? "10bit" : "7bit",
               g_ctx.cur_addr, g_ctx.cur_speed, g_ctx.cur_stuck);
    return RT_EOK;
}

/* Release test resources */
static int cmd_deinit(void)
{
    if (g_ctx.slave) {
        rt_device_close(g_ctx.slave);
        g_ctx.slave = RT_NULL;
    }
    g_ctx.master = RT_NULL;
    rt_kprintf("[OK] Resources released\n");
    return RT_EOK;
}

/* Run full automatic test suite */
static int cmd_test(void)
{
    uint32_t i;
    int fail;
    int ret;

    fail = 0;
    rt_kprintf("\n========== I2C Loop Full Test ==========\n");

    rt_kprintf("\n[1/5] Basic communication test...\n");
    ret = run_test_len(g_ctx.cur_addr, 16, "basic loopback", 1);
    if (ret != RT_EOK) fail++;

    rt_kprintf("\n[2/5] Combined write-read test...\n");
    ret = run_test_writeread();
    if (ret != RT_EOK) fail++;

    rt_kprintf("\n[3/5] Speed sweep test...\n");
    ret = test_sweep_speed();
    if (ret != RT_EOK) fail++;

    rt_kprintf("\n[4/5] Parameter sweep test...\n");
    ret = test_sweep_stuck();
    if (ret != RT_EOK) fail++;
    ret = test_sweep_len();
    if (ret != RT_EOK) fail++;

    rt_kprintf("\n[5/5] Stress test (%u loops)...\n", DEFAULT_STRESS_LOOPS);
    for (i = 0; i < DEFAULT_STRESS_LOOPS; i++) {
        ret = run_test_len(g_ctx.cur_addr, 32, "stress", 0);
        if (ret != RT_EOK) {
            rt_kprintf("[FAIL] Stress loop %u failed\n", i);
            fail++;
            break;
        }
        if ((i + 1) % 50 == 0)
            rt_kprintf("  ... %u loops passed\n", i + 1);
        rt_thread_mdelay(SHORT_DELAY_MS);
    }

    rt_kprintf("\n========== Test Result ==========\n");
    if (fail == 0) {
        rt_kprintf("[OK] All tests passed! %u stress loops finished.\n", DEFAULT_STRESS_LOOPS);
        return RT_EOK;
    }
    rt_kprintf("[FAIL] %d test item(s) failed\n", fail);
    return -RT_ERROR;
}

/* MSH command handler */
static int cmd_i2c_loop(int argc, char *argv[])
{
    uint32_t val;
    uint32_t save;
    int ret;

    if (argc < 2) {
        rt_kprintf("Usage:\n");
        rt_kprintf("  i2c_loop init     - Init and load menuconfig config\n");
        rt_kprintf("  i2c_loop test     - Run full automatic test (include 250 stress loops)\n");
        rt_kprintf("  i2c_loop deinit   - Release resources\n");
        rt_kprintf("  i2c_loop speed <hz>    - Test at specific bus speed\n");
        rt_kprintf("  i2c_loop stuck <cyc>   - Test with specific stuck threshold\n");
        rt_kprintf("  i2c_loop len <n>       - Test specific packet length (1~256)\n");
        rt_kprintf("  i2c_loop writeread     - Test combined write-then-read timing\n");
        rt_kprintf("  i2c_loop speedsweep    - Test all supported speeds\n");
        rt_kprintf("  i2c_loop stucksweep    - Test all stuck thresholds\n");
        rt_kprintf("  i2c_loop lensweep      - Test all packet lengths\n");
        return RT_EOK;
    }

    if (STREQ(argv[1], "init")) return cmd_init();
    if (STREQ(argv[1], "deinit")) return cmd_deinit();
    if (STREQ(argv[1], "test")) return cmd_test();
    if (STREQ(argv[1], "writeread")) return run_test_writeread();
    if (STREQ(argv[1], "speedsweep")) return test_sweep_speed();
    if (STREQ(argv[1], "stucksweep")) return test_sweep_stuck();
    if (STREQ(argv[1], "lensweep")) return test_sweep_len();

    if (STREQ(argv[1], "speed")) {
        if (argc < 3) {
            rt_kprintf("Usage: i2c_loop speed <hz>\n");
            return -RT_ERROR;
        }
        val = (uint32_t)strtoul(argv[2], RT_NULL, 10);
        save = g_ctx.cur_speed;
        ret = set_master_speed(val);
        if (ret == RT_EOK)
            ret = run_test_len(g_ctx.cur_addr, 16, "speed test", 1);
        set_master_speed(save);
        return ret;
    }

    if (STREQ(argv[1], "stuck")) {
        if (argc < 3) {
            rt_kprintf("Usage: i2c_loop stuck <cycles>\n");
            return -RT_ERROR;
        }
        val = (uint32_t)strtoul(argv[2], RT_NULL, 10);
        save = g_ctx.cur_stuck;
        set_bus_stuck(val);
        rt_thread_mdelay(MEDIUM_DELAY_MS);
        ret = run_test_len(g_ctx.cur_addr, 16, "stuck test", 1);
        set_bus_stuck(save);
        return ret;
    }

    if (STREQ(argv[1], "len")) {
        if (argc < 3) {
            rt_kprintf("Usage: i2c_loop len <1~256>\n");
            return -RT_ERROR;
        }
        val = (uint32_t)strtoul(argv[2], RT_NULL, 10);
        return run_test_len(g_ctx.cur_addr, val, "len test", 1);
    }

    rt_kprintf("Unknown command: %s\n", argv[1]);
    return -RT_ERROR;
}

MSH_CMD_EXPORT_ALIAS(cmd_i2c_loop, i2c_loop, I2C master-slave loopback test utility);
