/* SPDX-FileCopyrightText: 2026 Core Devices LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>

#include "console/prompt.h"
#include "drivers/rtc.h"
#include "pbl/os/mutex.h"
#include "pbl/services/analytics/backend.h"
#include "pbl/services/system_task.h"
#include "pbl/services/data_logging/data_logging_service.h"
#include "system/logging.h"
#include "system/passert.h"
#include "pbl/util/attributes.h"
#include "pbl/util/build_id.h"
#include "pbl/util/math.h"
#include "pbl/util/size.h"
#include "pbl/util/uuid.h"

PBL_LOG_MODULE_DEFINE(service_analytics, CONFIG_SERVICE_ANALYTICS_LOG_LEVEL);

#define NATIVE_HEARTBEAT_RECORD_VERSION 1

/* Heartbeat record logged to DLS */
struct PACKED native_heartbeat_record {
  uint8_t version;
  uint64_t timestamp;
  uint8_t build_id[BUILD_ID_EXPECTED_LEN];
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) uint32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) int32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) \
  uint32_t metric_##key;                                        \
  uint16_t metric_##key##_scale;
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) \
  int32_t metric_##key;                                       \
  uint16_t metric_##key##_scale;
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) uint32_t metric_##key;
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) char metric_##key[(len) + 1];
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

/* The record is logged as a raw byte blob, so it must have no padding. */
_Static_assert(
    sizeof(struct native_heartbeat_record) ==
        sizeof(uint8_t) + sizeof(uint64_t) + BUILD_ID_EXPECTED_LEN
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) +sizeof(uint32_t)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) +sizeof(int32_t)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) +sizeof(uint32_t) + sizeof(uint16_t)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) +sizeof(int32_t) + sizeof(uint16_t)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) +sizeof(uint32_t)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) +((len) + 1)
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
    ,
    "native_heartbeat_record must be packed (no padding)");

/* Type-specific internal index enums (dense, no gaps) */

enum native_integer_index {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len)
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
  NATIVE_INTEGER_COUNT,
};

enum native_timer_index {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) NATIVE_TIMER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len)
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
  NATIVE_TIMER_COUNT,
};

enum native_string_index {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) NATIVE_STRING_IDX_##key,
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
  NATIVE_STRING_COUNT,
};

/* Mapping tables: global key enum -> type-specific index (-1 if N/A) */

static const int8_t s_key_to_integer[] = {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) NATIVE_INTEGER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) -1,
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

static const int8_t s_key_to_timer[] = {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) NATIVE_TIMER_IDX_##key,
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) -1,
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

static const int8_t s_key_to_string[] = {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) -1,
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) NATIVE_STRING_IDX_##key,
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

/* Type-specific storage */

static int32_t s_integer_values[NATIVE_INTEGER_COUNT];

static struct {
  uint32_t value_ms;
  bool running;
  RtcTicks start_ticks;
} s_timers[NATIVE_TIMER_COUNT];

/* Per-string dedicated buffers (sized to declared length) */
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) static char s_string_##key[(len) + 1];
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING

/* String pointer and length lookup tables */

static char *const s_string_ptrs[] = {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) s_string_##key,
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

static const uint8_t s_string_lens[] = {
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) (len),
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
};

static PebbleMutex *s_mutex;
static DataLoggingSession *s_dls_session;

extern const ElfExternalNote TINTIN_BUILD_ID;

static void prv_record_metrics(struct native_heartbeat_record *record, bool reset) {
  uint32_t timer_value_ms[NATIVE_TIMER_COUNT];

  memset(record, 0, sizeof(*record));

  record->version = NATIVE_HEARTBEAT_RECORD_VERSION;
  record->timestamp = rtc_get_time();
  memcpy(record->build_id, &TINTIN_BUILD_ID.data[TINTIN_BUILD_ID.name_length],
         MIN(BUILD_ID_EXPECTED_LEN, TINTIN_BUILD_ID.data_length));

  RtcTicks now = rtc_get_ticks();
  for (size_t i = 0; i < NATIVE_TIMER_COUNT; i++) {
    timer_value_ms[i] = s_timers[i].value_ms;
    if (s_timers[i].running) {
      RtcTicks elapsed = now - s_timers[i].start_ticks;
      timer_value_ms[i] += (elapsed * 1000) / RTC_TICKS_HZ;
      if (reset) {
        s_timers[i].value_ms = timer_value_ms[i];
        s_timers[i].start_ticks = now;
      }
    }
  }

  /* Build heartbeat record from type-specific storage */
#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) \
  record->metric_##key = (uint32_t)s_integer_values[NATIVE_INTEGER_IDX_##key];
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) \
  record->metric_##key = s_integer_values[NATIVE_INTEGER_IDX_##key];
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)                \
  record->metric_##key = (uint32_t)s_integer_values[NATIVE_INTEGER_IDX_##key]; \
  record->metric_##key##_scale = (scale);
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)        \
  record->metric_##key = s_integer_values[NATIVE_INTEGER_IDX_##key]; \
  record->metric_##key##_scale = (scale);
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key) \
  record->metric_##key = timer_value_ms[NATIVE_TIMER_IDX_##key];
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len)    \
  strncpy(record->metric_##key, s_string_##key, (len)); \
  record->metric_##key[(len)] = '\0';
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING

  if (reset) {
    /* Reset storage for next heartbeat period, keeping running timers active */
    memset(s_integer_values, 0, sizeof(s_integer_values));
    for (size_t i = 0; i < NATIVE_TIMER_COUNT; i++) {
      s_timers[i].value_ms = 0;
    }
    for (size_t i = 0; i < NATIVE_STRING_COUNT; i++) {
      s_string_ptrs[i][0] = '\0';
    }
  }
}

