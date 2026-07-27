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

> **The re-pairing tax and dual connection turned out to be SEPARATE problems, and the cheap half
> is DONE.** The per-flash pump re-pair was caused by bond pruning, not by the single connection
> slot — so **v36 fixes it without touching connection handling at all** (HW-verified 2026-07-26,
> all three checks). A flash now costs zero pairings: sideload, reboot, SELECT into SPIKE, pump
> reconnects by itself. Every later experiment on this project just got cheaper.
>
> **Dual connection (phone + pump simultaneously) is still the next real job**, but only for what
> the bond fix cannot give: live logs over a real comm session instead of filming the watch screen.
> Design and corrected blockers: `docs/superpowers/specs/2026-07-26-dual-connection-design.md`
> section 2 — note it found four things wrong with the plan in "→ NEXT UP" below, most importantly
> that the pump-link swallow does not exist yet and that advertising-XOR-connected, not the two
> connection booleans, is the real blocker.

- **End-to-end PROVEN on real HW (2026-07-21):** advertise as "Mobile PB" → pump connects → SAKE
  handshake → GATT-client CGM read → decrypt → continuous auto-updating BG in mmol/L, matching the
  pump's display exactly. Feasibility fully settled; the rest is productization.
- Reconnect **HW-VERIFIED 2026-07-22** (v16): after a NORMAL⇄SPIKE toggle the pump reconnected by
  itself, re-ran the handshake, BG resumed.
- **ON THE WATCH NOW: v36** (`build/sake-spike-v36-bond-coexistence.pbz`, 2026-07-26).
  **Working:** pump pairs, BG + IOB correct, and **neither bond is ever lost again** — reboot and
  phone re-pair both verified. **Still broken (pre-existing, unrelated):** the real watchface
  crashes on launch, so the Spike app is still what's on screen. v36 = v35 + bond coexistence.
- v35 (`build/sake-spike-v35-scanrsp-and-name-revert.pbz`, 2026-07-26, superseded by v36).
  **Working:** pump pairs, BG + IOB correct on the SAKE Spike app display. **Broken:** the real
  watchface crashes on launch (see the OPEN section below) — Morten is wearing it with the Spike app
  visible instead. v35 = v34 + two fixes for the v34 pairing failure (scan response cleared; advert
  name reverted to "Mobile PB"). Contains v33 and v32. **Unverified in v34/v35 because pairing then
  the watchface ate the session: the graph, the staleness fix, and the v33 phone-bond fix have all
  had zero HW confirmation.** The vibration fix IS confirmed (no buzzing reported since).
- v34 (`build/sake-spike-v34-graph-quiet-battery.pbz`, 2026-07-26, **flashed and it broke pump
  pairing** — see the OPEN section; superseded by v35). Four changes, each independently
  visible in the on-watch log:
  1. **Graph, at last** (watchface key 17). No pump backfill — the watch simply plots the readings
     it has accumulated since it connected, which is what made this cheap (backfill is still item
     4 below). New pure module `minimed_graph.{c,h}` (2.5 h window, 30 points, wire format
     `[ref_ts u32][count u16][offset_min u16 ×N][bg u8 ×N]`), host-tested — 16 new checks, 48/48
     total. A cold start shows an empty graph that fills in over the next couple of hours.
  2. **BG staleness is now honest.** The timestamp sent to the watchface used to be `rtc_get_time()`
     at *poll* time, so a frozen sensor read as permanently fresh ("0 min ago" forever). It is now
     the time the reading first *appeared*, keyed off the CGM record's **Time Offset** field
     (bytes 4–5, minutes since session start) — the only genuine new-reading signal, since
     consecutive readings are often numerically identical. Same signal gates graph appends, so a
     re-poll no longer plots a duplicate point. New log line on a re-poll: `BG 6.2 same 3m`.
  3. **The watch no longer vibrates on Bluetooth activity.** This was the night-time buzzing.
     `minimed_sake_spike_report` fired `vibes_short_pulse()` on connect/subscribe and
     `vibes_double_pulse()` on the two handshake milestones — Spike-1 celebrations from when
     reaching those stages at all was the news. The pump re-handshakes on **every** reconnect, so a
     night of dropouts was a night of buzzing, and these were raw `vibes_*` calls that bypass Quiet
     Time and DND entirely. Worse, the connect pulse fired in NORMAL too, so the *phone*
     reconnecting buzzed as well. All removed. (Stock's own vibe-on-disconnect is a separate
     feature and is disabled by default.)
  4. **Battery instrumentation:** `prm <itvl>ms lat<N> sv<T>ms` on every connect and every
     parameter update, so the pump link's actual duty cycle can be read off the watch. See
     Remaining work item 6 — the connection interval is the prime suspect and this is the
     measurement it needs.
  Also carried: the advert name is now "Mobile Pebble" (needs a re-add on the pump to show).
  **Adversarial review found one real crash** before this flashed (as it did for v31 — keep doing
  this): the v32 pump-reject cleared `s_rejected_pump_conn` when `ble_gap_terminate` returned
  nonzero, which **re-armed the v31 NULL-deref**. The marker means "this connect was never routed
  to the fw stack" — true whether or not the terminate succeeded, since we return either way and no
  `GAPLEConnection` is ever created. Every connection ends eventually, and routing that disconnect
  derefs NULL at `gap_le_connect.c:500`. Now the marker is kept on failure (only the log differs);
  handle reuse is not a risk with a single slot. **The v32 build on HW still carries this bug** —
  it needed a terminate failure to bite, which is why it survived the v32 HW pass. Review also
  fixed: torn `count` read in `minimed_graph_serialize` (snapshot once — the blob could otherwise
  be internally inconsistent, not merely torn); the offset tracker now resets on a warmup sentinel
  (a new sensor session's offset could collide with the old session's last, pairing a fresh reading
  with an hours-old timestamp); age-log underflow on a backwards RTC; and one **vacuous host test**
  (the overflow fill used 1-second spacing, so every encoded offset was 0 and the "offsets are
  ascending" check could not fail for any implementation — now 4-min spacing plus a guard assertion
  that the offsets actually differ). 49/49.
- v33 (`build/sake-spike-v33-phone-bond-prune-fix.pbz`, 2026-07-24, **never flashed — folded into
  v34**):
  **fixes the phone re-pair papercut at its ROOT.** Investigation (workflow) pinned it, and I
  confirmed the code: the "single BLE pairing" prune in `bluetooth_persistent_storage_normal.c`
  ran *unconditionally* — so when the pump paired in SPIKE (a NON-gateway bond) it DELETED the
  phone's (gateway) bond (`prv_delete_other_ble_bondings` at :778, outside the is_gateway guard),
  and the boot prune kept the most-recent bond regardless of gateway status. Phone then couldn't
  re-encrypt on reconnect → `0x13` connect/terminate loop → forced re-pair, every SPIKE→NORMAL
  cycle. (This is why the v23 `is_gateway=false` gateway flag wasn't enough — it skipped the SPRF
  sync but not this prune.) Fix (2 edits, both no-ops in stock where every BLE bond is a gateway):
  gate :778 with `if (is_gateway)` (a non-gateway pump write no longer prunes the phone), and make
  the boot prune PREFER keeping a gateway. Accepted tradeoff (not fixed): the pump bond is still
  pruned when the phone re-pairs or at boot, so the pump re-pairs after a reboot — a tolerable,
  already-routine flow; full phone+pump bond coexistence (skip non-gateway in the collector) is a
  documented follow-up, deliberately NOT shipped blind. **Adversarial review PASSED** (stock
  behavior unchanged — both guards are no-ops where every BLE bond is a gateway; fix correct; no
  single-bond assumption broken; no bugs). Ready to flash pending HW confirm. Fallback if it
  misbehaves on HW: reflash v32 (pump-reject works, phone re-pair papercut returns) or v30 (IOB
  only). Pre-existing note (not from this change): pairing the pump calls
  `bt_persistent_storage_set_unfaithful(true)`, marking the watch unfaithful-to-phone on any pump
  add — spike-only, low severity; suspect it if the phone shows spurious re-sync after a pump add.
