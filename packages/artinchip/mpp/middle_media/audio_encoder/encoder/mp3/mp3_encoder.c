/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Author: <che.jiang@artinchip.com>
 * Desc: mp3_encoder (based on tinyMP3/shine encoder)
 */
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <inttypes.h>

#include "mpp_dec_type.h"
#include "mpp_mem.h"
#include "mpp_list.h"
#include "mpp_log.h"

#include "audio_encoder.h"
#include "shine_mp3.h"

//576 or 1152 is mp3 integer mult frame size, with higher efficiency
#define PCM_SAMPLES_PER_CHANNEL 1152

struct mp3_audio_encoder {
    struct aic_audio_encoder encoder;
    int bitrate;
    int channels;
    int sample_rate;
    int bits_per_sample;
    int packet_buffer_size;
    shine_t shine;
};

int __mp3_encode_init(struct aic_audio_encoder *encoder, struct aic_audio_encode_config *config)
{
    struct mp3_audio_encoder *mp3_encoder = (struct mp3_audio_encoder *)encoder;
    struct audio_enc_frame_manager_cfg frm_cfg = { 0 };
    struct aic_audio_encode_config dec_cfg = { 0 };
    shine_config_t shine_config;
    shine_t shine = NULL;
    int max_frame_size;

    if (NULL == mp3_encoder || NULL == config) {
        loge("mp3_encoder or config is null.");
        return -1;
    }

    shine_set_config_mpeg_defaults(&shine_config.mpeg);

    shine_config.wave.channels = (config->channels == 2) ? PCM_STEREO : PCM_MONO;
    shine_config.wave.samplerate = config->sample_rate;

    mp3_encoder->channels = config->channels;
    mp3_encoder->sample_rate = config->sample_rate;
    mp3_encoder->bits_per_sample = config->bits_per_sample;
    mp3_encoder->bitrate = 128;

    shine_config.mpeg.bitr = mp3_encoder->bitrate;
    if (config->sample_rate >= 44100) {
        /* MPEG-I: 44100/48000 */
        mp3_encoder->bitrate = 192;
        shine_config.mpeg.bitr = 192;
    } else if (config->sample_rate >= 32000) {
        /* MPEG-I: 32000 */
        mp3_encoder->bitrate = 128;
        shine_config.mpeg.bitr = 128;
    } else if (config->sample_rate >= 16000) {
        /* MPEG-II: 22050/24000/16000 */
        mp3_encoder->bitrate = 96;
        shine_config.mpeg.bitr = 96;
    } else {
        /* MPEG-2.5: 11025/12000/8000, 64 kbps is the max valid bitrate */
        mp3_encoder->bitrate = 64;
        shine_config.mpeg.bitr = 64;
    }

    if (config->channels > 1)
        shine_config.mpeg.mode = DUAL_CHANNEL;
    else
        shine_config.mpeg.mode = MONO;

    logd("channel: %d, bitrate: %d, samplerate: %d",
        mp3_encoder->channels, mp3_encoder->bitrate, mp3_encoder->sample_rate);

    if (shine_check_config(shine_config.wave.samplerate, shine_config.mpeg.bitr) < 0) {
        loge("shine_check_config failed: sample_rate %d, bitrate %d.",
              shine_config.wave.samplerate, shine_config.mpeg.bitr);
        goto error;
    }

    shine = shine_initialise(&shine_config);
    if (NULL == shine) {
        loge("shine_initialise failed.");
        goto error;
    }

    max_frame_size = PCM_SAMPLES_PER_CHANNEL * config->channels * sizeof(short) * 1.25;
    if (max_frame_size < config->packet_buffer_size) {
        mp3_encoder->packet_buffer_size = config->packet_buffer_size;
        dec_cfg.packet_buffer_size = config->packet_buffer_size;
    } else {
        mp3_encoder->packet_buffer_size = max_frame_size;
        dec_cfg.packet_buffer_size = max_frame_size;
    }
    dec_cfg.packet_count = config->packet_count + 1;
    dec_cfg.frame_count = config->frame_count;
    mp3_encoder->encoder.pm = audio_enc_pm_create(&dec_cfg);
    if (NULL == mp3_encoder->encoder.pm) {
        loge("audio_enc_pm_create error.");
        goto error;
    }

    frm_cfg.bits_per_sample = config->bits_per_sample;
    frm_cfg.samples_per_frame = shine_samples_per_pass(shine) * config->channels;
    frm_cfg.frame_count = config->frame_count;
    mp3_encoder->encoder.fm = audio_enc_fm_create(&frm_cfg);
    if (NULL == mp3_encoder->encoder.fm) {
        loge("audio_enc_fm_create fail.\n");
        goto error;
    }

    mp3_encoder->shine = shine;
    return 0;

error:
    if (mp3_encoder->encoder.fm) {
        audio_enc_fm_destroy(mp3_encoder->encoder.fm);
        mp3_encoder->encoder.fm = NULL;
    }
    if (mp3_encoder->encoder.pm) {
        audio_enc_pm_destroy(mp3_encoder->encoder.pm);
        mp3_encoder->encoder.pm = NULL;
    }

    if (shine)
        shine_close(shine);

    return -1;
}

