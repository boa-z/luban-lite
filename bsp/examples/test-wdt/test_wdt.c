/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>
#include <aic_core.h>
#include <stdio.h>
#include <drivers/watchdog.h>
#include <aic_drv_wdt.h>
#include <getopt.h>

#define AIC_WDT_DEV_NAME            "wdt"
#define KEEP_FEED_TIMES             5
#define WDT_WRITE_DISABLED_LEVEL    3

static volatile aicos_sem_t g_wdt_sem = NULL;
static u64 g_wdt_start_ms = 0;
static int g_wdt_feed_timeout = 0;

static void wdt_irq_cb(void)
{
    printf("Watchdog IRQ pretimeout happened after %ld ms\n",
           (long)(aic_get_time_ms() - g_wdt_start_ms));

    if (g_wdt_sem)
        aicos_sem_give(g_wdt_sem);
}

static void wdt_feed_thread_entry(void *parameter)
{
    int timeout = *(int *)parameter * 1000;
    rt_device_t wdt_dev;
    int i;

    if (timeout <= 0)
        timeout = 1000;

    wdt_dev = rt_device_find(AIC_WDT_DEV_NAME);
    if (!wdt_dev) {
        printf("Failed to find %s\n", AIC_WDT_DEV_NAME);
        return;
    }

    for (i = 0; i < KEEP_FEED_TIMES; i++) {
        aicos_msleep(timeout);
        printf("[%lld] %d. Feed watchdog\n", (long long)aic_get_time_ms(), i);
        if (rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_KEEPALIVE, NULL) != RT_EOK) {
            printf("Feed failed, the watchdog is not running\n");
            return;
        }
    }

    rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_STOP, NULL);
}

/*
 * A failed option aborts the run, so put the pretimeout IRQ and its semaphore
 * back to the idle state here instead of leaving them for the next command.
 */
static void wdt_irq_cleanup(rt_device_t wdt_dev)
{
    rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_IRQ_DISABLE, RT_NULL);

    if (g_wdt_sem) {
        aicos_sem_delete(g_wdt_sem);
        g_wdt_sem = NULL;
    }
}

static void usage(char *program)
{
    printf("\n");
    printf("Usage: %s [-s timeout] [-p pretimeout] [-c clear] [-w level] [-g] [-k] [-h]\n",
           program);
    printf("\t -s, --set-timeout\tSet the timeout, in second\n");
    printf("\t -p, --set-pretimeout\tSet the pretimeout, in second\n");
    printf("\t -c, --set-clear\tSet the clear threshold, in second\n");
    printf("\t -w, --write-protect\tSet the write protection level, 0 ~ 3\n");
    printf("\t -g, --get-timeout\tGet the current timeout, in second\n");
    printf("\t -k, --keepalive\tKeep feeding the watchdog\n");
    printf("\t -h, --help\n");
    printf("\n");
}

