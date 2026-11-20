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
#include <spinand_port.h>
#include <mtd.h>
#include <image.h>
#include <boot.h>
#include <aic_utils.h>
#ifdef LPKG_USING_FDTLIB
#include "fitimage.h"
#include "of.h"
#endif

#define PAGE_MAX_SIZE    4096

#ifdef AIC_AB_SYSTEM_INTERFACE
#include <absystem.h>
#endif

int do_nand_boot(int argc, char *argv[])
{
    char target[32] __attribute__((unused)) = { 0 };
    struct image_header *head;
    struct mtd_dev *mtd;
    void *la;
    u32 image_len, entry, read_len, checksum, load_addr;
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
        printf("Start-up from %s\n", boot_part);
    } else {
        ret = aic_ota_check();
        if (ret) {
            printf("Aic ota check error.\n");
        }

        ret = aic_get_os_to_startup(target);
        if (ret) {
            printf("Aic get os fail, startup from %s default.\n", boot_part);
            mtd = mtd_get_device(boot_part);
        } else {
            mtd = mtd_get_device(target);
            printf("Start-up from %s\n", target);
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
    head = malloc(mtd->writesize);
    if (!head) {
        printf("No memory for image header.\n");
        return -1;
    }

    ret = mtd_read(mtd, 0, (void *)head, mtd->writesize);
    if (ret == 0 && image_verify_magic((void *)head, AIC_IMAGE_MAGIC) == 0) {
        image_len = head->image_len;
        entry = head->entry_point;
        checksum = head->checksum;
        load_addr = head->load_address;
        la = (void *)(unsigned long)head->load_address;
        free(head);

        read_len = ROUNDUP(image_len, mtd->writesize);
#ifdef AIC_SPINAND_CONT_READ
        ret = mtd_contread(mtd, 0, la, read_len);
        if (ret < 0)
#endif
            ret = mtd_read(mtd, 0, la, read_len);
        if (ret < 0) {
            printf("Failed to load image.\n");
            return -1;
        }

        /*
         * A secure boot image keeps its checksum field as 0, and is
         * authenticated by the signature instead.
         */
        if (checksum && image_verify_checksum((u8 *)la, image_len)) {
            printf("AIC image checksum verify failed !!!\n");
            return -1;
        } else {
            printf("AIC image checksum verify ok.\n");
        }

        if ((load_addr + AIC_FW_BLOCK1_SIZ) != entry)
            memcpy((void *)(unsigned long)entry,
                    (void *)(unsigned long)(load_addr + AIC_FW_BLOCK1_SIZ),
                    image_len - AIC_FW_BLOCK1_SIZ);

        boot_app((void *)(unsigned long)entry);
        return 0;
    }
    free(head);

#ifdef LPKG_USING_FDTLIB
    info.dev = (void *)mtd;
    info.bl_len = mtd->writesize;
    info.dev_type = DEVICE_SPINAND;

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

CONSOLE_CMD(nand_boot, do_nand_boot, "Boot from NAND.");
