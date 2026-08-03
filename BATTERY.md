# Battery drain on the SAKE-spike firmware

Why this file exists: the on-watch spike burns roughly **6–8 days per charge** where stock
PebbleOS on this same Pebble 2 Duo runs **3.5–4× longer** — measured, not remembered
(2026-08-03). That gap is the last big open item (`PROGRESS.md` item 6), the obvious suspect (the
pump link) has been measured and largely cleared, and the measurements are subtle enough to get
wrong. This file holds the numbers, the method, and what is left to try. It is about **our firmware's power behaviour**; pump *protocol*
facts still belong in `../Documentation/`.

## The short version

- Measured drain is **~0.5–0.75 %/h ≈ 5.5–8 days/charge**, and it is roughly the same whether the
  pump link is up or not. The spread is mostly **SoC-dependent**, not condition-dependent: within
  a single 33 h pump-connected window the rate climbed 0.34 → 0.64 %/h as the battery went from
  93% to 76%. Only ever compare runs over the same SoC band.
- **The pump link is not a 5× factor.** A control night in NORMAL with no pump link came in at
  0.65 %/h against 0.96 %/h for the previous pump night and 0.63 %/h for a day of ordinary SPIKE
  use — i.e. within the spread of the measurement, at most a modest effect (see the confounds
  section: this method cannot resolve anything under ~1.5×).
- So the Medtronic **NOS "Observation Mode"** connection-parameter write — the lever this
  investigation was heading toward — would be optimising something that is not the problem.
  Deprioritised, not because it wouldn't work, but because there is nothing there to win.
- **It is our diff, and we now know the size of it.** A controlled night on stock 4.31.1
  (2026-08-02/03) drained **0.165 %/h** against 0.57–0.72 %/h for our firmware in nearby bands —
  **3.5–4×**. The 4.24 base is exonerated too: Morten has seen comparable life on the stock builds
  he ran before. So the work left is finding what in our diff keeps the watch awake, not a rebase.
- **How good are these numbers?** Endpoints are exact if you anchor on the logged 1% steps, but the
  method still can't resolve a difference smaller than ~1.5×, because one percent is not a fixed
  amount of energy. Good enough for "is the pump link a 5× factor"; not good enough for the 0.96 vs
  0.65 split above. See "How accurate is any of this?".
- **Correction to an earlier claim:** "875 µA steady" was a misreading. `I: … uA` in the battery
  log is quantised in steps of ~876 µA (observed values: 0, 875, 10510 = 12×, 28027 = 32×), so it
  is a coarse instantaneous sample, not an average. The true average draw is a *fraction* of one
  LSB and cannot be read off a single line. Use %/h.

## Measured data

All from one flash-log dump (`gen 0`, boot of 2026-07-27 18:39, dumped 2026-07-29 08:04), so
same firmware (v41) and same battery throughout — the cleanest comparison we have.

| Window | Mode | Range | Duration | Rate |
|---|---|---|---|---|
| 07-27 22:13 → 07-28 07:35 | SPIKE, pump connected + polling all night | 63 → 54% | 9 h 22 m | **0.96 %/h** |
| 07-28 07:35 → 23:35 | SPIKE, worn, ordinary day | 54 → 44% | 16 h 00 m | **0.63 %/h** |
| 07-28 23:35 → 07-29 07:14 | **NORMAL, phone only, no pump link** (control night) | 44 → 39% | 7 h 39 m | **0.65 %/h** |
| 07-27 20:59 → 07-29 07:14 | everything above | 64 → 39% | 34 h 15 m | **0.73 %/h ≈ 5.7 days** |

Earlier, separate capture (2026-07-26, also SPIKE with one 11-min pump outage in 7 h):
80 → 76% over 5 h 33 m = **0.72 %/h**. Consistent with the table.

Later capture (2026-08-02, v41, one flash-log generation, SPIKE with the pump connected throughout
— 2688 `minimed_sake_read.c` lines), and it is the most informative one so far:

| Window | Range | Duration | Rate |
|---|---|---|---|
| whole window | 93 → 76% | 33 h 23 m | **0.51 %/h ≈ 8.2 days** |
| first day | 93 → 89% | 11 h 48 m | 0.34 %/h |
| night | 89 → 83% | 10 h 28 m | 0.57 %/h |
| next day | 82 → 76% | 9 h 24 m | 0.64 %/h |