- v32 (`build/sake-spike-v32-reject-pump-fixed.pbz`, 2026-07-24, **pump-reject
  HW-VERIFIED; phone-bond loop was the separate cause now fixed in v33**): rejects a pump connection in NORMAL so the phone wins the
  single slot. **HW-confirmed working:** log showed `pump conn in NORMAL -> drop` → `pump drop done
  -> re-advertise` → `connected` + `conn phone m=N` (pump rejected, Pebble advert restored with no
  crash — the v31→v32 review fixes held up — and the phone got the freed slot). **BUT** the phone
  then loops `connected`→`reason 0x13`: its *bond* fails to re-establish after a SPIKE cycle. That's
  a SEPARATE issue (SM/bond, the deferred "v18" area), NOT the pump and NOT v32 — investigation in
  progress. Also queued (uncommitted, cosmetic, rides the next flash): advert name "Mobile PB" →
  "Mobile Pebble" (needs a pump re-add to show). v30's diagnostics settled the root cause on HW: in NORMAL the watch correctly
  advertises as Pebble (`advN 0201060303d9fe` = svc 0xFED9; NOT Medtronic) yet the bonded pump
  still connects + handshakes → it **reconnects by identity address, ignoring the advertised
  payload** (all the v25–v27 advert work was the wrong mechanism for reconnect). **v31 was the
  first cut and would have HARD-FAULTED the watch** (adversarial review caught it: the reject hid
  the connect from the fw stack but its disconnect still routed through → NULL-deref on a
  never-created `GAPLEConnection`, `gap_le_connect.c:500`; also left advertising off-air). v32
  makes the disconnect symmetric — a rejected pump's disconnect is swallowed (not routed) and
  `gap_le_advert_force_data_refresh()` puts the Pebble advert back on air so the phone can take the
  slot. Caveat: RAM-only identity → not rejected on a cold boot until one SPIKE handshake captures
  it (the SPIKE→NORMAL toggle case — the one that hurt — IS covered). **Fallback if v32 misbehaves:
  reflash v30 (IOB-verified, no reject logic — you keep IOB, just live with the re-pair papercut).**
  v30 committed `60eca16b`; v31 deleted (crashy); v32 uncommitted pending HW confirm.
- v30 (`build/sake-spike-v30-iob-diagnostics.pbz`, **IOB HW-VERIFIED 2026-07-24**, committed `60eca16b`):
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

## OPEN→DORMANT: watchface launch crash — does NOT reproduce on v36 (2026-07-26 evening)

**Could not be reproduced on v36 at all.** Tested in this order: launched in NORMAL (fine, shows
time, "need companion app" toast as expected with no sender); toggled to SPIKE with it running
(fine — graph appeared immediately, then live BG); left the watchface and returned while in SPIKE
(fine); switched to another watchface and back (fine). Every one of these crashed reliably on
v34/v35.

Nothing in v36 touches the sender, the watchface, or AppMessage, so the most probable explanation
is the one the audit flagged: **bad persisted state**. `load_state()` runs at every launch and was
reading a v33-era save with v34 code; once the watchface finally ran to completion it rewrote that
save in the current format, and the trigger disappeared. That also explains why the audit could
prove the injected dictionary well-formed while the crash stayed real, and why TEST_MODE never
caught it (`load_state()` is compiled out under TEST_MODE, and `new_data_callback` is never
exercised there — so the emulator run covered neither function that actually runs at launch).

Not proven, because the evidence is gone: the historical crash logs have rotated out of the flash
log, and older generations were written by v35 whose loghash dictionary differs, so they no longer
dehash. **If it ever returns, it is now cheap to catch** — see the flash-log tooling below; the
fault handler logs `PC:`/`LR:` (`fault_handling.c:105-108`) and `app_manager.c:502` logs
`Watchface crashed (id=…)`, all of which go to flash. Reproduce in SPIKE, toggle to NORMAL, dump.

Kept below for the record, since the ruled-out list is still valid and the audit's conclusions
stand:

## OPEN (historical): watchface crashed ("failed" screen) on launch under v34/v35 (2026-07-26)

BG + IOB read correctly and show in the SAKE Spike app, but launching the real watchface gives the
Pebble app-crash screen — the same symptom as the 2026-07-23 launch-gap crash (see Gotchas), which
was supposedly fixed. **Unresolved. Do not repeat these dead ends:**

- **The installed `.pbw` was NOT stale.** Tempting theory, checked and false: `src/c/main.c` was last
  modified 16:43:17 and the `.pbw` built 16:43:50 — 33 s later, so it *did* contain the NULL guards.
  (The 2fb919c commit timestamp of 16:57 is later than both and means nothing here. Compare artifact
  mtime to *source* mtime, not to the commit.)
- **The watchface's rendering and parse paths are healthy.** Verified in the emulator with TEST_MODE:
  BG, IOB, the graph trace and the trend projection all draw correctly with 30 points. So the new
  graph data is not what kills it. **But this clears less than it looks** (found 2026-07-26): under
  `TEST_MODE`, `load_state()` is compiled out (`main.c` `#ifndef TEST_MODE`) *and* `new_data_callback`
  is never exercised — i.e. the emulator run covers the rendering path but neither of the two
  functions that actually run on the watch at launch. Both were then read closely and no fault
  found (the persist reads are size-bounded; `count` is clamped to `MAX_GRAPH_POINTS` before use),
  but they are **not** covered by the emulator evidence. Cheap fix for the gap, no hardware needed:
  a TEST_MODE variant that pushes a synthetic 4-tuplet dictionary through `new_data_callback`.
- **Not memory.** flint reports 9436 B footprint and 56100 B free heap, so `app_message_open(2048,64)`
  is comfortable.
- **Audited every layer access** (`text_layer_set_text` / `layer_mark_dirty`): all were guarded except
  `update_time_and_date`'s time/date layers, which the old Gotcha had explicitly left unguarded with
  "guard them too if it ever recurs". Now guarded — but note a minute tick landing in the
  microsecond-wide window between `tick_timer_service_subscribe` and `window_load` is a lottery win,
  so this is unlikely to be the actual cause of a *consistent* crash. Treat it as hardening.
- `trend_slope` guards `n < 2`, `graph_layer_update_proc` guards `count == 0`, `graph_value_to_y` uses
  a fixed range (no data-derived divide-by-zero), and the graph parse bound-checks the tuple length.
  All confirmed, all fine.

