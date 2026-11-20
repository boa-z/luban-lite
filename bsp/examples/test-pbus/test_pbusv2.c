/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: zrq <ruiqi.zheng@artinchip.com>
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include <aic_core.h>
#include <aic_drv_pbus.h>

#define PBUS_DUMP_LINE      16

static rt_device_t pbus_device = RT_NULL;

/* bounded string compare for shell command matching */
static int pbus_cmd_match(const char *cmd, const char *name)
{
    return (strncmp(cmd, name, strlen(name)) == 0) && (cmd[strlen(name)] == '\0');
}

static void test_pbus_usage(void)
{
    rt_kprintf("Usage: \n");
    rt_kprintf("test_pbus probe                                        - probe the PBUS device\n");
    rt_kprintf("test_pbus cfg   <mode> <work> <width> <cs> <clkdiv>    - set the bus configuration (manual)\n");
    rt_kprintf("test_pbus tim   sram                                   - apply the SRAM bus config + timing (HCLK/2)\n");
    rt_kprintf("test_pbus tim   adc                                    - apply the AD7616 bus config + timing (HCLK/8)\n");
    rt_kprintf("test_pbus adc                                          - AD7616 register read/write: PBUS path test, not ADC function\n");
    rt_kprintf("test_pbus tim   <w|r> <sig> <pre> <pha0> <wait> <pha1> - set one signal timing\n");
    rt_kprintf("test_pbus reset                                        - software reset the module\n");
    rt_kprintf("test_pbus write <offset> <val> [len]                   - write the window (hex offset)\n");
    rt_kprintf("test_pbus read  <offset> <len>                         - read the window (hex offset)\n");
    rt_kprintf("test_pbus test  [count]                                - walking 1/0 pattern, print every word\n");
    rt_kprintf("test_pbus perf  [len]                                  - read/write speed over len bytes (default 64 KB, current cfg)\n");
    rt_kprintf("test_pbus disp  8080 [w] [h] [frames] [dma_first]      - 8080 panel, four band pattern\n");
    rt_kprintf("test_pbus color <0..4> [w] [h]                         - 0:bands 1:red 2:yellow 3:green 4:blue\n");
    rt_kprintf("test_pbus status                                       - show version and busy state\n");
    rt_kprintf("    mode  : 0:MANU 1:A/D share 2:A/D split 3:I8080 4:I6800\n");
    rt_kprintf("    work  : 0:CPU dir 1:DMA dir 2:DMA fifo rd 3:DMA fifo wr 4:CPU fifo rd 5:CPU fifo wr\n");
    rt_kprintf("    width : 0:16-bit 1:8-bit,  cs: 0:CS0 1:CS1,  clkdiv: 1:HCLK/2 2:HCLK/4 3:HCLK/8\n");
    rt_kprintf("    sig   : ncs nadv nwe noe dat adr\n");
    rt_kprintf("    e.g. test_pbus cfg 2 0 0 0 3\n");
    rt_kprintf("    e.g. test_pbus write 0x100 0x1234\n");
}

static int pbus_probe(int argc, char **argv)
{
    if (argc != 2) {
        rt_kprintf("test_pbus probe - probe the PBUS device\n");
        return -RT_ERROR;
    }

    pbus_device = rt_device_find(AIC_PBUS_DEV_NAME);
    if (pbus_device == RT_NULL) {
        rt_kprintf("Can't find PBUS device %s\n", AIC_PBUS_DEV_NAME);
        return -RT_ERROR;
    }

    if (rt_device_open(pbus_device, RT_DEVICE_OFLAG_RDWR) != RT_EOK) {
        rt_kprintf("Failed to open PBUS device %s\n", AIC_PBUS_DEV_NAME);
        pbus_device = RT_NULL;
        return -RT_ERROR;
    }

    rt_kprintf("probe %s success\n", AIC_PBUS_DEV_NAME);

    return RT_EOK;
}

/* Apply one whole bus configuration to the device */
static rt_err_t pbus_apply_cfg(u32 mode, u32 work, u32 width, u32 cs, u32 clkdiv)
{
    aic_pbus_config_t cfg = {0};
    rt_err_t result;

    cfg.clk_div = clkdiv;
    cfg.clk_oe = 1;             /* drive the PBUS CLK output */
    cfg.clk_pol = PBUS_LOW;
    cfg.ncs_pol = PBUS_LOW;
    cfg.nadv_pol = PBUS_LOW;
    cfg.nwe_pol = PBUS_LOW;
    cfg.noe_pol = PBUS_LOW;
    cfg.mode = mode;
    cfg.work_mode = work;
    cfg.data_width = width;
    cfg.cs_sel = cs;
    cfg.addr_bus_en = 0;
    cfg.manu_addr_incr = 0;
    cfg.ncs_check_dat_en = 0;

    result = rt_device_control(pbus_device, AIC_PBUS_CMD_SET_CONFIG, &cfg);
    rt_kprintf("set config %s\n", (result == RT_EOK) ? "success" : "failure");

    return result;
}

static int pbus_cfg(int argc, char **argv)
{
    if (argc != 7) {
        rt_kprintf("test_pbus cfg <mode> <work> <width> <cs> <clkdiv>\n");
        return -RT_ERROR;
    }

    return pbus_apply_cfg(atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                          atoi(argv[5]), atoi(argv[6]));
}

/* Set the write or the read timing of one direction at once */
static int pbus_set_timing(rt_uint32_t sel, aic_pbus_timing_t *tim)
{
    struct aic_pbus_timing_cfg cfg = {0};
    rt_err_t result;

    cfg.sel = sel;
    cfg.tim = *tim;

    result = rt_device_control(pbus_device, AIC_PBUS_CMD_SET_TIMING, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Set %s timing failure\n", sel ? "read" : "write");
        return result;
    }

    return RT_EOK;
}

