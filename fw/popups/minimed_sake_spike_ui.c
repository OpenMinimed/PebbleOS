/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#include "minimed_sake_spike_ui.h"

#ifdef CONFIG_MINIMED_SAKE_SPIKE

#include "comm/ble/gap_le_advert.h"
#include "kernel/event_loop.h"
#include "kernel/pbl_malloc.h"
#include "pbl/drivers/rtc.h"
#include "pbl/logging/logging.h"
#include "pbl/services/bluetooth/bluetooth_ctl.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define SAKE_LOG_LINES 14
#define SAKE_LOG_WIDTH 40
#define SAKE_LOG_TS_LEN 9  // "HH:MM:SS "

// Log state is mutated only on KernelMain (via prv_append_cb); the app task only reads s_joined,
// where a torn read is at worst a few garbage chars in a debug line -- acceptable, so no lock.
static char s_lines[SAKE_LOG_LINES][SAKE_LOG_WIDTH];
static int s_line_count;
static char s_joined[SAKE_LOG_LINES * SAKE_LOG_WIDTH];

static MinimedSakeMode s_mode = MinimedSakeModeNormal;

static void prv_rebuild_joined(void) {
  size_t off = 0;
  s_joined[0] = '\0';
  for (int i = 0; i < s_line_count && off < sizeof(s_joined); i++) {
    int n = snprintf(s_joined + off, sizeof(s_joined) - off, "%s\n", s_lines[i]);
    if (n < 0) {
      break;
    }
    off += n;
  }
}

static void prv_append_cb(void *data) {
  char *msg = (char *)data;
  char *slot;
  if (s_line_count < SAKE_LOG_LINES) {
    slot = s_lines[s_line_count++];
  } else {
    for (int i = 1; i < SAKE_LOG_LINES; i++) {
      memcpy(s_lines[i - 1], s_lines[i], SAKE_LOG_WIDTH);
    }
    slot = s_lines[SAKE_LOG_LINES - 1];
  }
  // Prefix the wall-clock time so a line's recency is readable on the watch without a phone.
  time_t now = rtc_get_time();
  struct tm t;
  localtime_r(&now, &t);
  snprintf(slot, SAKE_LOG_WIDTH, "%02d:%02d:%02d %s", t.tm_hour, t.tm_min, t.tm_sec, msg);
  slot[SAKE_LOG_WIDTH - 1] = '\0';
  prv_rebuild_joined();
  kernel_free(msg);
}

static const char *prv_stage_text(MinimedSakeStage stage) {
  switch (stage) {
    case MinimedSakeStageAdvertising:  return "advertising";
    case MinimedSakeStageConnected:    return "connected";
    case MinimedSakeStageEncrypted:    return "paired/encrypted";
    case MinimedSakeStageSubscribed:   return "subscribed";
    case MinimedSakeStageWrote:        return "PUMP WROTE!";
    case MinimedSakeStageHandshakeComplete: return "HANDSHAKE OK!";
    case MinimedSakeStageDisconnected: return "disconnected";
    default:                           return "?";
  }
}

// Connectivity events that must survive to the flash log. The 14-line ring is fine for watching
// the screen live, but a dropout gets diagnosed hours later from dump_flash_logs.py, and
// everything about the pump link -- its connect classification, its disconnects, whether its
// advert job even exists -- was ring-only. Two nights and one wrong fix went into rediscovering
// that. Deliberately NOT used for the per-poll read path, which has its own PBL_LOGs, nor for
// anything on the advert rotation path, which fires about once a second while two jobs exist.
void minimed_sake_log_evt(const char *msg) {
  PBL_LOG_INFO("SAKE: %s", msg);
  minimed_sake_log(msg);
}

void minimed_sake_log(const char *msg) {
  size_t n = strlen(msg) + 1;
  char *copy = (char *)kernel_malloc(n);
  if (!copy) {
    return;
  }
  memcpy(copy, msg, n);
  launcher_task_add_callback(prv_append_cb, copy);
}

// Deliberately silent. These stages used to buzz the motor (short on connect/subscribe, double on
// the handshake milestones) back when reaching them at all was the news. Now the pump re-handshakes
// on every reconnect, so a night of dropouts is a night of buzzing -- and these are raw vibes_*
// calls that ignore Quiet Time. The on-watch log is the debugging channel.
void minimed_sake_spike_report(MinimedSakeStage stage) { minimed_sake_log(prv_stage_text(stage)); }

MinimedSakeMode minimed_sake_get_mode(void) { return s_mode; }

