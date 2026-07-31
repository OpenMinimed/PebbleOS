# Battery drain on the SAKE-spike firmware

Why this file exists: the on-watch spike burns roughly **5–6 days per charge** where stock
PebbleOS on this same Pebble 2 Duo gave Morten *much* longer. That gap is the last big open item
(`PROGRESS.md` item 6), the obvious suspect (the pump link) has now been measured and largely
cleared, and the measurements are subtle enough to get wrong. This file holds the numbers, the
method, and what is left to try. It is about **our firmware's power behaviour**; pump *protocol*
facts still belong in `../Documentation/`.

## The short version

- Measured drain is **~0.65–0.75 %/h ≈ 5.5–6.5 days/charge**, and it is roughly the same whether
  the pump link is up or not.
- **The pump link is not a 5× factor.** A control night in NORMAL with no pump link came in at
  0.65 %/h against 0.96 %/h for the previous pump night and 0.63 %/h for a day of ordinary SPIKE
  use — i.e. within the spread of the measurement, at most a modest effect (see the confounds
  section: this method cannot resolve anything under ~1.5×).
- So the Medtronic **NOS "Observation Mode"** connection-parameter write — the lever this
  investigation was heading toward — would be optimising something that is not the problem.
  Deprioritised, not because it wouldn't work, but because there is nothing there to win.
- The remaining suspects are **our firmware diff** and, newly, **the stock 4.24 base we forked
  from**. The watch runs `v4.24.0-48-g5f10b8b3c`; the Core app currently offers **v4.30.1**, whose
  release notes read *"Better battery life through fewer background wakeups."* Morten's memory of
  much better life on an older stock build is consistent with either.
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

Per-1%-step rates inside those windows ranged from **0.38 to 2.07 %/h**. That spread is the
single most important thing to know before designing another experiment — see below.

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

## Measuring drain: three instruments

The watch already carries everything needed. **No firmware change is required to run a battery
test** — which is worth knowing before anyone builds instrumentation for it (v34 did, and its
`prm` line turned out to duplicate a stock log).

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
- `libpebble2` lives in the pebble-tool venv, not the system python: run the script directly
  (shebang) or via `~/.local/share/uv/tools/pebble-tool/bin/python3`, else ModuleNotFoundError.

### 2. The hourly analytics heartbeat — better, and not yet used

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
- **The way out, when we need better:** instrument 2. Hourly `battery_soc_pct_drop` at 0.01%
  resolution, with CPU-sleep and BLE-interval time in the same record, is both finer and
  self-explaining — it can attribute an hour's drain instead of just measuring it. That decoder is
  the highest-value piece of tooling left in this area.

## Ranked drain hypotheses

- **#0 (new, now top) The stock 4.24 base.** Morten observed much longer life on an older stock
  build; 4.30.1's own release notes claim fewer background wakeups. If stock 4.24 also drains
  ~0.7 %/h then our diff is innocent and the fix is a rebase, not an optimisation hunt. Nothing in
  the current data distinguishes "our code" from "our base".
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

Ordered by information per unit of effort. None of them needs new firmware instrumentation — the
watch already records everything (see "Measuring drain").

0. **Two zero-cost things to do alongside whatever else runs.** (a) Leave the app's battery feature
   on and check after the next SPIKE stretch whether its graph backfills the gap — settles the one
   open question the firmware can't answer. (b) Write the heartbeat-record decoder (layout generated
   from `analytics.def`); it upgrades every later experiment from one number per night to an hourly
   table with CPU-sleep and BLE-interval time beside the drain.
1. **A night on stock 4.30.1** (Morten has offered, and the app is already prompting to update).
   Decides #0 outright. If stock is ~0.15 %/h then the whole gap is ours to fix; if stock is also
   ~0.7 %/h the gap was never ours. Cost: reflashing the spike afterwards — which since v36 costs
   zero pairings, so this is cheap now. Two cautions: dump `-g 0` **before** updating, and stock
   will not have the spike's dict, so read its lines with `build/pebbleos_loghash_dict.json`.
2. **If stock 4.30.1 is good: a night on stock 4.24** (or just diff the two upstream trees for the
   wakeup change). Separates "upstream fixed it in 4.25–4.30" from "our diff". The former means
   rebase; the latter means bisect our own commits.
3. **NORMAL vs SPIKE over matched SoC ranges.** Repeat the control against a pump night that
   *starts at the same percentage*, both ≥8 h, to find out whether the 0.96 vs 0.65 split is real
   or an artifact of the model nonlinearity. This is below the resolving power of the 1%-step method
   (see accuracy), so do it with the heartbeat decoder or not at all. Only worth doing if #1/#2
   leave the pump link implicated.
4. **Outage-mode advertising (#2 lever).** Cheap to build, but only pays off on bad nights — hold
   until the baseline question is settled, then decide with real numbers on how often all-night
   outages actually happen.
5. **Idle-wakeup audit of our own code**, if #1/#2 point at our diff: what runs on a timer in
   SPIKE with no pump connected, and how often does the KernelBG/stationary service actually let
   the CPU sleep. Do this last — it is the most work and the least guided.

Not on the list any more: NOS Observation Mode (nothing to win), `ble_gap_update_params`
(mechanism refused by the pump).
