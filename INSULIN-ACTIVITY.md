# Insulin activity curve

Forward-projected **insulin activity** (U/h) on the watchface: how fast insulin is acting to lower
BG now, and how fast it will be acting over the next few hours. The forward half is the point.
Motivating decisions: time a temp target or a strategic suspend against a planned exercise window,
and check that a meal bolus produces activity overlapping the carb absorption window.

Status: **designed and approved (2026-09-19), nothing implemented.** Design posted publicly on
[OpenMinimed/PebbleOS#28](https://github.com/OpenMinimed/PebbleOS/issues/28), which builds on
[#22](https://github.com/OpenMinimed/PebbleOS/issues/22) (total IOB including basal). Related
investigation: `../Documentation/` has nothing on this yet; the IOB decay fit is described below.

Start here to resume: read #28's three comments (context, design, the RAM-only correction), then
§"Build order" below for the next concrete action.

## The model

Linear and time-invariant: a ledger of delivery events `(tᵢ, dᵢ)` convolved with a fixed kernel
`k(a)`, normalized so `∫₀^td k = 1`, evaluated on a 5-minute grid.

```
activity(t) = Σᵢ dᵢ · k(t − tᵢ)     for 0 ≤ t − tᵢ < td
```

Forward projection adds a **best guess that delivery continues at the recent basal rate**: synthetic
5-minute deliveries at that rate are convolved with the same kernel, so the forward half is
continuous with the past — it starts near zero at `now` and rises to a plateau at the assumed rate
as the real deliveries behind it decay. (Adding the rate as a constant instead would double-count
near `now`, where the committed curve already reflects that rate.)

- The rate is estimated from the ledger itself: recent deltas below 0.25 IU, summed over up to
  an hour, so bolus lumps don't inflate it and it self-tunes without a configured basal profile.
- **Suspend** (therapy PAUSE) forces the assumed rate to exactly zero, so the forward half falls.
- **Temp target** has no special constant: the reduced delivery feeds the rate estimate, so it
  drops within a sample or two and recovers after the temp target ends. This lags the pump's
  decision by the estimate window; the pump does not report the temp target's percentage.

Without the continuing term the curve could only decay, which read as wrong in daily use (a
suspend or temp target appeared to do nothing). Gross activity is never netted against a
configured basal profile: in SmartGuard the configured profile is not what is being delivered, so
netting against it would be meaningless.

**Units are U/h. Do not multiply by ISF.** ISF is the noisiest parameter in the chain and it is
exactly what shifts during exercise, which is the use case driving the feature. Keeping the curve
on the insulin side is honest about what is known. (An earlier draft proposed mmol/L; rejected.)

## Two kernels, two jobs

- **`k_fiasp` drives the display.** Loop/oref exponential parameterised by `tp`/`td`, starting at
  **tp = 55 min, td = 360 min** (LoopKit's Fiasp preset; AndroidAPS Ultra-Rapid Oref also uses
  peak 55). Both configurable, both fitted against real data in phase 2.
- **`f_pump` validates the ledger only.** The 780G's own decay, and it never reaches the display.

Why not the pump's own curve, which one draft recommended: what we have is a fit to the pump's
*displayed* IOB, which is an AIT-scaled accounting curve for the bolus calculator, hard-clipped to
zero at the AIT setting. Measured on a clean 4.25 U bolus (`../logs/watch/2026-08-21-g0-v51-raw-iob.txt`,
11:51): oref-style exponential, **DIA ≈ 130 min, peak ≈ 42 min, RMSE 1.5%** of the bolus, then
clipped at 116 min against AIT = 2 h. Differentiating that asserts insulin action peaks at 42 min
and is over after two hours. For Fiasp neither is true, and for exercise planning the error is in
the unsafe direction: it says the pressure is gone while it is still landing.

Consequence for validation: reconstructing IOB and comparing against the pump's reported IOB
validates the **ledger**, and only when reconstructing with `f_pump`. It says nothing about
whether the display kernel is right. Separate checks.

Exponential formulas (Loop/oref), `t` in minutes:

```
τ = tp·(1 − tp/td) / (1 − 2·tp/td)
a = 2τ/td
S = 1 / (1 − a + (1 + a)·exp(−td/τ))

k(t) = (S/τ²)·t·(1 − t/td)·exp(−t/τ)                              # fraction per minute
f(t) = 1 − S·(1 − a)·[ (t²/(τ·td·(1−a)) − t/τ − 1)·exp(−t/τ) + 1 ] # fraction remaining
```

## Where the deliveries come from: reservoir differencing

_Reservoir Remaining Amount_ (f32, IU) in IDD Status, already parsed as `reservoir_mu` in
`fw/services/minimed/core/minimed_status.c` (logging only, not displayed). Evidence from
`../logs/watch/2026-08-25-g0-v55-fullday.txt`:

- Resolution is **0.025 IU**, the same quantum as IOB, so individual micro-boluses are resolvable.
- Micro-boluses are directly visible. From 15:34: 149700, 149625, 149550, 149475, 149425 mu at
  5-minute intervals, about 0.85 U/h of auto-basal.
- Suspends need no reference basal rate: the reservoir simply stops falling. This is what made the
  history-event approach awkward in SmartGuard (reasoning in #22).
- Boluses, autocorrections and micro-boluses arrive in one stream with no attribution needed. For
  activity purposes a unit is a unit.

Alternatives rejected: **IOB steps** (bolus-only, so biased low in exactly the situation where you
are deciding whether to add insulin); **IDD history `0xf001` events** (exact and attributed, but
gated on the RACP history backfill still deferred as item 4 in `PROGRESS.md`). Still unprobed and
worth a cheap read-only experiment either way: `GET_DELIVERED_INSULIN` (0x03c0/0x03cf) and
`GET_TOTAL_DAILY_INSULIN_STATUS` (0x0395/0x039a).

### Why the current logs look sparse (answered 2026-09-19)

The comment at `fw/services/minimed/minimed_session.c` already records that IDD Status
Changed **bit 2, Reservoir Status Changed, "rides every microbolus"**. But the read path only
re-reads status on bits 0, 1 and 16, so the reservoir is sampled only as a side effect of suspends
and SmartGuard transitions. That is why the overnight gap in the v55 log is hours wide.

The ledger is therefore **push-driven, not polled**: one status read per bit-2 push is one
reservoir sample per micro-bolus, with the pump deciding when there is something to look at.

## Build order

### Phase 1 — logging build (next action)

Add `MINIMED_IDD_FLAG_RESERVOIR (1ULL << 2)` to `fw/services/minimed/core/minimed_idd_flags.h` and
to the trigger set at `minimed_session.c`. About five lines. The `res=%ld mu` log line
already exists and the 0x101 flag word is already logged, so sample provenance is derivable from
the dump.

Cost is roughly 12 extra SAKE status reads per hour in Auto Mode. Risk is low relative to the v57
hard-fault: this changes which pushes trigger an existing read path, it does not add logging
inside a BLE callback.

Soak about a week, then dump (`tools/dump_flash_logs.py`). HW checklist:

1. Reservoir samples arrive at ~5-minute cadence in Auto Mode, confirming the bit-2 comment.
2. A manual bolus appears as a single large delta.
3. A set change is captured, so the prime burst is characterised on more than the one instance we
   have.
4. Overnight cadence matches daytime, ruling out the observed gap being push behaviour rather than
   trigger selection.

### Phase 2 — offline prototype (Python, before any C)

Reservoir deltas to ledger, with three filters:

- A level **increase** means a reservoir change.
- `res=0` means **unknown**, not empty (it appears while the reservoir is detached during a set
  change).
- Deltas inside the priming window after an increase are discarded. The one captured set change
  shows why: 06:51 `res=0`, 07:06:01 `res=166000`, then 07:06:12 `res=159750` and 07:06:24
  `res=159150` — 6.85 U of fill and prime in 23 s, none of which reaches the body.

Free cross-checks:

- IOB step rises should coincide with bolus-sized reservoir deltas. Reservoir total minus the
  bolus-derived part is the basal side, which answers #22 as a by-product.
- Reconstruct IOB from the bolus-only ledger with `f_pump` and compare against reported IOB. Match
  means the ledger is complete; drift means deliveries are being missed.

Then fit `tp`/`td` of `k_fiasp` against CGM response on isolated corrections: no carbs 3 h either
side, no exercise, normalize dBG/dt by dose, stack and average. If the traces scatter, per-dose
variance dominates and the honest answer is to keep the Loop preset and say so.

Two things to keep in mind while fitting:

- **CGM lag** (~5–15 min) pushes the fitted peak later than a clamp study would. That is correct
  here, because the goal is predicting the number on the watch, so the lag belongs inside the
  kernel. It does mean the fitted peak should not be compared directly against published Fiasp
  figures.
- **SmartGuard responds to the same excursion being measured**, which is why the ledger must
  include micro-boluses rather than just the bolus.

Output is a lookup table, not a formula.

### Phase 3 — on-watch

New module `fw/services/minimed/core/minimed_activity.{c,h}`, pure and host-testable under
`tools/minimed_sake_hosttest`, following the `minimed_iob` and `minimed_idd_flags` pattern. Three
entry points: feed a reservoir sample, sample the curve onto a grid, load/store the ledger.
Roughly 300–400 lines of C plus tests.

- Ledger: ring buffer of `(uint32 ts, int16 milli_iu)`, 256 entries. Six hours of SmartGuard is
  about 72 micro-boluses, leaving headroom for boluses and autocorrections.
- Kernel: `static const` fixed-point table at 5-minute steps out to `td`.
- **RAM-only. No flash persistence** (decided 2026-09-19, corrected on the issue). A disconnect
  never needed it: the watch stays up and the gap rule covers it. Flash would only buy the reboot
  case, and reboots are rare since v55 fixed the hourly assert-reboot, so a write every 5 minutes
  forever is a bad trade. PebbleOS has no noinit/retained RAM section and adding one means editing
  stock linker files (against the upstream-isolation rule). On boot the ledger starts empty: the
  curve starts at zero and grows as deliveries arrive. It used to be marked **provisional** until
  6 h of samples accumulated, but that dotted the curve for most of the first day after any reboot
  and was the worst part of the feature in daily use (dropped 2026-10-13). The residual effect is
  that the past wing understates by the pre-boot deliveries it never saw — accepted, because it is
  a slowly-changing underestimate rather than a wrong-looking line. Fallbacks if that ever matters:
  write every 30 min instead of every 5, or seed from the pump's reported IOB on boot (recovers
  bolus-side magnitude, not timing).
- **Gaps:** a sample more than 10 minutes after the previous one marks the span unknown. At most
  `MINIMED_ACTIVITY_MAX_FLAT_MU_PER_HOUR` (1.5 U/h, a plausible basal rate) of the delta is spread
  over the gap as a flat rate; any excess is a bolus delivered inside the gap and is recorded as a
  lump at the sample time, so a meal bolus after a slow-basal stretch is not smeared across 25
  minutes as if it were basal. The provisional flag is set only for a dropout longer than
  `MINIMED_ACTIVITY_GAP_ALERT_SECS` (30 min) that hid `MINIMED_ACTIVITY_GAP_ALERT_MU` (0.25 IU) or
  more, and lasts until the gap is older than `td`.

  Both gates came from the first full-day soak (2026-10-13). Push-driven sampling means a
  slow-basal stretch (a reservoir step every 15-30 min) is a routine gap carrying a few tens of
  milliunits; v1 flagged those provisionally, which alone kept the curve dotted overnight. Worse,
  a meal bolus landing right after such a gap looked like a big gap delta, re-arming the flag for
  6 h and keeping the curve dotted most of the next day.
- **Priming:** a reservoir level increase starts a 5-minute window in which deltas are discarded.
  The ledger is **not** cleared — insulin already delivered is still in the body. Heuristic; it
  also discards a bolus given inside that window, which is rare and happens at a moment you know
  about.

### Phase 4 — protocol and display

New capability in `../pebble-glucose-protocol/` (propagate the key to both header copies and
reflash both sides). Firmware computes the convolution and sends the **sampled series**; the
watchface only draws, so the insulin model lives in exactly one place.

Payload: `uint8` array on a 5-minute grid, ~36 points back and 72 forward, at **0.05 U/h per LSB**
(12.75 U/h ceiling, which a 10 U bolus does not approach; finer than a 144 px display can show),
plus `grid_minutes`, `now_index`, `count`, and a flags byte carrying provisional. About 115 bytes,
sent on the recomputation cadence.

Sending the ledger instead was rejected: it would put the insulin model into every watchface that
wants the feature, and a SmartGuard day carries about 200 micro-boluses in 6 h.

Rendering is Morten's own work on `../pebble-glucose-watchface`.

## Not in v1

Deferred deliberately; two weeks of looking at the raw curve will specify these better than
guessing now.

- mmol/L conversion.
- **Neutral reference line.** Zero is a true floor but the wrong anchor for judging "low": at zero
  activity BG rises from endogenous glucose production. The neutral level is roughly where insulin
  action cancels liver output. Candidates: recent TDD/24, or a 7-day trailing mean of the activity
  curve at the same time of day.
- **Committed vs controllable shading** of `now` → `now + tp`.
- **Carb overlay.** Carb absorption in g/h scaled by `1/ICR` lands in U/h too, so both curves share
  an axis and meal matching becomes area vs area.
- Scenario mode (continue delivery forward at an assumed rate and compare curves). The
  continuing-delivery half is now in v1; only the side-by-side comparison of scenarios is
  deferred.

## Open questions

1. The **prime-burst rule** is fitted to exactly one observed set change. Phase 1 checklist item 3
   exists to get more instances before it hardens.
2. Whether `tp`/`td` are **identifiable at all** from Morten's data, or whether per-dose variance
   dominates.
3. What `0x03c0` and `0x0395` return.
4. Whether reservoir resolution (0.025 IU) is tied to the configured bolus-increment setting or is
   fixed.

## Known error sources

- Cannula fill outside the priming window consumes insulin that never reaches the body.
- Linearity breaks down for large doses (depot saturation); absorption varies with site and
  between individuals.
- Reservoir differencing establishes that insulin left the reservoir, not that it reached the body.
- Published `tp`/`td` are population averages.
- Exercise raises insulin sensitivity, so the same activity curve produces a larger BG drop than at
  rest. The curve says **when** the pressure lands, not how hard.

This is a personal visualisation tool, not a dosing calculator. It computes and suggests no doses.
