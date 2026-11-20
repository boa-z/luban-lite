/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: zrq <ruiqi.zheng@artinchip.com>
 */
#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/dev_pwm.h>
#include <string.h>
#include <stdlib.h>
#include <aic_drv_xpwm.h>

static struct rt_device_pwm *xpwm_device = RT_NULL;

/* bounded string compare for shell command matching */
static int xpwm_cmd_match(const char *cmd, const char *name)
{
    return (strncmp(cmd, name, strlen(name)) == 0) && (cmd[strlen(name)] == '\0');
}

static void test_xpwm_usage(void)
{
    rt_kprintf("Usage: \n");
    rt_kprintf("test_xpwm probe   <device name>                                            - probe xpwm by name\n");
    rt_kprintf("test_xpwm enable  <channel>                                                - enable xpwm channel\n");
    rt_kprintf("test_xpwm disable <channel>                                                - disable xpwm channel\n");
    rt_kprintf("test_xpwm get     <channel>                                                - get xpwm channel info\n");
    rt_kprintf("test_xpwm set     <channel> <period> <pulse> [<pulse cnt>]                 - set xpwm period/pulse (ns)\n");
    rt_kprintf("test_xpwm set_fifo_num <channel> <fifo_num>                                - set xpwm fifo count\n");
    rt_kprintf("test_xpwm set_fifo     <channel> <fifo_index> <period> <pulse> <pulse cnt> - set xpwm fifo info\n");
    rt_kprintf("test_xpwm get_fifo     <channel>                                           - get xpwm fifo info\n");
#ifdef AIC_USING_DMA
    rt_kprintf("test_xpwm dma_set_fifo <channel> <period> <pulse> <pulse cnt>              - set xpwm dma fifo info\n");
#endif
}

static int xpwm_probe(int argc, char **argv)
{
    if (argc != 3) {
        rt_kprintf("test_xpwm probe <device name> - probe xpwm by name\n");
        return -RT_ERROR;
    }

    xpwm_device = (struct rt_device_pwm *)rt_device_find(argv[2]);
    rt_kprintf("probe %s %s\n", argv[2],
               (xpwm_device == RT_NULL) ? "failure" : "success");

    return (xpwm_device == RT_NULL) ? -RT_ERROR : RT_EOK;
}

static int xpwm_enable(int argc, char **argv)
{
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_xpwm enable <channel> - enable xpwm channel\n");
        return -RT_ERROR;
    }

    result = rt_pwm_enable(xpwm_device, atoi(argv[2]));
    rt_kprintf("channel %d is enabled %s\n", atoi(argv[2]),
               (result == RT_EOK) ? "success" : "failure");

    return result;
}

static int xpwm_disable(int argc, char **argv)
{
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_xpwm disable <channel> - disable xpwm channel\n");
        return -RT_ERROR;
    }

    result = rt_pwm_disable(xpwm_device, atoi(argv[2]));
    rt_kprintf("channel %d is disabled %s\n", atoi(argv[2]),
               (result == RT_EOK) ? "success" : "failure");

    return result;
}

static int xpwm_get(int argc, char **argv)
{
    struct rt_pwm_configuration cfg = {0};
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_xpwm get <channel> - get xpwm channel info\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    result = rt_device_control(&xpwm_device->parent, PWM_CMD_GET, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Get info of channel %d error.\n", cfg.channel);
        return result;
    }

    rt_kprintf("Info of channel %d:\n", cfg.channel);
    rt_kprintf("period : %d\n", cfg.period);
    rt_kprintf("pulse  : %d\n", cfg.pulse);
    if (cfg.period) {
        rt_kprintf("Duty cycle : %d%%\n",
                   (int)(((double)(cfg.pulse) / (cfg.period)) * 100));
    }

    return RT_EOK;
}

