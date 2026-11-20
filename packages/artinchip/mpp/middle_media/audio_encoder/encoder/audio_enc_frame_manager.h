/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <che.jiang@artinchip.com>
 *  Desc: audio frame manager interface
 */

#ifndef _AUDIO_ENC_FRAME_MANAGER_H_
#define _AUDIO_ENC_FRAME_MANAGER_H_

#include "mpp_dec_type.h"
#include "aic_audio_encoder.h"

struct audio_enc_frame_manager_cfg {
    int frame_count;
    int samples_per_frame; //include all channels
    int bits_per_sample;
};

struct audio_enc_frame_manager;

/**
 * create packet manager
 */
struct audio_enc_frame_manager *audio_enc_fm_create(struct audio_enc_frame_manager_cfg *cfg);

/**
 * destroy frame manager
 */
int audio_enc_fm_destroy(struct audio_enc_frame_manager *fm);

/**
 * get a empty frame for external caller
 */
struct aic_enc_audio_frame *audio_enc_fm_dequeue_empty_frame(struct audio_enc_frame_manager *fm);

/**
 * put the packet to ready list, which is ready for output
 * external put the packet to read list, which is ready for encoder
 */
int audio_enc_fm_enqueue_ready_frame(struct audio_enc_frame_manager *fm,
                                     struct aic_enc_audio_frame *frame);

/**
 * get a ready frame for render or encoder.
 */
int audio_enc_fm_dequeue_ready_frame(struct audio_enc_frame_manager *fm,
                                     struct aic_enc_audio_frame *frame);

/**
 * render put the frame to empty list
 */
int audio_enc_fm_enqueue_empty_frame(struct audio_enc_frame_manager *fm,
                                     struct aic_enc_audio_frame *frame);

/**
 * get the frame number of empty list
 */
int audio_enc_fm_get_empty_frame_num(struct audio_enc_frame_manager *fm);

/**
 * get the packet number of ready list
 */
int audio_enc_fm_get_ready_frame_num(struct audio_enc_frame_manager *fm);

/**
 * reset frame manager
 */
int audio_enc_fm_reset(struct audio_enc_frame_manager *fm);

#endif /* PACKET_MANAGER_H */
