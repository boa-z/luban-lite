/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wu Dehuang <dehuang.wu@artinchip.com>
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <console.h>
#include <aic_core.h>
#include <aic_common.h>
#include <aic_errno.h>
#include <aic_stdio.h>
#include <boot_param.h>
#include <boot.h>
#include <boot_time.h>
#include <aic_reboot_reason.h>
#include <aic_clk_id.h>
#include <aic_reset_id.h>
#include <aic_soc.h>
#include <aic_hal.h>
#include <uart.h>
#include <config_parse.h>
#include <aicupg.h>
#include <mmc.h>
#include <hal_syscfg.h>
#include <aicupg_for_uart.h>
#ifdef LPKG_USING_USERID
#include <userid.h>
#endif
#ifdef LPKG_CHERRYUSB_DEVICE
#include <usbd_core.h>
#endif
#ifdef LPKG_CHERRYUSB_HOST
#include <usbh_core.h>
#endif
#ifdef LPKG_CHERRYUSB_OTG
#include <usb_otg.h>
#endif

#define UPG_TYPE_NONE 0
#define UPG_TYPE_USB 1
#define UPG_TYPE_HID 2
#define UPG_TYPE_UART 3
#define UPG_TYPE_SDCARD 4
#define UPG_TYPE_UDISK 5

#define UPG_MODE_NORMAL 0
#define UPG_MODE_USERID 1
#define UPG_MODE_FORCE 2

static int upg_type;
static int upg_mode;

#define ACK 0x06
#define CHECK_TIME_INTERVAL_US 10000
#if defined(LPKG_USING_USERID) || defined(AICUPG_FORCE_UPGRADE_SUPPORT)
static void uart_keywords_sync(char *keywords, int klen)
{
    int i = 0;
    char c;

    /* Send SYN + keywords to Host, if Tool resp in 10ms, then stop
     * booting and go into upgmode
     */
    c = 0x16; /* SYN */
    uart_putchar(0, c);
    for (i = 0; i < klen; i++)
        uart_putchar(0, keywords[i]);
}

static bool uart_keywords_detect(char *keywords, int klen)
{
    u64 start, cur;
    bool ret = false;

    if (AIC_BOOTLOADER_CONSOLE_UART != 0)
        uart_init(0);

    uart_keywords_sync(keywords, klen);

    start = aic_get_time_us();
    while (1) {
        cur = aic_get_time_us();
        if (uart_getchar(0) == ACK) {
            ret = true;
            break;
        }
        if ((cur - start) > CHECK_TIME_INTERVAL_US) {
            ret = false;
            break;
        }
    }

    return ret;
}
#endif

#if defined(LPKG_USING_USERID)
static bool uart_burn_userid_detect(void)
{
    return uart_keywords_detect("AIBURNID\n", sizeof("AIBURNID\n"));
}
#endif

#if defined(AICUPG_FORCE_UPGRADE_SUPPORT)
static bool uart_force_upgrade_detect(void)
{
    return uart_keywords_detect("AIBURNFORCE\n", sizeof("AIBURNFORCE\n"));
}
#endif

#define RUNCMD console_run_cmd

extern int usb_init(void);
extern int usb_deinit(void);
bool usbd_connect_pc_check(void)
{
    /*
     * If USB is connecting to HOST PC
     */
#if defined(AICUPG_USB_ENABLE)
    if (usb_init() == 0) {
        if (usbd_connect_check(1000)) {
            usb_deinit();
            return true;
        }
    }
    usb_deinit();
#endif

    return false;
}

extern int hid_init(void);
extern int hid_deinit(void);
bool usbd_hid_connect_pc_check(void)
{
    /*
     * If USB is connecting to HOST PC
     */
#if defined(AICUPG_HID_ENABLE)
    if (hid_init() == 0) {
        if (usbd_connect_check(1000)) {
            hid_deinit();
            return true;
        }
    }
    hid_deinit();
#endif

    return false;
}

