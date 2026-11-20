/*
 * Copyright (C) 2020-2026 ArtInChip Technology Co. Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 *  author: <jianye.liang@artinchip.com>
 *  Desc: video stream decode demo (containerized stream)
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <inttypes.h>
#include "aic_osal.h"
#include "aic_core.h"
#include "mpp_fb.h"
#include "mpp_mem.h"
#include "mpp_decoder.h"
#include "mpp_log.h"
#include "mpp_ge.h"

#define VID_PATH_MAX_LEN 256
#define VID_STREAM_CHUNK_SIZE (256 * 1024)
#define VID_STREAM_DETECT_SIZE (256 * 1024)

/* ---------- dump ---------- */

#define VID_DUMP_SUFFIX ".yuv"

struct vid_dump_ctx {
    s32 enabled;
    char dir[VID_PATH_MAX_LEN];
    char base[VID_PATH_MAX_LEN];
    char path[VID_PATH_MAX_LEN];
    FILE *fp;
    u64 tm_start;
    u32 frame_num;
    u32 width;
    u32 height;
    enum mpp_pixel_format format;
    unsigned int stride[3];
};

static struct vid_dump_ctx g_vid_dump = {0};

static const char *vid_dump_pix_fmt_name(enum mpp_pixel_format fmt)
{
    switch (fmt) {
    case MPP_FMT_YUV420P:   return "yuv420p";
    case MPP_FMT_NV12:      return "nv12";
    case MPP_FMT_NV21:      return "nv21";
    case MPP_FMT_YUV422P:   return "yuv422p";
    case MPP_FMT_NV16:      return "nv16";
    case MPP_FMT_NV61:      return "nv61";
    case MPP_FMT_YUV400:    return "gray";
    case MPP_FMT_YUV444P:   return "yuv444p";
    /* MPP_FMT names the 32-bit word, ffmpeg names the memory byte order,
       so the two nomenclatures are permuted on little-endian */
    case MPP_FMT_ARGB_8888: return "bgra";   /* word 0xAARRGGBB */
    case MPP_FMT_ABGR_8888: return "rgba";   /* word 0xAABBGGRR */
    case MPP_FMT_RGBA_8888: return "abgr";   /* word 0xRRGGBBAA */
    case MPP_FMT_BGRA_8888: return "argb";   /* word 0xBBGGRRAA */
    case MPP_FMT_RGB_888:   return "rgb24";
    case MPP_FMT_BGR_888:   return "bgr24";
    case MPP_FMT_RGB_565:   return "rgb565";
    case MPP_FMT_BGR_565:   return "bgr565";
    default:                return "unknown";
    }
}

struct vid_dump_plane {
    int comp_num;
    int div[3];    /* width divider of each plane */
    int vdiv[3];   /* height divider of each plane */
    int bytes[3];  /* bytes per pixel in a row of each plane */
};

static int vid_dump_get_plane_info(enum mpp_pixel_format fmt,
                                   struct vid_dump_plane *p)
{
    memset(p, 0, sizeof(*p));
    /* default: no sub-sampling, 1 byte per pixel */
    p->div[0] = p->div[1] = p->div[2] = 1;
    p->vdiv[0] = p->vdiv[1] = p->vdiv[2] = 1;
    p->bytes[0] = p->bytes[1] = p->bytes[2] = 1;

