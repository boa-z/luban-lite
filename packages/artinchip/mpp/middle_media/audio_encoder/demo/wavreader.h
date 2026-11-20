/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <xiaodong.zhao@artinchip.com>
 *  Desc: file wav read
 */

#ifndef WAVREADER_H
#define WAVREADER_H

#ifdef __cplusplus
extern "C" {
#endif

void *wav_read_open(const char *filename);
void wav_read_close(void *obj);

int wav_get_header(void *obj, int *format, int *channels, int *sample_rate, int *bits_per_sample,
                   unsigned int *data_length);
int wav_read_data(void *obj, unsigned char *data, unsigned int length, int *end_flag);

#ifdef __cplusplus
}
#endif

#endif
