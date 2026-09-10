/* SPDX-FileCopyrightText: 2025 Google LLC */
/* SPDX-License-Identifier: Apache-2.0 */

#include <stdio.h>

#include <bluetooth/bonding_sync.h>
#include <bluetooth/bt_driver_advert.h>
#include <bluetooth/gatt.h>
#include <bluetooth/pairing_confirm.h>
#include <comm/bt_lock.h>
#include <host/ble_gap.h>
#include <host/ble_hs_hci.h>
#include <kernel/pbl_malloc.h>
#include <os/os_mbuf.h>
#include <pbl/logging/logging.h>
#include <system/passert.h>
#include <pbl/util/math.h>

#include "nimble_gattc_op_queue.h"
#include "nimble_type_conversions.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE
#include "comm/ble/gap_le_advert.h"
#include "minimed_sake_read.h"
#include "minimed_sake_service.h"
#include "popups/minimed_sake_spike_ui.h"
#endif

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

static const ble_uuid16_t s_device_name_chr_uuid = BLE_UUID16_INIT(0x2A00);
static char s_device_name[BT_DEVICE_NAME_BUFFER_SIZE];
static bool s_pairing_in_progress;

#ifdef CONFIG_MINIMED_SAKE_SPIKE
// True if the last advertising-enable attempt succeeded. Used to log the "adv START FAIL" line
// only once per failure episode: with both links up the connection pool is full, NimBLE refuses
// connectable advertising, and the scheduler retries every second -- a line per second would
// flood the 8-line on-watch ring.
static bool s_last_adv_enable_ok = true;
#endif

#ifdef CONFIG_MINIMED_SAKE_SPIKE
static uint16_t s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;
// A pump connection we rejected in NORMAL (terminated on connect). Its connect was NOT routed to
// the Pebble stack, so its disconnect must not be either -- the stack would dereference a
// GAPLEConnection that was never created (NULL -> hard fault). Tracked so the disconnect handler
// can recognise and swallow it.
static uint16_t s_rejected_pump_conn = BLE_HS_CONN_HANDLE_NONE;

// Re-advertise under the current mode after a forget-pump. The advert payload is job-owned (the
// pump's Medtronic job in DUAL, the phone's Pebble jobs in NORMAL), but the advertising scheduler
// skips re-pushing data when its job pointer is unchanged -- so a payload change (e.g. FE82 after
// forget) must invalidate the scheduler's cache. gap_le_advert_force_data_refresh does that.
// Drops the active PUMP link if any (frees its slot and the ensuing disconnect re-airs). The phone
// link is never touched.
void minimed_sake_force_readvertise(void) {
  gap_le_advert_force_data_refresh();
  // If the pump link is up, drop it too: this frees the connection slot for a fresh first-pair
  // and the ensuing disconnect makes the scheduler re-air -- now with the refreshed, FE82 payload.
  if (s_sake_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    ble_gap_terminate(s_sake_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
  }
}

// Drop recorded link handles. Called before the NORMAL kill-switch restart so a stale handle from
// before the restart cannot alias (and swallow) a later phone connection's disconnect.
void minimed_sake_clear_link_state(void) {
  s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  s_rejected_pump_conn = BLE_HS_CONN_HANDLE_NONE;
}

// True while the pump link is tracked as connected. Used by the pump-liveness watchdog.
bool minimed_sake_pump_connected(void) {
  return s_sake_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}
#endif

static int prv_device_name_read_event_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         struct ble_gatt_attr *attr, void *arg) {
  if (error->status != 0) {
    nimble_gattc_op_queue_complete();
    return 0;
  }

  size_t len = MIN(OS_MBUF_PKTLEN(attr->om), sizeof(s_device_name) - 1);
  os_mbuf_copydata(attr->om, 0, len, s_device_name);
  s_device_name[len] = '\0';

  return 0;
}

static int prv_device_name_read_op_start(void *ctx) {
  const uint16_t conn_handle = *(uint16_t *)ctx;

  int rc = ble_gattc_read_by_uuid(conn_handle, 1, UINT16_MAX,
                                  (ble_uuid_t *)&s_device_name_chr_uuid,
                                  prv_device_name_read_event_cb, NULL);
  if (rc != 0) {
    PBL_LOG_ERR("Pairing device name read failed to start (rc=0x%04x)", (uint16_t)rc);
  }

  return rc;
}

