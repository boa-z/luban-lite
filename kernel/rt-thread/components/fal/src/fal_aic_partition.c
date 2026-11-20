/*
 * Copyright (c) 2023-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dehuang Wu <dehuang.wu@artinchip.com>
 */

#include <fal.h>
#include <aic_common.h>
#include <aic_image.h>
#include <private_param.h>
#include <aic_partition.h>
#include <partition_table.h>

#define AIC_NOR_IMAGE_TRY_POSITION_0  0
#define AIC_NOR_IMAGE_TRY_POSITION_1  1
#define AIC_NOR_IMAGE_TRY_POSITION_2  2

#define PRIVATE_TAG_MAGIC_MSK (0xFFFF0000)
#define PRIVATE_TAG_MAGIC_VAL (0x41490000)
#define AIC_PDAT_MAGIC        0x54414450      /* Magic is 'PDAT ' */

struct private_data2_header {
    u32 magic;
    u32 cksum;
    u32 ver;
    u32 sign_offset;
    u8 pad[16];
};

void dump_hex(char *msg, unsigned char *data, int len)
{
    unsigned long i;

    printf("%s, 0x%lx:\n", msg, (unsigned long)data);
    i = 0;
    printf("%04lx: ", i);
    for (i = 0; i < len; i++) {
        if (i && (i % 16 == 0))
            printf("\n%04lx: ", i);
        printf("%02x ", data[i]);
    }
    printf("\n");
}

static u32 calc_private_checksum(u8 *start, u32 size)
{
    u8 *p;
    u32 i, val, sum, rest, cnt;

    p = start;
    i = 0;
    sum = 0;
    cnt = size >> 2;

    for (i = 0; i < cnt; i++) {
        p = &start[i * 4];
        val = (p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0];
        sum += val;
    }

    /* Calculate not 32 bit aligned part */
    rest = size - (cnt << 2);
    p = &start[cnt * 4];
    val = 0;
    for (i = 0; i < rest; i++)
        val += (p[i] << (i * 8));
    sum += val;

    return sum;
}

static bool private_data_quick_check(u8 *res, u32 rlen)
{
    u32 offset = 0, type, len, sumval;
    struct private_data2_header head;

    memcpy(&head, res, sizeof(head));
    if (head.magic == AIC_PDAT_MAGIC) {
        /* Private data format2 case */
        if (head.ver != 0x00010001) {
            return false;
        }

        if (head.cksum != 0) {
            sumval = calc_private_checksum(res, rlen);
            if (sumval != 0xFFFFFFFF) {
                return false;
            } else {
                return true;
            }
        }
        /* No cksum value, should has secure signature */
        if (head.sign_offset == 0) {
            return false;
        }
        offset = sizeof(head);
    }

    /* Cases:
     *  - private data format1
     *  - private data format2, but use secure signature
     *    don't perform complex secure signature verification, just quick check TLV
     */
    while ((rlen - offset) >= 8) {
        /* no cksum case, only use TLV check */
        type = *(u32 *)(res + offset);
        len = *(u32 *)(res + offset + 4);

        if ((type & PRIVATE_TAG_MAGIC_MSK) != PRIVATE_TAG_MAGIC_VAL) {
            return false;
        }
        if (type == DATA_SECT_TYPE_END) {
            return true;
        }

        if ((len & 3) || (len > (rlen - offset -8))) {
            return false;
        }
        offset += (8 + len);
    }

    return false;
}

