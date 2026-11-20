#!/usr/bin/env python3
# -*- coding:utf-8 -*-
# SPDX-License-Identifier: Apache-2.0
# Copyright (C) 2023-2026 ArtInChip Technology Co., Ltd
#
# Build the MINI bootloader variant as a post-build step of the main
# (full) bootloader build, invoked from POST_ACTION.  The sub-build
# is marked with AIC_BUILDING_MINI_SPL=1 and writes straight to
# output/{base}/mini/, since its output directory comes from the temp
# defconfig name.  The parent build files are restored afterwards.
#

import argparse
import os
import shutil
import subprocess
import sys


def run(cmd_list):
    """Run a command line, quoting any argument that contains spaces."""
    cmd = ' '.join('"%s"' % c if ' ' in c else c for c in cmd_list)
    print('>>> ' + cmd)
    return subprocess.call(cmd, shell=True)


def main():
    parser = argparse.ArgumentParser(
            description='Build the mini bootloader variant')
    parser.add_argument('--root', required=True, help='SDK root directory')
    parser.add_argument('--base', required=True,
                        help='project name, e.g. d12p_demo88-nor_baremetal_bootloader')
    parser.add_argument('--mini-def', required=True,
                        help='absolute path of the temp mini defconfig')
    parser.add_argument('--scons', default='scons',
                        help='scons command of the outer build')
    args = parser.parse_args()

    root = os.path.abspath(args.root)
    base = args.base
    chip = base.split('_')[0]
    board = base[len(chip) + 1:].replace('_baremetal_bootloader', '')
    pack_dir = os.path.join(root, 'target', chip, board, 'pack')

    mini_dir = os.path.join(root, 'output', base, 'mini')

    # Files the sub-build overwrites, saved and restored so that the
    # project state stays exactly as the main build left it
    state_files = ('.config', '.defconfig', 'rtconfig.h', 'cconfig.h')
    saved_state = {}
    for name in state_files:
        path = os.path.join(root, name)
        if os.path.exists(path):
            with open(path, 'rb') as f:
                saved_state[name] = f.read()

    # Marks the sub-build, see patch_mini_bootloader()
    os.environ['AIC_BUILDING_MINI_SPL'] = '1'

    try:
        if run([args.scons, '--apply-def=' + args.mini_def, '-C', root]):
            print('*** Error: load mini defconfig failed')
            return 1
        if run([args.scons, '-j8', '-C', root]):
            print('*** Error: mini bootloader build failed')
            return 1

        mini_bin = os.path.join(mini_dir, 'images', chip + '.bin')
        if not os.path.isfile(mini_bin):
            print('*** Error: mini bootloader image %s not found' % mini_bin)
            return 1

        # Keep the sub-build configuration next to its output
        for name in ('.config', 'rtconfig.h', 'cconfig.h'):
            src = os.path.join(root, name)
            if os.path.exists(src):
                shutil.copyfile(src, os.path.join(mini_dir, name))

        if not os.path.isdir(pack_dir):
            os.makedirs(pack_dir)
        shutil.copyfile(mini_bin, os.path.join(pack_dir, 'bootloader.bin'))
        print('Copied mini bootloader to %s/bootloader.bin' % pack_dir)

        size_tool = os.path.join(root, 'toolchain', 'bin',
                                 'riscv64-unknown-elf-size')
        run([size_tool, os.path.join(mini_dir, 'images', chip + '.elf')])
    finally:
        for name, data in saved_state.items():
            with open(os.path.join(root, name), 'wb') as f:
                f.write(data)
        if os.path.exists(args.mini_def):
            os.remove(args.mini_def)

    return 0


if __name__ == '__main__':
    sys.exit(main())
