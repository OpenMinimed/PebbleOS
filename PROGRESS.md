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
- **Latest: v30** (`build/sake-spike-v30-iob-diagnostics.pbz`, **IOB HW-VERIFIED 2026-07-24**):
  on-watch **IOB now works end-to-end on real HW** — reads from the pump (IDD SRCP), forwards to
  the watchface (key 14); IOB **and** BG both show on the watchface. This is the first confirmed
  HW use of `encrypt-for-pump` (the watch encrypting a request TO the pump). v30 also bundles the
  passive advert diagnostics (byte-level advert dump + PUMP/phone label) for the still-open
  pump-in-NORMAL question — capture-when-convenient (film a SPIKE→NORMAL toggle; TESTING.md).
  Host test 32/32; adversarial review caught + fixed one latent stack-overflow (capacity-checked
  `minimed_sake_decrypt`). Watchface crash fixed separately (watchface repo master `2fb919c`).
- v27 (`build/sake-spike-v27-advert-scheduler-refresh.pbz`, 2026-07-22): **rearchitected the
  advert-mode switch** to kill the stale-payload bug class at its root (replaces v25/v26). NOTE:
  HW showed the pump STILL handshakes in NORMAL after v27 — the advert/pump-in-NORMAL bug is
  UNRESOLVED; v29's diagnostics are what will finally pin it (film a SPIKE→NORMAL toggle; see
  version log + the advert saga in HISTORY).

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

