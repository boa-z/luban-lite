#!/usr/bin/env python3
# coding=utf-8

# SPDX-License-Identifier: Apache-2.0
#
# Copyright (C) 2026, ArtInChip Technology Co., Ltd
# Author: Matteo <duanmt@artinchip.com>

# Check partition space occupancy after image packing, and warn early before
# a partition really overflows its size.
#
# Measurement rules (why not simply "file size vs part size"):
#   - *.jffs2     : mkfs.jffs2 pads the image to the erase block with 0xFF and
#                   stamps every block (empty ones too) with a leading compat /
#                   cleanmarker node, so "last non-0xFF byte" is pinned to the
#                   final padding block and says nothing about the content. The
#                   effective content is the end of the last erase block that
#                   holds any node *after* that per-block leading node.
#   - *.ubifs / UBI volumes : 'part' is 'container:volume'. The capacity is
#                   taken from the volume's own fixed size when it has one
#                   (e.g. "ubisystem:ota" -> volume ota = 32m even though the
#                   ubisystem container is "-"), otherwise from the fixed
#                   container (e.g. "ubiroot:rootfs" -> ubiroot = 32m, the
#                   auto-sized volume fills it). mkfs.ubifs emits only real
#                   content, so the .ubifs length is the effective usage. Only
#                   when BOTH the volume and the container are "-" (truly
#                   dynamic, e.g. "ubisystem:user") is it skipped.
#   - *.aic       : the packed artifact, not the source blob, is what occupies
#                   the partition. mk_image.py's pre-processor compresses the
#                   .bin into .aic before packing, and comp['file'] points at
#                   that .aic, so its on-disk length already equals the real
#                   occupancy. It is measured like any other file (measuring
#                   the source .bin instead would give false alarms).
#   - others      : the file is packed as-is, file size is authoritative.
#                   Note about ext2/ext4 rootfs: the file *is* the filesystem
#                   whose size is set by BR2_TARGET_ROOTFS_EXT4_SIZE, so the
#                   percentage below is 'how much of the partition the fs image
#                   takes', not 'how much of the fs is filled'; reading the
#                   free blocks inside would need dumpe2fs on the build host
#                   only, which is why mk_image.py stays on the portable
#                   measurements above.
#   - "size": "-"   : an auto-sized partition (e.g. a squashfs app over the
#                   leftover NOR flash) has no capacity in the cfg, but
#                   mk_image.py resolves the concrete leftover into
#                   comp['part_size'] right before packing, so it is still
#                   measured against that. A dynamic UBI volume is excluded:
#                   it shares its container with sibling volumes, so measuring
#                   it would double-count and it stays '-'.
#
# This script is warning-only by default: it never fails a build unless
# --strict is given AND some partition is really over its size.
#
# report() is also imported by mk_image.py, which prints the very same table
# at the end of packing, so the standalone run and the build log always agree.

import sys
import os
import re
import json
import struct
import argparse

if os.environ.get('LUBAN_PRJ_TOP_DIR'):
    LUBAN_TOPDIR = os.environ['LUBAN_PRJ_TOP_DIR']
else:
    LUBAN_TOPDIR = os.getcwd() + '/'

VERBOSE = False

# Default threshold above which a partition is flagged as 'nearly full', used
# both by the standalone run and by mk_image.py's end-of-packing table.
WARN_PERCENT = 90

COLOR_BEGIN = "\033["
COLOR_RED = COLOR_BEGIN + "41;37m"
COLOR_YELLOW = COLOR_BEGIN + "43;30m"
COLOR_WHITE = COLOR_BEGIN + "47;30m"
COLOR_END = "\033[0m"


def print_info(msg):
    print(COLOR_WHITE + msg + COLOR_END)


def print_warn(msg):
    print(COLOR_YELLOW + msg + COLOR_END)


def print_error(msg):
    print(COLOR_RED + msg + COLOR_END)


def dbg(msg):
    if VERBOSE:
        print_info(msg)


def load_cfg(path):
    """Load image_cfg.json which allows '//' comments and trailing commas."""
    with open(path, 'r') as f:
        raw = f.read()

    cleaned = []
    for line in raw.splitlines():
        out, in_str, esc = [], False, False
        i = 0
        while i < len(line):
            c = line[i]
            if in_str:
                out.append(c)
                if esc:
                    esc = False
                elif c == '\\':
                    esc = True
                elif c == '"':
                    in_str = False
            else:
                if c == '"':
                    in_str = True
                    out.append(c)
                elif c == '/' and i + 1 < len(line) and line[i + 1] == '/':
                    break
                else:
                    out.append(c)
            i += 1
        cleaned.append(''.join(out))

    # Dropping a commented-out entry often leaves a dangling comma behind
    text = re.sub(r',\s*([}\]])', r'\1', '\n'.join(cleaned))
    return json.loads(text)


