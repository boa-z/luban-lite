/*
 * Copyright (c) 2022-2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Authors: mingfeng.li <mingfeng.li@artinchip.com>
 *
 * Public NFTL API for the prebuilt library.
 *
 */

#ifndef _NFTL_API_H
#define _NFTL_API_H

#include <stdint.h>
#include <stddef.h>

#define NFTL_SECTOR_SIZE 512

/* Public operation results (not the low-level NAND driver's error numbers).
 * Only REFORMAT requests partition recreation, after backup/user approval.
 * Formatting cannot fix hardware failure, insufficient good blocks or OOM.
 * Usage and recovery guidance: doc/nftl_api_result.md in the NFTL source
 * package.
 */
enum nftl_api_result {
    NFTL_API_OK = 0,
    NFTL_API_EINVAL = -1001,
    NFTL_API_ENOMEM = -1002,
    NFTL_API_EIO = -1003,
    NFTL_API_ENOSPC = -1004,
    NFTL_API_EBLOCKED = -1005,
    NFTL_API_ELIMIT = -1006, /* bounded recovery exhausted; reinitialize */
    NFTL_API_EREFORMAT = -1007 /* unrecoverable mapping/GC state */
};

/* Bound one synchronous request.
 *
 * NFTL_API_MAX_SECTORS limits read/write length in 512-byte logical sectors;
 * the default 2048 equals 1 MiB. A larger request is rejected with
 * NFTL_API_EINVAL and must be split by the caller. Discard is exempt: it only
 * updates the RAM mapping and never writes flash, so a whole trim range can be
 * passed in one call (start/len are still range-checked).
 *
 * Driver hooks and callbacks must implement their own timeouts; calls must
 * be serialized.
 */
#define NFTL_API_MAX_SECTORS 2048U

#define NFTL_IO_STATUS_UNLOCK       0 /* Allow all NFTL operations. */
#define NFTL_IO_STATUS_BLOCK_IO     1 /* Block low-level NFTL flash I/O and reject nftl_api accesses. */
#define NFTL_IO_STATUS_BLOCK_API    2 /* Reject nftl_api accesses while leaving low-level I/O gating unchanged. */
#define NFTL_NAND_CFG_MAGIC         0x4e46544c /* 'NFTL' */
#define NFTL_NAND_CFG_VERSION       2
#define NFTL_NAND_CFG_FLAG_CROSSTALK_OOB_MARKER (1U << 0)

//this for nand page
struct nand_page{
    uint16_t page_num;
    uint16_t block_num;
};

// this is for physicals side
struct physical_op_info{
    struct nand_page physical_page;
    uint16_t page_bitmap;
    uint8_t *user_data_addr;
    uint8_t *spare_data_addr;
};

struct nftl_api_nand_t {
    uint16_t page_size; /* The Page size in the flash */
    uint16_t oob_size;  /* Out of bank size */
    uint16_t oob_free;  /* the free area_node in oob that flash driver not use */
    uint16_t plane_num; /* the number of plane in the NAND Flash */

    uint32_t pages_per_block; /* The number of page a block */
    uint16_t block_total;

    /* Only be touched by driver */
    uint32_t block_start; /* The start of available block*/
    uint32_t block_end;   /* The end of available block */
};

struct nftl_api_nand_cfg_t {
    uint16_t version; /* NFTL_NAND_CFG_VERSION */

    /*
     * Number of blocks reserved as free/backup space. Set to 0 to use the
     * legacy default MIN_FREE_BLOCK_NUM.
     */
    uint32_t free_block_reserved;
    uint32_t flags;
    uint32_t reserved[8];
};

struct nftl_api_handler_t {
    void *priv;
    void *priv_mtd;
    struct nftl_api_nand_t *nandt;
};

typedef void (*nftl_api_op_callback_t)(struct nftl_api_handler_t *handler,
                                       int result);