static int get_private_data_by_image_id(const struct fal_flash_dev *flash_dev, uint32_t img_id,
                                        uint8_t **private_data,
                                        uint32_t *private_len)
{
    struct aic_image_header head;
    uint8_t head_buf[256];
    uint32_t start = 0;
    int ret = 0;

    *private_data = NULL;
    *private_len = 0;

    switch (img_id) {
        case AIC_NOR_IMAGE_TRY_POSITION_0:
            start = 0;
            break;
        case AIC_NOR_IMAGE_TRY_POSITION_1:
            start = 0x20000; /* 128KB */
            break;
        case AIC_NOR_IMAGE_TRY_POSITION_2:
            start = 0x40000; /* 256KB */
            break;
        default:
            ret = -1;
            goto err;
    }

    if (flash_dev->ops.read(flash_dev, start, head_buf, sizeof(head_buf)) <= 0) {
        log_e("Failed to read aic image head.");
        ret = -1;
        goto err;
    }
    memcpy(&head, head_buf, sizeof(head));
    if (head.magic != AIC_IMAGE_MAGIC) {
        log_e("aic image head verify failure.");
        ret = -1;
        goto err;
    }

    if ((head.private_data_offset == 0) || (head.private_data_len == 0)) {
        ret = -1;
        goto err;
    }

    *private_data = malloc(head.private_data_len);
    if (*private_data == NULL) {
        log_e("Failed to malloc resource buffer.");
        ret = -RT_ENOMEM;
        goto err;
    }

    if (flash_dev->ops.read(flash_dev, start + head.private_data_offset, *private_data,
                            head.private_data_len) <= 0) {
        log_e("Failed to read private data from flash.");
        ret = -1;
        goto err;
    }

    if (*private_data && (private_data_quick_check(*private_data, head.private_data_len) == false)) {
        ret = -1;
        goto err;
    }
    *private_len = head.private_data_len;

    return 0;
err:
    if (*private_data) {
        free(*private_data);
        *private_data = NULL;
        *private_len = 0;
    }
    return ret;
}

/**
 * @brief Read private data from flash device.
 *
 * @param flash_dev Pointer to FAL flash device.
 * @param private_data Output buffer for private data (will be allocated).
 * @param private_len Output parameter for private data length.
 * @return 0 on success, -1 on failure.
 */
static int get_private_data_from_flash(const struct fal_flash_dev *flash_dev,
                                       uint8_t **private_data,
                                       uint32_t *private_len)
{
    int i, ret;

    for (i = 0; i <= AIC_NOR_IMAGE_TRY_POSITION_2; i++) {
        ret = get_private_data_by_image_id(flash_dev, i, private_data, private_len);
        if (ret == 0)
            return ret;
    }
    return -1;
}

/**
 * @brief Extract partition string from private data.
 *
 * @param private_data Pointer to private data buffer.
 * @return Pointer to partition string (either from private data or default macro).
 */
static char *get_partition_string_from_private_data(uint8_t *private_data)
{
    char *part_str;

    part_str = private_get_partition_string(private_data);
    if (part_str == NULL) {
        part_str = IMAGE_CFG_JSON_PARTS_MTD;
    } else if (strncmp(part_str, "mtd=", 4) == 0) {
        /* Private data stores partition strings in key=value format,
         * e.g. "mtd=spi0.0:...;spi2.0:...;ubi=...;".
         * Extract only the MTD value (after "mtd="). The MTD value
         * itself may contain ';' to separate multiple flash devices.
         * We identify the boundary by checking if the segment after
         * ';' starts with "spi" (flash device name prefix). */
        char *p = part_str + 4; /* Skip "mtd=" */

        while (*p) {
            if (*p == ';') {
                char *next = p + 1;

                /* If next segment starts with "spi<digit>", it is
                 * another flash partition still within MTD value.
                 * Otherwise it is a new key (e.g. "ubi="). */
                if (!(next[0] == 's' && next[1] == 'p' &&
                      next[2] == 'i' && next[3] >= '0' &&
                      next[3] <= '9')) {
                    *p = '\0';
                    break;
                }
            }
            p++;
        }
        return part_str + 4;
    }

    return part_str;
}

/**
 * @brief Extract flash index from device name like "spi0.0".
 *
 * @param spi_dev_name SPI device name (e.g., "spi0.0", "spi2.0").
 * @return Flash index number, -1 on parsing failure.
 */
static int extract_flash_index_from_spi_name(const char *spi_dev_name)
{
    int flash_idx = -1;

    if (strncmp(spi_dev_name, "spi", 3) != 0) {
        return -1;
    }

    flash_idx = atoi(spi_dev_name + 3);
    return flash_idx;
}

