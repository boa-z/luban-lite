/*
 * Copyright (c) 2006-2020, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2022-07-16     Lenoyan      the first version
 */

#include <assert.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sysinit/sysinit.h>
#include <syscfg/syscfg.h>
#include "os/os_mbuf.h"
#include "nimble/transport.h"
#include "nimble/transport_rtthread.h"
#ifdef LPKG_NIMBLE_HCI_H5
#include "nimble/transport/hci_h5.h"
#else
#include "nimble/transport/hci_h4.h"
#endif

#include <rtthread.h>
#include <rtdevice.h>

/* HCI snoop log: hex dump every HCI packet on the console. 0 = off. */
#define HCI_SNOOP_LOG 0

#if HCI_SNOOP_LOG
/* ---------------- HCI log dump ---------------- */

#define HCI_DUMP_CHUNK      64  /* bytes printed per line */

/*
 * The dump is enabled by default and keeps printing for the whole session:
 * connection setup, data transfer and disconnection. It is never stopped
 * automatically. Use the "hci_dump" shell command to control it at runtime:
 *   hci_dump on    - dump continuously (default)
 *   hci_dump off   - stop dumping
 *   hci_dump auto  - only start dumping at advertising/scanning/initiating,
 *                    so the controller init and firmware download are skipped
 */
static int g_hci_dump = 1;      /* dump state, on by default */
static int g_hci_dump_auto;     /* 1: wait for adv/scan/initiating to start */

/* One line is printed by a single rt_kprintf() so that TX/RX dumps from
 * different threads do not interleave and break the hex line. Packets longer
 * than HCI_DUMP_CHUNK are split into several lines, each starting with the
 * running byte offset ("+NNN").
 */
static void hci_dump_chunk(char dir, uint8_t type, size_t off,
                           const uint8_t *data, size_t len, size_t total)
{
    char line[3 * HCI_DUMP_CHUNK + 40];
    int pos = 0;
    size_t i;

    if (total <= HCI_DUMP_CHUNK) {
        pos += rt_snprintf(line + pos, sizeof(line) - pos, "HCI%c %02x", dir, type);
    } else {
        pos += rt_snprintf(line + pos, sizeof(line) - pos, "HCI%c %02x +%03u",
                           dir, type, (unsigned int)off);
    }

    for (i = 0; i < len; i++) {
        pos += rt_snprintf(line + pos, sizeof(line) - pos, " %02x", data[i]);
    }

    if (pos < (int)sizeof(line)) {
        if (off + len >= total) {
            rt_snprintf(line + pos, sizeof(line) - pos, " len=%u\n",
                        (unsigned int)total);
        } else {
            rt_snprintf(line + pos, sizeof(line) - pos, "\n");
        }
    } else {
        line[sizeof(line) - 2] = '\n';
        line[sizeof(line) - 1] = '\0';
    }

    rt_kprintf("%s", line);
}

static void hci_dump_buf(char dir, uint8_t type, const uint8_t *data, size_t len)
{
    size_t off;

    if (!g_hci_dump || data == NULL) {
        return;
    }

    for (off = 0; off < len; off += HCI_DUMP_CHUNK) {
        size_t n = len - off > HCI_DUMP_CHUNK ? HCI_DUMP_CHUNK : len - off;

        hci_dump_chunk(dir, type, off, data + off, n, len);
    }
    if (len == 0) {
        hci_dump_chunk(dir, type, 0, data, 0, 0);
    }
}

static void hci_dump_acl(char dir, struct os_mbuf *om)
{
    uint8_t tmp[HCI_DUMP_CHUNK];
    size_t total, off;

    if (!g_hci_dump || om == NULL) {
        return;
    }

    total = OS_MBUF_PKTLEN(om);
    for (off = 0; off < total; off += HCI_DUMP_CHUNK) {
        size_t n = total - off > HCI_DUMP_CHUNK ? HCI_DUMP_CHUNK : total - off;

        os_mbuf_copydata(om, off, n, tmp);
        hci_dump_chunk(dir, 0x02, off, tmp, n, total);
    }
    if (total == 0) {
        hci_dump_chunk(dir, 0x02, 0, tmp, 0, 0);
    }
}

/* HCI opcodes that start the interesting part of the log */
#define HCI_OP_LE_SET_ADV_ENABLE        0x200a
#define HCI_OP_LE_SET_SCAN_ENABLE       0x200c
#define HCI_OP_LE_CREATE_CONN           0x200d
#define HCI_OP_LE_EXT_CREATE_CONN       0x2036
#define HCI_OP_LE_SET_EXT_ADV_ENABLE    0x2039