    switch (fmt) {
    case MPP_FMT_YUV420P:
        p->comp_num = 3;
        p->div[1] = p->div[2] = 2;
        p->vdiv[1] = p->vdiv[2] = 2;
        break;
    case MPP_FMT_YUV422P:
        p->comp_num = 3;
        p->div[1] = p->div[2] = 2;
        break;
    case MPP_FMT_YUV444P:
        p->comp_num = 3;
        break;
    case MPP_FMT_NV12:
    case MPP_FMT_NV21:
        p->comp_num = 2;
        p->div[1] = 2;
        p->vdiv[1] = 2;
        p->bytes[1] = 2;
        break;
    case MPP_FMT_NV16:
    case MPP_FMT_NV61:
        p->comp_num = 2;
        p->div[1] = 2;
        p->bytes[1] = 2;
        break;
    case MPP_FMT_YUV400:
        p->comp_num = 1;
        break;
    case MPP_FMT_ARGB_8888:
    case MPP_FMT_ABGR_8888:
    case MPP_FMT_RGBA_8888:
    case MPP_FMT_BGRA_8888:
        p->comp_num = 1;
        p->bytes[0] = 4;
        break;
    case MPP_FMT_RGB_888:
    case MPP_FMT_BGR_888:
        p->comp_num = 1;
        p->bytes[0] = 3;
        break;
    case MPP_FMT_RGB_565:
    case MPP_FMT_BGR_565:
        p->comp_num = 1;
        p->bytes[0] = 2;
        break;
    default:
        loge("dump: unsupported pixel format %d", fmt);
        return -1;
    }
    return 0;
}

/* Write the real picture region of one decoded frame as a tight WxH image.
   All frames are concatenated into one raw yuv file. */
static int vid_dump_frame(struct mpp_frame *frame)
{
    struct vid_dump_ctx *dump = &g_vid_dump;
    struct vid_dump_plane p;
    u8 *base;
    int plane, row, w, h, pw, ph, row_bytes;
    int x, y;

    if (!dump->enabled || !dump->fp)
        return 0;

    if (vid_dump_get_plane_info(frame->buf.format, &p) < 0)
        return -1;

    if (frame->buf.size.width == 0 || frame->buf.size.height == 0) {
        loge("dump: invalid frame size %ux%u",
             frame->buf.size.width, frame->buf.size.height);
        return -1;
    }

    if (frame->buf.crop_en) {
        w = frame->buf.crop.width;
        h = frame->buf.crop.height;
    } else {
        w = frame->buf.size.width;
        h = frame->buf.size.height;
    }

    for (plane = 0; plane < p.comp_num; plane++) {
        x = frame->buf.crop_en ? frame->buf.crop.x / p.div[plane] : 0;
        y = frame->buf.crop_en ? frame->buf.crop.y / p.vdiv[plane] : 0;
        pw = w / p.div[plane];
        ph = h / p.vdiv[plane];
        row_bytes = pw * p.bytes[plane];
        if (pw <= 0 || ph <= 0 || row_bytes <= 0) {
            loge("dump: invalid plane %d: %dx%d row_bytes %d",
                 plane, pw, ph, row_bytes);
            return -1;
        }

        if (!frame->buf.phy_addr[plane]) {
            loge("dump: plane %d has no buffer", plane);
            return -1;
        }

        base = (u8 *)(unsigned long)frame->buf.phy_addr[plane];
        if (frame->buf.stride[plane])
            base += y * frame->buf.stride[plane] + x * p.bytes[plane];

        /* Frame is written by VE via DMA, invalidate dcache before CPU read */
        if (frame->buf.stride[plane]) {
            aicos_dcache_invalid_range(base,
                                       (ph - 1) * frame->buf.stride[plane] + row_bytes);
        }

        if (x == 0 && (int)frame->buf.stride[plane] == row_bytes) {
            /* Contiguous plane: one big write is much faster on fatfs */
            if (fwrite(base, 1, (size_t)ph * row_bytes,
                       dump->fp) != (size_t)ph * row_bytes) {
                loge("dump: write %s failed", dump->path);
                return -1;
            }
        } else {
            for (row = 0; row < ph; row++) {
                if (fwrite(base + row * frame->buf.stride[plane], 1, row_bytes,
                           dump->fp) != (size_t)row_bytes) {
                    loge("dump: write %s failed", dump->path);
                    return -1;
                }
            }
        }
    }

    if (dump->frame_num == 0) {
        dump->width = w;
        dump->height = h;
        dump->format = frame->buf.format;
        dump->stride[0] = frame->buf.stride[0];
        dump->stride[1] = frame->buf.stride[1];
        dump->stride[2] = frame->buf.stride[2];
        logi("dump: first frame %ux%u format %s stride %u %u %u",
             w, h, vid_dump_pix_fmt_name(dump->format),
             dump->stride[0], dump->stride[1], dump->stride[2]);
    } else if ((dump->frame_num % 20) == 0) {
        logi("dump: %u frames", dump->frame_num);
    }
    dump->frame_num++;
    return 0;
}