Two conclusions. **The 5.7-day figure above is pessimistic** — a full day and a half with the pump
connected came in at 8.2 days-equivalent, which matches Morten's own "roughly a week" impression.
And **the rate rises monotonically as SoC falls**, 0.34 → 0.57 → 0.64 %/h on one firmware, one
battery, and a comparable day-night mix. That is the model nonlinearity below, demonstrated rather
than suspected: it nearly doubles across a 17-point SoC span, which is larger than most effects we
are trying to measure. Any comparison between two runs in different SoC bands is worthless.

Per-1%-step rates inside those windows ranged from **0.38 to 2.07 %/h**. That spread is the
single most important thing to know before designing another experiment — see below.

### The stock 4.31.1 night (2026-08-02/03) — the decisive one

Protocol: charged to exactly 80%, updated to stock 4.31.1 (booted 22:44), phone connected all
night, glucose watchface in front, no pump link. Controlled charge because of the SoC dependence
above.

| Step | Time |
|---|---|
| 80% | 22:44:58 (charge ended) |
| 79% | 00:52:00 |
| 78% | 06:55:00 |

**79 → 78% took 6 h 03 m = 0.165 %/h ≈ 25 days.** Against our firmware in the nearest bands —
0.64 %/h (82→76%), 0.57 %/h (89→83%, also a night), 0.72 %/h (80→76%) — **stock is roughly
3.5–4× better**, far above the ~1.5× this method can resolve.

Three caveats, none of which overturn it. It is **one step**, so the rate is exact for that
interval but is a single interval. The band-matched spike numbers come from worn days, so the
fairest single comparison is the 89→83% spike *night* at 0.57 %/h — still ~3.5×. And stock
4.31.1 differs from our build in two ways at once, base version and our diff.

**The second one is settled by Morten's own experience**: he has seen similar battery life on the
stock builds he ran before, including 4.24-era ones. So the base is exonerated and **the drain is
in our diff**. Nothing left to learn from a stock 4.24 night.

Retrieval note: stock's log lines cannot be dehashed — Core Devices' build has its own loghash
dictionary that we don't have, so `Percent:` comes back as `NL:115f6 4f f5e 36b 73a1 'no' 'no'`.
The args are still there in hex, in order: pct, mV, µA, mC, charging, plugged. That is enough to
read a discharge curve off any stock build.

### The control night, and why it counts as a control

Protocol: SELECT into NORMAL at ~23:57, glucose watchface in front, phone left connected, pump
left paired (`PROGRESS.md` item 6). Verified after the fact from the flash log:

- Zero `minimed_sake_read.c` lines in 00:00–07:14 (the pump night had **783**), so the pump link
  genuinely never came up and nothing was polling it.
- 69 log lines total across the whole window vs 822 for the same hours of the pump night — the
  watch was idle in the way we wanted.
- The 100–140 ms forever-advertising clamp is gated on `spike_mode` (`advert.c:576`), and
  advertising stops while connected anyway, so the night was stock advertising behaviour: phone
  link only, at the stock 30–45 ms / latency-3 parameters.

The accepted contamination Morten's protocol predicted (the pump retrying by bonded address all
night, each attempt bounced by the v32 reject) **did not materialise** — no reject cycles in the
log at all. The pump appears not to have looked for the watch. Good for the control's cleanliness;
worth remembering separately, since it also means "pump gives up while we're in NORMAL" is a
thing that can happen.

### The first v44 day with hourly attribution (2026-08-03)

Worn workday, SPIKE, pump connected except for one 55-minute dropout. 08:08 78% → 16:06 72% =
**0.75 %/h**. What the `hb` lines say:

- **CPU, tasks and flash writes are innocent.** ~98% of every hour in nRF stop-mode sleep
  (`slp1`), 0.8% running, `main`/`bg`/`tmr` all ≤ 0.01%, app ≤ 0.1%, and 2–9 KB of flash writes per
  hour. Whatever costs the 4× does not run code.
