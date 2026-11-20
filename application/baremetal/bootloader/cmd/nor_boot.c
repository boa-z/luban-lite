/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
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
#include <sfud.h>
#include <mtd.h>
#include <image.h>
#include <boot.h>
#include <aic_utils.h>
#include "aic_time.h"
#ifdef LPKG_USING_FDTLIB
#include "fitimage.h"
#include "of.h"
#endif

#ifdef AIC_AB_SYSTEM_INTERFACE
#include <absystem.h>
#endif

int do_nor_boot(int argc, char *argv[])
{
    char target[32] __attribute__((unused)) = { 0 };
    struct image_header head;
    struct mtd_dev *mtd;
    void *la;
    int ret = 0;
#ifdef AIC_AB_SYSTEM_INTERFACE
    const char *boot_part;
#endif
#ifdef LPKG_USING_FDTLIB
    struct spl_load_info info;
    ulong entry_point;
#endif

    mtd_probe();

#ifdef AIC_AB_SYSTEM_INTERFACE
    /* An explicitly selected partition wins over the A/B choice */
    boot_part = boot_get_target_partition();
    if (boot_target_is_explicit()) {
        mtd = mtd_get_device(boot_part);
    } else {
        ret = aic_ota_check();
        if (ret) {
            printf("Aic ota check error.\n");
        }

        ret = aic_get_os_to_startup(target);
        printf("Start-up from %s\n", target);
        if (ret) {
            printf("Aic get os fail, startup from %s default.\n", boot_part);
            mtd = mtd_get_device(boot_part);
        } else {
            mtd = mtd_get_device(target);
        }
    }
#else
    mtd = mtd_get_device(boot_get_target_partition());
#endif
    if (!mtd) {
        printf("Failed to get application partition.\n");
        return -1;
    }

    /* The image format is detected from its header: AIC image or FIT */
    ret = mtd_read(mtd, 0, (void *)&head, sizeof(head));
    if (ret == 0 && image_verify_magic((void *)&head, AIC_IMAGE_MAGIC) == 0) {
        la = (void *)(unsigned long)head.load_address;
        ret = mtd_read(mtd, 0, la, head.image_len);
        if (ret < 0) {
            printf("Failed to load image.\n");
            return -1;
        }

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
        return 0;
    }

#ifdef LPKG_USING_FDTLIB
    info.dev = (void *)mtd;
    info.bl_len = 1;
    info.dev_type = DEVICE_SPINOR;

    entry_point = 0;
    ret = spl_load_simple_fit(&info, &entry_point);
    if (ret < 0)
        return ret;

    /* Read dtb from config partition */
    of_fdt_dt_init_bare_nornand();

    boot_app((void *)entry_point);
#else
    printf("Application header is unknown.\n");
    ret = -1;
#endif

    return ret;
}

CONSOLE_CMD(nor_boot, do_nor_boot, "Boot from NOR.");