/*
 * The output level of every phase is programmed by the same register as the
 * timing (PBUS_VAL_x_SET), and the IP resets it to NCS=0x13, NADV=0x03,
 * NWE=0x1B, NOE=0x1F. A timing structure must always start from those reset
 * values: leaving the fields at 0 would
 * drive NCS low in every phase, i.e. keep the chip select permanently
 * asserted.
 */
static void pbus_val_defaults(aic_pbus_timing_t *tim)
{
    /* NCS: idle 1, pre 1, pha0 0, wait 0, pha1 1 = 0x13 */
    tim->ncs_val.idle = 1;
    tim->ncs_val.pre = 1;
    tim->ncs_val.pha1 = 1;

    /* NADV: idle 1, pre 1, pha0 0, wait 0, pha1 0 = 0x03 */
    tim->nadv_val.idle = 1;
    tim->nadv_val.pre = 1;

    /* NWE: idle 1, pre 1, pha0 0, wait 1, pha1 1 = 0x1B */
    tim->nwe_val.idle = 1;
    tim->nwe_val.pre = 1;
    tim->nwe_val.wait = 1;
    tim->nwe_val.pha1 = 1;

    /* NOE: idle 1, pre 1, pha0 1, wait 1, pha1 1 = 0x1F */
    tim->noe_val.idle = 1;
    tim->noe_val.pre = 1;
    tim->noe_val.pha0 = 1;
    tim->noe_val.wait = 1;
    tim->noe_val.pha1 = 1;
}

/*
 * SRAM bus setup: address/data split, CPU direct access, 16-bit, CS0 and the
 * default bus clock (HCLK/2, the divider the module comes up with).
 *
 * The bus configuration is applied together with the timing because the two
 * only make sense as a pair.
 */
