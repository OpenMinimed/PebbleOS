/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_service.h"

#include <stdio.h>
#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include "popups/minimed_sake_spike_ui.h"
#include <system/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

#define SAKE_MESSAGE_SIZE 20

// Advertised as 16-bit Service Class UUID 0xfe82 (Medtronic SAKE, pairing).
static const ble_uuid16_t s_sake_svc_uuid = BLE_UUID16_INIT(0xfe82);

// SAKE Port characteristic 0000fe82-0000-1000-0000-009132591325 -- Medtronic's
// custom 128-bit base (NOT the standard BT base). Little-endian byte order.
static const ble_uuid128_t s_sake_port_chr_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x82, 0xfe, 0x00, 0x00);

static uint16_t s_sake_port_val_handle;

// The wake-up notification is deferred onto the BT task via this callout (see handle_subscribe).
static struct ble_npl_callout s_wakeup_co;
static uint16_t s_wakeup_conn;

// Medtronic's custom Device Information service on the VENDOR base:
// 00000900-0000-1000-0000-009132591325. The pump matches this during discovery and refuses to
// pair when only the standard 0x180A DIS is present. Its characteristics use standard SIG 16-bit
// UUIDs; values are placeholders (the pump doesn't validate them -- SAKE is the real auth).
static const ble_uuid128_t s_dis_svc_uuid =
    BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00,
                     0x00, 0x10, 0x00, 0x00, 0x00, 0x09, 0x00, 0x00);

static const ble_uuid16_t s_dis_manuf_uuid = BLE_UUID16_INIT(0x2A29);
static const ble_uuid16_t s_dis_model_uuid = BLE_UUID16_INIT(0x2A24);
static const ble_uuid16_t s_dis_serial_uuid = BLE_UUID16_INIT(0x2A25);
static const ble_uuid16_t s_dis_fw_uuid = BLE_UUID16_INIT(0x2A26);
static const ble_uuid16_t s_dis_hw_uuid = BLE_UUID16_INIT(0x2A27);
static const ble_uuid16_t s_dis_sw_uuid = BLE_UUID16_INIT(0x2A28);
static const ble_uuid16_t s_dis_sysid_uuid = BLE_UUID16_INIT(0x2A23);
static const ble_uuid16_t s_dis_pnp_uuid = BLE_UUID16_INIT(0x2A50);
static const ble_uuid16_t s_dis_reg_uuid = BLE_UUID16_INIT(0x2A2A);

static int prv_sake_port_access(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint8_t buf[SAKE_MESSAGE_SIZE] = {0};
  uint16_t len = 0;
  int rc = ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
  if (rc != 0) {
    PBL_LOG_ERR("SAKE: write copy failed 0x%04x", (uint16_t)rc);
    return BLE_ATT_ERR_UNLIKELY;
  }

  // The pump's first handshake write is 20 zero bytes (per SAKE init sequence).
  bool all_zero = true;
  for (uint16_t i = 0; i < len; i++) {
    if (buf[i] != 0) {
      all_zero = false;
      break;
    }
  }
  PBL_LOG_INFO("SAKE: pump WRITE conn=%d len=%u%s [%02x %02x %02x %02x]", conn_handle, len,
               all_zero ? " (all-zero)" : "", buf[0], buf[1], buf[2], buf[3]);
  minimed_sake_spike_report(MinimedSakeStageWrote);
  char line[32];
  snprintf(line, sizeof(line), "wrote %u:%02x %02x %02x %02x", len, buf[0], buf[1], buf[2], buf[3]);
  minimed_sake_log(line);
  return 0;
}

static int prv_dis_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  static const uint8_t zeros[8] = {0};
  const ble_uuid_t *u = ctxt->chr->uuid;
  const uint8_t *val = zeros;
  uint16_t len = 0;

  if (ble_uuid_cmp(u, &s_dis_manuf_uuid.u) == 0) {
    val = (const uint8_t *)"Pebble";
    len = 6;
  } else if (ble_uuid_cmp(u, &s_dis_model_uuid.u) == 0) {
    val = (const uint8_t *)"Mobile";
    len = 6;
  } else if (ble_uuid_cmp(u, &s_dis_serial_uuid.u) == 0) {
    val = (const uint8_t *)"PB";
    len = 2;
  } else if (ble_uuid_cmp(u, &s_dis_fw_uuid.u) == 0 || ble_uuid_cmp(u, &s_dis_hw_uuid.u) == 0 ||
             ble_uuid_cmp(u, &s_dis_sw_uuid.u) == 0) {
    val = (const uint8_t *)"0";
    len = 1;
  } else if (ble_uuid_cmp(u, &s_dis_sysid_uuid.u) == 0) {
    len = 8;  // System ID: 8 zero bytes
  } else if (ble_uuid_cmp(u, &s_dis_pnp_uuid.u) == 0) {
    len = 7;  // PnP ID: 7 zero bytes
  } else if (ble_uuid_cmp(u, &s_dis_reg_uuid.u) == 0) {
    len = 0;  // Regulatory cert list: empty
  }

  int rc = os_mbuf_append(ctxt->om, val, len);
  return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define DIS_READ_CHR(uuid_ptr) \
  { .uuid = (uuid_ptr), .access_cb = prv_dis_access, .flags = BLE_GATT_CHR_F_READ }