/* Log the drain-relevant subset of the heartbeat, so a battery test can be read off the flash
 * log alone — the DLS record needs a phone session, which is exactly what is missing overnight
 * and in pump mode. Hourly, so INFO is affordable.
 *
 * PBL_LOG allows at most 7 format conversions per line, hence the split and the `cpct`
 * (centi-percent, i.e. the raw scale-100 value) shorthand on everything but the headline SoC.
 * Timers are per-heartbeat and printed in seconds. */
static void prv_log_heartbeat(const struct native_heartbeat_record *record) {
  PBL_LOG_INFO("hb bat soc %" PRIu32 ".%02" PRIu32 " drop %" PRIu32 ".%02" PRIu32 " mv %" PRIu32
               " tte %" PRIu32 "m",
               record->metric_battery_soc_pct / 100, record->metric_battery_soc_pct % 100,
               record->metric_battery_soc_pct_drop / 100, record->metric_battery_soc_pct_drop % 100,
               record->metric_battery_voltage, record->metric_battery_tte_s / 60);

  PBL_LOG_INFO("hb cpu cpct run %" PRIu32 " slp %" PRIu32 "/%" PRIu32 "/%" PRIu32 " idle %" PRIu32,
               record->metric_cpu_running_pct, record->metric_cpu_sleep0_pct,
               record->metric_cpu_sleep1_pct, record->metric_cpu_sleep2_pct,
               record->metric_task_cpu_idle_pct);

  PBL_LOG_INFO("hb ble advs %" PRIu32 "/%" PRIu32 " conns %" PRIu32 "/%" PRIu32 "/%" PRIu32
               " cpct host %" PRIu32 " ctlr %" PRIu32,
               record->metric_ble_adv_short_intvl_time_ms / 1000,
               record->metric_ble_adv_long_intvl_time_ms / 1000,
               record->metric_ble_conn_itvl_min_time_ms / 1000,
               record->metric_ble_conn_itvl_mid_time_ms / 1000,
               record->metric_ble_conn_itvl_max_time_ms / 1000,
               record->metric_task_cpu_bt_host_pct, record->metric_task_cpu_bt_controller_pct);

  PBL_LOG_INFO("hb task cpct main %" PRIu32 " bg %" PRIu32 " tmr %" PRIu32 " app %" PRIu32,
               record->metric_task_cpu_kernel_main_pct,
               record->metric_task_cpu_kernel_background_pct,
               record->metric_task_cpu_new_timers_pct, record->metric_task_cpu_app_pct);

  PBL_LOG_INFO("hb sys stat %" PRIu32 " lowp %" PRIu32 " flashw %" PRIu32 "k flashe %" PRIu32
               "k up %" PRIu32 "m",
               record->metric_stationary_time_ms / 1000, record->metric_low_power_time_ms / 1000,
               record->metric_flash_spi_write_bytes / 1024,
               record->metric_flash_spi_erase_bytes / 1024, record->metric_uptime_s / 60);

  PBL_LOG_INFO("hb ble disc spvn %" PRIu32 " remterm %" PRIu32 " other %" PRIu32,
               record->metric_ble_disconnect_conn_spvn_tmo_count,
               record->metric_ble_disconnect_rem_user_term_count,
               record->metric_ble_disconnect_other_count);
}

void pbl_analytics__native_init(void) {
  s_mutex = mutex_create();
  PBL_ASSERTN(s_mutex != NULL);
}