- **Both of our radio states cost the same.** The hour containing the dropout logged
  `advs 0/3218` — 54 min advertising at the 100–140 ms SPIKE clamp, only 381 s connected — and
  drained 0.80 %/h, indistinguishable from the fully-connected hours (0.62–1.06). We are always in
  one state or the other, and neither is cheap.
- **The pump link is 125 ms at slave latency 0**, supervision 3000 ms (`gap_le_connect.c:372`:
  `master=0, 100, slave lat=0`). Stock's phone link is 45 ms at latency 3 = 180 ms effective — the
  same 45 ms our own phone link gets (recovered from the stock night's raw args,
  `NL:5695 1 0 0 24 0 1f4 59`). That is 1.4× the wake rate, which does **not** by itself explain
  4×.
- `stat 0`/`lowp 0` every hour, but the watch was *worn* — stationary mode is not supposed to engage.
  That column only means something overnight, and is still worth checking there.
- Hourly `drop` ranged 0.13 to 1.06 %/h across hours with **identical** metrics
  (`conns 0/0/3600`, `advs 0/0`). The instrument is fine; the fuel-gauge model is the noise. One
  hour is still one sample.

Caveat on the stock comparison, stated rather than buried: that night produced ~15 log lines and
has a reconnect at 03:34, so we cannot tell how much of it the phone link was actually **up**. If
the phone was away for hours, stock spent them advertising at 1022 ms — nearly free — and part of
the 4× is that rather than a per-event efficiency difference.

## Measuring drain: the instruments

The watch already measures everything needed. **No firmware change is required to run a battery
test** — which is worth knowing before anyone builds instrumentation for it (v34 did, and its
`prm` line turned out to duplicate a stock log). What v43 added is not new measurement but a
retrieval path: it prints metrics the firmware already collected to a log we can actually get at.

### 1. The flash log's 1%-step curve — what we have actually used

`Percent: N, V: … mV, I: … uA, T: … mC, charging: …, plugged: …` at **INFO**
(`src/fw/services/battery/nrf_fuel_gauge/battery_state.c:404`). Above 10% SoC it fires *only* when
the integer percent changes, so the log is exactly one line per percent: a complete discharge
curve, free, on every build, going back to the last flash.

The state machine samples once a minute (`BATTERY_SAMPLE_RATE_MIN 1`), so each step's timestamp is
accurate to ±1 min — which is why every one of those lines lands on `:00` seconds.

**Do not try to capture this live.** `pebble logs` only streams what happens while you are
attached, shows nothing for hours on an idle watch, and cannot see SPIKE mode at all (no phone
session). Reproduce the condition, then pull the history afterwards:

    adb forward tcp:9000 tcp:9000
    tools/dump_flash_logs.py -g 0 --dict build/sake-spike-vNN-<desc>.loghash.json -o /tmp/gen0.log
    grep 'Percent:' /tmp/gen0.log

- Needs Developer Connection on and the watch connected to the phone — toggle out of SPIKE into
  NORMAL first, then dump.
- `-g 0` is the current boot, `-g 1` the previous one. **A flash ends the generation**, so dump
  before reflashing or the window is split.
- The dict must match the firmware that *wrote* the lines (`spike-build.sh` archives one next to
  every `.pbz`); with the wrong dict the lines stay as raw `NL:xxxx`.
- Output has HH:MM:SS and no date. Reconstruct dates by walking the file in order and incrementing
  the day on each backwards time jump.
- `libpebble2` lives in the pebble-tool venv, not the system python. The script now re-executes
  itself with the venv interpreter when the import fails, so any `python3` works. (Before
  2026-08-02 this was a ModuleNotFoundError; the old advice to "run it via the shebang" was wrong,
  since `#!/usr/bin/env python3` is exactly the interpreter that lacks the module.)

### 1b. The hourly heartbeat log lines (v43+) — instrument 1's resolution problem, solved

v43 logs the drain-relevant subset of the analytics heartbeat (instrument 2 below) straight to the
flash log, once an hour, four lines:

    hb bat soc 63.42 drop 0.71 mv 3912 tte 486m
    hb cpu cpct run 213 slp 1231/4005/4519 idle 9622
    hb ble advs 0/3600 conns 3600/0/0 cpct host 42 ctlr 110
    hb ble disc spvn 1 remterm 0 other 0

