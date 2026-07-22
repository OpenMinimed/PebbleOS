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
#include <system/logging.h>
#include <system/passert.h>
#include <pbl/util/math.h>

#include "nimble_type_conversions.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE
#include "minimed_sake_read.h"
#include "minimed_sake_service.h"
#include "popups/minimed_sake_spike_ui.h"
#endif

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

static const ble_uuid16_t s_device_name_chr_uuid = BLE_UUID16_INIT(0x2A00);
static char s_device_name[BT_DEVICE_NAME_BUFFER_SIZE];
static bool s_pairing_in_progress;

#ifdef CONFIG_MINIMED_SAKE_SPIKE
static uint16_t s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;

// Drop the active link (if any) so the connection manager re-advertises under the current mode.
// ble_gap_terminate takes the ble_hs lock internally, so this is safe to call from the app task.
void minimed_sake_force_readvertise(void) {
  if (s_sake_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
    // Active link: terminate it. The disconnect makes the connection manager re-advertise, and the
    // advertising_enable clamp below makes that fast + Medtronic in SPIKE mode.
    ble_gap_terminate(s_sake_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    return;
  }
  // No active link (e.g. phone Bluetooth already off): nothing else would re-trigger advertising,
  // so in SPIKE mode kick the fast Medtronic advert directly. Makes the toggle order not matter.
  if (minimed_sake_get_mode() == MinimedSakeModeSpike) {
    ble_gap_adv_stop();
    bt_driver_advert_advertising_enable(100, 140);
  }
}
#endif

static int prv_device_name_read_event_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         struct ble_gatt_attr *attr, void *arg) {
  if (error->status == 0) {
    size_t len = MIN(attr->om->om_len, sizeof(s_device_name) - 1);
    strncpy(s_device_name, (char *)attr->om->om_data, len);
    s_device_name[len] = '\0';
  }

  return 0;
}

void bt_driver_advert_advertising_disable(void) {
  int rc;

  if (ble_gap_adv_active() == 0) {
    return;
  }

  rc = ble_gap_adv_stop();
  PBL_ASSERT(rc == 0, "Failed to stop advertising (0x%04x)", (uint16_t)rc);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  if (minimed_sake_get_mode() == MinimedSakeModeSpike) {
    minimed_sake_log("adv DISABLE");
  }
#endif
}

bool bt_driver_advert_client_get_tx_power(int8_t *tx_power) { return false; }

bool bt_driver_advert_set_advertising_data(const BLEAdData *ad_data) {
  int rc;

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  // In SAKE mode, hijack the advert to pose as a Medtronic pump peripheral. In NORMAL mode, fall
  // through to the real Pebble advert so the phone connects (and firmware can be sideloaded).
  if (minimed_sake_get_mode() == MinimedSakeModeSpike) {
    uint8_t sake_adv[31];
    uint8_t sake_adv_len = minimed_sake_build_adv(sake_adv, sizeof(sake_adv));
    rc = ble_gap_adv_set_data(sake_adv, sake_adv_len);
    if (rc != 0) {
      PBL_LOG_ERR("SAKE: failed to set Medtronic advert (0x%04x)", (uint16_t)rc);
      return false;
    }
    minimed_sake_log(minimed_sake_pump_paired() ? "adv data: FE81" : "adv data: FE82");
    return true;
  }
#endif

  rc = ble_gap_adv_set_data((uint8_t *)&ad_data->data, ad_data->ad_data_length);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to set advertising data (0x%04x)", (uint16_t)rc);
    return false;
  }

  rc = ble_gap_adv_rsp_set_data((uint8_t *)&ad_data->data[ad_data->ad_data_length],
                                ad_data->scan_resp_data_length);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to set scan response data (0x%04x)", (uint16_t)rc);
    return false;
  }

  return true;
}

static void prv_handle_connection_event(struct ble_gap_event *event) {
  // we only want to notify on a successful connection
  if (event->connect.status != 0) return;

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  s_sake_conn_handle = event->connect.conn_handle;
  minimed_sake_spike_report(MinimedSakeStageConnected);
#endif

  struct ble_gap_conn_desc desc;
  if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0) {
    PBL_LOG_ERR("prv_handle_connection_event: Failed to find connection descriptor");
    return;
  }

  struct BleConnectionCompleteEvent complete_event = {
      .handle = event->connect.conn_handle,
      .is_master = desc.role == BLE_GAP_ROLE_MASTER,
      .status = HciStatusCode_Success,
      .mtu = ble_att_mtu(event->connect.conn_handle),
  };

  // If OTA address != ID address, then the address must be resolved.
  // This happens for an already paired devices.
  complete_event.is_resolved = ble_addr_cmp(&desc.peer_id_addr, &desc.peer_ota_addr) != 0;
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
      complete_event.is_resolved = false;
    } else {
      memcpy(complete_event.irk.data, value_sec.irk, 16);
    }
  } else {
    // If the address is not resolved, pairing is gonna happen.
    // Trigger name read to have it ready for the pairing confirmation.
    memset(s_device_name, 0, sizeof(s_device_name));
    ble_gattc_read_by_uuid(event->connect.conn_handle, 1, UINT16_MAX,
                           (ble_uuid_t *)&s_device_name_chr_uuid, prv_device_name_read_event_cb,
                           NULL);
  }

  nimble_conn_params_to_pebble(&desc, &complete_event.conn_params);
  nimble_addr_to_pebble_device(&desc.peer_id_addr, &complete_event.peer_address);

  s_pairing_in_progress = false;

  bt_driver_handle_le_connection_complete_event(&complete_event);
}

