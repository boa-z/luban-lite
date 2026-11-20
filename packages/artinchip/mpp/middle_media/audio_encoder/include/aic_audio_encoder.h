/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <che.jiang@artinchip.com>
 *  Desc: aic_audio_encoder interface
 */

#ifndef _AIC_AUDIO_ENCODER_H_
#define _AIC_AUDIO_ENCODER_H_

#include "mpp_dec_type.h"

#define MPP_ENCODER_CMD            0xff
#define MPP_ENCODER_GET_SAMPLE_NUM (MPP_ENCODER_CMD + 0)

struct aic_enc_audio_frame {
    s32 sample_rate;
    s32 bits_per_sample;
    s32 channels;
    s64 pts;
    s32 id;
    void *data;
    u32 size;
    u32 flag;
};

struct aic_audio_encode_config {
    s32 packet_buffer_size; // bytestream size
    s32 packet_count;       // packet buffer count
    s32 frame_count;        // packet buffer count

    s32 channels; // audio channel number
    s32 sample_rate;
    s32 bits_per_sample;
};

/**
 * aic_audio_encoder_create - create encoder (mp3 ...)
 * @type: encoder type
 */
struct aic_audio_encoder *aic_audio_encoder_create(enum aic_audio_codec_type type);

/**
 * aic_audio_encoder_destroy - destory encoder
 * @encoder: aic_audio_encoder context
 */
void aic_audio_encoder_destroy(struct aic_audio_encoder *encoder);

/**
 * aic_audio_encoder_init - init encoder
 * @encoder: aic_audio_encoder context
 * @config: configuration of encoder
 */
s32 aic_audio_encoder_init(struct aic_audio_encoder *encoder,
                           struct aic_audio_encode_config *config);

/**
 * aic_audio_encoder_encode - encode one packet
 * @encoder: aic_audio_encoder context
 */
s32 aic_audio_encoder_encode(struct aic_audio_encoder *encoder);

/**
 * aic_audio_encoder_get_packet - get an empty packet from encoder
 * @encoder: aic_audio_encoder context
 * @return: the packet from aic_audio_encoder
 */
struct mpp_packet *aic_audio_encoder_get_packet(struct aic_audio_encoder *encoder);

/**
 * aic_audio_encoder_put_packet - put back unused es packet to encoder
 * @encoder: aic_audio_encoder context
 * @packet: unused es packet
 */
s32 aic_audio_encoder_put_packet(struct aic_audio_encoder *encoder, struct mpp_packet *packet);

/**
 * aic_audio_encoder_get_frame - get an empty frame from encoder
 * @encoder: aic_audio_encoder context
 * @return: an empty frame packet
 */
struct aic_enc_audio_frame *aic_audio_encoder_get_frame(struct aic_audio_encoder *encoder);

/**
 * aic_audio_encoder_put_frame - put the pcm frame to encoder
 * @encoder: aic_audio_encoder context
 * @frame: the packet filled by application
 */
s32 aic_audio_encoder_put_frame(struct aic_audio_encoder *encoder,
                                struct aic_enc_audio_frame *frame);

/**
 * aic_audio_encoder_reclaim_frame - reclaim the pcm frame to encoder
 * @encoder: aic_audio_encoder context
 * @frame: the frame get by application
 */
s32 aic_audio_encoder_reclaim_frame(struct aic_audio_encoder *encoder,
                                    struct aic_enc_audio_frame *frame);

/**
 * aic_audio_encoder_control - send a control command (like, set/get parameter) to aic_audio_encoder
 * @encoder: aic_audio_encoder context
 * @cmd: command name see in aic_audio_encoder.h
 * @param: command data
 */
s32 aic_audio_encoder_control(struct aic_audio_encoder *encoder, s32 cmd, void *param);

/**
 * aic_audio_encoder_reset - reset aic_audio_encoder
 * @encoder: aic_audio_encoder context
 */
s32 aic_audio_encoder_reset(struct aic_audio_encoder *encoder);

#endif
