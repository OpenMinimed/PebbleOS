# On-Watch SAKE Spike — Chronological Dev/Test History

Step-by-step record of every spike build and hardware test. Reference archive — **not needed in
context for ongoing work**; `PROGRESS.md` has the current state. Newest at the bottom.

## 2026-07-12 — Feasibility research + Spike 1 written

- Research session: Pebble 2 Duo = board `asterix` = nRF52840 (full central+peripheral BLE, HW AES,
  1MB flash). BLE stack = NimBLE. Verdict: feasible, no showstopper.
- Two scariest unknowns resolved positive: (1) peripheral/GATT-server capability exists in
  firmware (used for the phone link, just not exposed to apps); (2) SAKE is purely symmetric AES —
  no ECDH (earlier BouncyCastle/ECDH guess was wrong) — a weekend, not a blocker.
- Topology confirmed from OpenMinimed docs: pump = central + GATT client for the handshake; watch
  must host SAKE Port service (SIG svc 0xFE82, vendor char) AND later be GATT client to the pump's
  CGM/IDD services on the same link.
- Spike 1 firmware written (advertise + SAKE Port + wake-up, no crypto), builds green behind
  `CONFIG_MINIMED_SAKE_SPIKE`.
- Build environment created: `pebbleos-build:local` Docker image (coredevices v6 + pip deps
  committed); submodules initialized shallow, SiFli-SDK skipped.

## ~2026-07-19 — Spike 1 hardware test 1 (partial success)

- ✅ Pump lists "Mobile PB" and connects into the watch — inverted topology proven on HW.
- ❌ Drops before `paired/encrypted`: pairing negotiation fails.
- Gotcha found: bonded phone auto-reconnects and steals the single BLE slot
  (`BLE_MAX_CONNECTIONS=1`) → phone BT must be off for pump tests.
- Root cause found by reading the working bridge (no dev kit needed), 3 mismatches:
  1. Pump pairs **legacy Just Works**, not LESC. PebbleOS had `SC_ONLY=1/LEGACY=0` → rejected.
     Fix: SC_ONLY 0, LEGACY 1, IO NO_INPUT_OUTPUT, MITM 0.
  2. Pump refuses to pair without the **vendor `0x0900` DIS** (std 0x180A insufficient). Added.
  3. Name must be in **manufacturer data** (company 0x01F9), not the GAP name field. Fixed.

## v2 — pairing works, pump won't write

- HW: connect → `paired/encrypted` → discovery (~10-15s) → `subscribed`, wake-up sent.
- ❌ Pump never writes its first handshake frame; ~90s spin → "device not found".
- Suspect: wake-up notification timing (sent synchronously in the subscribe callback).

## v3 — on-watch debug console

- Dev kit unobtainable (out of stock, requires opening a watch) → built the persistent on-watch
  log instead (ring buffer + modal viewer; later the launcher app). This is the debugger for
  everything since. Logs added: wake-up notify rc, pump write bytes, disconnect reason.

## v4 — interactive NORMAL⇄SPIKE toggle

- Problem solved: spike FW made the phone bond flaky → factory reset before every sideload.
- "SAKE Spike" launcher app (id -200): SELECT toggles mode; boots NORMAL (ordinary Pebble, sideload
  works); Back never trapped.
- HW: toggle round-trip works. ❌ But pump can't find the watch in SPIKE mode (v3 could).

## v5–v6 — advertising interval bug

- v5 diagnostics: `adv EN 1022-1022ms` — Pebble's reconnection job re-advertises at ~1s; the pump
  ignores adverts slower than ~150ms.
