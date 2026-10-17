# Design: phone + pump connectivity without re-pairing

Date: 2026-07-26
Area: MiniMed on-watch SAKE spike (`spike/minimed-sake`), board `asterix` (nRF52840, NimBLE)
Status: approved, not yet implemented

## Goal

Make the watch hold Bluetooth connections to **both** the Medtronic 780G pump and the phone at the
same time. If that proves too hard, the accepted fallback is one connection at a time with the
existing manual toggle — but **reconnecting to either peer must never require pairing again**.

The motivation is iteration speed, not features. Today every firmware flash and every watchface
`.pbw` install costs a full pump re-pair, and reading logs at all means filming the on-watch 8-line
ring buffer, because `pebble logs` needs a phone session that SPIKE mode cannot have. That tax is
paid on every future change to anything.

## Key finding: the two problems are separable

The per-flash re-pair is **not** caused by the single connection slot. It is caused by bond
pruning. Verified in the working tree:

- The pump bond is deleted at **every boot**, by two independent paths inside
  `bt_persistent_storage_init` (`bluetooth_persistent_storage_normal.c:1482-1495`): the SPRF replay
  (`prv_load_ble_pairing_from_prf` :409 → a gateway write → the prune at :796) and
  `prv_prune_stale_ble_bondings` (:692-707).
- The pump bond is also deleted whenever the **phone** pairs or re-pairs, via the same prune.
- Both funnel through `prv_delete_other_ble_bondings` → `prv_collect_other_ble_bondings_itr`
  (:597-621), which filters only on "not the bond being kept" and "is a BLE bond". There is no
  `is_gateway` check.

So a single collector-level fix closes all of it, with no dual-connection risk. That yields the
fallback in full, and it is a prerequisite for dual regardless. Hence two stages.

**What gets implemented from this spec: Stage 1 only.** Stage 2 is documented here because its
findings shaped Stage 1's scope and because they correct the plan currently recorded in
`PROGRESS.md`, but it gets its own spec and implementation plan once Stage 1 is hardware-verified.

## Stage 1 — non-gateway bonds become invisible to gateway machinery

One coherent idea across three sites, plus the observability needed to tell whether it worked. All
three functional edits are no-ops in stock builds, where every BLE bond is a gateway. Ships as one
flash.

### 1.1 The prune collector skips non-gateway bonds

`prv_collect_other_ble_bondings_itr` in `bluetooth_persistent_storage_normal.c` already reads
`stored_data` before deciding, so this is one added guard on
`stored_data.ble_data.is_gateway`. Skipping non-gateway bonds means a gateway (phone) write no
longer evicts the pump, and neither boot path can evict it either, because both reach the pump
through this same collector.

v33 (commit `245d8121`) already stopped the reverse direction — a non-gateway pump write evicting
the phone — by gating the prune call itself with `if (is_gateway)`. Stage 1 completes the pair.

Residual cosmetic effect: `prv_prune_stale_ble_bondings` will log `Found 2 BLE bondings` at INFO on
every boot in the new steady state, and its "keep exactly one" comment becomes stale. Update the
comment; the log line is harmless.

### 1.2 Deleting a non-gateway bond must not erase the phone's PRF slot

`bt_persistent_storage_delete_ble_pairing_by_id` (:908-914) calls
`shared_prf_storage_erase_ble_pairing_data()` unconditionally on any successful delete. So
forgetting the **pump** wipes the **phone's** PRF pairing slot. It self-heals at the next boot, but
until then a crash into PRF/recovery finds no phone pairing — and "forget pump" (the watch's DOWN
button) is a routine step in the FE81/FE82 reconciliation procedure.

Fix: only erase the shared-PRF pairing data when the bond being deleted was a gateway. Note the
ordering constraint — the function deletes first and erases second (:909-913), so the `is_gateway`
value must be captured **before** `prv_delete_ble_pairing_by_id` runs, or the record is already
gone when the question is asked.

Note the callers are broader than the Settings UI: `nimble_store.c:297`
(`prv_nimble_store_delete_sec` → `..._delete_ble_pairing_by_addr` → `..._by_id`) reaches the same
function, so a NimBLE-host-initiated pump bond deletion hits this too.