/* Dump file is "<dir>/<input basename>.yuv" */
static int vid_dump_open(char *dir, char *input_path)
{
    struct vid_dump_ctx *dump = &g_vid_dump;
    char *name, *dot;

    memset(dump, 0, sizeof(*dump));
    dump->enabled = 1;
    snprintf(dump->dir, sizeof(dump->dir), "%s", dir);

    name = strrchr(input_path, '/');
    name = name ? name + 1 : input_path;
    snprintf(dump->base, sizeof(dump->base), "%s", name);

    dot = strrchr(dump->base, '.');
    if (dot)
        *dot = '\0';

    snprintf(dump->path, sizeof(dump->path), "%.63s/%.63s%s",
             dir, dump->base, VID_DUMP_SUFFIX);
    dump->fp = fopen(dump->path, "wb");
    if (!dump->fp) {
        loge("dump: open %s failed", dump->path);
        dump->enabled = 0;
        return -1;
    }

    /* Bigger stdio buffer: libc default is 1KB, which costs ~900 DFS/fatfs
       writes per MB-scale frame on slow media; 64KB cuts it to ~15 */
    setvbuf(dump->fp, NULL, _IOFBF, 64 * 1024);
    dump->tm_start = aic_get_time_ms();

    printf("dump decoded frame to %s\n", dump->path);
    return 0;
}

static int vid_dump_close(void)
{
    struct vid_dump_ctx *dump = &g_vid_dump;

    if (!dump->enabled)
        return 0;

    if (dump->fp) {
        fclose(dump->fp);
        dump->fp = NULL;
    }
    dump->enabled = 0;

    if (dump->frame_num == 0) {
        printf("dump: no frame dumped to %s\n", dump->path);
        return 0;
    }

    printf("dump: %u frames -> %s (%llu ms)\n", dump->frame_num, dump->path,
           (unsigned long long)(aic_get_time_ms() - dump->tm_start));
    printf("ffmpeg: ffplay -f rawvideo -pixel_format %s -video_size %ux%u"
           " -i %s\n",
           vid_dump_pix_fmt_name(dump->format), dump->width, dump->height,
           dump->path);
    return 0;
}

/* ---------- render ---------- */

static struct aicfb_screeninfo g_screen_info = {0};

static struct aicfb_screeninfo *get_screen_info(void)
{
    struct mpp_fb *fb = NULL;
    int ret;

    if (g_screen_info.width)
        return &g_screen_info;

    fb = mpp_fb_open();
    if (!fb)
        return NULL;

    ret = mpp_fb_ioctl(fb, AICFB_GET_SCREENINFO, &g_screen_info);
    if (ret) {
        loge("get screen info failed\n");
        mpp_fb_close(fb);
        return NULL;
    }

    mpp_fb_close(fb);
    return &g_screen_info;
}

static int ge_bitblt(struct ge_bitblt *blt)
{
    int ret = 0;
    struct mpp_ge *ge = mpp_ge_open();

    if (!ge) {
        loge("open ge device error\n");
        return -1;
    }

    ret = mpp_ge_bitblt(ge, blt);
    if (ret < 0) {
        loge("bitblt task failed\n");
        goto out;
    }

    ret = mpp_ge_emit(ge);
    if (ret < 0) {
        loge("emit task failed\n");
        goto out;
    }

    ret = mpp_ge_sync(ge);
    if (ret < 0) {
        loge("ge sync fail\n");
        goto out;
    }

out:
    mpp_ge_close(ge);
    return ret;
}