/**
 * @brief Weak debug hook reserved for platform extensions
 *
 * This weak symbol can be overridden by the integration layer when additional
 * debug handling is required.
 *
 * @return int Returns the platform-specific debug result
 */
int weak_debug(void);

/**
 * @brief Initialize an NFTL instance
 *
 * This function allocates and initializes the internal NFTL object, then builds
 * or restores the mapping information for the target NAND device.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param index NFTL instance index used during initialization
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_init(struct nftl_api_handler_t *handler, int index);

/* Both init APIs return enum nftl_api_result; handler->priv must start NULL.
 * On an allocated-init failure, owned memory is released and priv is NULL.
 * EREFORMAT means mapping rebuild/recovery failed: back up the dump before
 * recreating the partition. Other errors must NOT trigger formatting.
 * No automatic formatting is added; existing startup recovery may write flash.
 * Runtime read/write/discard/flush use the same enum. Errors may follow partial
 * progress: failed writes are NOT guaranteed to be uncommitted.
 */

/**
 * @brief Initialize an NFTL instance with extensible NAND configuration
 *
 * This interface keeps struct nftl_api_nand_t ABI-compatible and carries new
 * configuration fields through a versioned wrapper.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param index NFTL instance index used during initialization
 * @param cfg Pointer to versioned NAND configuration
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_init_ex(struct nftl_api_handler_t *handler, int index,
                     const struct nftl_api_nand_cfg_t *cfg);

/**
 * @brief Write logical sectors through NFTL
 *
 * This function writes one or more logical sectors to flash through the NFTL
 * translation layer.
 *
 * Note: When the I/O block status is any non-zero value, this interface
 *       rejects the request without touching flash.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param start_sector Logical start sector number
 * @param len Number of sectors to write
 * @param buffer Source data buffer
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_write(struct nftl_api_handler_t *handler, uint32_t start_sector, uint32_t len, unsigned char *buffer);

/**
 * @brief Discard a logical sector range through NFTL
 *
 * Marks the sectors as no longer needed: NFTL drops the logical -> physical
 * mapping of the affected pages, so GC reclaims their space instead of copying
 * it again. Nothing is erased here.
 *
 * Granularity: the interface takes logical sectors (same unit as read/write)
 * but the mapping table is page based, so only whole pages fully inside the
 * range are discarded - a partial page at either end is silently ignored.
 * Pass a page aligned range (and a length that is a multiple of the page size
 * in sectors) to get exactly what you asked for.
 *
 * Semantics worth knowing:
 *  - flash content is not zeroed, it just becomes undefined; reading a
 *    discarded page returns 0xFF because the page is no longer mapped;
 *  - the change lives in the in-RAM mapping table and is NOT durable: after a
 *    reboot the mapping is rebuilt from flash and the discarded range can
 *    become readable again, so do not rely on discard surviving power loss;
 *  - mark_info->total_receive_discard_sectors counts what was requested and
 *    total_real_discard_sectors what was actually dropped.
 *
 * Note: When the I/O block status is any non-zero value, this interface
 *       rejects the request without touching flash.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param start_sector Logical start sector number
 * @param len Number of sectors to discard
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_discard(struct nftl_api_handler_t *handler, uint32_t start_sector, uint32_t len);

/**
 * @brief Read logical sectors through NFTL
 *
 * This function reads one or more logical sectors from flash through the NFTL
 * translation layer.
 *
 * Note: When the I/O block status is any non-zero value, this interface
 *       rejects the request without touching flash.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param start_sector Logical start sector number
 * @param len Number of sectors to read
 * @param buffer Destination data buffer
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_read(struct nftl_api_handler_t *handler, uint32_t start_sector, uint32_t len, unsigned char *buffer);

/**
 * @brief Flush pending NFTL write cache data
 *
 * This function requests NFTL to flush cached write data to flash.
 *
 * Note: When the I/O block status is any non-zero value, this interface
 *       rejects the request without touching flash.
 * Note: The current implementation flushes all pending write cache entries,
 *       regardless of the num parameter value.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param num Flush request count hint reserved for compatibility
 * @return int Returns 0 on success, non-zero on failure
 */
