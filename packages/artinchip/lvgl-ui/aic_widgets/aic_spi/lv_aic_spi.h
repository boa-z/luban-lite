/*
 * Copyright (C) 2025-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors:  Huahui <huahui.mai@artinchip.com>
 */

#include <string.h>
#include <rtdevice.h>
#include <aic_core.h>
#include <drv_qspi.h>

#include "lvgl.h"
#include <mpp_fb.h>
#include <mpp_ge.h>

#include "aic_spi_lcd.h"
#include "lv_spi_panel.h"

#if defined(LV_SPI_ST77916)
#define  LV_LCD_WIDTH    360
#define  LV_LCD_HEIGHT   360
#elif defined(LV_SPI_ST7789)
#define  LV_LCD_WIDTH    320
#define  LV_LCD_HEIGHT   320
#elif defined(LV_SPI_ST77912)
#define  LV_LCD_WIDTH    240
#define  LV_LCD_HEIGHT   240
#elif defined(LV_SPI_GC9D01N)
#define  LV_LCD_WIDTH    160
#define  LV_LCD_HEIGHT   160
#else
#define  LV_LCD_WIDTH    0
#define  LV_LCD_HEIGHT   0
#endif

struct lv_spi_config {
    const char *panel;
    const aic_spi_lcd_cfg_t spi_cfg;
    const char *bl_pin;
    const char *te_pin;
    bool bl_low_active;
    unsigned int width;
    unsigned int height;
};

struct lv_spi_dev {
    const struct lv_spi_config *config;
    aic_spi_lcd_dev_t *dev;

    /* LVGL Draw Buffer */
    unsigned char *data;
    unsigned char *draw_buf[2];   /* the two CMA buffers handed to LVGL */
    unsigned int fb_size;
    unsigned int panel_w;
    unsigned int panel_h;

    /* SPI Transfer Buffer */
    unsigned char *tx_buf[2];
    unsigned char tx_wr_idx;
    unsigned int tx_len;
    struct mpp_ge *ge2d_dev;

    int te_pin;
    aicos_wqueue_t te_queue;
    aicos_sem_t display_sem;
    aicos_sem_t sync_ready;
    bool primed;

    bool bl_en;
    bool power_on;
    bool blit_owned;
    bool disp_thread_on;
    uint32_t spi_start;

    lv_display_t *disp;
    unsigned int id;
};

void lv_spi_flush(lv_display_t *disp, u8 *data);

void lv_spi_write_buffer(struct lv_spi_dev *spi_dev,
                         unsigned int cmd, unsigned int len, const u8 *data);

int lv_spi_display_init(int use_frame_buffer);

/*
 * defined in the spi tft controller dirver lv_xxx.c
 */
#if !defined(AIC_LVGL_FIVE_DISP_DEMO)
void lv_spi_panel_enable(struct lv_spi_dev *dev);
#endif

#define lv_spi_write_seq(dev, cmd, seq...)                  \
    do {                                                    \
        static const u8 d[] = { seq };                      \
        lv_spi_write_buffer(dev, cmd, ARRAY_SIZE(d), d);    \
    } while(0);

/*
 * Init ArtInChip SoC SPI Controller and enable
 * lcd peripheral by calling lv_spi_panel_enable()
 */
void lv_spi_screen_enable(struct lv_spi_dev *spi_dev);

int lv_spi_blit(int id, void *buf);
int lv_spi_blit_claim(int id);
int lv_spi_is_blit_owned(int id);
int lv_spi_get_blit_buffers(int id, void **buf0, void **buf1);
int lv_spi_get_panel_info(int id, unsigned int *width, unsigned int *height,
                          unsigned int *buf_size);
int lv_spi_get_phys_panel_info(int id, unsigned int *width, unsigned int *height);

struct disp_timing {
	uint32_t lvgl_ms;
	uint32_t sync_wait_ms;
	uint32_t ge2d_ms;
	uint32_t spi_ms;
	uint32_t frame_count;
};

void lv_spi_get_timing(int id, struct disp_timing *out);

uint32_t lv_spi_get_fps(int id);