static void render_frame(struct mpp_fb *fb, struct mpp_frame *frame)
{
    struct ge_bitblt blt;
    struct aicfb_screeninfo *info = get_screen_info();
    struct aicfb_layer_data layer = {0};

    if (info == NULL)
        return;

    layer.layer_id = AICFB_LAYER_TYPE_UI;
    layer.rect_id = 0;
    if (mpp_fb_ioctl(fb, AICFB_GET_LAYER_CONFIG, &layer) < 0) {
        loge("get ui layer config failed\n");
        return;
    }

    memset(&blt, 0, sizeof(struct ge_bitblt));
    memcpy(&blt.src_buf, &frame->buf, sizeof(struct mpp_buf));

    blt.dst_buf.buf_type = MPP_PHY_ADDR;
    blt.dst_buf.phy_addr[0] = layer.buf.phy_addr[0];
    blt.dst_buf.format = info->format;
    blt.dst_buf.stride[0] = info->stride;
    blt.dst_buf.size.width = info->width;
    blt.dst_buf.size.height = info->height;
    blt.dst_buf.crop_en = 1;
    blt.dst_buf.crop.x = 0;
    blt.dst_buf.crop.y = 0;
    blt.dst_buf.crop.width = blt.src_buf.crop.width;
    blt.dst_buf.crop.height = blt.src_buf.crop.height;

    ge_bitblt(&blt);
}

/* Dump the frame if dump mode is enabled, otherwise render to fb */
static void vid_output_frame(struct mpp_fb *fb, struct mpp_frame *frame)
{
    if (g_vid_dump.enabled)
        vid_dump_frame(frame);
    else
        render_frame(fb, frame);
}

/* ---------- stream detect ---------- */

enum vid_stream_type {
    VID_STREAM_UNKNOWN = 0,
    VID_STREAM_AICP,
    VID_STREAM_H264,   /* reserved for future extension */
};

/* AICP frame starts with "AICP" magic followed by JPEG SOI */
static int find_aicp_frame(const uint8_t *buf, int len, int *pos)
{
    int i;

    for (i = 0; i < len - 5; i++) {
        if (buf[i] == 'A' && buf[i + 1] == 'I' && buf[i + 2] == 'C' &&
            buf[i + 3] == 'P' && buf[i + 4] == 0xFF && buf[i + 5] == 0xD8) {
            *pos = i;
            return 0;
        }
    }
    return -1;
}

/* Sniff the media data in a container (e.g. mp4) to detect the codec.
   AICP frames are self-contained and start with "AICP"+SOI, so scanning
   the head of the file is enough to find the first frame. */
static int vid_detect_stream_type(char *path)
{
    uint8_t *buf;
    int fd, size;
    int type = VID_STREAM_UNKNOWN;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        loge("open file(%s) failed, %d", path, fd);
        return VID_STREAM_UNKNOWN;
    }

    buf = (uint8_t *)mpp_alloc(VID_STREAM_DETECT_SIZE);
    if (!buf) {
        loge("Failed to allocate detect buffer");
        close(fd);
        return VID_STREAM_UNKNOWN;
    }

    size = read(fd, buf, VID_STREAM_DETECT_SIZE);
    if (size > 0) {
        int pos = -1;

        if (find_aicp_frame(buf, size, &pos) == 0) {
            type = VID_STREAM_AICP;
            logi("detect AICP stream at offset %d", pos);
        }
        /* TODO: detect H264 by annex-b start code here */
    }

    mpp_free(buf);
    close(fd);
    return type;
}

/* ---------- decode ---------- */

