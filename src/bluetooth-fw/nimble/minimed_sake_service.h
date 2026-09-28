/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nimble/ble.h"  // ble_addr_t

//! MiniMed pump link: host the Medtronic "SAKE Port" GATT service so a MiniMed
//! 700-series pump (BLE Central) can connect into the watch (Peripheral) and
//! start its SAKE handshake. Topology proof only -- no SAKE crypto yet.

//! Register the SAKE Port service (svc 0xfe82, char 0000fe82-...). Call from
//! bt_driver_start() after ble_svc_gatt_init().
int minimed_sake_service_init(void);

//! Medtronic pairing advertising payload (flags + 16-bit svc UUID 0xfe82 +
//! "Mobile PB" name). advert.c substitutes this for the normal Pebble advert
//! while the MiniMed pump link is enabled. Returns the length written into `buf`.
uint8_t minimed_sake_build_adv(uint8_t *buf, uint8_t buf_len);

//! Called from the GAP subscribe handler. When the pump enables notifications
//! on the SAKE Port, emit the 20-zero-byte wake-up frame that makes it send
//! its first handshake write.
void minimed_sake_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify);

//! minimed_sake_decrypt / minimed_sake_encrypt (the post-handshake session cipher) are declared
//! in minimed_transport.h, the session's side of the link.

//! True if `addr` matches the pump identity captured at the last SAKE handshake completion (the
//! pump re-runs the full handshake on every reconnect, so this is refreshed each cycle). Used only
//! by the v29 advert diagnostics to label a connection PUMP vs phone. NimBLE resolves the pump's
//! RPA to this identity via the bond IRK, so it matches in both DUAL and NORMAL.
bool minimed_sake_addr_is_pump(const ble_addr_t *addr);
