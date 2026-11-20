#!/usr/bin/env python3
# -*- coding:utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2026 ArtInChip Technology Co., Ltd
# Yiguan Xu <yiguan.xu@artinchip.com>
#
# Convert a board's RT-Thread defconfig into the FreeRTOS defconfig of the
# same board. Only Kconfig symbols are touched, no driver or board source.
#
#     python3 rtt2freertos.py target/configs/<chip>_<board>_rt-thread_<app>_defconfig


import argparse
import os
import re
import sys

# ---------------------------------------------------------------------------
# Configuration tables
# ---------------------------------------------------------------------------

# AIC framework drivers built on rt_device, with no drv_bare counterpart.
# Matching is "exact name" or "prefix + '_'".
AIC_FRAMEWORK_SWITCHES = (
    "AIC_USING_TOUCH",      # -> AIC_USING_CTP -> touch_common.c
    "AIC_USING_TSEN",
    "AIC_USING_MTOP",
    "AIC_USING_CE",
    "AIC_USING_SID",
    "AIC_USING_GPAI",
    "AIC_USING_RTP",
    "AIC_USING_HRTIMER0",
    "AIC_ADCIM_DRV",
    "AIC_STARTUP_UI_SHOW",
    "AIC_MPP",              # MPP family, see also FORCE_OFF
    "LPKG_USING_BONNIE",
    "LPKG_USING_CPU_USAGE",
)

# Component-package switches that must be removed explicitly: they live in
# packages/, so Kconfig does not drop them along with the kernel side.
# Packages that include rtthread.h at file scope, that only ship an RT-Thread
# port, or that the removed drivers above select.
PACKAGE_RT_ONLY_SWITCHES = (
    "RT_USING_DFS_MNTTABLE",            # dfs_fs.c uses rt_device_t / RT_NULL
    # USB host stack: the RISC-V cherryusb port lacks xPortIsInsideInterrupt()
    "LPKG_USING_CHERRYUSB",
    "LPKG_CHERRYUSB",
    "AIC_USING_USB0_HOST",
    "AIC_USING_USB1_HOST",
    "LPKG_USING_CPU_LOAD",
    "LPKG_USING_MHZ",
    "LPKG_USING_I2C_TOOLS",
    "LPKG_USING_PINMUX_CHECHK",
    "AIC_ENV_INTERFACE",
    "LPKG_USING_OTA_DOWNLOADER",
    "LPKG_USING_HTTP_OTA_DOWNLOADER",
    "LPKG_USING_AIC_AUTHORIZATION",
    "LPKG_USING_AIC_SPIENC_AUTHORIZATION",
    "LPKG_USING_USERID",                # only _rtt.c / _bare.c variants
    "LPKG_USING_CJSON",
    # Multimedia and networking: RT-Thread ports only
    "LPKG_MPP",
    "LPKG_USING_MINIMP3",
    "LPKG_USING_LWIP",
    "LPKG_LWIP",
    "LPKG_USING_NETUTILS",
    "LPKG_USING_NETTOOLS",
    "LPKG_NETUTILS",
    "LPKG_USING_COMP_HID_TABLE1",       # USB composite descriptor tables
    "LPKG_USING_COMP_UAC_TABLE1",
)

# bsp/peripheral/ drivers are not required to use an OSAL, so they call
# rtthread.h / rtdevice.h / rt_kprintf / RT_NULL directly and do not compile
# under FreeRTOS. Their switches stay visible in the target tree.
# nftl / spinand / spinor_sfud are left out: they belong to the storage chain.
PERIPHERAL_RT_ONLY_SWITCHES = (
    "AIC_USING_CAMERA",         # camera root switch and sensors
    "AIC_I2S_CODEC",            # codec root switch AIC_I2S_CODEC_SELECT and codecs
    "AIC_BT",                   # Bluetooth: AIC_BT_BT8858A / AIC_BT_REALTEK
    "AIC_WLAN",                 # WLAN: REALTEK / ASR / AIC8800D40L / HUGEIC
    "WIFI_USING_SDIOWIFI_ATBM",
    "AIC_WIRELESS",             # root switch of bsp/peripheral/wireless/
    "AIC_RTC_PCF8563",          # external RTC
    "AIC_RTC_RX8010",
    "AIC_ENCODER_HEC40",
    "AIC_GYRO_SC7A20H",
    "AIC_QSPI_LCD_INTERFACE",   # spilcd (SPI panel)
    "AIC_USING_CTP",            # touch controller (root switch handled above)
)