Read it as: SoC and the drop since the previous heartbeat in percent (0.01% resolution, so an
hourly drain rate directly); `cpct` = centi-percent, i.e. `run 213` is 2.13% CPU; `advs` =
seconds advertising at the short/long interval; `conns` = seconds at the min/mid/max connection
interval; `host`/`ctlr` = BT host and controller task CPU; `disc` = disconnects by reason
(supervision timeout, remote terminate, other).

Why this and not the DLS decoder the earlier version of this file proposed: DLS records need a
phone session to retrieve, and the Core app is the other consumer — it empties the session when it
takes them. A SPIKE night's records would likely be gone before we could download. The flash log
has neither problem. Retrieval is exactly the `dump_flash_logs.py` recipe above, then
`grep 'hb '`.

Two caveats. `advs` counts the *time* correctly but labels it by the advertising job's nominal
short/long interval, not SPIKE's 100–140 ms clamp. And `soc`/`drop` still come from the fuel-gauge
model, so the nonlinearity in "How accurate is any of this?" applies unchanged — what this buys is
hourly granularity and the causes beside the drain, not a better absolute number.

v43 also fixes a gap that would have left `conns` at 0/0/0 for the pump: the connection-interval
timers were only started by a parameter-*update* event, so a link whose master never renegotiates
— which is exactly the pump — was never counted. They now also start at connection establishment
(`gap_le_connect.c`, slave connections). Stock bug, not spike-specific.

### 2. The hourly analytics heartbeat — the source of the above, still undecoded on the DLS side

Every hour (`HEARTBEAT_PERIOD_SEC 3600`, `src/fw/services/analytics/analytics.c`) the firmware
snapshots ~91 metrics into a 523-byte record and logs it to the **data logging service**
(`native.c`, tag `DlsSystemTagAnalyticsNativeHeartbeat` = 87). For battery work the payload is
much richer than the percent line (`include/pbl/services/analytics/analytics.def`):

- `battery_soc_pct` and `battery_soc_pct_drop` — SoC and the drop since the previous heartbeat, in
  **centi-percent (0.01%)**. That is a direct per-hour drain rate, 100× finer than method 1.
- `battery_tte_s` — the fuel gauge's own time-to-empty estimate. `battery_voltage`, `_delta`.
- `cpu_running_pct`, `cpu_sleep0/1/2_pct`, and **per-task** CPU including `task_cpu_bt_host_pct`,
  `task_cpu_bt_controller_pct`, `task_cpu_idle_pct` — i.e. the "causes".
- `ble_adv_short_intvl_time_ms` / `ble_adv_long_intvl_time_ms` — **this measures hypothesis #2
  directly.**
- `ble_conn_itvl_min/mid/max_time_ms` — **and these measure hypothesis #1 directly.**
- `backlight_on_time_ms` + average intensity, vibe and speaker on-time, `stationary_time_ms`,
  and per-reason BLE disconnect counters.

Properties that matter for our use, all read out of the firmware:

- **Recording is unconditional and phone-independent.** `dls_log()` writes to flash; the
  send-enable flags only gate *shipping* (`dls_main.c:44`), and both default to `true` in RAM
  (reset to true every boot).
- **Each record carries its own `rtc_get_time()` timestamp** (`native.c:241`), so records shipped
  late still plot at the hour they describe.
- **Capacity is not a constraint.** The DLS quota is 640 KiB total minus a 20×4 KiB per-session
  reserve ≈ 560 KiB, so at 523 B/hour a fully idle DLS holds **~1000 records ≈ 6 weeks** of
  heartbeats. Sharing with health sessions still leaves weeks.
- Shipping is attempted every 5 min while a phone session exists
  (`DATALOGGING_DO_FLUSH_CHECK_INTERVAL_MINUTES`); the phone then *empties* the session
  (`DataLoggingEndpointCmdEmptySession`).

We do not decode these records yet. The layout is generated from `analytics.def` by the same macro
sweep in `native.c`, so a decoder is mechanical — worth writing, because it turns a battery test
from "one number per 8 h" into "an hourly table with the suspected causes beside it".