/**
 * @brief Build FAL flash device name from SPI name.
 *
 * @param spi_dev_name SPI device name (e.g., "spi0.0").
 * @param fal_dev_name Output buffer for FAL device name.
 * @param name_size Size of output buffer.
 * @return 0 on success, -1 on failure.
 */
static int build_fal_flash_name(const char *spi_dev_name, char *fal_dev_name,
                                size_t name_size)
{
    int flash_idx;

    flash_idx = extract_flash_index_from_spi_name(spi_dev_name);
    if (flash_idx < 0) {
        log_e("Invalid SPI device name: %s", spi_dev_name);
        return -1;
    }

    rt_snprintf(fal_dev_name, name_size, "norflash%d", flash_idx);
    return 0;
}

/**
 * @brief Parse partitions for single flash device and add to table.
 *
 * @param part_str Partition string for one flash device.
 * @param spi_dev_name SPI device name for this partition group.
 * @param fal_dev_name FAL flash device name.
 * @param table Output pointer to FAL partition table array.
 * @param base_index Starting index in the table for this flash.
 * @return Number of partitions parsed for this flash.
 */
static int parse_single_flash_partitions(char *part_str, const char *spi_dev_name,
                                         const char *fal_dev_name,
                                         struct fal_partition **table,
                                         int base_index)
{
    struct aic_partition *list, *part;
    struct fal_partition *fal;
    int cnt = 0;

    list = aic_part_mtd_parse((void *)part_str);
    if (list == NULL) {
        log_e("Failed to parse partition string for %s.", spi_dev_name);
        return 0;
    }

    part = list;
    while (part) {
        part = part->next;
        cnt++;
    }

    fal = *table;
    part = list;
    for (int i = 0; i < cnt; i++) {
        fal[base_index + i].magic_word = FAL_PART_MAGIC_WORD;
        rt_strncpy(fal[base_index + i].name, part->name, FAL_DEV_NAME_MAX - 1);
        fal[base_index + i].name[FAL_DEV_NAME_MAX - 1] = '\0';
        rt_strncpy(fal[base_index + i].flash_name, fal_dev_name, FAL_DEV_NAME_MAX - 1);
        fal[base_index + i].flash_name[FAL_DEV_NAME_MAX - 1] = '\0';
        fal[base_index + i].offset = part->start;
        fal[base_index + i].len = part->size;
        printf("part: %s (%s) -> %s, offset=0x%lx, size=0x%lx\n",
               spi_dev_name, fal_dev_name, part->name,
               (unsigned long)part->start, (unsigned long)part->size);
        part = part->next;
    }

    aic_part_free(list);
    return cnt;
}

/**
 * @brief Parse partition string and build FAL partition table for multi-flash.
 *
 * @param part_str Partition string to parse (may contain multiple flash devices).
 * @param table Output pointer to FAL partition table array.
 * @return Total number of partitions parsed, 0 on failure.
 */
static int get_fal_partition_list(char *part_str,
                                  struct fal_partition **table)
{
    char *flash_start, *flash_end, *dup_str;
    int total_cnt = 0;
    int current_idx = 0;

    dup_str = rt_strdup(part_str);
    if (dup_str == NULL) {
        log_e("Failed to duplicate partition string.");
        return 0;
    }

    flash_start = dup_str;

    while (flash_start && *flash_start != '\0') {
        char *colon_pos;
        char *flash_dev_name;
        char fal_dev_name[16];
        int flash_part_cnt;

        /* Find the colon separating device name from partitions */
        colon_pos = strchr(flash_start, ':');
        if (colon_pos == NULL) {
            log_e("Invalid partition format, missing ':': %s", flash_start);
            break;
        }

        /* Extract flash device name (e.g., "spi0.0") */
        flash_dev_name = flash_start;
        *colon_pos = '\0';

        /* Build FAL flash device name (e.g., "norflash0") */
        if (build_fal_flash_name(flash_dev_name, fal_dev_name,
                                 sizeof(fal_dev_name)) != 0) {
            log_e("Failed to build FAL name for %s", flash_dev_name);
            break;
        }

        /* Find end of this flash's partition list */
        flash_end = strchr(colon_pos + 1, ';');
        if (flash_end != NULL) {
            *flash_end = '\0';
        }

        /* Parse partitions for this single flash device */
        flash_part_cnt = parse_single_flash_partitions(colon_pos + 1,
                                                       flash_dev_name,
                                                       fal_dev_name,
                                                       table,
                                                       current_idx);

        if (flash_part_cnt == 0) {
            log_i("No partitions parsed for %s", flash_dev_name);
        }

        total_cnt += flash_part_cnt;
        current_idx += flash_part_cnt;

        /* Move to next flash device partition string */
        if (flash_end == NULL) {
            break;
        }
        flash_start = flash_end + 1;
    }

