# Pump push: event-driven reads via IDD Status Changed 0x101

2026-07-27. Design for PROGRESS.md remaining-work item 3b Stage B — replace the 60 s CGM poll
with event-driven reads, ported from the bridge (Morten's explicit instruction: the bridge's
shape and tuned numbers are a week of soak; port them, don't reinvent them).

## Goal and non-goals

**Goal: freshness.** A reading reaches the watchface when the sensor produces it (~1 s after the
pump's push) instead of up to 60 s later. Side benefit: the reading timestamp (`s_reading_ts`)
becomes accurate to ~1 s instead of ≤60 s, which sharpens both the "N min ago" display and graph
point placement.

**Non-goals:**
- **Not a battery fix.** The 60 s poll is under 1% of the link's keep-alive cost (item 6).
- **No new watchface data.** Acting on more bits (status, fingerstick, annunciations) is Stage C /
  item 3. This change reads exactly what we read today: CGM last record + IOB.
- **Read-only rule unaffected.** Reset Status (SRCP `0x030C`) clears indication latches in the
  status-reader machinery — part of the standard IDS status-reader flow, no therapy effect. The
  bridge has issued it on every push for weeks. It joins RACP report-stored-records as the second
  sanctioned control-point write.

## Background (settled facts — do not re-research)

From item 3b, the 2026-07-26/27 overnight capture, and the bridge:

- `0x101` (IDD service `0x100`, Read + **Indicate**, CCCD `02 00`) is the push channel. v39
  already discovers it, subscribes passively, decrypts and logs — all proven on HW.
- **Bits latch.** Without Reset Status the pump indicates once *per subscription* (confirmed on
  this pump — strictly stricter than "once per session") and then goes silent. The cycle is:
  indication → targeted read(s) → **Reset Status write-back**.
- Flags are self-extending LE 16-bit blocks; bit 15/31 of a block = "another block follows";
  16/32/48 bits total. All observed indications: 4 plaintext bytes + 3-byte SeqCrypt trailer.
- The pump does **not** notify CGM unsolicited (Stage A answered NO: 427/427 notifications hugged
  a poll). There is no cheaper mechanism than `0x101`.
- **The SAKE cipher is not a blocker**: the inbound counter is recovered per-frame and committed
  only after MAC verification; all pump→watch frames arrive on one ordered ATT bearer dispatched
  synchronously on the host task. The bridge runs the same three producers concurrently.
- What **does** need work is serialising the exchanges: `prv_do_poll` unconditionally zeroes
  `s_rec_len`, and `s_srcp` has a single IOB-specific "complete at 7 bytes" rule that a Reset
  Status response breaks.
- NimBLE sends an indication's **confirmation after the handler returns** — a gattc write issued
  synchronously from the push handler would go on air ahead of the confirmation. All writes are
  therefore dispatched via callout, never from the notify path.
- NimBLE gattc ops queue FIFO (`BLE_GATT_MAX_PROCS=8`), but the 30 s unresponsive timer starts at
  *queue* time — another reason to serialise explicitly rather than letting ops pile up.
- Reset Status response: the generic SRCP **Response Code**, opcode `0x0303` (LE `03 03`), operand
  = request opcode + result code (OpenMinimed `idd/status/opcodes.py`; the bridge's `srcpGet`
  treats any short indication as the complete response). Exact bytes unconfirmed on HW — v40 logs
  them; once confirmed, document in `Documentation/idd-service.md` (it currently documents the
  Reset Status *request* extension but not the response).

## Bridge template (what we are porting)

| Bridge piece | Bridge location | Watch equivalent |
|---|---|---|
| Subscribe once per connection | `BridgeForegroundService.kt` `subscribeStatusChanged` | v39 already does this in `prv_start_polling` |
| Flags parse (self-extending blocks) | `parseStatusFlags` | v39 has it inline; moves to a pure module |
| Bit reactions: 18→CGM read, 17→IOB read | `handlePush` | same two bits, same targeted reads |
| Reset Status after every indication | `resetStatus` = `0x0C 0x03` + flags as received | same, flags re-encoded from the accumulated union |
| Fallback: full read if push silent 6 min | `FALLBACK_AFTER_SECS = 6*60`, checked on 60 s tick | poll callout re-armed to 360 s on push activity |
| Serialised delivery thread | Android binder thread | busy flag + pending mask (NimBLE has no such thread-level guarantee across our callouts) |

Bits the bridge additionally acts on (0 therapy, 3 annunciation, 16 therapy-algorithm,
fingerstick family {20,21,26,27}) are **not** ported: the watch displays neither pump status nor
fingerstick BG yet. They are still *reset* (the bridge resets the full received flags too) and
still logged by the existing v39 log lines, so nothing is lost for later RE.

## Design

All changes live in `minimed_sake_read.c` plus one new pure module. No sender, UI, or watchface
changes.

### 1. Pure flags module: `minimed_idd_flags.{c,h}`

Host-testable, no NimBLE dependencies (pattern: `minimed_iob.{c,h}`, `minimed_graph.{c,h}`):

- `uint64_t minimed_idd_flags_parse(const uint8_t *plain, uint16_t len)` — the v39 inline decode,
  moved. Returns the flag word including continuation bits (matches the bridge, which echoes raw
  bytes back to Reset Status verbatim).
- `uint16_t minimed_idd_flags_encode(uint64_t flags, uint8_t out[6])` — inverse: emit 1–3 LE
  16-bit blocks, width = highest set *real* bit, continuation bits 15/31 set on every non-final
  block and cleared on the final one. Needed because pending resets **accumulate** (a burst of
  indications can land while an exchange is in flight; one Reset then clears the union), and a
  union of a 16-bit and a 32-bit observation needs its continuation bits recomputed.

### 2. Exchange serialisation: busy flag + pending mask

The one genuinely new structure (item 3b's words). In `minimed_sake_read.c`:

```
PEND_CGM    — RACP report-last-record wanted
PEND_IOB    — SRCP get-IOB wanted
PEND_RESET  — SRCP Reset Status wanted (with s_reset_flags: uint64 union to clear)
s_busy      — an exchange is in flight
```

- `prv_request(mask)` ORs into the pending mask and schedules the dispatch callout (~50 ms — the
  same defer that today's `s_iob_co` provides, generalised; it also satisfies the
  indication-confirmation ordering rule above).
- Dispatch order: **CGM, then IOB, then RESET**. Reads first so data lands ASAP; reset last so one
  write covers a whole burst (fingerstick bursts are ~5 indications over 15 s).
- Completion (clears `s_busy`, re-runs dispatch):
  - CGM: the RACP success indication (or unexpected-response line), as today.
  - IOB: SRCP reassembly complete at ≥7 bytes, as today — the rule now applies **only when the
    in-flight op is IOB**.
  - RESET: first complete SRCP indication (expected `03 03 0C 03 <result>`; logged raw). Since ops
    are serialised, in-flight op identity disambiguates the SRCP interpretations.
- Reassembly resets move to dispatch time: `s_rec_len = 0` when the CGM op is *issued* (not in a
  free-running `prv_do_poll`), `s_srcp_len = 0` when an SRCP op is issued. This alone fixes the
  overlapping-exchange truncation hazard.
- **Op timeout, 10 s** (single callout armed at dispatch, stopped at completion): observed
  poll→notification latency is 0–3 s, and NimBLE's own 30 s proc timer would kill the whole link
  before ever helping us. On timeout: log, clear busy + both reassembly buffers, dispatch next
  pending. Without this, one lost indication would wedge push *and* fallback forever.

### 3. Push handler (the `0x101` branch of `minimed_sake_read_handle_notify`)

Replacing "act on nothing":

1. Parse flags (new module). Keep the v39 raw-plaintext log lines — the higher bits are still
   being characterised.
2. Bit 18 (new CGM) → `prv_request(PEND_CGM)`. Bit 17 (IOB) → `prv_request(PEND_IOB)`.
3. Always: `s_reset_flags |= flags; prv_request(PEND_RESET)` — every received indication gets
   reset, acted-on or not (bridge behaviour; also what keeps unlatched-bit semantics irrelevant).
4. Mark push alive: re-arm the poll callout to the 6-min fallback (see below) and set
   `s_push_mode = true` on the first indication.

The first indication after each subscribe carries the accumulated latched set (observed:
`ef 81 4f 00`), so it triggers one CGM+IOB read a second after the initial poll already ran. One
duplicate read per connection; the Time-Offset dedup means nothing is re-plotted. Not worth
special-casing.

If decrypt fails, flags are unknown: log (as v39 does) and do nothing — no reset without knowing
what to clear.

### 4. Fallback poll (the bridge's tuned safety net)

One callout (`s_poll_co`, reused) in two modes:

- **Poll mode** (from connection start): fires every **60 s**, requests CGM+IOB — exactly today's
  behaviour. This is also the terminal degraded state: if `0x101` is missing, the subscribe write
  fails, or no indication ever arrives, nothing changes vs v39.
- **Push mode** (entered on the *first* `0x101` indication — proof the channel actually works, not
  merely that the CCCD write was accepted): the callout becomes a dead-man timer, re-armed to
  **360 s** (`FALLBACK_AFTER_SECS = 6*60`, the bridge's tuned number) on every received indication
  and every completed CGM exchange. If it fires, push has gone quiet ≥6 min while CGM should push
  every ~5 min: log, request CGM+IOB, re-arm 360 s. A silently dead push thus degrades to a 6-min
  poll — the bridge's accepted trade, soaked for a week.

Mode is per-connection state, reset in `minimed_sake_read_start` (a reconnect re-subscribes and
must re-prove push).

### 5. Reset Status write

Built at dispatch time: plaintext `0C 03` + `minimed_idd_flags_encode(s_reset_flags)` (4–8 bytes),
SAKE-encrypted via the existing `minimed_sake_encrypt` (first used for IOB in v30), written to
`s_h_srcp`. `s_reset_flags` is cleared when the write is *issued*; an indication arriving during
the exchange ORs into a fresh union and re-pends.

**On failure (error response or timeout): log and give up until the next indication or
reconnect — no retry loop.** Degradation is benign by construction: unreset latches mean push
goes quiet, the 6-min fallback keeps BG+IOB flowing, and the next connection starts a fresh
subscription. (The bridge is equally fire-and-forget here.)

### 6. Housekeeping in the same change

- Correct the stale comment claiming a second concurrent gattc op returns `BLE_HS_EBUSY` (item 3b:
  ops queue FIFO; the real hazard is the 30 s timer starting at queue time).
- Log vocabulary (SAKE Spike 32-char lines): `0x101 act c/i` (which reads a push triggered),
  `rst ok` / `rst err=..` / `rst resp ..` (raw response bytes until format confirmed),
  `fallback poll` (push went quiet), `op timeout <op>`. The v39 `0x101 <hex>` flags line stays.

## Failure modes

| Failure | Behaviour |
|---|---|
| `0x101` char missing / subscribe write fails / no indication ever | Stays in poll mode = exactly v39 (60 s poll) |
| Push dies silently mid-session | 6-min fallback poll, indefinitely (bridge parity) |
| Reset Status rejected or unanswered | Push goes quiet → 6-min fallback; fresh indication next reconnect |
| Indication burst (fingerstick: ~5 in 15 s) | Pending-mask coalescing: ≤1 CGM read, ≤1 IOB read in flight; one Reset clears the union |
| Lost terminating indication mid-exchange | 10 s op timeout unwedges; next op dispatches |
| Indication during an in-flight exchange | ORs into pending/reset union; dispatched after completion |
| Decrypt failure on `0x101` | Logged, ignored (no reset of unknown flags); frame dropped, cipher unpoisoned (per-frame counter recovery) |

The property Morten's test-cheapness steer keys on: **every failure path lands on the existing
poll (60 s or 6 min), never on "no data".** This is a read-path change that degrades to the
existing poll — flash-and-observe, no adversarial review gate needed (it cannot hard-fault or
wedge the watch beyond what the op timeout already bounds; no new buffers, no new tasks, no
stack growth — the reset write is an 11-byte local).

## Files

- `src/bluetooth-fw/nimble/minimed_idd_flags.{c,h}` — new pure module (parse + encode).
- `src/bluetooth-fw/nimble/minimed_sake_read.c` — state machine, push reactions, reset write,
  fallback mode; `s_iob_co` generalised to the dispatch callout; comment fix.
- `src/bluetooth-fw/nimble/wscript_build` — add the new module.
- `tools/minimed_sake_hosttest/` — new section: flags parse/encode round-trips.
- `PROGRESS.md` — v40 version-log entry; item 3b Stage B marked done.
- `Documentation/idd-service.md` — after HW confirmation: Reset Status response format + observed
  reset/unlatch behaviour (per the "update eagerly" convention).

## Testing

**Host** (`tools/minimed_sake_hosttest`, `make run`): flags parse of the observed `ef 81 4f 00`;
encode round-trips at 16/32/48-bit widths; continuation-bit recomputation for a mixed-width
union; encode(parse(x)) == x for observed vectors. The state machine itself is NimBLE-coupled and
is deliberately *not* host-tested (Morten's steer: don't over-verify a change that degrades to
the poll; the flash loop is cheap).

**Hardware (v40),** via `spike-build.sh` + `tools/dump_flash_logs.py`:
1. Connect, SPIKE: expect initial poll → subscribe → first indication ~1 s later → `0x101 act c/i`
   → reads → `rst` response line. **The decisive check: a second indication ~5 min later** (the
   thing v39's latch made impossible), with BG following it.
2. Overnight soak: indications every ~5 min all night, zero (or rare, logged) fallback polls, BG
   age on the watchface staying <1 min.
3. Record the reset response bytes → update `Documentation/idd-service.md`.

## Decisions (with rationale)

1. **Act on bits 18 + 17 only; reset everything.** The watch displays only BG + IOB; the other
   bridge reactions have no on-watch consumer yet (Stage C / item 3).
2. **Fallback = 360 s, entered only after a proven indication.** The number is the bridge's tuned
   value per Morten's instruction; the entry condition is stricter than the bridge's (which trusts
   the subscribe) because a false "push works" on the watch would silently stretch 60 s → 6 min.
3. **Reset failure = log and wait, no retry.** A retry loop against a persistently unhappy pump
   is worse than the 6-min fallback we get for free.
4. **Flags module is pure + host-tested; state machine is not.** Encode has real edge cases
   (continuation bits, width growth); the state machine's failure modes all degrade to the poll
   and are cheaper to observe on HW.
5. **One callout doubles as poll timer and fallback dead-man** rather than a separate 60 s
   housekeeping tick (the bridge's tick also re-paints Android notifications — none of that
   exists here; a mode flag on the existing callout is less state).

## Open questions (answered by the v40 flash, none blocking)

- Exact Reset Status response bytes (expected `03 03 0C 03 <result>`); logged raw until confirmed.
- Whether the pump accepts our re-encoded flag union when it differs in width from what it sent
  (bridge always echoes verbatim; first mixed-width burst will tell — failure degrades to fallback).
- Whether IOB pushes (bit 17) really arrive "every few minutes" on this pump as they do via the
  bridge — determines how much of the freshness win IOB gets.