Retrieval today, with a footgun: `pebble data-logging list` / `download --session-id N`. Two
cautions. (a) `download` **temporarily flips send-enable on**, so it can drain buffered records —
and the phone app is the other consumer of the same session, so who ends up with them is a race we
have not characterised. (b) `list` crashes on `UUID.__format__` in the installed pebble-tool; it is
patched in place locally (`commands/data_logging.py`, wrap the value in `str()`), which will revert
if the tool is reinstalled.

### 3. The Core app's battery screen

Fed by the same hourly heartbeat (that is where a phone-side "detailed drain + causes" view would
get both the SoC series and the per-task/BLE/backlight breakdown). Convenient, and the only
instrument with a UI, but it is the one we cannot inspect or verify — treat its numbers as a
pointer and confirm anything load-bearing against instrument 1 or 2.

### Does it backfill a SPIKE stretch?

**Firmware side: yes, it should.** Heartbeats keep being recorded to flash with correct timestamps
while no phone is connected, and there is more than a month of room, so a SPIKE window is still
sitting there when you toggle to NORMAL and gets shipped within ~5 min.

The unverified half is the phone: whether the app plots late-arriving records into their real hours
or only appends "now" samples, and whether its own enable toggle discards what it receives while
off. Cheap empirical test, no code: leave the feature on, spend a normal SPIKE stretch, toggle to
NORMAL, and see whether the graph fills the gap or starts at the reconnect.

One caveat about last night specifically: the `data-logging download` run on the morning of
2026-07-29 flipped send-enable on and requested session 164 (which returned nothing to the tool).
If any pre-enable records were still buffered, that is likely when they went to the app — so if the
app now shows some history from before you enabled the feature, that is the cause, not evidence
about how it behaves normally.

## How accurate is any of this?

Short answer: **the endpoints are much better than 1%, and the dominant error is real variation in
what the watch was doing** — so precision is not the thing to improve, experiment design is.

- **Anchor windows on `Percent:` step lines, never on wall-clock endpoints.** A step line is the
  moment the modelled SoC crossed an integer, so a step-to-step window has an *exact* Δ% and a ±1
  min Δt (0.2% relative over 8 h). Using "44% at bedtime → 39% at breakfast" instead throws away
  up to two whole steps — that is where a ±15–20% error would come from, and it is avoidable.
- **What remains is the model, and it is not linear in energy.** SoC comes from the nRF fuel-gauge
  model over a flat Li-po region: the 63→54% window spent 7.7 mV per percent, the 44→39% window
  4.8 mV. So one percent is not one fixed amount of energy, and %/h taken in *different* SoC ranges
  are not strictly comparable. We have no independent energy measure to correct this with — the
  logged `I: … uA` has an ~876 µA LSB (values seen: 0, 875, 10510 = 12×, 28027 = 32×) and is
  useless as an average, and the fuel gauge's own current integration is what produced the SoC in
  the first place. **Mitigation is design: compare windows that start near the same percentage.**
- **Observed step-to-step spread was 0.38–2.07 %/h**, and 0.63–0.96 %/h across multi-hour windows.
  Some of that is the model nonuniformity above, some is genuine (worn vs. on a table, backlight,
  pump reconnect churn). Either way it sets the resolving power: with windows of ~5–10 steps this
  method **cannot settle a difference smaller than roughly 1.5×**. It comfortably answers "is the
  pump link a 5× factor" (no) and cannot answer "is the pump link a 30% factor" (unresolved).
- **Practical rules.** ≥8 h and ≥5 steps per window; overnight only, since wearing and interacting
  add uncompared load; note the SoC range with every number; and treat one night as one sample —
  the 0.96 vs 0.65 split in the table above is exactly the size of effect this method should not be
  trusted on alone.
- **The way out, when we need better:** instrument 1b, shipped in v43. Hourly
  `battery_soc_pct_drop` at 0.01% resolution, with CPU-sleep and BLE-interval time on the same
  lines, is both finer and self-explaining — it can attribute an hour's drain instead of just
  measuring it. It does not fix the model nonlinearity above; it fixes everything else.

## Ranked drain hypotheses

- **#0 The stock 4.24 base — REJECTED (2026-08-03).** Stock 4.31.1 drains 0.165 %/h against our
  0.57–0.72 %/h, and Morten has seen comparable life on the stock builds he ran before, 4.24-era
  included. So the gap is neither the hardware nor the base: **it is our diff**, and the fix is an
  optimisation hunt, not a rebase.
