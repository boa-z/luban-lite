/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: xuan.wen <xuan.wen@artinchip.com>
 */

#include "inc/spinand.h"
#include "inc/manufacturer.h"

#define SPINAND_MFR_ZETTA		0xBA

/*
 * Note: ZD35Q1GC-IB is not supported because it does not have
 * enough spare area for user.
 */
const struct aic_spinand_info zetta_spinand_table[] = {
    /*devid page_size oob_size block_per_lun pages_per_eraseblock planes_per_lun
    is_die_select*/
};

const struct aic_spinand_info *zetta_spinand_detect(struct aic_spinand *flash)
{
    u8 *id = flash->id.data;

    if (id[0] != SPINAND_MFR_ZETTA)
        return NULL;

    return spinand_match_and_init(&id[1], zetta_spinand_table,
                                  ARRAY_SIZE(zetta_spinand_table));
};

static int zetta_spinand_init(struct aic_spinand *flash)
{
    return 0;
};

static const struct spinand_manufacturer_ops zetta_spinand_manuf_ops = {
    .detect = zetta_spinand_detect,
    .init = zetta_spinand_init,
};

const struct spinand_manufacturer zetta_spinand_manufacturer = {
    .id = SPINAND_MFR_ZETTA,
    .name = "zetta",
    .ops = &zetta_spinand_manuf_ops,
};