    free(dup_str);
    return total_cnt;
}

/**
 * @brief Count total partitions across all flash devices in partition string.
 *
 * @param part_str Partition string (may contain multiple flash devices).
 * @return Total number of partitions, 0 on failure.
 */
static int count_total_partitions(const char *part_str)
{
    int total_partitions = 0;
    char *temp_str = rt_strdup(part_str);
    char *flash_start = temp_str;

    if (temp_str == NULL) {
        log_e("Failed to duplicate partition string.");
        return 0;
    }

    while (flash_start && *flash_start != '\0') {
        char *colon_pos;
        char *flash_end;
        int flash_cnt = 0;

        colon_pos = strchr(flash_start, ':');
        if (colon_pos == NULL) {
            break;
        }

        flash_end = strchr(colon_pos + 1, ';');
        if (flash_end != NULL) {
            *flash_end = '\0';
        }

        /* Count partitions for this flash device */
        struct aic_partition *list = aic_part_mtd_parse(colon_pos + 1);
        struct aic_partition *part = list;
        while (part) {
            part = part->next;
            flash_cnt++;
        }
        aic_part_free(list);

        total_partitions += flash_cnt;

        if (flash_end == NULL) {
            break;
        }
        flash_start = flash_end + 1;
    }

    free(temp_str);
    return total_partitions;
}

/**
 * @brief Get FAL partition table from flash private data.
 *
 * This function reads the image header from flash, extracts the private
 * data containing partition configuration string, and parses it to build
 * a FAL partition table. Supports multi-flash partition strings separated
 * by semicolons (e.g., "spi0.0:512k(spl),...;spi2.0:1m(extra),...").
 *
 * @param dev_name Flash device name to read from (e.g., "norflash0").
 * @param table Output pointer to the allocated FAL partition table array.
 * @return Total number of partitions parsed, 0 on failure, -1 if flash not found.
 */
int aic_get_fal_partition_table(const char *dev_name, struct fal_partition **table)
{
    const struct fal_flash_dev *flash_dev = NULL;
    uint8_t *private_data = NULL;
    uint32_t private_len = 0;
    char *part_str;
    int total_partitions = 0;
    struct fal_partition *fal_table = NULL;

    flash_dev = fal_flash_device_find(dev_name);
    if (flash_dev == NULL) {
        log_e("Initialize failed! Flash device (%s) NOT found.", dev_name);
        return -1;
    }

    /* Step 1: Get private data from flash */
    if (get_private_data_from_flash(flash_dev, &private_data, &private_len) != 0) {
        return 0;
    }

    /* Step 2: Get partition string from private data */
    part_str = get_partition_string_from_private_data(private_data);

    /* Step 3: Count total partitions across all flash devices */
    total_partitions = count_total_partitions(part_str);

    if (total_partitions == 0) {
        log_e("No partitions found in partition string.");
        free(private_data);
        return 0;
    }

    /* Allocate FAL partition table for all partitions */
    fal_table = malloc(sizeof(struct fal_partition) * total_partitions);
    if (fal_table == NULL) {
        log_e("Failed to malloc FAL partition table.");
        free(private_data);
        return 0;
    }
    *table = fal_table;

    /* Step 4: Second pass - parse and fill partition table */
    total_partitions = get_fal_partition_list(part_str, &fal_table);
    if (total_partitions == 0) {
        free(fal_table);
        *table = NULL;
    }

    free(private_data);
    return total_partitions;
}
