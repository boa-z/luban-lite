/*
 * Copyright (c) 2023-2026, Artinchip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wu Dehuang <dehuang.wu@artinchip.com>
 */

#include <rtconfig.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <console.h>
#include <aic_common.h>
#include <aic_errno.h>
#include <image.h>
#include <boot.h>
#include "aic_time.h"
#ifdef LPKG_USING_FDTLIB
#include "fitimage.h"
#include "of.h"
#endif

static int do_ram_boot(int argc, char *argv[])
{
    int ret = 0;
    uint8_t *data;
    struct image_header head;
    void *la;
    unsigned long addr;
#ifdef LPKG_USING_FDTLIB
    ulong entry_point;
    struct spl_load_info info;
#endif

    addr = strtoul(argv[1], NULL, 0);
    data = (uint8_t *)addr;
    memcpy(&head, data, sizeof(head));
    ret = image_verify_magic((void *)&head, AIC_IMAGE_MAGIC);
    if (ret) {
#ifdef LPKG_USING_FDTLIB
        /* fitimage format */
        info.dev = NULL;
        info.dev_type = DEVICE_RAM;
        info.bl_len = 1;
        info.priv = (void *)addr;

        entry_point = 0;
        ret = spl_load_simple_fit(&info, &entry_point);
        if (ret < 0)
            return ret;

        boot_app((void *)entry_point);
#else
        printf("Application header is unknown.\n");
        return -1;
#endif
    } else {
        /* AIC image format */
        la = (void *)(unsigned long)head.load_address;

        memcpy(la, data, head.image_len);

        /*
         * A secure boot image keeps its checksum field as 0, and is
         * authenticated by the signature instead.
         */
        if (head.checksum && image_verify_checksum((u8 *)la, head.image_len)) {
            printf("AIC image checksum verify failed !!!\n");
            return -1;
        } else {
            printf("AIC image checksum verify ok.\n");
        }

        if ((head.load_address + AIC_FW_BLOCK1_SIZ) != head.entry_point)
            memcpy((void *)(unsigned long)head.entry_point,
                    (void *)(unsigned long)(head.load_address + AIC_FW_BLOCK1_SIZ),
                    head.image_len - AIC_FW_BLOCK1_SIZ);

        boot_app((void *)(unsigned long)head.entry_point);
    }

    return ret;
}

CONSOLE_CMD(ram_boot, do_ram_boot, "Boot from RAM.");
