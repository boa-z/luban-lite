/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: Mingfeng.Li <mingfeng.li@artinchip.com>
 */

#include <rtconfig.h>
#if defined(RT_USING_FINSH) && defined(AIC_AXICFG_DRV)
#include <rtthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <aic_common.h>
#include <aic_drv_axicfg.h>

#define printf rt_kprintf

static const char *axicfg_port_name[HAL_AXICFG_PORT_MAX] = {
    "CPU", "AHB",
#ifdef AIC_CHIP_D12P
    "AHB1",
#endif
    "DE", "GE", "VE", "DVP",
#ifndef AIC_CHIP_D12P
    "CE",
#endif
};

#define AXICFG_HELP                                                    \
    "axicfg command:\n"                                                \
    "  axicfg (no args): show all ports' QoS\n"                       \
    "  axicfg <device>: show the port's QoS\n"                        \
    "  axicfg <device> <mode> <priority>: set QoS at runtime\n"       \
    "    device: dec value, get from chip spec\n"                     \
    "            (0:CPU 1:AHB 2:AHB1/DE 3:DE/GE ...)\n"               \
    "    mode: should be \"w\" \"r\" \"a\" \n"                        \
    "    priority: 0~15, larger value means higher priority\n"        \
    "  e.g.: \n"                                                      \
    "    axicfg 2 a 10  (read and write mode)\n"                      \
    "    axicfg 2 w 10\n"                                             \
    "    axicfg 2 r 10\n"

static void axicfg_config_display_help(void)
{
    puts(AXICFG_HELP);
}

static void axicfg_config_display_ports(void)
{
    u8 priority;
    int i;

    printf("Port QoS:\n");
    for (i = 0; i < HAL_AXICFG_PORT_MAX; i++) {
        if (aic_axicfg_get_qos((hal_axicfg_port_t)i, &priority)) {
            printf("  %-4s read error\n", axicfg_port_name[i]);
            continue;
        }
        printf("  %-4s %d\n", axicfg_port_name[i], priority);
    }
}

static void do_axicfg_config(int argc, char **argv)
{
    hal_axicfg_port_t device_p;
    u8 priority;
    u8 mode;
    int ret;

    if (argc == 1) {
        axicfg_config_display_ports();
        return;
    }

    device_p = (hal_axicfg_port_t)atoi(argv[1]);
    if ((u8)device_p >= HAL_AXICFG_PORT_MAX)
        goto help;

    if (argc == 2) {
        ret = aic_axicfg_get_qos(device_p, &priority);
        if (ret) {
            printf("Get %s QoS error\n", axicfg_port_name[device_p]);
            return;
        }
        printf("%s QoS: %d\n", axicfg_port_name[device_p], priority);
        return;
    }

    if (argc < 4)
        goto help;

    priority = (u8)atoi(argv[3]);
    if (priority > 15)
        goto help;

    if (*argv[2] == 'w') {
        mode = HAL_AXICFG_W_WRITE;
        ret = aic_axicfg_wr_set_qos(mode, device_p, priority);
    } else if (*argv[2] == 'r') {
        mode = HAL_AXICFG_W_READ;
        ret = aic_axicfg_wr_set_qos(mode, device_p, priority);
    } else if (*argv[2] == 'a') {
        ret = aic_axicfg_set_qos(device_p, priority);
    } else {
        goto help;
    }

    if (ret) {
        printf("Set %s QoS error\n", axicfg_port_name[device_p]);
        return;
    }
    printf("Set %s QoS to %d\n", axicfg_port_name[device_p], priority);
    return;

help:
    axicfg_config_display_help();
    return;
}

MSH_CMD_EXPORT_ALIAS(do_axicfg_config, axicfg, config IP access AXI priority);

#endif /* RT_USING_FINSH && AIC_AXICFG_DRV */
