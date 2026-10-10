/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  uint32_t set_started_at;
  uint32_t last_notification_day;
  int32_t last_reservoir_mu;
  bool have_reservoir;
} MinimedSetReminder;

void minimed_set_reminder_init(MinimedSetReminder *state, uint32_t set_started_at,
                               uint32_t last_notification_day);

//! Track reservoir values in milli-IU. Returns true when an increase marks a new set.
bool minimed_set_reminder_observe_reservoir(MinimedSetReminder *state, int32_t reservoir_mu,
                                            uint32_t now);

//! Calendar days elapsed since set start. The change date is day 0.
uint32_t minimed_set_reminder_age_days(const MinimedSetReminder *state, uint32_t now);

//! True from the third calendar day after the set started.
bool minimed_set_reminder_due(const MinimedSetReminder *state, uint32_t now);

//! Morning status window: 07:00 inclusive to 09:00 exclusive, local time.
bool minimed_set_reminder_morning_window(const MinimedSetReminder *state, uint32_t now);

//! True once at 20:00 local time on each eligible calendar day.
bool minimed_set_reminder_evening_due(const MinimedSetReminder *state, uint32_t now);
void minimed_set_reminder_mark_notified(MinimedSetReminder *state, uint32_t now);

//! Current local date as YYYYMMDD, or zero if conversion fails.
uint32_t minimed_set_reminder_day_key(uint32_t now);
