/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Parses a Pebble Glucose Protocol capability announcement (watchface -> sender) out of a raw
//! serialized AppMessage dictionary. Pure (no firmware dependencies, not even dict.c, which pulls
//! in passert + pbl_malloc) so the host harness in tools/minimed_sake_hosttest can test it.
//!
//! This doubles as the test for "is the foreground watchface one of ours?". We have to claim a
//! watchface's UUID on the transport *before* we can receive anything from it, so the claim is
//! provisional and this parse is what confirms or refutes it. The protocol has no magic number,
//! so the check leans on value constraints: version must be exactly PROTOCOL_VERSION, and the
//! capability word must be non-zero with no undefined bits set.

typedef struct {
  uint8_t version;
  uint32_t caps;      //!< CAP_* bitfield from pebble_glucose_protocol.h
  uint8_t graph_hours;  //!< 0 = no graph wanted, or the key was absent
} MinimedGlucoseAnnounce;

//! Parse `len` bytes of serialized dictionary. Returns false (leaving *out zeroed) for anything
//! that is not a well-formed announcement -- a truncated dictionary, or another watchface's
//! AppMessage that happens to have been routed to us.
bool minimed_glucose_parse_announce(const uint8_t *dict, uint16_t len,
                                    MinimedGlucoseAnnounce *out);