### 1.3 Settings → Bluetooth ignores non-gateway bonds

This is a trap that Stage 1 *creates*, and it must ship in the same flash. `prv_expand_cb` in
`src/fw/apps/system/settings/bluetooth.c` (:530-533) enables pairability **only when the remote
list is empty**, and the list is built from every BLE bonding via `prv_add_ble_remote`. Today the
pump bond dies at boot so the list is usually empty. Once the pump bond survives permanently, the
menu would permanently show "Forget this device to pair a new device" and you could no longer pair
a phone at all.

Fix: `prv_add_ble_remote` returns early for non-gateway bonds. The public iterator already passes
`BTBondingID *id`, and `bt_persistent_storage_is_ble_ancs_bonding(*id)` is already public and
returns `supports_ancs`, which `bt_persistent_storage_store_ble_pairing` sets equal to `is_gateway`
(:761). So no change to the `BtPersistBondingDBEachBLE` callback signature is needed. Like the
other two edits this needs no `#ifdef`: in stock every BLE bond is a gateway, so the early return
never fires. It does add one settings-file read per bonding in the menu path, which is negligible
at two bonds.

This also stops the pump being counted in the "%u Paired Phones" header.

### 1.4 Make the failure mode legible on the watch

Stage 1 must be diagnosable without a phone, because if it does not work the pump link is exactly
what is broken — so the phone-session channel is unavailable by definition. Three constraints shape
this:

- The bond pruning runs in `services_common_init` (`service.c:42`), long before the BT stack. The
  on-watch ring cannot be used there: `minimed_sake_log` needs `kernel_malloc` plus
  `launcher_task_add_callback` (`minimed_sake_spike_ui.c:67-75`).
- The ring is 8 lines and scrolls, so any boot line would be gone before it could be read.
- `PBL_LOG` needs a tethered console, which is the thing we do not have.

So the diagnostic is **live state, not history**: extend the SAKE Spike app's existing 400 ms
`prv_refresh` (`minimed_sake_app.c:29-35`) with a bond-inventory line above the log, e.g.

    MODE: SPIKE (FE81)
    bond gw1 pmp1 del0
    <the 8-line ring as today>

reading:

- `gw<N>` — number of stored **gateway** (phone) BLE bonds.
- `pmp<N>` — number of stored **non-gateway** bonds, i.e. the pump.
- `del<N>` — a static counter of non-gateway bonds deleted since boot, bumped wherever a
  non-gateway bond is actually removed.

This answers every Stage 1 question at a glance and cannot scroll away:

| What you see | What it means |
| --- | --- |
| `gw1 pmp1 del0` after a reboot | Working. The pump bond survived. |
| `pmp0 del1` | The fix did not hold — something still pruned the pump bond. |
| `pmp0 del0` | The bond was never stored in the first place; a different bug from pruning. |
| `MODE: SPIKE (FE81)` with `pmp0` | The FE81/FE82 mismatch, named directly instead of being inferred 30 s later from a `disc reason=0x08` loop. |
| `gw0` | The phone bond is gone — the v33 regression direction. |

Implementation notes: the inventory needs a small read-only accessor in
`bluetooth_persistent_storage_normal.c` that iterates the bonding file and counts by `is_gateway`
(the existing `prv_file_each` pattern); the `del` counter is a plain static, so it works at any
boot stage with no allocation and no task queue. Emit `PBL_LOG_INFO` copies of the same facts as
well, so they also show up in `pebble logs` once a phone session is available in NORMAL.

Deliberately **not** included: auto-healing the mismatch by clearing the app's paired flag at boot
when no pump bond exists (noted as "worth fixing eventually" in `TESTING.md`). That is a behaviour
change, not a diagnostic; make the state visible first.

### Explicitly out of scope for Stage 1