/* Create a decoder and init it with the common stream config */
static struct mpp_decoder *vid_decoder_create(enum mpp_codec_type type,
                                              int pix_fmt, int bitstream_size,
                                              int rot_flip_flag, int ver_scale,
                                              int hor_scale)
{
    struct mpp_decoder *dec;
    struct decode_config config;

    dec = mpp_decoder_create(type);
    if (!dec) {
        loge("create decoder fail");
        return NULL;
    }

    config.bitstream_buffer_size = bitstream_size;
    config.extra_frame_num = 0;
    config.packet_count = 1;
    config.pix_fmt = pix_fmt;

    if (rot_flip_flag)
        mpp_decoder_control(dec, MPP_DEC_INIT_CMD_SET_ROT_FLIP_FLAG, &rot_flip_flag);

    if (ver_scale || hor_scale) {
        struct mpp_scale_ratio scale;
        scale.hor_scale = hor_scale;
        scale.ver_scale = ver_scale;
        mpp_decoder_control(dec, MPP_DEC_INIT_CMD_SET_SCALE, &scale);
    }

    mpp_decoder_init(dec, &config);
    return dec;
}

static int vid_decode_aicp_one_frame(struct mpp_decoder *dec, struct mpp_fb *fb,
                                     uint8_t *data, int size, int is_last)
{
    struct mpp_packet pkt;
    struct mpp_frame frm;

    memset(&pkt, 0, sizeof(struct mpp_packet));
    if (mpp_decoder_get_packet(dec, &pkt, size) != 0) {
        loge("Get AICP packet failed, size %d", size);
        return -1;
    }
    memcpy(pkt.data, data, size);
    pkt.size = size;
    pkt.flag = is_last ? PACKET_FLAG_EOS : 0;
    mpp_decoder_put_packet(dec, &pkt);

    if (mpp_decoder_decode(dec) < 0) {
        loge("Decode AICP frame failed, size %d", size);
        return -1;
    }

    memset(&frm, 0, sizeof(struct mpp_frame));
    if (mpp_decoder_get_frame(dec, &frm) != 0) {
        loge("Get AICP frame failed");
        return -1;
    }

    vid_output_frame(fb, &frm);
    if (!g_vid_dump.enabled)
        aicos_msleep(33);

    mpp_decoder_put_frame(dec, &frm);
    return 0;
}

/* Decode an AICP video stream. Frames are located by the "AICP"+SOI magic,
   so the mp4 container (mdat holds contiguous AICP samples) is supported
   without parsing the moov sample table. */
static int vid_decode_aicp_stream(struct mpp_fb *fb, char *dump_dir, char *path,
                                  int out_format, int rot_flip_flag,
                                  int ver_scale, int hor_scale)
{
    struct mpp_decoder *dec = NULL;
    int input_fd = -1;
    int r_len = 0;
    int ret = -1;
    uint8_t *work_buf = NULL;
    int total = 0;
    int frame_count = 0;

    input_fd = open(path, O_RDONLY);
    if (input_fd < 0) {
        loge("open file(%s) failed, %d", path, input_fd);
        return -1;
    }
    logi("Read AICP stream from %s", path);

    dec = vid_decoder_create(MPP_CODEC_VIDEO_DECODER_AICP, out_format,
                             VID_STREAM_CHUNK_SIZE, rot_flip_flag,
                             ver_scale, hor_scale);
    if (!dec)
        goto out;

    work_buf = (uint8_t *)mpp_alloc(VID_STREAM_CHUNK_SIZE * 2);
    if (!work_buf) {
        loge("Failed to allocate work buffer");
        goto out;
    }

    if (dump_dir) {
        if (vid_dump_open(dump_dir, path) < 0)
            goto out;
    }

    while ((r_len = read(input_fd, work_buf + total, VID_STREAM_CHUNK_SIZE)) > 0) {
        total += r_len;
        int offset = 0;

        while (1) {
            int start = -1, next = -1;
            int frame_end;

            if (find_aicp_frame(work_buf + offset, total - offset, &start) < 0) {
                /* no frame start: keep tail for cross-chunk match */
                if (total - offset > 5)
                    offset = total - 5;
                break;
            }
            start += offset;

            /* next frame start is the end of current frame */
            if (find_aicp_frame(work_buf + start + 6, total - start - 6, &next) == 0) {
                frame_end = start + 6 + next;
                if (vid_decode_aicp_one_frame(dec, fb, work_buf + start,
                                              frame_end - start, 0) == 0)
                    frame_count++;
                offset = frame_end;
            } else {
                /* frame not complete yet, carry it to next read */
                offset = start;
                break;
            }
        }

        if (offset < total) {
            memmove(work_buf, work_buf + offset, total - offset);
            total -= offset;
        } else {
            total = 0;
        }

        if (total > VID_STREAM_CHUNK_SIZE) {
            loge("AICP frame larger than %d bytes, abort",
                 VID_STREAM_CHUNK_SIZE);
            goto out;
        }
    }

    /* decode the last frame carried to EOF */
    if (total > 0) {
        int start = -1;
        if (find_aicp_frame(work_buf, total, &start) == 0) {
            if (vid_decode_aicp_one_frame(dec, fb, work_buf + start,
                                          total - start, 1) == 0)
                frame_count++;
        }
    }

    if (frame_count == 0)
        logw("no AICP frame found in %s", path);
    else
        logi("AICP stream decoded: %d frames", frame_count);
    ret = 0;

out:
    if (dump_dir)
        vid_dump_close();
    if (work_buf)
        mpp_free(work_buf);
    if (dec)
        mpp_decoder_destory(dec);
    if (input_fd > 0)
        close(input_fd);
    return ret;
}