void pbl_analytics__native_heartbeat(void) {
  struct native_heartbeat_record record;

  mutex_lock(s_mutex);
  prv_record_metrics(&record, true);
  mutex_unlock(s_mutex);

  prv_log_heartbeat(&record);

  if (s_dls_session == NULL) {
    Uuid system_uuid = UUID_SYSTEM;

    s_dls_session = dls_create(DlsSystemTagAnalyticsNativeHeartbeat, DATA_LOGGING_BYTE_ARRAY,
                               sizeof(struct native_heartbeat_record), false, false, &system_uuid);
    PBL_ASSERTN(s_dls_session != NULL);
  }

  DataLoggingResult result = dls_log(s_dls_session, &record, 1);
  if (result != DATA_LOGGING_SUCCESS) {
    PBL_LOG_ERR("Native analytics DLS log failed: %d", result);
  }
}

static void prv_set_signed(enum pbl_analytics_key key, int32_t signed_value) {
  int8_t idx = s_key_to_integer[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  s_integer_values[idx] = signed_value;
  mutex_unlock(s_mutex);
}

static void prv_set_unsigned(enum pbl_analytics_key key, uint32_t unsigned_value) {
  int8_t idx = s_key_to_integer[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  s_integer_values[idx] = (int32_t)unsigned_value;
  mutex_unlock(s_mutex);
}

static void prv_set_string(enum pbl_analytics_key key, const char *value) {
  int8_t idx = s_key_to_string[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  strncpy(s_string_ptrs[idx], value, s_string_lens[idx]);
  s_string_ptrs[idx][s_string_lens[idx]] = '\0';
  mutex_unlock(s_mutex);
}

static void prv_timer_start(enum pbl_analytics_key key) {
  int8_t idx = s_key_to_timer[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  if (!s_timers[idx].running) {
    s_timers[idx].running = true;
    s_timers[idx].start_ticks = rtc_get_ticks();
  }
  mutex_unlock(s_mutex);
}

static void prv_timer_stop(enum pbl_analytics_key key) {
  int8_t idx = s_key_to_timer[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  if (s_timers[idx].running) {
    RtcTicks elapsed = rtc_get_ticks() - s_timers[idx].start_ticks;
    s_timers[idx].value_ms += (int32_t)((elapsed * 1000) / RTC_TICKS_HZ);
    s_timers[idx].running = false;
  }
  mutex_unlock(s_mutex);
}

static void prv_add(enum pbl_analytics_key key, int32_t amount) {
  int8_t idx = s_key_to_integer[key];
  if (idx < 0) {
    return;
  }
  mutex_lock(s_mutex);
  s_integer_values[idx] += amount;
  mutex_unlock(s_mutex);
}

const struct pbl_analytics_backend_ops pbl_analytics__native_ops = {
    .set_signed = prv_set_signed,
    .set_unsigned = prv_set_unsigned,
    .set_string = prv_set_string,
    .timer_start = prv_timer_start,
    .timer_stop = prv_timer_stop,
    .add = prv_add,
};

void command_analytics_native_metrics_dump(void) {
  struct native_heartbeat_record record;
  char buffer[64];

  prv_record_metrics(&record, false);

#define PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED(key) \
  prompt_send_response_fmt(buffer, sizeof(buffer), STRINGIFY(key) "=%" PRIu32, record.metric_##key);
#define PBL_ANALYTICS_METRIC_DEFINE_SIGNED(key) \
  prompt_send_response_fmt(buffer, sizeof(buffer), STRINGIFY(key) "=%" PRId32, record.metric_##key);
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED(key, scale)                            \
  prompt_send_response_fmt(buffer, sizeof(buffer), STRINGIFY(key) "=%" PRIu32 ".%" PRIu32, \
                           record.metric_##key / (scale),                                  \
                           record.metric_##key - (record.metric_##key / (scale)) * (scale));
#define PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED(key, scale)         \
  prompt_send_response_fmt(                                           \
      buffer, sizeof(buffer), STRINGIFY(key) "=%" PRId32 ".%" PRIu32, \
      record.metric_##key / (scale),                                  \
      (uint32_t)(record.metric_##key - (record.metric_##key / (scale)) * (scale)));
#define PBL_ANALYTICS_METRIC_DEFINE_TIMER(key)                                       \
  prompt_send_response_fmt(buffer, sizeof(buffer), STRINGIFY(key) "=%" PRId32 " ms", \
                           record.metric_##key);
#define PBL_ANALYTICS_METRIC_DEFINE_STRING(key, len) \
  prompt_send_response_fmt(buffer, sizeof(buffer), STRINGIFY(key) "=%s", record.metric_##key);
#include "pbl/services/analytics/analytics.def"
#undef PBL_ANALYTICS_METRIC_DEFINE_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_UNSIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_SCALED_SIGNED
#undef PBL_ANALYTICS_METRIC_DEFINE_TIMER
#undef PBL_ANALYTICS_METRIC_DEFINE_STRING
}