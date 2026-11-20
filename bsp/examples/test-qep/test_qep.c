/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <string.h>
#include <stdlib.h>
#include "aic_core.h"

#define REPORT_MS       200

struct aic_qep_test {
    struct rt_semaphore sem;    /* released when the target count is reached */
    rt_device_t dev;            /* qep device */
    rt_device_t pwm_dev;        /* pwm device generating the encoder input */
    volatile rt_bool_t running;
    rt_uint32_t ch;
    rt_uint32_t pwm_ch;
    rt_uint32_t pulse_count;    /* target count */
};

static struct aic_qep_test g_test[AIC_QEP_CH_NUM] = {0};

/*
 * Called in the QEP interrupt context when the position count reaches the
 * configured compare value. The ISR only wakes up the test thread: the pwm
 * is stopped from the thread context, since rt_pwm_disable() goes through
 * rt_device_control() and the pwm driver control path is not guaranteed to
 * be free of locks or scheduling. The thread also owns the device close.
 */
static rt_err_t qep_cb(rt_device_t dev, void *buffer)
{
    rt_uint32_t *p = (rt_uint32_t *)buffer;
    rt_uint32_t ch;
    struct aic_qep_test *test;

    if ((p == RT_NULL) || (dev == RT_NULL))
        return -RT_ERROR;

    ch = *p;
    if (ch >= AIC_QEP_CH_NUM)
        return -RT_ERROR;

    test = &g_test[ch];
    if (!test->running)
        return -RT_ERROR;

    rt_sem_release(&test->sem);
    return RT_EOK;
}

static void qep_test_thread(void *param)
{
    struct aic_qep_test *test = (struct aic_qep_test *)param;
    rt_err_t ret;
    rt_int32_t cnt;

#ifdef RT_USING_PM
    rt_pm_request(PM_SLEEP_MODE_NONE);
#endif

    while (test->running) {
        ret = rt_sem_take(&test->sem, rt_tick_from_millisecond(REPORT_MS));

        /*
         * The compare interrupt fired: stop the pulse generator first so
         * the pulse train does not run on while the test finishes.
         */
        if (ret == RT_EOK)
            rt_pwm_disable((struct rt_device_pwm *)test->pwm_dev,
                           test->pwm_ch);

        /* aborted by a new shell command run */
        if (!test->running)
            break;

        cnt = 0;
        rt_device_read(test->dev, 0, &cnt, 0);

        if (ret == RT_EOK) {
            /* target count reached: close the qep and finish the test */
            rt_kprintf("qep%d reached %d pulses, count:%d\n",
                       test->ch, test->pulse_count, cnt);
            break;
        }

        rt_kprintf("qep%d count:%d/%d\n", test->ch, cnt, test->pulse_count);
    }

    /* the test thread is the only owner of the device and the semaphore */
    rt_pwm_disable((struct rt_device_pwm *)test->pwm_dev, test->pwm_ch);
    rt_device_set_tx_complete(test->dev, RT_NULL);
    rt_device_close(test->dev);
    test->running = RT_FALSE;
    rt_sem_detach(&test->sem);

#ifdef RT_USING_PM
    rt_pm_release(PM_SLEEP_MODE_NONE);
#endif
}

/*
 * Stop the test from the shell context after the test thread has been
 * started. The test thread is the only owner of the qep device and of the
 * semaphore, so this function must not close the device or touch the
 * callbacks: it only flips the flag (and mutes the pulse train) and lets
 * the thread do the whole cleanup when it observes running == RT_FALSE.
 */
static void qep_test_abort(struct aic_qep_test *test)
{
    test->running = RT_FALSE;
    rt_pwm_disable((struct rt_device_pwm *)test->pwm_dev, test->pwm_ch);
}

