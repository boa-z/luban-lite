/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wudehuang <dehuang.wu@artinchip.com>
 */

#ifndef __BL_BOOT_H_
#define __BL_BOOT_H_

#include <stdbool.h>
#include <stdint.h>

enum boot_target_type {
    BOOT_TARGET_OS = 0,
    BOOT_TARGET_SPL,
};

/* Set the partition to boot from. type tells whether the partition holds
 * an OS or another bootloader (an SPL). Returns 0, or -1 on a bad name or
 * type. */
int boot_set_target_partition(const char *partition,
                              enum boot_target_type type);

/* Partition selected for the next boot. Defaults to "os". */
const char *boot_get_target_partition(void);

/* Whether the partition was explicitly selected by the caller. An explicit
 * selection wins over the A/B choice done by aic_get_os_to_startup(). */
bool boot_target_is_explicit(void);

void boot_app(void *app);
int do_nor_boot(int argc, char *argv[]);
int do_xip_boot(int argc, char *argv[]);
int do_mmc_boot(int argc, char *argv[]);
int do_nand_boot(int argc, char *argv[]);

#endif /* __BL_BOOT_H_ */