int test_wdt(int argc, char **argv)
{
    int opt, ret = RT_EOK;
    int timeout = 0, pretimeout = 0, clear = 0, protect = 0;
    rt_device_t wdt_dev = RT_NULL;
    rt_thread_t wdt_thread = RT_NULL;
    bool keepalive = false;

    if (argc < 2) {
        usage(argv[0]);
        return -RT_ERROR;
    }

    wdt_dev = rt_device_find(AIC_WDT_DEV_NAME);
    if (!wdt_dev) {
        printf("Failed to find %s device\n", AIC_WDT_DEV_NAME);
        return -RT_ERROR;
    }

    if (rt_device_init(wdt_dev) != RT_EOK) {
        printf("Failed to init %s device\n", AIC_WDT_DEV_NAME);
        return -RT_ERROR;
    }

    optind = 0;
    while ((opt = getopt(argc, argv, "s:p:c:w:gkh")) != -1) {
        switch (opt) {
        case 'c':
            clear = strtoul(optarg, NULL, 10);
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_SET_CLR_THD, &clear);
            if (ret != RT_EOK) {
                printf("Failed to set clear threshold %d: %d\n", clear, ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }
            printf("set clear threshold: %d\n", clear);
            break;
        case 's':
            timeout = strtoul(optarg, NULL, 10);
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_SET_TIMEOUT, &timeout);
            if (ret != RT_EOK) {
                printf("Failed to set timeout %d: %d\n", timeout, ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }
            rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_STOP, RT_NULL);
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_START, RT_NULL);
            if (ret != RT_EOK) {
                printf("Failed to start the watchdog: %d\n", ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }

            if (g_wdt_sem)
                rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_IRQ_ENABLE, (void *)wdt_irq_cb);

            printf("set timeout: %d\n", timeout);
            break;
        case 'g':
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_GET_TIMEOUT, &timeout);
            printf("timeout: %d\n", timeout);
            return timeout;
        case 'p':
            pretimeout = strtoul(optarg, NULL, 10);
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_SET_IRQ_TIMEOUT, &pretimeout);
            if (ret != RT_EOK) {
                printf("Failed to set pretimeout %d: %d\n", pretimeout, ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }
            g_wdt_sem = aicos_sem_create(0);
            if (!g_wdt_sem) {
                printf("Failed to create sem\n");
                wdt_irq_cleanup(wdt_dev);
                return -RT_ERROR;
            }
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_IRQ_ENABLE,
                                    (void *)wdt_irq_cb);
            if (ret) {
                printf("Failed to enable the pretimeout IRQ: %d\n", ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }
            printf("set pretimeout: %d\n", pretimeout);
            g_wdt_start_ms = aic_get_time_ms();
            break;
        case 'w':
            protect = strtoul(optarg, NULL, 10);
            ret = rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_EN_REG, &protect);
            if (ret != RT_EOK) {
                printf("Failed to set write protection level %d: %d\n", protect, ret);
                wdt_irq_cleanup(wdt_dev);
                return ret;
            }
            printf("set write protection level: %d\n", protect);
            if (protect >= WDT_WRITE_DISABLED_LEVEL)
                printf("Warning: level %d is a one-way door, only a reset releases it\n",
                       protect);
            break;
        case 'k':
            keepalive = true;
            break;
        case 'h':
        default:
            usage(argv[0]);
            break;
        }
    }

    if (keepalive) {
        if (timeout >= 2)
            timeout -= 1;
        else
            timeout = 1;

        g_wdt_feed_timeout = timeout;
        wdt_thread = rt_thread_create("wdt_feed", wdt_feed_thread_entry,
                                      &g_wdt_feed_timeout, 2048, 10, 10);
        if (wdt_thread != RT_NULL) {
            rt_thread_startup(wdt_thread);
            printf("Keep feeding the watchdog %d times, waiting %d sec ...\n",
                   KEEP_FEED_TIMES, (timeout + 1) * KEEP_FEED_TIMES);
            aicos_msleep((timeout + 1) * KEEP_FEED_TIMES * 1000);
        } else {
            printf("wdt thread create fail!\n");
            ret = -RT_ERROR;
        }
    }

    if (g_wdt_sem) {
        printf("Wait for the pretimeout IRQ ...\n");
        ret = aicos_sem_take(g_wdt_sem, (pretimeout + 2) * 1000);
        if (ret < 0) {
            printf("No pretimeout IRQ in %d sec\n", pretimeout + 2);
        } else {
            ret = (rt_uint32_t)((aic_get_time_ms() - g_wdt_start_ms + 300) / 1000);
            printf("Pretimeout IRQ after %d sec\n", ret);
        }

        if (rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_IRQ_DISABLE, RT_NULL))
            ret = -RT_ERROR;
        if (rt_device_control(wdt_dev, RT_DEVICE_CTRL_WDT_STOP, RT_NULL))
            ret = -RT_ERROR;

        aicos_sem_delete(g_wdt_sem);
        g_wdt_sem = NULL;
    }

    return ret;
}
MSH_CMD_EXPORT_ALIAS(test_wdt, test_wdt, Watchdog driver test);