/* ---------- options ---------- */

static int str_to_format(char *str)
{
    /* str to format table struct */
    struct StrToFormat {
        char *str;
        int format;
    };

    /* format conversion */
    struct StrToFormat table[] = {
        {"argb8888", MPP_FMT_ARGB_8888},
        {"abgr8888", MPP_FMT_ABGR_8888},
        {"rgba8888", MPP_FMT_RGBA_8888},
        {"bgra8888", MPP_FMT_BGRA_8888},
        {"xrgb8888", MPP_FMT_XRGB_8888},
        {"xbgr8888", MPP_FMT_XBGR_8888},
        {"rgbx8888", MPP_FMT_RGBX_8888},
        {"bgrx8888", MPP_FMT_BGRX_8888},
        {"argb4444", MPP_FMT_ARGB_4444},
        {"abgr4444", MPP_FMT_ABGR_4444},
        {"rgba4444", MPP_FMT_RGBA_4444},
        {"bgra4444", MPP_FMT_BGRA_4444},
        {"rgb565", MPP_FMT_RGB_565},
        {"bgr565", MPP_FMT_BGR_565},
        {"argb1555", MPP_FMT_ARGB_1555},
        {"abgr1555", MPP_FMT_ABGR_1555},
        {"rgba5551", MPP_FMT_RGBA_5551},
        {"bgra5551", MPP_FMT_BGRA_5551},
        {"rgb888", MPP_FMT_RGB_888},
        {"bgr888", MPP_FMT_BGR_888},
        {"yuv420", MPP_FMT_YUV420P},
        {"nv12", MPP_FMT_NV12},
        {"nv21", MPP_FMT_NV21},
        {"yuv422", MPP_FMT_YUV422P},
        {"nv16", MPP_FMT_NV16},
        {"nv61", MPP_FMT_NV61},
        {"yuyv", MPP_FMT_YUYV},
        {"yvyu", MPP_FMT_YVYU},
        {"uyvy", MPP_FMT_UYVY},
        {"vyuy", MPP_FMT_VYUY},
        {"yuv400", MPP_FMT_YUV400},
        {"yuv444", MPP_FMT_YUV444P},
    };
    int i;
    int table_size = sizeof(table) / sizeof(table[0]);

    for (i = 0; i < table_size; i++) {
        if (!strncmp(str, table[i].str, strlen(table[i].str)))
            return table[i].format;
    }
    return -1;
}

static int parse_rotation(char *str)
{
    if (!strncmp(str, "90", strlen("90")))
        return MPP_ROTATION_90;

    if (!strncmp(str, "180", strlen("180")))
        return MPP_ROTATION_180;

    if (!strncmp(str, "270", strlen("270")))
        return MPP_ROTATION_270;

    return MPP_ROTATION_0;
}

