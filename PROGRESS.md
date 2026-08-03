# On-Watch MiniMed → Pebble SAKE Spike — Progress & Notes

Run the MiniMed pipeline on the Pebble 2 Duo directly, no phone. Spinoff of the working
phone-bridge project (`../minimed-pebble-bridge`). This file is the stable map: current state,
how to build and test, the code map, and what is left. Topic detail lives in its own file.

| File | What is in it |
|---|---|
| [`BATTERY.md`](BATTERY.md) | Drain measurements, how to run a battery test, ranked hypotheses, remaining experiments |
| [`CONNECTIVITY.md`](CONNECTIVITY.md) | Bonds and pruning, advertising, the single-slot contest, dual connection |
| [`WATCHFACE.md`](WATCHFACE.md) | The launch-crash saga (dormant) and the launch-gap gotcha |
| [`TESTING.md`](TESTING.md) | The build/flash/test loop, pump and watchface test procedures |
| [`VERSIONS.md`](VERSIONS.md) | Every build vN, newest first, plus the chronological dev log |
| [`../Documentation/`](../Documentation/) | Pump protocol source of truth — update eagerly |

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

> **Battery is the current fight, and the first decisive number is in.** A night on stock 4.31.1
> (2026-08-02/03) drained **0.165 %/h** against **0.57–0.72 %/h** for our firmware in nearby SoC
> bands — roughly 3.5–4×. Combined with Morten's experience of good battery on older stock builds,
> that puts the cost in **our diff**, not in the 4.24 base we forked from and not in the hardware.
> v44 adds hourly `hb` log lines (drain at 0.01% resolution plus CPU-sleep, per-task CPU,
> advertising and connection-interval seconds, flash writes) so the next spike night can say *what*
> costs it. Everything battery: [`BATTERY.md`](BATTERY.md).

- **End-to-end PROVEN on real HW (2026-07-21):** advertise as "Mobile PB" → pump connects → SAKE
  handshake → GATT-client CGM read → decrypt → continuous auto-updating BG in mmol/L, matching the
  pump's display exactly. Feasibility fully settled; the rest is productization.
- Reconnect **HW-VERIFIED 2026-07-22** (v16): after a NORMAL⇄SPIKE toggle the pump reconnected by
  itself, re-ran the handshake, BG resumed.
- **ON THE WATCH NOW: v41 pump status — HW-VERIFIED 2026-07-28, 23 h soak**
  (`build/sake-spike-v41-pump-status.pbz`). Full bridge mirror on the watchface: BG + IOB +
  status band (suspend/temp-target/warm-up countdowns, BG "---" blanking) — details in the
  version log. Carries v40 pump push (HW-verified, 10 h soak). Everything v36 verified still
  holds: pump pairs, **neither bond is ever lost** — reboot and phone re-pair both verified.
  The watchface launch crash did NOT reproduce on v36+ (see OPEN→DORMANT below).

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

### After-the-fact logs from a SPIKE session (`tools/dump_flash_logs.py`)

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

- It needs `libpebble2`, which lives only in the pebble-tool venv, so the script re-executes
  itself with that interpreter when the import fails — any `python3` works. `pyelftools` was
  added to the same venv for the dehasher.
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


## Code map (branch `spike/minimed-sake`; all committed, nothing pushed)

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
  `pebble-glucose-watchface/docs/PEBBLE_GLUCOSE_PROTOCOL.md`.
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

Topic-specific ones have moved: the watchface launch-gap crash is in
[`WATCHFACE.md`](WATCHFACE.md); the single-slot rule, the FE81/FE82 pairing-state reconciliation
and the phone-bond loop are in [`CONNECTIVITY.md`](CONNECTIVITY.md).

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

## Remaining work (usability order)

1. ✅ **Reconnect — HW-verified 2026-07-22** (v16); `Documentation/bluetooth.md` updated. Paired
   flag persisted in v17. Later maybe: watchdog if advertising ever stalls.
2. **Real watchface — WORKING (v20, HW-verified): live BG on the actual watchface, screen clean
   (no "not connected").** No connection-event hack needed. Next display work is additive keys:
   IOB/status/graph. Design: firmware injects **local AppMessages** into the existing
   `../pebble-glucose-watchface`, unmodified — same watchface for phone-bridge and on-watch modes.
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
3. **IOB — DONE, HW-VERIFIED (v30, 2026-07-24). Pump status — BUILT in v41 (2026-07-27),
   awaiting HW.** IOB reads via IDD SRCP `0x03F3`→`0x03FC`, watchface key 14. Status reads IDD
   Status `0x102` + TAS `0x03FD` and drives watchface key 15 with the bridge's full label set +
   countdowns — see the v41 version-log entry. Reservoir IU is parsed and flash-logged but not
   displayed (bridge parity).
3b. ✅ **Event-driven push instead of the 60 s poll — DONE, HW-VERIFIED 2026-07-27 (v40, 10 h
   soak).** Stage C (act on more bits: therapy/status/fingerstick) remains open and overlaps
   item 3. The researched facts below are the reference material behind the v40 design.
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
5. **Dual pump+phone — never fairly tested, three known blockers.** Full detail, including why the
   old "radio scheduling starves the link" verdict was retracted, in
   [`CONNECTIVITY.md`](CONNECTIVITY.md). Demoted since the flash-log dump gave us after-the-fact
   SPIKE logs without it.
6. **Battery — the current job.** Stock 4.31.1 drains ~3.5–4× less than our firmware over matched
   SoC bands, so the cost is in our diff. v43/v44 log the hourly metrics that can attribute it.
   Numbers, method, confounds and remaining experiments: [`BATTERY.md`](BATTERY.md).
7. **Phone-bond papercut** (re-pair dance between test cycles) — see
   [`CONNECTIVITY.md`](CONNECTIVITY.md).
8. ✅ **Full phone+pump bond coexistence — DONE, HW-VERIFIED 2026-07-26 (v36).** A flash now costs
   zero pairings. Detail in [`CONNECTIVITY.md`](CONNECTIVITY.md).

## References

- `../Documentation/` — protocol source of truth. **Update eagerly** with confirmed facts.
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` — the working Kotlin impl to
  port (peripheral, SAKE, reads, history parser) + tests.
- `../xdrip-pebble-archive/PythonPumpConnector/PythonSake/pysake/` — readable SAKE reference +
  public test vectors (`constants.py`).
- Do NOT comment on GitHub/upstream — Morten does that himself (plans to update PebbleOS #853).