static void prv_handle_disconnection_event(struct ble_gap_event *event) {
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  s_sake_conn_handle = BLE_HS_CONN_HANDLE_NONE;
  minimed_sake_read_stop();  // stop CGM polling; the link is gone
  {
    char line[32];
    snprintf(line, sizeof(line), "disc reason=0x%02x", (uint8_t)event->disconnect.reason);
    minimed_sake_log(line);
  }
  minimed_sake_spike_report(MinimedSakeStageDisconnected);
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

#ifdef CONFIG_MINIMED_SAKE_SPIKE
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
  // In spike mode the pump's CGM notifications/indications land here (watch = GATT client). Let the
  // SAKE read layer consume the ones it owns before the normal Pebble routing sees them.
  if (minimed_sake_get_mode() == MinimedSakeModeSpike &&
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
      break;
  }
  return 0;
}

bool bt_driver_advert_advertising_enable(uint32_t min_interval_ms, uint32_t max_interval_ms) {
  int rc;
#ifdef CONFIG_MINIMED_SAKE_SPIKE
  bool spike_mode = minimed_sake_get_mode() == MinimedSakeModeSpike;
  unsigned spike_orig_max_ms = (unsigned)max_interval_ms;
  if (spike_mode) {
    // The pump ignores adverts slower than ~150ms, but the reconnection job uses ~1s. Force a fast
    // interval, and re-assert the Medtronic payload here (the reconnection job may reuse cached
    // Pebble advert data without calling set_advertising_data), so SPIKE always advertises fast and
    // as "Mobile PB" no matter who enabled advertising.
    min_interval_ms = 100;
    max_interval_ms = 140;
    uint8_t sake_adv[31];
    uint8_t sake_adv_len = minimed_sake_build_adv(sake_adv, sizeof(sake_adv));
    ble_gap_adv_set_data(sake_adv, sake_adv_len);
  }
#endif
  uint8_t own_addr_type;
  struct ble_gap_adv_params advp = {
      .conn_mode = BLE_GAP_CONN_MODE_UND,
      .disc_mode = BLE_GAP_DISC_MODE_GEN,
      .itvl_min = BLE_GAP_ADV_ITVL_MS(min_interval_ms),
      .itvl_max = BLE_GAP_ADV_ITVL_MS(max_interval_ms),
  };

  rc = ble_hs_id_infer_auto(0, &own_addr_type);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to infer own address type (%d)", rc);
    return false;
  }

#ifdef CONFIG_MINIMED_SAKE_SPIKE
  if (spike_mode) {
    // Address type by pairing state, mirroring the FE82/FE81 service split:
    //  - First-pair (FE82): a PLAIN identity address (infer_auto(0) -> this watch's static-random
    //    identity). The pump discovers + pairs with a plain address (Documentation/bluetooth.md);
    //    an RPA for first-pair proved undiscoverable on hardware (v15-v19 regression) and is what
    //    v10-v14 did NOT do.
    //  - Reconnect (FE81): an RPA (infer_auto(1)); the pump only reconnects to an RPA and resolves
    //    it via the IRK we distribute during pairing.
    const int privacy = minimed_sake_pump_paired() ? 1 : 0;
    uint8_t at;
    if (ble_hs_id_infer_auto(privacy, &at) == 0) {
      own_addr_type = at;
    } else {
      minimed_sake_log("no adv identity!");
    }
    char line[32];
    snprintf(line, sizeof(line), "adv EN FE8%c t%u %u->140ms",
             minimed_sake_pump_paired() ? '1' : '2', own_addr_type, spike_orig_max_ms);
    minimed_sake_log(line);
  }
#endif

  rc = ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &advp, prv_handle_gap_event, NULL);
  if (rc != 0) {
    PBL_LOG_ERR("Failed to start advertising (0x%04x)", (uint16_t)rc);
#ifdef CONFIG_MINIMED_SAKE_SPIKE
    if (spike_mode) {
      char line[32];
      snprintf(line, sizeof(line), "adv START FAIL 0x%04x", (uint16_t)rc);
      minimed_sake_log(line);  // v15 failed here invisibly -- always surface this on-watch
    }
#endif
    return false;
  }

  return true;
}
