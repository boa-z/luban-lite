/*
 * Copyright (c) 2026, ArtInChip Technology Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RT-Thread HCI UART transport helpers used by vendor initialization code
 * (e.g. the Realtek firmware/config download).
 */

#ifndef NIMBLE_TRANSPORT_RTTHREAD_H_
#define NIMBLE_TRANSPORT_RTTHREAD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reconfigure the HCI UART at runtime (baudrate, parity, flow control).
 *
 * @param baudrate      new host baud rate, e.g. 115200 or 1500000
 * @param flow_control  0 = disable CTS/RTS, non-zero = enable CTS/RTS
 * @return 0 on success, negative errno on failure
 */
int rtthread_hci_uart_set_config(uint32_t baudrate, int flow_control);

/*
 * Follow the controller to a new UART rate after a vendor command that makes
 * it switch speed immediately (e.g. Realtek 0xfc17): ack the controller's
 * last H5 frame at the old rate, give it time to settle, then move the host
 * UART over.
 *
 * @param baudrate      new host baud rate, must match the controller setting
 * @param flow_control  0 = disable CTS/RTS, non-zero = enable CTS/RTS
 * @return 0 on success, negative errno on failure
 */
int rtthread_hci_uart_change_baudrate(uint32_t baudrate, int flow_control);

#ifdef __cplusplus
}
#endif

#endif /* NIMBLE_TRANSPORT_RTTHREAD_H_ */