static bool userid_check(void)
{
#if defined(LPKG_USING_USERID)
    /*
     * UserID case:
     * 1. Image already is burned to Flash
     * 2. Boot from normal flash, but need to check in BootLoader
     * 3. If UserID is not locked
     * 4. If USB is connecting to HOST
     * 5. If UART is connecting to HOST, and HOST TOOL is ready
     *
     */
    int ret;
    u32 flag = 0;

    ret = userid_init();
    if (ret)
        return false;

    ret = userid_read("lock", 0, (void *)&flag, 4);
    userid_deinit();
    if (ret > 0 && flag != 0) {
        /* userid is locked, no need to enter userid mode anymore */
        return false;
    }
    /*
     * userid is not locked, need to enter burn userid mode
     * 1. if USB is connecting to PC, use USB to burn userid
     * 2. Otherwise try to check uart mode
     */
    if (usbd_connect_pc_check()) {
        upg_type = UPG_TYPE_USB;
        upg_mode = UPG_MODE_USERID;
        return true;
    }
    if (uart_burn_userid_detect()) {
        upg_type = UPG_TYPE_UART;
        upg_mode = UPG_MODE_USERID;
        return true;
    }
#endif

    return false;
}

static bool force_upg_check(void)
{
#if defined(AICUPG_FORCE_UPGRADE_SUPPORT)
    /*
     * 1. if USB is connecting to PC, use USB to force upgrade
     * 2. Otherwise try to check uart mode
     */
    if (usbd_connect_pc_check()) {
        upg_type = UPG_TYPE_USB;
        upg_mode = UPG_MODE_FORCE;
        return true;
    }
    if (uart_force_upgrade_detect()) {
        upg_type = UPG_TYPE_UART;
        upg_mode = UPG_MODE_FORCE;
        return true;
    }
#endif

    return false;
}

int upg_type_check(enum boot_device bd)
{
    upg_type = UPG_TYPE_NONE;
    upg_mode = UPG_MODE_NORMAL;

#ifdef LPKG_CHERRYUSB_OTG
    usb_otg_init();
#endif

#ifdef AIC_WRI_DRV
    enum aic_reboot_reason r;

    r = aic_get_reboot_reason();
    /*
     * if REBOOT_REASON_UPGRADE is not clear in BROM or PBP, should be clear
     * in bootloader, otherwise watchdog reset will reboot failure.
     */
    if (r == REBOOT_REASON_UPGRADE)
        aic_clr_reboot_reason();

    if (r == REBOOT_REASON_BL_UPGRADE) {
        /*
         * User run command "aicupg gotobl" in Application, to enter upgmode of
         * BootLoader
         */
        if (usbd_connect_pc_check()) {
            upg_type = UPG_TYPE_USB;
            upg_mode = UPG_MODE_NORMAL;
        } else {
            upg_type = UPG_TYPE_UART;
            upg_mode = UPG_MODE_NORMAL;
        }
        aic_clr_reboot_reason();
        return 0;
    }

    if (r == REBOOT_REASON_BL_HID_UPGRADE) {
        /*
         * User run command "aicupg gotobl" in Application, to enter upgmode of
         * BootLoader
         */
        if (usbd_hid_connect_pc_check()) {
            upg_type = UPG_TYPE_HID;
            upg_mode = UPG_MODE_NORMAL;
        }
        aic_clr_reboot_reason();
        return 0;
    }
#endif

    if (bd == BD_USB) {
        if (usbd_connect_pc_check()) {
            upg_type = UPG_TYPE_USB;
            upg_mode = UPG_MODE_NORMAL;
        } else {
            upg_type = UPG_TYPE_UART;
            upg_mode = UPG_MODE_NORMAL;
        }
        return 0;
    }

    if (bd == BD_SDFAT32) {
        upg_type = UPG_TYPE_SDCARD;
        upg_mode = UPG_MODE_NORMAL;
        return 0;
    }

    if (bd == BD_UDISK) {
        upg_type = UPG_TYPE_UDISK;
        upg_mode = UPG_MODE_NORMAL;
        return 0;
    }

#if defined(AICUPG_UDISK_ENABLE)
    /*
     * D21x udisk upgrade in bootloader
     * D13x maybe BROM not recognize the udisk but bootloader can recognize it
     */
    boot_time_trace("UDISK checked");
    if (usbh_init() == 0) {
        if (usbh_hub_connect_check()) {
            upg_type = UPG_TYPE_UDISK;
            upg_mode = UPG_MODE_NORMAL;
            return 0;
        }
    }
#endif

    if (userid_check()) {
        if (usbd_connect_pc_check()) {
            upg_type = UPG_TYPE_USB;
            upg_mode = UPG_MODE_USERID;
        } else {
            upg_type = UPG_TYPE_UART;
            upg_mode = UPG_MODE_USERID;
        }
        return 0;
    }

    /*
     * Force upgrading mode, jump into USB/UART loop and checking
     * This checking should behind of USERID checking
     */
    if (force_upg_check()) {
        if (usbd_connect_pc_check()) {
            upg_type = UPG_TYPE_USB;
            upg_mode = UPG_MODE_FORCE;
        } else {
            upg_type = UPG_TYPE_UART;
            upg_mode = UPG_MODE_FORCE;
        }
        return 0;
    }

    return -1;
}

