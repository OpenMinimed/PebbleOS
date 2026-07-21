/* SPDX-FileCopyrightText: 2026 Morten Fyhn Amundsen */
/* SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include <stdint.h>

//! Core state + on-watch log for the MiniMed SAKE spike. The BLE work runs in the firmware
//! regardless of any UI; the "SAKE Spike" launcher app is just a viewer + a mode toggle, so the
//! standard system menus are never blocked.

typedef enum {
  // Behave as a normal Pebble (advertise as Pebble, connect to the phone). Default on boot.
  MinimedSakeModeNormal = 0,
  // Advertise as a Medtronic pump peripheral ("Mobile PB") for pump testing.
  MinimedSakeModeSpike,
} MinimedSakeMode;

typedef enum {
  MinimedSakeStageAdvertising = 0,
  MinimedSakeStageConnected,
  MinimedSakeStageEncrypted,
  MinimedSakeStageSubscribed,
  MinimedSakeStageWrote,
  MinimedSakeStageDisconnected,
} MinimedSakeStage;

//! Current BLE mode. Read by the advertising hijack (BT task) and the app UI (app task).
MinimedSakeMode minimed_sake_get_mode(void);

//! Flip Normal<->Spike, log it, and force a re-advertise so the new mode takes effect promptly.
//! Called from the app's SELECT handler.
void minimed_sake_toggle_mode(void);

//! Append a stage line to the on-watch log (+ a vibe on key stages). Safe from any task.
void minimed_sake_spike_report(MinimedSakeStage stage);

//! Append an arbitrary line to the on-watch log (rc codes, byte dumps, disconnect reasons).
//! Safe from any task, including the BT host task.
void minimed_sake_log(const char *msg);

//! Snapshot of the joined log text for the app to render. Points at a static buffer.
const char *minimed_sake_get_log(void);

//! Implemented in the BT layer (advert.c): drop the active pump/phone link if any, so advertising
//! restarts under the current mode. Declared here so the core toggle can call it.
void minimed_sake_force_readvertise(void);