int nftl_api_write_cache(struct nftl_api_handler_t *handler, uint32_t num);

/**
 * @brief Register the nftl_api_write() completion callback
 *
 * The registered callback is invoked after nftl_api_write() finishes its NFTL
 * flash operation and before nftl_api_write() returns. The handler argument
 * passed to the callback identifies the NFTL instance that completed the
 * operation. Passing NULL unregisters the callback.
 *
 * Typical usage: report completion after each flash write operation. The
 * application can then call nftl_api_get_flash_op_status() to check whether
 * NFTL is idle and allow power-off as soon as flash is safe, improving the
 * timeliness of power-loss protection.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param callback Callback function pointer, or NULL to unregister
 * @return None
 */
void nftl_api_register_write_callback(struct nftl_api_handler_t *handler,
                                      nftl_api_op_callback_t callback);

/**
 * @brief Register the nftl_api_read() completion callback
 *
 * The registered callback is invoked after nftl_api_read() finishes its NFTL
 * flash operation and before nftl_api_read() returns. The handler argument
 * passed to the callback identifies the NFTL instance that completed the
 * operation. Passing NULL unregisters the callback.
 *
 * Typical usage: report completion after each flash read operation. The
 * application can then call nftl_api_get_flash_op_status() to check whether
 * NFTL is idle and allow power-off as soon as flash is safe, improving the
 * timeliness of power-loss protection.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param callback Callback function pointer, or NULL to unregister
 * @return None
 */
void nftl_api_register_read_callback(struct nftl_api_handler_t *handler,
                                     nftl_api_op_callback_t callback);

/**
 * @brief Register the nftl_api_write_cache() completion callback
 *
 * The registered callback is invoked after nftl_api_write_cache() finishes its
 * NFTL flash operation and before nftl_api_write_cache() returns. The handler
 * argument passed to the callback identifies the NFTL instance that completed
 * the operation. Passing NULL unregisters the callback.
 *
 * Typical usage: report completion after each flash cache-flush operation. The
 * application can then call nftl_api_get_flash_op_status() to check whether
 * NFTL is idle and allow power-off as soon as flash is safe, improving the
 * timeliness of power-loss protection.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param callback Callback function pointer, or NULL to unregister
 * @return None
 */
void nftl_api_register_write_cache_callback(struct nftl_api_handler_t *handler,
                                            nftl_api_op_callback_t callback);

/**
 * @brief Get the current NFTL flash-operation status
 *
 * This function reports whether the current NFTL instance is still executing
 * a flash access through the nftl_api layer.
 *
 * Return value meanings:
 *   - 0: NFTL is not operating flash
 *   - 1: NFTL is operating flash
 *
 * Note: For safe power-off sequencing, it is recommended to first call
 *       nftl_api_set_io_block_status(handler, NFTL_IO_STATUS_BLOCK_API) to block new nftl_api
 *       read/write/write_cache requests, then poll this interface until it
 *       returns 0 before cutting power.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return int Returns the current flash-operation status, or a negative value on failure
 */
int nftl_api_get_flash_op_status(struct nftl_api_handler_t *handler);

/**
 * @brief Print the current NFTL area-node information
 *
 * This function outputs the internal NFTL partition status for debugging.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return None
 */
void nftl_api_print_nftl_area_node(struct nftl_api_handler_t *handler);

/**
 * @brief Print the current NFTL free-block list
 *
 * This function outputs the free-block list for debugging and diagnostics.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return None
 */
void nftl_api_print_free_list(struct nftl_api_handler_t *handler);

/**
 * @brief Print the current NFTL invalid-block list
 *
 * This function outputs the invalid-page/block list for debugging and diagnostics.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return None
 */
void nftl_api_print_block_invalid_list(struct nftl_api_handler_t *handler);