void bt_driver_advert_advertising_disable(void) {
  int rc;

  if (ble_gap_adv_active() == 0) {
    return;
  }

  rc = ble_gap_adv_stop();
  PBL_ASSERT(rc == 0, "Failed to stop advertising (0x%04x)", (uint16_t)rc);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    minimed_sake_log("adv DISABLE");
  }
#endif
}

bool bt_driver_advert_client_get_tx_power(int8_t *tx_power) { return false; }

bool bt_driver_advert_set_advertising_data(const BLEAdData *ad_data) {
  int rc;

  rc = ble_gap_adv_set_data((uint8_t *)&ad_data->data, ad_data->ad_data_length);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to set advertising data (0x%04x)", (uint16_t)rc);
    return false;
  }

  if (ad_data->scan_resp_data_length > 0) {
    rc = ble_gap_adv_rsp_set_data((uint8_t *)&ad_data->data[ad_data->ad_data_length],
                                  ad_data->scan_resp_data_length);
  } else {
    // The pump advert carries no scan response, and a stale one from a previous phone job would
    // make the watch answer active scans as a Pebble while impersonating a Medtronic peripheral.
    // NULL/0 is the documented way to clear it (NimBLE only rejects NULL with a nonzero length).
    rc = ble_gap_adv_rsp_set_data(NULL, 0);
  }
  if (rc != 0) {
    PBL_LOG_ERR("Failed to set scan response data (0x%04x)", (uint16_t)rc);
    return false;
  }

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  // DIAGNOSTIC (v29): dump the actual bytes handed to the controller. In DUAL mode the scheduler
  // time-shares the pump's Medtronic payload and the phone's Pebble payload, so b[5]b[6] tells
  // which was pushed (82fe/81fe = pump, anything else = phone).
  {
    const uint8_t *b = ad_data->data;
    char line[32];
    snprintf(line, sizeof(line), "adv %02x%02x%02x%02x%02x%02x%02x", b[0], b[1], b[2], b[3], b[4],
             b[5], b[6]);
    minimed_sake_log(line);
  }
#endif

  return true;
}

#ifdef CONFIG_MINIMED_SAKE_SPIKE
// Connection interval / peripheral latency / supervision timeout, in milliseconds, to the on-watch
// log. Interval x (latency + 1) is how often the radio actually has to wake.
static void prv_log_conn_params(const struct ble_gap_conn_desc *desc) {
  char line[32];
  snprintf(line, sizeof(line), "prm %ums lat%u sv%ums",
           (unsigned)(desc->conn_itvl * BLE_HCI_CONN_ITVL / 1000), (unsigned)desc->conn_latency,
           (unsigned)(desc->supervision_timeout * BLE_HCI_CONN_SPVN_TMO_UNITS));
  minimed_sake_log_evt(line);
}
#endif