- v6 fix: clamp 100–140ms + re-assert Medtronic payload inside `advertising_enable` (single choke
  point the connection manager can't bypass).
- v6 HW: pump finds/connects/pairs/subscribes, wake-up rc=0 — but `disc 0x13`, pump never writes.
  Confirmed suspicion: wake-up must not be sent synchronously in the subscribe callback.

## v7–v9 — deferred wake-up; 🎉 Spike 1 proven

- v7: wake-up deferred ~120ms via `ble_npl_callout`.
- v8: + repeat-pairing auto-recovery re-enabled (the SM change had disabled it → phone bond
  connect/terminate(0x13) loops couldn't self-heal).
- v8 HW: **`PUMP WROTE! wrote 20:00 00 00 00`** — pump sent its first handshake frame. Spike 1
  fully proven. The following `disc 0x13` was expected: no crypto to reply with yet.
- v9: `force_readvertise` also works with no active link → toggle order no longer matters.
- Toggle-order gotcha (pre-v9): had to toggle SPIKE while the phone was still connected.

## 2026-07-21 — Spike 2: SAKE crypto in C

- All reference material found on disk (no JavaSake fetch needed): `pysake` in
  `xdrip-pebble-archive/PythonPumpConnector/PythonSake/` + public test vectors in `constants.py`
  (`KEYDB_PUMP_EXTRACTED` + the real `780g_pairing_with_mobile` capture incl. RNG values).
- C port (`minimed_sake_aes.c` + `minimed_sake_crypto.c`), self-contained, byte-verified on the
  laptop: `tools/minimed_sake_hosttest/` 24/24 — AES vs FIPS-197, CMAC vs RFC 4493, captured trace
  msg0/2/4 byte-identical, session key matches pysake, SeqCrypt interop incl. tamper rejection.
- Documentation/sake.md 5_c section filled in (was TODO).

## v10 — 🎉 handshake proven on HW (2026-07-21)

- State machine wired into `minimed_sake_service.c` (embedded key DB, RNG = `ble_hs_hci_rand`,
  re-init per subscription, replies deferred 30ms).
- HW: pump wrote non-zero msg1/3/5, watch replied st1/3/5, **`HANDSHAKE OK!`** — authenticated
  encrypted session live on the watch, against the real pump.

## v11–v12 — 🎉 first live BG (2026-07-21)

- v11: post-handshake GATT-client read (`minimed_sake_read.c`): discover CGM svc 0x181F, chars
  2AA7/2AA8/2A52, read CGM Feature (plaintext; E2E-CRC bit set). HW-proven.
- v12: subscribe Measurement (notify) + RACP (indicate), CCCD=val+1; write RACP report-last-record
  `01 06` (plaintext — only Measurement records are SAKE-encrypted); decrypt → reassemble →
  SFLOAT at offset 2. HW: **`*** BG 128 mg/dL ***`** = 7.1 mmol/L = exactly the pump's display.
  End-to-end premise validated.

## v13–v14 — continuous BG + rounding fix (2026-07-21)

- v13: 60s RACP polling + mmol/L display (integer math). HW: tracks the pump (6.1→5.8→5.6)…
- …but read 0.1 high at ~100 mg/dL (watch 5.6, pump 5.5). Root cause: conversion constant. The
  bridge's GlucoseFormat deliberately uses **18.0182** mg/dL per mmol/L (not textbook 18.0156) to
  match the pump's own rounding; 100/18.0156 and 100/18.0182 straddle the 5.55 boundary.
- v14 with 18.0182: HW-confirmed exact match. (First guess — poll lag vs push — was wrong; Morten
  correctly pushed back.)

## v15–v16 — reconnect (2026-07-22)

- v15: FE81 (reconnect) advertising once handshake completed; RPA in SPIKE mode (pump only
  reconnects to RPAs per bluetooth.md); `BLE_SM_OUR_KEY_DIST 1→3` so the pump gets our IRK +
  identity at pairing (IRK = persisted device-unique root key → reboot-stable); RAM `s_pump_paired`
  flag; DOWN = forget pump.
- v15 HW: **DOA — pump couldn't find the watch at all.** Root cause: hardcoded
  `BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT`, but the watch has only a *static-random* identity (no public
  address) → `ble_gap_adv_start` failed `BLE_HS_ENOADDR` → no advertising, and the error was
  invisible on-watch (PBL_LOG only).
- v16: `ble_hs_id_infer_auto(1,…)` picks the correct RPA flavor (t3 = RPA-random); any adv-start
  failure now logs `adv START FAIL 0xNNNN` on-watch.
- v16 HW: 🎉 **reconnect works.** NORMAL⇄SPIKE toggle → pump reconnected by itself, re-ran the full
  handshake, correct BG resumed. Confirms the pump's RPA + distributed-IRK reconnect requirements
  (noted in `Documentation/bluetooth.md`) and that the handshake re-runs fresh per connection.
