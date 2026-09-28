/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

// minimed_transport.h on NimBLE: GATT client plumbing for the pump link and nothing else. Every
// callback here runs on the NimBLE host task and only copies the result into a MinimedEvent for
// the MiniMed task; the session logic lives in src/fw/services/minimed/minimed_session.c.

#include "minimed_transport_nimble.h"

#include <string.h>

#include "comm/bt_lock.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "minimed_task.h"
#include "minimed_transport.h"

#define CGM_SERVICE_UUID 0x181F
#define RACP_UUID 0x2A52

// Medtronic vendor base 0000XXXX-0000-1000-0000-009132591325, little-endian; the last two data
// bytes are the 16-bit short code low/high (00 01 for 0x0100, 05 01 for 0x0105).
#define MEDTRONIC_UUID128(lo, hi)                                                          \
  BLE_UUID128_INIT(0x25, 0x13, 0x59, 0x32, 0x91, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, \
                   (lo), (hi), 0x00, 0x00)

static const ble_uuid128_t s_idd_svc_uuid = MEDTRONIC_UUID128(0x00, 0x01);
static const ble_uuid128_t s_idd_srcp_uuid = MEDTRONIC_UUID128(0x05, 0x01);
static const ble_uuid128_t s_idd_status_changed_uuid = MEDTRONIC_UUID128(0x01, 0x01);
static const ble_uuid128_t s_idd_status_uuid = MEDTRONIC_UUID128(0x02, 0x01);
static const ble_uuid128_t s_idd_hist_uuid = MEDTRONIC_UUID128(0x08, 0x01);
static const ble_uuid128_t s_sensor_exp_uuid = MEDTRONIC_UUID128(0x02, 0x02);
static const ble_uuid128_t s_idd_features_uuid = MEDTRONIC_UUID128(0x04, 0x01);

// The read-by-UUID characteristics: no discovery, a read over the whole handle range. None of
// these 16-bit SIG UUIDs can collide with the vendor 128-bit ones.
static const uint16_t s_by_uuid16[MinimedChrCount] = {
    [MinimedChrBattery] = 0x2A19,
    [MinimedChrSessionRunTime] = 0x2AAB,
    [MinimedChrDisManufacturer] = 0x2A29,
    [MinimedChrDisModel] = 0x2A24,
    [MinimedChrDisSerial] = 0x2A25,
    [MinimedChrDisHardware] = 0x2A27,
    [MinimedChrDisFirmware] = 0x2A26,
    [MinimedChrDisSoftware] = 0x2A28,
    [MinimedChrDisSystemId] = 0x2A23,
    [MinimedChrDisPnpId] = 0x2A50,
    [MinimedChrDisRegulatory] = 0x2A2A,
};

#define CONN_NONE 0xFFFF

// All of it written on the host task during discovery and link changes, read on the MiniMed task
// by the operations below; bt_lock covers both.
static uint16_t s_conn = CONN_NONE;
static uint16_t s_handles[MinimedChrCount];  // value handles of the discovered characteristics
static uint16_t s_svc_start, s_svc_end;      // the service being walked
static bool s_by_uuid_delivered[MinimedChrCount];

static MinimedGattStatus prv_status(int rc) {
  const uint16_t status = (uint16_t)rc;
  const uint16_t hci = status >= BLE_HS_ERR_HCI_BASE ? status - BLE_HS_ERR_HCI_BASE : 0;
  // ENOTCONN / ETIMEOUT, or an HCI disconnect reason (0x08 supervision timeout, 0x13 remote
  // terminated, 0x16 local terminated, 0x22 LL response timeout, 0x3e failed to establish): the
  // host knows the link is gone, and a disconnect event should follow shortly.
  const bool link_dead = status == BLE_HS_ENOTCONN || status == BLE_HS_ETIMEOUT ||
                         hci == 0x08 || hci == 0x13 || hci == 0x16 || hci == 0x22 || hci == 0x3e;
  return (MinimedGattStatus){.ok = rc == 0, .link_dead = link_dead, .code = status};
}

static void prv_post_simple(MinimedEventType type) {
  const MinimedEvent event = {.type = type};
  minimed_task_post(&event);
}

