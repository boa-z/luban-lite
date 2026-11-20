/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: Jiji Chen <jiji.chen@artinchip.com>
 */

#include "aic_core.h"
#include "aic_drv_axicfg.h"
#include <rtdevice.h>

struct aic_axicfg_dev {
    struct rt_device dev;
    char *name;
};

static struct aic_axicfg_dev aic_axicfg =
{
    .name = "axicfg",
};

static rt_err_t axicfg_ops_init(rt_device_t dev)
{
    return RT_EOK;
}

static rt_err_t axicfg_ops_control(rt_device_t dev, int cmd, void *args)
{
    struct aic_axicfg_qos *qos = (struct aic_axicfg_qos *)args;
    int ret;

    switch (cmd) {
    case AIC_AXICFG_CMD_SET_QOS:
        if (!qos)
            return -RT_EINVAL;
        ret = hal_axicfg_wr_set_qos(qos->mode, (hal_axicfg_port_t)qos->port,
                                    qos->priority);
        if (ret)
            return -RT_ERROR;
        break;
    case AIC_AXICFG_CMD_GET_QOS:
        if (!qos)
            return -RT_EINVAL;
        ret = hal_axicfg_wr_get_qos(qos->mode, (hal_axicfg_port_t)qos->port,
                                    &qos->priority);
        if (ret)
            return -RT_ERROR;
        break;
    default:
        return -RT_EINVAL;
    }

    return RT_EOK;
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops aic_axicfg_ops =
{
    axicfg_ops_init,
    NULL,
    NULL,
    NULL,
    NULL,
    axicfg_ops_control,
};
#endif

int drv_axicfg_init(void)
{
#ifdef RT_USING_DEVICE_OPS
    aic_axicfg.dev.ops = &aic_axicfg_ops;
#else
    aic_axicfg.dev.init = axicfg_ops_init;
    aic_axicfg.dev.control = axicfg_ops_control;
    aic_axicfg.dev.type = RT_Device_Class_Miscellaneous;
#endif
    return rt_device_register(&aic_axicfg.dev, aic_axicfg.name, 0);
}

INIT_DEVICE_EXPORT(drv_axicfg_init);

int aic_axicfg_wr_set_qos(u8 mode, hal_axicfg_port_t port, u8 priority)
{
    rt_device_t dev;
    struct aic_axicfg_qos qos;

    dev = rt_device_find("axicfg");
    if (!dev)
        return -RT_ERROR;

    qos.port = (u8)port;
    qos.mode = mode;
    qos.priority = priority;
    return rt_device_control(dev, AIC_AXICFG_CMD_SET_QOS, &qos);
}

int aic_axicfg_set_qos(hal_axicfg_port_t port, u8 priority)
{
    u8 mode;
    int ret;

    for (mode = 0; mode < 2; mode++) {
        // read: mode = 0; write: mode = 1;
        ret = aic_axicfg_wr_set_qos(mode, port, priority);
        if (ret)
            return ret;
    }

    return 0;
}

int aic_axicfg_get_qos(hal_axicfg_port_t port, u8 *priority)
{
    rt_device_t dev;
    struct aic_axicfg_qos qos;

    dev = rt_device_find("axicfg");
    if (!dev)
        return -RT_ERROR;

    qos.port = (u8)port;
    qos.mode = HAL_AXICFG_W_READ;
    if (rt_device_control(dev, AIC_AXICFG_CMD_GET_QOS, &qos))
        return -RT_ERROR;

    if (priority)
        *priority = qos.priority;
    return 0;
}