static void prv_handle_connection_event(struct ble_gap_event *event) {
  // we only want to notify on a successful connection
  if (event->connect.status != 0) return;

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_connection_event: Failed to find connection descriptor");
    return;
  }

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  // Pump recognition: the known pump identity, or any connection while the pump-pairing window is
  // open (DUAL with the pump not yet bonded -- its identity is unknown until the handshake). The
  // gateway check stops a reconnecting phone in that window from being swallowed as the pump.
  const bool is_pump = minimed_sake_addr_is_pump(&desc.peer_id_addr) ||
                       (minimed_sake_pump_pairing_window() &&
                        !minimed_sake_addr_is_gateway(desc.peer_id_addr.val,
                                                      desc.peer_id_addr.type));

  if (minimed_sake_get_mode() == MinimedSakeModeNormal && is_pump) {
    // The bonded pump reconnects by identity address regardless of the advertised payload (it holds
    // the bond + our IRK -- HW-confirmed: it handshakes in NORMAL even though we advertise a plain
    // Pebble payload). In NORMAL we advertise for the phone, so a pump connection here would run SAKE
    // and squat the connection, blocking the phone. Reject it: the freed slot lets the phone win.
    minimed_sake_log_evt("pump conn in NORMAL -> drop");
    s_rejected_pump_conn = event->connect.conn_handle;  // so its disconnect is swallowed, not routed
    int rc = ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    if (rc != 0) {
      // Surface it: the pump may squat a slot until the link drops on its own. But KEEP the
      // marker. It records "this connect was never routed to the fw stack", which is true whether
      // or not the terminate succeeded (we return either way, so no GAPLEConnection exists). The
      // connection always ends eventually, and routing that disconnect would deref a NULL
      // GAPLEConnection in gap_le_connect.c -- the exact v31 hard fault. Clearing it here re-armed
      // that crash for e.g. a link that died in the window before the terminate reached the
      // controller. The marker is dropped either by this connection's own disconnect or when a
      // later phone connection reuses the handle (see the stale-handle clear below).
      char line[32];
      snprintf(line, sizeof(line), "pump drop FAIL 0x%04x", (uint16_t)rc);
      minimed_sake_log_evt(line);
    }
    return;
  }

  if (minimed_sake_get_mode() == MinimedSakeModeDual && is_pump) {
    // DUAL: the pump link is driver-private. Record it for the SAKE layer and report, but do NOT
    // route the connect into the Pebble firmware stack -- the phone keeps its own connection
    // bookkeeping there, and routing the pump would flip its single-connection state and free the
    // advert scheduler incorrectly.
    s_sake_conn_handle = event->connect.conn_handle;
    minimed_sake_spike_report(MinimedSakeStageConnected);
    {
      char line[32];
      snprintf(line, sizeof(line), "conn PUMP m=D %02x:%02x t%u",
               desc.peer_id_addr.val[5], desc.peer_id_addr.val[0], desc.peer_id_addr.type);
      minimed_sake_log_evt(line);
      prv_log_conn_params(&desc);
    }
    // The link-layer controller stopped advertising when this connected, but the scheduler still
    // believes it is on air (its s_is_advertising state). Re-arm it so the phone can still connect
    // while the pump holds a link.
    gap_le_advert_force_data_refresh();
    return;
  }

  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    char line[32];
    snprintf(line, sizeof(line), "conn phone m=D %02x:%02x t%u",
             desc.peer_id_addr.val[5], desc.peer_id_addr.val[0], desc.peer_id_addr.type);
    minimed_sake_log_evt(line);
    prv_log_conn_params(&desc);
  }

  // A phone connection reusing a handle a pump (swallowed/rejected) link used before a Bluetooth
  // stack restart: that pump link is gone, so drop the stale markers -- otherwise this phone's
  // disconnect would be swallowed as the pump's and the firmware stack would think it is still
  // connected forever. Handle reuse is possible now that there are two connection slots.
  if (event->connect.conn_handle == s_sake_conn_handle) {
    s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  }
  if (event->connect.conn_handle == s_rejected_pump_conn) {
    s_rejected_pump_conn = BLE_HS_CONN_HANDLE_NONE;
  }
