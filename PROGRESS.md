# On-Watch MiniMed → Pebble SAKE Spike — Progress & Notes

Run the MiniMed pipeline on the Pebble 2 Duo directly, no phone. Spinoff of the working
phone-bridge project (`../minimed-pebble-bridge`). This file = current state + how-to.
`HISTORY.md` = chronological dev/test log (reference archive; no need to load into context).

## Context & scope (read first)

- Personal DIY-diabetes project: Morten (type 1) reads **his own** glucose from **his own**
  Medtronic 780G, offline, onto his own watch. No cloud, no third party.
- **Read-only by design, and always will be.** Only read/report requests (glucose, IOB, status,
  history). No path to any command/settings characteristic; the only control-point write is the
  standard RACP "report stored records" query. Cannot dose, calibrate, or change settings.
- Built on public open-source RE (OpenMinimed, GPL-3.0) with published model-static keys — same as
  the working phone bridge. Same category as OpenAPS/Loop/xDrip (decade of precedent).
- Pump + CGM remain the authoritative devices; this is a supplementary display.

## Status

- **End-to-end PROVEN on real HW (2026-07-21):** advertise as "Mobile PB" → pump connects → SAKE
  handshake → GATT-client CGM read → decrypt → continuous auto-updating BG in mmol/L, matching the
  pump's display exactly. Feasibility fully settled; the rest is productization.
- Reconnect **HW-VERIFIED 2026-07-22** (v16): after a NORMAL⇄SPIKE toggle the pump reconnected by
  itself, re-ran the handshake, BG resumed.
- **Latest: v17** (`build/sake-spike-v17-watchface-sender.pbz`, 2026-07-22, **awaiting HW test**):
  pairing persisted (reboot/reflash → straight back to FE81) + the **watchface local-sender** —
  BG is injected as normal AppMessages into the unmodified `minimed-pebble-watchface`.

## Hardware facts

- Pebble 2 Duo = board **`asterix`** (nRF52840, B&W 144x168). BLE = **NimBLE**
  (`src/bluetooth-fw/nimble/`), host + controller on the same chip.
- Pebble Time 2 = `obelix`, Round 2 = `getafix` (SiFli): same NimBLE host, different controller —
  re-verify before trusting.

## Build / flash / test

**→ See `TESTING.md`** for the full loop (`./spike-build.sh <desc>` builds+versions+adb-pushes;
BT sideload steps; pump + watchface test procedures; the FE81/FE82 reconciliation fix; log
vocabulary). Quick facts kept here:

- No local toolchain; build in Docker image **`pebbleos-build:local`** (recreate per "How to build"
  below if gone). `spike-build.sh` wraps the whole Docker invocation.
- Everything is behind Kconfig `CONFIG_MINIMED_SAKE_SPIKE`; boots NORMAL (ordinary Pebble).
  "SAKE Spike" app: SELECT = NORMAL⇄SPIKE, DOWN = forget pump, Back = exit.
- Crypto changes: run `tools/minimed_sake_hosttest/` (`make run`, 24/24) before reflashing.

### How to build (recreate the image if `pebbleos-build:local` is gone)

`ghcr.io/coredevices/pebbleos-docker:v6` + `pip install -r requirements.txt`, then `docker commit`.
Submodules must be checked out (skip the huge `third_party/hal_sifli/SiFli-SDK`, obelix-only;
`resources/iconography` is required or the resource build dies).

## Version log (terse; full chronological history in `HISTORY.md`)

- v10 handshake, v11 GATT-client discovery, v12 first live BG, v13 60s polling — all HW-proven
  2026-07-21.
- v14: mmol/L fix — divide by **18.0182** (not 18.0156) to match Medtronic's own rounding (same
  constant as the bridge's GlucoseFormat). HW-confirmed.
- v15: reconnect (FE81 + RPA + IRK distribution + forget-pump). **DOA on HW**: hardcoded
  `BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT`, but the watch has only a static-random identity → adv_start
  failed `ENOADDR` → no advertising, invisibly.
- v16: fix — `ble_hs_id_infer_auto(1,…)` picks the right RPA type (t3 = RPA-random); adv failures
  now log `adv START FAIL` on-watch. **HW-proven 2026-07-22: full self-reconnect after toggle.**
