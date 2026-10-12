/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#define _POSIX_C_SOURCE 200809L

#include "minimed_sensor_reminder.h"

#include <stdlib.h>
#include <time.h>

#define CGMS_START_TIME_BYTES 11
#define SENSOR_EXPIRATION_VALUE_OFFSET 8
#define MORNING_CUTOFF_MINUTES (9 * 60)
#define NOTIFICATION_QUIET_START 21

static int64_t prv_days_from_civil(int32_t year, uint32_t month, uint32_t day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t year_of_era = (uint32_t)(year - era * 400);
  const uint32_t adjusted_month = month > 2 ? month - 3 : month + 9;
  const uint32_t day_of_year = (153 * adjusted_month + 2) / 5 + day - 1;
  const uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return (int64_t)era * 146097 + day_of_era - 719468;
}

void minimed_sensor_reminder_init(MinimedSensorReminder *state, uint32_t session_start,
                                  uint16_t expiration_minutes, bool notified) {
  *state = (MinimedSensorReminder){
      .session_start = session_start,
      .expiration_minutes = expiration_minutes,
      .notified = notified,
  };
}

bool minimed_sensor_reminder_parse_session_start(const uint8_t *plain, uint16_t len,
                                                 uint32_t *timestamp_out) {
  if (len != CGMS_START_TIME_BYTES) return false;
  const int year = plain[0] | (plain[1] << 8);
  if (year < 2000 || plain[2] < 1 || plain[2] > 12 || plain[3] < 1 || plain[3] > 31 ||
      plain[4] > 23 || plain[5] > 59 || plain[6] > 59) {
    return false;
  }
  const int64_t days = prv_days_from_civil(year, plain[2], plain[3]);
  const int64_t local_wall_time = days * 86400 + plain[4] * 3600 + plain[5] * 60 + plain[6];
  if (local_wall_time <= 0 || local_wall_time > UINT32_MAX) return false;
  *timestamp_out = (uint32_t)local_wall_time;
  return true;
}

bool minimed_sensor_reminder_parse_expiration(const uint8_t *value, uint16_t len,
                                              uint16_t *minutes_out) {
  if (len < SENSOR_EXPIRATION_VALUE_OFFSET + 2) return false;
  *minutes_out = (uint16_t)(value[SENSOR_EXPIRATION_VALUE_OFFSET] |
                            (value[SENSOR_EXPIRATION_VALUE_OFFSET + 1] << 8));
  return *minutes_out != 0;
}

bool minimed_sensor_reminder_set_session_start(MinimedSensorReminder *state, uint32_t timestamp) {
  if (timestamp == 0 || timestamp == state->session_start) return false;
  state->session_start = timestamp;
  state->notified = false;
  return true;
}

bool minimed_sensor_reminder_set_expiration(MinimedSensorReminder *state, uint16_t minutes) {
  if (minutes == 0 || minutes == state->expiration_minutes) return false;
  state->expiration_minutes = minutes;
  return true;
}

uint32_t minimed_sensor_reminder_expiration(const MinimedSensorReminder *state) {
  if (state->session_start == 0 || state->expiration_minutes == 0) return 0;
  return state->session_start + (uint32_t)state->expiration_minutes * 60;
}

static uint32_t prv_local_time_on_day(uint32_t timestamp, int day_offset, int hour) {
  const time_t base = (time_t)timestamp;
  struct tm local;
  if (localtime_r(&base, &local) == NULL) return 0;
  local.tm_mday += day_offset;
  local.tm_hour = hour;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  const time_t result = mktime(&local);
  return result > 0 ? (uint32_t)result : 0;
}

bool minimed_sensor_reminder_status_window(const MinimedSensorReminder *state, uint32_t now,
                                           uint32_t *expiration_out) {
  const uint32_t expiration = minimed_sensor_reminder_expiration(state);
  if (expiration == 0 || state->notified || now >= expiration) return false;
  const time_t expiry_time = (time_t)expiration;
  struct tm expiry_local;
  if (localtime_r(&expiry_time, &expiry_local) == NULL) return false;

  const int minute_of_day = expiry_local.tm_hour * 60 + expiry_local.tm_min;
  const uint32_t start = minute_of_day <= MORNING_CUTOFF_MINUTES
                             ? prv_local_time_on_day(expiration, -1, 18)
                             : prv_local_time_on_day(expiration, 0, 7);
  const uint32_t end = minute_of_day <= MORNING_CUTOFF_MINUTES
                           ? prv_local_time_on_day(expiration, -1, 20)
                           : prv_local_time_on_day(expiration, 0, 9);
  if (start == 0 || end == 0 || now < start || now >= end) return false;
  *expiration_out = expiration;
  return true;
}

bool minimed_sensor_reminder_notification_due(const MinimedSensorReminder *state, uint32_t now) {
  const uint32_t expiration = minimed_sensor_reminder_expiration(state);
  if (expiration == 0 || state->notified || now >= expiration) return false;
  const time_t expiry_time = (time_t)expiration;
  struct tm expiry_local;
  if (localtime_r(&expiry_time, &expiry_local) == NULL) return false;
  const int expiry_minute_of_day = expiry_local.tm_hour * 60 + expiry_local.tm_min;
  const uint32_t target = expiration - 60 * 60;
  const time_t target_time = (time_t)target;
  struct tm target_local;
  if (localtime_r(&target_time, &target_local) == NULL) return false;
  uint32_t notification_time = target;
  if (expiry_minute_of_day <= MORNING_CUTOFF_MINUTES) {
    notification_time = prv_local_time_on_day(expiration, -1, 20);
  } else if (target_local.tm_hour < MORNING_CUTOFF_MINUTES / 60) {
    notification_time = prv_local_time_on_day(expiration, 0, 9);
  } else if (target_local.tm_hour >= NOTIFICATION_QUIET_START) {
    notification_time = prv_local_time_on_day(target, 0, 20);
  }
  return notification_time != 0 && now >= notification_time;
}

void minimed_sensor_reminder_mark_notified(MinimedSensorReminder *state) {
  state->notified = true;
}
