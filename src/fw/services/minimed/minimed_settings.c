/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_settings.h"

#include "kernel/event_loop.h"
#include "pbl/services/settings/settings_file.h"
#include "pebble_glucose_protocol.h"
#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Shares the pump-pairing settings file (minimed_sake_service.c) rather than opening a second one
// -- this is one more small key in the same 256-byte budget, not a reason to pay for another file.
#define MINIMED_SETTINGS_FILE "minimedsake"
#define MINIMED_SETTINGS_MAX_SIZE 256
static const char s_alerts_key[] = "alerts";
static const char s_features_key[] = "features";

// Default until the watchface's first announcement carries a real value: low-BG alerts only,
// matching the maintainer's stated preference (issue #15) rather than the old MINIMED_ALERT_POPUPS
// flag's all-or-nothing choice.
static uint8_t s_alerts_mask = SETTINGS_ALERT_LOW | SETTINGS_ALERT_HYPO_MODEL;
// Default: the hypo model runs, same as before this key existed.
static uint8_t s_features_mask = SETTINGS_FEATURE_HYPO;

void minimed_settings_init(void) {
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    return;  // no file yet -> the default above stands
  }
  uint8_t v;
  if (settings_file_get(&fd, s_alerts_key, sizeof(s_alerts_key), &v, sizeof(v)) == S_SUCCESS) {
    s_alerts_mask = v;
  }
  if (settings_file_get(&fd, s_features_key, sizeof(s_features_key), &v, sizeof(v)) == S_SUCCESS) {
    s_features_mask = v;
  }
  settings_file_close(&fd);
}

bool minimed_settings_alert_enabled(bool is_low) {
  return (s_alerts_mask & (is_low ? SETTINGS_ALERT_LOW : SETTINGS_ALERT_OTHER)) != 0;
}

bool minimed_settings_hypo_alert_enabled(void) {
  return (s_alerts_mask & SETTINGS_ALERT_HYPO_MODEL) != 0;
}

// Flash write deferred to KernelMain: minimed_settings_set_alerts runs on the BT host task
// (parsing the watchface's announcement), which should not block on filesystem I/O.
static void prv_store_alerts_cb(void *data) {
  const uint8_t v = (uint8_t)(uintptr_t)data;
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    PBL_LOG_WRN("minimed: settings persist open fail");
    return;
  }
  if (settings_file_set(&fd, s_alerts_key, sizeof(s_alerts_key), &v, sizeof(v)) != S_SUCCESS) {
    PBL_LOG_WRN("minimed: settings persist set fail");
  }
  settings_file_close(&fd);
}

void minimed_settings_set_alerts(uint8_t mask) {
  if (mask == s_alerts_mask) {
    return;  // no change -> no flash write (the watchface re-announces this every couple minutes)
  }
  PBL_LOG_INFO("minimed: settings alerts 0x%02x -> 0x%02x", s_alerts_mask, mask);
  s_alerts_mask = mask;
  launcher_task_add_callback(prv_store_alerts_cb, (void *)(uintptr_t)mask);
}

bool minimed_settings_hypo_enabled(void) {
  return (s_features_mask & SETTINGS_FEATURE_HYPO) != 0;
}

static void prv_store_features_cb(void *data) {
  const uint8_t v = (uint8_t)(uintptr_t)data;
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    PBL_LOG_WRN("minimed: settings persist open fail");
    return;
  }
  if (settings_file_set(&fd, s_features_key, sizeof(s_features_key), &v, sizeof(v)) != S_SUCCESS) {
    PBL_LOG_WRN("minimed: settings persist set fail");
  }
  settings_file_close(&fd);
}

void minimed_settings_set_features(uint8_t mask) {
  if (mask == s_features_mask) {
    return;
  }
  PBL_LOG_INFO("minimed: settings features 0x%02x -> 0x%02x", s_features_mask, mask);
  s_features_mask = mask;
  launcher_task_add_callback(prv_store_features_cb, (void *)(uintptr_t)mask);
}