static int xpwm_set(int argc, char **argv)
{
    rt_err_t result;

    if (argc == 6) {
        /* with pulse count (AIC XPWM extension) */
        result = aic_xpwm_set(xpwm_device, atoi(argv[2]), atoi(argv[3]),
                              atoi(argv[4]), atoi(argv[5]));
    } else if (argc == 5) {
        /* continuous mode */
        result = rt_pwm_set(xpwm_device, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    } else {
        rt_kprintf("test_xpwm set <channel> <period> <pulse> [<pulse cnt>] - set xpwm period/pulse (ns)\n");
        rt_kprintf("    e.g. test_xpwm set 0 1000000 500000\n");
        rt_kprintf("    e.g. test_xpwm set 0 1000000 500000 100\n");
        return -RT_ERROR;
    }

    if (result != RT_EOK) {
        rt_kprintf("Set info of channel %d error.\n", atoi(argv[2]));
        return result;
    }
    rt_kprintf("test_xpwm info set on channel %d\n", atoi(argv[2]));

    return RT_EOK;
}

static int xpwm_set_fifo_num(int argc, char **argv)
{
    struct aic_xpwm_config cfg = {0};
    rt_err_t result;

    if (argc != 4) {
        rt_kprintf("test_xpwm set_fifo_num <channel> <fifo_num> - set xpwm fifo count\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    cfg.fifo_num = atoi(argv[3]);
    result = rt_device_control(&xpwm_device->parent, AIC_PWM_CMD_SET_FIFO_NUM, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Set fifo num of channel %d error.\n", cfg.channel);
        return result;
    }
    rt_kprintf("test_xpwm set fifo num on channel %d\n", cfg.channel);

    return RT_EOK;
}

static int xpwm_set_fifo(int argc, char **argv)
{
    struct aic_xpwm_config cfg = {0};
    rt_err_t result;

    if (argc != 7) {
        rt_kprintf("test_xpwm set_fifo <channel> <fifo_index> <period> <pulse> <pulse cnt> - set xpwm fifo info\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    cfg.fifo_index = atoi(argv[3]);
    cfg.pul_prd = atoi(argv[4]);
    cfg.pul_cmp = atoi(argv[5]);
    cfg.pul_num = atoi(argv[6]);
    result = rt_device_control(&xpwm_device->parent, AIC_PWM_CMD_SET_FIFO, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Set fifo of channel %d error.\n", cfg.channel);
        return result;
    }
    rt_kprintf("test_xpwm set fifo on channel %d\n", cfg.channel);

    return RT_EOK;
}

static int xpwm_get_fifo(int argc, char **argv)
{
    struct aic_xpwm_config cfg = {0};
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_xpwm get_fifo <channel> - get xpwm fifo info\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    result = rt_device_control(&xpwm_device->parent, AIC_PWM_CMD_GET_FIFO, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Get fifo info of channel %d error.\n", cfg.channel);
        return result;
    }

    return RT_EOK;
}

#ifdef AIC_USING_DMA
static int xpwm_dma_set_fifo(int argc, char **argv)
{
    struct aic_xpwm_config cfg = {0};
    rt_uint32_t buf[3] = {0};
    rt_err_t result;

    if (argc != 6) {
        rt_kprintf("test_xpwm dma_set_fifo <channel> <period> <pulse> <pulse cnt> - set xpwm dma fifo info\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    buf[0] = atoi(argv[3]);
    buf[1] = atoi(argv[4]);
    buf[2] = atoi(argv[5]);
    cfg.buf = buf;
    cfg.buf_len = sizeof(buf);
    result = rt_device_control(&xpwm_device->parent, AIC_PWM_CMD_DMA_SET_FIFO, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("DMA set fifo of channel %d error.\n", cfg.channel);
        return result;
    }
    rt_kprintf("test_xpwm dma set fifo on channel %d\n", cfg.channel);

    return RT_EOK;
}
#endif

static int test_xpwm(int argc, char **argv)
{
    if (argc <= 1) {
        test_xpwm_usage();
        return RT_EOK;
    }

    if (xpwm_cmd_match(argv[1], "probe")) {
        return xpwm_probe(argc, argv);
    }

    if (xpwm_device == RT_NULL) {
        rt_kprintf("Please using 'test_xpwm probe <device name>' first.\n");
        return -RT_ERROR;
    }

    if (xpwm_cmd_match(argv[1], "enable")) {
        return xpwm_enable(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "disable")) {
        return xpwm_disable(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "get")) {
        return xpwm_get(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "set")) {
        return xpwm_set(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "set_fifo_num")) {
        return xpwm_set_fifo_num(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "set_fifo")) {
        return xpwm_set_fifo(argc, argv);
    } else if (xpwm_cmd_match(argv[1], "get_fifo")) {
        return xpwm_get_fifo(argc, argv);
#ifdef AIC_USING_DMA
    } else if (xpwm_cmd_match(argv[1], "dma_set_fifo")) {
        return xpwm_dma_set_fifo(argc, argv);
#endif
    }

    test_xpwm_usage();
    return -RT_ERROR;
}
MSH_CMD_EXPORT(test_xpwm, test_xpwm control device);
