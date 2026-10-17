# Connectivity: bonds, advertising, and the dual link

How the watch is discovered and who it is bonded to: the pump's first-pair/reconnect advertising,
bond storage and pruning, and the dual link that holds the phone and the pump at the same time.
Pump *protocol* facts belong in `../Documentation/`; battery effects of the link are in
`BATTERY.md`. The single-connection slot this file used to be built around is gone since v59 — its
history and the lessons from it are kept at the end, marked as such.

## Stand-By Mode takes the pump link down

Stock PebbleOS "Stand-By Mode" (`Settings -> System`, the `stationary` service) drops the watch to
`RunLevel_Stationary` after **30 minutes without motion**, which stops Bluetooth outright: the phone
link and the pump link both go, with the pump's disconnect logged as a clean `reason=0x16`. It comes
back on any movement, and since v64 the pump reconnects about 4 s later.

For a glucose watch this is the wrong trade — the display is most useful exactly when you are lying
still — so **Stand-By Mode is off** (2026-09-06). Notes for anyone reconsidering it:

- It is a plain user setting, no firmware change needed. `stationary_set_enabled()`.
- It never engages while charging (`service.c`: `!battery_is_usb_connected()`).
- The countdown is a once-per-minute accelerometer position sample, not a vibration detector, so a
  watch sitting on a desk next to someone typing still goes stationary. The hypersensitive shake
  handler is only subscribed *after* it sleeps, which is what wakes it.
- It was engaging for only ~16 min out of each 20 h night, so the power it saved was small. If the
  drain without it turns out to matter, the middle option is teaching `RunLevel_Stationary` to keep
  Bluetooth alive so the display and sensors sleep but the pump link survives — a stock-file change
  to the runlevel table, hence not done first.

## Gotchas

- **Two BLE connections since v59 (`BLE_MAX_CONNECTIONS` 2, `CONFIG_MINIMED_SAKE`), one mode toggle:
  NORMAL⇄DUAL.** NORMAL is phone-only and a full Bluetooth stack restart (the kill switch); DUAL
  holds the phone and the pump at the same time, with the pump link *swallowed* so the firmware
  stack still sees a single connection. The pump has its own advert job, so the phone connecting no
  longer takes the pump off air. Note for the SiFli boards: they raise the connection count in their
  own syscfg.
- **Do NOT repeat the old "radio scheduling starves the pump link" conclusion — it was retracted.**
  The 0x08 supervision timeout that produced it reproduced on a single-connection build with phone
  BT off, so a second link cannot have caused it; it was the FE81/FE82 bond mismatch.
- Phone bond can loop connect/terminate(0x13) after an SM change; repeat-pairing recovery usually
  self-heals, else forget + re-pair on the phone (NORMAL mode).

- **FE81/FE82 pairing-state reconciliation.** The watch's paired flag and the pump's bond can
  disagree, and neither auto-corrects: if the watch advertises FE81 (log `adv EN FE81 …`) but the
  pump can't find it, the pump's side is unpaired → press **DOWN (forget pump)** on the watch to
  drop to FE82, then add "Mobile PB" on the pump. Conversely, a watch on FE82 that a bonded pump
  ignores means re-pair on the pump. Rule of thumb: **pump can't see watch → make both do
  first-pair (DOWN on watch + remove/add on pump).** A smarter auto-fallback (try FE82 if FE81
  draws no connection for N s) is a possible future improvement.

## Resolved: pump "device not found" on v34 (2026-07-26, fixed in v35)

After flashing v34 the pump could no longer discover the watch for first-pairing, with the watch
provably advertising correctly. v35 fixed it; kept because the technique and the lesson still apply,
and because it is the reason to read this before touching `minimed_sake_build_adv`.

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

1. **Real bug, fixed in v35: the pump-mode advert never cleared the scan response.** The pump-mode
   branch of `bt_driver_advert_set_advertising_data` set the advert data and returned early, leaving
   whatever scan response NORMAL had installed — Pebble manufacturer data (company `0x0eea`)
   carrying the watch's serial. So while impersonating a Medtronic peripheral the watch answered
   active scans by identifying itself as a Pebble. Long-standing (not a v34 regression), but wrong.
   Fixed with `ble_gap_adv_rsp_set_data(NULL, 0)` (NimBLE only rejects NULL with a nonzero length).
2. **The "Mobile Pebble" rename is reverted to "Mobile PB", and the advert bytes should now be
   treated as load-bearing.** The rename is *legal* — `Documentation/bluetooth.md` allows
   `Mobile ` + 0–7 chars, "Pebble" is 6, and the bridge's own fixture uses the equally long
   "Mobile 000001" — yet it is the only advert-payload change between the last known-good pair
   (v32) and the failure. Reverted to the exact bytes that have paired since v10. **We do not know
   which of the two changes (if either) was responsible**, because both went into v35 together —
   a deliberate trade of diagnostic precision for getting a wearable watch back. If v35 pairs, the
   clean follow-up is to re-try the rename *alone* to find out.

If it ever recurs, the candidates left unexplored were: pump-side state (a stale/hidden
paired-device entry, or the pump needing its Bluetooth or itself power-cycled), and the total advert
length having gone 22 → 26 bytes (the service UUID stays at bytes 3–6 either way, so a UUID filter
should be unaffected — but that is reasoning, not evidence).

Process note for next time: this cost a flash because a cosmetic advert change was bundled with four
functional ones, against this project's own one-change-per-flash rule. Cosmetic changes to the
advert payload are not cosmetic.

## History: the single-slot era