// A GATT result for the connection the operation was issued on; one from an earlier connection
// is dropped rather than read as belonging to the new one.
static bool prv_is_current(uint16_t conn) {
  bt_lock();
  const bool current = (conn == s_conn);
  bt_unlock();
  return current;
}

static void prv_post_done(MinimedChr chr, uint8_t tag, MinimedGattStatus status,
                          const struct os_mbuf *om) {
  MinimedEvent event = {
      .type = MinimedEventGattDone,
      .chr = chr,
      .tag = tag,
      .status = status,
  };
  if (om) {
    const uint16_t len = OS_MBUF_PKTLEN(om);
    event.truncated = len > sizeof(event.data);
    event.len = event.truncated ? sizeof(event.data) : len;
    os_mbuf_copydata(om, 0, event.len, event.data);
  }
  minimed_task_post(&event);
}

#define PACK_ARG(chr, tag) ((void *)(uintptr_t)(((chr) << 8) | (tag)))
#define ARG_CHR(arg) ((MinimedChr)(((uintptr_t)(arg)) >> 8))
#define ARG_TAG(arg) ((uint8_t)(uintptr_t)(arg))

// -- Discovery: CGM service, its characteristics, then the same for IDD --------------------------

static void prv_discovery_done(void) { prv_post_simple(MinimedEventDiscovered); }

static int prv_disc_idd_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg) {
  if (!prv_is_current(conn)) return 0;
  if (error->status == 0 && chr) {
    bt_lock();
    if (ble_uuid_cmp(&chr->uuid.u, &s_idd_srcp_uuid.u) == 0) {
      s_handles[MinimedChrIddSrcp] = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_status_changed_uuid.u) == 0) {
      s_handles[MinimedChrIddStatusChanged] = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_status_uuid.u) == 0) {
      s_handles[MinimedChrIddStatus] = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_idd_hist_uuid.u) == 0) {
      s_handles[MinimedChrIddHistory] = chr->val_handle;
    } else if (chr->uuid.u.type == BLE_UUID_TYPE_16 && ble_uuid_u16(&chr->uuid.u) == RACP_UUID) {
      // The IDD service has its own RACP: same SIG 0x2A52 as the CGM one, different handle.
      s_handles[MinimedChrIddRacp] = chr->val_handle;
    }
    bt_unlock();
    return 0;
  }
  prv_discovery_done();  // BLE_HS_EDONE or an error: report whatever was found
  return 0;
}

static int prv_disc_idd_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *service, void *arg) {
  if (!prv_is_current(conn)) return 0;
  if (error->status == 0 && service) {
    s_svc_start = service->start_handle;
    s_svc_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE || s_svc_start == 0 ||
      ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, prv_disc_idd_chr_cb, NULL) != 0) {
    prv_discovery_done();  // no IDD service: BG still works, without IOB/status/alerts
  }
  return 0;
}

static void prv_discover_idd(uint16_t conn) {
  s_svc_start = s_svc_end = 0;
  if (ble_gattc_disc_svc_by_uuid(conn, &s_idd_svc_uuid.u, prv_disc_idd_svc_cb, NULL) != 0) {
    prv_discovery_done();
  }
}

static int prv_disc_cgm_chr_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_chr *chr, void *arg) {
  if (!prv_is_current(conn)) return 0;
  if (error->status == 0 && chr) {
    const uint16_t u = (chr->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&chr->uuid.u) : 0;
    bt_lock();
    if (u == 0x2AA7) {
      s_handles[MinimedChrCgmMeasurement] = chr->val_handle;
    } else if (u == 0x2AA8) {
      s_handles[MinimedChrCgmFeature] = chr->val_handle;
    } else if (u == RACP_UUID) {
      s_handles[MinimedChrCgmRacp] = chr->val_handle;
    } else if (u == 0x2AAA) {
      s_handles[MinimedChrCgmSessionStart] = chr->val_handle;
    } else if (ble_uuid_cmp(&chr->uuid.u, &s_sensor_exp_uuid.u) == 0) {
      s_handles[MinimedChrSensorExpiration] = chr->val_handle;
    }
    bt_unlock();
    return 0;
  }
  prv_discover_idd(conn);
  return 0;
}

