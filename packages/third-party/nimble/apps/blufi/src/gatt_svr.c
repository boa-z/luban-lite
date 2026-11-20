/*
 * SPDX-FileCopyrightText: 2015-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "blufi_api.h"
#include "blufi_prf.h"
#include "blufi.h"

/* 512 bytes max ATT attribute value plus the BLUFI header margin. */
#define BLUFI_GATT_RX_BUF_SIZE  517

struct gatt_value gatt_values[SERVER_MAX_VALUES];

enum {
    GATT_VALUE_TYPE_CHR,
    GATT_VALUE_TYPE_DSC,
};

static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def gatt_svr_svcs[] = {
    {
        /*** Service: Blufi */
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID16_DECLARE(BLUFI_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) { {
            /* Characteristic: Host to Device */
            .uuid = BLE_UUID16_DECLARE(BLUFI_CHAR_H2D_UUID),
            .access_cb = gatt_svr_access_cb,
            .arg = &gatt_values[0],
            .val_handle = &gatt_values[0].val_handle,
            .flags = BLE_GATT_CHR_F_WRITE,
        }, {
            /* Characteristic: Device to Host */
            .uuid = BLE_UUID16_DECLARE(BLUFI_CHAR_D2H_UUID),
            .access_cb = gatt_svr_access_cb,
            .arg = &gatt_values[1],
            .val_handle = &gatt_values[1].val_handle,
            .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
        }, {
            0, /* No more characteristics in this service */
        }, }
    },

    {
        0, /* No more services */
    },
};

void blufi_gatt_svr_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
    /* Only used by the MODLOG calls below; keep it valid when they are
     * compiled out (log level below DEBUG).
     */
    char buf[BLE_UUID_STR_LEN] __attribute__((unused));

    switch (ctxt->op) {
    case BLE_GATT_REGISTER_OP_SVC:
        MODLOG_DFLT(DEBUG, "registered service %s with handle=%d",
                    ble_uuid_to_str(ctxt->svc.svc_def->uuid, buf), ctxt->svc.handle);
        break;

    case BLE_GATT_REGISTER_OP_CHR:
        MODLOG_DFLT(DEBUG, "registering characteristic %s with "
                    "def_handle=%d val_handle=%d\n",
                    ble_uuid_to_str(ctxt->chr.chr_def->uuid, buf),
                    ctxt->chr.def_handle, ctxt->chr.val_handle);
        break;

    case BLE_GATT_REGISTER_OP_DSC:
        MODLOG_DFLT(DEBUG, "registering descriptor %s with handle=%d",
                    ble_uuid_to_str(ctxt->dsc.dsc_def->uuid, buf), ctxt->dsc.handle);
        break;

    default:
        assert(0);
        break;
    }
}

static int write_value(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    struct gatt_value *value = (struct gatt_value *)arg;
    struct os_mbuf *temp;
    uint32_t offset = 0;
    uint16_t len;
    uint8_t *fw_buf;
    int rc;

    /* gatt_svr_access_cb only dispatches characteristic accesses. */
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->chr->flags & BLE_GATT_CHR_F_WRITE_AUTHOR) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    /* Data may come in linked om. So retrieve all data */
    if (SLIST_NEXT(ctxt->om, om_next) != NULL) {
        fw_buf = malloc(BLUFI_GATT_RX_BUF_SIZE);
        if (fw_buf == NULL) {
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }

        for (temp = ctxt->om; temp != NULL; temp = SLIST_NEXT(temp, om_next)) {
            if (offset + temp->om_len > BLUFI_GATT_RX_BUF_SIZE) {
                free(fw_buf);
                return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }

            memcpy(fw_buf + offset, temp->om_data, temp->om_len);
            offset += temp->om_len;
        }
        ble_blufi_recv_handler(fw_buf, (int)offset);

        free(fw_buf);
    } else {
        ble_blufi_recv_handler(ctxt->om->om_data, ctxt->om->om_len);
    }

    rc = ble_hs_mbuf_to_flat(ctxt->om, value->buf->om_data, value->buf->om_len, &len);
    if (rc != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    /* Maximum attribute value size is 512 bytes */
    assert(value->buf->om_len < MAX_VAL_SIZE);

    return 0;
}