/*
 * Turn the dump on when the host starts advertising / scanning / initiating,
 * i.e. after the controller init and firmware download are done.
 * Only used in "hci_dump auto" mode; the default mode is not gated.
 */
static void hci_dump_track_cmd(const uint8_t *cmd, size_t len)
{
    uint16_t opcode;

    if (cmd == NULL || len < 4 || !g_hci_dump_auto) {
        return;
    }

    opcode = cmd[0] | (cmd[1] << 8);

    switch (opcode) {
    case HCI_OP_LE_SET_ADV_ENABLE:
    case HCI_OP_LE_SET_EXT_ADV_ENABLE:
    case HCI_OP_LE_SET_SCAN_ENABLE:
        if (cmd[3] == 0x01) { /* enable */
            g_hci_dump = 1;
        }
        break;
    case HCI_OP_LE_CREATE_CONN:
    case HCI_OP_LE_EXT_CREATE_CONN:
        g_hci_dump = 1;
        break;
    default:
        break;
    }
}

static int hci_dump_cmd(int argc, char **argv)
{
    if (argc > 1) {
        if (!rt_strncmp(argv[1], "auto", sizeof("auto"))) {
            g_hci_dump_auto = 1;
            g_hci_dump = 0; /* wait for adv/scan/conn start */
        } else if (!rt_strncmp(argv[1], "off", sizeof("off")) ||
                   !rt_strncmp(argv[1], "0", sizeof("0"))) {
            g_hci_dump_auto = 0;
            g_hci_dump = 0;
        } else {
            g_hci_dump_auto = 0;
            g_hci_dump = 1;
        }
    } else {
        g_hci_dump_auto = 0;
        g_hci_dump = !g_hci_dump;
    }

    if (g_hci_dump_auto) {
        rt_kprintf("hci dump auto: starts at adv/scan/connect, never stops\n");
    } else {
        rt_kprintf("hci dump %s\n", g_hci_dump ? "on" : "off");
    }
    return 0;
}
MSH_CMD_EXPORT_ALIAS(hci_dump_cmd, hci_dump, dump HCI packets);
#endif /* HCI_SNOOP_LOG */

#if defined(LPKG_NIMBLE_HCI_H5)
static struct hci_h5_sm g_hci_h5sm;
#elif defined(LPKG_NIMBLE_HCI_H4)
static struct hci_h4_sm g_hci_h4sm;
#else
#error "Not support HCI protocol"
#endif

static rt_device_t g_serial;
static char g_msg_pool[256] = {0};
static struct rt_messagequeue g_rx_mq;

#if defined(LPKG_NIMBLE_HCI_H5)
static const uint8_t sync_req[] = { 0x01, 0x7e };
static const uint8_t sync_rsp[] = { 0x02, 0x7d };
/* Third byte may change */
static uint8_t conf_req[3] = { 0x03, 0xfc };
static const uint8_t conf_rsp[] = { 0x04, 0x7b };

