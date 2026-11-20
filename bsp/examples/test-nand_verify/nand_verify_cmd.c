/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * NAND verification test command-line entry
 *
 * nand_verify [-b N] [-n N] [dev]   cycle test: write -> read+verify -> erase + OOB
 * nand_verify_phase_scan [dev]      QSPI phase scan
 */

#include <stdlib.h>
#include <rtthread.h>
#include <finsh.h>
#include <string.h>
#include "nand_verify_core.h"

static void print_usage(void)
{
    rt_kprintf("Usage: nand_verify [-b <block>] [-n <count>] [dev]\n");
    rt_kprintf("Options:\n");
    rt_kprintf("  -b <block>  specify test block (bypass auto detection)\n");
    rt_kprintf("  -n <count>  speed samples per item (default 5)\n");
    rt_kprintf("  -h          help\n");
    rt_kprintf("\nExamples:\n");
    rt_kprintf("  nand_verify                   cycle test (write+read verify+erase+OOB)\n");
    rt_kprintf("  nand_verify -n 10             sample each item 10 times\n");
    rt_kprintf("  nand_verify -b 5 data         specify block 5\n");
    rt_kprintf("  nand_verify_phase_scan data   phase scan\n");
}

static void nand_verify(int argc, char *argv[])
{
    const char *dev = "data";
    int block = -1, iterations = 5;
    int i = 1;

    while (i < argc) {
        if (argv[i][0] != '-')
            break;
        if (!strncmp(argv[i], "-h", sizeof("-h"))) {
            print_usage();
            return;
        } else if (!strncmp(argv[i], "-b", sizeof("-b")) && (i + 1) < argc) {
            block = atoi(argv[++i]);
        } else if (!strncmp(argv[i], "-n", sizeof("-n")) && (i + 1) < argc) {
            iterations = atoi(argv[++i]);
            if (iterations < 1) {
                rt_kprintf("invalid iteration count: %s\n", argv[i]);
                print_usage();
                return;
            }
        } else {
            rt_kprintf("unknown option: %s\n", argv[i]);
            print_usage();
            return;
        }
        i++;
    }

    if (i < argc)
        dev = argv[i];

    if (nand_verify_init(dev))
        return;

    if (iterations != 5)
        nand_verify_set_iterations(iterations);

    if (block >= 0) {
        if (nand_verify_set_block(block)) {
            nand_verify_deinit();
            return;
        }
    }

    rt_kprintf("\n========== NAND Verify ==========\n");
    rt_kprintf("Device: %s\n", nand_verify_dev_name());
    rt_kprintf("Page: %u B, Block: %u KB, Total: %u KB\n",
        nand_verify_page_size(), nand_verify_block_size() / 1024,
        nand_verify_capacity() / 1024);
    rt_kprintf("Samples: %d\n", iterations);
    rt_kprintf("==================================\n\n");

    /* Cycle test: three granularities */
    rt_kprintf("--- Cycle Tests ---\n");
    if (nand_verify_cycle()) {
        nand_verify_deinit();
        return;
    }
    rt_kprintf("\n");

    /* OOB verification */
    rt_kprintf("--- OOB Verify ---\n");
    nand_verify_oob();
    rt_kprintf("\n");

    nand_verify_deinit();
    rt_kprintf("test completed\n");
}
MSH_CMD_EXPORT(nand_verify, NAND medium verification: [-b N] [-n N] [dev]);

static void nand_verify_phase_scan_cmd(int argc, char *argv[])
{
    const char *dev = "data";

    if (argc >= 2)
        dev = argv[1];

    if (nand_verify_init(dev)) return;
    nand_verify_phase_scan();
    nand_verify_deinit();
}
MSH_CMD_EXPORT_ALIAS(nand_verify_phase_scan_cmd, nand_verify_phase_scan, NAND phase scan: [dev]);
