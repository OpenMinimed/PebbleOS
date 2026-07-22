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
- **Latest: v16** (`build/sake-spike-v16-rpa-fix.pbz`) — reconnect **HW-VERIFIED 2026-07-22**:
  after a NORMAL⇄SPIKE toggle the pump reconnected by itself, re-ran the handshake, BG resumed.

## Hardware facts

- Pebble 2 Duo = board **`asterix`** (nRF52840, B&W 144x168). BLE = **NimBLE**
  (`src/bluetooth-fw/nimble/`), host + controller on the same chip.
- Pebble Time 2 = `obelix`, Round 2 = `getafix` (SiFli): same NimBLE host, different controller —
  re-verify before trusting.

## How to build

- No local toolchain; build in Docker. Image **`pebbleos-build:local`** (3.58GB) already exists =
  `ghcr.io/coredevices/pebbleos-docker:v6` + `pip install -r requirements.txt`, `docker commit`ed.
- Submodules must be checked out (needed ones already are). **Skip `third_party/hal_sifli/SiFli-SDK`**
  (huge, obelix-only). `resources/iconography` is required or the resource build dies.
- Build:
  ```sh
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -v "$PWD":/pebbleos -w /pebbleos pebbleos-build:local bash -lc '
      git config --global --add safe.directory /pebbleos
      export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:$PATH
      ./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y   # once / after Kconfig changes
      ./waf build && ./waf bundle'
  ```
- Then `cp` the newest `.pbz` (`ls -t build/*.pbz` — the name includes the git describe, don't grab
  a stale one) to `build/sake-spike-vN-<desc>.pbz` and send that.
- Crypto changes: run `tools/minimed_sake_hosttest/` (`make run`, 24/24) before reflashing.

## How to flash

- Always a BT sideload via the Pebble mobile app (no dev kit).
- Get the `.pbz` to the phone (Claude sends it in chat).
- Watch must be in **NORMAL mode** + connected to the Pebble app (SPIKE breaks the phone link).
- Pebble app → Settings → Show debug options → Devices → the watch → Firmware Update Debug →
  Sideload FW → pick the `.pbz`. Wait for install + reboot.
- Recovery if a build misbehaves: factory reset, or PRF recovery mode (separate slot; bricking
  very unlikely).

## How to drive it

- Everything is behind Kconfig `CONFIG_MINIMED_SAKE_SPIKE`. Boots in NORMAL mode (ordinary Pebble).
- "SAKE Spike" launcher app: live debug log + mode header.
  - **SELECT** = toggle NORMAL ⇄ SPIKE. **DOWN** = forget pump (back to FE82 first-pair).
  - **Back** = exit to launcher (system menus never blocked).
- SPIKE mode = advertise as Medtronic peripheral "Mobile PB", host SAKE + vendor-DIS services.

**Test procedure:**

1. SAKE Spike app → SELECT → `MODE: SPIKE (FE82)`. Phone BT **off** (single connection slot).
2. Pump: add device → "Mobile PB". (First time after v15+: pair fresh — the pump needs our IRK,
   only a new pairing distributes it.)
3. Expected log: `adv EN FE82 t3 …` → `connected` → `paired/encrypted` → `subscribed` →
   `notify rc=0x0000` → `PUMP WROTE!` → `sent reply (st1/3/5)` → `HANDSHAKE OK!` → `CGM svc` /
   `chr … h=..` / `CGM feat` → `*** BG N.N mmol/L ***` every 60s, matching the pump.
4. **Reconnect (HW-verified):** after `HANDSHAKE OK!` header shows `(FE81)`. On link loss (toggle,
   out of range) expect `adv EN FE81 t3 …` → pump reconnects **by itself** → handshake re-runs
   (fresh each connection, by design) → BG resumes.
5. Failure split: no `connected` after the FE81 advert = pump can't find/resolve us (IRK/RPA side);
   dies before `subscribed` = bond/encryption side. `adv START FAIL 0xNNNN` = advertising never
   started.
6. Back to NORMAL to restore the phone link.

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
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — spike core: log ring buffer, mode flag, stage
  reports; declares the app↔BT-layer seam (`force_readvertise`, `pump_paired`, `forget_pump`).
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

## Remaining work (usability order)

1. ✅ **Reconnect — HW-verified 2026-07-22** (v16); `Documentation/bluetooth.md` updated. Later
   maybe: persist the paired flag (RAM-only now, lost on reboot), watchdog if advertising stalls.
2. **Real watchface (next up).** Decided direction: firmware injects **local AppMessages** into the existing
   `../minimed-pebble-watchface` (built sender-agnostic; computes its own trend from BG history) —
   same watchface for phone-bridge and on-watch modes. Research needed: PebbleOS inbound AppMessage
   delivery seam. Long-term: BLE peripheral APIs in the public SDK (beyond issue #853's
   client-only ask).
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
