/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: Jiji Chen <jiji.chen@artinchip.com>
 */

#ifndef _AIC_DRV_AXICFG_H_
#define _AIC_DRV_AXICFG_H_

#include "hal_axicfg.h"

enum {
    AIC_AXICFG_CMD_SET_QOS,
    AIC_AXICFG_CMD_GET_QOS,
};

struct aic_axicfg_qos {
    u8 port;     /* hal_axicfg_port_t */
    u8 mode;     /* HAL_AXICFG_W_READ / HAL_AXICFG_W_WRITE */
    u8 priority; /* 0~15, larger value means higher priority */
};

int aic_axicfg_wr_set_qos(u8 mode, hal_axicfg_port_t port, u8 priority);
int aic_axicfg_set_qos(hal_axicfg_port_t port, u8 priority);
int aic_axicfg_get_qos(hal_axicfg_port_t port, u8 *priority);

#endif