static int prv_disc_cgm_svc_cb(uint16_t conn, const struct ble_gatt_error *error,
                               const struct ble_gatt_svc *service, void *arg) {
  if (!prv_is_current(conn)) return 0;
  if (error->status == 0 && service) {
    s_svc_start = service->start_handle;
    s_svc_end = service->end_handle;
    return 0;
  }
  if (error->status != BLE_HS_EDONE || s_svc_start == 0 ||
      ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, prv_disc_cgm_chr_cb, NULL) != 0) {
    prv_discover_idd(conn);
  }
  return 0;
}

void minimed_transport_discover(void) {
  bt_lock();
  const uint16_t conn = s_conn;
  memset(s_handles, 0, sizeof(s_handles));
  s_svc_start = s_svc_end = 0;
  bt_unlock();
  const ble_uuid16_t svc_uuid = BLE_UUID16_INIT(CGM_SERVICE_UUID);
  if (conn == CONN_NONE ||
      ble_gattc_disc_svc_by_uuid(conn, &svc_uuid.u, prv_disc_cgm_svc_cb, NULL) != 0) {
    prv_discovery_done();
  }
}

// -- Operations -----------------------------------------------------------------------------

static const ble_uuid_t *prv_by_uuid(MinimedChr chr, ble_uuid16_t *storage) {
  if (chr == MinimedChrIddFeatures) {
    return &s_idd_features_uuid.u;
  }
  *storage = (ble_uuid16_t)BLE_UUID16_INIT(s_by_uuid16[chr]);
  return &storage->u;
}

bool minimed_transport_has(MinimedChr chr) {
  if (chr >= MinimedChrCount) return false;
  bt_lock();
  const bool has = s_conn != CONN_NONE && (chr >= MINIMED_CHR_FIRST_BY_UUID || s_handles[chr] != 0);
  bt_unlock();
  return has;
}

static int prv_read_cb(uint16_t conn, const struct ble_gatt_error *error,
                       struct ble_gatt_attr *attr, void *arg) {
  if (!prv_is_current(conn)) return 0;
  prv_post_done(ARG_CHR(arg), ARG_TAG(arg), prv_status(error->status),
                error->status == 0 && attr ? attr->om : NULL);
  return 0;
}

// read_by_uuid calls back once per matching attribute, then once with BLE_HS_EDONE. Deliver the
// first value only; an EDONE with none delivered is "not there" (ok false, code 0).
static int prv_read_by_uuid_cb(uint16_t conn, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg) {
  if (!prv_is_current(conn)) return 0;
  const MinimedChr chr = ARG_CHR(arg);
  if (error->status == 0 && attr) {
    if (!s_by_uuid_delivered[chr]) {
      s_by_uuid_delivered[chr] = true;
      prv_post_done(chr, ARG_TAG(arg), prv_status(0), attr->om);
    }
    return 0;
  }
  if (!s_by_uuid_delivered[chr]) {
    MinimedGattStatus status = prv_status(error->status);
    if (error->status == BLE_HS_EDONE) {
      status = (MinimedGattStatus){.ok = false, .link_dead = false, .code = 0};
    }
    s_by_uuid_delivered[chr] = true;
    prv_post_done(chr, ARG_TAG(arg), status, NULL);
  }
  return 0;
}

bool minimed_transport_read(MinimedChr chr, uint8_t tag, MinimedGattStatus *status) {
  bt_lock();
  const uint16_t conn = s_conn;
  const uint16_t handle = chr < MinimedChrCount ? s_handles[chr] : 0;
  bt_unlock();
  int rc = BLE_HS_ENOTCONN;
  if (conn != CONN_NONE && chr < MinimedChrCount) {
    if (chr >= MINIMED_CHR_FIRST_BY_UUID) {
      ble_uuid16_t storage;
      s_by_uuid_delivered[chr] = false;
      rc = ble_gattc_read_by_uuid(conn, 0x0001, 0xffff, prv_by_uuid(chr, &storage),
                                  prv_read_by_uuid_cb, PACK_ARG(chr, tag));
    } else {
      rc = handle ? ble_gattc_read(conn, handle, prv_read_cb, PACK_ARG(chr, tag)) : BLE_HS_ENOENT;
    }
  }
  if (status) *status = prv_status(rc);
  return rc == 0;
}