int bl_upgmode_get_type(void)
{
    return upg_type;
}

int bl_upgmode_get_mode(void)
{
    return upg_mode;
}

int bl_upgmode_detect(enum boot_device bd)
{
    int ret = 0, cont_boot = 1;
    char cmd[32];

    ret = upg_type_check(bd);

    if (ret) {
        return cont_boot;
    }

    if (upg_type == UPG_TYPE_NONE) {
        /*
         * No need to run upgmode, just try to boot from storage device
         */
        cont_boot = 1;
        return cont_boot;
    } else {
        if (upg_type == UPG_TYPE_SDCARD) {
#ifdef AICUPG_SDCARD_CONTROLLER_ID
            snprintf(cmd, sizeof(cmd), "aicupg fat mmc %d", AICUPG_SDCARD_CONTROLLER_ID);
#else
            snprintf(cmd, sizeof(cmd), "aicupg fat mmc %d", 1);
#endif
            ret = RUNCMD(cmd);
            /* SDCARD FAT32 information is from BROM, if upgrading failure, just
             * stop in command line mode
             */
            cont_boot = 0;
        }
        if (upg_type == UPG_TYPE_UDISK) {
            ret = RUNCMD("aicupg fat udisk 0");
            if (!ret) {
                /* Upgrading success, no need to run boot from storage anymore */
                cont_boot = 0;
            } else {
                /* Upgrading failed, need to run boot from storage */
                cont_boot = 1;
            }
        }
        if (upg_type == UPG_TYPE_USB) {
            if (upg_mode == UPG_MODE_NORMAL) {
                ret = RUNCMD("aicupg usb 0");
            } else if (upg_mode == UPG_MODE_USERID) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg usb 0 userid");
            } else if (upg_mode == UPG_MODE_FORCE) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg usb 0 force");
            }
            if (ret)
                cont_boot = 1;
            else
                cont_boot = 0;
        }
        if (upg_type == UPG_TYPE_HID) {
            if (upg_mode == UPG_MODE_NORMAL) {
                ret = RUNCMD("aicupg hid 0");
            } else if (upg_mode == UPG_MODE_USERID) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg hid 0 userid");
            } else if (upg_mode == UPG_MODE_FORCE) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg hid 0 force");
            }
            if (ret)
                cont_boot = 1;
            else
                cont_boot = 0;
        }
        if (upg_type == UPG_TYPE_UART) {
            if (upg_mode == UPG_MODE_NORMAL) {
                ret = RUNCMD("aicupg uart 0");
            } else if (upg_mode == UPG_MODE_USERID) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg uart 0 userid");
            } else if (upg_mode == UPG_MODE_FORCE) {
                /* If no host connect to device, skip it and continue to boot app */
                ret = RUNCMD("aicupg uart 0 force");
            }
            if (ret)
                cont_boot = 1;
            else
                cont_boot = 0;
        }
    }

    return cont_boot;
}

