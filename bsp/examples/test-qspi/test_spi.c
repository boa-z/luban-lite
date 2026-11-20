/*
 * Copyright (c) 2024-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: Jiji Chen <jiji.chen@artinchip.com>
 */

#include <string.h>
#include <finsh.h>
#include <rtdevice.h>
#include <aic_core.h>

#define USAGE \
    "test_spi help : Get this information.\n" \
    "test_spi attach <bus name> <dev name> : Attach device to SPI bus.\n" \
    "test_spi init <name> <mode> <freq> : Initialize SPI for device.\n" \
    "test_spi send <len> : Send data.\n" \
    "example:\n" \
    "test_spi attach spi3 spidev\n" \
    "test_spi init spidev 0 50000000\n" \
    "test_spi send 32\n" \
    "test_spi transfer 32\n" \
    "test_spi send_recv 33 128\n" \


/* Chunked transfer: at most 16K per call to keep the buffer small */
#define SPI_TEST_CHUNK_SIZE   (16 * 1024)

static void spi_usage(void)
{
    printf("%s", USAGE);
}

static void show_speed(char *msg, u32 len, u32 us)
{
    u32 tmp, speed;

    /* Split to serval step to avoid overflow */
    tmp = 1000 * len;
    tmp = tmp / us;
    tmp = 1000 * tmp;
    speed = tmp / 1024;

    printf("%s: %d byte, %d us -> %d KB/s\n", msg, len, us, speed);
}