static int prv_write_cb(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg) {
  if (!prv_is_current(conn)) return 0;
  prv_post_done(ARG_CHR(arg), ARG_TAG(arg), prv_status(error->status), NULL);
  return 0;
}

static bool prv_write_handle(MinimedChr chr, uint16_t offset, const uint8_t *data, uint16_t len,
                             uint8_t tag, MinimedGattStatus *status) {
  bt_lock();
  const uint16_t conn = s_conn;
  const uint16_t handle = chr < MINIMED_CHR_FIRST_BY_UUID ? s_handles[chr] : 0;
  bt_unlock();
  int rc = BLE_HS_ENOTCONN;
  if (conn != CONN_NONE) {
    rc = handle ? ble_gattc_write_flat(conn, handle + offset, data, len,
                                       tag ? prv_write_cb : NULL, PACK_ARG(chr, tag))
                : BLE_HS_ENOENT;
  }
  if (status) *status = prv_status(rc);
  return rc == 0;
}

bool minimed_transport_write(MinimedChr chr, const uint8_t *data, uint16_t len, uint8_t tag,
                             MinimedGattStatus *status) {
  return prv_write_handle(chr, 0, data, len, tag, status);
}

bool minimed_transport_subscribe(MinimedChr chr, bool indicate, uint8_t tag,
                                 MinimedGattStatus *status) {
  // The CCCD sits right after the value handle on every characteristic this pump exposes.
  const uint8_t cccd[] = {indicate ? 0x02 : 0x01, 0x00};
  return prv_write_handle(chr, 1, cccd, sizeof(cccd), tag, status);
}

uint16_t minimed_transport_max_value_len(void) {
  bt_lock();
  const uint16_t conn = s_conn;
  bt_unlock();
  const uint16_t mtu = conn != CONN_NONE ? ble_att_mtu(conn) : 0;
  return (mtu > 3) ? (mtu - 3) : 20;
}

bool minimed_transport_link_alive(uint16_t *mtu) {
  bt_lock();
  const uint16_t conn = s_conn;
  bt_unlock();
  struct ble_gap_conn_desc desc;
  const bool alive = conn != CONN_NONE && ble_gap_conn_find(conn, &desc) == 0;
  if (mtu) *mtu = alive ? ble_att_mtu(conn) : 0;
  return alive;
}

// -- Driver-facing entry points (minimed_transport_nimble.h) ---------------------------------

void minimed_transport_nimble_link_up(uint16_t conn_handle) {
  bt_lock();
  s_conn = conn_handle;
  memset(s_handles, 0, sizeof(s_handles));
  bt_unlock();
  prv_post_simple(MinimedEventLinkUp);
}

void minimed_transport_nimble_link_down(void) {
  bt_lock();
  const bool was_up = s_conn != CONN_NONE;
  s_conn = CONN_NONE;
  memset(s_handles, 0, sizeof(s_handles));
  bt_unlock();
  if (was_up) {
    prv_post_simple(MinimedEventLinkDown);
  }
}

bool minimed_transport_nimble_handle_notify(uint16_t conn_handle, uint16_t attr_handle,
                                            const struct os_mbuf *om) {
  MinimedChr chr = MinimedChrCount;
  bt_lock();
  if (conn_handle == s_conn && attr_handle != 0) {
    for (unsigned i = 0; i < MINIMED_CHR_FIRST_BY_UUID; i++) {
      if (s_handles[i] == attr_handle) {
        chr = (MinimedChr)i;
        break;
      }
    }
  }
  bt_unlock();
  if (chr == MinimedChrCount) {
    return false;  // not ours; the caller hands it on
  }
  MinimedEvent event = {.type = MinimedEventNotify, .chr = chr};
  const uint16_t len = OS_MBUF_PKTLEN(om);
  event.truncated = len > sizeof(event.data);
  event.len = event.truncated ? sizeof(event.data) : len;
  os_mbuf_copydata(om, 0, event.len, event.data);
  minimed_task_post(&event);
  return true;
}