# The graphics stack (LVGL + MPP) is dropped as a whole. Neither has a
# drv_bare counterpart, and the LVGL integration layer includes mpp_fb.h
# (lv_port_disp.h:16), so no demo can be kept. LPKG_MPP is "default y" and
# needs an explicit "is not set" line as well -- see FORCE_OFF.
GRAPHICS_SWITCHES = (
    "AIC_LVGL",             # LVGL demos and their sub-switches
    "LPKG_USING_LVGL",
    "LPKG_LVGL",
    "LVGL_V",               # LVGL_V_8 / LVGL_V_9
    "LV",                   # LV_USE_* / LV_COLOR_* tuning knobs
)

# DFS mapping: kernel-side symbol -> component-side symbol
# (packages/third-party/dfs/). Keys omit the CONFIG_ prefix, values are the
# full configuration line names.
DFS_MAP = {
    "RT_USING_DFS_ELMFAT": ("CONFIG_LPKG_USING_DFS", "CONFIG_LPKG_USING_DFS_ELMFAT"),
    "RT_USING_DFS_ROMFS": ("CONFIG_LPKG_USING_DFS", "CONFIG_LPKG_USING_DFS_ROMFS"),
    "RT_USING_DFS_UFFS": ("CONFIG_LPKG_USING_DFS", "CONFIG_LPKG_USING_DFS_UFFS"),
    "RT_USING_DFS_LITTLEFS": ("CONFIG_LPKG_USING_DFS", "CONFIG_LPKG_USING_DFS_LITTLEFS"),
    # Long-filename choice entries: both sides define the same choices under
    # different prefixes, so a name match misses them and the setting is
    # silently lost (the component default is ANSI/OEM).
    "RT_DFS_ELM_USE_LFN_0": ("CONFIG_LPKG_DFS_ELM_USE_LFN_0",),
    "RT_DFS_ELM_USE_LFN_1": ("CONFIG_LPKG_DFS_ELM_USE_LFN_1",),
    "RT_DFS_ELM_USE_LFN_2": ("CONFIG_LPKG_DFS_ELM_USE_LFN_2",),
    "RT_DFS_ELM_USE_LFN_3": ("CONFIG_LPKG_DFS_ELM_USE_LFN_3",),
    "RT_DFS_ELM_LFN_UNICODE_0": ("CONFIG_LPKG_DFS_ELM_LFN_UNICODE_0",),
    "RT_DFS_ELM_LFN_UNICODE_1": ("CONFIG_LPKG_DFS_ELM_LFN_UNICODE_1",),
    "RT_DFS_ELM_LFN_UNICODE_2": ("CONFIG_LPKG_DFS_ELM_LFN_UNICODE_2",),
    "RT_DFS_ELM_LFN_UNICODE_3": ("CONFIG_LPKG_DFS_ELM_LFN_UNICODE_3",),
}

# Bare console option set (drv_bare/printf + drv_bare/console).
BARE_CONSOLE_BASE = (
    "CONFIG_AIC_CONSOLE_BARE_DRV=y",
    "CONFIG_AIC_PRINTF_BARE_DRV=y",
)