- v30 (2026-07-23; **IOB HW-VERIFIED 2026-07-24** — IOB + BG both live on the watchface, IOB value
  correct; supersedes the deleted v29 by adding the buffer-overflow fix below): **on-watch IOB +
  advert diagnostics.** Adversarial review addition: `minimed_sake_decrypt`
  now takes an `out_cap` and refuses if the `n-3` plaintext would overflow the caller's buffer —
  closes a latent stack overflow on BOTH the CGM and new SRCP decrypt paths (the 256-byte MTU means
  the transport doesn't cap frames at 20, so a >27-byte pump frame would have overrun `plain[24]`).
  - **IOB (feature):** reads insulin-on-board from the pump's IDD service and forwards it to the
    watchface. New pure module `minimed_iob.{c,h}` (medfloat32 decode + SRCP 0x03FC parse,
    host-tested against OpenMinimed vectors incl. the live-confirmed 1.4 IU frame). `minimed_sake_
    read.c` discovers IDD service `0x100` + SRCP char `0x105` (128-bit vendor UUIDs) after the CGM
    setup, then on each 60 s CGM poll's RACP-success it chains (via a ~200 ms `s_iob_co` callout, to
    respect NimBLE's one-outstanding-gattc-op rule) a SAKE-encrypted SRCP "get IOB" request
    (`0x03F3`, no E2E on the 780G), decrypts the `0x03FC` response, parses medfloat32 → milliunits,
    logs `*** IOB N.N U ***`, and forwards via new `minimed_sake_sender_send_iob` (watchface key 14).
    First-ever HW use of encrypt-for-pump (new `minimed_sake_encrypt` wrapper in service.c). **BG is
    never blocked by IOB:** every IDD-discovery failure path falls back to `prv_start_polling` with
    `s_h_srcp==0` (logs `polling BG only`), so a missing/rejected IDD service just means no IOB.
  - **v29 diagnostics (passive, log-only):** `advN <7 hex>` / `advS <7 hex>` dump the ACTUAL advert
    bytes pushed (read b[5]b[6]: 82fe/81fe = Medtronic FE82/FE81 live; else Pebble) instead of
    inferring; `conn PUMP/phone m=S/N ..` labels each connection by the pump identity captured at
    handshake DONE (RAM-only `s_pump_id_addr`). These finally answer "does the pump reconnect in
    NORMAL by address (advert irrelevant) or is the refresh not landing?" — **film a SPIKE→NORMAL
    toggle** (8-line ring scrolls). Graph still DEFERRED (its own later flash; watchface graph side
    already done).

- v27 (2026-07-22, awaiting HW): **advert-mode switch rearchitected — root fix for the whole
  "pump connects/handshakes in NORMAL + phone error-pairing" family.** Root cause (confirmed by
  reading `gap_le_advert.c`): SPIKE doesn't own an advert; it *piggybacks* the Pebble Reconnection
  job and repaints its payload to Medtronic via the `set_advertising_data` hijack. The scheduler
  caches "what data did I last push" by POINTER (`s_current_ad_data`) and skips re-pushing on an
  unchanged pointer — blind to our hijack — so a mode toggle left the previous mode's payload live
  (stale FE81 in NORMAL → pump connects + runs SAKE in NORMAL, and racing the single slot is what
  produced the phone "error pairing"). Fix: new `gap_le_advert_force_data_refresh()` (models the
  existing `bt_driver_handle_host_resynced` — nulls `s_current_ad_data` + forces a re-air), called
  from `minimed_sake_force_readvertise` on every toggle/forget. Now the scheduler always re-pushes
  through its normal path, so `set_advertising_data` runs and sets the right payload for the
  current mode — deterministic, not sampled. `set_advertising_data` is now the single source of
  truth for the payload (SPIKE→Medtronic hijack, NORMAL→Pebble passthrough); `advertising_enable`
  keeps only interval+address-type ("how", not "what"). Removed the v25 Pebble-cache and v26
  manual stop/enable dance. Likely fixes the phone-bond papercut too (it was downstream of the
  pump stealing the NORMAL slot), but VERIFY — see TESTING.md "decisive test".
- v26 (2026-07-22, superseded by v27): **force advert refresh on the mode toggle.** v25 fixed the payload
  in `advertising_enable`, but HW showed the pump STILL ran SAKE in NORMAL sometimes — because
  when advertising is already on air at toggle time (pump mid connect/disconnect loop),
  gap_le_advert returns early and never calls `advertising_enable`, so the re-assert never fires
  and the stale FE81 payload stays live. Fix: `minimed_sake_force_readvertise` (NORMAL, nothing
  connected) now does `ble_gap_adv_stop()` + `advertising_enable()` to push the correct payload
  immediately, instead of doing nothing and hoping. v25's advertising_enable re-assert stays (it
  covers the reconnect-manager-driven restart after the pump link is terminated). NOTE: phone
  "error pairing" was likely a *downstream* symptom of the pump stealing the single slot in
  NORMAL — if v26 keeps the pump out of NORMAL, the phone bond may just work; re-check before
  assuming a separate phone-bond bug.
- v25 (2026-07-22, superseded by v26 — necessary but insufficient): **NORMAL-mode advert-payload refresh.** HW clue that cracked it: the full pump handshake (`HANDSHAKE OK`, `polling BG`) showed
  up *after switching to NORMAL* — the pump was connecting and running SAKE in NORMAL. Cause: the
  SPIKE hijack overwrites the controller's advert data with the Medtronic FE81 payload, but the
  upper advertising layer (`gap_le_advert.c` ~L340) only re-sends advert data when its *job
  pointer* changes — so a SPIKE→NORMAL toggle on the same job left the stale FE81 payload live.
  The pump then found FE81 in NORMAL, connected, ran SAKE, and (single slot) raced/blocked the
  phone → the "error pairing" we'd been chasing. Fix (advert.c): cache the real Pebble payload in
  `set_advertising_data` and re-assert it on every NORMAL `advertising_enable`, symmetric to the
  existing Medtronic re-assert in SPIKE. Also **reverted v24** (repeat-pairing scoping — it was a
  wrong lead and turned the phone loop into a hard failure). If this holds, the phone-bond
  papercut is actually FIXED, not just tolerated.