- v17 (2026-07-22): pump-paired flag persisted (settings file `minimedsake`; flash write deferred
  to KernelMain; boot log `paired (persisted): FE81`) + watchface local-sender
  (`minimed_sake_sender.c`): loopback CommSession in SPIKE mode only, BG pushed as AppMessage
  (keys 10+11) on every reading; ACKs + re-pushes on the watchface's ready ping. Log vocabulary:
  `wf sender up`, `wf ready ping`, `wf dict fail`. HW note: going v16→v17 needs a state resync
  (see the FE81/FE82 reconciliation gotcha below) — expected, one-time.
- v20 (2026-07-22, **HW-VERIFIED first-pair**): **first-pair address fix.** Advertise address type
  now follows pairing state, like FE82/FE81: first-pair (FE82) = **plain** static-random identity
  (`infer_auto(0)`, logs `t1`); reconnect (FE81) = **RPA** (`infer_auto(1)`, `t3`). v15–v19
  wrongly advertised an RPA for first-pair too, which the pump often could not discover (pure
  discovery failure: nothing after `adv EN FE82`, pump "device not found"). v10–v14 used a plain
  address and paired reliably — this restores that. Also **reverted v19's connection event** (it
  was an unverified discovery confound; the watchface "not connected" banner is a separate problem
  to solve *after* pairing, by emitting the connection event only once the pump link is up).
  Confirmed on HW: `adv EN FE82 t1` → pump found + paired; **reconnect (`adv EN FE81 t3`) also
  works** (1–2 min latency — the pump scans for reconnects slowly to save battery, expected); and
  the **real watchface shows live BG** (saw "6.2" + `wf ready ping`) with the v19 connection-event
  hack removed — and the watchface is **clean, no "not connected" anywhere**. So the fake
  connection event is NOT needed and stays dropped for good.
- v19 (SUPERSEDED, reverted in v20): watchface "not connected" fix — the sender emitted
  `PEBBLE_BT_CONNECTION_EVENT` on session open/close (comm_session_open already fires the comm
  session event that feeds the app connection service via debounce, but the system-wide
  disconnected banner shown on every watchface is driven by the BT connection event, which only
  the real transports emitted). Bundles v18's phone-bond fix too. **Future (user's call):** dual
  pump+phone connection (`BLE_MAX_CONNECTIONS` 2) would give a real phone link — resolves the
  banner for real and removes the NORMAL-toggle-to-sideload step.
- v18 (2026-07-22, awaiting HW): **phone-bond fix** — runtime SM reconfig
  (`minimed_sake_apply_sm_config`). NimBLE reads `ble_hs_cfg.sm_*` live at pairing time, so NORMAL
  mode now uses stock strict LESC (io_cap DISPLAY_YESNO, MITM, LTK-only) and SPIKE flips to legacy
  Just Works + IRK dist for the pump. syscfg defaults restored to stock (only SC_ONLY 0 / LEGACY 1
  kept as the compile gates the pump needs). Should stop the watch↔phone bond breaking every
  cycle. Expect ONE more phone re-pair when first adopting v18 (the current phone bond was formed
  under the old weak config), then stable.

## Code map (branch `spike/minimed-sake`; Spikes 1+2 committed, v15/v16 reconnect uncommitted, nothing pushed)

New files (all spike-only via wscript/ifdef):

- `minimed_sake_aes.{c,h}` + `minimed_sake_crypto.{c,h}` — SAKE server handshake + session cipher
  in C (AES-128/CMAC/CTR, SeqCrypt, key-DB parse, 6-msg state machine). Self-contained; byte-verified
  vs the captured 780G trace by `tools/minimed_sake_hosttest/` (same sources, 24/24).
- `minimed_sake_service.c` — SAKE Port GATT server (svc SIG `0xFE82`, char **vendor**
  `0000fe82-…-009132591325`) + required vendor-`0x0900` DIS (9 placeholder chars) + advert payload
  builder (FE82/FE81 by `s_pump_paired`) + handshake driver (embedded `KEYDB_PUMP_EXTRACTED`, RNG =
  `ble_hs_hci_rand`, re-init per subscription, deferred notify 120ms wake-up / 30ms replies) +
  `minimed_sake_decrypt` + `pump_paired`/`forget_pump`.