#endif

  struct BleConnectionCompleteEvent complete_event = {
      .handle = event->connect.conn_handle,
      .is_master = desc.role == BLE_GAP_ROLE_MASTER,
      .status = HciStatusCode_Success,
      .mtu = ble_att_mtu(event->connect.conn_handle),
  };

  // If OTA address != ID address, then the address must be resolved.
  // This happens for an already paired devices.
  complete_event.is_resolved = ble_addr_cmp(&desc.peer_id_addr, &desc.peer_ota_addr) != 0;

  {
    BTDeviceAddress ota_addr, id_addr;
    nimble_addr_to_pebble_addr(&desc.peer_ota_addr, &ota_addr);
    nimble_addr_to_pebble_addr(&desc.peer_id_addr, &id_addr);
    PBL_LOG_DBG("Conn compl: ota=" BT_DEVICE_ADDRESS_FMT " atype=%u",
                BT_DEVICE_ADDRESS_XPLODE(ota_addr), desc.peer_ota_addr.type);
    PBL_LOG_DBG("Conn compl: id=" BT_DEVICE_ADDRESS_FMT " atype=%u",
                BT_DEVICE_ADDRESS_XPLODE(id_addr), desc.peer_id_addr.type);
  }

  if (complete_event.is_resolved) {
    int rc;
    struct ble_store_key_sec key_sec;
    struct ble_store_value_sec value_sec;

    key_sec.idx = 0;
    key_sec.peer_addr = desc.peer_id_addr;

    rc = ble_store_read_peer_sec(&key_sec, &value_sec);
    if (rc != 0) {
      // We can get a resolved address in case of a repeated pairing event,
      // where peer security is deleted. An identity resolved event will be
      // received later after the new pairing is completed.
      PBL_LOG_INFO("Address resolved but no stored peer security (rc=%d)", rc);
      complete_event.is_resolved = false;
    } else {
      memcpy(complete_event.irk.data, value_sec.irk, 16);
    }
  } else {
    // If the address is not resolved, pairing is gonna happen.
    // Trigger name read to have it ready for the pairing confirmation.
    memset(s_device_name, 0, sizeof(s_device_name));
    uint16_t *conn_handle = kernel_malloc_check(sizeof(*conn_handle));
    *conn_handle = event->connect.conn_handle;
    nimble_gattc_op_queue_push(prv_device_name_read_op_start, conn_handle);
  }

  nimble_conn_params_to_pebble(&desc, &complete_event.conn_params);
  nimble_addr_to_pebble_device(&desc.peer_id_addr, &complete_event.peer_address);

  s_pairing_in_progress = false;

  bt_driver_handle_le_connection_complete_event(&complete_event);
}

static void prv_handle_disconnection_event(struct ble_gap_event *event) {
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  const uint16_t conn_handle = event->disconnect.conn.conn_handle;
  // Layer 2 diagnostic: every host-delivered disconnect, BEFORE the swallow routing, with the two
  // tracked pump handles so the reason it was (or was not) swallowed is visible. An untracked
  // disconnect that also fails the pump-address test is a pump link the driver never recorded
  // (e.g. a handle cleared early or a second pump link) and would route into the fw stack.
  {
    char line[64];
    snprintf(line, sizeof(line), "disc hdl=%u r=0x%02x sake=%u rej=%u %02x:%02x",
             conn_handle, (uint8_t)event->disconnect.reason, s_sake_conn_handle,
             s_rejected_pump_conn, event->disconnect.conn.peer_id_addr.val[5],
             event->disconnect.conn.peer_id_addr.val[0]);
    minimed_sake_log_evt(line);
  }
  if (conn_handle == s_rejected_pump_conn) {
    // A pump connection we rejected in NORMAL. The stack never saw it connect, so do NOT route its
    // disconnect (that path derefs a never-created GAPLEConnection -> NULL crash). The controller
    // stopped advertising when this connected and the scheduler was never told, so force the Pebble
    // advert back on air here -- otherwise we sit off-air and the phone can't take the freed slot.
    s_rejected_pump_conn = BLE_HS_CONN_HANDLE_NONE;
    gap_le_advert_force_data_refresh();
    minimed_sake_log_evt("pump drop done -> re-advertise");
    return;
  }
  // A pump link went down: either the recorded pump link (handle match, any mode -- covers a
  // disconnect in the window after toggling to NORMAL but before the stack restart) or a known
  // pump identity (covers a stale/reused handle after a stack restart). The pump's connect was
  // swallowed, so the firmware stack never created a GAPLEConnection for it -- routing this
  // disconnect would deref NULL (hard fault).
  if (conn_handle == s_sake_conn_handle ||
      minimed_sake_addr_is_pump(&event->disconnect.conn.peer_id_addr)) {
    s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    minimed_sake_read_stop();  // stop CGM polling; the link is gone
    {
      char line[32];
      snprintf(line, sizeof(line), "disc pump reason=0x%02x", (uint8_t)event->disconnect.reason);
      minimed_sake_log_evt(line);
    }
    minimed_sake_spike_report(MinimedSakeStageDisconnected);
    // The pump just dropped and DUAL expects it to come straight back -- re-arm its advert job at
    // the fast interval instead of leaving it wherever the back-off had gotten to.
    minimed_sake_pump_advert_update();
    gap_le_advert_force_data_refresh();
    return;
  }
  {
    // Untracked disconnect routed into the fw stack: the phone, or a pump the driver never
    // recorded. Name it so a mis-swallowed pump shows up as "disc UNTRACKED <pump addr>" instead
    // of silently passing as a phone disconnect.
    char line[40];
    snprintf(line, sizeof(line), "disc UNTRACKED r=0x%02x %02x:%02x",
             (uint8_t)event->disconnect.reason, event->disconnect.conn.peer_id_addr.val[5],
             event->disconnect.conn.peer_id_addr.val[0]);
    minimed_sake_log_evt(line);
  }
#endif

  GattDeviceDisconnectionEvent gatt_event;
  nimble_addr_to_pebble_addr(&event->disconnect.conn.peer_id_addr, &gatt_event.dev_address);
  bt_driver_cb_gatt_handle_disconnect(&gatt_event);

  struct BleDisconnectionCompleteEvent disconnection_event = {
      .handle = event->disconnect.conn.conn_handle,
      .reason = event->disconnect.reason,
      .status = HciStatusCode_Success,
  };
  nimble_addr_to_pebble_device(&event->disconnect.conn.peer_id_addr,
                               &disconnection_event.peer_address);
  bt_driver_handle_le_disconnection_complete_event(&disconnection_event);
}