#if defined(AIC_BOOTLOADER_MINI) && defined(AICUPG_UDISK_ENABLE) && \
    (defined(AIC_USING_USB0_HOST) || defined(AIC_USING_USB0_OTG))
/*
 * The mini SPL only needs to know whether something is attached to the USB
 * port; the full SPL serves the upgrade itself, so no USB protocol
 * stack is linked in here. The sequence follows cherryusb's
 * usb_hc_low_level_init() and usb_hc_init().
 */
#define EHCI_HCOR_OFFSET     0x10
#define EHCI_USBCMD_RUN      (1 << 0)
#define EHCI_USBCMD_HCRESET  (1 << 1)
#define EHCI_USBCMD_ASEN     (1 << 5) /* Asynchronous Schedule Enable */
#define EHCI_USBSTS_HALTED   (1 << 12) /* HC Halted */
#define EHCI_PORTSC_CCS      (1 << 0) /* Current Connect Status */
#define EHCI_PORTSC_CSC      (1 << 1) /* Connect Status Change */
#define EHCI_PORTSC_PEC      (1 << 3) /* Port Enable/Disable Change */
#define EHCI_PORTSC_OCC      (1 << 5) /* Over-current Change */
#define EHCI_PORTSC_PP       (1 << 12) /* Port Power */

/* Offsets from the operational base */
#define EHCI_ASYNCLIST       0x18 /* Next Asynchronous List Address */
#define EHCI_CONFIGFLAG      0x40 /* Configured Flag: 1 = EHCI owns the ports */

/* Only syscfg v1.6 (d12p / g72x) gates the USB PLL, see usb_glue_aic.c */
#define USB_HOST_PLL_COM     0x878
#define USB_HOST_PLL_LOCKED  (1 << 23)

/* Same fallbacks as usb_glue_aic.c, boards may override them */
#ifndef AIC_USB_PHY_TXREF
#define AIC_USB_PHY_TXREF       0xA
#endif
#ifndef AIC_USB_TXPREEMPAMP
#define AIC_USB_TXPREEMPAMP     2
#endif

static inline u32 usb_host_rd(u32 off)
{
    return readl((volatile void *)(unsigned long)(USB_HOST0_BASE + off));
}

/* The controller does not start without an asynchronous schedule head */
static u32 g_ehci_async_qh[8] __attribute__((aligned(32)));

static inline void usb_host_wr(u32 off, u32 val)
{
    writel(val, (volatile void *)(unsigned long)(USB_HOST0_BASE + off));
}

