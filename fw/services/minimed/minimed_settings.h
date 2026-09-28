/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Firmware-side home for settings the phone configures through the watchface's Settings page
//! (Clay), as opposed to a plain `#define` flipped in a personal build -- see TESTING.md's old
//! "Local compile-time overrides" section, now replaced by this for anything a user (not just a
//! developer) should be able to change. Persisted so a reboot/reflash keeps the choice; RAM-cached
//! so the annunciation handler (BT host task) never touches flash on the hot path.

//! Load the persisted value, if any, at boot. Call once, before the first annunciation can
//! possibly arrive (minimed_sake_read_init is fine).
void minimed_settings_init(void);

//! Should a pump alert of this category pop up on the watch? `is_low` selects SETTINGS_ALERT_LOW
//! vs SETTINGS_ALERT_OTHER -- see minimed_annunciation_is_low().
bool minimed_settings_alert_enabled(bool is_low);

//! Update the alert mask from a watchface's capability announcement (KEY_SETTINGS_ALERTS).
//! Persists to flash only when the value actually changes.
void minimed_settings_set_alerts(uint8_t mask);

//! Should the hypo (treat-or-wait) model run at all? False skips the computation entirely
//! (prv_hypo_check), not just the send/display -- distinct from a watchface just not asking for
//! CAP_HYPO, which still leaves the sender computing it every cycle for nothing.
bool minimed_settings_hypo_enabled(void);

//! Update the feature mask from a watchface's capability announcement (KEY_SETTINGS_FEATURES).
//! Persists to flash only when the value actually changes.
void minimed_settings_set_features(uint8_t mask);