- v24 (2026-07-22, SUPERSEDED/reverted in v25): **scoped repeat-pairing recovery to the pump only.** HW on v23
  confirmed pump/BG solid but the **phone bond breaks on every SPIKE→NORMAL cycle** (disc 0x13 +
  phone "error pairing", self-heals only after a manual forget/re-pair on the phone + a spike
  toggle). Root cause suspected: `prv_handle_repeat_pairing_event` was set (by
  `CONFIG_MINIMED_SAKE_SPIKE`) to unconditionally delete-peer-bond + RETRY on any repeat-pairing —
  including the phone's, forcing a full re-pair each cycle. Fix: that delete+retry now only runs
  when `minimed_sake_get_mode()==SPIKE` (the pump, which genuinely needs it for legacy JW); in
  NORMAL the phone falls through to stock behavior (IGNORE → keep the bond), like an ordinary
  Pebble. Low-risk attempt; if the phone still loops, next step is a diagnostic-logging build to
  see why the phone re-initiates pairing at all (needs several cycles, each costing a re-pair).
  Log: `repeat-pair: pump bond reset`.
- v23 (2026-07-22, HW: pump/BG solid, gateway fix worked, phone-bond loop found): **single-connection toggle + gateway-displacement fix.** Dual
  connection (v21/v22) was set aside: the pump connected but the link died with supervision
  timeout (disc 0x08) before the SAKE handshake. **CORRECTION (later same session):** that 0x08
  reproduced on v23 *single-connection with phone BT off*, so it was NOT dual radio scheduling —
  it was the **FE81/FE82 bond mismatch** (watch advertises FE81 + paired flag set, but its SM
  bond was deleted — via the recovery step "forget pump in Settings→Bluetooth", which doesn't
  clear the app's paired flag — so the pump connects, can't encrypt, times out). Fix = DOWN
  (forget pump) on watch + remove/re-add "Mobile PB" on pump (clean FE82 first-pair). This means
  **dual connection was never fairly tested** (v21/v22 hit the same mismatch); if revisited,
  reconcile the bond first. User is fine toggling regardless, so
  v23 reverts `BLE_MAX_CONNECTIONS` to 1 and the advert.c/service.c dual machinery back to v20,
  keeping ONLY the real fix for the "phone won't reconnect after SPIKE" bug (seen on v20 too, not
  a dual-only bug): in `nimble_store.c prv_nimble_store_write_sec`, a bond written while in SPIKE
  mode is the pump's (the phone only ever pairs in NORMAL), so it's persisted like any bond (keys
  needed to reconnect after reboot) but flagged **`is_gateway=false`** — it no longer becomes the
  OS *active gateway* and can't displace the phone. Toggle model unchanged from v20: SELECT
  = SPIKE (pump) ⇄ NORMAL (phone); switching terminates the active link and re-advertises.
- v22 (2026-07-22, SUPERSEDED by v23 — dual abandoned): **gateway-displacement fix on top of v21.** The pump's SM bond
  was being registered with the OS as `is_gateway=true` (every bond is, via
  `nimble_store.c prv_notify_host_bonding_changed`), making the pump the *active gateway* and
  displacing the phone — symptom seen on v20 HW: pump shows in the watch's Bluetooth menu, phone
  won't reconnect. This path runs below v21's GAP-event routing, so v21 alone did NOT fix it.
  Fix: `prv_nimble_store_write_sec` still keeps the pump's LTK in NimBLE's in-memory store (the
  encrypted link + RPA resolution need it) but skips the OS-bonding notify when
  `minimed_sake_bond_is_pump(peer_addr)`. The pump pairs (legacy JW) *before* its SAKE handshake,
  so its address isn't persisted yet then — advert.c captures the pump's identity address at
  connect-classification time (`s_pump_conn_addr`) precisely so this filter works for a
  first-pair pump. **v21 is superseded; flash v22.**
