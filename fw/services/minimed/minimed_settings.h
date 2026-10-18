/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! User settings set in the MiniMed app's menu, as opposed to Kconfig choices made at build time.
//! Persisted so a reboot or reflash keeps them; cached in RAM so the annunciation handler (BT host
//! task) never touches flash on the hot path.

//! Alert popups (the alert mask).
#define MINIMED_ALERT_LOW             0x01 //!< predicted-low, low and severe-low pump alerts
#define MINIMED_ALERT_OTHER           0x02 //!< every other pump alert (reservoir, battery, ...)
#define MINIMED_ALERT_HYPO_MODEL      0x04 //!< the hypo model's own alert when it reaches TREAT
#define MINIMED_ALERT_HYPO_DND_BYPASS 0x08 //!< let that alert ring through Quiet Time

//! Features that can be switched off at runtime (the feature mask). All default on.
#define MINIMED_FEATURE_HYPO            0x01 //!< run the hypo (treat-or-wait) model at all
#define MINIMED_FEATURE_PREDICTOR       0x02 //!< run the 30-minute forecast
#define MINIMED_FEATURE_SET_REMINDER    0x04 //!< infusion set reminder (status line and popup)
#define MINIMED_FEATURE_SENSOR_REMINDER 0x08 //!< sensor expiry reminder (status line and popup)

//! Load the persisted values, if any, at boot. Call once, before the first annunciation can
//! possibly arrive (minimed_sake_read_init is fine).
void minimed_settings_init(void);

//! Should a pump alert of this category pop up on the watch? `is_low` selects MINIMED_ALERT_LOW
//! vs MINIMED_ALERT_OTHER -- see minimed_annunciation_is_low().
bool minimed_settings_alert_enabled(bool is_low);

//! Should the hypo model's own "Low predicted" alert pop up (MINIMED_ALERT_HYPO_MODEL)?
bool minimed_settings_hypo_alert_enabled(void);

//! Should that alert ring through Quiet Time/Do Not Disturb instead of being hidden with
//! everything else (MINIMED_ALERT_HYPO_DND_BYPASS)? Meaningless if the alert itself is off.
bool minimed_settings_hypo_dnd_bypass_enabled(void);

//! Is this MINIMED_FEATURE_* switched on? An off feature skips its computation, not just its
//! output, where that is possible.
bool minimed_settings_feature_enabled(uint8_t feature);

//! The raw masks, for the MiniMed app's menu.
uint8_t minimed_settings_get_alerts(void);
uint8_t minimed_settings_get_features(void);

//! Replace a mask. Persists to flash only when the value actually changes.
void minimed_settings_set_alerts(uint8_t mask);
void minimed_settings_set_features(uint8_t mask);

//! Persist the set-reminder start time and last notification date (YYYYMMDD).
void minimed_settings_get_set_reminder(uint32_t *set_started_at, uint32_t *last_notification_day);
void minimed_settings_set_set_reminder(uint32_t set_started_at, uint32_t last_notification_day);

//! Persist sensor session start, expiration duration, and whether its notification was sent.
void minimed_settings_get_sensor_reminder(uint32_t *session_start, uint16_t *duration_minutes,
                                          bool *notified);
void minimed_settings_set_sensor_reminder(uint32_t session_start, uint16_t duration_minutes,
                                          bool notified);
