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

//! True if this alert is about a predicted/impending low BG, so showing the latest BG next to
//! it is contextually useful. False for everything else (including alerts already about BG,
//! e.g. an already-triggered low or a high SG, which don't need "predicted" framing).
bool minimed_annunciation_shows_bg(uint16_t type);

//! True for the low-BG family of alerts (predicted low, low, severe low, and the suspend-before/
//! threshold-suspend alarms that ride along with them) -- the SETTINGS_ALERT_LOW category a user
//! can choose to keep even with every other pump alert popup turned off (issue #15). Broader than
//! minimed_annunciation_shows_bg: that one is about whether to caption the notification with the
//! BG value, this one is about whether to show the notification at all.
bool minimed_annunciation_is_low(uint16_t type);