static void hex_dump(uint8_t *data, unsigned long len)
{
    unsigned long i = 0;
    printf("\n");
    for (i = 0; i < len; i++) {
        if (i && (i % 16) == 0)
            printf("\n");
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static struct rt_spi_device *g_spi;


static int test_spi_attach(int argc, char **argv)
{
    struct rt_spi_device *spi_device = RT_NULL;
    char *bus_name, *dev_name;
    rt_err_t result = RT_EOK;

    if (argc != 3) {
        spi_usage();
        return -1;
    }
    bus_name = argv[1];
    dev_name = argv[2];

    RT_ASSERT(bus_name != RT_NULL);
    RT_ASSERT(dev_name != RT_NULL);

    spi_device =
        (struct rt_spi_device *)rt_malloc(sizeof(struct rt_spi_device));
    if (spi_device == RT_NULL) {
        printf("malloc failed.\n");
        return -RT_ERROR;
    }

    result = rt_spi_bus_attach_device(spi_device, dev_name,
                                      bus_name, RT_NULL);

    if (result != RT_EOK && spi_device != NULL)
        rt_free(spi_device);

    return result;
}

#define AIC_SPI_MODE_MASK    (RT_SPI_CPHA | RT_SPI_CPOL | RT_SPI_MSB | RT_SPI_SLAVE | RT_SPI_3WIRE)

static int test_spi_init(int argc, char **argv)
{
    struct rt_spi_configuration spi_cfg;
    struct rt_device *dev;
    char *name;
    int ret = 0;

    if (argc != 4) {
        printf("Argument error, please see help information.\n");
        return -1;
    }
    name = argv[1];

    g_spi = (struct rt_spi_device *)rt_device_find(name);
    if (!g_spi) {
        printf("Failed to get device in name %s\n", name);
        return -1;
    }

    dev = (struct rt_device *)g_spi;
    if (dev->type != RT_Device_Class_SPIDevice) {
        g_spi = NULL;
        printf("%s is not SPI device.\n", name);
        return -1;
    }
    rt_memset(&spi_cfg, 0, sizeof(spi_cfg));
    spi_cfg.mode = atol(argv[2]);
    spi_cfg.mode &= AIC_SPI_MODE_MASK;
    spi_cfg.max_hz = atol(argv[3]);
    ret = rt_spi_configure(g_spi, &spi_cfg);
    if (ret < 0) {
        printf("qspi configure failure.\n");
        return ret;
    }
    return 0;
}

static void test_spi_send_recv(int argc, char **argv)
{
    unsigned long  send_len, recv_len, chunk, offset, start_us;
    uint8_t *send_buf, *recv_buf;
    rt_size_t ret;
    int k;

    if (!g_spi) {
        printf("SPI device is not init yet.\n");
        return;
    }
    if (argc < 3) {
        printf("Argument is not correct, please see help for more information.\n");
        return;
    }

    send_len = strtoul(argv[1], NULL, 0);
    recv_len = strtoul(argv[2], NULL, 0);

    /* transfer len can not be 0 */
    if (!send_len || !recv_len)
        return;

    /* Fixed 16K buffer, chunked transfer */
    send_buf = aicos_malloc_align(0, SPI_TEST_CHUNK_SIZE, CACHE_LINE_SIZE);
    recv_buf = aicos_malloc_align(0, SPI_TEST_CHUNK_SIZE, CACHE_LINE_SIZE);
    if (send_buf == NULL || recv_buf == NULL) {
        printf("Low memory!\n");
        if (send_buf)
            aicos_free_align(0, send_buf);
        if (recv_buf)
            aicos_free_align(0, recv_buf);
        return;
    }
    for (k = 0; k < SPI_TEST_CHUNK_SIZE; k++)
        send_buf[k] = k & 0xff;

    printf("send len %lu, recv len %lu\n", send_len, recv_len);

    rt_spi_take_bus((struct rt_spi_device *)g_spi);
    start_us = aic_get_time_us();
    /* Send send_len bytes first */
    for (offset = 0; offset < send_len; offset += chunk) {
        chunk = send_len - offset;
        if (chunk > SPI_TEST_CHUNK_SIZE)
            chunk = SPI_TEST_CHUNK_SIZE;
        ret = rt_spi_transfer(g_spi, (void *)send_buf, NULL, chunk);
        if (ret != chunk) {
            printf("Send failed at %lu, ret 0x%x\n", offset, (int)ret);
            break;
        }
    }
    /* Then receive recv_len bytes */
    for (offset = 0; offset < recv_len; offset += chunk) {
        chunk = recv_len - offset;
        if (chunk > SPI_TEST_CHUNK_SIZE)
            chunk = SPI_TEST_CHUNK_SIZE;
        rt_memset(recv_buf, 0xee, chunk);
        ret = rt_spi_transfer(g_spi, NULL, (void *)recv_buf, chunk);
        if (ret != chunk) {
            printf("Recv failed at %lu, ret 0x%x\n", offset, (int)ret);
            break;
        }
    }
    show_speed("spi send recv speed", send_len + recv_len, aic_get_time_us() - start_us);
    rt_spi_release_bus((struct rt_spi_device *)g_spi);
    aicos_free_align(0, send_buf);

    /* Only dump the first 64 bytes of the last chunk, avoid dumping everything */
    printf("receive data (last chunk):\n");
    hex_dump(recv_buf, (recv_len < 64) ? recv_len : 64);
    printf("\n");
    aicos_free_align(0, recv_buf);
}

static void test_spi_send(int argc, char **argv)
{
    unsigned long  data_len, chunk, offset, start_us;
    uint8_t *data;
    rt_size_t ret;
    int k;

    if (!g_spi) {
        printf("SPI device is not init yet.\n");
        return;
    }
    if (argc < 2) {
        printf("Argument is not correct, please see help for more information.\n");
        return;
    }

    data_len = strtoul(argv[1], NULL, 0);
    if (!data_len)
        return;

    /* Fixed 16K buffer, chunked transfer */
    data = aicos_malloc_align(0, SPI_TEST_CHUNK_SIZE, CACHE_LINE_SIZE);
    if (data == NULL) {
        printf("Low memory!\n");
        return;
    }
    for (k = 0; k < SPI_TEST_CHUNK_SIZE; k++)
        data[k] = k & 0xff;

    printf("data len %lu\n", data_len);

    rt_spi_take_bus((struct rt_spi_device *)g_spi);
    start_us = aic_get_time_us();
    for (offset = 0; offset < data_len; offset += chunk) {
        chunk = data_len - offset;
        if (chunk > SPI_TEST_CHUNK_SIZE)
            chunk = SPI_TEST_CHUNK_SIZE;
        ret = rt_spi_transfer(g_spi, (void *)data, NULL, chunk);
        if (ret != chunk) {
            printf("Send failed at %lu, ret 0x%x\n", offset, (int)ret);
            break;
        }
    }
    show_speed("spi send speed", data_len, aic_get_time_us() - start_us);
    rt_spi_release_bus((struct rt_spi_device *)g_spi);
    aicos_free_align(0, data);
}

static void test_spi_treansfer(int argc, char **argv)
{
    unsigned long  data_len, chunk, offset, start_us;
    uint8_t *data, *recv;
    rt_size_t ret;
    int k;

    if (!g_spi) {
        printf("SPI device is not init yet.\n");
        return;
    }
    if (argc < 2) {
        printf("Argument is not correct, please see help for more information.\n");
        return;
    }

    data_len = strtoul(argv[1], NULL, 0);
    if (!data_len)
        return;

    /* Fixed 16K buffer, chunked transfer */
    data = aicos_malloc_align(0, SPI_TEST_CHUNK_SIZE, CACHE_LINE_SIZE);
    recv = aicos_malloc_align(0, SPI_TEST_CHUNK_SIZE, CACHE_LINE_SIZE);
    if (data == NULL || recv == NULL) {
        printf("Low memory!\n");
        if (data)
            aicos_free_align(0, data);
        if (recv)
            aicos_free_align(0, recv);
        return;
    }
    for (k = 0; k < SPI_TEST_CHUNK_SIZE; k++)
        data[k] = k & 0xff;

    printf("data len %lu\n", data_len);

    rt_spi_take_bus((struct rt_spi_device *)g_spi);
    start_us = aic_get_time_us();
    for (offset = 0; offset < data_len; offset += chunk) {
        chunk = data_len - offset;
        if (chunk > SPI_TEST_CHUNK_SIZE)
            chunk = SPI_TEST_CHUNK_SIZE;
        rt_memset(recv, 0xee, chunk);
        ret = rt_spi_transfer(g_spi, (void *)data, (void *)recv, chunk);
        if (ret != chunk) {
            printf("Transfer failed at %lu, ret 0x%x\n", offset, (int)ret);
            break;
        }
    }
    show_speed("spi transfer speed", data_len, aic_get_time_us() - start_us);
    rt_spi_release_bus((struct rt_spi_device *)g_spi);
    aicos_free_align(0, data);

    /* Only dump the first 64 bytes of the last chunk, avoid dumping everything */
    printf("receive data (last chunk):\n");
    hex_dump(recv, (data_len < 64) ? data_len : 64);
    printf("\n");
    aicos_free_align(0, recv);
}

static void cmd_test_spi(int argc, char **argv)
{
    if (argc < 2)
        goto help;

    if (!rt_strcmp(argv[1], "help")) {
        goto help;
    } else if (!rt_strcmp(argv[1], "attach")) {
        test_spi_attach(argc - 1, &argv[1]);
        return;
    } else if (!rt_strcmp(argv[1], "init")) {
        test_spi_init(argc - 1, &argv[1]);
        return;
    } else if (!rt_strcmp(argv[1], "send_recv")) {
        test_spi_send_recv(argc - 1, &argv[1]);
        return;
    } else if (!rt_strcmp(argv[1], "send")) {
        test_spi_send(argc - 1, &argv[1]);
        return;
    } else if (!rt_strcmp(argv[1], "transfer")) {
        test_spi_treansfer(argc - 1, &argv[1]);
        return;
    }

help:
    spi_usage();
}

MSH_CMD_EXPORT_ALIAS(cmd_test_spi, test_spi, Test SPI);
