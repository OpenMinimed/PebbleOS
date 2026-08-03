# Watchface: crashes and display integration

The on-watch firmware injects AppMessages into `../pebble-glucose-watchface` unmodified. This file
holds the launch-crash saga (currently dormant) and the launch-gap gotcha behind it. Firmware
architecture is in `PROGRESS.md`; the wire format is
`../pebble-glucose-watchface/docs/PEBBLE_GLUCOSE_PROTOCOL.md`.

## Gotcha: the on-watch sender crashes an under-hardened watchface on launch (FIXED 2026-07-23)

  local sender injects a BG AppMessage with ~zero latency (loopback), unlike a phone whose reply
  carries BLE round-trip delay. If the watchface opens its AppMessage inbox before `window_load`
  creates its layers (it does), a message landing in that launch gap hits any *unguarded*
  `text_layer_set_text(NULL, …)` → `PBL_ASSERTN` hard fault → "slow load then sad-watch", but
  ONLY in SPIKE (NORMAL has no sender, so it looked like a corrupt install). Fix lives in the
  **watchface** (`pebble-glucose-watchface/src/c/main.c`): NULL-guard the bg/ago/iob text-layer
  updates like the graph/status layers already are; `window_load` re-renders them so no data is
  lost. Not a firmware bug — no flash needed, just rebuild+reinstall the `.pbw`. Symmetric latent
  risk on the time/date layers (tick subscribed before window_load) left unguarded for now (a tick
  in the sub-ms launch gap is very unlikely); guard them too if it ever recurs.

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
