#!/usr/bin/env python3
# -*- coding:utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2023-2026 ArtInChip Technology Co., Ltd
# Xiong Hao <hao.xiong@artinchip.com>
#
# Tool to calculate bootloader linked addr from .config file
#

import argparse
import os
import sys
from menuconfig import get_config_val, get_text_base

# Gap between the mini and full bootloader in RAM (512 KB)
_UPGRADE_OFFSET = 0x80000


def calc_link_addr(infile, config, outfile, dual=False):
    """Resolve the bootloader address placeholders in image_cfg.json.

    In a dual build the full bootloader is placed _UPGRADE_OFFSET
    below the mini one, which keeps the address from the config.
    """
    if get_config_val(config, 'CONFIG_AIC_BOOTLOADER_MEM_AUTO') == 'y':
        text = hex(get_text_base(config))
    else:
        text = get_config_val(config, 'CONFIG_AIC_BOOTLOADER_TEXT_BASE')
    load = hex(int(text, 16) - 0x100)

    full_text, full_load = text, load
    if dual:
        full_text = hex(int(text, 16) - _UPGRADE_OFFSET)
        full_load = hex(int(load, 16) - _UPGRADE_OFFSET)

    with open(infile, 'r') as f:
        cfg = f.read()

    # Upgrade (full) placeholders first: they are longer names
    for name, value in (
            ('CONFIG_AIC_BOOTLOADER_UPGRADE_TEXT_BASE', full_text),
            ('CONFIG_AIC_BOOTLOADER_UPGRADE_LOAD_BASE', full_load),
            ('CONFIG_AIC_BOOTLOADER_TEXT_BASE', text),
            ('CONFIG_AIC_BOOTLOADER_LOAD_BASE', load)):
        cfg = cfg.replace(name, str(value))

    with open(outfile, 'w') as f:
        f.write(cfg)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
            description='Tool to calculate bootloader linked addr from .config file')
    parser.add_argument('-i', '--infile', type=str, help='input image_cfg.json file')
    parser.add_argument('-c', '--config', type=str, help='bootloader defconfig file')
    parser.add_argument('-o', '--outfile', type=str, help='output image_cfg.jaon file')
    args = parser.parse_args()
    if args.infile is None:
        print('Error, option --infile is required.')
        sys.exit(1)
    if args.config is None:
        print('Error, option --config is required.')
        sys.exit(1)
    if args.outfile is None:
        args.outfile = os.path.join(os.path.dirname(args.infile),
                                    '.' + os.path.basename(args.infile) + '.tmp')

    calc_link_addr(args.infile, args.config, args.outfile)
