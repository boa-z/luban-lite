/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <che.jiang@artinchip.com>
 *  Desc: aic_audio_encoder interface
 */

#include "mpp_log.h"
#include "audio_encoder.h"

#ifdef AIC_MPP_MP3_ENC
extern struct aic_audio_encoder *create_mp3_encoder();
#endif

struct aic_audio_encoder *aic_audio_encoder_create(enum aic_audio_codec_type type)
{
#ifdef AIC_MPP_MP3_ENC
    if (type == MPP_CODEC_AUDIO_ENCODER_MP3) {
        return create_mp3_encoder();
    }
#endif

    loge("Not support audio codec type %d", type);
    return NULL;
}

void aic_audio_encoder_destroy(struct aic_audio_encoder *encoder)
{
    if (!encoder || !encoder->ops || !encoder->ops->destroy)
        return;
    encoder->ops->destroy(encoder);
}

s32 aic_audio_encoder_init(struct aic_audio_encoder *encoder,
                           struct aic_audio_encode_config *config)
{
    if (!encoder || !encoder->ops || !encoder->ops->init)
        return -1;
    return encoder->ops->init(encoder, config);
}

s32 aic_audio_encoder_encode(struct aic_audio_encoder *encoder)
{
    if (!encoder || !encoder->ops || !encoder->ops->encode)
        return -1;
    return encoder->ops->encode(encoder);
}

s32 aic_audio_encoder_control(struct aic_audio_encoder *encoder, int cmd, void *param)
{
    if (!encoder)
        return -1;
    switch (cmd) {
        default:
            break;
    }
    if (!encoder->ops || !encoder->ops->encode)
        return -1;
    return encoder->ops->control(encoder, cmd, param);
}

s32 aic_audio_encoder_reset(struct aic_audio_encoder *encoder)
{
    if (!encoder || !encoder->ops || !encoder->ops->reset)
        return -1;
    return encoder->ops->reset(encoder);
}

struct mpp_packet *aic_audio_encoder_get_packet(struct aic_audio_encoder *encoder)
{
    if (encoder == NULL)
        return NULL;

    if (0 >= audio_enc_pm_get_ready_packet_num(encoder->pm)) {
        return NULL;
    }

    return audio_enc_pm_dequeue_ready_packet(encoder->pm);
}

s32 aic_audio_encoder_put_packet(struct aic_audio_encoder *encoder, struct mpp_packet *packet)
{
    if (encoder == NULL || packet == NULL)
        return ENC_ERR_NULL_PTR;

    return audio_enc_pm_enqueue_empty_packet(encoder->pm, packet);
}

struct aic_enc_audio_frame *aic_audio_encoder_get_frame(struct aic_audio_encoder *encoder)
{
    if (encoder == NULL)
        return NULL;

    if (encoder->fm == NULL) {
        return NULL;
    }
    if (audio_enc_fm_get_empty_frame_num(encoder->fm) == 0)
        return NULL;

    return audio_enc_fm_dequeue_empty_frame(encoder->fm);
}

s32 aic_audio_encoder_put_frame(struct aic_audio_encoder *encoder,
                                struct aic_enc_audio_frame *frame)
{
    if (encoder == NULL || frame == NULL)
        return ENC_ERR_NULL_PTR;

    return audio_enc_fm_enqueue_ready_frame(encoder->fm, frame);
}

s32 aic_audio_encoder_reclaim_frame(struct aic_audio_encoder *encoder,
                                    struct aic_enc_audio_frame *frame)
{
    if (encoder == NULL || frame == NULL)
        return ENC_ERR_NULL_PTR;

    return audio_enc_fm_enqueue_empty_frame(encoder->fm, frame);
}
