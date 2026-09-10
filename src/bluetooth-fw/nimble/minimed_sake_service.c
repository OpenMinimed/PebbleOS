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
#include "nimble_type_conversions.h"
#include "comm/ble/gap_le_advert.h"
#include "comm/bt_lock.h"
#include "pbl/services/bluetooth/bluetooth_persistent_storage.h"
#include "popups/minimed_sake_spike_ui.h"
#include "kernel/event_loop.h"
#include "pbl/services/settings/settings_file.h"
#include "pbl/util/size.h"
#include <pbl/logging/logging.h>

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
// True once flash holds the address currently in s_pump_id_addr. The paired flag cannot stand in
// for this: prv_set_pump_paired short-circuits when the flag does not change, so a watch that was
// already paired before the address was persisted would never write it.
static bool s_pump_addr_persisted;

// The pump's own advertising job in DUAL mode. The scheduler has a single advertising instance,
// so this time-shares with the phone's jobs; it is independent of the Reconnection job, which the
// kernel LE client unschedules when the phone connects -- the pump needs a job that survives that.
static GAPLEAdvertisingJobRef s_pump_advert_job;

// Cached identity of the phone (gateway) bond, captured when the pump-pairing window opens so a
// reconnecting phone during the window is not misclassified as the pump (whose identity is unknown
// until its first handshake). RAM-only, refreshed on every DUAL entry and on stack re-init.
static bool s_gateway_addr_known;
static ble_addr_t s_gateway_addr;

#define MINIMED_SETTINGS_FILE "minimedsake"
#define MINIMED_SETTINGS_MAX_SIZE 256
static const char s_paired_setting_key[] = "paired";
static const char s_pump_addr_key[] = "pumpaddr";

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
  // The pump's identity address rides along so the pump link is recognised (and, in NORMAL,
  // rejected) even right after a cold boot, before the first handshake of the boot.
  ble_addr_t addr;
  if (settings_file_get(&fd, s_pump_addr_key, sizeof(s_pump_addr_key), (uint8_t *)&addr,
                        sizeof(addr)) == S_SUCCESS) {
    s_pump_id_addr = addr;
    s_pump_addr_known = true;
    s_pump_addr_persisted = true;
  }
  settings_file_close(&fd);
}

