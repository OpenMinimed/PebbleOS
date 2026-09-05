/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

//! Core state + on-watch log for the MiniMed SAKE spike. The BLE work runs in the firmware
//! regardless of any UI; the "SAKE Spike" launcher app is just a viewer + a mode toggle, so the
//! standard system menus are never blocked.

typedef enum {
  // Behave as a normal Pebble (advertise as Pebble, connect to the phone). Default on boot.
  MinimedSakeModeNormal = 0,
  // Hold the phone link AND the MiniMed pump link at the same time.
  MinimedSakeModeDual,
} MinimedSakeMode;

typedef enum {
  MinimedSakeStageAdvertising = 0,
  MinimedSakeStageConnected,
  MinimedSakeStageEncrypted,
  MinimedSakeStageSubscribed,
  MinimedSakeStageWrote,
  MinimedSakeStageHandshakeComplete,
  MinimedSakeStageDisconnected,
} MinimedSakeStage;

//! Current BLE mode. Read by the advertising hijack (BT task) and the app UI (app task).
MinimedSakeMode minimed_sake_get_mode(void);

//! Flip Normal<->Dual, log it, and re-arm the relevant advertising. Called from the app's SELECT
//! handler. Entering Dual opens the pump's own advert job and the loopback watchface session.
//! Entering Normal performs a full Bluetooth stack restart (bt_ctl_reset_bluetooth) so the watch
//! returns to a clean, phone-only state -- the kill switch for sideloading firmware.
void minimed_sake_toggle_mode(void);

//! Called once the Bluetooth stack is fully up (after gap_le_init). Re-arms the DUAL advert state;
//! it cannot be done from the driver's own init, because gap_le_advert_init() runs after that and
//! resets the advert scheduler.
void minimed_sake_bt_started(void);

//! Append a stage line to the on-watch log (+ a vibe on key stages). Safe from any task.
void minimed_sake_spike_report(MinimedSakeStage stage);

//! Append an arbitrary line to the on-watch log (rc codes, byte dumps, disconnect reasons).
//! Safe from any task, including the BT host task.
void minimed_sake_log(const char *msg);

//! Like minimed_sake_log, but also writes the line to the durable flash log. Connectivity events
//! only -- never anything on a per-poll or per-advert-rotation path.
void minimed_sake_log_evt(const char *msg);

//! Snapshot of the joined log text for the app to render. Points at a static buffer.
const char *minimed_sake_get_log(void);

//! Implemented in the BT layer (advert.c): drop the active pump/phone link if any, so advertising
//! restarts under the current mode. Declared here so the core toggle can call it.
void minimed_sake_force_readvertise(void);

//! Implemented in the BT layer (minimed_sake_service.c): whether a SAKE handshake has completed,
//! i.e. the pump is bonded and the spike advertises FE81 (reconnect) instead of FE82 (first-pair).
bool minimed_sake_pump_paired(void);

//! Implemented in the BT layer (minimed_sake_service.c): clear the pump-paired state and go back
//! to FE82 (first-pair) advertising -- for when the "Mobile PB" device was removed on the pump.
//! Called from the spike app's DOWN handler.
void minimed_sake_forget_pump(void);

//! Implemented in the BT layer (minimed_sake_sender.c): open/close the loopback CommSession that
//! feeds the watchface local AppMessages. Open only in DUAL mode (in NORMAL mode it would
//! compete with the real phone session). Called from the mode toggle.
void minimed_sake_sender_set_mode(bool open);

//! Implemented in the BT layer (minimed_sake_service.c): switch the runtime Security Manager
//! config between the phone's stock strict LESC and the pump's legacy Just Works. `pump_window`
//! true opens the pump-pairing window (legacy JW), false closes it. Called from the mode toggle
//! and the pump handshake.
void minimed_sake_apply_sm_config(bool pump_window);

//! True while in DUAL mode with the pump not yet bonded: the pump's identity is unknown until its
//! first handshake, so the driver presumes any incoming connection is the pump and pairing uses
//! legacy Just Works.
bool minimed_sake_pump_pairing_window(void);

//! Cache the phone (gateway) identity so a reconnecting phone during the pump-pairing window is
//! not misclassified as the pump. Safe only on the app/KernelMain task (it reads flash). Called
//! when the pairing window opens.
void minimed_sake_cache_gateway_addr(void);

//! True if the peer (raw identity address + type) is the cached phone (gateway) identity.
bool minimed_sake_addr_is_gateway(const uint8_t addr[6], uint8_t addr_type);

//! Implemented in the BT layer (advert.c): drop any recorded pump/rejected link handles. Called on
//! DUAL entry, where the pump link is guaranteed dead, so a stale handle from before a Bluetooth
//! stack restart cannot alias (and swallow) a later phone connection.
void minimed_sake_clear_link_state(void);

//! Implemented in the BT layer (advert.c): true while a pump link is tracked as connected. Used by
//! the pump-liveness watchdog.
bool minimed_sake_pump_connected(void);

//! Implemented in the popups layer (minimed_sake_spike_ui.c): the pump-liveness watchdog's
//! recovery. Keeps the mode DUAL and runs the NORMAL kill-switch stack restart to free a phantom
//! pump connection slot and re-arm the pump advert; the SAKE service re-arms DUAL after the
//! restart completes (see minimed_sake_service_init).
void minimed_sake_watchdog_retoggle(void);

//! Implemented in the BT layer (minimed_sake_service.c): start/stop/update the pump's own
//! advertising job (Medtronic FE82/FE81 payload at ~100ms, independent of the phone's
//! Reconnection job). Update re-schedules with the current paired flag (FE82<->FE81).
void minimed_sake_pump_advert_start(void);
void minimed_sake_pump_advert_stop(void);
void minimed_sake_pump_advert_update(void);