static int hci_h5_uart_frame_cb(uint8_t pkt_type, void *data)
{

    switch (pkt_type) {
    case HCI_H5_LINK:
        if (!memcmp(data, sync_req, sizeof(sync_req))) {
            if (g_hci_h5sm.link_state == ACTIVE) {
                /* TODO Reset H5 */
                g_hci_h5sm.link_state = UNINIT;
            }

            hci_h5_sm_send(&g_hci_h5sm, HCI_H5_LINK, sync_rsp, sizeof(sync_rsp));
        } else if (!memcmp(data, sync_rsp, sizeof(sync_rsp))) {
            if (g_hci_h5sm.link_state == ACTIVE) {
                /* TODO Reset H5 */
                g_hci_h5sm.link_state = UNINIT;
            }

            g_hci_h5sm.link_state = INIT;
            conf_req[2] = g_hci_h5sm.tx_win & 0x07;
            hci_h5_sm_send(&g_hci_h5sm, HCI_H5_LINK, conf_req, sizeof(conf_req));
        } else if (!memcmp(data, conf_req, 2)) {
            /* The Host sends Config Response messages without a Configuration Field.*/
            hci_h5_sm_send(&g_hci_h5sm, HCI_H5_LINK, conf_rsp, sizeof(conf_rsp));
            /* Then send Config Request with Configuration Field */
            conf_req[2] = g_hci_h5sm.tx_win & 0x07;
            hci_h5_sm_send(&g_hci_h5sm, HCI_H5_LINK, conf_req, sizeof(conf_req));
        } else if (!memcmp(data, conf_rsp, 2)) {
            g_hci_h5sm.link_state = ACTIVE;
            /* Configuration field present */
            g_hci_h5sm.tx_win = (((uint8_t *)data)[2] & 0x07);
            pr_debug("Finished H5 configuration, tx_win %u\n", g_hci_h5sm.tx_win);
        } else {
            pr_err("Not handled yet %x %x\n", ((uint8_t *)data)[0], ((uint8_t *)data)[1]);
        }
        aicos_mdelay(100);
        break;
    case HCI_H5_EVT: {
#if HCI_SNOOP_LOG
        uint8_t *evt = data;

        if (evt != NULL) {
            hci_dump_buf('R', 0x04, evt, evt[1] + 2);
        }
#endif
        return ble_transport_to_hs_evt(data);
    }
    case HCI_H5_ACL:
#if HCI_SNOOP_LOG
        hci_dump_acl('R', data);
#endif
        return ble_transport_to_hs_acl(data);
    default:
        assert(0);
        break;
    }
    return -1;
}
#elif defined(LPKG_NIMBLE_HCI_H4)
static int hci_h4_uart_frame_cb(uint8_t pkt_type, void *data)
{
    switch (pkt_type) {
    case HCI_H4_EVT: {
#if HCI_SNOOP_LOG
        uint8_t *evt = data;

        if (evt != NULL) {
            hci_dump_buf('R', 0x04, evt, evt[1] + 2);
        }
#endif
        return ble_transport_to_hs_evt(data);
    }
    case HCI_H4_ACL:
#if HCI_SNOOP_LOG
        hci_dump_acl('R', data);
#endif
        return ble_transport_to_hs_acl(data);
    default:
        assert(0);
        break;
    }
    return -1;
}
#endif

static void rtthread_uart_rx_entry(void *parameter)
{
    rt_size_t size;
    /* Static buffer: the thread stack is only 4 KB, keep the 2 KB RX buffer
     * out of it. Only this RX thread touches it.
     */
    static uint8_t data[2048];
    size_t data_len;

    while (1) {
        if (!rt_mq_recv(&g_rx_mq, &size, sizeof(rt_size_t), 10000)) {
            /* size comes from the driver's rx_indicate; clamp it to the
             * local buffer before handing it to rt_device_read().
             */
            if (size > sizeof(data)) {
                size = sizeof(data);
            }

            data_len = rt_device_read(g_serial, 0, data, size);
            if (data_len > 0) {
                /* Forward raw HCI bytes to the aic8800D40L BT driver server
                 * queue. Only the aic8800 wifi/BT combo platform provides this
                 * queue, so it must not be referenced on other BT modules
                 * (e.g. Realtek), otherwise linking fails.
                 */
#if defined(AIC_WLAN_AIC8800D40L)
                extern rt_mq_t bt_driver_server_queue;

                /* rt_mq_send() copies the message into the queue, so send
                 * straight from the RX buffer: no per-packet malloc/memset/
                 * memcpy/free. The queue message size (BT_HCI_QUEUE_SIZE) is
                 * the same 2048 bytes; the consumer only parses the HCI
                 * packet length, so stale tail bytes are harmless.
                 */
                if (bt_driver_server_queue) {
                    rt_mq_send(bt_driver_server_queue, data, sizeof(data));
                }
#endif
#if defined(LPKG_NIMBLE_HCI_H5) // H5
                hci_h5_sm_rx(&g_hci_h5sm, data, data_len);
#elif defined(LPKG_NIMBLE_HCI_H4)
                hci_h4_sm_rx(&g_hci_h4sm, data, data_len);
#endif
            }
        }
    }
}

void rtthread_uart_tx(const uint8_t *buf, size_t len)
{
    size_t remaining = len;
    size_t tx_size = 0;

    while (remaining > 0) {
        tx_size = rt_device_write(g_serial, 0, buf, remaining);
        if (tx_size == 0 || tx_size > remaining) {
            /* Device not ready / out of space: do not spin forever. */
            pr_err("hci uart tx stalled, %u bytes left\n", (unsigned int)remaining);
            break;
        }
        buf += tx_size;
        remaining -= tx_size;
    }
}

static rt_err_t serial_input_mq(rt_device_t dev, rt_size_t size)
{
    rt_err_t result;

    result = rt_mq_send(&g_rx_mq, &size, sizeof(rt_size_t));
    if (result == -RT_EFULL) {
        pr_err("message queue full!\n");
    }
    return result;
}

