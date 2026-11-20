/*
 * Copyright (c) 2024-2026, ArtInChip Technology CO.,LTD. All Rights Reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Author: Qi <qi.xu@artinchip.com>
 */

#include "aic_core.h"
#include "aic_log.h"
#include "aic_drv_dce.h"
#include "hal_dce.h"

#include <rtdevice.h>

/*
 * DCE(checksum/crc32) is a standalone module, its registers and its module
 * clock are shared by every user(hwcrypto, mpp, application ...), so two
 * tasks doing a calculation at the same time will overwrite each other's
 * DCE_ADDR/CFG/CTL and read back a wrong result.
 *
 * All the users have to go through this driver:
 *   - the whole sequence of a calculation(start -> wait -> result) is done
 *     with the lock held, so no other user can touch the registers in between
 *   - the module clock is reference counted, it is kept on until the last
 *     user is gone, so a short calculation never gates the clock of another
 *     one which is still running
 */
struct aic_dce_dev {
    struct rt_device dev;
    struct rt_mutex lock;
    int ref_count;
};

static struct aic_dce_dev aic_dce = {0};

/* the caller must hold the lock */
static int dce_clk_enable(void)
{
    int ret = 0;

    if (aic_dce.ref_count++ == 0) {
        ret = hal_dce_init();
        if (ret) {
            aic_dce.ref_count = 0;
            return -RT_ERROR;
        }
    }

    return RT_EOK;
}

/* the caller must hold the lock */
static void dce_clk_disable(void)
{
    if (--aic_dce.ref_count <= 0) {
        aic_dce.ref_count = 0;
        hal_dce_deinit();
    }
}

static int dce_checksum(struct dce_buf *buf)
{
    int ret;

    if (!buf || !buf->addr || !buf->len)
        return -RT_EINVAL;

    rt_mutex_take(&aic_dce.lock, RT_WAITING_FOREVER);

    ret = dce_clk_enable();
    if (ret == RT_EOK) {
        hal_dce_checksum_start((u8 *)(unsigned long)buf->addr, buf->len);
        if (hal_dce_checksum_wait() == 0)
            buf->result = hal_dce_checksum_result();
        else
            ret = -RT_ETIMEOUT;
        dce_clk_disable();
    }

    rt_mutex_release(&aic_dce.lock);

    return ret;
}

static int dce_crc32(struct dce_buf *buf)
{
    int ret;

    if (!buf || !buf->addr || !buf->len)
        return -RT_EINVAL;

    rt_mutex_take(&aic_dce.lock, RT_WAITING_FOREVER);

    ret = dce_clk_enable();
    if (ret == RT_EOK) {
        /* the config registers of the crc32 are shared as well, but they are
         * not a part of the interface: they are left at their default value
         * for every calculation, so that the result of a crc does not depend
         * on the module which used the DCE before */
        hal_dce_crc32_start(0, (u8 *)(unsigned long)buf->addr, buf->len);
        if (hal_dce_crc32_wait() == 0)
            buf->result = hal_dce_crc32_result();
        else
            ret = -RT_ETIMEOUT;
        dce_clk_disable();
    }

    rt_mutex_release(&aic_dce.lock);

    return ret;
}

static rt_err_t dce_ops_init(rt_device_t dev)
{
    return RT_EOK;
}

static rt_err_t dce_ops_control(rt_device_t dev, int cmd, void *args)
{
    switch (cmd) {
    case DCE_CMD_GET_VERSION:
        if (!args)
            return -RT_EINVAL;
        *(unsigned int *)args = hal_get_version();
        return RT_EOK;
    case DCE_CMD_CHECKSUM:
        return dce_checksum((struct dce_buf *)args);
    case DCE_CMD_CRC32:
        return dce_crc32((struct dce_buf *)args);
    default:
        return -RT_EINVAL;
    }
}

#ifdef RT_USING_DEVICE_OPS
static const struct rt_device_ops aic_dce_ops = {
    dce_ops_init,
    NULL,
    NULL,
    NULL,
    NULL,
    dce_ops_control,
};
#endif

static int drv_dce_device_init(void)
{
    rt_err_t ret;

#ifdef RT_USING_DEVICE_OPS
    aic_dce.dev.ops = &aic_dce_ops;
#else
    aic_dce.dev.type = RT_Device_Class_Miscellaneous;
    aic_dce.dev.init = dce_ops_init;
    aic_dce.dev.control = dce_ops_control;
#endif

    /* the lock has to be ready before the device becomes visible */
    rt_mutex_init(&aic_dce.lock, DCE_DEV_NAME, RT_IPC_FLAG_PRIO);

    ret = rt_device_register(&aic_dce.dev, DCE_DEV_NAME, RT_DEVICE_FLAG_RDWR);
    if (ret != RT_EOK) {
        pr_err("register %s device failed\n", DCE_DEV_NAME);
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_DEVICE_EXPORT(drv_dce_device_init);

static rt_device_t dce_get_device(void)
{
    static rt_device_t dev = NULL;

    if (!dev)
        dev = rt_device_find(DCE_DEV_NAME);

    return dev;
}

int drv_dce_get_version(unsigned int *version)
{
    rt_device_t dev = dce_get_device();

    if (!dev || !version)
        return -RT_ERROR;

    return rt_device_control(dev, DCE_CMD_GET_VERSION, version);
}

int drv_dce_checksum(unsigned int addr, unsigned int len, unsigned int *result)
{
    struct dce_buf buf;
    rt_device_t dev = dce_get_device();
    int ret;

    if (!dev || !result)
        return -RT_ERROR;

    buf.addr = addr;
    buf.len = len;
    buf.result = 0;

    ret = rt_device_control(dev, DCE_CMD_CHECKSUM, &buf);
    if (ret == RT_EOK)
        *result = buf.result;

    return ret;
}

int drv_dce_crc32(unsigned int addr, unsigned int len, unsigned int *result)
{
    struct dce_buf buf;
    rt_device_t dev = dce_get_device();
    int ret;

    if (!dev || !result)
        return -RT_ERROR;

    buf.addr = addr;
    buf.len = len;
    buf.result = 0;

    ret = rt_device_control(dev, DCE_CMD_CRC32, &buf);
    if (ret == RT_EOK)
        *result = buf.result;

    return ret;
}