/**
 * @brief Print the logical-to-physical page mapping table
 *
 * This function outputs the current logical page mapping information for
 * debugging and diagnostics.
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return None
 */
void nftl_api_print_logic_page_map(struct nftl_api_handler_t *handler);

/**
 * @brief Print NFTL library build information
 *
 * This function outputs the current NFTL library build date and time.
 *
 * @return None
 */
void nftl_api_lib_info(void);

/**
 * @brief Get the current NFTL I/O block status
 *
 * This function retrieves the current I/O block status from the NFTL instance.
 * The return value uses the following meanings:
 *   - NFTL_IO_STATUS_UNLOCK: all accesses are allowed
 *   - NFTL_IO_STATUS_BLOCK_IO: low-level erase/write/mark operations are blocked,
 *        and nftl_api_read/write/write_cache are also rejected
 *   - NFTL_IO_STATUS_BLOCK_API: nftl_api_read/write/write_cache are rejected
 *
 * @param handler Pointer to the NFTL API handler structure
 * @return int Returns the current I/O block status value, or a negative value on failure
 */
int nftl_api_get_io_block_status(struct nftl_api_handler_t *handler);

/**
 * @brief Set the NFTL I/O block status
 *
 * This function updates the NFTL block status using the following values:
 *   - NFTL_IO_STATUS_UNLOCK: clear all software block states
 *   - NFTL_IO_STATUS_BLOCK_IO: block low-level NFTL erase/write/mark
 *        operations in the hardware I/O layer, and also reject
 *        nftl_api_read(), nftl_api_write(), and nftl_api_write_cache()
 *   - NFTL_IO_STATUS_BLOCK_API: reject nftl_api_read(), nftl_api_write(), and
 *        nftl_api_write_cache() requests from upper layers while leaving the
 *        hardware I/O gating state unchanged
 *
 * Typical safe power-off sequence:
 *   1. call nftl_api_set_io_block_status(handler, 2)
 *   2. poll nftl_api_get_flash_op_status(handler) until it returns 0
 *   3. then allow power-off
 *
 * @param handler Pointer to the NFTL API handler structure
 * @param status NFTL I/O block status value
 *
 * @return None
 */
void nftl_api_set_io_block_status(struct nftl_api_handler_t *handler, int status);

/**
 * @brief Get the OOB verification status from the NFTL instance
 *
 * This function retrieves the current status of Out-Of-Band (OOB) data verification
 * for the NFTL area. The verification status is determined by checking the least
 * significant bit of the verify_enable flag in the I/O control structure.
 *
 * @param handler Pointer to the NFTL API handler structure containing private data
 * @return int Returns 1 if OOB verification is enabled, 0 if disabled
 */
int nftl_api_get_oob_verify_status(struct nftl_api_handler_t *handler);

/**
 * @brief Enable or disable NFTL OOB verification functionality
 *
 * This function controls the Out-of-Band (OOB) data verification feature
 * in the NFTL (NAND Flash Translation Layer) API by setting the verify_enable
 * flag in the I/O control structure.
 *
 * @param handler Pointer to the NFTL API handler structure containing private data
 * @param enable Verification enable flag - non-zero to enable, zero to disable
 *
 * @return None
 */
void nftl_api_enable_oob_verify(struct nftl_api_handler_t *handler, int enable);

/* Legacy API for backward compatibility - Not recommended for new usage */

/**
 * @brief Get the latest global NFTL I/O block status
 *
 * This function retrieves the global compatibility status exported for legacy
 * users that do not pass a handler. The return value is the most recently
 * updated block status value among all NFTL instances.
 *
 * Note: This is a legacy interface maintained for backward compatibility only.
 *       When multiple NFTL instances exist, this function only returns the status
 *       of the instance that most recently had its status updated.
 *       It is recommended to use nftl_api_get_io_block_status() in preference
 *       to this one.
 *
 * @return int Returns the latest I/O block status value
 */
int nftl_api_check_io_error(void);

#endif /*_NFTL_API_H*/