# Bare storage chain, filled in per flash type. The branch is taken at compile
# time, so a missing entry is a build error:
#
#   NAND: mtd_probe.c takes the #ifdef AIC_SPINAND_DRV branch and needs the
#         NFTL translation layer.
#   NOR : mtd_probe.c takes the #ifdef AIC_SPINOR_DRV branch and includes
#         sfud.h, which spinor_sfud adds to CPPPATH only when
#         LPKG_USING_SFUD=y.
#   MMC : drv_bare/sdmc + drv_bare/block, triggered by AIC_USING_SDMCx;
#         nothing to add.
BARE_STORAGE_MTD = "CONFIG_AIC_MTD_BARE_DRV=y"
BARE_STORAGE_NAND = (
    "CONFIG_AIC_NFTL_SUPPORT=y",
    "CONFIG_AIC_NFLT_USED_LIB=y",
)
BARE_STORAGE_NOR = (
    "CONFIG_LPKG_USING_SFUD=y",
)

# Symbols that default to y in the target tree and that the RT-Thread solution
# switches off explicitly. Dropping the "# CONFIG_X is not set" line would let
# the default turn them back on, so the line is carried over. Symbols from
# kernel-only trees (RT_*, FAL_*) need no entry.
DEFAULT_Y_SWITCHES = (
    "AIC_AUDIO_DMIC",
    "AIC_BARCODE_BEEP",
    "AIC_DISP_BOOTUP_LOGO",
    "AIC_I2S_CODEC_TLV320",
    "AIC_USING_DE",
    "AIC_USING_GE",
    "AIC_USING_SDMC1",
    "AIC_USING_THREAD_STACK_TRACE",
    "AIC_USING_VE",
    "LPKG_MPP",
    "LPKG_USING_ENV",
    "LPKG_USING_FDTLIB",
    "LPKG_USING_MINIMP3",
    "LPKG_USING_NETTOOLS",
    "RGB_DATA_MIRROT",
    "USBDEV_MSC_THREAD",
    "USBD_MSC_STORAGE_USING_HOTPLUG",
)

# Blacklisted symbols whose Kconfig default is y: omitting the =y line is not
# enough, they need an explicit "is not set". Every entry must also be matched
# by one of the blacklists above.
FORCE_OFF = (
    "LPKG_MPP",
    "LPKG_USING_MINIMP3",
    "LPKG_USING_NETTOOLS",
)

RE_CONFIG = re.compile(r"^(CONFIG_[A-Za-z0-9_]+)=(.*)$")
RE_NOTSET = re.compile(r"^# (CONFIG_[A-Za-z0-9_]+) is not set$")
RE_CONSOLE = re.compile(r'^CONFIG_RT_CONSOLE_DEVICE_NAME="uart(\d+)"')


# ---------------------------------------------------------------------------
# defconfig parsing
# ---------------------------------------------------------------------------

def _bare(name):
    """Strip the CONFIG_ prefix."""
    return name[len("CONFIG_"):] if name.startswith("CONFIG_") else name


def parse_defconfig(path):
    """Parse a defconfig into [(name, line)]; name is None for comments/blanks."""
    entries = []
    with open(path, "r", encoding="utf-8") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            m = RE_CONFIG.match(line) or RE_NOTSET.match(line)
            entries.append((m.group(1) if m else None, line))
    return entries


def get_value(entries, name):
    """Return an option's value without quotes; None if unset or 'not set'."""
    for item, line in entries:
        if item != name:
            continue
        m = RE_CONFIG.match(line)
        if not m:
            return None
        return m.group(2).strip().strip('"')
    return None