- **The injected AppMessage is NOT malformed — hypothesis DISPROVED, not merely unconfirmed**
  (audit + 2 independent refuters, 2026-07-26). This was the leading theory: a mis-length-ed
  dictionary making `dict_find` walk off the buffer and fault before any watchface guard applies.
  Every link in it was checked against the real code and every one is clean:
  - **`dict_serialize_tuplets_to_buffer`'s size parameter is capacity-in / bytes-written-out**
    (`src/fw/util/dict.c:246` → `:108-116` → `:27-29`, documented at `dict.h:390-391`). So
    `prv_push_bg_cb` passes the bytes actually written, never the 192-byte capacity, and the
    declared dictionary length always equals its content. **This is the fact to remember.**
  - Protocol header length and delivered byte count come from the same variable and cannot diverge
    (`minimed_sake_sender.c:141-148`); endianness matches the reader.
  - `s_frame` is 214 B against a 145 B worst case (real 30-point push: 137 B dict / 155 B payload).
    An overflow is impossible *and* would not be silent — `dict_write_data_internal` bounds-checks
    every tuple and the caller logs `wf dict fail`.
  - The graph blob length is a plain local assigned before the tuplet initializer, from the same
    snapshotted `count` the body loop uses; macro-expansion time and serialize time cannot disagree.
  - `comm_session_receive_router_write` has no max single-write size, no chunking threshold and no
    reassembly state that 155 bytes trips.
  - The app inbox really is 2048 B (the `.pbw` declares SDK 0x05/0x65, past the 8k cutoff), so the
    push is never dropped, let alone truncated.
  - Watchface UUID matches; a mismatch would NACK cleanly anyway, not crash.
- **The launch gap is real but cannot be the cause.** `window_load` genuinely is deferred
  (`window_stack_push` → … → `animation_schedule`), so the 2026-07-23 Gotcha's mechanism is sound —
  but it needs a message to land in a microsecond window against a 60 s poll. That is a per-launch
  lottery, not a consistent crash, and every layer access is now guarded. Consider it closed.

Remaining candidates, in order: **the app fault log's PC/LR** (nothing in a code audit substitutes
for it); the watchface's own launch sequence that the emulator skips — especially `load_state()`
reading a **v33-era persisted state with v34 code**; and firmware code touched by v34 outside the
sender, notably the new `s_reading_ts` / Time-Offset tracking in `minimed_sake_read.c:125-149`.

Two fragile-but-correct spots found along the way, worth a comment each so nobody "fixes" them:
`prv_ack_and_resend_cb` writes an ACK into `s_frame` and `prv_push_bg_cb` immediately overwrites the
same buffer (safe only because `comm_session_receive_router_write` copies synchronously);
and `window_unload` destroys the layers but leaves the pointers non-NULL, so the NULL guards the
2026-07-23 fix relies on are stale after an unload (currently unreachable, but NULLing them is free).

**Next step requires the app crash log**, which needs a phone connection — and that costs the pump
bond (a gateway write prunes it). Use the USB tunnel, not an IP: `adb forward tcp:9000 tcp:9000`
then `pebble logs --phone 127.0.0.1`. This is the second time in one session that the single
connection slot has blocked a diagnosis — more evidence for prioritising dual.

## OPEN: pump "device not found" on v34 (2026-07-26)

After flashing v34 the pump could no longer discover the watch for first-pairing, with the watch
provably advertising correctly. **Unresolved; v35 is the attempted fix.** Read this before touching
`minimed_sake_build_adv`.

What was ruled out by scanning the watch from a laptop (`bluetoothctl`) rather than trusting the
on-watch log — worth repeating as a technique, it settled in one minute what guessing hadn't:

    Device DC:D8:C3:1A:C0:61 (random)        <- static-random identity, matches the log's `t1`
      UUID: Medtronic Inc. (0000fe82-...)    <- the service class the pump scans for, present
      ManufacturerData 0x01f9: 00 "Mobile Pebble" 00
      ManufacturerData 0x0eea: 00 "S103260B014B" ...   <- !! Pebble/Core Devices + watch serial
      AdvertisingFlags: 06                   <- LE General Discoverable + BR/EDR not supported
      RSSI: -34 dBm

So the advert payload, service UUID, flags, address type, interval and signal strength were all
correct — the watch was not the obvious culprit, and the on-watch log (`adv EN FE82 t1`, no
`adv START FAIL`) agreed.

Two things came out of it:

1. **Real bug, fixed in v35: the SPIKE advert never cleared the scan response.** The SPIKE branch of
   `bt_driver_advert_set_advertising_data` set the advert data and returned early, leaving whatever
   scan response NORMAL had installed — Pebble manufacturer data (company `0x0eea`) carrying the
   watch's serial. So while impersonating a Medtronic peripheral the watch answered active scans by
   identifying itself as a Pebble. Long-standing (not a v34 regression), but wrong. Fixed with
   `ble_gap_adv_rsp_set_data(NULL, 0)` (NimBLE only rejects NULL with a nonzero length).
2. **The "Mobile Pebble" rename is reverted to "Mobile PB", and the advert bytes should now be
   treated as load-bearing.** The rename is *legal* — `Documentation/bluetooth.md` allows
   `Mobile ` + 0–7 chars, "Pebble" is 6, and the bridge's own fixture uses the equally long
   "Mobile 000001" — yet it is the only advert-payload change between the last known-good pair
   (v32) and the failure. Reverted to the exact bytes that have paired since v10. **We do not know
   which of the two changes (if either) was responsible**, because both went into v35 together —
   a deliberate trade of diagnostic precision for getting a wearable watch back. If v35 pairs, the
   clean follow-up is to re-try the rename *alone* to find out.

Still-unexplored candidates if v35 does not fix it: pump-side state (a stale/hidden paired-device
entry, or the pump needing its Bluetooth or itself power-cycled), and the total advert length going
22 → 26 bytes (the service UUID stays at bytes 3–6 either way, so a UUID filter should be
unaffected — but that is reasoning, not evidence).

Process note for next time: this cost a flash because a cosmetic advert change was bundled with four
functional ones, against this project's own one-change-per-flash rule. Cosmetic changes to the
advert payload are not cosmetic.

## → NEXT UP: pump push (handoff brief, written 2026-07-27)

**The job: replace the 60 s CGM poll with event-driven reads via IDD Status Changed `0x101`.**
Decided 2026-07-27. Read in this order: remaining-work **item 3b** (the mechanism, fully researched
— read this before touching code), then item 6 (why this is *not* a battery fix), then the two
"ON THE WATCH" version-log entries for v39.

**Use the bridge as the template — Morten's explicit instruction.** It has a week of soak and
robustness tuning behind it, including a tuned fallback poll rate, and those numbers were earned,
not guessed. Port the shape, don't reinvent it. Start from
`../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/src/main/java/com/glycemicgpt/mobile/ble/read/IddStatusReader.kt`
(the Reset Status write is at :129-135) and `../minimed-pebble-bridge/app/src/main/java/com/
mortenfyhn/minimedpebble/BridgeForegroundService.kt` (subscribe at :242-254, flags parse at
:260-272, bit reactions at :943-959, fallback timer at :197-212). Bridge gotchas are in
`../minimed-pebble-bridge/docs/PUMP-DATA.md:33-38`.

**Expectation management, already agreed with Morten:** this is a **freshness** win, not a battery
win. A reading arrives when the sensor produces it instead of up to 60 s later. The 60 s poll is
~10 GATT PDUs/min riding connection events that happen ~8×/second anyway — under 1% of the link's
cost. Do not sell this as battery work; item 6 has the real battery story.

**What is already done:** v39 subscribes to `0x101` passively and logs decoded flags plus raw
plaintext. Discovery, subscribe and decrypt are proven on hardware. The remaining work is the Reset
Status write-back and the serialisation state machine — see item 3b, which also records that the
SAKE cipher is *not* a blocker (that was the feared one, and it is settled).

