/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Pure decode of IDD History Data records, narrowed to pump annunciations (alarms/alerts).
//! NimBLE-free so the host harness links it. Input is one reassembled, decrypted record:
//! event type(2 LE) | sequence number(4 LE) | relative offset(2 LE) | event data.
//! Formats: Documentation/idd-service.md + PythonPumpConnector history/data.py.

typedef struct {
  uint32_t seq;    // record sequence number (valid on Yes and Other)
  uint16_t type;   // annunciation type, 0x0FFF-masked (e.g. 0x054 = insert battery)
  uint16_t id;     // annunciation instance id
  uint8_t status;  // raw status: 0x0f undetermined, 0x33 pending, 0x3c snoozed, 0x55 confirmed
  bool silenced;   // event flags bit 6: the pump raised this alert quietly (alert settings)
} MinimedAnnunciation;

typedef enum {
  MinimedAnnuncRecordBad = 0,  // malformed; only out->seq is usable (filled when len >= 8)
  MinimedAnnuncRecordOther,    // a valid record of another event type; out->seq filled
  MinimedAnnuncRecordYes,      // Annunciation Consolidated (0xf010); *out fully filled
} MinimedAnnuncRecord;

MinimedAnnuncRecord minimed_annunciation_parse_record(const uint8_t *rec, uint16_t len,
                                                      MinimedAnnunciation *out);

//! Short display name for an annunciation type code, or NULL if not in the table
//! (caller shows the hex code instead). Codes: PythonPumpConnector AnnunciationType.
const char *minimed_annunciation_name(uint16_t type);