def parse_size(text):
    """'9m' / '512k' / '768K' / '128m' / 4096 -> bytes, '-' or None -> None."""
    if text is None:
        return None
    if isinstance(text, int):
        return text
    text = str(text).strip().lower()
    if text in ('-', '', 'auto'):
        return None
    mult = 1
    if text.endswith('k'):
        mult, text = 1024, text[:-1]
    elif text.endswith('m'):
        mult, text = 1024 * 1024, text[:-1]
    elif text.endswith('g'):
        mult, text = 1024 * 1024 * 1024, text[:-1]
    try:
        return int(float(text) * mult)
    except ValueError:
        dbg("Unrecognized size: %s" % text)
        return None


def _jffs2_detect_erasesize(buf):
    """Guess the erase block size of a jffs2 image.

    mkfs.jffs2 stamps every erase block with a node header at its start, so
    the true size is the smallest standard block size that divides the image
    and leaves a jffs2 magic (0x1985, stored little-endian as 85 19) at the
    beginning of *every* block.
    """
    n = len(buf)
    for size in (0x1000, 0x2000, 0x4000, 0x8000, 0x10000, 0x20000, 0x40000,
                 0x80000, 0x100000, 0x200000, 0x400000, 0x800000):
        if size > n or n % size:
            continue
        if all(buf[b:b + 2] == b'\x85\x19' for b in range(0, n, size)):
            return size
    return None


def jffs2_content_size(path):
    """End offset of the real jffs2 content, erase-block aware.

    mkfs.jffs2 writes a small compat/cleanmarker node at the start of every
    erase block, including the 0xFF padding blocks at the tail of the image,
    so the naive 'last non-0xFF byte' always lands in the final padding block
    (a constant) no matter how much data the fs really holds. What is counted
    here instead: the last erase block that contains any node *beyond* its
    leading one, and the end of data inside that block.
    """
    with open(path, 'rb') as f:
        buf = f.read()
    total = len(buf)
    erasesize = _jffs2_detect_erasesize(buf)
    if erasesize is None:
        # Unrecognized layout: fall back to the naive measurement
        while total > 0 and buf[total - 1] == 0xFF:
            total -= 1
        return total

    def last_used(seg):
        end = len(seg)
        while end > 0 and seg[end - 1] == 0xFF:
            end -= 1
        return end

    content_end = 0
    for block in range(0, len(buf), erasesize):
        head = block
        # Skip this block's leading node (compat feature / cleanmarker)
        if head + 8 <= len(buf):
            magic, _ntype, totlen = struct.unpack_from('<HHI', buf, head)
            if magic == 0x1985 and 0 < totlen <= erasesize:
                head += (totlen + 3) & ~3
        if last_used(buf[head:block + erasesize]):
            # Real content beyond the leading node: whole block counts
            content_end = block + last_used(buf[block:block + erasesize])
    return content_end


def usage_tag(path):
    """How the occupancy of a packed file is measured."""
    return 'content' if path.lower().endswith('.jffs2') else 'file'


def effective_usage(path, size=None):
    """Effective bytes a packed file takes in its partition.

    mkfs.jffs2 pads the image with 0xFF up to the erase block and stamps even
    the padding blocks with a leading node, so a jffs2 file is always as large
    as its partition would allow and the naive tail scan is useless: for those
    the effective usage is the end of the last erase block that carries real
    nodes. Everything else (including .aic, which is already compressed by the
    pre-processor) is written as-is, so the file size is authoritative.
    """
    if path.lower().endswith('.jffs2'):
        return jffs2_content_size(path)
    if size is None:
        size = os.path.getsize(path)
    return size


def measure(path):
    return effective_usage(path), usage_tag(path)