**Test loop is cheap now — use it.** A flash costs no pairings (v36), and
`tools/dump_flash_logs.py` reads a whole SPIKE session back afterwards. Morten's steer 2026-07-26:
don't over-verify upfront when testing is this cheap. Keep adversarial review for changes that can
hard-fault or wedge the watch; a read-path change that degrades to the existing poll is not that.

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

### After-the-fact logs from a SPIKE session (`tools/dump_flash_logs.py`) — NEW 2026-07-26

**This is the debugging unblock, and it did not need dual connection.** Every `PBL_LOG` line is
written to flash continuously (`flash_logging_set_enabled(true)` at init) and kept per boot
*generation*, so a SPIKE session with no phone attached is still fully recorded. Reproduce a
problem in SPIKE, toggle to NORMAL, and pull the history:

    adb forward tcp:9000 tcp:9000
    tools/dump_flash_logs.py              # this boot
    tools/dump_flash_logs.py -g 1         # previous boot
    tools/dump_flash_logs.py -g 2 --dict build/sake-spike-v35-….loghash.json

Verified on hardware: recovered the complete SAKE handshake (`pump WRITE conn=1 len=20 …`) from a
SPIKE session, dehashed and readable, over the phone afterwards. That is most of what "live logs"
was wanted for — so **Stage 2 dual connection is now a convenience, not a debugging prerequisite.**

Details worth knowing:

- Run it with the pebble-tool interpreter (it needs `libpebble2`), e.g.
  `~/.local/share/uv/tools/pebble-tool/bin/python3 tools/dump_flash_logs.py`. `pyelftools` was
  added to that venv for the dehasher.
- **The dict is per build.** Log lines are stored hashed and the hashes change every build, so an
  older generation needs that firmware's dictionary. `spike-build.sh` now archives one next to each
  `.pbz` as `sake-spike-vNN-<desc>.loghash.json`; pass it with `--dict`. Without the right dict the
  lines come back as raw `NL:xxxx`.
- Requires **Developer Connection** enabled in the Pebble app *and* the watch in NORMAL. Re-pairing
  the watch turns Developer Connection off — that cost time once; the symptom is
  "Connection to remote host was lost" while port 9000 still accepts, i.e. the server is up but has
  no watch. `pebble screenshot` working does **not** mean Developer Connection is on.
- Useful forensic line at every boot: `debug.c:211 Last launched app: <…>`.

### Host unit tests (`./waf test`) — worth using, three traps

The firmware has a real clar unit-test suite (327 tests) that runs on the host in ~1 s once built.
`tests/fw/services/bluetooth/test_bluetooth_persistent_storage.c` in particular covers the bonding
DB, so bond-storage work can be genuine TDD with no hardware. Run it in Docker like everything else:

    docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$PWD":/pebbleos -w /pebbleos \
      pebbleos-build:local bash -lc "
        git config --global --add safe.directory /pebbleos
        ./waf test -M '.*bluetooth_persistent_storage.*'"

- **Never run `./waf configure` without `--board`** — it wipes `build/c4che` and de-configures the
  firmware build. Restore with `./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y`
  (in Docker, with `/opt/pebbleos-sdk/arm-none-eabi/bin` on PATH); verify with
  `grep MINIMED build/autoconf.h`.
- **`-M` is an anchored `re.match` against the test source path.** `-M bluetooth_persistent_storage`
  matches **nothing** and the run still says "finished successfully" having built no tests. Always
  wrap it: `-M '.*name.*'`. Drop `-M` entirely for the full suite (~40 s).
- `./waf test` uses the `test` waf **variant** (`build/test/`), so it does *not* clobber the
  firmware build and is fine to run with the asterix configure in place. First run ~2 min.

### How to build (recreate the image if `pebbleos-build:local` is gone)

`ghcr.io/coredevices/pebbleos-docker:v6` + `pip install -r requirements.txt`, then `docker commit`.
Submodules must be checked out (skip the huge `third_party/hal_sifli/SiFli-SDK`, obelix-only;
`resources/iconography` is required or the resource build dies).

## Version log (terse; full chronological history in `HISTORY.md`)

- v39 (2026-07-26, **ON THE WATCH, soaking overnight — results pending**): v38 + **passive
  subscription to IDD Status Changed `0x101`**, the pump's push channel. Logs the decoded flags and
  the raw plaintext; acts on nothing. Deliberately fire-and-forget *after* `prv_start_polling`
  rather than chained into discovery, so a failed subscribe cannot cost a night of BG.
  Expect one indication then silence (bits latch until an explicit Reset Status, which v39 does not
  send) — if more arrive, newly-set bits re-indicate while others stay latched, which would simplify
  the eventual reset design.
- v38 (2026-07-26, superseded by v39 before flashing): **push probe.** `PBL_LOG` on every RACP poll
  write and every CGM measurement notification, so a flash dump shows whether notifications hug the
  polls or arrive on their own. Both lines are marked `push probe` and **should be removed once
  answered**. Also added timestamps to `tools/dump_flash_logs.py`, without which the experiment is
  unreadable.
- v37 (2026-07-26, **flashed; result: pump REJECTED it**): asked the pump for slave latency 4 via a
  standard peripheral-initiated update. Came back HCI `0x3B` *Unacceptable Connection Parameters*
  despite being spec-valid with a wide margin. Now a probe at latency 1 (riding v39) to separate
  "dislikes the value" from "refuses the mechanism". See remaining-work item 6 and
  `../Documentation/bluetooth.md`.
