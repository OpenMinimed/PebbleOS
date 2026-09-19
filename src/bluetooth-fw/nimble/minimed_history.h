/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Pure decode of IDD History Data records other than annunciations (those live in
//! minimed_annunciation.{c,h}). NimBLE-free so the host harness links it. Input is one
//! reassembled, decrypted record: event type(2 LE) | sequence number(4 LE) | relative offset(2 LE)
//! | event data. Formats: Documentation/idd-service.md + PythonPumpConnector history/events.

//! SG Measurement (0xf00c): one CGM sample as stored in the pump's event log.
typedef struct {
  uint32_t seq;
  int16_t offset_min;  //!< minutes on the same clock as the CGM Measurement's Time Offset
  uint16_t sg;         //!< raw SG value; see minimed_history_sg_to_mgdl
} MinimedHistSg;

//! Decode an SG Measurement record. False if it is another event type or is truncated.
bool minimed_history_parse_sg(const uint8_t *rec, uint16_t len, MinimedHistSg *out);

//! Special SG Value codes (Documentation/idd-service.md): no sample, or clamped at the sensor's
//! range edge.
#define MINIMED_HIST_SG_STARTING 0x0301
#define MINIMED_HIST_SG_UPDATING 0x0303
#define MINIMED_HIST_SG_ABOVE 0x0308
#define MINIMED_HIST_SG_BELOW 0x030d

//! Convert a raw SG value to mg/dL. Off-scale codes map to `floor_mgdl` / `ceiling_mgdl` (where
//! the caller graphs "at or beyond" readings). Returns -1 for "no usable sample" (starting,
//! updating, or any other code in the special range).
int32_t minimed_history_sg_to_mgdl(uint16_t sg, int32_t floor_mgdl, int32_t ceiling_mgdl);
