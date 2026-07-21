/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_read.h"

#include <stdio.h>
#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include "minimed_sake_service.h"
#include "popups/minimed_sake_spike_ui.h"
#include <system/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

// Standard SIG 16-bit UUIDs for the pump's CGM service (Documentation/cgm-service.md; the bridge's
// MedtronicProtocol.kt). The pump exposes these as a GATT server over the post-handshake link.
#define CGM_SERVICE_UUID 0x181F
#define CGM_MEASUREMENT_UUID 0x2AA7  // notify, SAKE-encrypted records
#define CGM_FEATURE_UUID 0x2AA8      // read, plaintext (E2E-CRC flag)
#define RACP_UUID 0x2A52             // write/indicate, plaintext control point

// RACP "Report Stored Records: Last Record" and its success response (Bluetooth SIG RACP).
static const uint8_t RACP_REPORT_LAST_RECORD[] = {0x01, 0x06};
static const uint8_t RACP_REPORT_SUCCESS[] = {0x06, 0x00, 0x01, 0x01};

// Re-poll the latest record on this cadence. The sensor updates ~every 5 min; polling faster just
// re-shows the current value and keeps the on-watch reading fresh within one interval.
#define POLL_INTERVAL_SECS 60

static struct ble_npl_callout s_read_co;
static struct ble_npl_callout s_poll_co;
static uint16_t s_conn;
static uint16_t s_cgm_start, s_cgm_end;
static uint16_t s_h_measurement, s_h_feature, s_h_racp;

// Reassembly buffer for a (decrypted) CGM Measurement record. The record's byte 0 is its total
// length, so accumulate decrypted fragments until we have that many bytes.
static uint8_t s_rec[64];
static uint8_t s_rec_len;

// Decode an IEEE-11073 SFLOAT (MedFloat16) to an integer mg/dL. Returns INT32_MIN for the
// NaN/NRes/Inf sentinels (no usable value). Glucose normally has exponent 0.
static int32_t prv_decode_medfloat16(uint16_t raw) {
  uint16_t m12 = raw & 0x0FFF;
  if (m12 == 0x07FF || m12 == 0x0800 || m12 == 0x0801 || m12 == 0x07FE || m12 == 0x0802) {
    return INT32_MIN;
  }
  int exp = (raw >> 12) & 0x0F;
  if (exp & 0x8) exp -= 0x10;
  int32_t mant = raw & 0x0FFF;
  if (mant & 0x800) mant -= 0x1000;
  for (; exp > 0; exp--) mant *= 10;
  for (; exp < 0; exp++) mant /= 10;
  return mant;
}

static void prv_parse_and_show(void) {
  // Mandatory prefix: size(1) | flags(1) | glucose SFLOAT(2) | time offset(2).
  if (s_rec_len < 6 || s_rec[0] != s_rec_len) {
    char line[32];
    snprintf(line, sizeof(line), "bad CGM rec len=%u sz=%u", s_rec_len, s_rec[0]);
    minimed_sake_log(line);
    return;
  }
  uint16_t raw = (uint16_t)(s_rec[2] | (s_rec[3] << 8));
  int32_t mgdl = prv_decode_medfloat16(raw);
  char line[32];
  if (mgdl == INT32_MIN) {
    minimed_sake_log("SG: no value (warmup?)");
    return;
  }
  // mg/dL -> mmol/L to one decimal, rounded. Uses 18.0182 (not the textbook 18.0156): the bridge's
  // GlucoseFormat picked this constant specifically so the rounded value matches the Medtronic
  // pump's own display (differs at rounding boundaries, e.g. 100 mg/dL -> 5.5, not 5.6). Scaled
  // integer math (no float printf on the watch); +90091 = 180182/2 for round-half-up.
  int32_t tenths = (mgdl * 100000 + 90091) / 180182;
  snprintf(line, sizeof(line), "*** BG %ld.%ld mmol/L ***", (long)(tenths / 10), (long)(tenths % 10));
  minimed_sake_log(line);
}

// Feed an inbound pump notification/indication. Returns true if consumed (a CGM char we own).
bool minimed_sake_read_handle_notify(uint16_t attr_handle, const uint8_t *data, uint16_t len) {
  if (s_h_measurement != 0 && attr_handle == s_h_measurement) {
    uint8_t plain[24];
    uint16_t plain_len = 0;
    if (!minimed_sake_decrypt(data, len, plain, &plain_len)) {
      minimed_sake_log("CGM decrypt failed");
      return true;
    }
    if (s_rec_len + plain_len > sizeof(s_rec)) {
      s_rec_len = 0;  // overflow guard; abandon this frame
    }
    memcpy(s_rec + s_rec_len, plain, plain_len);
    s_rec_len += plain_len;
    if (s_rec_len >= 1 && s_rec_len >= s_rec[0]) {
      prv_parse_and_show();
      s_rec_len = 0;
    }
    return true;
  }
  if (s_h_racp != 0 && attr_handle == s_h_racp) {
    // Success is the common case and stays quiet so the log keeps scrolling BG readings; only an
    // unexpected RACP response is worth a line.
    bool ok = (len == sizeof(RACP_REPORT_SUCCESS) &&
               memcmp(data, RACP_REPORT_SUCCESS, len) == 0);
    if (!ok) {
      minimed_sake_log("RACP unexpected resp");
    }
    return true;
  }
  return false;
}

static int prv_racp_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP write err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
  }
  return 0;
}