static void prv_handle_enc_change_event(struct ble_gap_event *event) {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->enc_change.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_enc_change_event: Failed to find connection descriptor");
    return;
  }

  PBL_LOG_INFO("Encryption change: status=0x%04x encrypted=%u bonded=%u",
               (uint16_t)event->enc_change.status, desc.sec_state.encrypted,
               desc.sec_state.bonded);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  // The pump link (swallowed, so no GAPLEConnection exists): routing this would deref NULL in
  // bt_driver_handle_le_encryption_change_event (gap_le_connect.c) -- the v31 hard-fault class.
  // Handle match in any mode (a stale handle is cleared on the phone-routing connect path).
  if (event->enc_change.conn_handle == s_sake_conn_handle) {
    // The pump link (DUAL): encryption/status change is driver-private. The firmware stack never
    // created a GAPLEConnection for the pump, so routing this would deref NULL in
    // bt_driver_handle_le_encryption_change_event (gap_le_connect.c) -- the v31 hard-fault class.
    if (desc.sec_state.encrypted) {
      minimed_sake_spike_report(MinimedSakeStageEncrypted);
    }
    return;
  }
  if (desc.sec_state.encrypted) {
    minimed_sake_spike_report(MinimedSakeStageEncrypted);
  }
#endif

  struct BleEncryptionChange enc_change_event = {
      .encryption_enabled = desc.sec_state.encrypted,
      .status =
          event->enc_change.status,  // doesn't technically match but only logged so this is fine
  };
  nimble_addr_to_pebble_addr(&desc.peer_id_addr, &enc_change_event.dev_address);
  bt_driver_handle_le_encryption_change_event(&enc_change_event);
}

static void prv_handle_conn_params_updated_event(struct ble_gap_event *event) {
  if (event->conn_update.status != 0) {
    PBL_LOG_ERR("Connection parameters update failed: 0x%04x",
              (uint16_t)event->conn_update.status);
    return;
  }

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_conn_params_updated_event: Failed to find connection descriptor");
    return;
  }

  PBL_LOG_INFO("Connection parameters updated: "
            "itvl=%u ms, latency=%u, spvn timeout=%u ms",
            desc.conn_itvl * BLE_HCI_CONN_ITVL / 1000, desc.conn_latency,
            desc.supervision_timeout * BLE_HCI_CONN_SPVN_TMO_UNITS);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  prv_log_conn_params(&desc);  // the link's duty cycle changed; keep the on-watch record current
#endif

  struct BleConnectionUpdateCompleteEvent conn_params_update_event = {
      .status = HciStatusCode_Success,
  };
  nimble_conn_params_to_pebble(&desc, &conn_params_update_event.conn_params);
  nimble_addr_to_pebble_addr(&desc.peer_id_addr, &conn_params_update_event.dev_address);

  bt_driver_handle_le_conn_params_update_event(&conn_params_update_event);
}