- v21 (2026-07-22, SUPERSEDED by v22 — do not flash): **dual pump+phone connection, "manual dual" scope.**
  `MYNEWT_VAL_BLE_MAX_CONNECTIONS` 1→2 (syscfg). The pump's link is now spike-only and never
  surfaces to the Pebble stack; the phone's flows through normally — classification at connect
  time by the pump's **persisted identity address** (new settings key `pumpaddr`, captured at
  handshake DONE), else no-stored-bond ⇒ first-pair pump, else (paired, pre-v21 state, Medtronic
  advert live) ⇒ pump — the FE81 advert is an RPA the phone can't resolve, so that fallback is
  sound and retires itself after one handshake. All per-conn GAP events (disc/enc/MTU/params/
  identity/subscribe/NOTIFY_RX) filter on the pump handle; the SAKE char rejects writes from any
  other central. Advert rule: pump slot open → fast Medtronic advert; pump connected & phone
  open → ordinary advert (bonded phone reconnects by address, payload irrelevant); both
  connected → silent. A deferred 1.5s "ensure" callout re-asserts this after every connect/
  disconnect/stack-disable. Mode toggle no longer drops the phone (`force_readvertise`
  reworked); SPIKE→NORMAL terminates only the pump link. `forget_pump` now also deletes the
  pump's NimBLE bond (a stale bond would misclassify a re-pairing pump as the phone). Known
  risks for the HW test: loopback watchface sender vs a real phone session (possible AppMessage
  competition), new-phone *pairing* in SPIKE still unsupported (payload/SM wrong — pair in
  NORMAL), battery unmeasured.

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
- v18 (2026-07-22, **NOT independently verified** — v20's pump-side pass doesn't cover it; run
  the deliberate cycle test in `TESTING.md` "Verify the v18 phone-bond fix"): **phone-bond fix**
  — runtime SM reconfig
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
- `minimed_iob.{c,h}` — **pure** IOB decode/parse (NimBLE-free so the host harness links it):
  `minimed_iob_decode_medfloat32_mu` (medfloat32 → milliunits, faithful/overflow-guarded, no
  business gate) + `minimed_iob_parse_response` (SRCP `0x03FC` opcode check + medfloat32 at offset
  3 + 0..100 IU plausibility gate). Host-tested (§Section 4, 8 checks) vs OpenMinimed vectors incl.
  the live-confirmed 1.4 IU frame.
- `minimed_sake_read.c` — post-handshake reads, watch as GATT client:
  - **CGM (BG):** discover `0x181F`, chars `2AA7/2AA8/2A52`; read Feature (plaintext); subscribe
    Measurement (notify) + RACP (indicate), CCCD = val_handle+1; poll RACP `01 06` every 60s.
    Measurement is SAKE-encrypted → decrypt → reassemble by size byte → SFLOAT → mmol/L.
  - **IOB (v29):** after CGM setup, discover IDD service `0x100` + SRCP char `0x105` (128-bit
    vendor UUIDs, same base as the SAKE port char); subscribe SRCP indicate (CCCD = val+1). On
    each CGM RACP-success, chain (via ~200 ms `s_iob_co` callout, after the CGM gattc op finishes)
    a SAKE-**encrypted** SRCP `0xF3 0x03` write; decrypt the `0x03FC` indication → `minimed_iob_
    parse_response` → `send_iob`. NO byte-0 length prefix (unlike CGM). All IDD-discovery failures
    fall back to BG-only polling (`s_h_srcp==0`). Shared inbound `client_crypt` stays in sync
    because CGM→IOB is serialized per poll.
