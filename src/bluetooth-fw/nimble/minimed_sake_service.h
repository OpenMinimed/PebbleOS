/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Bench spike: host the Medtronic "SAKE Port" GATT service so a MiniMed
//! 700-series pump (BLE Central) can connect into the watch (Peripheral) and
//! start its SAKE handshake. Topology proof only -- no SAKE crypto yet.

//! Register the SAKE Port service (svc 0xfe82, char 0000fe82-...). Call from
//! bt_driver_start() after ble_svc_gatt_init().
int minimed_sake_service_init(void);

//! Medtronic pairing advertising payload (flags + 16-bit svc UUID 0xfe82 +
//! "Mobile PB" name). advert.c substitutes this for the normal Pebble advert
//! while the spike is enabled. Returns the length written into `buf`.
uint8_t minimed_sake_build_adv(uint8_t *buf, uint8_t buf_len);

//! Called from the GAP subscribe handler. When the pump enables notifications
//! on the SAKE Port, emit the 20-zero-byte wake-up frame that makes it send
//! its first handshake write.
void minimed_sake_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify);

//! Decrypt a pump->watch payload with the post-handshake session cipher (client direction).
//! Returns false if the handshake isn't complete or the MAC fails. `out` needs >= `n` bytes;
//! the plaintext length (n-3) is written to `*out_len`.
bool minimed_sake_decrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t *out_len);