- v36 (2026-07-26, **FULLY HW-VERIFIED — all three checks passed**): **phone + pump bond
  coexistence — no more re-pairing.** Verified the same evening:
  1. **Reboot survival.** `bond gw1 pmp1 del0` read identically before and after a power cycle
     (phone BT off to isolate the test), then SELECT into SPIKE → `HANDSHAKE OK!` → correct BG +
     IOB, **without touching the pump**. That is the flash tax gone. BG/IOB also confirms no
     collateral damage to the SAKE/CGM/IDD paths.
  2. **Phone re-pair survival.** Forgot the watch on the phone *and* the phone on the watch, paired
     fresh, and the line still read `gw1 pmp1 del0`. A gateway write no longer evicts the pump —
     the direction v33 did not fix.
  3. **Settings pairability.** With the pump bonded, Settings → Bluetooth listed **only the phone,
     no pump**. This is the check with no unit-test coverage (no settings-app harness in this repo),
     so hardware was the only way to know.
  Bonus real-world confirmation of *why* edit 3 was needed: on v35 that same evening the pump bond
  in the Bluetooth menu made the watch unpairable, so the phone sat at "connecting" forever and the
  only way out was forgetting the pump — exactly the lockout edit 3 removes. Also re-learned the
  hard way: forgetting the pump from the **Bluetooth menu** instead of the app's DOWN button leaves
  the app's paired flag set (FE81 with no bond); v36's mode line now shows this directly as
  `MODE: SPIKE (FE81)` above `pmp0`.
  The
  per-flash pump re-pair was never about the single connection slot; it was bond pruning. Three
  edits, all no-ops in stock where every BLE bond is a gateway:
  1. `prv_collect_other_ble_bondings_itr` skips non-gateway bonds. This one edit closes the
     phone-re-pair prune **and** both boot paths, because all three reach the pump through this
     same collector (the boot SPRF replay re-stores the phone as a gateway and lands here, and
     `prv_prune_stale_ble_bondings` deletes through it too). Completes v33, which fixed only the
     mirror image (a pump write evicting the phone).
  2. `bt_persistent_storage_delete_ble_pairing_by_id` only erases the shared-PRF slot for gateway
     bonds, captured *before* the delete since the record is gone after. Forgetting the pump no
     longer wipes the phone's PRF record.
  3. Settings → Bluetooth skips non-gateway bonds when building the phone list. **Required, not
     cosmetic:** pairability is gated on that list being empty, so a now-permanent pump bond would
     otherwise lock out phone pairing forever behind "Forget this device to pair a new device".
  Plus a live `bond gw<N> pmp<N> del<N>` readout in the SAKE Spike app (throttled to every 10th
  refresh — reading it opens the bonding settings file). The `del` counter resets at the *top* of
  `bt_persistent_storage_init`, before the boot prune it runs a few lines later, so after a reboot
  `pmp0 del1` means the prune ate the bond while `pmp0 del0` means it was never stored — and
  `FE81` in the mode line with `pmp0` names the FE81/FE82 mismatch outright.
  **Adversarial review earned its keep again:** the first cut of edit 3 called
  `bt_persistent_storage_is_ble_ancs_bonding` from inside the `for_each_ble_pairing` callback,
  which re-enters the **non-recursive** bonding-DB mutex — opening Settings → Bluetooth would have
  frozen the watch. All 327 host tests passed anyway, because `stubs_mutex.h` stubs `mutex_lock()`
  to a no-op (now in Gotchas). Fixed by filtering in the second pass, after the lock is released.
  A second review pass (4 lenses × 15 findings, each refuted by 2 independent skeptics) confirmed
  that deadlock independently and left only one other survivor, now fixed: the boot prune logged at
  INFO on every boot about a prune that no longer happens (it was effectively unreachable before,
  since the store-time prune had already eaten the pump bond by then) — dropped to DBG and reworded,
  since "most recent" is not the rule applied either. 13 findings were refuted, several of them
  interesting near-misses worth not re-litigating: the NimBLE `BLE_STORE_MAX_BONDS` cap counts peer
  *addresses* in its RAM store, not settings-file records, so two bonds cannot lock out pairing; and
  the pump bond is **not** unremovable despite Settings hiding it — `prv_handle_repeat_pairing_event`
  in `advert.c` deletes and re-pairs it automatically, which is exactly the path a pump-side
  remove/re-add takes.
  Host tests 327/327 including 5 new bond-coexistence tests; the settings filter is HW-only (this
  repo has no settings-app test harness). Fallback: reflash v35.
  Known nit, deliberately not fixed (pre-existing, out of scope): `bt_persistent_storage_is_ble_ancs_bonding`
  ignores the return of `prv_file_get`, so a failed record read reads uninitialized stack. The
  settings filter now depends on it, but the bonding ID it passes was just produced by an iteration
  over that same file, so the read cannot miss.
- v35 (2026-07-26, **ON THE WATCH, pump pairs again**): two fixes for v34's pairing failure.
  (1) The SPIKE advert branch of `bt_driver_advert_set_advertising_data` never cleared the **scan
  response**, so the watch answered active scans with Pebble manufacturer data (company `0x0eea`)
  carrying its serial while claiming to be a Medtronic peripheral — fixed with
  `ble_gap_adv_rsp_set_data(NULL, 0)`. (2) The advert name was reverted "Mobile Pebble" → **"Mobile
  PB"**. Both shipped together, so **which one fixed it is unknown** — see the OPEN section; the
  clean follow-up is to re-try the rename alone.
- v34 (2026-07-26, flashed; **broke pump pairing**, superseded by v35): **graph + honest staleness +
  no more BT vibrations + connection-parameter logging.** Contains v33. Details in Status above. Files: new `minimed_graph.{c,h}`
  (+ wscript_build, + host-test section 5); `minimed_sake_sender.{c,h}` (graph ring, `send_bg` now
  takes the reading's timestamp, new `add_graph_point`, and the two nested stack buffers replaced
  by one static `s_frame` — the graph blob made that pair ~500 B of KernelMain stack);
  `minimed_sake_read.c` (Time-Offset new-reading detection); `minimed_sake_spike_ui.c` (vibes
  removed); `advert.c` (`prv_log_conn_params`).
- v33 (2026-07-24, folded into v34, never flashed standalone): **root fix for the phone re-pair
  papercut — gateway-aware bond pruning.** Root cause (workflow-diagnosed, code-confirmed): `bluetooth_persistent_storage_normal.c`
  pruned "other BLE bondings" unconditionally, so the non-gateway pump bond (SPIKE) evicted the
  phone's gateway bond → phone LTK gone → reconnect LTK-restore MISS → `0x13` loop → re-pair, every
  cycle; the boot prune likewise kept the most-recent bond regardless of gateway. Two edits: (1)
  `if (is_gateway)` around `prv_delete_other_ble_bondings(key)` at ~:778 so a non-gateway write
  never prunes; (2) `prv_find_most_recent_ble_bonding_itr` prefers a gateway so the boot prune keeps
  the phone. Both are no-ops in stock (all stock BLE bonds are gateways). Ruled out (workflow): the
  SM-config flip (bonded reconnect reads none of `ble_hs_cfg.sm_*`; `apply_sm_config` restores all
  fields) and the repeat-pairing handler (it's the recovery, doesn't emit 0x13). Accepted tradeoff:
  pump bond still pruned on a gateway write / at boot (pump re-pairs after reboot); full coexistence
  = skip non-gateway in `prv_collect_other_ble_bondings_itr` (follow-up, not shipped blind).

