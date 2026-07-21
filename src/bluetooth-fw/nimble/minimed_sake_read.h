/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Post-handshake CGM read on the pump connection. Once the SAKE handshake completes, the pump
//! exposes its CGM service as a GATT server over the same link and the watch reads it as a GATT
//! client. v11: discover the CGM service (0x181F) + its characteristics and read CGM Feature,
//! logging results to the on-watch spike log.

//! Init the deferred-kickoff callout. Call once from the SAKE service init.
void minimed_sake_read_init(void);

//! Kick off the CGM read on `conn_handle` (deferred a beat to let the link settle after handshake).
void minimed_sake_read_start(uint16_t conn_handle);

//! Feed an inbound pump notification/indication (from the GAP NOTIFY_RX handler). Returns true if
//! it targeted a CGM characteristic we own (Measurement or RACP) and was consumed.
bool minimed_sake_read_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len);

//! Stop the read/poll timers (call on disconnect so polling doesn't fire on a dead link).
void minimed_sake_read_stop(void);