- `minimed_sake_sender.c` — the **watchface local-sender**: loopback CommSession (QEMU-transport
  pattern) opened in SPIKE mode only (would compete with the real phone session in NORMAL);
  injects `[PP hdr 0x0030][CMD_PUSH][watchface UUID][dict: key 10 ts, key 11 BG "N.N", key 14 IOB
  "N.N"]` via `comm_session_receive_router_write` on KernelMain under `bt_lock`. `send_bg` and
  `send_iob` are separate setters (BG/IOB arrive from different reads); `send_iob` deliberately
  does NOT advance the BG timestamp. `send_next` drains the watchface's outbox: ready ping
  (CMD_PUSH) → ACK + immediate push; its ACKs of our pushes are swallowed (loop guard). Protocol:
  `minimed-pebble-watchface/docs/PEBBLE_GLUCOSE_PROTOCOL.md`.
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — spike core: log ring buffer, mode flag, stage
  reports; declares the app↔BT-layer seam (`force_readvertise`, `pump_paired`, `forget_pump`,
  `sender_set_mode`).
- `src/fw/apps/system/minimed_sake_app.{c,h}` — launcher app (viewer + buttons).

Modified:

- `Kconfig` (+`MINIMED_SAKE_SPIKE`), `wscript_build` (spike sources, incl. `minimed_iob.c`),
  `init.c` (register service), `system_app_registry_list.json` (app id `-200`).
- `minimed_sake_service.{c,h}` — added `minimed_sake_encrypt` (server-direction wrapper, mirror of
  `minimed_sake_decrypt`; used for the SRCP request) and `minimed_sake_addr_is_pump` (RAM-only pump
  identity captured at handshake DONE, for the v29 conn label).
- `advert.c` — SPIKE-mode advert hijack (Medtronic payload; mfr data `0x01F9` carries the name);
  interval clamp 100–140ms; own-addr type by mode; `force_readvertise()` = `gap_le_advert_force_
  data_refresh()` + drop the active link (v27 rearchitecture); NOTIFY_RX routes pump notifications
  to the read layer first; disconnect stops polling; repeat-pairing auto-recovery re-enabled. **v29
  diagnostics:** `advS`/`advN` byte dumps of the actual advert payload; `conn PUMP/phone m=S/N`.
- `gap_le_advert.{c,h}` — added `gap_le_advert_force_data_refresh()` (v27; nulls `s_current_ad_data`
  + re-airs, modeled on `bt_driver_handle_host_resynced`), so a mode toggle re-pushes the payload.
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
- **IOB read HW-confirmed (2026-07-24):** IDD service `0x100`, SRCP char `0x105` (write+indicate,
  CCCD = val+1). Request = SAKE-encrypted `0xF3 0x03` (opcode 0x03F3), **no E2E-CRC**; response =
  SAKE-encrypted single indication, opcode `0x03FC` + flags + medfloat32 IOB, **no E2E trailer**.
  Confirms the 780G leaves E2E off for the IDD service (already in `Documentation/idd-service.md`)
  and that watch→pump SRCP requests are just SAKE-encrypted with no wrapper. Same machinery should
  serve the other SRCP gets (pump status 0x102, active basal 0x0365, SmartGuard/TAS 0x03FD).
- SAKE init: pump subscribes → watch notifies 20 zero bytes (**deferred, never synchronously in
  the subscribe callback** or the pump misses it) → pump writes 20 zeros → 6-msg handshake. Keys
  per device-type pair; DB format in `Documentation/key_databases.md`.

## Gotchas

- **On-watch sender crashes an under-hardened watchface on launch (FIXED 2026-07-23).** The
  local sender injects a BG AppMessage with ~zero latency (loopback), unlike a phone whose reply
  carries BLE round-trip delay. If the watchface opens its AppMessage inbox before `window_load`
  creates its layers (it does), a message landing in that launch gap hits any *unguarded*
  `text_layer_set_text(NULL, …)` → `PBL_ASSERTN` hard fault → "slow load then sad-watch", but
  ONLY in SPIKE (NORMAL has no sender, so it looked like a corrupt install). Fix lives in the
  **watchface** (`minimed-pebble-watchface/src/c/main.c`): NULL-guard the bg/ago/iob text-layer
  updates like the graph/status layers already are; `window_load` re-renders them so no data is
  lost. Not a firmware bug — no flash needed, just rebuild+reinstall the `.pbw`. Symmetric latent
  risk on the time/date layers (tick subscribed before window_load) left unguarded for now (a tick
  in the sub-ms launch gap is very unlikely); guard them too if it ever recurs.