static void prv_handle_conn_update_req_event(struct ble_gap_event *event) {
  *event->conn_update_req.self_params = *event->conn_update_req.peer_params;

  PBL_LOG_INFO("Connection update request: "
            "itvl=(%u, %u) ms, latency=%u, spvn timeout=%u ms",
            event->conn_update_req.self_params->itvl_min * BLE_HCI_CONN_ITVL / 1000,
            event->conn_update_req.self_params->itvl_max * BLE_HCI_CONN_ITVL / 1000,
            event->conn_update_req.self_params->latency,
            event->conn_update_req.self_params->supervision_timeout * BLE_HCI_CONN_SPVN_TMO_UNITS);
}

static void prv_handle_passkey_event(struct ble_gap_event *event) {
  char passkey_str[7];
  uint32_t passkey = 0;
  const char *device_name = NULL;
  PairingUserConfirmationCtx *ctx =
      (PairingUserConfirmationCtx *)((uintptr_t)event->passkey.conn_handle);

  if (event->passkey.params.action == BLE_SM_IOACT_NUMCMP) {
    passkey = event->passkey.params.numcmp;
  }

  if (s_device_name[0] != '\0') {
    device_name = s_device_name;
  }

  snprintf(passkey_str, sizeof(passkey_str), "%06lu", passkey);
  bt_driver_cb_pairing_confirm_handle_request(ctx, device_name, passkey_str);
  s_pairing_in_progress = true;
}

static void prv_handle_pairing_complete_event(struct ble_gap_event *event) {
  PBL_LOG_INFO("Pairing complete: status=0x%04x", (uint16_t)event->pairing_complete.status);

  if (!s_pairing_in_progress) {
    return;
  }

  PairingUserConfirmationCtx *ctx =
      (PairingUserConfirmationCtx *)((uintptr_t)event->pairing_complete.conn_handle);
  bt_driver_cb_pairing_confirm_handle_completed(ctx, event->pairing_complete.status == 0);
  s_pairing_in_progress = false;
}

static void prv_handle_identity_resolved_event(struct ble_gap_event *event) {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->identity_resolved.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_identity_resolved_event: Failed to find connection descriptor");
    return;
  }

  BleAddressChange addr_change_event;
  nimble_addr_to_pebble_device(&desc.peer_ota_addr, &addr_change_event.device);
  nimble_addr_to_pebble_device(&desc.peer_id_addr, &addr_change_event.new_device);
  bt_driver_handle_le_connection_handle_update_address(&addr_change_event);
}

static void prv_handle_mtu_change_event(struct ble_gap_event *event) {
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->mtu.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_mtu_change_event: Failed to find connection descriptor");
    return;
  }

  GattDeviceMtuUpdateEvent mtu_update_event = {.mtu = event->mtu.value};
  nimble_addr_to_pebble_addr(&desc.peer_id_addr, &mtu_update_event.dev_address);
  bt_driver_cb_gatt_handle_mtu_update(&mtu_update_event);
}

extern int pebble_pairing_service_get_connectivity_send_notification(uint16_t conn_handle,
                                                                     uint16_t attr_handle);
static void prv_handle_subscription_event(struct ble_gap_event *event) {
  PBL_LOG_DBG("prv_handle_subscription_event: connhandle: %d attr:%d notify:%d/%d indicate:%d/%d",
            event->subscribe.conn_handle, event->subscribe.attr_handle,
            event->subscribe.prev_notify, event->subscribe.cur_notify,
            event->subscribe.prev_indicate, event->subscribe.cur_indicate);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  minimed_sake_handle_subscribe(event->subscribe.conn_handle, event->subscribe.attr_handle,
                                event->subscribe.cur_notify);
#endif
}

static void prv_handle_notification_rx_event(struct ble_gap_event *event) {
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  // In DUAL mode the pump's CGM notifications/indications land here (watch = GATT client). Let the
  // SAKE read layer consume the ones it owns before the normal Pebble routing sees them.
  if (minimed_sake_get_mode() == MinimedSakeModeDual &&
      minimed_sake_read_handle_notify(event->notify_rx.attr_handle, event->notify_rx.om->om_data,
                                      event->notify_rx.om->om_len)) {
    return;
  }
#endif
  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->notify_rx.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_notification_rx_event: Failed to find connection descriptor");
    return;
  }

  GattServerNotifIndicEvent notification_event = {
      .attr_handle = event->notify_rx.attr_handle,
      .attr_val = event->notify_rx.om->om_data,
      .attr_val_len = event->notify_rx.om->om_len,
  };
  nimble_addr_to_pebble_addr(&desc.peer_id_addr, &notification_event.dev_address);

  if (event->notify_rx.indication == 1) {
    bt_driver_cb_gatt_handle_indication(&notification_event);
  } else {
    bt_driver_cb_gatt_handle_notification(&notification_event);
  }
}