Persisting the SPIKE/NORMAL mode across reboot. The mode is RAM-only
(`minimed_sake_spike_ui.c:23`) and resets to NORMAL at boot, so a flash still costs one SELECT
press to return to SPIKE. That is trivial next to a re-pair, it is a second idea in the same flash,
and booting straight into SPIKE would mean booting with strict LESC off and FE81 advertising live
before the build has been confirmed sane.

### Stage 1 outcome

A flash costs: sideload, reboot, `paired (persisted): FE81`, pump reconnects on its own. No pump
re-pair and no phone re-pair, ever. The mode toggle is still needed to move the single connection
between peers, but it costs two button presses instead of a pairing dance.

## Stage 2 — dual connection

Bigger than the existing notes assume. Four findings change the plan recorded in
`PROGRESS.md` remaining-work item 5.

### 2.1 The pump-link swallow does not exist yet

The design intent is that the phone's connection routes into the Pebble firmware stack as it does
today in NORMAL, while the pump's connection stays driver-private. Only the **v32 reject** path is
swallowed today (`advert.c:179` gates on NORMAL mode only). In SPIKE the pump connection is fully
routed: `advert.c:260` calls `bt_driver_handle_le_connection_complete_event`, which sets both
single-connection booleans, logs "No intent for connection" and fires the legacy
`PEBBLE_BT_CONNECTION_EVENT` that Settings UI consumes. Dual must **build** the swallow.

The swallow must cover the **encryption-change** event, not just connect and disconnect.
`bt_driver_handle_le_encryption_change_event` dereferences `connection->is_encrypted` with no NULL
check (`gap_le_connect.c:592-593`), exactly like the disconnect path at :496-500 that produced the
v31 hard fault — and the pump link does encrypt. Other driver→firmware entry points (address
update, connection-parameter update, all `gatt.c` callbacks) are NULL-safe.

Per-connection state in the driver that is currently single-connection and must become
pump-specific: `s_sake_conn_handle` is set for *any* non-rejected connection including the phone
(`advert.c:200`); `minimed_sake_force_readvertise` terminates whichever link it holds (:52-54); the
disconnect handler unconditionally clears it and calls `minimed_sake_read_stop` (:275-276), so a
phone disconnect would today stop pump polling. The SAKE notification filter is `attr_handle`-only
and not qualified by connection handle (:435-437).

### 2.2 Refcounting the two booleans is the wrong fix

`s_is_connected` (`gap_le_advert.c:100`) and `s_is_connected_as_slave` (`gap_le_connect.c:136`) are
indeed plain booleans, but they are written **only** from stack-routed events
(`gap_le_connect.c:388-389`, :514-515). Under a correct swallow the pump never touches them, so
they stay accurate as phone-only flags with no change at all.

The real blocker is a semantic one: `gap_le_advert` enforces **advertising XOR connected** — the
cycle timer bails while connected (:257), and both `gap_le_advert_force_data_refresh` (:631) and
`bt_driver_handle_host_resynced` (:653) skip re-airing. So the moment the phone connects, the watch
stops advertising and the pump can never reconnect. That semantic has to be relaxed to "advertise
while a connection slot is free", not refcounted.

Compounding it: the Reconnection advert job that SPIKE piggybacks on is **unscheduled** when the
gateway connects (`kernel_le_client.c:439`), so the payload vehicle disappears rather than pausing.

Separately, after a swallowed pump connect the controller silently stops advertising while
`s_is_advertising` stays true (only `gap_le_advert_handle_connect_as_slave` corrects it, :589), so
the scheduler believes it is on air forever and the phone can never connect. The driver must re-arm
advertising itself after a hidden connect. The existing rejected-pump path only does this at the
pump's *disconnect* (`advert.c:265-274`).

### 2.3 There is exactly one advertising instance

`MYNEWT_VAL_BLE_EXT_ADV` and `MYNEWT_VAL_BLE_MULTI_ADV_INSTANCES` are both `0`
(`third_party/nimble/port/include/nrf52/syscfg/syscfg.h:1254`, :1295). The two payloads can only
**time-share** one instance; there is no way to radiate a Medtronic and a Pebble advert
simultaneously.