# Whitelist: options known to work under FreeRTOS. Anything outside is dropped.
#
# A blacklist cannot be exhaustive here. Most peripheral switches stay visible
# in the target tree and are interlinked through select chains, so deleting a
# child entry only makes its parent select it back (AIC_USING_I2S0 ->
# AIC_I2S_DRV -> select AIC_I2S_CODEC_SELECT -> TLV320). Some directories
# under bsp/peripheral/ have no guarding switch at all.
#
# Retention criterion: the peripheral has a bare driver under
# bsp/artinchip/drv_bare/ (built when DRIVER_BARE_DRV_EN is set), or it is a
# kernel-agnostic board resource.
KEEP_EXACT = (
    # Bare console option set
    "AIC_CONSOLE_BARE_DRV",
    "AIC_PRINTF_BARE_DRV",
    "AIC_BAREMETAL_CONSOLE_UART",
    "AIC_CONSOLE_SYSNAME",
    # Bare storage chain
    "AIC_MTD_BARE_DRV",
    "AIC_NFTL_SUPPORT",
    "AIC_NFLT_USED_LIB",
    "LPKG_USING_SFUD",
    "LPKG_USING_DFS",
    "LPKG_USING_DFS_ELMFAT",
    "LPKG_USING_DFS_ROMFS",
    "LPKG_USING_DFS_UFFS",
    "LPKG_USING_DFS_LITTLEFS",
    "DFS_FILESYSTEMS_MAX",
    # Base peripherals with a drv_bare implementation
    "AIC_USING_DMA",
    "AIC_USING_RTC",
    "AIC_USING_WDT",
    "AIC_USING_WRI",
    "AIC_INTERRUPTSTACK_SIZE",
)

# Whitelist prefixes (cover groups such as AIC_USING_UART0/UART1)
KEEP_PREFIX = (
    "PRJ_",                     # project header (rewritten separately)
    "KERNEL_",                  # kernel identity
    "DRIVER_",                  # driver layer switches
    "AIC_DRAM_",                # DRAM layout
    "AIC_PSRAM_",               # PSRAM layout
    "AIC_USING_UART",           # UART
    "AIC_USING_QSPI",           # QSPI controller
    "AIC_QSPI0_DEVICE",         # flash part mounted on QSPI0..QSPI4
    "AIC_QSPI1_DEVICE",
    "AIC_QSPI2_DEVICE",
    "AIC_QSPI3_DEVICE",
    "AIC_QSPI4_DEVICE",
    "AIC_USING_SDMC",           # SD/MMC
    "RT_DFS_ELM",               # DFS component (packages/third-party/dfs)
    # Resource image, not graphics code: packs a directory into a fatfs
    # partition at build time. The *-secure boards sign rodata.fatfs, so
    # dropping it breaks their packaging.
    "AIC_USING_FS_IMAGE",
    "AIC_FS_IMAGE",
)


# Prefixes of the switches worth listing in the "dropped" report. Kernel
# options and plain value settings are counted silently instead, to keep the
# report readable.
REPORTED_PREFIXES = ("AIC_USING_", "AIC_I2S_", "AIC_WLAN", "AIC_BT", "AIC_RTC_",
                     "AIC_ENCODER", "AIC_GYRO", "AIC_GMAC", "LPKG_", "PKG_",
                     "AIC_LVGL")


# AIC_ options that are peripheral/driver switches: no bare implementation, so
# they must be dropped. The rest of the AIC_ namespace is kept conservatively
# because it holds build-time options the link script templates depend on (33
# macros such as AIC_SRAM_* / AIC_XIP* / AIC_SEC_*), and dropping one of those
# breaks the link.
AIC_DRIVER_PATTERNS = (
    "AIC_USING_",       # peripheral enable switches (UART/QSPI/SDMC kept above)
    "AIC_I2S", "AIC_I2C", "AIC_SPI", "AIC_CAN", "AIC_AUDIO", "AIC_CODEC",
    "AIC_CAMERA", "AIC_TOUCH", "AIC_CTP", "AIC_TSEN", "AIC_GPAI", "AIC_CE",
    "AIC_SID", "AIC_MTOP", "AIC_RTP", "AIC_HRTIMER", "AIC_ADCIM",
    "AIC_VE", "AIC_GE", "AIC_DVP", "AIC_DE", "AIC_GMAC", "AIC_WLAN", "AIC_BT",
    "AIC_ENCODER", "AIC_GYRO", "AIC_USB", "AIC_MMC", "AIC_PWM", "AIC_KEYADC",
    "AIC_PSADC", "AIC_DISP", "AIC_PAN", "AIC_RING_BUFFER",
)


