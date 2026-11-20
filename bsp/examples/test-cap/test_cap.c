/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include "boot_param.h"
#include "aic_core.h"

#define WATER_MARK      50
#define REPORT_MS       1000

/*
 * Layout must match struct aic_cap_data exposed through dev->user_data by
 * the CAP driver (bsp/artinchip/drv/cap/drv_cap.c).
 */
struct aic_cap_usr {
    rt_uint8_t id;
    rt_uint32_t freq;
    float duty;
};

struct aic_cap_test {
    struct rt_semaphore sem;
    rt_device_t dev;
    volatile rt_bool_t running;
    rt_uint32_t ch;
    rt_uint32_t time_us;
    rt_uint32_t start_us;
    rt_uint32_t cnt;            /* captured samples */
    rt_uint32_t pulsewidth_us;  /* latest captured pulsewidth */
};

static struct aic_cap_test g_test[AIC_CAPS_CH_NUM] = {0};

static rt_err_t cap_cb(rt_device_t dev, rt_size_t size)
{
    struct aic_cap_usr *data = (struct aic_cap_usr *)dev->user_data;

    if ((data == RT_NULL) || !g_test[data->id].running)
        return -RT_ERROR;

    rt_sem_release(&g_test[data->id].sem);
    return RT_EOK;
}

static void cap_test_thread(void *param)
{
    struct aic_cap_test *test = (struct aic_cap_test *)param;
    struct aic_cap_usr *usr = (struct aic_cap_usr *)test->dev->user_data;
    struct rt_inputcapture_data data[WATER_MARK];
    rt_uint32_t elapsed, last_report = 0;
    rt_size_t n;

    while (test->running) {
        elapsed = aic_timer_get_us() - test->start_us;
        if (elapsed >= test->time_us)
            break;

        /* wait for data, at most 1s so the report below still shows up */
        rt_sem_take(&test->sem,
                    rt_tick_from_millisecond((test->time_us - elapsed <
                                              REPORT_MS * 1000UL) ?
                                             (test->time_us - elapsed) / 1000 :
                                             REPORT_MS));

        /* read out all pending samples */
        while ((n = rt_device_read(test->dev, 0, data,
                                   sizeof(data) / sizeof(data[0]))) > 0) {
            test->cnt += n;
            test->pulsewidth_us = data[n - 1].pulsewidth_us;
        }

        /* report once per second: freq/duty/latest pulsewidth/count */
        if (elapsed / 1000 - last_report >= REPORT_MS) {
            rt_kprintf("cap%d: freq:%dHz, duty:%d.%02d%%, pulsewidth:%d us, samples:%d\n",
                       test->ch, usr ? usr->freq : 0,
                       usr ? (rt_uint32_t)usr->duty : 0,
                       usr ? (rt_uint32_t)(usr->duty * 100) % 100 : 0,
                       test->pulsewidth_us, test->cnt);
            last_report = elapsed / 1000;
        }
    }

    test->running = RT_FALSE;
    rt_device_close(test->dev);
    rt_sem_detach(&test->sem);
#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_NONE);
#endif
    rt_kprintf("cap%d test done: %d samples\n", test->ch, test->cnt);
}

int test_cap(int argc, char **argv)
{
    struct aic_cap_test *test;
    rt_device_t cap_dev;
    rt_thread_t tid;
    char dev_name[8];
    int ch, ret;

    if (argc < 2) {
        rt_kprintf("Usage: test_cap channel [second]\n");
        return -RT_ERROR;
    }

    ch = atoi(argv[1]);
    if ((ch < 0) || (ch >= AIC_CAPS_CH_NUM)) {
        rt_kprintf("Invalid channel %d, range: 0-%d!\n", ch, AIC_CAPS_CH_NUM - 1);
        return -RT_ERROR;
    }
    if (g_test[ch].running) {
        rt_kprintf("cap%d is already under test!\n", ch);
        return -RT_ERROR;
    }

    snprintf(dev_name, sizeof(dev_name), "cap%d", ch);
    cap_dev = rt_device_find(dev_name);
    if (cap_dev == RT_NULL) {
        rt_kprintf("Can't find %s device!\n", dev_name);
        return -RT_ERROR;
    }

    test = &g_test[ch];
    memset(test, 0, sizeof(*test));
    test->dev = cap_dev;
    test->ch = ch;
    test->time_us = (argc >= 3) ? atoi(argv[2]) * US_PER_SEC : 10 * US_PER_SEC;
    test->start_us = aic_timer_get_us();
    rt_sem_init(&test->sem, "cap", 0, RT_IPC_FLAG_FIFO);

    rt_device_set_rx_indicate(cap_dev, cap_cb);

    ret = rt_device_open(cap_dev, RT_DEVICE_OFLAG_RDWR);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to open %s device!\n", dev_name);
        /* undo rt_sem_init() so a later retry does not memset an object
         * that is still linked into the kernel object container */
        rt_sem_detach(&test->sem);
        rt_device_set_rx_indicate(cap_dev, RT_NULL);
        return ret;
    }

    test->running = RT_TRUE;
    tid = rt_thread_create("cap_test", cap_test_thread, test,
                           1024, RT_THREAD_PRIORITY_MAX / 3, 20);
    if (tid == RT_NULL) {
        test->running = RT_FALSE;
        rt_sem_detach(&test->sem);
        rt_device_set_rx_indicate(cap_dev, RT_NULL);
        rt_device_close(cap_dev);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

#ifdef RT_USING_PM
    rt_pm_request(PM_SLEEP_MODE_NONE);
#endif
    rt_kprintf("cap%d open, test %d seconds.\n", ch, test->time_us / US_PER_SEC);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(test_cap, test_cap, Test the cap);