def component_usage(comp, partitions, dirpath):
    """Measure one image.target.* component against its partition(s).

    Shared with mk_image.py's 'Meta data for image components' table, so the
    packing log and this table always compute the same numbers.
    Returns (part_names, used, part_size, tag), or None when the component is
    not measurable. '.aic' blobs ARE measurable: comp['file'] is the
    already-compressed packed artifact, so its on-disk length equals the
    partition occupancy. UBI components ARE too: 'part' is 'container:volume',
    and the capacity is the volume's own fixed size if it has one, otherwise
    the container's fixed size (an auto-sized volume fills its fixed
    container); mkfs.ubifs output has no padding, so the .ubifs length is the
    effective usage. A plain partition whose cfg size is '-' (auto-extend) is
    measured against comp['part_size'], which mk_image.py resolves to the
    concrete leftover just before packing. Only genuinely unbounded targets --
    a dynamic UBI volume sharing its container, with no fixed size anywhere --
    are skipped.
    """
    fname = str(comp.get('file', ''))
    if not fname:
        return None
    filepath = os.path.join(dirpath, fname)
    if not os.path.isfile(filepath):
        return None

    # A component may be spread over several partitions. UBI components name
    # their target as 'container:volume'; prefer the volume's own fixed size,
    # else fall back to the container's fixed capacity.
    part_size, part_names, dynamic, has_ubi = 0, [], False, False
    for part in comp.get('part', []):
        container, sep, volume = part.partition(':')
        node = partitions.get(container)
        if node is None:
            continue
        if 'ubi' in node:
            has_ubi = True
        size = parse_size(node.get('size'))
        if sep and 'ubi' in node:
            vol_size = parse_size(node['ubi'].get(volume, {}).get('size'))
            if vol_size is not None:
                size = vol_size
        if size is None:
            # Both volume and container are dynamic: no capacity to compare
            dynamic = True
            break
        part_size += size
        part_names.append(part)

    if dynamic or not part_size:
        # The cfg leaves this capacity as '-' (auto-extend). mk_image.py
        # resolves the real capacity into comp['part_size'] right before
        # packing, so for a plain (non-UBI) partition that is an exclusive
        # leftover region we can still measure the packed file against it
        # (e.g. a squashfs app filling the remaining NOR flash). A dynamic UBI
        # volume is NOT eligible: its resolved size is the whole shared
        # container, which would double-count against sibling volumes.
        if not has_ubi:
            resolved = comp.get('part_size')
            if resolved:
                return list(comp.get('part', [])), effective_usage(filepath), \
                    int(resolved), usage_tag(filepath)
        return None

    return part_names, effective_usage(filepath), part_size, usage_tag(filepath)


def report(cfg, dirpath, threshold=WARN_PERCENT):
    """Print the partition occupancy table for an already-packed image cfg.

    Reused by mk_image.py at the end of packing. Never raises and never fails
    a build (warning-only); returns the list of component names whose usage is
    over 100%, so callers that want a hard gate (--strict) can act on it.
    """
    image = cfg.get('image', {})
    media_type = image.get('info', {}).get('media', {}).get('type')
    if not media_type or media_type not in cfg:
        dbg("No media section for type '%s'" % media_type)
        return 0

    partitions = cfg[media_type].get('partitions', {})
    components = image.get('target', {})

    rows, over = [], []
    for name, comp in sorted(components.items()):
        usage = component_usage(comp, partitions, dirpath)
        if usage is None:
            dbg("Skip '%s': not measurable" % name)
            continue
        part_names, used, part_size, how = usage
        percent = used * 100.0 / part_size
        rows.append((name, ','.join(part_names), used, part_size, percent, how))
        if percent > 100.0:
            over.append(name)

    if not rows:
        dbg("Nothing to check")
        return []

    print_info("Partition space occupancy (%s):" % media_type)
    print_info("      %-8s %-12s %10s %10s %8s" %
               ("image", "partition", "used", "part_size", "used%"))
    worst = 0.0
    for name, part, used, part_size, percent, how in \
            sorted(rows, key=lambda x: x[4], reverse=True):
        worst = max(worst, percent)
        line = "      %-8s %-12s %10s %10s %7.1f%% (%s)" % \
               (name, part, used, part_size, percent, how)
        if percent > 100.0:
            print_error(line)
        elif percent > threshold:
            print_warn(line)
        else:
            print_info(line)

    if over:
        print_error("Partition overflow: %s" % ', '.join(over))
    elif worst > threshold:
        print_warn("Partition space is above %d%%, please trim images or "
                   "re-balance the partition table" % threshold)

    return over


def main():
    global VERBOSE

    parser = argparse.ArgumentParser(
        description='Check flash partition space occupancy')
    parser.add_argument('-c', '--config', required=True,
                        help='path of image_cfg.json')
    parser.add_argument('-d', '--dir', required=True,
                        help='directory holding the packed images')
    parser.add_argument('-t', '--threshold', type=int, default=WARN_PERCENT,
                        help='warn when occupancy exceeds this percent (90)')
    parser.add_argument('--strict', action='store_true',
                        help='return non-zero when a partition exceeds 100%%')
    parser.add_argument('-v', '--verbose', action='store_true',
                        help='show verbose output')
    args = parser.parse_args()
    VERBOSE = args.verbose

    if not os.path.isfile(args.config):
        print_error("%s not exist" % args.config)
        return 0
    if not os.path.isdir(args.dir):
        print_error("%s not exist" % args.dir)
        return 0

    try:
        cfg = load_cfg(args.config)
    except ValueError as err:
        print_error("Parse %s failed: %s" % (args.config, err))
        return 0

    over = report(cfg, args.dir, args.threshold)
    if over and args.strict:
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