- Single BLE connection (`BLE_MAX_CONNECTIONS 1`): phone or pump, never both — toggle between
  them (SPIKE=pump, NORMAL=phone). Dual was tried (v21/v22) and abandoned: the second connection
  died with supervision timeout before the handshake (radio scheduling starves the pump link on
  one nRF radio). If dual is ever revisited, that scheduling problem is the thing to solve, not
  the host config (`BLE_MAX_CONNECTIONS`/controller pools scale fine).
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
3. **IOB — DONE, HW-VERIFIED (v30, 2026-07-24).** Reads IOB via IDD SRCP `0x03F3`→`0x03FC`
   (first confirmed HW use of `sake_encrypt_for_pump`), forwards to watchface key 14; IOB + BG both
   on the watchface. **Pump status still TODO** (therapy/operational
   state, reservoir, sensor state via IDD Status `0x102` encrypted read; SmartGuard via TAS
   `0x03FD`) — same IDD machinery now proven by IOB, plus watchface status key 15. Ref: bridge
   `.../ble/read/IddStatusReader.kt`, `Documentation/idd-service.md`.
4. **History/graph backfill — DEFERRED, staged next flash.** Watchface graph side already done
   (keys 17/18/19). Do it as its OWN flash after IOB proves the shared IDD layer on HW (one change
   per flash). Staged plan: (0, host) port SG_MEASUREMENT 0xF00C + reference-time parse + a
   multi-record short-PDU reassembler into the host harness, vectors from the bridge's
   HistoryReader/MedtronicHistoryParser tests; (2a, flash, log-only) IDD-scoped RACP count +
   report-last-N, log the decoded count/points, no inject — proves the streaming/framing/decrypt on
   HW; (2b, flash) wire history→graph, enlarge the two sender buffers in `minimed_sake_sender.c`
   (frame[] +96, payload[] +64: a 2 h trace ~78 B, 24 h ~870 B; CommSessionAppMessage8kSupport is
   already set), add keys 17/18/19, start with a ~2 h window. Multi-record RACP over IDD History
   `0x108`, 184-MTU chunked streaming (`Documentation/gatt-streaming.md`). Watch the exact-multiple
   short-PDU ambiguity the bridge itself never fully pinned (HistoryReader TODO 48.A2).
5. **Dual pump+phone** — ATTEMPTED AND ABANDONED (v21/v22). Second connection died with
   supervision timeout (disc 0x08) before the handshake: two connections on the single nRF radio
   starve the pump link. Would need connection-parameter tuning (slave latency / negotiated
   intervals so both fit the scheduler) to revisit; user opted for the single-connection toggle
   instead (v23).
6. **Battery measurement** — connection-interval keep-alive dominates (same for poll vs push);
   levers: connection interval (pump-dictated), slave latency, persistent-vs-per-update link.
   Unmeasured; could be the limiting factor for dual connection.
7. **Phone-bond papercut** (re-pair dance between test cycles) — CONFIRMED on HW (v23): phone
   bond breaks on every SPIKE→NORMAL cycle. v24 attempts a fix (scope repeat-pairing recovery to
   the pump only). If that's not enough, root-cause why the phone re-initiates pairing (diagnostic
   build). v18's SM-reconfig is also still not independently verified.

## References

- `../Documentation/` — protocol source of truth. **Update eagerly** with confirmed facts.
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` — the working Kotlin impl to
  port (peripheral, SAKE, reads, history parser) + tests.
- `../xdrip-pebble-archive/PythonPumpConnector/PythonSake/pysake/` — readable SAKE reference +
  public test vectors (`constants.py`).
- Do NOT comment on GitHub/upstream — Morten does that himself (plans to update PebbleOS #853).
