/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <che.jiang@artinchip.com>
 *  Desc: audio packet manager interface
 */

#ifndef _AUDIO_ENC_PACKET_MANAGER_H_
#define _AUDIO_ENC_PACKET_MANAGER_H_

#include "mpp_dec_type.h"
#include "aic_audio_encoder.h"

struct audio_enc_packet_manager;


/**
 * create packet manager
 */
struct audio_enc_packet_manager *audio_enc_pm_create(struct aic_audio_encode_config *cfg);

/**
 *  destroy packet manager
 */
int audio_enc_pm_destroy(struct audio_enc_packet_manager *pm);

/**
 * get a empty packet for external caller
 */
int audio_enc_pm_dequeue_empty_packet(struct audio_enc_packet_manager *pm,
                                      struct mpp_packet *packet, size_t size);

/**
 * external caller put the packet to ready list, which is ready for decoding
 */
int audio_enc_pm_enqueue_ready_packet(struct audio_enc_packet_manager *pm,
                                      struct mpp_packet *packet);

/**
 * get a ready packet for codec.
 */
struct mpp_packet *audio_enc_pm_dequeue_ready_packet(struct audio_enc_packet_manager *pm);

/**
 * Return the packet to empty list (bypass decode)
 */
int audio_enc_pm_return_empty_packet(struct audio_enc_packet_manager *pm,
                                     struct mpp_packet *packet);

/**
 * codec put the packet to empty list
 */
int audio_enc_pm_enqueue_empty_packet(struct audio_enc_packet_manager *pm,
                                      struct mpp_packet *packet);

/**
 * get the packet number of empty list
 */
int audio_enc_pm_get_empty_packet_num(struct audio_enc_packet_manager *pm);

/**
 * get the packet number of ready list
 */
int audio_enc_pm_get_ready_packet_num(struct audio_enc_packet_manager *pm);

/**
 * reclaim the packet to ready list, if the packet not decoded
 */
int audio_enc_pm_reclaim_ready_packet(struct audio_enc_packet_manager *pm,
                                      struct mpp_packet *packet);

int audio_enc_pm_reset(struct audio_enc_packet_manager *pm);

#endif /* PACKET_MANAGER_H */
