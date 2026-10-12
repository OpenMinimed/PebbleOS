/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t session_start;
  uint16_t expiration_minutes;
  bool notified;
} MinimedSensorReminder;

void minimed_sensor_reminder_init(MinimedSensorReminder *state, uint32_t session_start,
                                  uint16_t expiration_minutes, bool notified);
//! Parse CGMS DateTime into local wall-clock seconds; convert with time_local_to_utc before use.
bool minimed_sensor_reminder_parse_session_start(const uint8_t *plain, uint16_t len,
                                                 uint32_t *timestamp_out);
bool minimed_sensor_reminder_parse_expiration(const uint8_t *value, uint16_t len,
                                              uint16_t *minutes_out);

//! Return true when the session start changes. A new session rearms its notification.
bool minimed_sensor_reminder_set_session_start(MinimedSensorReminder *state, uint32_t timestamp);
bool minimed_sensor_reminder_set_expiration(MinimedSensorReminder *state, uint16_t minutes);

//! Expiration as a Unix timestamp, or zero when either input is missing.
uint32_t minimed_sensor_reminder_expiration(const MinimedSensorReminder *state);

//! The quiet display window and one-time notification schedule, in watch-local time.
bool minimed_sensor_reminder_status_window(const MinimedSensorReminder *state, uint32_t now,
                                           uint32_t *expiration_out);
bool minimed_sensor_reminder_notification_due(const MinimedSensorReminder *state, uint32_t now);
void minimed_sensor_reminder_mark_notified(MinimedSensorReminder *state);