- v32 (2026-07-24, awaiting HW): **crash fix for v31's reject logic.** v31 would NULL-deref on the
  first rejected pump connection — the reject returns before routing the connect to the fw stack
  (no `GAPLEConnection` created), but `prv_handle_disconnection_event` still routed the disconnect,
  and `bt_driver_handle_le_disconnection_complete_event` → `gap_le_connect.c:500` derefs the
  never-created connection. v32: track the rejected handle (`s_rejected_pump_conn`); in the
  disconnect handler, if it matches, swallow it (return before the `bt_driver_*` routing) and call
  `gap_le_advert_force_data_refresh()` to re-air (the controller stopped advertising on the pump
  connect and the scheduler was never told — without this the watch sits off-air and the phone
  can't take the slot). Terminate rc now logged (a failed terminate would silently leak the slot).
  Adversarial review found this before it ever flashed; v31 deleted.
- v31 (2026-07-24, SUPERSEDED — would crash, see v32): **reject the pump's connection in NORMAL.** Root fix for the
  pump-in-NORMAL / phone-re-pair papercut, now that v30's `advN`/`conn PUMP` diagnostics proved on
  HW that the pump reconnects **by identity address**, not by the advertised service UUID (in
  NORMAL the watch advertised Pebble svc 0xFED9 yet the pump handshaked anyway). `advert.c`
  `prv_handle_connection_event`: if `mode != SPIKE && minimed_sake_addr_is_pump(peer)`,
  `ble_gap_terminate` + return before touching s_sake_conn_handle / the stack. Single slot → the
  freed slot lets the phone win; once connected the phone locks the pump out. Uses the RAM-only
  `s_pump_id_addr` from v30 (so a fresh boot doesn't reject the pump until one SPIKE handshake
  captures it — the SPIKE→NORMAL toggle case, the painful one, is covered). Passive diagnostics
  from v30 retained. If it ever needs to work from cold boot, persist the pump identity.

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
  them (SPIKE=pump, NORMAL=phone). Dual was tried (v21/v22) and set aside. **Do NOT repeat the old
  "radio scheduling starves the pump link" conclusion — it was retracted**: the 0x08 supervision
  timeout that produced it reproduced on v23 *single-connection with phone BT off*, so a second
  link cannot have caused it (it was the FE81/FE82 bond mismatch). Dual has never been fairly
  tested. See "Dual pump+phone" in Remaining work for what it would actually take.
- Phone bond can loop connect/terminate(0x13) after the SM changes; repeat-pairing recovery usually
  self-heals; else forget + re-pair on the phone (NORMAL mode).
- **The host tests are blind to locks — `stubs_mutex.h` makes `mutex_lock()` a no-op.** So a
  lock-ordering or reentrancy bug passes all 327 tests and then hangs the watch. Specifically: the
  bonding DB's `s_db_mutex` is **non-recursive** (`mutex_create()`, not `mutex_create_recursive()`),
  and `prv_file_each` holds it across the *entire* iteration — so calling any
  `bt_persistent_storage_*` reader from inside a `for_each_ble_pairing` callback self-deadlocks.
  This was caught by review in v36 before flashing (it would have frozen the watch on opening
  Settings → Bluetooth). Do per-bond lookups in a **second pass** after the iteration returns;
  `prv_add_ble_remotes` in `settings/bluetooth.c` is the worked example.
- clangd floods spike files with false errors (missing NimBLE include paths). Trust `./waf build`.
- Don't `docker system prune` without asking (~100GB of other images on this machine).
- **FE81/FE82 pairing-state reconciliation.** The watch's paired flag and the pump's bond can
  disagree, and neither auto-corrects: if the watch advertises FE81 (log `adv EN FE81 …`) but the
  pump can't find it, the pump's side is unpaired → press **DOWN (forget pump)** on the watch to
  drop to FE82, then add "Mobile PB" on the pump. Conversely, a watch on FE82 that a bonded pump
  ignores means re-pair on the pump. Rule of thumb: **pump can't see watch → make both do
  first-pair (DOWN on watch + remove/add on pump).** A smarter auto-fallback (try FE82 if FE81
  draws no connection for N s) is a possible future improvement.

## → NEXT UP: dual connection (phone + pump at the same time)

**This is the priority after v34, and it is a tooling fix, not a feature.** Decided 2026-07-26.
Full detail in remaining-work item 5; this section exists so nobody has to infer the priority.

The single connection slot is what makes every other task on this project expensive:

- **No live logs.** SPIKE has no real CommSession, so the only debug channel is the 8-line on-watch
  ring buffer — which we read by *filming the screen*. The firmware already supports log dumping
  over a comm session (`src/fw/debug/debug.c`, `CommSessionInfiniteLogDumping`; what `pebble logs`
  uses), but it needs a phone session that SPIKE cannot currently have. The loopback session is not
  a substitute: its `send_next` discards everything.
- **Every flash and every watchface `.pbw` install costs a pump un-pair/re-pair** (see "What the
  flash costs you in pairings"), so each iteration is minutes of fiddling plus a chance of landing
  in the FE81/FE82 mismatch. That tax is paid on *every* future change to anything.

So dual pays for itself immediately in iteration speed, independently of it being nicer to use.
The old note that "the user is fine toggling instead" is **stale** — that was true when the toggle
only cost convenience, not when it costs the whole dev loop.

Extra design point specific to dual, not yet in item 5: the loopback watchface sender is opened in
SPIKE **only**, deliberately, because "a real phone session would compete with it"
(`minimed_sake_sender.c prv_set_mode_cb`). Under dual both sessions exist at once, so that
competition becomes real and must be resolved — AppMessage routing to the watchface was already
flagged as a v21 known risk. Decide it before flashing: probably keep injecting locally and let the
phone session carry only logs/sideload traffic.

### Handoff brief (written 2026-07-26 — dual work was deliberately handed to a fresh session)

Read in this order: this section → remaining-work item 5 (the three blockers, with file references)
→ item 8 (bond coexistence, a prerequisite) → the two OPEN sections above (known-broken things, so
you don't mistake them for something you caused).

**Hardware state you are starting from:** watch is on **v35**, paired to the pump, BG + IOB correct,
being worn with the **SAKE Spike app** on screen because the real watchface crashes on launch. The
phone is **not** paired to the watch. Everything in the working tree is **uncommitted** (v32 through
v35 inclusive) — consider getting Morten to commit before you start changing things, or you will not
be able to tell your changes from four flashes' worth of his.

**Do not start by writing code.** The two OPEN bugs above mean the baseline is not clean, and one of
them (the watchface crash) is plausibly *caused by* a v34 change of ours. Decide with Morten whether
to fix that first — it is also the thing dual would make cheap to debug, so there is a chicken-and-egg
argument for doing dual first and accepting a broken watchface meanwhile.

**Scope the first flash to the probe, nothing else** (remaining-work item 5 has the detail):
`BLE_MAX_CONNECTIONS` 1→2, refcount `s_is_connected` / `s_is_connected_as_slave`, and give SPIKE its
own `gap_le_advert_schedule()` job. Test one question only: *can both links be up at once, and does
the pump still complete SAKE?* Leave the bond-store and Settings-pairability work (item 8) out of it.

**Rules this project learned the hard way — all of them the expensive way:**
- **One change per flash.** v34 bundled a *cosmetic* advert rename with four functional changes and
  cost a flash plus an evening; the rename is still not exonerated. Cosmetic changes to the advert
  payload are not cosmetic.
- **Run an adversarial review of every HW-untestable change before flashing.** It has caught a
  watch-hard-faulting bug twice now (v31, and again in v34's carried-over v32 logic).
- **Prefer evidence to inference.** Two diagnoses this session were settled by *looking* — scanning
  the watch from the laptop with `bluetoothctl` (which proved the advert was fine when the log
  suggested otherwise), and running the watchface in the emulator with TEST_MODE (which proved the
  graph rendering was fine). Both took a minute. Guessing took hours.
- **Compare artifact mtime to source mtime, not to the commit timestamp** — that mistake produced a
  confident wrong diagnosis of the watchface crash.
- Flashing costs a pump re-pair every time, and pairing the phone deletes the pump bond. Budget for
  it: see "What the flash costs you in pairings" in TESTING.md. This tax is precisely what dual is
  meant to remove, so it is worth being slow and careful to get dual right in few flashes.
- Use the USB tunnel for anything involving the phone (`adb forward tcp:9000 tcp:9000`, then
  `pebble … --phone 127.0.0.1`). Do not chase the phone's IP; it changes with the network.

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
3b. **Event-driven push instead of the 60 s poll — RESEARCHED 2026-07-26, Stage A soaking in v39.**
   The mechanism is **IDD Status Changed `0x101`** (vendor UUID, IDD service `0x100`, Read +
   **Indicate**), the same service we already discover for IOB. Three things the obvious mental
   model gets wrong:
   - It is an **indication**, not a notification (CCCD `0x02 0x00`, pump awaits a confirmation).
   - The flags are a **variable-width self-extending bitfield**: little-endian 16-bit blocks, bit 15
     of each meaning "another block follows", 16/32/48 bits (`Documentation/idd-service.md:143-149`).
     Field capture: all 296 observed indications were 7 bytes on the wire = 4 plaintext + the 3-byte
     SeqCrypt trailer. No length prefix, no fragmentation, no E2E on the 780G.
   - **The bits LATCH.** You must write Reset Status (SRCP `0x030C` + the exact flags to clear) or
     the pump indicates once and goes silent forever. So the cycle is receive → read → *write back*,
     an extra encrypted SRCP exchange. This is the bridge's gotcha #2 (`../minimed-pebble-bridge/
     docs/PUMP-DATA.md:33-35`), implemented at `.../ble/read/IddStatusReader.kt:129-135`.
   Bits the bridge acts on: 0 therapy-control, 3 annunciation, 16 therapy-algorithm, 17 IOB,
   18 new-CGM, plus the fingerstick family {20,21,26,27}.
   **The SAKE cipher is NOT a problem** — this was the feared blocker and it is settled. The inbound
   counter is recovered per-frame from a wire byte (`minimed_sake_crypto.c:132-147`) and committed
   only after the MAC verifies, so a bad frame is dropped rather than poisoning the session; all
   pump→watch frames share one ordered ATT bearer and NimBLE dispatches them synchronously on the
   host task. An async push therefore cannot desynchronise anything. The bridge relies on the same
   invariant and has run all three producers concurrently for months.
   What *does* need serialising is the **reassembly buffers**: `prv_do_poll` unconditionally zeroes
   `s_rec_len` (so two overlapping RACP exchanges truncate a record), and `s_srcp` has a single
   IOB-specific "complete at 7 bytes" rule that a Reset Status reply would break. That is a small
   explicit state machine (one busy flag + a pending-work mask) — the only genuinely new structure.
   Also correct a stale comment while there: `minimed_sake_read.c` claims a second concurrent gattc
   op returns `BLE_HS_EBUSY`. It does not in this build — ops queue FIFO at the ATT layer with
   `BLE_GATT_MAX_PROCS=8`. The real hazard is that the 30 s unresponsive timer starts when a proc is
   *queued*, not sent, so a burst can time out without ever going on air. Deferring is still right,
   for a different reason than we wrote down. Related: NimBLE sends an indication's confirmation
   *after* the handler returns, so a gattc write issued synchronously from a push handler goes out
   ahead of the confirmation the pump is waiting for — keep using the callout defer.
   **Stage A — ANSWERED NO (overnight capture 2026-07-26/27).** The pump does **not** notify CGM
   unsolicited: all 427 measurement notifications arrived within 0–3 s of one of our 428 RACP polls,
   and zero arrived on their own. There is no free shortcut; event-driven BG requires the `0x101`
   machinery. (Analysis trap worth avoiding on a re-run: pair notifications to polls in *file* order,
   not by sorting timestamps — same-second events sort `notify` before `poll` and every notification
   then appears to belong to the poll 59 s earlier, which reads convincingly like unsolicited pushes.)
   **`0x101` latch behaviour CONFIRMED on this pump, and it is stricter than "once".** Exactly two
   indications all night — one per pump connection, each ~1 s after subscribing, both carrying
   identical flags — and nothing at all during steady state. So without Reset Status you get one
   indication *per subscription*, not one per session, carrying the accumulated latched bits.
   Observed value: raw `ef 81 4f 00` → bits {0,1,2,3,5,6,7,8, 16,17,18,19,22} (remember bits 15/31
   are the block-continuation markers, not real flags). That covers therapy-control (0), annunciation
   (3), therapy-algorithm (16), IOB (17), new-CGM (18), 19 (rides every CGM push) and sensor
   connectivity (22) — exactly the "nothing has ever been cleared" set.
   **Stage B:** the port proper (~a few hundred lines). **Stage C:** act on more bits, which overlaps
   with pump status in item 3.
4. **Graph — DONE in v34 (awaiting HW); BACKFILL still deferred.** The graph is now drawn from
   readings the watch accumulates itself, so a cold start begins empty and fills over ~2 h. That
   was the cheap 90%: no new pump protocol, no new failure mode, and it made the staleness fix
   fall out for free. **Pump-side backfill** (filling the graph immediately on connect, and closing
   gaps after a dropout) is the remaining piece and still deserves its own flash. Staged plan: (0, host) port SG_MEASUREMENT 0xF00C + reference-time parse + a
   multi-record short-PDU reassembler into the host harness, vectors from the bridge's
   HistoryReader/MedtronicHistoryParser tests; (2a, flash, log-only) IDD-scoped RACP count +
   report-last-N, log the decoded count/points, no inject — proves the streaming/framing/decrypt on
   HW; (2b, flash) wire history→graph, enlarge the two sender buffers in `minimed_sake_sender.c`
   (frame[] +96, payload[] +64: a 2 h trace ~78 B, 24 h ~870 B; CommSessionAppMessage8kSupport is
   already set), add keys 17/18/19, start with a ~2 h window. Multi-record RACP over IDD History
   `0x108`, 184-MTU chunked streaming (`Documentation/gatt-streaming.md`). Watch the exact-multiple
   short-PDU ambiguity the bridge itself never fully pinned (HistoryReader TODO 48.A2).
5. **← THE NEXT JOB. Dual pump+phone — NEVER FAIRLY TESTED; more feasible than the old notes
   claimed.** Motivation and priority: see "NEXT UP" above (live logs + no re-pair per flash). The v21/v22
   "radio scheduling starves the link" verdict is **retracted** (the 0x08 reproduced
   single-connection with phone BT off → it was the FE81/FE82 bond mismatch, not contention). The
   v21/v22 source is unrecoverable (never committed; only the `.pbz` artifacts survive) — treat the
   v21 entry below as a design description to re-implement, not code to restore. Researched
   2026-07-26; three real blockers, in order:
   - **Advertising while connected is blocked at three layers.** (a) NimBLE refuses connectable
     advertising when the connection pool is full (`ble_gap.c` `ble_hs_conn_can_alloc`), so
     `BLE_MAX_CONNECTIONS` 1→2 is a hard prerequisite; everything else in NimBLE scales off that one
     knob (conn state machines, host pool, L2CAP chans, CCCD state — and `BLE_STORE_MAX_BONDS` is
     already 3). (b) `gap_le_advert.c` `s_is_connected` and `gap_le_connect.c`
     `s_is_connected_as_slave` are booleans, not counters — with two slave links the first
     disconnect re-enables advertising while a peer is still connected. (c) **The likely silent
     killer of v21/v22:** SPIKE has no advert job of its own, it repaints the *Reconnection* job's
     payload — and that job is **unscheduled** when the gateway connects
     (`kernel_le_client.c` → `gap_le_slave_reconnect_stop`), with `_start` refusing to restart while
     connected as slave. So once the phone connects, the pump's advert vehicle is gone, not paused.
     Fix (c) by giving SPIKE its **own** `gap_le_advert_schedule()` job — that also retires the
     whole v25–v27 stale-payload bug class and stops the 100–140 ms clamp leaking onto the phone's
     advert. Caveat: two scheduled jobs round-robin per second, so the pump would see its fast
     advert ~50% of the time (costs discovery latency, not function).
   - **`apply_sm_config` is global and mode-keyed.** Today the toggle guarantees only one peer can
     pair at a time; dual removes that. Needs a per-peer decision or a "pump pairing window".
   - **Bond coexistence** (see item 8) is a prerequisite, and is worth doing on its own merits.
   The **only genuine unknown is whether two concurrent links actually work on this radio** — a
   routine nRF52840/NimBLE configuration, so the prior is good, but only HW can answer. Cheapest
   decisive experiment: `BLE_MAX_CONNECTIONS` 2 + refcount the two booleans + SPIKE's own advert
   job, then test *only* "both links up at once, pump completes SAKE". Leave the bond-store and
   settings work out of that probe.

8. ✅ **Full phone+pump bond coexistence — DONE, HW-VERIFIED 2026-07-26 (v36).** All three checks
   passed: pump bond survives a reboot, survives a phone re-pair, and Settings → Bluetooth no
   longer lists it or refuses to pair a phone. A flash now costs zero pairings. Design:
   `docs/superpowers/specs/2026-07-26-dual-connection-design.md`; plan:
   `docs/superpowers/plans/2026-07-26-stage1-bond-coexistence.md`. All three holes researched here
   on 2026-07-26 are now closed — the collector skip (which turned out to close the boot paths too,
   since they all funnel through it), the unconditional `shared_prf_storage_erase_ble_pairing_data()`
   on any delete, and the Settings pairability gate. The `is_gateway` signal the
   `BtPersistBondingDBEachBLE` callback does not expose turned out **not** to be needed: the public
   `bt_persistent_storage_is_ble_ancs_bonding(id)` answers it (`supports_ancs` is set equal to
   `is_gateway` at store time), as long as it is called *after* the iteration returns and not from
   inside the callback — see the non-recursive-mutex Gotcha. Once HW-verified this closes the item;
   the remaining bond work for dual is per-peer `is_gateway`/SM decisions, tracked under item 5.
6. **Battery — investigated 2026-07-26; drains now ranked, top lever needs one measurement.**
   v34 logs the numbers on-watch (`prm <itvl>ms lat<N> sv<T>ms` at every connect and param update)
   so this stops being guesswork. Ranked:
   - **MEASURED 2026-07-26 (the blocking number, now known): the pump link runs at
     `itvl=125 ms, slave latency=0, supervision timeout=3000 ms`.** Read out of the flash log
     (`gap_le_connect.c:372`, `conn_interval_1_25ms=100`) after a normal SPIKE session — no flash
     needed, and note this FW log already carried the number, so v34's on-watch `prm` line was
     redundant. The radio therefore wakes **every 125 ms, 24/7**, against the phone link's
     45 ms × (latency 3 + 1) = 180 ms effective. Headroom for the fix is ample: the constraint
     `(latency+1) × interval × 2 < supervision timeout` allows latency up to 11, so asking for
     latency 4 (→ 625 ms effective, ~5× fewer wakeups) or even 7 (→ 1 s) is safely inside it.
     **Lever (a) TRIED AND REJECTED (v37, 2026-07-26).** A standard peripheral-initiated update
     asking only for slave latency 4 — keeping the pump's own interval and supervision timeout —
     came back `Connection parameters update failed: 0x023b`, i.e. HCI `0x3B` *Unacceptable
     Connection Parameters*. The request was spec-valid with a wide margin
     (`(1+4) × 125 × 2 = 1250 ms` against a 3000 ms timeout), so this reads as pump policy, not a
     malformed ask — and it suggests the NOS Observation Mode exists precisely because the generic
     mechanism is blocked. Recorded upstream in `../Documentation/bluetooth.md`.
     **Latency 1 was refused too (overnight 2026-07-26/27), on both of the night's pump
     connections, with the same `0x3B`. So the pump refuses the MECHANISM, not the value.**
     `ble_gap_update_params` can never help here; the request code is deleted. Lever (a) is closed.
     Untried variation, low expectation: offering an interval *range* instead of
     `itvl_min == itvl_max`.
     **Measured baseline to beat (same capture): 80% → 76% over 5 h 33 m ≈ 0.72 %/h at a steady
     875 µA — about 6 days per charge**, against ~30 days for this watch in ordinary use. That gap
     is NOT explained by the connection interval alone: 125 ms at latency 0 is only ~1.4× the wake
     rate of the stock phone link (45 ms, latency 3 → 180 ms effective), nowhere near 5×. Something
     else contributes and we do not know what.
     **Link stability that night, which rules #2 out as the explanation:** exactly one pump outage,
     05:20:00 → 05:30:57 (~11 min), in a 7 h session — so the watch was fast-advertising for ~2.6%
     of the night and connected for the rest. The 875 µA is therefore the cost of the **connected**
     state, not of advertising or of reconnect churn. Reducing reconnections is a *conditional*
     lever: worth something on a bad night (an all-night outage is an all-night fast-advertise, the
     #2 entry below), worth almost nothing on a good one. Two loose observations from the same
     capture, neither chased: the reconnect took ~11 min against the 1–2 min this file documents
     elsewhere, and the pump disconnect logged `reason=0x216` (HCI 0x16, *terminated by local host*)
     with `master=1`, which is odd for a link we did not knowingly drop at 05:20 while asleep.
     **Cheapest next step, no code: a control night in NORMAL with the phone only.** Same watch,
     same firmware, no pump link. If the draw falls to stock levels the pump link owns the gap and
     lever (b) is the answer; if it does not, the cost is in our firmware and NOS would be optimising
     the wrong thing. Do this before building anything for battery.
     Lever (b), now the likely real answer: the NOS service "Observation Mode" write carries
     min/max interval, slave latency and supervision timeout (`../Documentation/nos-service.md`).
     Needs its own discovery + a SAKE-encrypted write, and every field's unit is `???` in the doc,
     so doing it would also be a documentation contribution.
   - **#1 The pump link's connection parameters are never negotiated.** Structural, not incidental:
     Pebble only issues a param update on a *consumer-driven* state change (`bt_conn_mgr.c`), and
     every consumer (PPoGATT, GATT discovery, AMS, pairing service) is on the phone path — the
     spike does raw `ble_gattc_*` calls and registers none. So the pump link runs at whatever the
     pump chose, **peripheral latency 0, 24/7**. Compare the stock phone link: 30–45 ms interval
     *with latency 3* → ~180 ms effective. Plausibly a 25–50% battery-life hit on its own.
     Levers, once the logged interval is known: (a) `ble_gap_update_params()` right after
     `SAKE_RESULT_DONE` to ask for peripheral latency — even at an unchanged interval, latency 4
     cuts radio wakeups ~5×; (b) the pump-sanctioned route, the Medtronic **NOS service**
     "Observation Mode" write, which carries min/max interval, slave latency and supervision
     timeout (`Documentation/nos-service.md`) — presumably how the official app does it.
     Mind the constraint `(latency+1) × interval × 2 < supervision timeout`.
   - **#2 SPIKE advertising is ~8.5× stock, forever, while the pump is away.** The clamp is
     100–140 ms with `BLE_HS_FOREVER` (`advert.c`), vs stock 20 ms for 30 s then **1022 ms**
     indefinitely (`gap_le_advert.c`). Only bites during outages (advertising stops while
     connected), but an all-night outage is an all-night drain. Lever: make the clamp a *term
     schedule* (bursts of fast advert alternating with slow) — pump reconnect latency is already
     1–2 min, so little is lost.
   - **#3 Vibrations** — fixed in v34; the motor cost more per reconnect than the whole handshake.
   - **Not a lever: the 60 s poll.** ~10 GATT PDUs/min riding connection events that happen
     thousands of times a minute anyway — well under 1% of the link's keep-alive cost. Stretching
     it to 5 min saves essentially nothing. (The SAKE Spike *app*'s 400 ms refresh timer does cost
     something, but only while it is the foreground app.)
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