- `minimed_sake_read.c` — post-handshake CGM read, watch as GATT client: discover `0x181F`, chars
  `2AA7/2AA8/2A52`; read Feature (plaintext); subscribe Measurement (notify) + RACP (indicate),
  CCCD = val_handle+1 (assumed layout; add dsc discovery if it ever breaks); poll RACP `01 06`
  every 60s. Measurement is SAKE-encrypted → decrypt → reassemble by size byte → SFLOAT at offset
  2 → mmol/L. **RACP itself is plaintext; only Measurement records are encrypted.**
- `minimed_sake_sender.c` — the **watchface local-sender**: loopback CommSession (QEMU-transport
  pattern) opened in SPIKE mode only (would compete with the real phone session in NORMAL);
  injects BG as `[PP hdr 0x0030][CMD_PUSH][watchface UUID][dict: key 10 ts, key 11 "N.N"]` via
  `comm_session_receive_router_write` on KernelMain under `bt_lock`. `send_next` drains the
  watchface's outbox: its ready ping (CMD_PUSH) → ACK + immediate BG push; its ACKs of our pushes
  are swallowed (loop guard). Watchface UUID + protocol:
  `minimed-pebble-watchface/docs/PEBBLE_GLUCOSE_PROTOCOL.md`.
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — spike core: log ring buffer, mode flag, stage
  reports; declares the app↔BT-layer seam (`force_readvertise`, `pump_paired`, `forget_pump`,
  `sender_set_mode`).
- `src/fw/apps/system/minimed_sake_app.{c,h}` — launcher app (viewer + buttons).

Modified:

- `Kconfig` (+`MINIMED_SAKE_SPIKE`), `wscript_build` (spike sources), `init.c` (register service),
  `system_app_registry_list.json` (app id `-200`).
- `advert.c` — SPIKE-mode advert hijack (Medtronic payload; mfr data `0x01F9` carries the name);
  interval clamp 100–140ms (pump ignores >150ms; Pebble reconnect job uses ~1s); RPA own-addr type
  via `infer_auto(1,…)`; `force_readvertise()`; NOTIFY_RX routes pump notifications to the read
  layer first; disconnect stops polling; repeat-pairing auto-recovery re-enabled for the spike.
- `third_party/nimble/port/include/nrf52/syscfg/syscfg.h` — SM config for the pump's **legacy Just
  Works** (SC_ONLY 0, LEGACY 1, IO NO_INPUT_OUTPUT, MITM 0) + `BLE_SM_OUR_KEY_DIST 1→3` (distribute
  IRK + identity so the pump can resolve our RPAs).

## Protocol facts confirmed on HW (mirror into `../Documentation/`!)

- SAKE is purely symmetric AES (ECB key-derivation, CTR payload, CMAC tags). No ECDH.
- UUID bases: SIG `-0000-1000-8000-00805f9b34fb`; Medtronic vendor `-0000-1000-0000-009132591325`
  (variant `0000`, not `8000`). SAKE service = SIG; SAKE char = vendor (mixups → pump finds the
  service but not the char).
- Advertising: pump scans svc class `0xFE82` first-pair / `0xFE81` reconnect; name in mfr data
  (company `0x01F9`), not GAP name; **ignores adverts slower than ~150ms**; only *reconnects* to an
  RPA, and needs our IRK + identity distributed at pairing to resolve it (HW-confirmed 2026-07-22;
  noted in `Documentation/bluetooth.md`). Handshake re-runs fresh on every reconnect — no resume.
- Pairing: legacy Just Works, NoInputNoOutput (pump requests MITM but has no SC → falls back).
- Pump refuses to pair without the vendor `0x0900` DIS (standard `0x180A` is not enough).
- SAKE init: pump subscribes → watch notifies 20 zero bytes (**deferred, never synchronously in
  the subscribe callback** or the pump misses it) → pump writes 20 zeros → 6-msg handshake. Keys
  per device-type pair; DB format in `Documentation/key_databases.md`.

## Gotchas

- Single BLE connection (`BLE_MAX_CONNECTIONS 1`): phone or pump, never both. Phone BT off to test.
- Phone bond can loop connect/terminate(0x13) after the SM changes; repeat-pairing recovery usually
  self-heals; else forget + re-pair on the phone (NORMAL mode).
