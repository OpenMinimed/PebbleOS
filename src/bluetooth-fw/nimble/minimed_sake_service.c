/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_service.h"

#include <stdio.h>
#include <string.h>

#include "host/ble_gatt.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_hci.h"
#include "host/ble_uuid.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"

#include "minimed_sake_crypto.h"
#include "minimed_sake_read.h"
#include "popups/minimed_sake_spike_ui.h"
#include "kernel/event_loop.h"
#include "pbl/services/settings/settings_file.h"
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

// Server replies (and the initial wake-up) are always notified from a callout on the BT task,
// never synchronously inside the GATT write/subscribe callback: sending before the peer's
// write-response goes out makes the pump miss it and drop the link (disc 0x13).
static struct ble_npl_callout s_notify_co;
static uint16_t s_notify_conn;
static uint8_t s_notify_buf[SAKE_MESSAGE_SIZE];

// SAKE server-role handshake state. The crypto + state machine live in minimed_sake_crypto.c and
// are byte-verified on the host against OpenMinimed's captured 780G trace (tools/minimed_sake_hosttest).
// KEYDB_PUMP_EXTRACTED: the 780G-model-static key database (same bytes the working Android bridge
// uses at runtime via org.openminimed.sake.Constants).
static const uint8_t s_pump_keydb_bytes[] = {
    0xf7, 0x59, 0x95, 0xe7, 0x04, 0x01, 0x01, 0x1b, 0xc1, 0xbf, 0x7c, 0xbf,
    0x36, 0xfa, 0x1e, 0x23, 0x67, 0xd7, 0x95, 0xff, 0x09, 0x21, 0x19, 0x03,
    0xda, 0x6a, 0xfb, 0xe9, 0x86, 0xb6, 0x50, 0xf1, 0x41, 0x79, 0xc0, 0xe6,
    0x85, 0x2e, 0x0c, 0xe3, 0x93, 0x78, 0x10, 0x78, 0xff, 0xc6, 0xf5, 0x19,
    0x19, 0xe2, 0xea, 0xef, 0xbd, 0xe6, 0x9b, 0x8e, 0xca, 0x21, 0xe4, 0x1a,
    0xb5, 0x9b, 0x88, 0x1a, 0x0b, 0xea, 0x02, 0x86, 0xea, 0x91, 0xdc, 0x75,
    0x82, 0xa8, 0x6a, 0x71, 0x4e, 0x17, 0x37, 0xf5, 0x58, 0xf0, 0xd6, 0x6d,
    0xc1, 0x89, 0x5c,
};
static sake_keydb s_keydb;
static bool s_keydb_ok;
static sake_server s_server;

// True once a SAKE handshake has completed, i.e. the pump holds a bond (LTK + our IRK) to this
// watch. Selects the FE81 (reconnect) advert instead of FE82 (first-pair). Cleared by "forget
// pump" in the spike app. Written on the BT host task, read on the BT task and the app task --
// a bool, torn reads impossible. Persisted to a settings file so a reboot/reflash goes straight
// back to FE81 and the pump reconnects unattended (the NimBLE bond already persists).
static bool s_pump_paired;

// Pump identity address, captured (RAM-only) at handshake completion so the v29 advert diagnostics
// can label a later connection PUMP vs phone. Not persisted: the pump re-runs the full SAKE
// handshake on every reconnect, so it is re-captured each cycle. Written and read on the NimBLE
// host task, so no locking.
static ble_addr_t s_pump_id_addr;
static bool s_pump_addr_known;

#define MINIMED_SETTINGS_FILE "minimedsake"
#define MINIMED_SETTINGS_MAX_SIZE 256
static const char s_paired_setting_key[] = "paired";

static void prv_load_pump_paired(void) {
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    return;  // no file yet -> stay unpaired (FE82)
  }
  uint8_t v = 0;
  if (settings_file_get(&fd, s_paired_setting_key, sizeof(s_paired_setting_key), &v, sizeof(v)) ==
      S_SUCCESS) {
    s_pump_paired = (v != 0);
  }
  settings_file_close(&fd);
}

// Flash write deferred to KernelMain: the flag flips on the BT host task (handshake) or the app
// task (forget), neither of which should block on filesystem I/O.
static void prv_store_pump_paired_cb(void *data) {
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    minimed_sake_log("persist open fail");
    return;
  }
  uint8_t v = (data != NULL) ? 1 : 0;
  if (settings_file_set(&fd, s_paired_setting_key, sizeof(s_paired_setting_key), &v, sizeof(v)) !=
      S_SUCCESS) {
    minimed_sake_log("persist set fail");
  }
  settings_file_close(&fd);
}

static void prv_set_pump_paired(bool paired) {
  if (s_pump_paired == paired) {
    return;  // no change -> no flash write (handshake re-runs on every reconnect)
  }
  s_pump_paired = paired;
  launcher_task_add_callback(prv_store_pump_paired_cb, paired ? (void *)1 : NULL);
}

