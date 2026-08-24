# Version log & history

Every spike build, newest first, then the chronological dev/test log. This is a reference
archive — you rarely need it in context. Current state is in `PROGRESS.md`; the per-topic
files are listed there.


- v53 (2026-08-22, BUILT, awaiting flash; `build/sake-spike-v53-alert-text.pbz`): notification
  text tweak only (no logic change from v52). Title = "MiniMed"; body = alert name with the BG
  in parens, e.g. "Alert before low (4.2)" ("LO"/"HI" for off-scale), or just the name when the
  pump has no current glucose. (v52 had put the alert name in the title and "BG N.N" in the body
  with no source label.) Host tests 109/109.
- v52 (2026-08-22, BUILT, superseded by v53; `build/sake-spike-v52-alert-notif-final.pbz`): **alert
  UI settled: standard notification only, with the BG in it.** The v51 Quick View experiment is
  removed after a day of wear: it WORKED (banner over the watchface, watchface shifts up, and —
  correcting the v51 entry — the back button dismisses it just like the notification), but
  rendering both was redundant and the pin's fixed 15-min window is a worse fit for alarms than
  the notification's dismiss + history. Decision: standard notifications. **To change our minds
  later:** the whole working pin implementation is the v51 diff of
  `src/fw/popups/minimed_alert_popup.c` (`git show`, ~20 lines: ongoing persistent pin via
  `timeline_add`); only re-adding that block is needed. New notification content: title = the
  alert name (pump wording), body = the latest BG ("BG 4.2" / "BG LO"; omitted while the pump
  has no valid glucose) — the CGM read dispatches before the annunciation read on the same push,
  so the number is usually seconds old. Host tests 109/109 (no parser change).
  HW checklist: next alert shows "Alert before low" / "BG N.N" as a notification, no banner.
- v51 (2026-08-19, BUILT, awaiting flash; `build/sake-spike-v51-alert-quickview.pbz`): **alert
  Quick View experiment** — each pump alert now ALSO inserts an ongoing timeline pin (15 min,
  persistent, `from_watch`), which Timeline Quick View shows as a bottom banner over the
  watchface; the full-screen notification (and its vibe) is unchanged, so both render and Morten
  picks a winner. Needs Quick View enabled on the watch (Settings → Timeline). Also: 0x325
  renamed to the pump's wording "Alert before low" (HW-confirmed 2026-08-19), and the icon
  attribute now uses `add_resource_id` (kills the benign per-alert `attribute.c:432` warning).
  Host tests 109/109. HW checklist: (1) on the next alert, dismiss the full-screen notification —
  the banner should sit at the bottom of the watchface for 15 min (watchface content shifts up);
  (2) the pin also appears in the Timeline app until it ages out; (3) verdict: banner vs
  notification vs both, then strip the loser.
- v50 (2026-08-18, BUILT, pushed to phone Download; `build/sake-spike-v50-opstate-trigger.pbz`):
  status reads now also trigger on push bit 1 (Operational State Changed). The 2026-08-18
  reservoir-change capture got only two status lines (bits 0/16 were the only triggers), missing
  the Preparing/Priming walk PR #1 claims — Documentation PR #1 is closed pending a dense
  re-capture with this build. Bit 2 deliberately not added (rides every microbolus). Caveat: a
  state shorter than the indication→read round-trip (~1-3 s) can still be missed, and a burst of
  bit-1 pushes can coalesce into one read. Carries v49. Host tests 108/108.
  HW checklist: next reservoir change should log a status line per o-transition
  (96→55→5a→66→96 expected, order TBD) with fl/res captured at each step.
- v49 (2026-08-17, BUILT, awaiting flash; `build/sake-spike-v49-iob-log.pbz`, adb push pending —
  phone was disconnected): one flash-log line, no behavior change: `SAKE: IOB N mu` (raw
  milliunits) on every successful SRCP 0x03FC parse in `prv_parse_iob`. The value previously went
  only to the on-watch UI ring log, so flash dumps had pushes-with-bit-17 but no IOB values
  (confirmed in the 2026-08-17 g0-v46 capture). Purpose: minute-cadence IOB traces in routine
  dumps, to empirically recover the pump's active-insulin decay curve (true-IOB investigation —
  fit the decay after a big bolus in a window with no follow-on boluses). Carries v48 unchanged.
  HW checklist: (1) `SAKE: IOB N mu` appears ~every minute in a flash dump and matches the
  watchface IOB to 0.1 U; (2) after a meal bolus, the trace shows the step jump and ~2 h decay.