static bool usb_host_device_attached(void)
{
    u32 val, portsc, hcor;
    u8 caplen;
    bool attached;
    int i;

    /* PHY and clock */
#if defined(AIC_SYSCFG_DRV_V16)
    hal_syscfg_usb_pll_en();
#endif
    hal_syscfg_usb_phy0_sw_host(1);

    hal_clk_enable(CLK_USB_PHY0);
    hal_clk_enable(CLK_USBH0);
    aicos_udelay(300);
    hal_reset_assert(RESET_USBPHY0);
    hal_reset_assert(RESET_USBH0);
    aicos_udelay(300);
    hal_reset_deassert(RESET_USBPHY0);
    hal_reset_deassert(RESET_USBH0);
    aicos_udelay(300);

    /* PHY type: UTMI */
    val = usb_host_rd(0x800);
    usb_host_wr(0x800, val | 0x1 | (0xe << 8));

    /* PHY tune: TX pre-emphasis current and TX vref, only when the board
     * overrides the hardware defaults (0x8 / 0) */
    if (AIC_USB_PHY_TXREF != 0x8 || AIC_USB_TXPREEMPAMP != 0) {
        val = usb_host_rd(0x818);
        val &= ~((0xf << 8) | 0x3);
        val |= (AIC_USB_PHY_TXREF << 8) | AIC_USB_TXPREEMPAMP;
        usb_host_wr(0x818, val);
    }

    /* AHB2STBUS: packet buffer IN/OUT threshold, OUT raised against
     * underrun (FIFO size - 4) */
    usb_host_wr(0x94, 32 | (127 << 16));

#if defined(AIC_SYSCFG_DRV_V16)
    /* The lock is only observable on syscfg v1.6. The timeout keeps us
     * from hanging on a broken PHY */
    for (i = 0; i < 20000; i++) {
        if (usb_host_rd(USB_HOST_PLL_COM) & USB_HOST_PLL_LOCKED)
            break;
    }
    if (i >= 20000) {
        hal_reset_assert(RESET_USBPHY0);
        hal_reset_assert(RESET_USBH0);
        hal_clk_disable(CLK_USB_PHY0);
        hal_clk_disable(CLK_USBH0);
        return false;
    }
#endif

    /* The operational registers start at caplength, usually 0x10 */
    caplen = usb_host_rd(0x00) & 0xff;
    hcor = caplen ? caplen : EHCI_HCOR_OFFSET;

    /* Controller reset */
    usb_host_wr(hcor + 0x00, 0);
    aicos_udelay(2000);
    usb_host_wr(hcor + 0x00, EHCI_USBCMD_HCRESET);
    for (i = 0; i < 1000; i++) {
        if (!(usb_host_rd(hcor + 0x00) & EHCI_USBCMD_HCRESET))
            break;
        aicos_udelay(100);
    }

    /* Polling is enough, no interrupt is needed or registered */
    usb_host_wr(hcor + 0x08, 0);
    usb_host_wr(hcor + 0x04, usb_host_rd(hcor + 0x04));

    /* An empty queue head that links to itself and heads the
     * reclamation list (type QH + H flag) */
    g_ehci_async_qh[0] = (u32)(unsigned long)&g_ehci_async_qh[0] | 2;
    g_ehci_async_qh[1] = (1 << 15); /* EPCHAR.H */
    usb_host_wr(hcor + EHCI_ASYNCLIST,
                (u32)(unsigned long)g_ehci_async_qh);
    aicos_dcache_clean_range((unsigned long *)g_ehci_async_qh,
                             sizeof(g_ehci_async_qh));

    /* Start the controller, the periodic schedule is not used */
    usb_host_wr(hcor + 0x00, EHCI_USBCMD_ASEN | EHCI_USBCMD_RUN);
    for (i = 0; i < 1000; i++) {
        if (!(usb_host_rd(hcor + 0x04) & EHCI_USBSTS_HALTED))
            break;
        aicos_udelay(100);
    }

    /* While this flag is 0 the ports belong to the companion controller
     * and PORTSC reports nothing useful */
    usb_host_wr(hcor + EHCI_CONFIGFLAG, 1);

    /* Power the port, a device cannot show up before that */
    portsc = usb_host_rd(hcor + 0x44);
    portsc |= EHCI_PORTSC_PP;
    portsc &= ~(EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC);
    usb_host_wr(hcor + 0x44, portsc);

    /* A device takes a while to pull D+ up after the port is powered, so
     * poll instead of waiting a fixed time. A device that is already
     * powered shows up on the first round. */
    for (i = 0; i < 50; i++) {
        aicos_udelay(10000);
        portsc = usb_host_rd(hcor + 0x44);
        if (portsc & EHCI_PORTSC_CCS)
            break;
    }

    attached = (portsc & EHCI_PORTSC_CCS) != 0;

    /* Leave no bus master running behind us, the next stage brings the
     * USB up again by itself */
    usb_host_wr(hcor + 0x00, 0);
    usb_host_wr(hcor + EHCI_CONFIGFLAG, 0);
    portsc = usb_host_rd(hcor + 0x44);
    usb_host_wr(hcor + 0x44, portsc & ~EHCI_PORTSC_PP);
    hal_reset_assert(RESET_USBPHY0);
    hal_reset_assert(RESET_USBH0);
    hal_clk_disable(CLK_USB_PHY0);
    hal_clk_disable(CLK_USBH0);

    return attached;
}