// Flash write deferred to KernelMain: the flag flips on the BT host task (handshake) or the app
// task (forget), neither of which should block on filesystem I/O.
static void prv_store_pump_paired_cb(void *data) {
  SettingsFile fd;
  if (settings_file_open(&fd, MINIMED_SETTINGS_FILE, MINIMED_SETTINGS_MAX_SIZE) != S_SUCCESS) {
    minimed_sake_log_evt("persist open fail");
    return;
  }
  uint8_t v = (data != NULL) ? 1 : 0;
  if (settings_file_set(&fd, s_paired_setting_key, sizeof(s_paired_setting_key), &v, sizeof(v)) !=
      S_SUCCESS) {
    minimed_sake_log_evt("persist set fail");
  }
  if (s_pump_addr_known) {
    if (settings_file_set(&fd, s_pump_addr_key, sizeof(s_pump_addr_key),
                          (uint8_t *)&s_pump_id_addr, sizeof(s_pump_id_addr)) != S_SUCCESS) {
      minimed_sake_log_evt("persist addr fail");
    } else {
      s_pump_addr_persisted = true;
    }
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

// -------------------------------------------------------------------------------------------------
// Pump advert job (DUAL mode)
// -------------------------------------------------------------------------------------------------
// The scheduler's single advertising instance time-shares this job with the phone's. It is
// separate from the Reconnection job because that one is unscheduled when the phone connects
// (kernel_le_client) and refuses to restart while connected as a slave -- the pump's discovery
// vehicle must survive both.

// How long the pump job stays at the fast (Medtronic) interval before backing off to
// MedtronicSlow. Same 30s window as the phone's reconnection job.
#define PUMP_ADVERT_FAST_DURATION_SECS (30)

static void prv_pump_advert_unscheduled_cb(GAPLEAdvertisingJobRef job, bool completed, void *data) {
  s_pump_advert_job = NULL;
}

static void prv_pump_advert_rebuild(void) {
  bt_lock();  // the unschedule callback below runs under bt_lock; guard the pointer against
              // concurrent rebuilds from the app task (toggle/forget) and the BT task (handshake).
  // Stop whatever is scheduled; when not in DUAL mode that is all we want (NORMAL has no pump job).
  if (s_pump_advert_job) {
    gap_le_advert_unschedule(s_pump_advert_job);
    s_pump_advert_job = NULL;
  }
  if (minimed_sake_get_mode() != MinimedSakeModeDual) {
    goto unlock;
  }

  // BLEAdData ends in a flexible array, so it can't be declared on its own and then written past
  // -- allocate the struct and the data buffer as one object. ad.data aliases data[].
  typedef struct {
    BLEAdData ad;
    uint8_t data[GAP_LE_AD_REPORT_DATA_MAX_LENGTH];
  } MinimedPumpAdBuf;
  static MinimedPumpAdBuf s_pump_ad;
  s_pump_ad.ad.ad_data_length = minimed_sake_build_adv(s_pump_ad.data, sizeof(s_pump_ad.data));
  s_pump_ad.ad.scan_resp_data_length = 0;

  // Fast-then-slow back-off, same shape as the phone's reconnection job: burn the fast interval
  // for a while, then settle for as long as the pump doesn't show up. prv_pump_advert_rebuild is
  // called fresh (cur_term resets to 0) whenever the pump is expected back soon -- on a mode entry
  // and, from the disconnect handler, right after the pump link drops.
  const GAPLEAdvertisingJobTerm terms[] = {
      {.duration_secs = PUMP_ADVERT_FAST_DURATION_SECS,
       .interval = GAPLEAdvertisingInterval_Medtronic},
      {.duration_secs = GAPLE_ADVERTISING_DURATION_INFINITE,
       .interval = GAPLEAdvertisingInterval_MedtronicSlow},
  };
  s_pump_advert_job =
      gap_le_advert_schedule(&s_pump_ad.ad, terms, ARRAY_LENGTH(terms),
                             prv_pump_advert_unscheduled_cb, NULL, GAPLEAdvertisingJobTagMinimed);
  if (s_pump_advert_job) {
    char line[32];
    snprintf(line, sizeof(line), "adv job FE8%c len%u", s_pump_paired ? '1' : '2',
             s_pump_ad.ad.ad_data_length);
    minimed_sake_log_evt(line);
  } else {
    minimed_sake_log_evt("pump adv job FAIL");
  }
unlock:
  bt_unlock();
}

void minimed_sake_pump_advert_start(void) { prv_pump_advert_rebuild(); }
void minimed_sake_pump_advert_stop(void) { prv_pump_advert_rebuild(); }
void minimed_sake_pump_advert_update(void) { prv_pump_advert_rebuild(); }

// The pump-pairing window: while in DUAL mode with the pump not yet bonded, any incoming
// connection is presumed to be the pump (its identity is unknown until the handshake completes)
// and pairing must use legacy Just Works. Closes once the pump is paired.
bool minimed_sake_pump_pairing_window(void) {
  return minimed_sake_get_mode() == MinimedSakeModeDual && !s_pump_paired;
}

// Cache the phone (gateway) identity so a reconnecting phone during the pump-pairing window is not
// misclassified as the pump. Called on the app/KernelMain task (toggle to DUAL, forget-pump) and
// on the BT host task from minimed_sake_service_init. The settings read is safe on either --
// prv_load_pump_paired below already reads the same way from service_init.
void minimed_sake_cache_gateway_addr(void) {
  s_gateway_addr_known = false;
  BTBondingID gw = bt_persistent_storage_get_ble_ancs_bonding();
  if (gw == BT_BONDING_ID_INVALID) {
    return;
  }
  BTDeviceInternal dev;
  if (!bt_persistent_storage_get_ble_pairing_by_id(gw, NULL, &dev, NULL)) {
    return;
  }
  ble_addr_t a;
  pebble_device_to_nimble_addr(&dev, &a);
  s_gateway_addr = a;
  s_gateway_addr_known = true;
}

// True if the peer is the cached phone (gateway) identity.
bool minimed_sake_addr_is_gateway(const uint8_t addr[6], uint8_t addr_type) {
  if (!s_gateway_addr_known || s_gateway_addr.type != addr_type) {
    return false;
  }
  return memcmp(s_gateway_addr.val, addr, 6) == 0;
}

// Recover the pump identity from the bond store. Needed on a watch that paired before the address
// was persisted: flash has the paired flag but no pumpaddr, and prv_set_pump_paired short-circuits
// on an unchanged flag, so nothing would write it. Until the address is known the first connect of
// every boot is classified as the phone -- addr_is_pump is false and the pairing window is shut
// once paired -- which routes the pump into the fw stack and later swallows its disconnect.
// The pump is the only non-gateway BLE bond (nimble_store stores it with is_gateway = false).
#define PUMP_BOND_SEARCH_MAX 4
typedef struct {
  int count;
  BTBondingID ids[PUMP_BOND_SEARCH_MAX];
  ble_addr_t addrs[PUMP_BOND_SEARCH_MAX];
} PumpBondSearch;

// Collect only. bt_persistent_storage_for_each_ble_pairing holds the bonding-DB mutex while it
// runs this, and that mutex is not recursive, so anything that reads the bonding DB here -- the
// is-gateway test included -- self-deadlocks. The same rule is spelled out at the other call site
// in settings/bluetooth.c. Deadlocking here hangs the NimBLE host task during init, which times
// out into PBL_CROAK and reboots the watch until it drops to PRF.
static void prv_adopt_pump_bond_cb(BTDeviceInternal *device, SMIdentityResolvingKey *irk,
                                   const char *name, BTBondingID *id, void *context) {
  PumpBondSearch *search = (PumpBondSearch *)context;
  if (search->count >= PUMP_BOND_SEARCH_MAX) {
    return;
  }
  search->ids[search->count] = *id;
  pebble_device_to_nimble_addr(device, &search->addrs[search->count]);
  search->count++;
}

// Runs on KernelMain, never on the BT host task: this reads the bonding DB, and doing that from
// inside minimed_sake_service_init hangs BT init (see the callback above). Scheduling it also
// keeps the read off whatever init ordering the bonding DB itself needs. The pump cannot connect
// before the user toggles to DUAL, so landing a beat later is soon enough.
static void prv_adopt_pump_bond_cb2(void *unused) {
  PumpBondSearch search = {0};
  bt_persistent_storage_for_each_ble_pairing(prv_adopt_pump_bond_cb, &search);
  // The lock is free now, so the gateway test is safe.
  int found = 0;
  ble_addr_t addr = {0};
  for (int i = 0; i < search.count; i++) {
    if (bt_persistent_storage_is_ble_ancs_bonding(search.ids[i])) {
      continue;  // the phone
    }
    found++;
    addr = search.addrs[i];
  }
  if (found != 1) {
    return;  // no pump bond, or ambiguous -- leave it to the next handshake
  }
  s_pump_id_addr = addr;
  s_pump_addr_known = true;
  minimed_sake_log_evt("pump addr from bond");
}

static void prv_adopt_pump_bond(void) { launcher_task_add_callback(prv_adopt_pump_bond_cb2, NULL); }

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
  minimed_sake_log_evt(line);

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
    minimed_sake_log_evt(line);
  } else if (r == SAKE_RESULT_DONE) {
    prv_set_pump_paired(true);  // pump is bonded now -> advertise FE81 (reconnect) from here on
    struct ble_gap_conn_desc d;  // remember who the pump is, for the v29 PUMP/phone conn label
    if (ble_gap_conn_find(conn_handle, &d) == 0) {
      const bool changed =
          !s_pump_addr_known || memcmp(&s_pump_id_addr, &d.peer_id_addr, sizeof(s_pump_id_addr));
      s_pump_id_addr = d.peer_id_addr;
      s_pump_addr_known = true;
      // Persist here, not only via prv_set_pump_paired: on a watch that was already paired the
      // flag never changes, so that path never fires and the address would stay RAM-only. Then
      // every cold boot classifies the pump's first connect as the phone (the pairing window is
      // closed once paired), routing a link into the fw stack whose disconnect is later swallowed.
      if (changed || !s_pump_addr_persisted) {
        launcher_task_add_callback(prv_store_pump_paired_cb, s_pump_paired ? (void *)1 : NULL);
      }
    }
    minimed_sake_spike_report(MinimedSakeStageHandshakeComplete);
    minimed_sake_read_start(conn_handle);  // begin the post-handshake CGM read
    // The pump is bonded now: close the pump-pairing window (back to strict LESC for the phone)
    // and re-air the pump advert as FE81.
    minimed_sake_apply_sm_config(false);
    minimed_sake_pump_advert_update();
  } else {
    snprintf(line, sizeof(line), "sake ERR (st%d)", stage);
    minimed_sake_log_evt(line);
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
    minimed_sake_log_evt("notify mbuf fail");
    return;
  }
  int rc = ble_gatts_notify_custom(s_notify_conn, s_sake_port_val_handle, om);
  char line[24];
  snprintf(line, sizeof(line), "notify rc=0x%04x", (uint16_t)rc);
  minimed_sake_log_evt(line);
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
  minimed_sake_log_evt("forget pump -> FE82");
  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    // Open the pump-pairing window: legacy JW for the next pair, FE82 advert, and drop any live
    // pump link so the pump re-pairs fresh. The phone link is untouched.
    minimed_sake_cache_gateway_addr();
    minimed_sake_apply_sm_config(true);
    minimed_sake_pump_advert_update();
    minimed_sake_force_readvertise();
  }
}