- clangd floods spike files with false errors (missing NimBLE include paths). Trust `./waf build`.
- Don't `docker system prune` without asking (~100GB of other images on this machine).
- **FE81/FE82 pairing-state reconciliation.** The watch's paired flag and the pump's bond can
  disagree, and neither auto-corrects: if the watch advertises FE81 (log `adv EN FE81 …`) but the
  pump can't find it, the pump's side is unpaired → press **DOWN (forget pump)** on the watch to
  drop to FE82, then add "Mobile PB" on the pump. Conversely, a watch on FE82 that a bonded pump
  ignores means re-pair on the pump. Rule of thumb: **pump can't see watch → make both do
  first-pair (DOWN on watch + remove/add on pump).** A smarter auto-fallback (try FE82 if FE81
  draws no connection for N s) is a possible future improvement.

## Remaining work (usability order)

1. ✅ **Reconnect — HW-verified 2026-07-22** (v16); `Documentation/bluetooth.md` updated. Paired
   flag persisted in v17. Later maybe: watchdog if advertising ever stalls.
2. **Real watchface — WORKING (v20, HW-verified): live BG on the actual watchface, screen clean
   (no "not connected").** No connection-event hack needed. Next display work is additive keys:
   IOB/status/graph. Design: firmware injects **local AppMessages** into the existing
   `../minimed-pebble-watchface`, unmodified — same watchface for phone-bridge and on-watch modes.
   Seam researched (2026-07-22), plan = **copy the QEMU transport pattern**
   (`src/bluetooth-fw/qemu/qemu_transport.c`, the existing phone-less precedent):
   - Once: `comm_session_open()` a synthetic loopback transport (`TransportDestinationHybrid`,
     `send_next` discards — swallows the app's ACKs) under `bt_lock`.
   - Per BG: build `[PebbleProtocolHeader len,0x0030 BE][CMD_PUSH,txn][watchface UUID][dict]`
     (dict via `dict_serialize_tuplets_to_buffer`; construction example in
     `launcher_app_message.c`) → `comm_session_receive_router_write(session, …)` under `bt_lock`,
     one kernel task only (router keeps per-session reassembly state).
   - Delivery hops to the app task via app_inbox_service; the watchface receives a 100% normal
     AppMessage. **UUID must equal the running app's** — only inject when our watchface is
     foreground (else clean NACK/drop, no crash). Workers can't receive AppMessages; foreground
     only. Bonus: the watchface's *outbound* messages (e.g. a launch-time sync request) arrive in
     our `send_next` — parse to trigger an immediate BG push on watchface launch.
   - Speak the exact key set the bridge sends (see bridge GlucoseFormat / watchface `main.c`).
   Long-term: BLE peripheral APIs in the public SDK (beyond issue #853's client-only ask).
3. **IOB + pump status.** Port `IddStatusReader`; IDD SRCP/SOCP **requests are SAKE-encrypted** →
   first real use of `sake_encrypt_for_pump` (host-verified, unexercised on HW).
   Ref: bridge `.../ble/read/`, `Documentation/idd-service.md`.
4. **History/graph backfill.** Multi-record RACP over IDD History, 184-MTU chunked streaming
   (`Documentation/gatt-streaming.md`). Ref: bridge `HistoryReader`/`MedtronicHistoryParser`.
5. **Dual pump+phone** — `BLE_MAX_CONNECTIONS` 2 + two-peer handling (~doubles radio duty).
6. **Battery measurement** — connection-interval keep-alive dominates (same for poll vs push);
   levers: connection interval (pump-dictated), slave latency, persistent-vs-per-update link.
   Unmeasured; could be the limiting factor for dual connection.
7. **Phone-bond papercut** (re-pair dance between test cycles) — untriaged, non-trivial.

## References

- `../Documentation/` — protocol source of truth. **Update eagerly** with confirmed facts.
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` — the working Kotlin impl to
  port (peripheral, SAKE, reads, history parser) + tests.
- `../xdrip-pebble-archive/PythonPumpConnector/PythonSake/pysake/` — readable SAKE reference +
  public test vectors (`constants.py`).
- Do NOT comment on GitHub/upstream — Morten does that himself (plans to update PebbleOS #853).
