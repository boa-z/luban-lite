/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _AIC_DRV_DCE_H_
#define _AIC_DRV_DCE_H_

#include "aic_core.h"

#define DCE_DEV_NAME        "dce"

/* the argument of DCE_CMD_CHECKSUM / DCE_CMD_CRC32 */
struct dce_buf {
    unsigned int addr;          /* the address of the data */
    unsigned int len;           /* the length of the data, in byte */
    unsigned int result;        /* the result of the calculation */
};

enum {
    DCE_CMD_GET_VERSION,
    DCE_CMD_CHECKSUM,
    DCE_CMD_CRC32,
};

/*
 * The DCE registers and the module clock are shared by all the users, the
 * driver serializes them, so every user has to call the driver instead of
 * the registers directly.
 *
 * The config registers of the crc32 are not exposed: the driver leaves them
 * at their default value for every calculation, so that a crc only depends
 * on the data and not on the module which ran a calculation before.
 */
int drv_dce_get_version(unsigned int *version);
int drv_dce_checksum(unsigned int addr, unsigned int len, unsigned int *result);
int drv_dce_crc32(unsigned int addr, unsigned int len, unsigned int *result);

#endif /* _AIC_DRV_DCE_H_ */
