/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <che.jiang@artinchip.com>
 *  Desc: aic_audio_encoder interface
 */

#ifndef _AUDIO_ENCODER_H_
#define _AUDIO_ENCODER_H_

#include "mpp_dec_type.h"
#include "aic_audio_encoder.h"
#include "audio_enc_packet_manager.h"
#include "audio_enc_frame_manager.h"

struct aic_audio_encoder {
    struct aic_audio_encoder_ops *ops;
    struct audio_enc_packet_manager *pm;
    struct audio_enc_frame_manager *fm;
};

struct aic_audio_encoder_ops {
    const char *name;
    s32 (*init)(struct aic_audio_encoder *encoder, struct aic_audio_encode_config *config);
    s32 (*destroy)(struct aic_audio_encoder *encoder);
    s32 (*encode)(struct aic_audio_encoder *encoder);
    s32 (*control)(struct aic_audio_encoder *encoder, int cmd, void *param);
    s32 (*reset)(struct aic_audio_encoder *encoder);
};

#endif