Before v59 the watch had one BLE connection, so the phone and the pump took turns — pump mode, or
NORMAL phone-only — with the phone winning whenever it was around. Everything above about bonds and
advertising was worked out under that constraint. Two consequences are worth remembering:

- Every flash and every watchface `.pbw` install cost a pump un-pair/re-pair, and there were no live
  logs while the pump held the slot (only the 8-line on-watch ring, read by filming the screen).
  That tax is the whole reason dual was worth building: it is an iteration-speed fix, not a feature.
  The old "the user is fine toggling instead" note was stale the moment the toggle started costing
  the dev loop.
- Rules this project learned the hard way: **one change per flash** (v34 bundled a *cosmetic* advert
  rename with four functional changes and cost a flash plus an evening — cosmetic changes to the
  advert payload are not cosmetic); **run an adversarial review of every HW-untestable change before
  flashing** (it caught a watch-hard-faulting bug twice); **prefer evidence to inference** (scanning
  the watch from a laptop with `bluetoothctl`, and running the watchface in the emulator with
  TEST_MODE, each settled in a minute what hours of guessing had not); and **compare artifact mtime
  to source mtime, not to the commit timestamp** (a confident wrong diagnosis of the watchface
  crash).

For anything involving the phone, use the USB tunnel (`adb forward tcp:9000 tcp:9000`, then
`pebble … --phone 127.0.0.1`) — the phone's IP changes with the network.

## The three blockers dual had to clear (all closed in v59, HW-verified in v62)

Researched 2026-07-26. Kept for the design detail, and because the v21/v22 attempt left no source to
read (never committed; only its `.pbz` artifacts survive), so that entry is a description to
re-implement, not code to restore. Three real blockers, in order (SPIKE was the name the pump mode
had at the time; it is DUAL now):
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

## Bond coexistence (remaining-work item 8) — DONE, HW-VERIFIED 2026-07-26 (v36)

The pump bond survives a reboot and a phone re-pair, and Settings → Bluetooth no longer lists it or
refuses to pair a phone, so a flash costs zero pairings. The checks are in `TESTING.md`.

Three holes closed together on 2026-07-26. **All three had to ship in one flash**: fixing the first
two alone leaves the watch unable to pair a phone at all.

1. `prv_collect_other_ble_bondings_itr` collected every BLE bond but the one being kept, with no
   `is_gateway` check, so every prune path deleted the pump. Both boot paths
   (`prv_load_ble_pairing_from_prf`, `prv_prune_stale_ble_bondings`) and the phone pair/re-pair
   prune funnel through that one collector, so a single guard closed the boot paths too.
2. `bt_persistent_storage_delete_ble_pairing_by_id` erased the shared-PRF pairing slot on any
   delete, so "forget pump" (the DOWN button, a routine step in the FE81/FE82 reconciliation) also
   wiped the *phone's* PRF record until the next boot. `nimble_store.c` reaches the same function,
   so a NimBLE-initiated deletion hit it too. The `is_gateway` value must be read *before* the
   delete runs, or the record is already gone.
3. Settings → Bluetooth offers pairability only when the bonded-device list is empty, and that list
   held every BLE bond. With the pump bond now permanent, the menu would have shown "Forget this
   device to pair a new device" forever and blocked phone pairing.

The `is_gateway` signal the `BtPersistBondingDBEachBLE` callback does not expose turned out **not**
to be needed: the public `bt_persistent_storage_is_ble_ancs_bonding(id)` answers it (`supports_ancs`
is set equal to `is_gateway` at store time), as long as it is called *after* the iteration returns
and not from inside the callback — see the non-recursive-mutex Gotcha. `nimble_store.c` clears
`is_gateway` for any bond formed while the watch is in the pump mode, which is what marks the pump's
bond non-gateway. The remaining bond work for dual is per-peer `is_gateway`/SM decisions, tracked
under item 5.

## Dual-link implementation notes

Three findings from the 2026-07-26 design that survive as properties of the shipped dual link:

- **The watchface loopback session opens as `TransportDestinationApp`.** A `Hybrid` loopback is a
  "system" session and `comm_session_open` closes an existing system session on open ("last system
  session to connect wins"), so it would tear down the phone's session and with it `pebble logs` and
  sideload. App destination skips the eviction block. Known degradation, accepted: the watchface's
  *outbound* messages now have two candidate fallback sessions and its ready-ping may reach the
  phone instead of the loopback, which costs the immediate push on watchface launch — see
  remaining-work item 10.
- **The SAKE GATT server has no per-connection visibility and `prv_sake_port_access` has no peer
  guard.** The Medtronic services are registered unconditionally, so under dual the *phone*
  discovers them, and a write to the SAKE port char from any connection advances the handshake
  state machine. Not observed as a problem; treat it as the known exposure of sharing one GATT
  server between the peers.
- **The pump-pairing window is implemented.** Stock strict LESC stays the default at all times;
  `minimed_sake_apply_sm_config` flips to legacy Just Works only for a user-initiated pump
  first-pair and flips back at handshake DONE, so a phone that pairs is not silently persisted as
  non-gateway.

## Phone-bond papercut — resolved (v33 + v36)

The phone's bond used to break on every pump↔phone mode cycle, which cost a re-pair each time. Two
causes, both fixed on hardware: the pump's bond pruning used to delete the phone's PRF slot (v33),
and the pump's bond was pruned on every boot (v36, the coexistence fix above). The checks in
`TESTING.md` confirmed the phone reconnects across cycles with no re-pair prompt. v24's
repeat-pairing scoping and v18's runtime SM reconfig are both still in place; the LESC
numeric-confirm prompt is the one part never independently verified (see `TESTING.md`).
