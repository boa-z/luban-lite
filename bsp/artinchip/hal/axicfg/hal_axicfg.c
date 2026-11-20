/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: Mingfeng.Li <mingfeng.li@artinchip.com>
 */

#include <rtconfig.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdint.h>
#include <aic_common.h>
#include <aic_core.h>
#include <aic_hal.h>
#include <hal_dma.h>
#include <hal_axicfg.h>
#include "axicfg_hw_v1.0.h"
#include "aic_hal_reset.h"


int hal_axicfg_module_wr_init(u8 mode, hal_axicfg_port_t device_p, u8 priority)
{
    // read: mode = 0; write: mode = 1;
    axicfg_hw_cfg0_set_reulator_en(HAL_AXICFG_BASE, mode, (u8)device_p, 0);
    axicfg_hw_cfg0_set_slv_ready(HAL_AXICFG_BASE, mode, (u8)device_p, 0);
    axicfg_hw_cfg0_set_burst_limit(HAL_AXICFG_BASE, mode, (u8)device_p, 0);
    axicfg_hw_cfg0_set_qos_sel(HAL_AXICFG_BASE, mode, (u8)device_p, HAL_AXICFG_INT_QOS);
    axicfg_hw_cfg0_set_qos_val(HAL_AXICFG_BASE, mode, (u8)device_p, priority);
    axicfg_hw_cfg1_set_basic_rate(HAL_AXICFG_BASE, mode, (u8)device_p, 0);
    axicfg_hw_cfg1_set_burst_rate(HAL_AXICFG_BASE, mode, (u8)device_p, 0);

    return 0;
}

int hal_axicfg_module_init(hal_axicfg_port_t device_p, u8 priority)
{
    u8 mode = 0;

    for (mode = 0; mode < 2; mode++) {
        // read: mode = 0; write: mode = 1;
        hal_axicfg_module_wr_init(mode, device_p, priority);
    }

    return 0;
}

static int hal_axicfg_check_param(u8 mode, hal_axicfg_port_t device_p,
                                  u8 priority)
{
    if (mode > HAL_AXICFG_W_WRITE) {
        hal_log_err("mode %d is out of range, max num: %d\n",
                    mode, HAL_AXICFG_W_WRITE);
        return -EINVAL;
    }
    if ((u8)device_p >= HAL_AXICFG_PORT_MAX) {
        hal_log_err("port %d is out of range, max num: %d\n",
                    device_p, HAL_AXICFG_PORT_MAX - 1);
        return -EINVAL;
    }
    if (priority > 15) {
        hal_log_err("priority %d is out of range, max num: %d\n",
                    priority, 15);
        return -EINVAL;
    }

    return 0;
}

int hal_axicfg_wr_set_qos(u8 mode, hal_axicfg_port_t device_p, u8 priority)
{
    int ret;

    ret = hal_axicfg_check_param(mode, device_p, priority);
    if (ret)
        return ret;

    /* Switch to INT_QOS, so QOS_VAL takes effect */
    axicfg_hw_cfg0_set_qos_sel(HAL_AXICFG_BASE, mode, (u8)device_p,
                               HAL_AXICFG_INT_QOS);
    axicfg_hw_cfg0_set_qos_val(HAL_AXICFG_BASE, mode, (u8)device_p, priority);

    return 0;
}

int hal_axicfg_set_qos(hal_axicfg_port_t device_p, u8 priority)
{
    u8 mode = 0;
    int ret;

    for (mode = 0; mode < 2; mode++) {
        // read: mode = 0; write: mode = 1;
        ret = hal_axicfg_wr_set_qos(mode, device_p, priority);
        if (ret)
            return ret;
    }

    return 0;
}

int hal_axicfg_wr_get_qos(u8 mode, hal_axicfg_port_t device_p, u8 *priority)
{
    u32 val;
    int ret;

    if (!priority)
        return -EINVAL;

    ret = hal_axicfg_check_param(mode, device_p, 0);
    if (ret)
        return ret;

    val = readl(AXICFG_REG_CFG0(HAL_AXICFG_BASE, mode, (u8)device_p));
    *priority = (val & CFG0_BIT_QOS_VAL_MSK) >> CFG0_BIT_QOS_VAL_OFS;

    return 0;
}

int hal_axicfg_get_qos(hal_axicfg_port_t device_p, u8 *priority)
{
    // read channel: mode = 0;
    return hal_axicfg_wr_get_qos(HAL_AXICFG_W_READ, device_p, priority);
}