static void prv_rng(void *ud, uint8_t *out, size_t n) {
  (void)ud;
  ble_hs_hci_rand(out, (int)n);  // the NimBLE host's CSPRNG (also used for SM pairing randoms)
}

static void prv_defer_notify(uint16_t conn, const uint8_t payload[SAKE_MESSAGE_SIZE],
                             uint32_t delay_ms) {
  s_notify_conn = conn;
  memcpy(s_notify_buf, payload, SAKE_MESSAGE_SIZE);
  ble_npl_callout_reset(&s_notify_co, ble_npl_time_ms_to_ticks32(delay_ms));
}

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
  char line[32];
  if (all_zero) {
    // The first (stage 0) write is the milestone that proved Spike 1; keep its distinct report.
    minimed_sake_spike_report(MinimedSakeStageWrote);
  }
  snprintf(line, sizeof(line), "wrote %u:%02x %02x %02x %02x", len, buf[0], buf[1], buf[2], buf[3]);
  minimed_sake_log(line);

  if (!s_keydb_ok) {
    return 0;  // no key DB -> stay inert (Spike 1 behaviour: log the write, don't handshake)
  }

  // Advance the handshake. The wake-up (20 zeros on subscribe) is NOT fed here -- only pump writes.
  uint8_t reply[SAKE_MESSAGE_SIZE];
  sake_result r = sake_server_handshake(&s_server, buf, reply);
  int stage = s_server.stage;
  if (r == SAKE_RESULT_MSG) {
    prv_defer_notify(conn_handle, reply, 30);
    snprintf(line, sizeof(line), "sent reply (st%d)", stage);
    minimed_sake_log(line);
  } else if (r == SAKE_RESULT_DONE) {
    prv_set_pump_paired(true);  // pump is bonded now -> advertise FE81 (reconnect) from here on
    struct ble_gap_conn_desc d;  // remember who the pump is, for the v29 PUMP/phone conn label
    if (ble_gap_conn_find(conn_handle, &d) == 0) {
      s_pump_id_addr = d.peer_id_addr;
      s_pump_addr_known = true;
    }
    minimed_sake_spike_report(MinimedSakeStageHandshakeComplete);
    minimed_sake_read_start(conn_handle);  // begin the post-handshake CGM read
  } else {
    snprintf(line, sizeof(line), "sake ERR (st%d)", stage);
    minimed_sake_log(line);
  }
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

// Runs on the BT host task; sends whatever prv_defer_notify staged (wake-up or a handshake reply).
static void prv_notify_cb(struct ble_npl_event *ev) {
  struct os_mbuf *om = ble_hs_mbuf_from_flat(s_notify_buf, sizeof(s_notify_buf));
  if (!om) {
    minimed_sake_log("notify mbuf fail");
    return;
  }
  int rc = ble_gatts_notify_custom(s_notify_conn, s_sake_port_val_handle, om);
  char line[24];
  snprintf(line, sizeof(line), "notify rc=0x%04x", (uint16_t)rc);
  minimed_sake_log(line);
}

void minimed_sake_handle_subscribe(uint16_t conn_handle, uint16_t attr_handle, bool notify) {
  if (attr_handle != s_sake_port_val_handle || !notify) {
    return;
  }

  minimed_sake_spike_report(MinimedSakeStageSubscribed);
  // Start a fresh handshake for this subscription -- the pump restarts from stage 0 on every
  // pairing attempt, so re-init the server (and draw new server key material) each time.
  if (s_keydb_ok) {
    sake_server_init(&s_server, &s_keydb, SAKE_DEV_MOBILE_APPLICATION, prv_rng, NULL);
  }
  // Defer the 20-zero wake-up ~120ms. Sending it synchronously here is too early -- it can go out
  // before the CCCD write-response, so the pump never registers notifications, waits, then drops
  // (disc reason 0x13). The working Android bridge likewise posts it to a worker thread.
  uint8_t wakeup[SAKE_MESSAGE_SIZE] = {0};
  prv_defer_notify(conn_handle, wakeup, 120);
}

bool minimed_sake_decrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t out_cap,
                          uint16_t *out_len) {
  if (!s_keydb_ok || !sake_server_is_complete(&s_server)) {
    return false;
  }
  // sake_decrypt_from_pump writes n-3 plaintext bytes unconditionally; `n` is an external device's
  // frame length and the MTU is not clamped to 20, so bound it against the caller's buffer here.
  if (n < 3 || (uint16_t)(n - 3) > out_cap) {
    return false;
  }
  size_t out_n = 0;
  if (!sake_decrypt_from_pump(&s_server, in, n, out, &out_n)) {
    return false;
  }
  *out_len = (uint16_t)out_n;
  return true;
}