This is survivable because of the project's own v30 hardware evidence: the bonded pump reconnects
by **identity address**, ignoring the advertised payload (it handshook in NORMAL while the watch
advertised Pebble service `0xFED9`). In NORMAL the watch advertises its plain static-random
identity (`ble_hs_id_infer_auto(0)`, `advert.c:593`) — the very address the pump bonded to. So the
Medtronic payload matters mainly for the pump's **first pairing** (it scans for service class
`0xFE82`), not for reconnects. Remaining uncertainty to test rather than assume: the pump ignores
adverts slower than ~150 ms, and stock NORMAL advertising falls back to 1022 ms after 30 s.

`MYNEWT_VAL_BLE_MAX_CONNECTIONS` is `1` (:1287-1289) and must become `2`. That gate is real for the
legacy advertising path, not only the extended one: `ble_gap_adv_validate` refuses connectable
undirected advertising when `ble_hs_conn_can_alloc()` is false
(`ble_gap.c:2743`). `ble_hs_conn_can_alloc` checks the connection pool, the L2CAP channel pool and
`ble_gatts_conn_can_alloc()` (`ble_hs_conn.c:41-49`); `BLE_L2CAP_MAX_CHANS` is already
`3 * BLE_MAX_CONNECTIONS` (syscfg.h:2166) and `BLE_STORE_MAX_BONDS` is already 3.

### 2.4 The loopback session would kill the phone session

`minimed_sake_sender.c:216` opens the watchface loopback session with
`TransportDestinationHybrid`, which `comm_session_open` treats as a system session
(`session.c:173`). When a system session already exists it **closes** it — "last system session to
connect wins" (:190-196). PPoGATT implements `.close`, so opening the loopback would tear down the
phone's session, killing `pebble logs` and sideload.

The reverse direction is already safe: the loopback reports `CommSessionTransportType_QEMU`
(`minimed_sake_sender.c:115`), and `prv_find_session_is_system_filter` (`session.c:421-427`)
excludes QEMU and PULSE, so `comm_session_get_system_session()` does not see the loopback and the
phone's session opens normally.

Fix: open the loopback as `TransportDestinationApp`, which skips the eviction block entirely.
Inbound injection is unaffected because it passes the session pointer explicitly to
`comm_session_receive_router_write`. Known degradation to accept and document: the watchface's
*outbound* messages route via `prv_get_app_session` (:403-418), where both sessions now qualify as
fallbacks ("Fallback session already set!?"), so the watchface's ready-ping may reach the phone
session instead of the loopback. Consequence is the loss of the immediate push on watchface launch
— the first BG then waits for the next 60 s poll. Its ACKs are already swallowed by design.

### 2.5 Also unresolved for Stage 2

- **`apply_sm_config` is global and mode-keyed** (`minimed_sake_spike_ui.c:91`). NimBLE reads
  `ble_hs_cfg.sm_*` live when building the pairing request/response
  (`ble_sm.c:1645-1646`, :1660-1661, :1680-1682), so with both peers connectable the wrong config
  can be live when a peer initiates pairing. Preferred resolution: a **pump-pairing window** —
  keep the stock strict-LESC config as the default at all times and flip to legacy Just Works only
  during an explicit user-initiated pump first-pair, then flip back. This relies on a bonded FE81
  reconnect reading none of `ble_hs_cfg.sm_*`, which the v33 investigation claimed but which is
  **not independently verified**; verify before relying on it.
- **The `is_gateway` decision in `nimble_store.c:258` is also global-mode-keyed**, so a phone that
  pairs while the watch sits in SPIKE is silently persisted as non-gateway. Same per-peer fix.
- **Single shared GATT server.** The Medtronic services are registered unconditionally at init, and
  NimBLE has no per-connection service visibility, so under dual the phone will discover the
  Medtronic services on the watch. Check whether the SAKE characteristic write handler rejects
  non-pump writers.
- **Cold-boot classification.** The pump identity is RAM-only (`s_pump_id_addr`, captured at
  handshake DONE), so nothing distinguishes pump from phone before the first handshake of a boot.
  Persisting the pump identity may become necessary.