- v48 **HW-VERIFIED 2026-08-19** (flashed as part of v49; g0-v49 dump in
  `../logs/from-watch/2026-08-19-g0-v49-alerts-test.txt`): two real alerts decoded over ~23 h,
  both `type=0x325` = ALERT_BEFORE_LOW_SG ("Low predicted"), each right after a falling BG —
  correct records, correct name, both landed in the watch's notification list with the right
  timestamps. **The popup/vibe was suppressed by watch-side Quiet Time** (storage + event chain
  are upstream of that gate, so the list still filled) — QT off now; popup+vibe still to be
  seen live. Open HW questions answered: the pump ACCEPTS the open-ended range max (no
  `IDD RACP resp` errors) and the ATT-MTU framing rule holds (no `bad hist rec`). 10/10
  reconnect baselines fired. Known-benign wart: each notification logs
  `attribute.c:432 ... uint32 for non-uint32_t` (IconTiny wants add_resource_id; stock HRM popup
  does the same). Watch-side "Mute All"/Quiet Time gate these alerts like any notification;
  phone-side Pebble-app muting does not (never traverses the phone).
- v48 (2026-08-17, BUILT, awaiting flash; `build/sake-spike-v48-pump-alerts.pbz`, adb push
  pending): **pump alarms as native watch notifications.** On a 0x101 annunciation push (bit 3)
  the watch reads the new IDD history records (IDD RACP 0x2A52 + History Data 0x108, both newly
  discovered/subscribed), decodes Annunciation Consolidated (0xf010) events
  (`minimed_annunciation.{c,h}`, pure + host-tested), maps the type code to a name
  (PythonPumpConnector's AnnunciationType; unknown codes show "Pump alert 0xNNN"), and posts a
  native notification (`minimed_alert_popup.{c,h}` → `notifications_add_notification`): popup,
  vibe, notification list, watch-local dismiss. Raise-only (cleared events skipped); silenced
  raises (event-flag bit 6, e.g. night mode) are skipped to mirror the pump's own alerting; dedup
  per annunciation instance id. Baseline "report last record" per connection (never notifies), so
  alarms raised while disconnected are dropped by design. Host tests 108/108.
  HW checklist: (1) baseline line `SAKE: annunc baseline seq=N` after connect; (2) trigger an
  alert (easiest: a Personal Reminder on the pump, or wait for a real one) → notification pops
  with the right name + `SAKE: annunc type=...` in flash logs; (3) no re-buzz when the same alarm
  re-logs (status change/clear); (4) the catch-up RACP request uses an open-ended max seq
  (33 5a 0f lo..ffffffff) — if the pump rejects it, expect `IDD RACP resp ...` in the ring log and
  no notification: then the range needs a real upper bound (report-number-of-records first);
  (5) a silenced-hours alert (night mode almost-low) should log `sil=1` and NOT notify — this also
  verifies the ALERT_SILENCED bit on HW; (6) fragment framing: `bad hist rec` lines would mean the
  ATT-MTU record-delimiting rule is wrong on this link.
- v47 (2026-08-17, BUILT for tomorrow's reservoir-change capture;
  `build/sake-spike-v47-reservoir-battery.pbz`, adb push pending — phone was disconnected):
  two logging additions to verify Documentation PRs #1 and #2, no behavior change.
  (1) The IDD Status flags byte (bit 0 = reservoir attached) now parses into
  `MinimedIddStatus.flags` and prints as `fl=` in the `SAKE: status` line — PR #1's
  Reservoir Attached claim was unverifiable without it. (2) Hourly plaintext read of the pump's
  SIG Battery Level 0x2a19 (`SAKE: pump battery N pct`; read-by-UUID, no discovery-chain change,
  bypasses the SAKE serialiser) — re-gathers PR #2's coarse-battery evidence, whose bridge-era
  log died with the app uninstall. Host tests 94/94. Reservoir-change expectation per PR #1:
  t 55→33 (Stop, not 3c Pause), o 96→55→5a→66→96, fl bit 0 dropping then rising at priming.
- v46 **HW-VERIFIED 2026-08-16 through a full sensor change** (19:52 CHANGE SENSOR → transmitter
  charge → 20:18 warm-up → 22:16:56 first reading, 155 mg/dL at offset 120 = warm-up estimate
  right again at 1 h 59 m). v45 marker fix verified: the CHANGE SENSOR 0-record with a fresh
  offset was skipped (`CGM edge rec 0e c3 00 00 bc 20 side=0`), no 0.0, no graph point. Capture
  findings (now in `../Documentation/cgm-service.md`): **zero SFLOAT sentinels the whole arc** —
  change-sensor and all of warm-up send plain 0 mg/dL records, new-session offsets restart near
  zero, and no records flow while the transmitter charges; so the sentinel branch
  (`SG: no value`) may be dead code on the 780G. Sensor Connectivity showed an undocumented
  **bit 3** (conn 0x0b right after removal — signal-lost bit 2 only joined ~6 min later, 0x0f —
  cleared at reconnect); this change was a sensor-death one and Morten was slow to swap, so the
  pump fell out of SmartGuard (shield=03 throughout, BG REQUIRED + fingerstick after warm-up) —
  bit 3 may relate to that, unconfirmed. Push bits at the transitions: removal 22+21, reconnect
  22 alone, warm-up start 21, then 20/27 minutes later and 26 only at first readings — order of
  20/26/27 differs from the bridge-era capture; 25/30 still never fired.
- v46 (2026-08-16, BUILT for the sensor-change capture; `build/sake-spike-v46-sensor-change-capture.pbz`,
  pushed to phone Download — flash this instead of v45, it contains it): two flash mirrors in
  `prv_parse_and_show` so a dump can verify the Documentation `sensor-change` branch claims:
  `SAKE: BG new <N> mg/dL offset=<M>` on every genuinely new reading (when readings resume after
  warm-up, previously ring-only), and `SAKE: CGM sentinel rec ...` raw bytes when the record
  decodes to an SFLOAT sentinel (which sentinel warm-up/charging uses is undocumented; also shows
  whether records flow at all while the transmitter charges). No behavior change over v45.
- v45 (2026-08-16, BUILT, awaiting flash; `build/sake-spike-v45-sg-markers.pbz`, pushed to phone
  Download): **0 mg/dL marker handling** — fixes the false "0.0" BG + 0-point graph cliff seen
  on HW 2026-08-16 during SG-below-range (09:15, msg=09) and "sensor updating" (18:10, msg=02).
  Root cause: those states deliver plain 0 mg/dL CGM records with advancing time offsets (not
  the SFLOAT sentinels), so they counted as real new readings (now in
  `../Documentation/cgm-service.md`). New behavior: 0 mg/dL is a marker, never shown or graphed
  as a number. Off-scale (msg=09/0x0A) shows **LO**/**HI** as the BG with a fresh timestamp,
  graphs at the scale edge (50/400 mg/dL = 2.8/22.2), and the LOW/HIGH status band is gone (it
  only repeated the BG and covered the graph). Any other state skips the record entirely — BG
  goes stale with a climbing age and the band explains why; this also closes the hole where a
  *new* 0-record bypassed the `bg_invalid` "---" blanking (e.g. during CHANGE SENSOR). HI is an
  assumption (no HIGH capture yet — pump may send 0 or a real 400+); every new 0-or-≥400 record
  flash-logs `SAKE: CGM edge rec ... side=N` so a future capture can correct it. Onset race:
  the CGM read runs before the status read on the same push, so the first off-scale record can
  lag one cycle (≤5 min, skipped-not-zero) before LO/HI appears. New host checks for the
  off-scale accessors/empty band (94/94). Changes: `minimed_sake_read.c` (marker branch +
  SG_FLOOR/CEILING), `minimed_status.{c,h}` (`minimed_status_sg_below/above()`, LOW/HIGH compose
  to ""), tests, docs.
  **HW checklist:**
  1. Flash, SPIKE while the pump shows a sensor state (CHANGE SENSOR now): band shows the state,
     BG stays stale/`---` — no 0.0, no new graph points, `SG: 0 marker, skip` in the ring and
     `SAKE: CGM edge rec ... side=0` in flash (if 0-records still flow in this state).
  2. Next real LOW: BG shows `LO` fresh-stamped, graph hugs the bottom at 2.8, no LOW band,
     flash has `CGM edge rec ... side=1`.
  3. Someday HIGH: check flash `CGM edge rec` lines to confirm or correct the 0-assumption.
  Fallback: reflash v44 (heartbeat logging, pre-marker behavior).
- v41 (2026-07-27, **ON THE WATCH, HW-VERIFIED 2026-07-28 — all three checklist items passed in a
  23 h soak**; `build/sake-spike-v41-pump-status.pbz`): **pump status on the watchface — the full
  bridge mirror** (remaining-work item 3). Soak evidence (flash dump, 1398 pushes, zero
  timeouts/errors/fallbacks): suspend + temp-target reacted within seconds of the push and
  cleared on revert; a real sensor change ran the whole arc — `CHANGE SENSOR` (BG blanked with
  the reason shown) → `SAFE BASAL` → `WARM-UP 2:00` counting down → band cleared at 1 h 59 m,
  i.e. the self-timed countdown was accurate to one minute. Bits 25/30 never fired — not even
  through a physical sensor change (~1400 indications, zero hits; other pump situations remain
  unsampled) — recorded in `../Documentation/idd-service.md`; the sensor-change wishlist items
  are all retired.
  Reads IDD Status `0x102` (encrypted GATT read — the serialiser's first read-shaped op) and Get
  Therapy Algorithm States (SRCP `0x03FD`→`0x03FE`) as a pair on connect, on fallback polls, and
  on push bits 0/16; maps them through the bridge's iterated priority chain to the watchface
  status band (key 15, already on the watchface since the bridge era): SUSPENDED (count-up) /
  LOAD RESERVOIR / LOW / HIGH / sensor family / WARM-UP with the self-timed 2 h countdown /
  CALIBRATE / BG REQUIRED / SMARTGUARD OFF / SAFE BASAL / TEMP TARGET (countdown) / "" normal.
  Countdowns tick via a 60 s local re-send (AppMessage only, no BLE). When status says the pump
  has no current glucose (GST signal lost / warm-up family), BG blanks to "---" immediately and
  stale re-polls are suppressed until a genuinely new reading. New pure module
  `minimed_status.{c,h}` (29 new host checks, 89/89 total — incl. the TAS flag-gated field order
  and the entry-only warm-up stamping the bridge needed field iteration to learn). New log
  vocabulary: `st: <label>` / `st: (normal)` (ring), `SAKE: status t=.. o=.. conn=.. msg=..
  res=..`, `SAKE: tas auto=.. shield=.. ready=.. tt=..`, `SAKE: status label '..' bg_invalid=..`
  (all three PBL_LOG → flash-visible), `st read err/rc`, `st decrypt failed`, `TAS bad resp`.
  Failure shape: either read failing just skips its clauses; both failing keeps the last label;
  no status char = no status line; BG/IOB never blocked.
  **HW checklist:**
  1. Flash, SPIKE: expect `st: (normal)` (or the current state) within ~2 s of `polling BG + IOB`,
     and the flash log's `SAKE: status ...`/`SAKE: tas ...` lines showing sane fields
     (t=55 RUN, o=96 READY, shield 02 auto-basal in normal operation).
  2. Provoke: suspend the pump briefly → watch shows `SUSPENDED 0:00` within ~seconds (push bit 0),
     counting up each minute; resume → band disappears. A temp target set on the pump → `TEMP
     TARGET H:MM` counting down (bit 16).
  3. Next sensor change: warm-up should show `WARM-UP 1:59`→counting down ~2 h, BG showing `---`
     with the band explaining why (this retires the sensor-change-warmup wishlist item).
  Fallback: reflash v40 (BG/IOB/push, no status line).
- v40 (2026-07-27, **ON THE WATCH, HW-VERIFIED — 10 h workday soak, all checklist items passed**;
  `build/sake-spike-v40-pump-push.pbz`): **pump push — event-driven reads via IDD Status Changed
  `0x101`**. Soak evidence (flash dump, 07:42–17:54): **569 indications, zero inter-arrival gaps
  over 6 min** (the fallback never had cause to fire), zero decrypt failures, zero unexpected
  responses, pump link up all day. Reset Status response confirmed on HW: `03 03 0c 03 0f` =
  Response Code + echoed `0x030C` + Success (now in `../Documentation/idd-service.md`). A 4 IU
  lunch bolus produced 9 indications at exactly 2 s spacing (bits 17+6+2, final one +7) — the
  serialiser sustained back-to-back pushes (12 pairs arrived 0 s apart) and the watch tracked
  each 0.5 IU step live. Flags histogram: ~137 CGM-family (≈4.5 min cadence), 195 IOB-only,
  ~135 reservoir/history (SmartGuard microbolus heartbeat). **Flash-dump gotcha discovered:**
  `minimed_sake_log` ring lines (`rst resp`, `fallback poll`, `op timeout`, `push mode`) do NOT
  go to flash — only `PBL_LOG` does — so a future soak that needs those in the dump must add
  `PBL_LOG_DBG` mirrors. Original design/build notes follow. (remaining-work
  item 3b Stage B; spec `docs/superpowers/specs/2026-07-27-pump-push-design.md`, plan
  `docs/superpowers/plans/2026-07-27-pump-push.md`). Ported from the bridge: bit 18 → CGM read,
  bit 17 → IOB read, then **Reset Status (SRCP `0x030C` + the accumulated flag union)** after
  every indication so the latches re-arm. The three exchanges (CGM, IOB, reset) are serialised
  behind a pending-mask dispatcher (one in flight; writes always issued off a callout so they
  can't overtake an indication confirmation; 10 s op timeout unwedges a lost indication; the
  reassembly buffers now reset at issue time, closing the overlap-truncation hazard). On the
  first real indication the 60 s poll becomes a **6-min dead-man fallback** (the bridge's tuned
  number), re-armed by every indication and completed CGM exchange — so a missing `0x101` char,
  failed subscribe, or silently dead push all degrade to a poll, never to "no data". Freshness
  win only (a reading reaches the watchface ~1 s after the sensor produces it, and BG age is now
  accurate to ~1 s); NOT battery — see item 6. New pure module `minimed_idd_flags.{c,h}`
  (parse + Reset-operand encode, continuation bits recomputed for mixed-width unions); host
  tests 60/60. Log vocabulary: `0x101 <16hex>` (flags), `push mode (6m fallback)` (first
  indication), `rst resp <n>:<hex>` (Reset Status response — **record these bytes**, expected
  `03030c03<result>`, then document in `../Documentation/idd-service.md`), `fallback poll`
  (push went quiet), `op timeout 0xNN`, `SRCP unsolicited`.
  **HW checklist:**
  1. SPIKE session: `polling BG + IOB` → `0x101 sub rc=0` → first `0x101 …` ~1 s later →
     `push mode (6m fallback)` → reads → `rst resp …`.
  2. **The decisive check: a second `0x101` ~5 min later** (v39's latch made that impossible),
     with BG following within seconds.
  3. Overnight soak: indications all night, `fallback poll` rare/absent, watchface BG age
     staying under ~1 min.
  Fallback if it misbehaves: reflash v39 (passive push, 60 s poll).
- v39 (2026-07-26, **ON THE WATCH; overnight results now in item 3b** — Stage A answered NO, latch
  confirmed strict): v38 + **passive
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


---

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
- v16 soaked clean all day (a few disconnects, always self-recovered).

## v17–v20 — watchface display + the first-pair-address bug (2026-07-22 evening)

Goal this session: show BG on the *real* `minimed-pebble-watchface`, plus DX/pairing polish.

- Two research agents mapped (a) the inbound AppMessage seam and (b) the watchface protocol
  (UUID `567a3f6e-…`, Pebble Glucose Protocol keys). Chosen approach: firmware injects local
  AppMessages via a phone-less loopback CommSession (QEMU-transport pattern) — watchface unmodified.
- **v17:** persisted pump-paired flag (settings file) + watchface local-sender (BG → AppMessage).
- **DX:** `spike-build.sh` (build+version+adb-push in one) and `TESTING.md` written. User's real
  pain was re-pairing, not file copy.
- **v18:** phone-bond fix — discovered NimBLE reads `ble_hs_cfg.sm_*` at runtime (old "compile-time,
  unfixable" assumption was wrong), so NORMAL keeps stock strict LESC and SPIKE flips to legacy JW.
- **v19:** tried faking `PEBBLE_BT_CONNECTION_EVENT` to clear the watchface "not connected" banner.
- **The evening's rabbit hole:** v16→v17 needed a pairing resync (persisted flag empty → FE82 vs
  pump's FE81); then a mirror mismatch (watch FE81 vs pump unpaired) fixed by DOWN=forget. Then
  after clearing bonds, **first-pair stopped working entirely — pump couldn't discover the watch**
  (pure discovery failure, nothing after `adv EN FE82`). v19's connection event was suspected but
  the code showed no advertising-disable path.
- **Root cause (v20):** v15 had made SPIKE *always* advertise an RPA (needed for reconnect), which
  dragged first-pair onto an RPA too — and the pump frequently can't discover an RPA first-pair
  advert. v10–v14 used a plain address and paired reliably. **Fix: address type follows pairing
  state** — plain (`t1`) for first-pair FE82, RPA (`t3`) for reconnect FE81. v19's connection-event
  hack reverted (confound; and unnecessary).
- **v20 HW-verified:** `adv EN FE82 t1` → pump found + paired; reconnect (`t3`) works (1–2 min pump
  latency); **real watchface shows live BG ("6.2"), screen clean, no "not connected."** Milestone:
  the on-watch pipeline drives the actual watchface, no phone.
- Committed as a checkpoint (`d066acc3` tooling+docs, `7a5d44ec` code). Nothing pushed.
- Lesson recorded: stop stacking unverified changes; one change per flash, verify, then next.