#if defined(LPKG_NIMBLE_HCI_H5)
/*
 * Send a pure H5 ACK right away.
 *
 * After the controller completes a command that changes its UART baudrate
 * (Realtek 0xfc17), it switches speed immediately. The vendor driver acks
 * that last packet at the old baudrate before changing the host side, see
 * baudrate_send_the_ack() in hci_process_int.c.
 */
void rtthread_hci_h5_send_ack(void)
{
    if (g_hci_h5sm.link_state == ACTIVE) {
        hci_h5_sm_send(&g_hci_h5sm, HCI_H5_ACK, NULL, 0);
    }
}
#endif

/*
 * Reconfigure the HCI UART at runtime.
 *
 * Vendor drivers use this after telling the controller to switch to a higher
 * baudrate (e.g. Realtek 0xfc17). The controller changes speed as soon as the
 * command completes, so the host side has to follow immediately, otherwise
 * every following HCI command times out.
 *
 * The device stays open: rt_serial_control() only forwards
 * RT_DEVICE_CTRL_CONFIG to the driver (which programs the hardware) while the
 * device ref_count is non-zero. Configuring a closed device just caches the
 * new values and the UART keeps running at the old baudrate.
 */
int rtthread_hci_uart_set_config(uint32_t baudrate, int flow_control)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;
    rt_err_t ret = -RT_ERROR;

    if (g_serial == RT_NULL) {
        return -1;
    }

    if (rt_device_control(g_serial, RT_SERIAL_GET_CONFIG, &config) != RT_EOK) {
        pr_err("uart get configure parameter fail!\n");
        return -1;
    }

    config.baud_rate = baudrate;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
#if defined(LPKG_NIMBLE_HCI_H5) // H5
    config.parity    = PARITY_EVEN;
#elif defined(LPKG_NIMBLE_HCI_H4)
    config.parity    = PARITY_NONE;
#endif
    if (flow_control) {
        config.flowcontrol = RT_SERIAL_FLOWCONTROL_CTSRTS;
        config.function = RT_SERIAL_RS232_AUTO_FLOW_CTRL;
    } else {
        config.flowcontrol = RT_SERIAL_FLOWCONTROL_NONE;
    }

    ret = rt_device_control(g_serial, RT_DEVICE_CTRL_CONFIG, &config);
    if (ret != RT_EOK) {
        pr_err("uart set baudrate %u fail!\n", (unsigned int)baudrate);
    }

    return ret;
}

/*
 * Switch the controller and the host to a new UART rate in one step.
 *
 * Some controllers (Realtek 0xfc17) change their UART speed right after the
 * Command Complete, while H5 still expects the host to acknowledge that
 * frame at the old rate. Ack it, give the controller time to settle (the
 * vendor driver waits 20 ms), and only then move the host UART.
 */
int rtthread_hci_uart_change_baudrate(uint32_t baudrate, int flow_control)
{
#ifdef LPKG_NIMBLE_HCI_H5
    rtthread_hci_h5_send_ack();
#endif
    rt_thread_mdelay(20);

    return rtthread_hci_uart_set_config(baudrate, flow_control);
}

static int rtthread_hci_uart_init(void)
{
    struct serial_configure config = RT_SERIAL_CONFIG_DEFAULT;

    g_serial = rt_device_find(LPKG_NIMBLE_HCI_UART_DEVICE_NAME);
    if (!g_serial) {
        pr_err("find %s failed!\n", LPKG_NIMBLE_HCI_UART_DEVICE_NAME);
        return -1;
    }

    if (rt_device_control(g_serial, RT_SERIAL_GET_CONFIG, &config) != RT_EOK) {
        rt_kprintf("uart get configure parameter fail!\n");
        return -1;
    }

    config.baud_rate = BAUD_RATE_115200;
    config.data_bits = DATA_BITS_8;
    config.stop_bits = STOP_BITS_1;
#if defined(LPKG_NIMBLE_HCI_H5) // H5
    config.parity    = PARITY_EVEN;
#elif defined(LPKG_NIMBLE_HCI_H4)
    config.baud_rate = BAUD_RATE_1500000;
    config.parity    = PARITY_NONE;
    config.flowcontrol = RT_SERIAL_FLOWCONTROL_CTSRTS;
    config.function = RT_SERIAL_RS232_AUTO_FLOW_CTRL;
#endif

    if (rt_device_control(g_serial, RT_DEVICE_CTRL_CONFIG, &config) != RT_EOK) {
        pr_err("uart set baudrate fail!\n");
        return -1;
    }

    rt_device_set_rx_indicate(g_serial, serial_input_mq);
    rt_mq_init(&g_rx_mq, "hci_rx_mq",
               g_msg_pool,
               sizeof(rt_size_t),
               sizeof(g_msg_pool),
               RT_IPC_FLAG_FIFO);

#ifdef RT_USING_SERIAL_V2
    rt_device_open(g_serial, RT_DEVICE_FLAG_RX_NON_BLOCKING | RT_DEVICE_FLAG_TX_BLOCKING);
#else
    rt_device_open(g_serial, RT_DEVICE_FLAG_INT_RX);
#endif

    rt_thread_t rx_thread = rt_thread_create("hci_uart_rx", rtthread_uart_rx_entry,
                                            RT_NULL, 4096, 24, 10);
    if (rx_thread != RT_NULL) {
        rt_thread_startup(rx_thread);
    } else {
        rt_device_close(g_serial);
        return -1;
    }

    return 0;
}