int __mp3_encode_destroy(struct aic_audio_encoder *encoder)
{
    struct mp3_audio_encoder *mp3_encoder = (struct mp3_audio_encoder *)encoder;

    if (NULL == mp3_encoder) {
        loge("mp3_encoder is null.");
        return -1;
    }
    if (mp3_encoder->encoder.fm) {
        audio_enc_fm_destroy(mp3_encoder->encoder.fm);
        mp3_encoder->encoder.fm = NULL;
    }
    if (mp3_encoder->encoder.pm) {
        audio_enc_pm_destroy(mp3_encoder->encoder.pm);
        mp3_encoder->encoder.pm = NULL;
    }
    if (mp3_encoder->shine) {
        shine_close(mp3_encoder->shine);
        mp3_encoder->shine = NULL;
    }
    mpp_free(mp3_encoder);
    mp3_encoder = NULL;
    return 0;
}

int __mp3_encode_frame(struct aic_audio_encoder *encoder)
{
    struct mp3_audio_encoder *mp3_encoder = (struct mp3_audio_encoder *)encoder;
    struct mpp_packet packet = { 0 };
    struct aic_enc_audio_frame frame;
    unsigned char *mp3_data;
    int imp3_bytes;
    int samples_per_pass;

    if (NULL == mp3_encoder) {
        loge("mp3_encoder is null.");
        return ENC_ERR_NULL_PTR;
    }
    if (0 == mp3_encoder->channels || mp3_encoder->bits_per_sample < 8) {
        loge("mp3_encoder wrong param: channels %d, bits_per_sample %d.", mp3_encoder->channels,
             mp3_encoder->bits_per_sample);
        return ENC_ERR_NOT_SUPPORT;
    }

    if (0 == audio_enc_fm_get_ready_frame_num(mp3_encoder->encoder.fm))
        return ENC_NO_READY_FRAME;

    if (0 == audio_enc_pm_get_empty_packet_num(mp3_encoder->encoder.pm))
        return ENC_NO_EMPTY_PACKET;

    if (0 != audio_enc_fm_dequeue_ready_frame(mp3_encoder->encoder.fm, &frame)) {
        loge("mp3 encoder get pcm buf fail!");
        return ENC_NO_READY_FRAME;
    }

    /* one frame holds exactly shine_samples_per_pass() samples per channel */
    samples_per_pass = shine_samples_per_pass(mp3_encoder->shine);
    if (frame.size !=
        samples_per_pass * mp3_encoder->channels * (mp3_encoder->bits_per_sample / 8)) {
        loge("mp3 encoder wrong frame size %d, expect %d!", frame.size,
             samples_per_pass * mp3_encoder->channels * (mp3_encoder->bits_per_sample / 8));
        audio_enc_fm_enqueue_empty_frame(mp3_encoder->encoder.fm, &frame);
        return ENC_ERR_INTERNAL;
    }

    /* shine supports mono data with the interleaved interface as well */
    mp3_data =
        shine_encode_buffer_interleaved(mp3_encoder->shine, (short *)frame.data, &imp3_bytes);

    audio_enc_fm_enqueue_empty_frame(mp3_encoder->encoder.fm, &frame);
    if (imp3_bytes < 0 || imp3_bytes > mp3_encoder->packet_buffer_size) {
        loge("mp3 encoder fail: imp3_bytes %d, buffer_size %d!", imp3_bytes,
             mp3_encoder->packet_buffer_size);
        return ENC_ERR_INTERNAL;
    } else if (imp3_bytes == 0) {
        logi("mp3 encoder next frame!");
        return ENC_OK;
    }

    logd("frame_size %d, imp3_bytes %d!", frame.size, imp3_bytes);

    if (0 != audio_enc_pm_dequeue_empty_packet(mp3_encoder->encoder.pm, &packet, imp3_bytes)) {
        loge("mp3 encoder get empty buf fail: imp3_bytes %d!", imp3_bytes);
        return ENC_NO_EMPTY_PACKET;
    }
    packet.flag &= ~FRAME_FLAG_EOS;
    packet.pts = frame.pts; /* forward capture timestamp to muxer */
    memcpy(packet.data, mp3_data, imp3_bytes);
    audio_enc_pm_enqueue_ready_packet(mp3_encoder->encoder.pm, &packet);

    // process end frame encode
    if (frame.flag & FRAME_FLAG_EOS) {
        mp3_data = shine_flush(mp3_encoder->shine, &imp3_bytes);

        logd("process the end frame, shine_flush return %d bytes.", imp3_bytes);
        if (imp3_bytes > 0) {
            if (0 !=
                audio_enc_pm_dequeue_empty_packet(mp3_encoder->encoder.pm, &packet, imp3_bytes)) {
                loge("mp3 encoder get empty buf fail: imp3_bytes %d!", imp3_bytes);
                return ENC_NO_EMPTY_PACKET;
            }
            packet.pts = frame.pts; /* same timestamp as the main frame */
            memcpy(packet.data, mp3_data, imp3_bytes);
            packet.size = imp3_bytes;
            packet.flag |= FRAME_FLAG_EOS;
            audio_enc_pm_enqueue_ready_packet(mp3_encoder->encoder.pm, &packet);
        }
    }
    return ENC_OK;
}