#endif

#if defined(AIC_BOOTLOADER_MINI) && \
    (defined(AIC_USING_USB0_DEVICE) || defined(AIC_USING_USB0_OTG)) && \
    defined(AICUPG_FORCE_UPGRADE_SUPPORT)
/*
 * Whether a real host (a PC) is driving the port, for the force upgrade:
 * the full SPL then serves it over USB. No USB stack is linked in
 * here, so bring the device controller up just far enough to see the bus
 * reset a host issues. Offsets and bits are the same on UDC v1.0
 * (d13x / d21x) and v3.0 (d12p / g72x); the sequence follows cherryusb's
 * usb_dc_low_level_init() and usb_dc_init().
 */
#define UDC_AHBBASIC         0x0000
#define UDC_USBDEVINIT       0x0004
#define UDC_USBPHYIF         0x0008
#define UDC_USBINTSTS        0x0010
#define UDC_USBINTMSK        0x0014
#define UDC_USBDEVCONF       0x0200
#define UDC_USBDEVFUNC       0x0204
#define UDC_PHYTUNE          0x8018

#define UDC_AHBIDLE          (1 << 2)
#define UDC_CSFTRST          (1 << 0)
#define UDC_GLBL_INTR_EN     (1 << 9)
#define UDC_SFTDISCON        (1 << 1)
#define UDC_INT_RESET        (1 << 12) /* bus reset from the host */
#define UDC_INT_ENUMDONE     (1 << 13) /* speed enumeration done */
#define UDC_DEV_SPEED_MASK   0x3

/* A host resets the port within about 100ms of seeing the pull-up */
#define UDC_RESET_TIMEOUT_MS 300
#define UDC_POLL_MS          5

static inline u32 usb_dev_rd(u32 off)
{
    return readl((volatile void *)(unsigned long)(USB_DEV_BASE + off));
}

static inline void usb_dev_wr(u32 off, u32 val)
{
    writel(val, (volatile void *)(unsigned long)(USB_DEV_BASE + off));
}

