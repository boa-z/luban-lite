/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wu Dehuang <dehuang.wu@artinchip.com>
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <image.h>
#include <boot.h>
#include <aic_core.h>
#include <aic_time.h>
#include <boot_time.h>
#include <console.h>
#include <boot_param.h>
#include <uart.h>

#define BOOT_TARGET_PARTITION_MAX 32

static char target_partition[BOOT_TARGET_PARTITION_MAX] = "os";
static enum boot_target_type target_type = BOOT_TARGET_OS;
static bool target_explicit;

int boot_set_target_partition(const char *partition,
                              enum boot_target_type type)
{
    size_t len = 0;

    if (!partition || (type != BOOT_TARGET_OS && type != BOOT_TARGET_SPL))
        return -1;

    while (len < sizeof(target_partition) && partition[len] != '\0')
        len++;

    if (len == 0 || len >= sizeof(target_partition))
        return -1;

    memcpy(target_partition, partition, len + 1);
    target_type = type;
    target_explicit = true;
    return 0;
}

const char *boot_get_target_partition(void)
{
    return target_partition;
}

bool boot_target_is_explicit(void)
{
    return target_explicit;
}

/*
 * Start the application: the caller always passes the entry point of
 * the image it has loaded, no matter whether it was a FIT or an AIC
 * format image.
 */
void boot_app(void *app)
{
    void (*ep)(int, unsigned long);
    enum boot_device dev;
    unsigned long boot_arg;

    ep = app;
    if (!ep) {
        printf("Entry point is null, Run APP failure.\n");
        return;
    }
    boot_time_trace("Run APP");
    boot_time_show();
    dev = aic_get_boot_device();

    /* An OS and an SPL expect different things in a1, see boot.h */
    if (target_type == BOOT_TARGET_OS)
        boot_arg = (unsigned long)aic_get_boot_args();
    else
        boot_arg = (unsigned long)aic_get_boot_resource();

    aicos_dcache_clean();
    aicos_icache_invalid();
    /* Drain pending console output before interrupts are disabled */
    uart_tx_flush(AIC_BOOTLOADER_CONSOLE_UART);
    aicos_local_irq_disable();

    ep(dev, boot_arg);
}