bool minimed_sake_encrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t *out_len) {
  if (!s_keydb_ok || !sake_server_is_complete(&s_server)) {
    return false;
  }
  sake_encrypt_for_pump(&s_server, in, n, out);
  *out_len = (uint16_t)(n + 3);  // SeqCrypt appends a 1-byte counter + 2-byte MAC
  return true;
}

uint8_t minimed_sake_build_adv(uint8_t *buf, uint8_t buf_len) {
  // Flags: LE General Discoverable + BR/EDR not supported.
  // Complete 16-bit Service Class UUID list: 0xfe82 (SAKE, first-pair) or 0xfe81 (reconnect --
  // what an already-bonded pump scans for; it ignores the rest of the payload then).
  // Manufacturer data: Medtronic company 0x01f9, payload = 0x00 + "Mobile PB" + 0x00.
  // Matches the working Android bridge; the pump reads the name from mfr data, not the GAP name.
  //
  // Do NOT lengthen the name without testing pairing on hardware. "Mobile Pebble" was tried in v34
  // and the pump then reported "device not found", even though a laptop scanner confirmed the
  // advert was well-formed (FE82 present, flags 0x06, -34 dBm) and the name is legal per
  // Documentation/bluetooth.md ("Mobile " + 0-7 chars). Reverted to these exact bytes, which have
  // paired reliably since v10. Unexplained, so treat the payload as load-bearing.
  static const uint8_t adv[] = {
      0x02, 0x01, 0x06,
      0x03, 0x03, 0x82, 0xfe,
      // mfr data (company 0x01f9): 0x00 + name + 0x00. Name "Mobile PB" (9 chars) -> payload
      // len 0x0e.
      0x0e, 0xff, 0xf9, 0x01, 0x00,
      'M', 'o', 'b', 'i', 'l', 'e', ' ', 'P', 'B', 0x00,
  };
  if (buf_len < sizeof(adv)) {
    return 0;
  }
  memcpy(buf, adv, sizeof(adv));
  if (s_pump_paired) {
    buf[5] = 0x81;  // service class low byte: 0xfe82 -> 0xfe81
  }
  return sizeof(adv);
}

bool minimed_sake_pump_paired(void) { return s_pump_paired; }

bool minimed_sake_addr_is_pump(const ble_addr_t *addr) {
  return s_pump_addr_known && ble_addr_cmp(addr, &s_pump_id_addr) == 0;
}

void minimed_sake_forget_pump(void) {
  prv_set_pump_paired(false);
  minimed_sake_log("forget pump -> FE82");
  // Re-advertise first-pair immediately; in NORMAL mode the next SPIKE toggle picks it up anyway
  // (and force_readvertise would needlessly drop the phone link).
  if (minimed_sake_get_mode() == MinimedSakeModeSpike) {
    minimed_sake_force_readvertise();
  }
}

void minimed_sake_apply_sm_config(bool spike) {
  // The pump and the phone want opposite Security Manager settings, and NimBLE reads ble_hs_cfg
  // live when it builds each pairing request -- so flip at runtime by mode instead of baking one
  // compromise into syscfg. The phone never pairs in SPIKE mode (its BT is off) and the pump never
  // in NORMAL, so the two configs never meet. This keeps the phone bond identical to stock (no
  // re-pair dance on every reflash) while still letting the pump pair legacy Just Works.
  if (spike) {
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;  // pump: no MITM -> Just Works
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = 3;  // + IRK/identity so the pump can resolve our RPA on reconnect
    ble_hs_cfg.sm_their_key_dist = 3;
    minimed_sake_log("SM: pump (legacy JW)");
  } else {
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_DISPLAY_YESNO;  // phone: stock LESC + numeric-compare MITM
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_our_key_dist = 1;
    ble_hs_cfg.sm_their_key_dist = 3;
  }
}

int minimed_sake_service_init(void) {
  ble_npl_callout_init(&s_notify_co, nimble_port_get_dflt_eventq(), prv_notify_cb, NULL);
  minimed_sake_read_init();

  // Boot mode is NORMAL: make sure the phone gets the stock strict config even though we compile
  // with the permissive legacy gates (SC_ONLY 0 / LEGACY 1) the pump needs.
  minimed_sake_apply_sm_config(false);

  prv_load_pump_paired();
  if (s_pump_paired) {
    minimed_sake_log("paired (persisted): FE81");
  }

  s_keydb_ok = sake_keydb_parse(&s_keydb, s_pump_keydb_bytes, sizeof(s_pump_keydb_bytes));
  if (s_keydb_ok) {
    sake_server_init(&s_server, &s_keydb, SAKE_DEV_MOBILE_APPLICATION, prv_rng, NULL);
  } else {
    PBL_LOG_ERR("SAKE: key DB parse failed (CRC/length) -- handshake disabled");
  }

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