static bool usb_dev_pc_attached(void)
{
    u32 val;
    int i;
    bool attached = false;

    /* PHY and clock */
    hal_syscfg_usb_phy0_sw_host(0);
#if defined(AIC_SYSCFG_DRV_V16)
    hal_syscfg_usb_pll_en();
#endif
    hal_clk_enable(CLK_USB_PHY0);
    hal_clk_enable(CLK_USBD);
    aicos_udelay(300);
    hal_reset_assert(RESET_USBPHY0);
    hal_reset_assert(RESET_USBD);
    aicos_udelay(300);
    hal_reset_deassert(RESET_USBPHY0);
    hal_reset_deassert(RESET_USBD);
    aicos_udelay(300);

    if (AIC_USB_PHY_TXREF != 0x8 || AIC_USB_TXPREEMPAMP != 0) {
        val = usb_dev_rd(UDC_PHYTUNE);
        val &= ~((0xf << 8) | 0x3);
        val |= (AIC_USB_PHY_TXREF << 8) | AIC_USB_TXPREEMPAMP;
        usb_dev_wr(UDC_PHYTUNE, val);
    }

    /* Stay disconnected while the controller is set up, so that the host
     * sees a fresh connect and resets the port afterwards */
    usb_dev_wr(UDC_USBDEVFUNC, usb_dev_rd(UDC_USBDEVFUNC) | UDC_SFTDISCON);
    usb_dev_wr(UDC_USBDEVINIT, usb_dev_rd(UDC_USBDEVINIT) & ~UDC_GLBL_INTR_EN);

    /* Core soft reset */
    for (i = 0; i < 1000; i++) {
        if (usb_dev_rd(UDC_AHBBASIC) & UDC_AHBIDLE)
            break;
        aicos_udelay(10);
    }
    usb_dev_wr(UDC_USBDEVINIT, usb_dev_rd(UDC_USBDEVINIT) | UDC_CSFTRST);
    for (i = 0; i < 1000; i++) {
        if (!(usb_dev_rd(UDC_USBDEVINIT) & UDC_CSFTRST))
            break;
        aicos_udelay(10);
    }

    /* UTMI+, high speed; polling is enough, no interrupt is needed */
    usb_dev_wr(UDC_USBPHYIF, (0x5 << 10) | (0x7 << 0));
    usb_dev_wr(UDC_USBDEVCONF, usb_dev_rd(UDC_USBDEVCONF) & ~UDC_DEV_SPEED_MASK);
    usb_dev_wr(UDC_USBINTMSK, 0);
    usb_dev_wr(UDC_USBINTSTS, 0xBFFFFFFF); /* W1C, clear all pending */
    usb_dev_wr(UDC_USBDEVINIT, usb_dev_rd(UDC_USBDEVINIT) | UDC_GLBL_INTR_EN);

    /* Connect and let the host drive the port */
    usb_dev_wr(UDC_USBDEVFUNC, usb_dev_rd(UDC_USBDEVFUNC) & ~UDC_SFTDISCON);

    for (i = 0; i < UDC_RESET_TIMEOUT_MS / UDC_POLL_MS; i++) {
        if (usb_dev_rd(UDC_USBINTSTS) & (UDC_INT_RESET | UDC_INT_ENUMDONE)) {
            attached = true;
            break;
        }
        aicos_udelay(UDC_POLL_MS * 1000);
    }

    /* Disconnect and leave nothing running behind us */
    usb_dev_wr(UDC_USBDEVFUNC, usb_dev_rd(UDC_USBDEVFUNC) | UDC_SFTDISCON);
    usb_dev_wr(UDC_USBINTMSK, 0);
    hal_reset_assert(RESET_USBPHY0);
    hal_reset_assert(RESET_USBD);
    hal_clk_disable(CLK_USB_PHY0);
    hal_clk_disable(CLK_USBD);

    return attached;
}

#endif

/*
 * For the mini SPL: tell whether an upgrade is being requested, so that it
 * hands over to the full SPL. Only the cheap checks are done, the
 * full SPL resolves the upgrade type afterwards and serves it.
 */
bool bl_upgmode_requested(enum boot_device bd)
{
    enum aic_reboot_reason r;

    /*
     * BROM or PBP may leave REBOOT_REASON_UPGRADE behind, clear it before
     * any early return, otherwise a later reset would enter the upgrade
     * mode again.
     */
    r = aic_get_reboot_reason();
    if (r == REBOOT_REASON_UPGRADE)
        aic_clr_reboot_reason();

    if (bd == BD_USB || bd == BD_UDISK || bd == BD_SDFAT32)
        return true;

    /* BL_UPGRADE / BL_HID_UPGRADE are left for the full SPL, it reads
     * the same flag to pick the USB, UART or HID upgrade mode */
    if (r == REBOOT_REASON_BL_UPGRADE || r == REBOOT_REASON_BL_HID_UPGRADE)
        return true;

#ifdef AICUPG_FORCE_UPGRADE_SUPPORT
    if (uart_force_upgrade_detect())
        return true;
#endif

#ifdef LPKG_USING_USERID
    if (uart_burn_userid_detect())
        return true;
#endif

#if defined(AIC_BOOTLOADER_MINI) && defined(AICUPG_UDISK_ENABLE) && \
    (defined(AIC_USING_USB0_HOST) || defined(AIC_USING_USB0_OTG))
    if (usb_host_device_attached()) {
        printf("USB disk attached\n");
        return true;
    }
#endif

#if defined(AIC_BOOTLOADER_MINI) && \
    (defined(AIC_USING_USB0_DEVICE) || defined(AIC_USING_USB0_OTG)) && \
    defined(AICUPG_FORCE_UPGRADE_SUPPORT)
    if (usb_dev_pc_attached()) {
        printf("PC attached\n");
        return true;
    }
#endif

    return false;
}