// Issue one RACP "report last stored record"; the record arrives via measurement notifications.
static void prv_do_poll(void) {
  s_rec_len = 0;
  int rc = ble_gattc_write_flat(s_conn, s_h_racp, RACP_REPORT_LAST_RECORD,
                                sizeof(RACP_REPORT_LAST_RECORD), prv_racp_write_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP write rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
}

static void prv_poll_timer_cb(struct ble_npl_event *ev) {
  prv_do_poll();
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
}

static int prv_sub_racp_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  // Both characteristics are subscribed; start polling the latest record continuously.
  minimed_sake_log("polling BG every 60s");
  prv_do_poll();
  ble_npl_callout_reset(&s_poll_co, ble_npl_time_ms_to_ticks32(POLL_INTERVAL_SECS * 1000));
  return 0;
}

static int prv_sub_meas_cb(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    char line[32];
    snprintf(line, sizeof(line), "meas sub err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  // Subscribe to RACP indications (CCCD = value handle + 1 for these regular 3-handle chars).
  static const uint8_t indicate[] = {0x02, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_racp + 1, indicate, sizeof(indicate),
                                prv_sub_racp_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "RACP sub rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
  return 0;
}

static int prv_read_feature_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  char line[32];
  if (error->status != 0) {
    snprintf(line, sizeof(line), "feat read err=0x%04x", (uint16_t)error->status);
    minimed_sake_log(line);
    return 0;
  }
  uint16_t n = (attr && attr->om) ? attr->om->om_len : 0;
  const uint8_t *d = (attr && attr->om) ? attr->om->om_data : NULL;
  snprintf(line, sizeof(line), "CGM feat %u:%02x %02x", n, n > 0 ? d[0] : 0, n > 1 ? d[1] : 0);
  minimed_sake_log(line);

  if (s_h_measurement == 0 || s_h_racp == 0) {
    minimed_sake_log("missing meas/RACP chr");
    return 0;
  }
  // Subscribe to CGM Measurement notifications (CCCD = value handle + 1). Chaining the RACP write
  // behind these subscribe write-responses guarantees notifications are effective first.
  static const uint8_t notify[] = {0x01, 0x00};
  int rc = ble_gattc_write_flat(s_conn, s_h_measurement + 1, notify, sizeof(notify),
                                prv_sub_meas_cb, NULL);
  if (rc != 0) {
    snprintf(line, sizeof(line), "meas sub rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
  return 0;
}

static int prv_disc_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_chr *chr, void *arg) {
  char line[32];
  if (error->status == 0 && chr) {
    uint16_t u = (chr->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&chr->uuid.u) : 0;
    if (u == CGM_MEASUREMENT_UUID) {
      s_h_measurement = chr->val_handle;
    } else if (u == CGM_FEATURE_UUID) {
      s_h_feature = chr->val_handle;
    } else if (u == RACP_UUID) {
      s_h_racp = chr->val_handle;
    }
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    snprintf(line, sizeof(line), "chrs: m=%u f=%u r=%u", s_h_measurement, s_h_feature, s_h_racp);
    minimed_sake_log(line);
    if (s_h_feature != 0) {
      int rc = ble_gattc_read(s_conn, s_h_feature, prv_read_feature_cb, NULL);
      if (rc != 0) {
        snprintf(line, sizeof(line), "feat read rc=0x%04x", (uint16_t)rc);
        minimed_sake_log(line);
      }
    } else {
      minimed_sake_log("no CGM feature chr!");
    }
    return 0;
  }
  snprintf(line, sizeof(line), "chr disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  return 0;
}

static int prv_disc_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                           const struct ble_gatt_svc *service, void *arg) {
  char line[32];
  if (error->status == 0 && service) {
    s_cgm_start = service->start_handle;
    s_cgm_end = service->end_handle;
    return 0;
  }
  if (error->status == BLE_HS_EDONE) {
    if (s_cgm_start == 0) {
      minimed_sake_log("no CGM svc 181F!");
      return 0;
    }
    int rc = ble_gattc_disc_all_chrs(s_conn, s_cgm_start, s_cgm_end, prv_disc_chr_cb, NULL);
    if (rc != 0) {
      snprintf(line, sizeof(line), "chr disc rc=0x%04x", (uint16_t)rc);
      minimed_sake_log(line);
    }
    return 0;
  }
  snprintf(line, sizeof(line), "svc disc err=0x%04x", (uint16_t)error->status);
  minimed_sake_log(line);
  return 0;
}

// Runs on the BT host task a beat after the handshake, so GATT-client procedures aren't started
// synchronously inside the SAKE-port write callback that completed the handshake.
static void prv_read_kickoff(struct ble_npl_event *ev) {
  minimed_sake_log("discovering CGM svc...");
  const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(CGM_SERVICE_UUID);
  int rc = ble_gattc_disc_svc_by_uuid(s_conn, &svc_uuid.u, prv_disc_svc_cb, NULL);
  if (rc != 0) {
    char line[32];
    snprintf(line, sizeof(line), "svc disc rc=0x%04x", (uint16_t)rc);
    minimed_sake_log(line);
  }
}

void minimed_sake_read_init(void) {
  ble_npl_callout_init(&s_read_co, nimble_port_get_dflt_eventq(), prv_read_kickoff, NULL);
  ble_npl_callout_init(&s_poll_co, nimble_port_get_dflt_eventq(), prv_poll_timer_cb, NULL);
}

void minimed_sake_read_start(uint16_t conn_handle) {
  ble_npl_callout_stop(&s_poll_co);
  s_conn = conn_handle;
  s_cgm_start = s_cgm_end = 0;
  s_h_measurement = s_h_feature = s_h_racp = 0;
  s_rec_len = 0;
  ble_npl_callout_reset(&s_read_co, ble_npl_time_ms_to_ticks32(250));
}

void minimed_sake_read_stop(void) {
  ble_npl_callout_stop(&s_read_co);
  ble_npl_callout_stop(&s_poll_co);
}