- **#0b (now top) Something in our diff keeps the watch awake.** The 3.5–4× is far too large for
  the pump link alone (#1, largely cleared) and larger than the advertising clamp can explain on a
  good night (#2). The candidates that fit an always-on cost are: the watch never entering
  stationary/low-power mode, a timer of ours running far more often than stock's, and the flash
  writes our own logging generates. v44 prints all three hourly (`hb task`, `hb sys`), so the next
  spike night should name the culprit rather than narrow the field.
- **#1 The pump link's connection parameters** (125 ms, slave latency 0, 3000 ms supervision,
  never negotiated because every param-update consumer in `bt_conn_mgr.c` is on the phone path).
  **Largely cleared as the main cost** by the control night, and both levers for it are
  closed/deprioritised: `ble_gap_update_params` is refused by the pump with HCI `0x3B` for the
  *mechanism*, not the value (latency 1 and 4 both refused, twice, recorded in
  `../Documentation/bluetooth.md`), and NOS Observation Mode is now not worth building for battery
  reasons alone. Still the honest explanation for the pump night's 0.96 vs the control's 0.65, if
  that difference is real at all.
- **#2 SPIKE advertising is ~8.5× stock, forever, while the pump is away.** 100–140 ms with
  `BLE_HS_FOREVER` (`advert.c`), vs stock 20 ms for 30 s then 1022 ms indefinitely
  (`gap_le_advert.c`). Only bites during outages, but an all-night outage is an all-night drain —
  a *conditional* lever, worth nothing on a good night. Fix would be a term schedule (fast bursts
  alternating with slow); pump reconnect latency is already 1–2 min, so little is lost.
- **#3 Vibrations** — fixed in v34. The motor cost more per reconnect than the whole handshake.
- **Not a lever: the 60 s poll.** ~10 GATT PDUs/min riding connection events that happen thousands
  of times a minute anyway. Stretching it to 5 min saves essentially nothing.

## Remaining experiments

Ordered by information per unit of effort. The baseline question is answered; what is left is
finding which part of our diff costs the 3.5–4×.

0. **Use the second watch (arrived 2026-08-03; a third comes with the dev kit).** Running stock and
   our build **simultaneously** removes the confound that limits every measurement above — SoC band,
   daily activity, ambient temperature — because both watches see the same night. Do one
   calibration night with the *same* firmware on both first, to measure how much the two batteries
   differ on their own; only then is a stock-vs-ours pair worth trusting to better than 1.5×.
1. **A v44 night in NORMAL, charged to 80%, matching the stock night exactly.** Same band, same
   anchor (the 79% step), phone connected, glucose watchface in front. This is the decisive one:
   same firmware, stock-like radio config. Near 0.165 %/h means the whole cost is SPIKE's radio
   configuration — the advertising clamp and the 125 ms latency-0 link — and the advertising half is
   ours to fix without the pump's cooperation. Near 0.7 %/h means the cost is in the base or in
   something of ours that is active even in NORMAL. Also read `hb sys stat`: overnight, stationary
   seconds near zero would be a finding in itself.
2. **Then bisect our diff against whatever the lines implicate.** The three shapes to expect:
   never entering stationary/low-power mode (`hb sys stat`/`lowp`), a task of ours running
   constantly (`hb task main`/`bg`/`tmr`), or our own logging writing flash all night
   (`hb sys flashw`/`flashe` — 2688 pump-read lines in a day is not free).
3. **Outage-mode advertising (#2 lever).** Cheap to build, but only pays off on bad nights — decide
   with real numbers on how often all-night outages actually happen once `hb ble advs` has
   reported a few nights.
4. **NORMAL vs SPIKE over matched SoC ranges**, if the pump link is still implicated after the
   above. Both ≥8 h from the same start percentage, read off the `hb` lines rather than the 1%
   steps.
5. **One zero-cost thing to do alongside whatever else runs:** leave the app's battery feature on
   and check after the next SPIKE stretch whether its graph backfills the gap — settles the one
   open question the firmware can't answer.

Not on the list any more: a night on stock 4.24 (the base is exonerated — see #0), NOS Observation
Mode (nothing to win), `ble_gap_update_params` (mechanism refused by the pump).