def is_blacklisted(name):
    """Return True if the option is known to be unusable under FreeRTOS.

    Covers four sources: AIC framework drivers, RT dependencies living in
    component packages, bsp/peripheral drivers, and the graphics stack.
    """
    bare = _bare(name)
    for sw in (AIC_FRAMEWORK_SWITCHES + PACKAGE_RT_ONLY_SWITCHES
               + PERIPHERAL_RT_ONLY_SWITCHES + GRAPHICS_SWITCHES):
        if bare == sw or bare.startswith(sw + "_"):
            return True
    return False


def is_kept(name):
    """Decide whether an option is kept for the FreeRTOS solution."""
    bare = _bare(name)
    # 0) The blacklist wins over the conservative AIC_ range below.
    if is_blacklisted(name):
        return False
    # 1) Whitelist: drivers and components confirmed to work under FreeRTOS
    if bare in KEEP_EXACT:
        return True
    if any(bare.startswith(p) for p in KEEP_PREFIX):
        return True
    # 2) Non-driver AIC_ options (memory layout, boot mode, link parameters):
    #    keeping an extra switch only adds redundancy, dropping a layout macro
    #    breaks the link. The driver self-test programs (AIC_UART_DRV_TEST,
    #    AIC_DISPLAY_TEST ...) are excluded: several of them (test-uart,
    #    test-ctp, test-fb) have no kernel guard in their SConscript.
    if bare.startswith("AIC_") and "_TEST" not in bare and not any(
            bare.startswith(p) for p in AIC_DRIVER_PATTERNS):
        return True
    # 3) Everything else (LPKG_/PKG_/RT_/FINSH_/ULOG_ ...) is dropped
    return False


# ---------------------------------------------------------------------------
# Conversion
# ---------------------------------------------------------------------------

def _line_symbol(line):
    """Return the CONFIG_ symbol a defconfig line refers to."""
    m = RE_CONFIG.match(line) or RE_NOTSET.match(line)
    if not m:
        raise ValueError("internal error: malformed config line %r" % line)
    return m.group(1)


def _read_console_uart(entries):
    """Console UART number, derived from RT_CONSOLE_DEVICE_NAME."""
    for name, line in entries:
        if name != "CONFIG_RT_CONSOLE_DEVICE_NAME":
            continue
        m = RE_CONSOLE.match(line)
        if m:
            return int(m.group(1))
    return 0


def _read_qspi_buses(entries):
    """QSPI buses that actually carry a bare NOR/NAND device."""
    buses = set()
    for name, _ in entries:
        m = re.match(r"^CONFIG_AIC_QSPI(\d)_DEVICE_(SPINOR|SPINAND)$", name or "")
        if m:
            buses.add(m.group(1))
    return buses


def _read_storage(entries):
    """Which parts of the bare storage chain the source solution needs.

    Any bus selects AIC_SPINOR_DRV / AIC_SPINAND_DRV, so all of QSPI0..QSPI4
    are checked, not just the bus the board happens to boot from.
    """
    spinand = spinor = False
    for bus in range(5):
        spinand |= get_value(entries,
                             "CONFIG_AIC_QSPI%d_DEVICE_SPINAND" % bus) == "y"
        spinor |= get_value(entries,
                            "CONFIG_AIC_QSPI%d_DEVICE_SPINOR" % bus) == "y"
    dfs = any(n and (n.startswith("CONFIG_RT_USING_DFS")
                     or n.startswith("CONFIG_LPKG_USING_DFS"))
              for n, _ in entries)
    return spinand, spinor, dfs