int ble_transport_to_ll_cmd_impl(void *buf)
{
    uint8_t *cmd_pkt_data = (uint8_t *)buf;
    size_t pkt_len;

    if (cmd_pkt_data == NULL) {
        return -1;
    }

    pkt_len = cmd_pkt_data[2] + 3;

#if HCI_SNOOP_LOG
    hci_dump_track_cmd(cmd_pkt_data, pkt_len);
    hci_dump_buf('T', 0x01, cmd_pkt_data, pkt_len);
#endif

#if defined(LPKG_NIMBLE_HCI_H5) // H5
    if (g_hci_h5sm.link_state == ACTIVE) {
        hci_h5_sm_send(&g_hci_h5sm, HCI_H5_CMD, buf, pkt_len);
    } else {
        /* The command never reaches the controller in this state and the
         * host will time out, so make the reason visible.
         */
        pr_warn("H5 link state %d, drop cmd 0x%04x\n",
                g_hci_h5sm.link_state,
                cmd_pkt_data[0] | (cmd_pkt_data[1] << 8));
    }
#elif defined(LPKG_NIMBLE_HCI_H4)
    uint8_t indicator = 0;
    indicator = HCI_H4_CMD;
    rtthread_uart_tx(&indicator, 1);
    rtthread_uart_tx(cmd_pkt_data, pkt_len);

#endif
    ble_transport_free(buf);

    return 0;
}

int ble_transport_to_ll_acl_impl(struct os_mbuf *om)
{

#if defined(LPKG_NIMBLE_HCI_H5) // H5
    if (g_hci_h5sm.link_state == ACTIVE) {
        uint16_t data_len = OS_MBUF_PKTLEN(om);
        uint8_t *data = malloc(data_len);

        if (data == NULL) {
            os_mbuf_free_chain(om);
            return -1;
        }

        os_mbuf_copydata(om, 0, data_len, data);
#if HCI_SNOOP_LOG
        hci_dump_acl('T', om);
#endif
        hci_h5_sm_send(&g_hci_h5sm, HCI_H5_ACL, data, data_len);
        free(data);
    }

    os_mbuf_free_chain(om);
#elif defined(LPKG_NIMBLE_HCI_H4)
    uint8_t indicator = 0;

    indicator = HCI_H4_ACL;

#if HCI_SNOOP_LOG
    hci_dump_acl('T', om);
#endif

    rtthread_uart_tx(&indicator, 1);

    struct os_mbuf *x = om;
    while (x != NULL)
    {
        rtthread_uart_tx(x->om_data, x->om_len);
        x = SLIST_NEXT(x, om_next);
    }

    os_mbuf_free_chain(om);
#endif

    return 0;
}

static int rtthread_ble_transport_init(void)
{
    int rc;
    SYSINIT_ASSERT_ACTIVE();

    rc = rtthread_hci_uart_init();
    SYSINIT_PANIC_ASSERT(rc == 0);

#if defined(LPKG_NIMBLE_HCI_H5) // H5
    hci_h5_sm_init(&g_hci_h5sm, &hci_h5_allocs_from_ll, hci_h5_uart_frame_cb);
    hci_h5_sm_send(&g_hci_h5sm, HCI_H5_LINK, sync_req, sizeof(sync_req));
#elif defined(LPKG_NIMBLE_HCI_H4)
    hci_h4_sm_init(&g_hci_h4sm, &hci_h4_allocs_from_ll, hci_h4_uart_frame_cb);
#endif
    return 0;
}
#ifdef LPKG_NIMBLE_HCI_USING_RTT_UART
INIT_APP_EXPORT(rtthread_ble_transport_init);
#endif