int __mp3_encode_control(struct aic_audio_encoder *encoder, int cmd, void *param)
{
    struct mp3_audio_encoder *mp3_encoder = (struct mp3_audio_encoder *)encoder;

    if ((NULL == encoder) || (cmd < MPP_ENCODER_CMD) || (NULL == param)) {
        loge("invalid parameter [%p/%d/%p]", encoder, cmd, param);
        return -1;
    }

    switch (cmd) {
        case MPP_ENCODER_GET_SAMPLE_NUM:
            *(int *)param = shine_samples_per_pass(mp3_encoder->shine);
            break;
        default:
            loge("unsupport cmd:%d", cmd);
            return -1;
    }

    return 0;
}

int __mp3_encode_reset(struct aic_audio_encoder *encoder)
{
    return 0;
}

struct aic_audio_encoder_ops mp3_encoder = {
    .name = "mp3",
    .init = __mp3_encode_init,
    .destroy = __mp3_encode_destroy,
    .encode = __mp3_encode_frame,
    .control = __mp3_encode_control,
    .reset = __mp3_encode_reset,
};

struct aic_audio_encoder *create_mp3_encoder()
{
    struct mp3_audio_encoder *s =
        (struct mp3_audio_encoder *)mpp_alloc(sizeof(struct mp3_audio_encoder));
    if (s == NULL) {
        loge("mpp_alloc error!!!!\n");
        return NULL;
    }
    memset(s, 0, sizeof(struct mp3_audio_encoder));
    s->encoder.ops = &mp3_encoder;

    return &s->encoder;
}
