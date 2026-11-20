/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wu Dehuang <dehuang.wu@artinchip.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <console.h>
#include <aic_common.h>
#include <aic_errno.h>
#include <boot_param.h>
#include <mmc.h>
#include <image.h>
#include <boot.h>
#include <aic_utils.h>
#ifdef LPKG_USING_FDTLIB
#include "fitimage.h"
#endif

#ifdef AIC_AB_SYSTEM_INTERFACE
#include <absystem.h>

char target[32] = { 0 };
#endif

/* Same bound as the boot target name buffer in boot_app.c */
#define BOOT_PART_NAME_MAX 32

static struct aic_partition *find_boot_part(struct aic_partition *part,
                                            const char *name)
{
    struct aic_partition *p = part;
    const char *nm = name ? name : "";

    while (p) {
        if (!strncmp(p->name, nm, BOOT_PART_NAME_MAX))
            break;

        p = p->next;
    }

    return p;
}

int do_mmc_boot(int argc, char *argv[])
{
    int ret = 0, mmc_id = 0;
    enum boot_device bd;
    struct aic_sdmc *host = NULL;
    struct aic_partition *part = NULL, *parts = NULL;
#ifdef AIC_AB_SYSTEM_INTERFACE
    const char *boot_part;
#endif
#ifdef LPKG_USING_FDTLIB
    struct spl_load_info info;
    ulong entry_point;
#endif

    bd = aic_get_boot_device();
    if (BD_SDMC0 == bd) {
        mmc_id = 0;
    } else if (BD_SDMC1 == bd) {
        mmc_id = 1;
    }

    ret = mmc_init(mmc_id);
    if (ret) {
        printf("sdmc %d init failed.\n", mmc_id);
        return ret;
    }

    host = find_mmc_dev_by_index(mmc_id);
    if (!host) {
        pr_err("find mmc dev failed.\n");
        return -1;
    }

    parts = mmc_create_gpt_part2(mmc_id);
    if (!parts) {
        pr_err("sdmc %d create gpt part failed.\n", mmc_id);
        goto out;
    }

    part = parts;

#ifdef AIC_AB_SYSTEM_INTERFACE
    /* An explicitly selected partition wins over the A/B choice */
    boot_part = boot_get_target_partition();
    if (boot_target_is_explicit()) {
        part = find_boot_part(part, boot_part);
    } else {
        ret = aic_ota_check();
        if (ret) {
            printf("Aic ota check error.\n");
        }

        ret = aic_get_os_to_startup(target);
        if (ret) {
            printf("Aic get os fail, startup from %s default.\n", boot_part);

            part = find_boot_part(part, boot_part);

        } else {
            part = find_boot_part(part, target);

            printf("Start-up from %s\n", target);
        }
    }
#else
    part = find_boot_part(part, boot_get_target_partition());
#endif

    if (!part) {
        printf("Failed to get application partition.\n");
        goto out;
    }

    /* The image format is detected from its header: AIC image or FIT */
    {
        struct image_header *head = malloc(MMC_BLOCK_SIZE);
        u64 blkstart, blkcnt;
        u32 image_len, entry, checksum, load_addr;
        void *la;

        if (!head) {
            printf("No memory for image header.\n");
            ret = -1;
            goto out;
        }

        blkstart = part->start / MMC_BLOCK_SIZE;
        ret = mmc_bread(host, blkstart, 1, (void *)head);
        if (ret == 0 &&
            image_verify_magic((void *)head, AIC_IMAGE_MAGIC) == 0) {
            image_len = head->image_len;
            entry = head->entry_point;
            checksum = head->checksum;
            load_addr = head->load_address;
            la = (void *)(unsigned long)head->load_address;
            free(head);

            blkcnt = ROUNDUP(image_len, MMC_BLOCK_SIZE) / MMC_BLOCK_SIZE;
            ret = mmc_bread(host, blkstart, blkcnt, la);
            if (ret < 0) {
                printf("Failed to load image.\n");
                ret = -1;
                goto out;
            }

            /*
             * A secure boot image keeps its checksum field as 0, and is
             * authenticated by the signature instead.
             */
            if (checksum && image_verify_checksum((u8 *)la, image_len)) {
                printf("AIC image checksum verify failed !!!\n");
                ret = -1;
                goto out;
            } else {
                printf("AIC image checksum verify ok.\n");
            }

            if ((load_addr + AIC_FW_BLOCK1_SIZ) != entry)
                memcpy((void *)(unsigned long)entry,
                        (void *)(unsigned long)(load_addr + AIC_FW_BLOCK1_SIZ),
                        image_len - AIC_FW_BLOCK1_SIZ);

            boot_app((void *)(unsigned long)entry);
            goto out;
        }
        free(head);
    }

#ifdef LPKG_USING_FDTLIB
    info.dev = (void *)host;
    info.priv = (void *)part;
    info.dev_type = DEVICE_MMC;
    info.bl_len = MMC_BLOCK_SIZE;

    entry_point = 0;
    ret = spl_load_simple_fit(&info, &entry_point);
    if (ret < 0)
        goto out;

    boot_app((void *)entry_point);
#else
    printf("Application header is unknown.\n");
    ret = -1;
#endif

out:
    if (parts)
        mmc_free_partition(parts);
    return ret;
}

CONSOLE_CMD(mmc_boot, do_mmc_boot, "Boot from eMMC/SD.");
