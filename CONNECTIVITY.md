# Connectivity: bonds, advertising, the single slot, and dual connection

Everything about who gets the one BLE connection and how the watch is discovered: the pump's
first-pair/reconnect advertising, bond storage and pruning, the phone-vs-pump slot contest, and
the dual-connection work that would dissolve it. Pump *protocol* facts belong in
`../Documentation/`; battery effects of the link are in `BATTERY.md`.

## Gotchas

- Single BLE connection (`BLE_MAX_CONNECTIONS 1`): phone or pump, never both — toggle between
  them (SPIKE=pump, NORMAL=phone). Dual was tried (v21/v22) and set aside. **Do NOT repeat the old
  "radio scheduling starves the pump link" conclusion — it was retracted**: the 0x08 supervision
  timeout that produced it reproduced on v23 *single-connection with phone BT off*, so a second
  link cannot have caused it (it was the FE81/FE82 bond mismatch). Dual has never been fairly
  tested. See "Dual pump+phone" in Remaining work for what it would actually take.
- Phone bond can loop connect/terminate(0x13) after the SM changes; repeat-pairing recovery usually
  self-heals; else forget + re-pair on the phone (NORMAL mode).

- **FE81/FE82 pairing-state reconciliation.** The watch's paired flag and the pump's bond can
  disagree, and neither auto-corrects: if the watch advertises FE81 (log `adv EN FE81 …`) but the
  pump can't find it, the pump's side is unpaired → press **DOWN (forget pump)** on the watch to
  drop to FE82, then add "Mobile PB" on the pump. Conversely, a watch on FE82 that a bonded pump
  ignores means re-pair on the pump. Rule of thumb: **pump can't see watch → make both do
  first-pair (DOWN on watch + remove/add on pump).** A smarter auto-fallback (try FE82 if FE81
  draws no connection for N s) is a possible future improvement.

## Known papercut: phone steals the SPIKE slot (diagnosed 2026-07-28, no firmware bug)

Toggling to SPIKE with the phone still connected does NOT hand the slot to the pump: nothing
rejects the *phone* in SPIKE (the mirror image of the v32 pump-in-NORMAL reject), the phone
re-grabs the freed slot within ~1 s, and a connected watch stops advertising — so the pump can
never get in. **The "tap disconnect in the Pebble app before toggling to SPIKE" habit is
load-bearing.** Second factor: the pump's reconnect scan backs off with outage length (observed
since v41: 1m40s and 1m48s after brief drops, 6m51s after a ~7 min outage) — after an hour away,
give it up to ~15 min with the slot actually free before suspecting anything. Diagnosis notes:
`local_addr.c "No bondings found that require address pinning!"` at SPIKE entry is normal noise
(fires on working toggles too). Proper fixes, both deferred: reject/deprioritise the phone in
SPIKE (small, v32-mirror), or dual connection (item 5) which dissolves the whole slot contest.

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

## Dual pump+phone — the three blockers (remaining-work item 5)

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

## Bond coexistence (remaining-work item 8) — DONE

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

## Phone-bond papercut (remaining-work item 7)

   bond breaks on every SPIKE→NORMAL cycle. v24 attempts a fix (scope repeat-pairing recovery to
   the pump only). If that's not enough, root-cause why the phone re-initiates pairing (diagnostic
   build). v18's SM-reconfig is also still not independently verified.