static void prv_handle_notification_tx_event(struct ble_gap_event *event) {
  PBL_LOG_DBG("notification tx event; status=%d attr_handle=%d indication=%d\n",
            event->notify_tx.status,
            event->notify_tx.attr_handle,
            event->notify_tx.indication);
}

static int prv_handle_repeat_pairing_event(struct ble_gap_event *event) {
  // In recovery mode there is no UI that allows to manually delete a pairing,
  // so we unconditionally enable repeat pairing. In main firmware, only allow
  // repeat pairing if using secure connections and we support user confirmation.
// The MiniMed spike uses legacy Just Works (SC_ONLY off, IO NoInputNoOutput), which otherwise
// disables this auto-recovery path -- leaving a mismatched phone/pump bond stuck in a
// connect/terminate(0x13) loop. Re-enable it here so a repeat-pairing just deletes the stale bond
// and re-pairs cleanly.
#if defined(CONFIG_RECOVERY_FW) || defined(CONFIG_MINIMED_SAKE_SPIKE) || \
    (MYNEWT_VAL(BLE_SM_SC_ONLY) && (MYNEWT_VAL(BLE_SM_IO_CAP) == BLE_HS_IO_DISPLAY_YESNO))
  struct ble_gap_conn_desc desc;
  int ret;

  ret = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
  if (ret != 0) {
    return ret;
  }

  PBL_LOG_INFO("Repeat pairing: deleting stored peer keys and retrying");
  ble_store_util_delete_peer(&desc.peer_id_addr);

  return BLE_GAP_REPEAT_PAIRING_RETRY;
#else
  PBL_LOG_WRN("BLE_GAP_EVENT_REPEAT_PAIRING ignored");
  return BLE_GAP_REPEAT_PAIRING_IGNORE;
#endif
}

static void prv_handle_phy_update_event(struct ble_gap_event *event) {
  if (event->phy_updated.status != 0) {
    PBL_LOG_ERR("PHY update failed: 0x%04x",
              (uint16_t)event->phy_updated.status);
    return;
  }

  PBL_LOG_DBG("PHY update complete; conn_handle=%d, tx_phy=%d, rx_phy=%d",
          event->phy_updated.conn_handle, event->phy_updated.tx_phy, event->phy_updated.rx_phy);
}