const char *minimed_sake_get_log(void) { return s_joined; }

// Watchdog recovery: the pump link went silent. Keep the mode DUAL and run the NORMAL kill-switch
// restart -- it frees the phantom connection slot, drops the silent link, and re-arms advertising.
// The SAKE service re-arms the full DUAL state when the restarted stack re-inits
// (minimed_sake_service_init). bt_ctl_reset_bluetooth is async (scheduled on the system task), so
// there is no race with the mode flag here.
void minimed_sake_watchdog_retoggle(void) {
  if (minimed_sake_get_mode() != MinimedSakeModeDual) {
    return;
  }
  minimed_sake_clear_link_state();  // a stale pump handle must not alias (and swallow) a phone link
  minimed_sake_pump_advert_stop();
  minimed_sake_sender_set_mode(false);
  gap_le_advert_set_allow_advert_while_connected(false);
  bt_ctl_reset_bluetooth();
}

// Called by bluetooth_ctl.c once the whole Bluetooth stack is up. The advert half of the DUAL
// re-arm has to happen here, not in minimed_sake_service_init: that one runs inside
// bt_driver_start, and gap_le_init() -> gap_le_advert_init() runs right after it and resets
// s_jobs, s_current and s_allow_advert_while_connected. A job scheduled any earlier is thrown
// away -- gap_le_advert_schedule frees it outright while the module is deinitialised -- so after a
// stack restart the pump had no advert at all and could never reconnect.
void minimed_sake_bt_started(void) {
  if (minimed_sake_get_mode() != MinimedSakeModeDual) {
    return;
  }
  gap_le_advert_set_allow_advert_while_connected(true);
  minimed_sake_pump_advert_start();
  minimed_sake_log_evt("BT up -> re-arm DUAL adv");
}

void minimed_sake_toggle_mode(void) {
  s_mode = (s_mode == MinimedSakeModeNormal) ? MinimedSakeModeDual : MinimedSakeModeNormal;
  if (s_mode == MinimedSakeModeDual) {
    minimed_sake_log_evt("mode -> DUAL");
    minimed_sake_clear_link_state();  // no pump link exists entering DUAL; drop any stale handle so
                                      // it cannot alias (and swallow) a future phone connection
    minimed_sake_cache_gateway_addr();     // so a reconnecting phone isn't mistaken for the pump
    minimed_sake_apply_sm_config(minimed_sake_pump_pairing_window());
    minimed_sake_sender_set_mode(true);       // open the loopback watchface session
    gap_le_advert_set_allow_advert_while_connected(true);  // keep advertising for the pump
    minimed_sake_pump_advert_start();         // the pump's own fast advert job
  } else {
    // NORMAL is the complete kill switch: tear the Bluetooth stack fully down and bring it back
    // up in a clean, phone-only state, so the pump link cannot interfere with firmware sideload.
    // The pump advert job, loopback session and dual-advertising all close first. Do NOT touch the
    // pump link handles here: the pump link is still up until the stack stops, and a disconnect
    // arriving in that window must still be swallowed (routing it would deref a never-created
    // GAPLEConnection). The handles are cleared on the next DUAL entry instead.
    minimed_sake_log_evt("mode -> NORMAL");
    minimed_sake_apply_sm_config(false);
    minimed_sake_pump_advert_stop();
    minimed_sake_sender_set_mode(false);
    gap_le_advert_set_allow_advert_while_connected(false);
    bt_ctl_reset_bluetooth();
  }
}

#else

MinimedSakeMode minimed_sake_get_mode(void) { return MinimedSakeModeNormal; }
void minimed_sake_toggle_mode(void) {}
void minimed_sake_bt_started(void) {}
void minimed_sake_spike_report(MinimedSakeStage stage) { (void)stage; }
void minimed_sake_log(const char *msg) { (void)msg; }
void minimed_sake_log_evt(const char *msg) { (void)msg; }
const char *minimed_sake_get_log(void) { return ""; }
bool minimed_sake_pump_pairing_window(void) { return false; }
void minimed_sake_cache_gateway_addr(void) {}
bool minimed_sake_addr_is_gateway(const uint8_t addr[6], uint8_t addr_type) {
  (void)addr;
  (void)addr_type;
  return false;
}
void minimed_sake_clear_link_state(void) {}
bool minimed_sake_pump_connected(void) { return false; }
void minimed_sake_watchdog_retoggle(void) {}
void minimed_sake_pump_advert_start(void) {}
void minimed_sake_pump_advert_stop(void) {}
void minimed_sake_pump_advert_update(void) {}

#endif