class Converter(object):
    """Turns one board's RT-Thread defconfig into its FreeRTOS defconfig."""

    def __init__(self, entries, console_uart=None, sysname="aic"):
        self.entries = entries
        self.sysname = sysname
        self.console_uart = (_read_console_uart(entries) if console_uart is None
                             else console_uart)
        self.qspi_buses = _read_qspi_buses(entries)
        self.lines = []
        self.seen = set()
        self.report = {"dropped": [], "mapped": [], "added": [],
                       "kept_off": [], "silent": 0}

    def run(self, new_name):
        for name, line in self.entries:
            if name is None:
                self.lines.append(line)
            elif not self._rewrite(name, line, new_name):
                self._filter(name, line)
        self._append_forced_off()
        self._append_console()
        self._append_storage()
        return self.lines, self.report

    # -- output -----------------------------------------------------------

    def _write(self, line):
        """Append a line; a symbol already written is left untouched."""
        name = _line_symbol(line)
        if name in self.seen:
            return False
        self.seen.add(name)
        self.lines.append(line)
        return True

    # -- per source line --------------------------------------------------

    def _rewrite(self, name, line, new_name):
        """Handle a line whose symbol is rewritten instead of kept."""
        bare = _bare(name)

        # An explicit "is not set" must survive when the symbol defaults to y
        # in the target tree, otherwise Kconfig would turn it back on.
        if bare in DEFAULT_Y_SWITCHES and RE_NOTSET.match(line):
            if self._write(line):
                self.report["kept_off"].append(name)
            return True

        # Project header
        if name == "CONFIG_PRJ_KERNEL":
            self._write('CONFIG_PRJ_KERNEL="freertos"')
            self.report["mapped"].append('%s -> "freertos"' % name)
            return True
        if name == "CONFIG_PRJ_DEFCONFIG_FILENAME":
            self._write('CONFIG_PRJ_DEFCONFIG_FILENAME="%s_defconfig"' % new_name)
            self.report["mapped"].append("%s -> %s_defconfig" % (name, new_name))
            return True

        # Console: keep only the UART number; the bare console set is written
        # as a whole further down.
        if name == "CONFIG_RT_CONSOLE_DEVICE_NAME":
            self.report["mapped"].append("RT_CONSOLE_DEVICE_NAME -> "
                                         "AIC_BAREMETAL_CONSOLE_UART=%d"
                                         % self.console_uart)
            return True

        # DFS: kernel-side symbol -> component-side symbol. Must run before the
        # whitelist filter, otherwise the option would simply be dropped.
        if bare in DFS_MAP:
            for sym in DFS_MAP[bare]:
                if self._write("%s=y" % sym):
                    self.report["mapped"].append("%s -> %s" % (name, sym))
            return True

        return self._drop_orphan_qspi(bare, name)

    def _drop_orphan_qspi(self, bare, name):
        """Drop AIC_USING_QSPI<N> when bus N carries no bare flash device.

        sfud_port.c keys its device table off that switch but needs
        AIC_QSPI<N>_DEVICE_SPINOR_FREQ, which only a configured device defines.
        """
        m = re.match(r"^AIC_USING_QSPI(\d)$", bare)
        if not m or not self.qspi_buses or m.group(1) in self.qspi_buses:
            return False
        self.report["dropped"].append(
            "%s (no AIC_QSPI%s_DEVICE_* configured)" % (name, m.group(1)))
        return True

    def _filter(self, name, line):
        """Whitelist filter for the lines that are not rewritten."""
        if is_kept(name):
            self._write(line)
            return
        if _bare(name).startswith(REPORTED_PREFIXES):
            self.report["dropped"].append(name)
        else:
            self.report["silent"] += 1

    # -- appended at the end ----------------------------------------------

    def _append_forced_off(self):
        """Blacklisted symbols whose default is y need an explicit line.

        A source plan that already switched the symbol off carries the very
        same line, in which case there is nothing left to write.
        """
        for bare in FORCE_OFF:
            line = "# CONFIG_%s is not set" % bare
            if self._write(line):
                self.report["added"].append("%s (Kconfig default is y)" % line)

    def _append_console(self):
        lines = list(BARE_CONSOLE_BASE)
        lines.append("CONFIG_AIC_BAREMETAL_CONSOLE_UART=%d" % self.console_uart)
        if self.sysname:
            lines.append('CONFIG_AIC_CONSOLE_SYSNAME="%s"' % self.sysname)
        for line in lines:
            if self._write(line):
                self.report["added"].append(line)

    def _append_storage(self):
        """Bare storage chain, only when the source solution uses storage."""
        spinand, spinor, dfs = _read_storage(self.entries)
        lines = []
        if spinand or spinor or dfs:
            lines.append(BARE_STORAGE_MTD)
        if spinand:
            lines.extend(BARE_STORAGE_NAND)
        if spinor:
            lines.extend(BARE_STORAGE_NOR)
        for line in lines:
            if self._write(line):
                self.report["added"].append(line)


