/* SPDX-FileCopyrightText: 2026 Pal Marci */
/* SPDX-License-Identifier: Apache-2.0 */

// A stand-in pump link for boards with no radio (CONFIG_BT_HCI_FAKE, i.e. QEMU). The real SAKE
// service and advertising hooks run on the NimBLE host as usual, but no connection can ever come
// up, so this replaces the GATT transport and the cipher. It only exists to boot the MiniMed task
// in the emulator and let the session's setup and timers run, not to simulate the pump: the link
// comes up once, every operation succeeds and reads return a few fixed bytes. A command on the
// IDD Command Control Point is answered "opcode not supported", so the Snooze/Confirm probe runs.

#include "minimed_fake_pump.h"

#include "minimed_task.h"
#include "minimed_transport.h"
#include "minimed_transport_nimble.h"
#include "pbl/services/new_timer/new_timer.h"
#include "popups/minimed_sake_ui.h"

#include <string.h>

#include <pbl/logging/logging.h>

PBL_LOG_MODULE_DECLARE(bt, CONFIG_BT_LOG_LEVEL);

#define LINK_UP_MS (5 * 1000)
#define ATT_MTU    23
#define CIPHER_PAD 3 // the real SeqCrypt trailer length

static bool s_connected;
static TimerID s_link_timer;

static void prv_post(MinimedEventType type, MinimedChr chr, uint8_t tag, const uint8_t *data,
                     uint16_t len) {
  MinimedEvent e = {.type = type, .chr = chr, .tag = tag, .status = {.ok = true}};
  if (data) {
    memcpy(e.data, data, len);
    e.len = len;
  }
  minimed_task_post(&e);
}

static void prv_link_timer_cb(void *unused) {
  s_connected = true;
  PBL_LOG_INFO("fake pump: link up");
  prv_post(MinimedEventLinkUp, MinimedChrCount, 0, NULL, 0);
}

void minimed_fake_pump_start(void) {
  if (s_link_timer) {
    return; // runs again on every Bluetooth off/on
  }
  minimed_task_init();
  s_link_timer = new_timer_create();
  new_timer_start(s_link_timer, LINK_UP_MS, prv_link_timer_cb, NULL, 0);
  PBL_LOG_INFO("fake pump: MiniMed task started, link up in %d ms", LINK_UP_MS);
}

// -- minimed_transport.h: everything succeeds ---------------------------------------------

void minimed_transport_discover(void) {
  prv_post(MinimedEventDiscovered, MinimedChrCount, 0, NULL, 0);
}

bool minimed_transport_has(MinimedChr chr) {
  return s_connected && chr < MinimedChrCount;
}

bool minimed_transport_read(MinimedChr chr, uint8_t tag, MinimedGattStatus *status) {
  static const uint8_t value[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  if (status)
    *status = (MinimedGattStatus){.ok = s_connected};
  if (!s_connected)
    return false;
  prv_post(MinimedEventGattDone, chr, tag, value, sizeof(value));
  return true;
}

bool minimed_transport_write(MinimedChr chr, const uint8_t *data, uint16_t len, uint8_t tag,
                             MinimedGattStatus *status) {
  if (status)
    *status = (MinimedGattStatus){.ok = s_connected};
  if (!s_connected)
    return false;
  if (tag)
    prv_post(MinimedEventGattDone, chr, tag, NULL, 0);
  if (chr == MinimedChrIddCommandCp && len >= 2) {
    // Response Code: 0x0f55 | request opcode | 0x70 opcode not supported | cipher pad
    const uint8_t resp[] = {0x55, 0x0f, data[0], data[1], 0x70, 0, 0, 0};
    prv_post(MinimedEventNotify, chr, 0, resp, sizeof(resp));
  }
  return true;
}

bool minimed_transport_subscribe(MinimedChr chr, bool indicate, uint8_t tag,
                                 MinimedGattStatus *status) {
  return minimed_transport_write(chr, NULL, 0, tag, status);
}

uint16_t minimed_transport_max_value_len(void) {
  return ATT_MTU - 3;
}

bool minimed_transport_link_alive(uint16_t *mtu) {
  if (mtu)
    *mtu = s_connected ? ATT_MTU : 0;
  return s_connected;
}

bool minimed_sake_decrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t out_cap,
                          uint16_t *out_len) {
  if (n < CIPHER_PAD || n - CIPHER_PAD > out_cap)
    return false;
  memcpy(out, in, n - CIPHER_PAD);
  *out_len = n - CIPHER_PAD;
  return true;
}

bool minimed_sake_encrypt(const uint8_t *in, uint16_t n, uint8_t *out, uint16_t *out_len) {
  memcpy(out, in, n);
  memset(out + n, 0, CIPHER_PAD);
  *out_len = n + CIPHER_PAD;
  return true;
}

// -- NimBLE transport hooks the advertising and service code call: no link ever exists here --

void minimed_transport_nimble_link_up(uint16_t conn_handle) {
}
void minimed_transport_nimble_link_down(void) {
}
bool minimed_transport_nimble_handle_notify(uint16_t conn_handle, uint16_t attr_handle,
                                            const struct os_mbuf *om) {
  return false;
}