static int pbus_tim_sram(void)
{
    aic_pbus_timing_t tim = {0};

    if (pbus_apply_cfg(PBUS_ADDR_DATA_SPLIT, PBUS_CPU_DIR, PBUS_DATA_X16,
                       PBUS_CS0, HCLK_D2) != RT_EOK)
        return -RT_ERROR;

    pbus_val_defaults(&tim);

    /* Write timing */
    tim.ncs.pre = 0;
    tim.ncs.pha = 10;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.nwe.pre = 1;
    tim.nwe.pha0 = 6;
    tim.nwe.wait = 1;
    tim.nwe.pha1 = 1;
    tim.dat.pre = 0;
    tim.dat.pha0 = 4;
    tim.dat.pha1 = 6;
    tim.adr.pre = 0;
    tim.adr.pha = 4;
    tim.dat_pos = 0;
    tim.multi_mode = 0;
    tim.noe_val.idle = 1;
    tim.noe_val.pre = 1;
    tim.noe_val.pha0 = 1;
    tim.noe_val.wait = 1;
    tim.noe_val.pha1 = 1;
    tim.nwe_val.idle = 1;
    tim.nwe_val.pre = 1;
    tim.nwe_val.pha0 = 0;
    tim.nwe_val.wait = 1;
    tim.nwe_val.pha1 = 1;

    if (pbus_set_timing(WRITE_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    /* Read timing */
    memset(&tim, 0, sizeof(tim));
    pbus_val_defaults(&tim);
    tim.ncs.pre = 0;
    tim.ncs.pha = 11;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.noe.pre = 2;
    tim.noe.pha0 = 1;
    tim.noe.wait = 1;
    tim.noe.pha1 = 6;
    tim.dat.pre = 0;
    tim.dat.pha0 = 4;
    tim.dat.wait = 2;
    tim.dat.pha1 = 4;
    tim.adr.pre = 2;
    tim.adr.pha = 4;
    tim.dat_pos = 0;
    tim.multi_mode = 0;
    tim.noe_val.idle = 1;
    tim.noe_val.pre = 1;
    tim.noe_val.pha0 = 1;
    tim.noe_val.wait = 1;
    tim.noe_val.pha1 = 0;
    tim.nwe_val.idle = 1;
    tim.nwe_val.pre = 1;
    tim.nwe_val.pha0 = 1;
    tim.nwe_val.wait = 1;
    tim.nwe_val.pha1 = 1;

    if (pbus_set_timing(READ_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    rt_kprintf("load SRAM config + timing success\n");

    return RT_EOK;
}

/*
 * ADC bus setup: it runs in address/data share mode with HCLK/8 and longer
 * NCS/DAT phases than the SRAM case, see pbus_adc() for the full sequence.
 *
 * The bus configuration is applied together with the timing because the two
 * only make sense as a pair.
 */
static int pbus_tim_adc(void)
{
    aic_pbus_timing_t tim = {0};

    if (pbus_apply_cfg(PBUS_ADDR_DATA_SHARE, PBUS_CPU_DIR, PBUS_DATA_X16,
                       PBUS_CS0, HCLK_D8) != RT_EOK)
        return -RT_ERROR;

    pbus_val_defaults(&tim);

    /* Write timing */
    tim.ncs.pre = 0;
    tim.ncs.pha = 13;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.nwe.pre = 1;
    tim.nwe.pha0 = 6;
    tim.nwe.wait = 1;
    tim.nwe.pha1 = 1;
    tim.dat.pre = 0;
    tim.dat.pha0 = 1;
    tim.dat.wait = 3;
    tim.dat.pha1 = 6;
    tim.adr.pre = 2;
    tim.adr.pha = 4;
    tim.multi_mode = 0;

    if (pbus_set_timing(WRITE_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    /* Read timing */
    memset(&tim, 0, sizeof(tim));
    pbus_val_defaults(&tim);
    tim.ncs.pre = 0;
    tim.ncs.pha = 13;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.noe.pre = 2;
    tim.noe.pha0 = 1;
    tim.noe.wait = 1;
    tim.noe.pha1 = 4;
    tim.dat.pha0 = 0;
    tim.dat.wait = 3;
    tim.dat.pha1 = 8;
    tim.adr.pre = 2;
    tim.adr.pha = 4;
    tim.noe_val.pha1 = 0;
    tim.nwe_val.pha0 = 1;
    tim.multi_mode = 0;

    if (pbus_set_timing(READ_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    rt_kprintf("load ADC config + timing success\n");

    return RT_EOK;
}

static int pbus_tim(int argc, char **argv)
{
    aic_pbus_timing_t tim = {0};
    rt_uint32_t sel;
    u32 pre, pha0, wait, pha1;

    if (argc < 3) {
        rt_kprintf("test_pbus tim <sram> | <w|r> <sig> <pre> <pha0> <wait> <pha1>\n");
        return -RT_ERROR;
    }

    if (pbus_cmd_match(argv[2], "sram"))
        return pbus_tim_sram();

    if (pbus_cmd_match(argv[2], "adc"))
        return pbus_tim_adc();

    if (argc != 8) {
        rt_kprintf("test_pbus tim <w|r> <sig> <pre> <pha0> <wait> <pha1>\n");
        return -RT_ERROR;
    }

    if (pbus_cmd_match(argv[2], "w")) {
        sel = WRITE_TIM;
    } else if (pbus_cmd_match(argv[2], "r")) {
        sel = READ_TIM;
    } else {
        rt_kprintf("Invalid direction: %s\n", argv[2]);
        return -RT_ERROR;
    }

    /* Start from the IP reset levels so that tuning one signal does not wipe
     * the output level of the others */
    pbus_val_defaults(&tim);

    pre = atoi(argv[4]);
    pha0 = atoi(argv[5]);
    wait = atoi(argv[6]);
    pha1 = atoi(argv[7]);

    if (pbus_cmd_match(argv[3], "ncs")) {
        tim.ncs.pre = pre;
        tim.ncs.pha0 = pha0;
        tim.ncs.wait = wait;
        tim.ncs.pha1 = pha1;
        tim.multi_mode = 1;
    } else if (pbus_cmd_match(argv[3], "nadv")) {
        tim.nadv.pre = pre;
        tim.nadv.pha0 = pha0;
        tim.nadv.wait = wait;
        tim.nadv.pha1 = pha1;
    } else if (pbus_cmd_match(argv[3], "nwe")) {
        tim.nwe.pre = pre;
        tim.nwe.pha0 = pha0;
        tim.nwe.wait = wait;
        tim.nwe.pha1 = pha1;
    } else if (pbus_cmd_match(argv[3], "noe")) {
        tim.noe.pre = pre;
        tim.noe.pha0 = pha0;
        tim.noe.wait = wait;
        tim.noe.pha1 = pha1;
    } else if (pbus_cmd_match(argv[3], "dat")) {
        tim.dat.pre = pre;
        tim.dat.pha0 = pha0;
        tim.dat.wait = wait;
        tim.dat.pha1 = pha1;
    } else if (pbus_cmd_match(argv[3], "adr")) {
        tim.adr.pre = pre;
        tim.adr.pha = pha0;
    } else {
        rt_kprintf("Invalid signal: %s\n", argv[3]);
        return -RT_ERROR;
    }

    if (pbus_set_timing(sel, &tim) != RT_EOK)
        return -RT_ERROR;

    rt_kprintf("set %s %s timing: pre %d, pha0 %d, wait %d, pha1 %d\n",
               sel ? "read" : "write", argv[3], pre, pha0, wait, pha1);

    return RT_EOK;
}

static int pbus_reset(void)
{
    rt_err_t result = rt_device_control(pbus_device, AIC_PBUS_CMD_SOFT_RESET, RT_NULL);

    rt_kprintf("software reset %s\n", (result == RT_EOK) ? "success" : "failure");

    return result;
}

static void pbus_dump(const u8 *buf, u32 len)
{
    u32 i;

    for (i = 0; i < len; i++) {
        if ((i % PBUS_DUMP_LINE) == 0)
            rt_kprintf("\n%04x: ", i);
        rt_kprintf("%02x ", buf[i]);
    }
    rt_kprintf("\n");
}

static int pbus_read(int argc, char **argv)
{
    static u8 buf[256];
    u32 offset, len, size;
    rt_ssize_t ret;

    if (argc != 4) {
        rt_kprintf("test_pbus read <offset> <len>\n");
        return -RT_ERROR;
    }

    offset = strtol(argv[2], NULL, 16);
    len = atoi(argv[3]);

    if ((offset + len > AIC_PBUS_SIZE) || (len > sizeof(buf))) {
        rt_kprintf("Invalid range: 0x%x + %d\n", offset, len);
        return -RT_ERROR;
    }

    ret = rt_device_read(pbus_device, offset, buf, len);
    if (ret < 0) {
        rt_kprintf("Read [0x%x] %d failure: %d\n", offset, len, ret);
        return -RT_ERROR;
    }

    size = (u32)ret;
    rt_kprintf("read [0x%x] %d bytes", offset, size);
    pbus_dump(buf, size);

    return RT_EOK;
}

static int pbus_write(int argc, char **argv)
{
    u16 val;
    u32 offset;
    u32 len = 2;
    rt_ssize_t ret;

    if ((argc != 4) && (argc != 5)) {
        rt_kprintf("test_pbus write <offset> <val> [len]\n");
        return -RT_ERROR;
    }

    offset = strtol(argv[2], NULL, 16);
    val = (u16)strtol(argv[3], NULL, 16);
    if (argc == 5)
        len = atoi(argv[4]);

    if ((offset + len > AIC_PBUS_SIZE) || (len > sizeof(val))) {
        rt_kprintf("Invalid range: 0x%x + %d\n", offset, len);
        return -RT_ERROR;
    }

    ret = rt_device_write(pbus_device, offset, &val, len);
    if (ret < 0) {
        rt_kprintf("Write [0x%x] 0x%x failure: %d\n", offset, val, ret);
        return -RT_ERROR;
    }

    rt_kprintf("write [0x%x] 0x%x, %d bytes\n", offset, val, (u32)ret);

    return RT_EOK;
}

/*
 * Test value of word i: the first 16 words walk a single 1 across the bus,
 * the next 16 walk a single 0 (the complement), then it repeats.
 *
 * Every data line therefore has to carry both a 1 and a 0, which is what
 * tells "stuck at 0" apart from "stuck at 1" or from an inverted line. A
 * plain counter (or ~i with a small count) only ever drives the high lines to
 * one level and cannot see those faults.
 */
static u16 pbus_test_val(u32 i)
{
    u16 v = (u16)(1u << (i % 16));

    return ((i % 32) < 16) ? v : (u16)~v;
}

/*
 * Byte at byte offset off of the same pattern, so that a buffer based test can
 * check exactly what the word based self test writes: word j holds
 * pbus_test_val(j), low byte first.
 */
static u8 pbus_pattern_byte(u32 off)
{
    u16 val = pbus_test_val(off / 2);

    return (off & 1) ? (u8)(val >> 8) : (u8)(val & 0xFF);
}

/*
 * Write the walking pattern over the external device window and read it back,
 * printing the written and the read value of every word.
 */
static int pbus_selftest(int argc, char **argv)
{
    u32 count = 32;
    u32 i;
    u32 fail = 0;
    u16 val;
    u16 rd;

    if (argc == 3)
        count = atoi(argv[2]);

    if ((count == 0) || ((u64)count * 2 > AIC_PBUS_SIZE)) {
        rt_kprintf("Invalid count: %d\n", count);
        return -RT_EINVAL;
    }

    for (i = 0; i < count; i++) {
        val = pbus_test_val(i);
        if (rt_device_write(pbus_device, i * 2, &val, 2) < 0) {
            rt_kprintf("Write [0x%04x] failure\n", i * 2);
            return -RT_EIO;
        }
    }

    rt_kprintf("offset  wrote   read    diff\n");
    for (i = 0; i < count; i++) {
        val = pbus_test_val(i);
        if (rt_device_read(pbus_device, i * 2, &rd, 2) < 0) {
            rt_kprintf("Read [0x%04x] failure\n", i * 2);
            return -RT_EIO;
        }
        rt_kprintf("[0x%04x] 0x%04x 0x%04x 0x%04x %s\n",
                   i * 2, val, rd, (u16)(val ^ rd),
                   (rd == val) ? "" : "<-- FAIL");
        if (rd != val)
            fail++;
    }

    rt_kprintf("self test over %d words: %s (%d mismatches)\n", count,
               fail ? "FAIL" : "PASS", fail);

    return RT_EOK;
}

static void pbus_perf_speed(const char *dir, u32 len, u32 us)
{
    /* len is at most one 64 KB window, the 64-bit intermediate only keeps the
     * multiply clear of the 32-bit ceiling */
    u32 speed = (u32)(((u64)len * 1000000ULL) / (us ? us : 1) / 1024ULL);

    rt_kprintf("%s: %d bytes in %d us -> %d KB/s\n", dir, len, us, speed);
}

/*
 * Measure the CPU direct access path over the external device window.
 *
 * The bus configuration and the timing are taken from whatever the caller has
 * already applied with 'cfg' and 'tim', exactly like 'test', so the number is
 * comparable between runs and this command stays a pure measurement.
 *
 * len defaults to the full 64 KB window and can be lowered to the size of a
 * smaller external device. The 16-bit bus does one access per word (see
 * hal_pbus_write()), so the result is dominated by the PBUS timing and not by
 * the buffer handling around it.
 */
static int pbus_perf(int argc, char **argv)
{
    u32 len = AIC_PBUS_SIZE;
    u8 *buf;
    u64 start;
    u32 wr_us, rd_us;
    u32 i;
    u32 fail = 0;
    rt_ssize_t ret;

    if (argc >= 3) {
        len = atoi(argv[2]);
        if ((len == 0) || (len > AIC_PBUS_SIZE)) {
            rt_kprintf("Invalid length: %d (1 .. %d)\n", len, AIC_PBUS_SIZE);
            return -RT_EINVAL;
        }
    }

    buf = rt_malloc(len);
    if (buf == RT_NULL) {
        rt_kprintf("No memory for %d bytes\n", len);
        return -RT_ENOMEM;
    }

    for (i = 0; i < len; i++)
        buf[i] = pbus_pattern_byte(i);

    start = aic_get_time_us();
    ret = rt_device_write(pbus_device, 0, buf, len);
    wr_us = (u32)(aic_get_time_us() - start);
    if (ret < 0) {
        rt_kprintf("Write %d bytes failure: %d\n", len, ret);
        rt_free(buf);
        return -RT_EIO;
    }

    /* Zero the buffer so a read that stores nothing is visible as a mismatch */
    memset(buf, 0, len);

    start = aic_get_time_us();
    ret = rt_device_read(pbus_device, 0, buf, len);
    rd_us = (u32)(aic_get_time_us() - start);
    if (ret < 0) {
        rt_kprintf("Read %d bytes failure: %d\n", len, ret);
        rt_free(buf);
        return -RT_EIO;
    }

    for (i = 0; i < len; i++) {
        if (buf[i] != pbus_pattern_byte(i))
            fail++;
    }

    pbus_perf_speed("write", len, wr_us);
    pbus_perf_speed("read ", len, rd_us);
    rt_kprintf("verify: %d/%d bytes ok%s\n", len - fail, len,
               fail ? " (check the external device)" : "");

    rt_free(buf);

    return RT_EOK;
}

/*
 * Registers used by the ADC case. Registers 2-7 are its six read/write ones,
 * and an address scan over the whole six bit register field showed those are
 * the only ones implemented in the first bank.
 */
#define PBUS_ADC_REG_FIRST  2
#define PBUS_ADC_REG_LAST   7

/* 9 steps walking a single 1 over the data field, then 9 walking a single 0 */
#define PBUS_ADC_STEPS      18

static int pbus_adc_write(u32 reg, u16 data)
{
    u16 cmd = (u16)(0x8000 | (reg << 9) | data);

    return rt_device_write(pbus_device, 0, &cmd, 2);
}

static int pbus_adc_read(u32 reg, u16 *data)
{
    u16 cmd = (u16)(reg << 9);

    if (rt_device_write(pbus_device, 0, &cmd, 2) < 0)
        return -1;

    return rt_device_read(pbus_device, 0, data, 2);
}

static u16 pbus_adc_data(u32 step)
{
    return (step < 9) ? (u16)(1u << step)
                      : (u16)(~(1u << (step - 9)) & 0x1FF);
}

/*
 * ADC case. The slave is an AD7616 (16 channel
 * DAS, dual 16-bit simultaneous sampling ADC) on its parallel interface,
 * always at offset 0, taking a 16-bit command frame:
 *
 *     write: bit15 = 1, [14:9] = register, [8:0] = data
 *     read : bit15 = 0, [14:9] = register
 *
 * bit14 (REGADDR[5]) selects the 32 sequencer stack registers, so the frame
 * addresses 0x02-0x07 (the six read/write registers) and 0x20-0x3F.
 *
 * Registers 2-7 are written with one data value after another and read back,
 * the value walking a 1 and then a 0 over all nine data bits, so every data
 * line is driven both ways. One row per data value, one column per register,
 * a '!' marks a mismatch.
 *
 * @note register 2 (0x02) has a read-only bit 7, SDEF (self test error flag):
 *       it reads 0 when the AD7616 passed its self configuration, so the rows
 *       driving bit 7 high always mismatch on that one column and the run ends
 *       at 106/108. That is the expected, healthy result
 */
static int pbus_adc(void)
{
    u32 reg;
    u32 step;
    u32 bad = 0;
    const u32 total = PBUS_ADC_STEPS * (PBUS_ADC_REG_LAST - PBUS_ADC_REG_FIRST + 1);
    u16 data;
    u16 rd;

    /* Bus configuration and timing for the ADC case (pbus_tim_adc()) */
    if (pbus_tim_adc() != RT_EOK)
        return -RT_ERROR;

    rt_kprintf("data ");
    for (reg = PBUS_ADC_REG_FIRST; reg <= PBUS_ADC_REG_LAST; reg++)
        rt_kprintf("  reg%d", reg);
    rt_kprintf("\n");

    for (step = 0; step < PBUS_ADC_STEPS; step++) {
        data = pbus_adc_data(step);

        /* Write the same value into every register first */
        for (reg = PBUS_ADC_REG_FIRST; reg <= PBUS_ADC_REG_LAST; reg++) {
            if (pbus_adc_write(reg, data) < 0) {
                rt_kprintf("Write reg %d failure\n", reg);
                return -RT_EIO;
            }
        }

        /* Then read them all back */
        rt_kprintf("0x%03x", data);
        for (reg = PBUS_ADC_REG_FIRST; reg <= PBUS_ADC_REG_LAST; reg++) {
            if (pbus_adc_read(reg, &rd) < 0) {
                rt_kprintf("Read reg %d failure\n", reg);
                return -RT_EIO;
            }
            if (rd == data) {
                rt_kprintf(" 0x%03x ", rd);
            } else {
                rt_kprintf(" 0x%03x!", rd);
                bad++;
            }
        }
        rt_kprintf("\n");
    }

    rt_kprintf("ADC test: %d/%d ok (regs %d-%d, %d data values)\n",
               total - bad, total, PBUS_ADC_REG_FIRST, PBUS_ADC_REG_LAST,
               PBUS_ADC_STEPS);

    return RT_EOK;
}

/* --------------------------------------------------------------------------
 * DMA 8080 display demo
 *
 * Data path: DRAM --(DMA)--> PBUS_DATA (0x100) --> 8080 bus --> panel.
 * The PBUS driver owns the DMA channel: the example only fills the buffers
 * and passes them to AIC_PBUS_CMD_DMA_WRITE, and the driver runs the whole
 * transfer and cleans the source range from the D-cache before starting it.
 * ------------------------------------------------------------------------*/

#define PBUS_8080_WIDTH_DEFAULT     320
#define PBUS_8080_HEIGHT_DEFAULT    480
#define PBUS_8080_FRAMES_DEFAULT    4
#define PBUS_8080_WIDTH_MAX         1024
#define PBUS_8080_HEIGHT_MAX        2048
#define PBUS_8080_FRAMES_MAX        100
#define PBUS_8080_DMA_TIMEOUT_MS    100
#define PBUS_8080_CMD_BUF_SIZE      (2 * CACHE_LINE_SIZE)

/* Pixel write timing, see pbus_tim_8080_pixels() */
#define PBUS_8080_PIXEL_NCS_PHA     4000
#define PBUS_8080_PIXEL_DAT_PHA0    4
#define PBUS_8080_PIXEL_DAT_PHA1    3

/* Panel command codes */
#define PBUS_8080_CMD_RAMWR         0x2C
#define PBUS_8080_CMD_RAMWRC        0x3C

/* Four horizontal colour bands, RGB565 */
static const u16 pbus_8080_color[4] = {
    0xF800,     /* red */
    0xFFE0,     /* yellow */
    0x07E0,     /* green */
    0x001F,     /* blue */
};

/*
 * ILI9488 init table: count, command, then count - 1 data bytes.
 * The column and row windows are sent separately so that they follow the
 * width and height given on the command line.
 */
static const u8 pbus_8080_init_tbl[] = {
    5,  0xF7, 0xA9, 0x51, 0x2C, 0x82,
    5,  0xEC, 0x00, 0x02, 0x03, 0x7A,
    3,  0xC0, 0x13, 0x13,
    2,  0xC1, 0x41,
    4,  0xC5, 0x00, 0x28, 0x80,
    3,  0xB1, 0xB0, 0x11,
    2,  0xB4, 0x02,
    3,  0xB6, 0x02, 0x22,
    2,  0xB7, 0xC6,
    3,  0xBE, 0x00, 0x04,
    2,  0xE9, 0x00,
    2,  0xB7, 0x07,
    4,  0xF4, 0x00, 0x00, 0x0F,
    16, 0xE0, 0x00, 0x04, 0x0E, 0x08, 0x17, 0x0A, 0x40, 0x79, 0x4D, 0x07,
              0x0E, 0x0A, 0x1A, 0x1D, 0x0F,
    16, 0xE1, 0x00, 0x1B, 0x1F, 0x02, 0x10, 0x05, 0x32, 0x34, 0x43, 0x02,
              0x0A, 0x09, 0x33, 0x37, 0x0F,
    4,  0xF4, 0x00, 0x00, 0x0F,
    2,  0x35, 0x00,
    3,  0x44, 0x00, 0x10,
    7,  0x33, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00,
    3,  0x37, 0x00, 0x00,
    5,  0x2A, 0x00, 0x00, 0x01, 0x3F,
    5,  0x2B, 0x00, 0x00, 0x01, 0xDF,
    2,  0x36, 0x08,
    2,  0x3A, 0x55,
    2,  0x11, 0x00,
    2,  0x29, 0x00,
};

/*
 * Write and read timing of the 8080 case. ncs_pha is 35 for commands and the
 * pixel timing (pbus_tim_8080_pixels()) for a pixel line, so the chip select
 * stays low across the whole line.
 */
static int pbus_tim_8080(u32 ncs_pha)
{
    aic_pbus_timing_t tim = {0};

    pbus_val_defaults(&tim);
    tim.ncs.pre = 1;
    tim.ncs.pha = ncs_pha;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.nwe.pre = 1;
    tim.nwe.pha0 = 1;
    tim.nwe.wait = 1;
    tim.nwe.pha1 = 1;
    tim.noe.pre = 2;
    tim.noe.pha0 = 2;
    tim.noe.wait = 1;
    tim.noe.pha1 = 1;
    tim.dat.pre = 0;
    tim.dat.pha0 = 3;
    tim.dat.pha1 = 2;
    tim.adr.pre = 2;
    tim.adr.pha = 3;
    tim.dat_pos = 0;
    tim.multi_mode = 0;

    if (pbus_set_timing(WRITE_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    memset(&tim, 0, sizeof(tim));
    pbus_val_defaults(&tim);
    tim.ncs.pre = 0;
    tim.ncs.pha = 11;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.noe.pre = 2;
    tim.noe.pha0 = 2;
    tim.noe.wait = 1;
    tim.noe.pha1 = 1;
    tim.dat.pre = 0;
    tim.dat.pha0 = 4;
    tim.dat.wait = 2;
    tim.dat.pha1 = 4;
    tim.adr.pre = 2;
    tim.adr.pha = 3;
    tim.noe_val.pha1 = 0;
    tim.nwe_val.pha0 = 1;
    tim.dat_pos = 0;
    tim.multi_mode = 0;

    if (pbus_set_timing(READ_TIM, &tim) != RT_EOK)
        return -RT_ERROR;

    return RT_EOK;
}

/*
 * Write timing of the pixel transfers.
 *
 * The data phases are one cycle longer than in the command timing: with the
 * shorter phases the panel loses a pixel now and then and the lost pixels
 * show up as thin black diagonal lines on bright colours. The command timing
 * is left alone, the panel init was tuned with it.
 */
static int pbus_tim_8080_pixels(void)
{
    aic_pbus_timing_t tim = {0};

    pbus_val_defaults(&tim);
    tim.ncs.pre = 1;
    tim.ncs.pha = PBUS_8080_PIXEL_NCS_PHA;
    tim.ncs.post = 1;
    tim.nadv.pre = 1;
    tim.nadv.pha0 = 2;
    tim.nwe.pre = 1;
    tim.nwe.pha0 = 1;
    tim.nwe.wait = 1;
    tim.nwe.pha1 = 1;
    tim.noe.pre = 2;
    tim.noe.pha0 = 2;
    tim.noe.wait = 1;
    tim.noe.pha1 = 1;
    tim.dat.pre = 0;
    tim.dat.pha0 = PBUS_8080_PIXEL_DAT_PHA0;
    tim.dat.pha1 = PBUS_8080_PIXEL_DAT_PHA1;
    tim.adr.pre = 2;
    tim.adr.pha = 3;
    tim.dat_pos = 0;
    tim.multi_mode = 0;

    return (pbus_set_timing(WRITE_TIM, &tim) == RT_EOK) ? 0 : -RT_ERROR;
}

static int pbus_8080_control(int cmd, void *arg)
{
    return (rt_device_control(pbus_device, cmd, arg) == RT_EOK) ? 0 : -1;
}

static void pbus_8080_report(void)
{
    u32 fifo = 0;
    u32 busy = 0;

    rt_device_control(pbus_device, AIC_PBUS_CMD_GET_FIFO_STA, &fifo);
    rt_device_control(pbus_device, AIC_PBUS_CMD_GET_BUSY, &busy);
    rt_kprintf("pbus: busy=%d fifo=0x%x\n", busy, fifo);
}

/*
 * Hand one command or line to the driver, which owns the DMA channel.
 *
 * The buffers are cacheable heap memory, but no cache maintenance is needed
 * here: pbus_dma_write() cleans the source range (buf, len) before it starts
 * the DMA.
 */
static int pbus_8080_write(u32 addr, u32 data_num, const void *buf, u32 len,
                           u32 flags)
{
    struct aic_pbus_dma_xfer x = {
        .addr = addr,
        .data_num = data_num,
        .buf = buf,
        .len = len,
        .timeout_ms = PBUS_8080_DMA_TIMEOUT_MS,
        .flags = flags,
    };

    return pbus_8080_control(AIC_PBUS_CMD_DMA_WRITE, &x);
}

/*
 * Send one panel command with its data through the FIFO. The DMA always moves
 * 8 or 16 bytes so that the source is a whole number of 32-bit words; the
 * DATA_NUM field tells the PBUS how many of them to put on the bus.
 */
static int pbus_8080_cmd(u8 *cmd_buf, u32 cmd, const u8 *data, u32 dlen,
                         u32 flags)
{
    u32 len = (dlen < 8) ? 8 : 16;

    memset(cmd_buf, 0, PBUS_8080_CMD_BUF_SIZE);
    if (dlen)
        memcpy(cmd_buf, data, dlen);

    return pbus_8080_write(cmd, dlen, cmd_buf, len, flags);
}

static int pbus_8080_init_panel(u8 *cmd_buf, u32 flags)
{
    const u8 *p = pbus_8080_init_tbl;
    const u8 *end = pbus_8080_init_tbl + sizeof(pbus_8080_init_tbl);
    u32 index = 0;
    int ret;

    if (pbus_apply_cfg(PBUS_I8080, PBUS_DMA_FIFO_WRITE, PBUS_DATA_X8,
                       PBUS_CS0, HCLK_D8) != RT_EOK)
        return -RT_ERROR;
    if (pbus_tim_8080(35) != RT_EOK)
        return -RT_ERROR;

    while (p < end) {
        u32 count = *p++;
        u32 cmd = *p++;
        u32 dlen;

        if (count < 2)
            continue;
        dlen = count - 1;

        ret = pbus_8080_cmd(cmd_buf, cmd, p, dlen, flags);
        if (ret) {
            rt_kprintf("init command 0x%x (entry %d) failed: %d\n",
                       cmd, index, ret);
            return ret;
        }
        p += dlen;
        index++;
    }

    return RT_EOK;
}

static void pbus_8080_fill_line(u16 *line, u32 width, u32 y, u32 height,
                                u32 solid)
{
    u32 band;
    u32 x;

    if (solid)
        band = solid - 1;       /* 1..4 selects one colour for the whole frame */
    else
        band = (y * 4) / height;
    if (band > 3)
        band = 3;
    for (x = 0; x < width; x++)
        line[x] = pbus_8080_color[band];
}

static void pbus_8080_frame_stats(u32 frame, u32 width, u32 height, u32 us)
{
    u64 bytes = (u64)width * 2 * height;
    u32 denom = us ? us : 1;
    u32 kb_s = (u32)(bytes * 1000000 / denom / 1024);
    u32 line_s = (u32)((u64)height * 1000000 / denom);

    rt_kprintf("frame %d: %d lines, %d bytes, %d us, %d KB/s, %d lines/s\n",
               frame, height, (u32)bytes, us, kb_s, line_s);
}

static int pbus_8080_pixels(u16 *line, u32 width, u32 height, u32 frames,
                            u32 flags, u32 solid)
{
    u32 frame;
    u32 y;
    u32 cmd;
    u64 start;

    if (pbus_apply_cfg(PBUS_I8080, PBUS_DMA_FIFO_WRITE, PBUS_DATA_X16,
                       PBUS_CS0, HCLK_D2) != RT_EOK)
        return -RT_ERROR;
    if (pbus_tim_8080_pixels() != RT_EOK)
        return -RT_ERROR;

    for (frame = 0; frame < frames; frame++) {
        start = aic_get_time_us();

        for (y = 0; y < height; y++) {
            cmd = (y == 0) ? PBUS_8080_CMD_RAMWR : PBUS_8080_CMD_RAMWRC;

            pbus_8080_fill_line(line, width, y, height, solid);
            if (pbus_8080_write(cmd, width, line, width * 2, flags))
                return -RT_EIO;
        }

        pbus_8080_frame_stats(frame, width, height,
                              (u32)(aic_get_time_us() - start));
    }

    return RT_EOK;
}

static int pbus_display(u32 width, u32 height, u32 frames, u32 dma_first,
                        u32 solid)
{
    u32 flags;
    u32 line_len;
    u8 *cmd_buf = RT_NULL;
    u16 *line = RT_NULL;
    int ret = -RT_ERROR;

    flags = dma_first ? AIC_PBUS_DMA_F_DMA_FIRST : 0;

    line_len = ALIGN_UP(width * 2, CACHE_LINE_SIZE);
    line = aicos_malloc_align(MEM_DEFAULT, line_len, CACHE_LINE_SIZE);
    cmd_buf = aicos_malloc_align(MEM_DEFAULT, PBUS_8080_CMD_BUF_SIZE,
                                 CACHE_LINE_SIZE);
    if ((line == RT_NULL) || (cmd_buf == RT_NULL)) {
        rt_kprintf("No memory for the display buffers\n");
        goto out;
    }

    rt_kprintf("pbus display: %dx%d x%d frames, dma_first=%d, solid=%d\n",
               width, height, frames, dma_first, solid);

    ret = pbus_8080_init_panel(cmd_buf, flags);
    if (ret == RT_EOK)
        ret = pbus_8080_pixels(line, width, height, frames, flags, solid);

    pbus_8080_report();

out:
    if (line != RT_NULL)
        aicos_free_align(MEM_DEFAULT, line);
    if (cmd_buf != RT_NULL)
        aicos_free_align(MEM_DEFAULT, cmd_buf);

    return ret;
}

static int pbus_disp_8080(int argc, char **argv)
{
    u32 width = PBUS_8080_WIDTH_DEFAULT;
    u32 height = PBUS_8080_HEIGHT_DEFAULT;
    u32 frames = PBUS_8080_FRAMES_DEFAULT;
    u32 dma_first = 1;

    if (argc >= 4)
        width = atoi(argv[3]);
    if (argc >= 5)
        height = atoi(argv[4]);
    if (argc >= 6)
        frames = atoi(argv[5]);
    if (argc >= 7)
        dma_first = atoi(argv[6]) ? 1 : 0;

    if ((width < 2) || (width > PBUS_8080_WIDTH_MAX) || (width & 1)) {
        rt_kprintf("Invalid width: %d (even, 2..%d)\n", width,
                   PBUS_8080_WIDTH_MAX);
        return -RT_EINVAL;
    }
    if ((height < 1) || (height > PBUS_8080_HEIGHT_MAX)) {
        rt_kprintf("Invalid height: %d (1..%d)\n", height,
                   PBUS_8080_HEIGHT_MAX);
        return -RT_EINVAL;
    }
    if ((frames < 1) || (frames > PBUS_8080_FRAMES_MAX)) {
        rt_kprintf("Invalid frames: %d (1..%d)\n", frames,
                   PBUS_8080_FRAMES_MAX);
        return -RT_EINVAL;
    }

    return pbus_display(width, height, frames, dma_first, 0);
}

/*
 * Fill the panel with a single colour so the colours can be compared on the
 * screen. 0 keeps the four band pattern, 1..4 select red/yellow/green/blue.
 */
static int pbus_color_cmd(int argc, char **argv)
{
    u32 solid;
    u32 width = PBUS_8080_WIDTH_DEFAULT;
    u32 height = PBUS_8080_HEIGHT_DEFAULT;

    if (argc < 3) {
        rt_kprintf("test_pbus color <0..4> [width] [height]\n");
        return -RT_EINVAL;
    }

    solid = atoi(argv[2]);
    if (solid > 4) {
        rt_kprintf("Invalid colour: %d (0:bands 1:red 2:yellow 3:green 4:blue)\n",
                   solid);
        return -RT_EINVAL;
    }

    if (argc >= 4)
        width = atoi(argv[3]);
    if (argc >= 5)
        height = atoi(argv[4]);

    if ((width < 2) || (width > PBUS_8080_WIDTH_MAX) || (width & 1)) {
        rt_kprintf("Invalid width: %d (even, 2..%d)\n", width,
                   PBUS_8080_WIDTH_MAX);
        return -RT_EINVAL;
    }
    if ((height < 1) || (height > PBUS_8080_HEIGHT_MAX)) {
        rt_kprintf("Invalid height: %d (1..%d)\n", height,
                   PBUS_8080_HEIGHT_MAX);
        return -RT_EINVAL;
    }

    return pbus_display(width, height, 1, 1, solid);
}

static int pbus_disp_cmd(int argc, char **argv)
{
    if ((argc >= 3) && pbus_cmd_match(argv[2], "8080"))
        return pbus_disp_8080(argc, argv);

    rt_kprintf("test_pbus disp 8080 [width] [height] [frames] [dma_first]\n");
    return -RT_EINVAL;
}

static int pbus_status(void)
{
    rt_uint32_t version = 0;
    rt_uint32_t busy = 0;

    if (rt_device_control(pbus_device, AIC_PBUS_CMD_GET_VERSION, &version) != RT_EOK)
        return -RT_ERROR;
    rt_device_control(pbus_device, AIC_PBUS_CMD_GET_BUSY, &busy);

    rt_kprintf("PBUS version: 0x%x (expect 0x200)\n", version);
    rt_kprintf("PBUS busy   : %d\n", busy);

    return RT_EOK;
}

static int test_pbus(int argc, char **argv)
{
    if (argc <= 1) {
        test_pbus_usage();
        return RT_EOK;
    }

    if (pbus_cmd_match(argv[1], "probe"))
        return pbus_probe(argc, argv);

    if (pbus_device == RT_NULL) {
        rt_kprintf("Please using 'test_pbus probe' first.\n");
        return -RT_ERROR;
    }

    if (pbus_cmd_match(argv[1], "cfg"))
        return pbus_cfg(argc, argv);
    else if (pbus_cmd_match(argv[1], "tim"))
        return pbus_tim(argc, argv);
    else if (pbus_cmd_match(argv[1], "reset"))
        return pbus_reset();
    else if (pbus_cmd_match(argv[1], "read"))
        return pbus_read(argc, argv);
    else if (pbus_cmd_match(argv[1], "write"))
        return pbus_write(argc, argv);
    else if (pbus_cmd_match(argv[1], "test"))
        return pbus_selftest(argc, argv);
    else if (pbus_cmd_match(argv[1], "perf"))
        return pbus_perf(argc, argv);
    else if (pbus_cmd_match(argv[1], "adc"))
        return pbus_adc();
    else if (pbus_cmd_match(argv[1], "disp"))
        return pbus_disp_cmd(argc, argv);
    else if (pbus_cmd_match(argv[1], "color"))
        return pbus_color_cmd(argc, argv);
    else if (pbus_cmd_match(argv[1], "status"))
        return pbus_status();

    test_pbus_usage();
    return -RT_ERROR;
}
MSH_CMD_EXPORT(test_pbus, test_pbus control device);
