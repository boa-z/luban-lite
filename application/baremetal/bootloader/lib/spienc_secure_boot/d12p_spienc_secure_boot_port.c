/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * D12p platform backend for FIT image decryption.
 *
 * AES-128-CTR is performed by the SPIENC independent mode engine,
 * with the key taken from eFuse, so it never passes through the CPU.
 */

#include <rtconfig.h>
#include <string.h>
#include <aic_common.h>
#include <aic_core.h>
#include <hal_spienc.h>

#define SPIENC_CTR_IV_LEN       16

int fit_image_get_cipher_key(const char *algo, const char *key_name_hint,
                             u8 *key, int key_len)
{
    (void)key_name_hint;

    if (key == NULL)
        return -1;

    if (strncmp(algo, "aes-128-ctr", 11) != 0)
        return -1;

    /* The key stays in eFuse and is read by the SPIENC engine */
    memset(key, 0, key_len);
    return 0;
}

int fit_image_aes_128_ctr_decrypt(const u8 *key, const u8 *iv, u8 *data,
                                  unsigned int length)
{
    u8 aligned_iv[SPIENC_CTR_IV_LEN] __attribute__((aligned(4)));

    (void)key;

    if (iv == NULL || data == NULL || length == 0)
        return -1;

    memcpy(aligned_iv, iv, SPIENC_CTR_IV_LEN);

    hal_spienc_init();
    hal_spienc_stop();
    hal_spienc_set_work_mode(AIC_SPIENC_IND_MODE);
    hal_spienc_select_alg(AIC_SPIENC_AES_128_CTR);
    hal_spienc_select_alg_key(AIC_SPIENC_KEY_SRC_EFUSE);
    hal_spienc_set_alg_iv(aligned_iv);

    aicos_dcache_clean_invalid_range(data, ALIGN_UP(length, 32));
    hal_spienc_start_alg(data, length, data, length);
    aicos_dcache_invalid_range(data, ALIGN_UP(length, 32));

    hal_spienc_stop();
    hal_spienc_set_work_mode(AIC_SPIENC_BUS_MODE);

    return 0;
}
