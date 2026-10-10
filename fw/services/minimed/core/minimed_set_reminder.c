/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#define _POSIX_C_SOURCE 200809L

#include "minimed_set_reminder.h"

#include <stddef.h>
#include <time.h>

#define RESERVOIR_CHANGE_MIN_MU 1000
#define THIRD_DAY_OFFSET 3

uint32_t minimed_set_reminder_day_key(uint32_t now) {
  const time_t time = (time_t)now;
  struct tm local;
  if (localtime_r(&time, &local) == NULL) return 0;
  return (uint32_t)(local.tm_year + 1900) * 10000 + (uint32_t)(local.tm_mon + 1) * 100 +
         (uint32_t)local.tm_mday;
}

void minimed_set_reminder_init(MinimedSetReminder *state, uint32_t set_started_at,
                               uint32_t last_notification_day) {
  *state = (MinimedSetReminder){
      .set_started_at = set_started_at,
      .last_notification_day = last_notification_day,
  };
}

bool minimed_set_reminder_observe_reservoir(MinimedSetReminder *state, int32_t reservoir_mu,
                                            uint32_t now) {
  if (!state->have_reservoir) {
    // There is no reliable set-start date until a reservoir increase is observed.
    state->last_reservoir_mu = reservoir_mu;
    state->have_reservoir = true;
    return false;
  }

  const int32_t previous = state->last_reservoir_mu;
  state->last_reservoir_mu = reservoir_mu;
  if ((int64_t)reservoir_mu <= (int64_t)previous + RESERVOIR_CHANGE_MIN_MU) return false;

  state->set_started_at = now;
  state->last_notification_day = 0;
  return true;
}

static int32_t prv_calendar_day_number(uint32_t timestamp) {
  const time_t time = (time_t)timestamp;
  struct tm local;
  if (localtime_r(&time, &local) == NULL) return -1;
  int32_t year = local.tm_year + 1900;
  const int32_t month = local.tm_mon + 1;
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t year_of_era = (uint32_t)(year - era * 400);
  const uint32_t day_of_year =
      (153 * (uint32_t)(month + (month > 2 ? -3 : 9)) + 2) / 5 + (uint32_t)local.tm_mday - 1;
  const uint32_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
  return era * 146097 + (int32_t)day_of_era;
}

uint32_t minimed_set_reminder_age_days(const MinimedSetReminder *state, uint32_t now) {
  if (state->set_started_at == 0) return 0;
  const int32_t start_day = prv_calendar_day_number(state->set_started_at);
  const int32_t current_day = prv_calendar_day_number(now);
  return start_day < 0 || current_day < start_day ? 0 : (uint32_t)(current_day - start_day);
}

bool minimed_set_reminder_due(const MinimedSetReminder *state, uint32_t now) {
  return minimed_set_reminder_age_days(state, now) >= THIRD_DAY_OFFSET;
}

bool minimed_set_reminder_morning_window(const MinimedSetReminder *state, uint32_t now) {
  if (!minimed_set_reminder_due(state, now)) return false;
  const time_t time = (time_t)now;
  struct tm local;
  if (localtime_r(&time, &local) == NULL) return false;
  return local.tm_hour >= 7 && local.tm_hour < 9;
}

bool minimed_set_reminder_evening_due(const MinimedSetReminder *state, uint32_t now) {
  if (!minimed_set_reminder_due(state, now)) return false;
  const time_t time = (time_t)now;
  struct tm local;
  if (localtime_r(&time, &local) == NULL || local.tm_hour != 20 || local.tm_min != 0) {
    return false;
  }
  return state->last_notification_day != minimed_set_reminder_day_key(now);
}

void minimed_set_reminder_mark_notified(MinimedSetReminder *state, uint32_t now) {
  state->last_notification_day = minimed_set_reminder_day_key(now);
}
