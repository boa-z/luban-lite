/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ARTINCHIP_AIC_MACADDR_H_
#define _ARTINCHIP_AIC_MACADDR_H_


void aicmac_get_macaddr_from_chipid(int port, unsigned char out_addr[6]);
void aicmac_get_btmacaddr_from_chipid(unsigned char out_addr[6]);

#endif