static const struct ble_gatt_svc_def s_sake_svcs[] = {
    {
        // Medtronic custom Device Information service (required for pairing).
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_dis_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                DIS_READ_CHR(&s_dis_manuf_uuid.u),
                DIS_READ_CHR(&s_dis_model_uuid.u),
                DIS_READ_CHR(&s_dis_serial_uuid.u),
                DIS_READ_CHR(&s_dis_fw_uuid.u),
                DIS_READ_CHR(&s_dis_hw_uuid.u),
                DIS_READ_CHR(&s_dis_sw_uuid.u),
                DIS_READ_CHR(&s_dis_sysid_uuid.u),
                DIS_READ_CHR(&s_dis_pnp_uuid.u),
                DIS_READ_CHR(&s_dis_reg_uuid.u),
                {
                    0, /* no more characteristics */
                },
            },
    },
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_sake_svc_uuid.u,
        .characteristics =
            (struct ble_gatt_chr_def[]){
                {
                    .uuid = &s_sake_port_chr_uuid.u,
                    .access_cb = prv_sake_port_access,
                    .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
                    .val_handle = &s_sake_port_val_handle,
                },
                {
                    0, /* no more characteristics */
                },
            },
    },
    {
        0, /* no more services */
    },
};

// Runs on the BT host task ~120ms after the subscribe (deferred via callout).
static void prv_send_wakeup(struct ble_npl_event *ev) {
  uint8_t wakeup[SAKE_MESSAGE_SIZE] = {0};
  struct os_mbuf *om = ble_hs_mbuf_from_flat(wakeup, sizeof(wakeup));
  if (!om) {
    minimed_sake_log("wakeup mbuf fail");
    return;
  }
  int rc = ble_gatts_notify_custom(s_wakeup_conn, s_sake_port_val_handle, om);
  char line[32];
  snprintf(line, sizeof(line), "wakeup notify rc=0x%04x", (uint16_t)rc);
  minimed_sake_log(line);
}

void minimed_sake_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify) {
  if (attr_handle != s_sake_port_val_handle || !notify) {
    return;
  }

  minimed_sake_spike_report(MinimedSakeStageSubscribed);
  // Defer the 20-zero wake-up ~120ms. Sending it synchronously here is too early -- it can go out
  // before the CCCD write-response, so the pump never registers notifications, waits, then drops
  // (disc reason 0x13). The working Android bridge likewise posts it to a worker thread.
  s_wakeup_conn = conn_handle;
  ble_npl_callout_reset(&s_wakeup_co, ble_npl_time_ms_to_ticks32(120));
}

uint8_t minimed_sake_build_adv(uint8_t *buf, uint8_t buf_len) {
  // Flags: LE General Discoverable + BR/EDR not supported.
  // Complete 16-bit Service Class UUID list: 0xfe82 (SAKE, pairing).
  // Manufacturer data: Medtronic company 0x01f9, payload = 0x00 + "Mobile PB" + 0x00.
  // Matches the working Android bridge; the pump reads the name from mfr data, not the GAP name.
  static const uint8_t adv[] = {
      0x02, 0x01, 0x06,
      0x03, 0x03, 0x82, 0xfe,
      0x0e, 0xff, 0xf9, 0x01, 0x00, 'M', 'o', 'b', 'i', 'l', 'e', ' ', 'P', 'B', 0x00,
  };
  if (buf_len < sizeof(adv)) {
    return 0;
  }
  memcpy(buf, adv, sizeof(adv));
  return sizeof(adv);
}

int minimed_sake_service_init(void) {
  ble_npl_callout_init(&s_wakeup_co, nimble_port_get_dflt_eventq(), prv_send_wakeup, NULL);

  int rc = ble_gatts_count_cfg(s_sake_svcs);
  if (rc != 0) {
    PBL_LOG_ERR("SAKE: count_cfg failed 0x%04x", (uint16_t)rc);
    return rc;
  }
  rc = ble_gatts_add_svcs(s_sake_svcs);
  if (rc != 0) {
    PBL_LOG_ERR("SAKE: add_svcs failed 0x%04x", (uint16_t)rc);
    return rc;
  }
  PBL_LOG_INFO("SAKE: service registered (spike)");
  minimed_sake_spike_report(MinimedSakeStageAdvertising);
  return 0;
}