int test_qep(int argc, char **argv)
{
    struct aic_qep_test *test;
    rt_device_t qep_dev, pwm_dev;
    rt_thread_t tid;
    rt_uint32_t pulse_count;
    rt_uint32_t period_ns, duty_ns, pwm_ch;
    char dev_name[8] = "";
    int ch, ret;

    if (argc < 6) {
        rt_kprintf("usage: test_qep <qep_channel> <pwm_channel> <period_ns> <duty_ns> <pulse_count>\n");
        rt_kprintf("example: test_qep 0 0 1000 500 100\n");
        return -RT_ERROR;
    }

    ch = atoi(argv[1]);
    if ((ch < 0) || (ch >= AIC_QEP_CH_NUM)) {
        rt_kprintf("Invalid qep channel %d, range: 0-%d!\n", ch,
                   AIC_QEP_CH_NUM - 1);
        return -RT_ERROR;
    }
    if (g_test[ch].running) {
        rt_kprintf("qep%d is already under test!\n", ch);
        return -RT_ERROR;
    }

    pwm_ch = atoi(argv[2]);
    period_ns = atoi(argv[3]);
    duty_ns = atoi(argv[4]);
    pulse_count = atoi(argv[5]);
    if (pulse_count == 0) {
        rt_kprintf("Invalid pulse count 0!\n");
        return -RT_ERROR;
    }

    snprintf(dev_name, sizeof(dev_name), "qep%d", ch);
    qep_dev = rt_device_find(dev_name);
    if (qep_dev == RT_NULL) {
        rt_kprintf("Can't find %s device!\n", dev_name);
        return -RT_ERROR;
    }

    pwm_dev = rt_device_find("xpwm");
    if (pwm_dev == RT_NULL) {
        rt_kprintf("Can't find pwm device!\n");
        return -RT_ERROR;
    }

    test = &g_test[ch];
    memset(test, 0, sizeof(*test));
    test->dev = qep_dev;
    test->pwm_dev = pwm_dev;
    test->ch = ch;
    test->pwm_ch = pwm_ch;
    test->pulse_count = pulse_count;
    rt_sem_init(&test->sem, "qep", 0, RT_IPC_FLAG_FIFO);

    /* fired in the QEP ISR when the count reaches the compare value */
    rt_device_set_tx_complete(qep_dev, qep_cb);

    ret = rt_device_open(qep_dev, RT_DEVICE_OFLAG_RDWR);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to open %s device!\n", dev_name);
        rt_sem_detach(&test->sem);
        rt_device_set_tx_complete(qep_dev, RT_NULL);
        return ret;
    }

    /*
     * Start the counting from zero: the position compare interrupt fires
     * only when the count reaches the compare value, a leftover count
     * above it would never match again.
     */
    ret = rt_device_control(qep_dev, PULSE_ENCODER_CMD_CLEAR_COUNT, NULL);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to clear the count of %s device!\n", dev_name);
        rt_sem_detach(&test->sem);
        rt_device_set_tx_complete(qep_dev, RT_NULL);
        rt_device_close(qep_dev);
        return ret;
    }

    test->running = RT_TRUE;
    tid = rt_thread_create("qep_test", qep_test_thread, test,
                           1024, RT_THREAD_PRIORITY_MAX / 3, 20);
    if (tid == RT_NULL) {
        test->running = RT_FALSE;
        rt_sem_detach(&test->sem);
        rt_device_set_tx_complete(qep_dev, RT_NULL);
        rt_device_close(qep_dev);
        return -RT_ENOMEM;
    }
    rt_thread_startup(tid);

    /* interrupt when the count reaches (pulse_count - 1) */
    ret = rt_device_control(qep_dev, PULSE_ENCODER_CMD_SET_COUNT,
                            &pulse_count);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to set the pulse count of %s device!\n", dev_name);
        qep_test_abort(test);
        return ret;
    }

    /* pwm pulses feed the qep input as the simulated encoder signal */
    ret = rt_pwm_set((struct rt_device_pwm *)pwm_dev, pwm_ch,
                     period_ns, duty_ns);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to set pwm%d!\n", pwm_ch);
        qep_test_abort(test);
        return ret;
    }

    ret = rt_pwm_enable((struct rt_device_pwm *)pwm_dev, pwm_ch);
    if (ret != RT_EOK) {
        rt_kprintf("Failed to enable pwm%d!\n", pwm_ch);
        qep_test_abort(test);
        return ret;
    }

    rt_kprintf("qep%d test start, pulse count:%d\n", ch, pulse_count);
    return RT_EOK;
}
MSH_CMD_EXPORT_ALIAS(test_qep, test_qep, Test the qep);