static int prv_handle_gap_event(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      PBL_LOG_DBG("BLE_GAP_EVENT_CONNECT");
      prv_handle_connection_event(event);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      PBL_LOG_DBG("BLE_GAP_EVENT_DISCONNECT reason=0x%x",
              event->disconnect.reason);
      prv_handle_disconnection_event(event);
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      PBL_LOG_DBG("BLE_GAP_EVENT_ENC_CHANGE");
      prv_handle_enc_change_event(event);
      break;
    case BLE_GAP_EVENT_CONN_UPDATE:
      PBL_LOG_DBG("BLE_GAP_EVENT_CONN_UPDATE");
      prv_handle_conn_params_updated_event(event);
      break;
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
      PBL_LOG_DBG("BLE_GAP_EVENT_CONN_UPDATE_REQ");
      prv_handle_conn_update_req_event(event);
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      PBL_LOG_DBG("BLE_GAP_EVENT_PASSKEY_ACTION");
      prv_handle_passkey_event(event);
      break;
    case BLE_GAP_EVENT_IDENTITY_RESOLVED:
      PBL_LOG_DBG("BLE_GAP_EVENT_IDENTITY_RESOLVED");
      prv_handle_identity_resolved_event(event);
      break;
    case BLE_GAP_EVENT_PAIRING_COMPLETE:
      PBL_LOG_DBG("BLE_GAP_EVENT_PAIRING_COMPLETE");
      prv_handle_pairing_complete_event(event);
      break;
    case BLE_GAP_EVENT_MTU:
      PBL_LOG_DBG("BLE_GAP_EVENT_MTU");
      prv_handle_mtu_change_event(event);
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      PBL_LOG_DBG("BLE_GAP_EVENT_SUBSCRIBE");
      prv_handle_subscription_event(event);
      break;
    case BLE_GAP_EVENT_NOTIFY_RX:
      // no log here because it's incredibly noisy
      prv_handle_notification_rx_event(event);
      break;
    case BLE_GAP_EVENT_NOTIFY_TX:
      PBL_LOG_DBG("BLE_GAP_EVENT_NOTIFY_TX");
      prv_handle_notification_tx_event(event);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      PBL_LOG_DBG("BLE_GAP_EVENT_REPEAT_PAIRING");
      return prv_handle_repeat_pairing_event(event);
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
      PBL_LOG_DBG("BLE_GAP_EVENT_PHY_UPDATE_COMPLETE");
      prv_handle_phy_update_event(event);
      break;
    default:
      PBL_LOG_WRN("Unhandled GAP event: %d", event->type);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
      {
        // Layer 2 diagnostic: an unhandled GAP event could carry a disconnect-like signal (e.g. a
        // termination the host routed oddly). Name it on-watch so a silent pump drop is not
        // invisible here.
        char line[32];
        snprintf(line, sizeof(line), "gap evt unhandled %d", (int)event->type);
        minimed_sake_log_evt(line);
      }
#endif
      break;
  }
  return 0;
}

bool bt_driver_advert_advertising_enable(uint32_t min_interval_ms, uint32_t max_interval_ms) {
  int rc;
  uint8_t own_addr_type;
  struct ble_gap_adv_params advp = {
      .conn_mode = BLE_GAP_CONN_MODE_UND,
      .disc_mode = BLE_GAP_DISC_MODE_GEN,
      .itvl_min = BLE_GAP_ADV_ITVL_MS(min_interval_ms),
      .itvl_max = BLE_GAP_ADV_ITVL_MS(max_interval_ms),
  };

  // A PLAIN identity address (infer_auto(0) -> this watch's static-random identity). The pump
  // reconnects by identity address regardless of the advertised payload (HW-confirmed on asterix).
  // An RPA is NOT required and breaks on the SF32LB52 external LCPU controller (the link fails at
  // accept with 0x10 accept timeout). This is the default for every advert, phone and pump.
  rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to infer own address type (%d)", rc);
    return false;
  }

  rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &advp, prv_handle_gap_event, NULL);
  if (rc != 0) {
    // Log once per failure episode, not every scheduler cycle. With both DUAL links up the
    // connection pool is full, NimBLE rejects connectable advertising with 0x0006 (ENOMEM), and
    // the scheduler retries every second -- a line per attempt would flood both the flash log and
    // the 8-line on-watch ring. The retry itself is load-bearing: it is what puts the pump back
    // on air the moment a slot frees.
#ifdef CONFIG_MINIMED_SAKE_SPIKE
    if (s_last_adv_enable_ok) {
      PBL_LOG_ERR("Failed to start advertising (0x%04x)", (uint16_t)rc);
      if (minimed_sake_get_mode() == MinimedSakeModeDual) {
        char line[32];
        snprintf(line, sizeof(line), "adv START FAIL 0x%04x", (uint16_t)rc);
        minimed_sake_log_evt(line);  // v15 failed here invisibly -- surface the first failure, not the spam
      }
    }
    s_last_adv_enable_ok = false;
#else
    PBL_LOG_ERR("Failed to start advertising (0x%04x)", (uint16_t)rc);
#endif
    return false;
  }

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  if (minimed_sake_get_mode() == MinimedSakeModeDual) {
    // DIAGNOSTIC: what we advertise + which address type. Only on success, so the on-watch ring
    // does not spin with "adv EN" lines while both links are up and advertising is rejected.
    char line[32];
    snprintf(line, sizeof(line), "adv EN t%u %ums", own_addr_type, (unsigned)min_interval_ms);
    minimed_sake_log(line);
  }
  s_last_adv_enable_ok = true;
#endif
  return true;
}