def convert(entries, console_uart=None, sysname="aic"):
    """Run the conversion. Returns (output lines, report dict, new plan name)."""
    chip = get_value(entries, "CONFIG_PRJ_CHIP")
    board = get_value(entries, "CONFIG_PRJ_BOARD")
    kernel = get_value(entries, "CONFIG_PRJ_KERNEL")
    app = get_value(entries, "CONFIG_PRJ_APP")

    if not all([chip, board, kernel, app]):
        sys.exit("input file has no project header "
                 "(PRJ_CHIP/PRJ_BOARD/PRJ_KERNEL/PRJ_APP)")
    if kernel != "rt-thread":
        sys.exit("input PRJ_KERNEL is '%s'; this tool only handles rt-thread "
                 "solutions" % kernel)

    new_name = "%s_%s_freertos_%s" % (chip, board, app)
    lines, report = Converter(entries, console_uart, sysname).run(new_name)
    return lines, report, new_name


# ---------------------------------------------------------------------------
# Validation (optional, requires kconfiglib)
# ---------------------------------------------------------------------------

def verify(sdk_root, chip, board, kernel, app, defconfig_path):
    """Generate .Kconfig.prj for the target kernel and check symbol visibility."""
    try:
        import kconfiglib
    except ImportError:
        print("  [skipped] kconfiglib not installed, cannot verify")
        return

    prj = os.path.join(sdk_root, ".Kconfig.prj")
    with open(prj, "w", encoding="utf-8") as fh:
        fh.write('source "bsp/artinchip/sys/%s/Kconfig.chip"\n' % chip)
        fh.write('source "target/%s/common/Kconfig.board"\n' % chip)
        ext = os.path.join(sdk_root, "target", chip, board, "Kconfig.extend")
        if os.path.exists(ext):
            fh.write('source "target/%s/%s/Kconfig.extend"\n' % (chip, board))
        fh.write('source "kernel/%s/Kconfig"\n' % kernel)
        fh.write('source "application/%s/%s/Kconfig"\n' % (kernel, app))
        if kernel == "rt-thread":
            fh.write('source "tools/env/packages/packages/Kconfig"\n')

    out_entries = parse_defconfig(defconfig_path)

    # The root Kconfig reaches the target tree through the literal line
    # source ".Kconfig.prj", so the file has to sit in the SDK root. Loading
    # the root by absolute path keeps the process cwd untouched.
    kconf = kconfiglib.Kconfig(os.path.join(sdk_root, "Kconfig"),
                               suppress_traceback=True)

    # A symbol counts as "defined" only when its nodes list is non-empty:
    # kconfiglib also creates Symbol objects for names merely referenced by
    # select/depends, and those have an empty nodes list.
    def defined(name):
        sym = kconf.syms.get(_bare(name)) or kconf.syms.get(name)
        return bool(sym is not None and sym.nodes)

    unknown = [n for n, _ in out_entries if n and not defined(n)]
    if unknown:
        print("  options without a definition in the target tree "
              "(auto-dropped on apply-def): %d" % len(unknown))
        for n in unknown[:8]:
            print("    - %s" % n)
        if len(unknown) > 8:
            print("    ... %d more" % (len(unknown) - 8))
    else:
        print("  every option is defined in the target tree")

    # Double check: no blacklisted switch may be left on. Only "CONFIG_X=..."
    # lines count, "# CONFIG_X is not set" is the expected result here.
    risky = [n for n, line in out_entries
             if n and RE_CONFIG.match(line) and defined(n)
             and is_blacklisted(n)]
    if risky:
        print("  [warning] framework switches were not removed, "
              "please confirm manually:")
        for n in risky:
            print("    ! %s" % n)


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def find_sdk_root(start):
    """Walk upwards from start until a directory holds SConstruct and kernel/."""
    cur = os.path.abspath(start)
    while True:
        if os.path.isfile(os.path.join(cur, "SConstruct")) \
                and os.path.isdir(os.path.join(cur, "kernel")):
            return cur
        parent = os.path.dirname(cur)
        if parent == cur:
            return None
        cur = parent


