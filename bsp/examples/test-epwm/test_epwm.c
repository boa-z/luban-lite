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
#include <aic_drv_epwm.h>

static struct rt_device_pwm *epwm_device = RT_NULL;

/* bounded string compare for shell command matching */
static int epwm_cmd_match(const char *cmd, const char *name)
{
    return (strncmp(cmd, name, strlen(name)) == 0) && (cmd[strlen(name)] == '\0');
}

static void test_epwm_usage(void)
{
    rt_kprintf("Usage: \n");
    rt_kprintf("test_epwm probe      <device name>                           - probe epwm by name\n");
    rt_kprintf("test_epwm enable     <channel>                               - enable epwm channel\n");
    rt_kprintf("test_epwm disable    <channel>                               - disable epwm channel\n");
    rt_kprintf("test_epwm get        <channel>                               - get epwm channel info\n");
    rt_kprintf("test_epwm set        <channel> <period> <pulse>              - set period/pulse (ns), output A+B\n");
    rt_kprintf("test_epwm set_output <channel> <period> <pulse> <output>     - set one output (0:A 1:B 2:A+B)\n");
    rt_kprintf("    e.g. test_epwm set 1 1000000 500000           - EPWM1 output A+B, 1KHz 50%%\n");
    rt_kprintf("    e.g. test_epwm set_output 2 1000000 200000 1  - EPWM2 output B, 1KHz 20%%\n");
}

static int epwm_probe(int argc, char **argv)
{
    if (argc != 3) {
        rt_kprintf("test_epwm probe <device name> - probe epwm by name\n");
        return -RT_ERROR;
    }

    epwm_device = (struct rt_device_pwm *)rt_device_find(argv[2]);
    rt_kprintf("probe %s %s\n", argv[2],
               (epwm_device == RT_NULL) ? "failure" : "success");

    return (epwm_device == RT_NULL) ? -RT_ERROR : RT_EOK;
}

static int epwm_enable(int argc, char **argv)
{
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_epwm enable <channel> - enable epwm channel\n");
        return -RT_ERROR;
    }

    result = rt_pwm_enable(epwm_device, atoi(argv[2]));
    rt_kprintf("channel %d is enabled %s\n", atoi(argv[2]),
               (result == RT_EOK) ? "success" : "failure");

    return result;
}

static int epwm_disable(int argc, char **argv)
{
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_epwm disable <channel> - disable epwm channel\n");
        return -RT_ERROR;
    }

    result = rt_pwm_disable(epwm_device, atoi(argv[2]));
    rt_kprintf("channel %d is disabled %s\n", atoi(argv[2]),
               (result == RT_EOK) ? "success" : "failure");

    return result;
}

static int epwm_get(int argc, char **argv)
{
    struct rt_pwm_configuration cfg = {0};
    rt_err_t result;

    if (argc != 3) {
        rt_kprintf("test_epwm get <channel> - get epwm channel info\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    result = rt_device_control(&epwm_device->parent, PWM_CMD_GET, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Get info of channel %d error.\n", cfg.channel);
        return result;
    }

    rt_kprintf("Info of channel %d:\n", cfg.channel);
    rt_kprintf("period : %d ns\n", cfg.period);
    rt_kprintf("pulse  : %d ns\n", cfg.pulse);
    if (cfg.period)
        rt_kprintf("Duty cycle : %d%%\n",
                   (rt_uint32_t)((rt_uint64_t)cfg.pulse * 100 / cfg.period));

    return RT_EOK;
}

static int epwm_set(int argc, char **argv)
{
    rt_err_t result;

    if (argc != 5) {
        rt_kprintf("test_epwm set <channel> <period> <pulse> - set period/pulse (ns)\n");
        return -RT_ERROR;
    }

    result = rt_pwm_set(epwm_device, atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));
    if (result != RT_EOK) {
        rt_kprintf("Set info of channel %d error.\n", atoi(argv[2]));
        return result;
    }
    rt_kprintf("test_epwm set channel %d: period %d ns, pulse %d ns\n",
               atoi(argv[2]), atoi(argv[3]), atoi(argv[4]));

    return RT_EOK;
}

static int epwm_set_output(int argc, char **argv)
{
    struct aic_epwm_config cfg = {0};
    rt_err_t result;

    if (argc != 6) {
        rt_kprintf("test_epwm set_output <channel> <period> <pulse> <output> - set one output\n");
        return -RT_ERROR;
    }

    cfg.channel = atoi(argv[2]);
    cfg.period = atoi(argv[3]);
    cfg.pulse = atoi(argv[4]);
    cfg.output = atoi(argv[5]);

    result = rt_device_control(&epwm_device->parent, AIC_EPWM_CMD_SET_OUTPUT, &cfg);
    if (result != RT_EOK) {
        rt_kprintf("Set output of channel %d error.\n", cfg.channel);
        return result;
    }
    rt_kprintf("test_epwm set_output channel %d: output %d, period %d ns, pulse %d ns\n",
               cfg.channel, cfg.output, cfg.period, cfg.pulse);

    return RT_EOK;
}

static int test_epwm(int argc, char **argv)
{
    if (argc <= 1) {
        test_epwm_usage();
        return RT_EOK;
    }

    if (epwm_cmd_match(argv[1], "probe")) {
        return epwm_probe(argc, argv);
    }

    if (epwm_device == RT_NULL) {
        rt_kprintf("Please using 'test_epwm probe <device name>' first.\n");
        return -RT_ERROR;
    }

    if (epwm_cmd_match(argv[1], "enable")) {
        return epwm_enable(argc, argv);
    } else if (epwm_cmd_match(argv[1], "disable")) {
        return epwm_disable(argc, argv);
    } else if (epwm_cmd_match(argv[1], "get")) {
        return epwm_get(argc, argv);
    } else if (epwm_cmd_match(argv[1], "set")) {
        return epwm_set(argc, argv);
    } else if (epwm_cmd_match(argv[1], "set_output")) {
        return epwm_set_output(argc, argv);
    }

    test_epwm_usage();
    return -RT_ERROR;
}
MSH_CMD_EXPORT(test_epwm, test_epwm control device);