void minimed_sake_apply_sm_config(bool pump_window) {
  // The pump and the phone want opposite Security Manager settings, and NimBLE reads ble_hs_cfg
  // live when it builds each pairing request -- so flip at runtime instead of baking one
  // compromise into syscfg. The phone keeps the stock strict-LESC bond (no re-pair dance on every
  // reflash) while the pump pairs legacy Just Works -- only during the pump-pairing window (DUAL
  // mode with the pump not yet bonded).
  if (pump_window) {
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;  // pump: no MITM -> Just Works
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_our_key_dist = 3;  // + IRK/identity so the pump can resolve our RPA on reconnect
    ble_hs_cfg.sm_their_key_dist = 3;
    minimed_sake_log_evt("SM: pump (legacy JW)");
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

  // Re-arm DUAL state after a Bluetooth stack restart that was triggered while in DUAL (the
  // pump-liveness watchdog's recovery, or a manual DUAL-keep restart). s_mode survives the
  // restart (it is a static), but the loopback session and the driver's link state were torn down
  // with the stack -- re-create them.
  // The advert half of the re-arm is NOT done here. This runs inside bt_driver_start, and
  // bluetooth_ctl.c calls gap_le_init() -- hence gap_le_advert_init() -- immediately afterwards,
  // which clears s_jobs, s_current and s_allow_advert_while_connected. Anything scheduled here is
  // discarded before it can ever air. minimed_sake_bt_started() does that part, after gap_le_init.
  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    minimed_sake_clear_link_state();
    minimed_sake_cache_gateway_addr();
    minimed_sake_apply_sm_config(minimed_sake_pump_pairing_window());
    minimed_sake_sender_set_mode(true);
  } else {
    // Boot mode is NORMAL: make sure the phone gets the stock strict config even though we compile
    // with the permissive legacy gates (SC_ONLY 0 / LEGACY 1) the pump needs.
    minimed_sake_apply_sm_config(false);
  }

  prv_load_pump_paired();
  if (s_pump_paired) {
    minimed_sake_log_evt("paired (persisted): FE81");
    if (!s_pump_addr_known) {
      prv_adopt_pump_bond();  // paired before pumpaddr existed; the handshake will persist it
    }
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