def print_report(report):
    """Print the summary of a conversion."""
    if report["kept_off"]:
        print("kept explicitly off (%d, they default to y in the target tree):"
              % len(report["kept_off"]))
        for n in report["kept_off"]:
            print("  - %s" % n)
    if report["dropped"]:
        shown = report["dropped"][:15]
        print("dropped functional switches (%d):" % len(report["dropped"]))
        for n in shown:
            print("  - %s" % n)
        if len(report["dropped"]) > len(shown):
            print("  ... %d more" % (len(report["dropped"]) - len(shown)))
    if report["silent"]:
        print("silently dropped non-functional options "
              "(kernel options / value settings): %d" % report["silent"])
    if report["mapped"]:
        print("mapped:")
        for n in report["mapped"]:
            print("  - %s" % n)
    if report["added"]:
        print("added (%d):" % len(report["added"]))
        for n in report["added"]:
            print("  - %s" % n)
    print()


def main():
    ap = argparse.ArgumentParser(
        description="Convert an RT-Thread defconfig into a FreeRTOS defconfig")
    ap.add_argument("defconfig", help="input: path to an rt-thread defconfig")
    ap.add_argument("-o", "--output",
                    help="output path (defaults to the freertos plan name in "
                         "the same directory)")
    ap.add_argument("-n", "--dry-run", action="store_true",
                    help="preview only, write nothing")
    ap.add_argument("--console-uart", type=int,
                    help="console UART number (default: derived from "
                         "RT_CONSOLE_DEVICE_NAME)")
    ap.add_argument("--sysname", default="aic",
                    help="console prompt, defaults to aic; pass an empty "
                         "string to keep the SDK default tinySPL")
    ap.add_argument("--sdk-root",
                    help="SDK root (default: searched upwards from the output)")
    ap.add_argument("--verify", action="store_true",
                    help="validate against the target Kconfig tree "
                         "(requires kconfiglib)")
    args = ap.parse_args()

    src = os.path.abspath(args.defconfig)
    if not os.path.isfile(src):
        sys.exit("input file not found: %s" % src)

    entries = parse_defconfig(src)
    out_lines, report, new_name = convert(
        entries, console_uart=args.console_uart, sysname=args.sysname)

    if args.output:
        dst = args.output
    else:
        dst = os.path.join(os.path.dirname(src), "%s_defconfig" % new_name)

    print("input  : %s" % os.path.relpath(src))
    print("output : %s" % os.path.relpath(dst))
    print("plan   : %s" % new_name)
    print()
    print_report(report)

    if args.dry_run:
        print("--- preview (nothing written) ---")
        print("\n".join(out_lines))
        return

    with open(dst, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out_lines) + "\n")
    print("written to %s" % dst)

    if args.verify:
        sdk_root = args.sdk_root or find_sdk_root(os.path.dirname(dst))
        if not sdk_root:
            print("  [skipped] SDK root not found (use --sdk-root)")
            return
        verify(sdk_root,
               get_value(entries, "CONFIG_PRJ_CHIP"),
               get_value(entries, "CONFIG_PRJ_BOARD"),
               "freertos",
               get_value(entries, "CONFIG_PRJ_APP"),
               dst)

    print()
    print("next steps:")
    print("  scons --apply-def=%s_defconfig && scons -j8" % new_name)


if __name__ == "__main__":
    main()
