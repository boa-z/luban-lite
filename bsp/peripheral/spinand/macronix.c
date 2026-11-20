/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: xuan.wen <xuan.wen@artinchip.com>
 */

#include "inc/spinand.h"
#include "inc/manufacturer.h"

#define SPINAND_MFR_MACRONIX 0xC2

/*
 * Note: MX35LF2G14AC-Z4I and MX35LF1G24AD-Z4I are not supported
 * because they lack internal ECC.
 */
const struct aic_spinand_info macronix_spinand_table[] = {
    /*devid page_size oob_size block_per_lun pages_per_eraseblock planes_per_lun
    is_die_select*/
};

const struct aic_spinand_info *
macronix_spinand_detect(struct aic_spinand *flash)
{
    u8 *id = flash->id.data;

    if (id[0] != SPINAND_MFR_MACRONIX)
        return NULL;

    return spinand_match_and_init(&id[1], macronix_spinand_table,
                                  ARRAY_SIZE(macronix_spinand_table));
};

static int macronix_spinand_init(struct aic_spinand *flash)
{
    return 0;
};

static const struct spinand_manufacturer_ops macronix_spinand_manuf_ops = {
    .detect = macronix_spinand_detect,
    .init = macronix_spinand_init,
};

const struct spinand_manufacturer macronix_spinand_manufacturer = {
    .id = SPINAND_MFR_MACRONIX,
    .name = "macronix",
    .ops = &macronix_spinand_manuf_ops,
};