static int read_value(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    const struct gatt_value *value = (const struct gatt_value *) arg;
    int rc;

    /* gatt_svr_access_cb only dispatches characteristic accesses. */
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    if (ctxt->chr->flags & BLE_GATT_CHR_F_READ_AUTHOR) {
        return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    rc = os_mbuf_append(ctxt->om, value->buf->om_data, value->buf->om_len);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
        return read_value(conn_handle, attr_handle, ctxt, arg);
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        return write_value(conn_handle, attr_handle, ctxt, arg);
    default:
        /* Unknown access; reject it instead of aborting in a stack callback. */
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static void free_gatt_value_buf(int i)
{
    if (gatt_values[i].buf != NULL) {
        os_mbuf_free(gatt_values[i].buf);
        gatt_values[i].buf = NULL;
    }
}

static int init_gatt_values(void)
{
    int i = 0;
    const struct ble_gatt_svc_def *svc;
    const struct ble_gatt_chr_def *chr;
    const struct ble_gatt_dsc_def *dsc;

    for (svc = gatt_svr_svcs; svc && svc->uuid; svc++) {
        for (chr = svc->characteristics; chr && chr->uuid; chr++) {
            assert(i < SERVER_MAX_VALUES);
            gatt_values[i].type = GATT_VALUE_TYPE_CHR;
            gatt_values[i].ptr = (void *)chr;
            gatt_values[i].buf = os_msys_get(0, 0);
            if (gatt_values[i].buf == NULL ||
                os_mbuf_extend(gatt_values[i].buf, 1) == NULL) {
                free_gatt_value_buf(i);
                return -1;
            }
            ++i;

            for (dsc = chr->descriptors; dsc && dsc->uuid; dsc++) {
                assert(i < SERVER_MAX_VALUES);
                gatt_values[i].type = GATT_VALUE_TYPE_DSC;
                gatt_values[i].ptr = (void *)dsc;
                gatt_values[i].buf = os_msys_get(0, 0);
                if (gatt_values[i].buf == NULL ||
                    os_mbuf_extend(gatt_values[i].buf, 1) == NULL) {
                    free_gatt_value_buf(i);
                    return -1;
                }
                ++i;
            }
        }
    }

    return 0;
}

static void deinit_gatt_values(void)
{
    int i = 0;
    const struct ble_gatt_svc_def *svc;
    const struct ble_gatt_chr_def *chr;
    const struct ble_gatt_dsc_def *dsc;

    for (svc = gatt_svr_svcs; svc && svc->uuid; svc++) {
        for (chr = svc->characteristics; chr && chr->uuid; chr++) {
            if (i < SERVER_MAX_VALUES && gatt_values[i].buf != NULL) {
                os_mbuf_free(gatt_values[i].buf);  /* Free the buffer */
                gatt_values[i].buf = NULL;         /* Nullify the pointer to avoid dangling references */
            }
            ++i;

            for (dsc = chr->descriptors; dsc && dsc->uuid; dsc++) {
                if (i < SERVER_MAX_VALUES && gatt_values[i].buf != NULL) {
                    os_mbuf_free(gatt_values[i].buf);  /* Free the buffer */
                    gatt_values[i].buf = NULL;         /* Nullify the pointer to avoid dangling references */
                }
                ++i;
            }
        }
    }
}

int blufi_gatt_svr_init(void)
{
    int rc;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    rc = ble_gatts_count_cfg(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(gatt_svr_svcs);
    if (rc != 0) {
        return rc;
    }

    rc = init_gatt_values();
    if (rc != 0) {
        deinit_gatt_values();
        return rc;
    }

    return 0;
}

int blufi_gatt_svr_deinit(void)
{
    deinit_gatt_values();

    return 0;
}

void blufi_send_notify(void *arg)
{
    struct pkt_info *pkts = (struct pkt_info *) arg;
    struct os_mbuf *om;

    om = ble_hs_mbuf_from_flat(pkts->pkt, pkts->pkt_len);
    if (om == NULL) {
        MODLOG_DFLT(ERROR, "Error in allocating memory");
        return;
    }
    int rc = ble_gatts_notify_custom(blufi_env.conn_id, gatt_values[1].val_handle, om);
    if (rc != 0) {
        MODLOG_DFLT(ERROR, "Error in sending notification");
    }
}

void blufi_send_encap(void *arg)
{
    struct blufi_hdr *hdr = (struct blufi_hdr *)arg;

    if (blufi_env.is_connected == false) {
        MODLOG_DFLT_WARN("%s ble connection is broken\n", __func__);
        return;
    }
    ble_blufi_send_notify((uint8_t *)hdr,
                      ((hdr->fc & BLUFI_FC_CHECK) ?
                      hdr->data_len + sizeof(struct blufi_hdr) + 2 :
                      hdr->data_len + sizeof(struct blufi_hdr)));
}
