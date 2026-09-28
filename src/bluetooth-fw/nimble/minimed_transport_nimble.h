/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

struct os_mbuf;

//! Driver-side hooks of the NimBLE pump transport (minimed_transport.h is the session side).

//! The SAKE handshake completed on `conn_handle`: the link is ready for GATT client use.
void minimed_transport_nimble_link_up(uint16_t conn_handle);

//! The pump disconnected.
void minimed_transport_nimble_link_down(void);

//! A notification/indication arrived (GAP NOTIFY_RX). Returns true if it was on one of the pump
//! characteristics the session uses, in which case it has been queued for the session.
bool minimed_transport_nimble_handle_notify(uint16_t conn_handle, uint16_t attr_handle,
                                            const struct os_mbuf *om);