static int parse_flip(char *str)
{
    if (!strncmp(str, "1", strlen("1")))
        return MPP_FLIP_H;

    if (!strncmp(str, "2", strlen("2")))
        return MPP_FLIP_V;

    if (!strncmp(str, "3", strlen("3")))
        return MPP_FLIP_H | MPP_FLIP_V;

    return 0;
}

static void print_help(char *program)
{
    printf("Usage: %s [options]\n", program);
    printf("\t -i, --input: \t\t input mp4 file name\n");
    printf("\t -o, --output: \t\t dump decoded frames to a directory\n");
    printf("\t -f, --format: \t\t output format:rgba8888/bgra8888/argb8888/abgr8888/bgr565/rgb565\n");
    printf("\t -r, --rotate: \t\t enable clockwise rotate(0/90/180/270)\n");
    printf("\t -s, --scale: \t\t enable scale(1- 1/2 scale; 2- 1/4 scale; 3- 1/8 scale)\n");
    printf("\t -l, --flip: \t\t enable flip(1-horizontal flip; 2-vertical flip; 3-ver & hor flip)\n");
    printf("\t -h, --help: \t\t print help info\n");
}

static int vid_check_mp4_ext(char *path)
{
    char *ptr = strrchr(path, '.');

    if (!ptr || strncmp(ptr, ".mp4", strlen(".mp4")) != 0) {
        logw("only mp4 container is supported now: %s", path);
        return -1;
    }
    return 0;
}

void video_test(int argc, char **argv)
{
    int out_format = MPP_FMT_ABGR_8888;
    int rot_flip_flag = 0;
    int ver_scale = 0;
    int hor_scale = 0;
    struct mpp_fb *fb = NULL;
    char input_path[VID_PATH_MAX_LEN] = {0};
    char dump_dir[VID_PATH_MAX_LEN] = {0};
    char *dump = NULL;
    int c;

    if (argc < 2) {
        print_help(argv[0]);
        return;
    }

    optind = 0;
    while ((c = getopt(argc, argv, "i:f:r:l:s:o:h")) != -1) {
        switch (c) {
        case 'i':
            snprintf(input_path, sizeof(input_path), "%s", optarg);
            continue;
        case 'f':
            out_format = str_to_format(optarg);
            continue;
        case 'r':
            rot_flip_flag |= parse_rotation(optarg);
            continue;
        case 'l':
            rot_flip_flag |= parse_flip(optarg);
            continue;
        case 's':
            ver_scale = atoi(optarg);
            hor_scale = ver_scale = ver_scale > 3 ? 3 : ver_scale;
            continue;
        case 'o':
            snprintf(dump_dir, sizeof(dump_dir), "%s", optarg);
            continue;
        case 'h':
        default:
            return print_help(argv[0]);
        }
    }

    if (input_path[0] == '\0') {
        print_help(argv[0]);
        return;
    }

    if (vid_check_mp4_ext(input_path) < 0)
        return;

    dump = dump_dir[0] ? dump_dir : NULL;

    /* In dump mode there is no display, decode frames to files only */
    if (!dump) {
        fb = mpp_fb_open();
        if (!fb) {
            loge("mpp_fb_open error!\n");
            return;
        }
    }

    /* mp4 is a container: sniff the media data to pick the decoder */
    switch (vid_detect_stream_type(input_path)) {
    case VID_STREAM_AICP:
        vid_decode_aicp_stream(fb, dump, input_path, out_format,
                               rot_flip_flag, ver_scale, hor_scale);
        break;
    /* TODO: case VID_STREAM_H264: vid_decode_h264_stream(...); */
    default:
        logw("video stream codec not supported yet");
        break;
    }

    if (fb)
        mpp_fb_close(fb);
}

MSH_CMD_EXPORT_ALIAS(video_test, video_test, video stream decode test);