### Stage 2 probe scope

The only genuine unknown is whether this radio holds two concurrent links. Scope the first Stage 2
flash to answering exactly that: `BLE_MAX_CONNECTIONS` 1→2, the pump-link swallow (connect,
disconnect, **encryption change**), driver-side re-arming of advertising after a swallowed connect,
and relaxing the advertising-XOR-connected gate. Test one question: *can both links be up at once,
and does the pump still complete SAKE?* Leave the SM per-peer work and the loopback session change
out of that probe unless they block it.

## Risks and fallbacks

Stage 1's risk is that a permanently-present pump bond exposes further single-bond assumptions
beyond the three sites above. Every one found so far is one `is_gateway` / `supports_ancs` check
away, the inventory line in 1.4 distinguishes "pruned" from "never stored" without a second flash,
and the fallback is reflashing v35 (`build/sake-spike-v35-scanrsp-and-name-revert.pbz`).

Stage 2's risk is concentrated in the swallow: an incompletely swallowed event is a **watch hard
fault**, not a degradation. An adversarial review before flashing is mandatory — it has caught
exactly this class of bug twice (v31, and the carried-over v32 logic in v34).

Two known-broken things are **not** caused by this work and must not be mistaken for regressions:
the watchface crashes on launch under v34/v35, and the v34 "device not found" pairing failure whose
v35 fix bundled two changes so the responsible one is still unknown. Both are documented in the
OPEN sections of `PROGRESS.md`. The watchface crash is deliberately parked; dual is what makes it
cheap to diagnose.

## Test plan

Follow the discipline in `TESTING.md`: judge pump presence only by SAKE-specific log lines
(`HANDSHAKE OK`, `discovering CGM svc`, `polling BG`), never by `connected`, which fires for any
device; remember the on-watch log is an 8-line ring buffer whose stale lines do not count; and run
about five cycles rather than one, deliberately provoking bad timing, because the historical bugs
were all timing-dependent.

**Stage 1** — three decisive checks, no new tooling needed:

1. **Reboot survival.** With the pump paired, power-cycle the watch. Open the SAKE Spike app and
   read the inventory line first — `gw1 pmp1 del0` is the pass. Then expect `paired (persisted):
   FE81` and, after the SELECT press into SPIKE, a pump reconnect and `HANDSHAKE OK!` with no
   re-add on the pump. This is the check that proves the tax is gone.
2. **Phone re-pair survival.** Forget the watch on the phone and pair it again in NORMAL, then
   check the inventory line still reads `pmp1 del0` before toggling to SPIKE. The pump must
   reconnect without a re-pair. This is the direction v33 did not fix.
3. **Settings pairability.** With the pump bond present, open Settings → Bluetooth. The pump must
   not appear as a row, the header must not count it as a paired phone, and pairing a new phone
   must still be offered (no "Forget this device to pair a new device" when only the pump is
   bonded).

Also confirm the flash itself got cheaper end to end: sideload → reboot → SELECT → BG and IOB back,
with zero pairing actions on either device.

**Stage 2 probe** — the pass condition inverts the old v27 "decisive test": the SAKE-specific lines
must appear **while** `conn phone m=N` is simultaneously up and stays up. Capture the `prm` line for
both links, since two live connections mean two connection-parameter sets and the pump link already
runs at latency 0 around the clock (the leading battery suspect). Confirm `pebble logs --phone
127.0.0.1` over the USB tunnel works while the pump link is live — that is the whole point of the
exercise.

Host tests (`tools/minimed_sake_hosttest/`, currently 49/49) stay green throughout as a regression
net for the crypto, IOB and graph modules. They do not cover any code touched by Stage 1.

## Documentation follow-ups

Per the project's split: bridge/firmware behaviour goes in `PROGRESS.md` and `TESTING.md`; anything
learned about how the **pump** communicates goes to the OpenMinimed `Documentation/` repo. If the
Stage 2 probe establishes whether the pump reconnects to a plain identity address as readily as to
an RPA, and at what advertising interval, that is a protocol fact and belongs in
`Documentation/bluetooth.md`.
