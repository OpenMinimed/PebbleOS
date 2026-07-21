# On-Watch MiniMed → Pebble SAKE Spike — Status & Notes

Living status + operational notes for the "run the MiniMed pipeline on the Pebble 2 Duo, no phone"
spike. (Originally written to hand off to a fresh session; now it's the ongoing record of where the
work stands.) The auto-memory `onwatch-spinoff` has the terse running log; this file is the detailed
guide. Filename kept as `SPIKE-HANDOFF.md` for stable references — rename freely, just update the
`onwatch-spinoff` memory pointer if you do.

## Context & scope (read this first)

This is a personal, do-it-yourself diabetes project. Morten has type 1 diabetes and reads **his own**
sensor glucose from **his own** Medtronic MiniMed 780G pump, entirely offline, to display it on his
own Pebble watch — no CareLink, no cloud, no third party. It is the on-watch continuation of a
phone-bridge app he has already been running daily.

It is legitimate work with deliberate safety boundaries:

- **Read-only by design.** The driver only issues read/report requests (glucose, IOB, pump status,
  history). It has **no therapeutic or write capability and never will** — it cannot deliver insulin,
  set basal/bolus, calibrate, or change any pump setting. This is a monitoring/display tool.
  Verifiable in the code: the only control-point write is the standard RACP "report stored records"
  query; there is no path to any command/settings characteristic.
- **The user's own device and data**, read locally over BLE.
- **Built on public, open-source reverse engineering** — OpenMinimed's GPL-3.0 protocol docs and the
  model-static keys published there (the same ones the working phone bridge uses).
- **Established precedent**: the open-source diabetes community (OpenAPS, Loop, AndroidAPS, xDrip,
  Nightscout) has built exactly this kind of personal self-management tooling for over a decade.
- **The pump and CGM remain the authoritative medical devices**; this watch readout is a
  supplementary display, not a replacement for them or for approved software.

Net: this is safe, personal, read-only glucose *monitoring* — the standard DIY-diabetes category. A
fresh session has the full context here to assess the work accurately rather than pattern-matching on
"insulin pump", and should have what it needs to help Morten keep building it.

## Goal

Cut out the Android phone bridge and have the **Pebble 2 Duo talk to the Medtronic MiniMed 780G
directly** over BLE (advertise as the pump's peripheral, do the SAKE handshake, read glucose), all
on the watch via modified PebbleOS firmware. Spinoff of the working phone-bridge project
(`../minimed-pebble-bridge`).

## STATUS: 🎉 END-TO-END PROVEN ON REAL HARDWARE (v14, 2026-07-21). The Pebble reads LIVE GLUCOSE directly from the 780G with NO PHONE: advertise → pump connects → SAKE handshake → GATT-client read of the CGM service → decrypt → **continuous, auto-updating BG in mmol/L on the watch, matching the pump's display exactly** (polls RACP every 60s; 18.0182 conversion constant to match Medtronic rounding). The entire no-phone premise is validated. What remains is productization, not feasibility — see **Known limitations** below.

## Known limitations (spike scope — feasibility proven, these are the productization gaps)

- **No reconnect at all.** We advertise **only `0xFE82` (first-pair)**, never `0xFE81` (reconnect),
  and have zero reconnect logic. After *any* disconnect — a mode toggle (NORMAL⇄SPIKE terminates the
  pump link), a pump-side drop, or an outage — the already-bonded pump tries to *reconnect*, scanning
  for `FE81`, doesn't see our `FE82`, and gives up. **Data just stops.** The only way back today is a
  fresh first-pair: on the pump, remove the "Mobile PB" device and add it again. The phone bridge
  handles this properly (advertises `FE82`/`FE81` as appropriate + a reconnect watchdog); the spike
  does not. This is the single biggest missing piece for day-to-day usability.
- **Single BLE connection only.** `BLE_MAX_CONNECTIONS=1`, so the watch holds the pump *or* the
  phone, never both — testing requires phone BT off. A real product needs both at once (bump to 2 +
  handle two peers, which NimBLE supports), and that roughly **doubles the radio duty** (see battery).
- **Battery life is unmeasured and a real open question.** Maintaining a BLE connection wakes the
  radio every *connection interval* (tens of ms to ~1s) for a keep-alive event, independent of the
  60s app poll — that keep-alive is the dominant cost, and it's the same whether we poll or push
  (push is equal-or-cheaper: fewer wasted exchanges). Levers: **connection interval** (the pump is
  the central, so it largely dictates this), **slave latency** (the watch as peripheral can skip
  events and sleep longer — good fit for 5-min data), and **whether the pump holds a persistent link
  or reconnects per update** (unknown; measurable and swings the answer a lot). A *second* continuous
  connection (phone + pump) on a small watch battery could be the limiting factor for the whole idea.
  Needs real multi-hour measurement on the watch before anyone claims all-day battery.
- **Phone-bond papercut** (re-pair dance every test cycle) — see Gotchas; separate, non-trivial.

**Proven on a real Pebble 2 Duo + real 780G pump:** the watch advertises as a Medtronic peripheral
("Mobile PB"), the pump connects *into* it, bonds (legacy Just Works), discovers the GATT server,
subscribes to the SAKE characteristic, receives the watch's wake-up notification, and **writes its
first SAKE handshake frame** (`wrote 20:00 00 00 00`). i.e. the inverted BLE topology + pairing +
handshake-initiation all work on a Pebble. **The "no phone" approach is feasible — no doubt left.**

After the pump's first write it disconnects (`disc reason=0x13`) because the watch has **no SAKE
crypto yet** — it must now reply with handshake msg `0_s` and run the 6-message exchange. That is
Spike 2.

## Hardware / firmware facts

- **Pebble 2 Duo** = firmware board **`asterix`** (`boards/asterix/defconfig`:
  `CONFIG_PLATFORM_FLINT`, `CONFIG_SOC_NRF52` + nRF52840). B&W 144x168, 1-bit.
- BLE stack = **NimBLE** (`src/bluetooth-fw/nimble/`). Host + controller both on the nRF52840.
- Pebble Time 2 = `obelix`, Pebble Round 2 = `getafix` (both SF32LB52/SiFli) — same NimBLE host, so
  the code ports, but the BLE controller differs → re-verify on that silicon before trusting it.

## Build & flash workflow (operational — this is the stuff that's annoying to rediscover)

No local nix/arm-gcc. Build in Docker (both docker & podman present on this machine).

1. **Reusable build image `pebbleos-build:local`** already exists (3.58GB). It's
   `ghcr.io/coredevices/pebbleos-docker:v6` + `pip install -r requirements.txt` baked in via
   `docker commit`. If it's gone, recreate: run the base image as root with the repo mounted,
   `pip install -r requirements.txt`, `docker commit <container> pebbleos-build:local`.
2. **Submodules** must be checked out (they were empty on clone). Needed ones are initialized;
   **skip `third_party/hal_sifli/SiFli-SDK`** (huge, only for obelix/getafix). If missing:
   `git submodule update --init --recursive --depth 1 <paths>` for freertos, hal_nordic/nrfx,
   nimble/mynewt-nimble, cmsis_core, picolibc, nanopb, memfault, nonfree/pebbleos-nonfree, moddable,
   tinymt, qr_code_generator, speex, **resources/iconography** (this last one is needed or the
   resource build dies with `resource_generator_pdc.py ... NoneType.abspath`).
3. **Build + bundle** (run as your uid so files aren't root-owned):
   ```sh
   docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
     -v "$PWD":/pebbleos -w /pebbleos pebbleos-build:local bash -lc '
       git config --global --add safe.directory /pebbleos
       export PATH=/opt/pebbleos-sdk/arm-none-eabi/bin:$PATH
       ./waf configure --board asterix -DCONFIG_MINIMED_SAKE_SPIKE=y   # only needed once / after Kconfig|registry changes
       ./waf build
       ./waf bundle'
   ```
   Output: `build/normal_asterix_v4.24.0-dirty.pbz` (+ `build/pebbleos.elf`). Build ~a few seconds
   incremental. FW ~833KB, ~182KB flash free.
4. **Distinct filenames:** `./waf bundle` always emits the same base name. Morten wants versioned
   names → `cp build/<the-just-written>.pbz build/sake-spike-vN-<desc>.pbz` and send that. **NOTE:**
   now that the spike branch has a commit, `git describe` changed the bundle name to
   `normal_asterix_v4.24.0-1-g80bcc143-dirty.pbz` — copy from *that* (check `ls -t build/*.pbz`),
   NOT the old `normal_asterix_v4.24.0-dirty.pbz` which is a stale v9 artifact.
   Last shipped: **v14** (`sake-spike-v14-mmol-fix.pbz` — mmol/L rounding fix: use **18.0182** mg/dL
   per mmol/L, not the textbook 18.0156, so the rounded value matches the Medtronic pump's own
   display; the bridge's GlucoseFormat uses the same constant for the same reason. Fixes a v13
   symptom where the watch read e.g. 5.6 while the pump showed 5.5 at ~100 mg/dL). v13 = continuous
   auto-updating BG via 60s RACP polling (HW-proven: values tracked the pump). v12 first live BG,
   v11 GATT-client discovery, v10 handshake — all HW-proven 2026-07-21.
5. **Flashing = BT sideload via the Pebble mobile app** (no dev kit — kit is out of stock and
   requires opening/sacrificing a watch). Sideload needs a working phone↔watch Pebble link, so do
   it while the watch is in **NORMAL mode** (see below): Pebble app → Settings → Show debug options
   → Devices → the watch → Firmware Update Debug → Sideload FW → pick the `.pbz`.
6. **Recovery** if a build misbehaves: factory reset (reverts to old stock firmware) or PRF recovery
   mode. PRF is a separate slot a normal sideload can't touch, so bricking is very unlikely.

## The interactive spike firmware (how to drive it)

Everything is gated behind Kconfig **`CONFIG_MINIMED_SAKE_SPIKE`** (off by default). When on:

- Boots in **NORMAL mode** = an ordinary Pebble (advertises as Pebble, connects to phone, sideload
  works normally). No boot-time weirdness.
- A **"SAKE Spike" launcher app** shows a live on-watch debug log + current mode.
  - **SELECT** (middle-right button) toggles **NORMAL ⇄ SPIKE**.
  - **Back** exits to the launcher (never rebound — system menus always reachable).
- In **SPIKE mode** the watch advertises as the Medtronic peripheral "Mobile PB" (fast interval) and
  hosts the SAKE + custom-DIS GATT services.

**Test procedure (v14):**
1. Open SAKE Spike app → SELECT → `MODE: SPIKE`.
2. Turn phone Bluetooth **off** (frees the single BLE connection slot for the pump).
3. On the pump: add/pair a new device → select **"Mobile PB"**.
4. Read the on-watch log — full current flow: `advertising`/`adv EN …fast` → `connected` →
   `paired/encrypted` → `subscribed` → `wakeup notify rc=0x0000` → `PUMP WROTE!`/`wrote 20:…` →
   `sent reply (st1/3/5)` → **`HANDSHAKE OK!`** → `discovering CGM svc...` → `CGM svc S-E` →
   `chr 2aa7/2aa8/2a52 h=..` → `CGM feat …` → `polling BG every 60s` → **`*** BG N.N mmol/L ***`**
   (auto-updates every 60s, matches the pump's display).
5. Toggle back to NORMAL to restore the phone link.

> **Reconnect caveat (see Known limitations):** each SPIKE session is ONE pairing. A NORMAL⇄SPIKE
> toggle, a pump drop, or an outage kills the link and it will NOT come back on its own (we advertise
> `FE82` first-pair only, never `FE81` reconnect). To resume, re-pair fresh on the pump: remove the
> "Mobile PB" device and add it again.

## Code changed (branch `spike/minimed-sake`; Spike 1 committed, Spike 2 uncommitted working tree — NOT pushed)

New files:
- `src/bluetooth-fw/nimble/minimed_sake_crypto.{c,h}` + `minimed_sake_aes.{c,h}` — the **SAKE
  server-role handshake + session cipher in C** (AES-128, AES-CMAC, AES-CTR, SeqCrypt, key-DB
  parse, 6-message state machine). Self-contained (no firmware deps), byte-verified on the host —
  see the Spike 2 section + `tools/minimed_sake_hosttest/` (`make run`, 24/24).
- `src/bluetooth-fw/nimble/minimed_sake_service.{c,h}` — the SAKE Port GATT server (svc 16-bit
  `0xFE82`; char `0000fe82-0000-1000-0000-009132591325` = Medtronic **vendor** base, Write+Notify),
  the required **custom Device Information service** on vendor UUID `0x0900` (9 std DIS chars w/
  placeholder values), `minimed_sake_build_adv()` (advert payload), the **deferred wake-up**
  (`s_notify_co` callout, ~120ms after subscribe → 20 zero bytes), and now the **handshake driver**:
  embeds+parses `KEYDB_PUMP_EXTRACTED`, re-inits the server on subscribe, feeds each pump write into
  `sake_server_handshake`, and defer-notifies each reply (`prv_defer_notify`, 30ms) → completes the
  handshake on-watch (`MinimedSakeStageHandshakeComplete` = "HANDSHAKE OK!"). RNG = `ble_hs_hci_rand`.
  Also exposes `minimed_sake_decrypt` (client-direction SeqCrypt, for the read layer) and calls
  `minimed_sake_read_start(conn)` on `SAKE_RESULT_DONE`.
- `src/bluetooth-fw/nimble/minimed_sake_read.{c,h}` — the **post-handshake CGM read (watch as GATT
  client)**. On `HANDSHAKE OK`, deferred ~250ms, then: `ble_gattc_disc_svc_by_uuid(0x181F)` →
  `ble_gattc_disc_all_chrs` (captures Measurement `0x2AA7`, Feature `0x2AA8`, RACP `0x2A52` value
  handles) → read CGM Feature (plaintext) → subscribe Measurement (CCCD = val_handle+1, notify) +
  RACP (CCCD = val_handle+1, indicate) → **poll** RACP report-last-record (`01 06`) every 60s via
  `s_poll_co`. Inbound pump notifications reach `minimed_sake_read_handle_notify` (called from
  advert.c's NOTIFY_RX): Measurement (SAKE-encrypted) → `minimed_sake_decrypt` → reassemble by the
  record's size-field (byte 0) → parse SFLOAT glucose at offset 2 → mmol/L (÷**18.0182**, integer
  round-half-up) → `*** BG N.N mmol/L ***`; RACP indication (plaintext `06 00 01 01`) = success
  (silent). `minimed_sake_read_stop` halts polling on disconnect. **RACP control-point traffic is
  plaintext; only the Measurement records are encrypted** → the read path needs decrypt only, not
  encrypt. CCCD = val_handle+1 is assumed from the observed regular 3-handle char layout; if a future
  device differs, add `ble_gattc_disc_all_dscs` to find the `0x2902` descriptors.
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — spike **core**: on-watch log ring buffer + the
  runtime **mode flag** (`minimed_sake_get_mode`/`toggle_mode`), `minimed_sake_log`,
  `minimed_sake_spike_report`. (Not a popup anymore despite the dir.)
- `src/fw/apps/system/minimed_sake_app.{c,h}` — the "SAKE Spike" launcher app (viewer + SELECT
  toggle).

Modified:
- `src/bluetooth-fw/Kconfig` — adds `config MINIMED_SAKE_SPIKE`.
- `src/bluetooth-fw/nimble/wscript_build` — compiles `minimed_sake_service.c` + `minimed_sake_crypto.c`
  + `minimed_sake_aes.c` + `minimed_sake_read.c` when the flag is on.
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — added `MinimedSakeStageHandshakeComplete`
  ("HANDSHAKE OK!" + double vibe).
- `src/bluetooth-fw/nimble/init.c` — registers the SAKE service in `bt_driver_start()`.
- `src/bluetooth-fw/nimble/advert.c` — the meat:
  - `bt_driver_advert_set_advertising_data`: in SPIKE mode substitute the Medtronic advert
    (flags + 16-bit svc `0xFE82` + manufacturer data company `0x01F9`, payload `0x00`+"Mobile PB"+`0x00`);
    NORMAL falls through to the real Pebble advert.
  - `bt_driver_advert_advertising_enable`: in SPIKE mode **clamp interval to 100-140ms** (pump
    ignores >150ms; the reconnect job defaults to ~1022ms) and re-assert the Medtronic payload.
  - `minimed_sake_force_readvertise()`: terminate the active link (→ re-advertise) or, if none,
    directly restart fast Medtronic advertising (makes toggle order not matter).
  - conn-handle tracking; connect/enc/disconnect/subscribe → `minimed_sake_*` logging incl. the
    disconnect reason; repeat-pairing auto-recovery re-enabled for the spike (`#if … ||
    defined(CONFIG_MINIMED_SAKE_SPIKE)`) so bond mismatches self-heal instead of looping.
  - **NOTIFY_RX**: in SPIKE mode, pump notifications/indications are routed to
    `minimed_sake_read_handle_notify` first (returns true if it owns the handle) before the normal
    Pebble GATT routing — this is how the CGM Measurement/RACP data reaches the read layer.
  - disconnect also calls `minimed_sake_read_stop()` so polling doesn't fire on a dead link.
- `src/fw/shell/normal/system_app_registry_list.json` — registers the app (id `-200`, enum
  `MINIMED_SAKE`, `ifdefs: [CONFIG_MINIMED_SAKE_SPIKE]`).
- `third_party/nimble/port/include/nrf52/syscfg/syscfg.h` — **SM/pairing config** (spike defaults):
  `BLE_SM_SC_ONLY 0`, `BLE_SM_LEGACY 1`, `BLE_SM_IO_CAP NO_INPUT_OUTPUT`, `BLE_SM_MITM 0`. Needed
  because the pump pairs **legacy Just Works** and does NOT support LE Secure Connections. (One
  permissive config serves both the phone (SC) and the pump (legacy).)

> Branch `spike/minimed-sake`: Spike 1 is the committed spike commit (`80bcc143`); the Spike 2
> handshake port + firmware wiring are **uncommitted working-tree changes** on top of it. Nothing
> pushed anywhere.

## Protocol facts learned / confirmed on hardware (update `../Documentation/` with these!)

- **SAKE is purely symmetric AES** — AES-128 ECB (session-key derivation, permit decrypt), AES-CTR
  (payload "SeqCrypt"), AES-CMAC (auth tags). **No ECDH / no public-key.** (`Documentation/sake.md`.)
- **UUID bases** (from the bridge's `MedtronicProtocol.kt`): SIG `-0000-1000-8000-00805f9b34fb`;
  Medtronic **vendor** `-0000-1000-0000-009132591325` (variant `0000`, NOT SIG `8000`). SAKE
  service = SIG `0xFE82`; SAKE characteristic = **vendor** `0xFE82`. Getting the char base wrong =
  pump finds the service but not the characteristic (fork issue #844).
- **Advertising:** pump scans for 16-bit service class `0xFE82` (first-pair) / `0xFE81` (reconnect);
  name "Mobile xxxx" carried in **manufacturer data** (company `0x01F9`), not the GAP name. Pump
  **ignores adverts slower than ~150ms** — must advertise fast.
- **Pairing:** legacy **Just Works**, IO cap **NoInputNoOutput**; pump requests MITM but doesn't
  support Secure Connections, so it falls back to Just Works.
- **The pump won't pair unless the peripheral exposes the custom `0x0900` (vendor) Device
  Information service** — the standard `0x180A` alone is not enough.
- **SAKE init sequence** (`sake.md`): (1) client/pump enables notifications on the SAKE char;
  (2) server/watch sends 20 zero bytes as a notification — **must be deferred a beat after the
  subscribe, not sent synchronously in the subscribe callback**, or the pump misses it;
  (3) pump writes 20 zero bytes back (← we reached HERE); (4) server sends `0_s`, then the 6-message
  handshake runs. Keys per device-type pair (pump type 1 ↔ mobile/secondary-display type 4/8),
  DB format in `Documentation/key_databases.md`.

## Gotchas

- **Single BLE connection slot** (`BLE_MAX_CONNECTIONS 1`). Phone and pump can't both be connected
  → phone BT off to test the pump. Fine for the "no phone" goal.
- **Phone bond flakiness:** the legacy-pairing SM change can disturb the phone's bond → a
  connect/terminate(`0x13`) loop. v8+ re-enabled repeat-pairing auto-recovery to self-heal. If it
  still loops: on the phone, forget the watch and re-pair fresh (NORMAL mode); factory reset last.
- **clangd noise:** the editor floods advert.c / the spike files with "file not found" / unknown-type
  errors because clangd lacks the NimBLE include paths. Ignore — trust the `./waf build`.
- **Don't `docker system prune`** without asking — Morten's machine has ~100GB of other images.

## SPIKE 2 — status: steps 1-4 ALL DONE & HW-PROVEN (handshake + live BG on the watch). History (below, "after the handshake") is the remaining read; see "Remaining work" for the full forward list.

Implement the SAKE handshake in C. **Develop and byte-verify it on the laptop first** (plain gcc, no
watch), then integrate into the firmware — so there's no reflash-per-crypto-tweak loop.

1. ✅ **Reference + test data — all found on disk, no fetch needed.** The full reference is the
   `pysake` package at `../xdrip-pebble-archive/PythonPumpConnector/PythonSake/pysake/`
   (`server.py`, `session.py`, `seqcrypt.py`, `keys.py`, `constants.py`) — readable Python, cleaner
   to port than the JavaSake JAR (which is only in the gradle cache as a binary). Test vectors are
   public OpenMinimed constants: `constants.py` has `KEYDB_PUMP_EXTRACTED` + `__PUMP_TEST_MSGS_1`
   (the real `780g_pairing_with_mobile` capture: 6 handshake messages + the RNG values the phone
   chose, so the server output is reproducible byte-for-byte). The bridge's own copies of the same
   vectors are in `SakeTestVectors.java` / `MedtronicSakeSessionTest.kt`. (The other two captures
   `__PUMP_TEST_MSGS_2/3` are from different pumps and have no matching key DB on disk — only
   trace 1 is end-to-end verifiable.)
2. ✅ **C port written + byte-verified.** Self-contained module in the tree, no firmware deps
   (only `<stdint/stddef/string.h>`), cross-compiles clean for Cortex-M4 (`-Werror`, ~3.2 KB total):
   - `src/bluetooth-fw/nimble/minimed_sake_aes.{c,h}` — AES-128 block enc/dec (tiny-AES derived).
   - `src/bluetooth-fw/nimble/minimed_sake_crypto.{c,h}` — AES-CMAC, AES-CTR, `SeqCrypt`, key-DB
     parse (+CRC32), and the **server-role** 6-message state machine (`sake_server_handshake`:
     feed each 20-byte pump write, get the 20-byte reply or DONE). Plus `sake_encrypt_for_pump` /
     `sake_decrypt_from_pump` for the post-handshake read layer.
   - Host test: `tools/minimed_sake_hosttest/` (`make run`). Compiles the **same** module source
     (single source of truth) with `-DSAKE_TEST_HOOKS`. **24/24 checks pass:** AES vs FIPS-197,
     CMAC vs RFC 4493, then the captured 780G trace driven through the state machine —
     **msg0/msg2/msg4 byte-identical to the capture, handshake completes to stage 6 with the permit
     verified**, session key `99ab5c7c…` matches pysake, and SeqCrypt is byte-identical to pysake's
     (interop-checked) incl. multi-frame sequence + tamper rejection. Not yet wired into firmware,
     so `./waf build -DCONFIG_MINIMED_SAKE_SPIKE=y` compiles these as unused objects (the wscript
     glob picks them up when the flag is on; they're excluded when it's off).
3. ✅ **Wired into firmware AND PROVEN ON REAL HARDWARE (v10, 2026-07-21).** On-watch log reached
   `HANDSHAKE OK!` against the real 780G — pump wrote non-zero msg1/msg3/msg5, watch replied
   st1/st3/st5 (all `notify rc=0x0000`), no more `disc 0x13`. `minimed_sake_service.c` drives the
   state machine:
   - `KEYDB_PUMP_EXTRACTED` embedded as `s_pump_keydb_bytes[]`, parsed once into `s_keydb` at
     `minimed_sake_service_init` (logs + disables handshake if CRC/length ever fails).
   - RNG = `ble_hs_hci_rand` (the NimBLE host CSPRNG, already linked; used by the SM for pairing
     randoms) via the 3-line `prv_rng` adapter — the fixed test queue is host-test-only, never shipped.
   - `minimed_sake_handle_subscribe` re-inits the server each subscription (pump restarts from
     stage 0 every pairing attempt) then defers the 20-zero wake-up (unchanged 120 ms).
   - `prv_sake_port_access` feeds each pump write into `sake_server_handshake`; on `SAKE_RESULT_MSG`
     it stages the reply through the **generalized deferred-notify** (`prv_defer_notify` → the same
     `s_notify_co` callout the wake-up now uses; 30 ms) so replies never race the ATT write-response;
     on `SAKE_RESULT_DONE` it reports the new `MinimedSakeStageHandshakeComplete` (on-watch
     "HANDSHAKE OK!" + double vibe); on `SAKE_RESULT_ERR` it logs `sake ERR (stN)`. The wake-up is
     NOT fed into the state machine — only pump writes are.
   - New on-watch log lines to watch for in SPIKE mode: `subscribed` → `notify rc=0x0000` (wake-up)
     → `PUMP WROTE!` + `wrote 20:00 00 00 00` → `sent reply (st1)` → `notify rc=…` →
     `wrote …`/`sent reply (st3)` → `sent reply (st5)` → **`HANDSHAKE OK!`** (replacing the old
     `disc reason=0x13`). If it still drops: read the last `sake ERR (stN)` / the `disc reason` to
     see which stage failed. *(All confirmed working on HW 2026-07-21 — this whole flow ran clean.)*
4. **After the handshake:** the SeqCrypt session is up (`sake_encrypt_for_pump` /
   `sake_decrypt_from_pump`) → port the reads (IDD/CGM, history backfill) from the bridge's
   medtronic module, and surface BG to a watchface. That's the rest of the on-watch pipeline.

## Remaining work — where a fresh session picks up

Feasibility is fully proven (live BG on the watch, no phone). Everything below is porting
already-working bridge code onto the now-proven on-watch transport, or polish. Rough usability order:

1. **Reconnect (highest impact — see Known limitations).** Advertise `0xFE81` (reconnect) when the
   pump is already bonded, not just `0xFE82` (first-pair), and let the existing
   subscribe→handshake→read flow re-run (it already re-inits the SAKE server per connection, and the
   handshake is redone fresh each time — no session resume — so reconnect ≈ "advertise FE81 + our
   current flow", without tearing the bond). The pump reconnects to a **resolvable private address**
   (RPA): see `bt_driver_id_generate_private_resolvable_address` (id.c). Port the FE82/FE81
   advertising-mode switching + reconnect watchdog from the bridge:
   `../minimed-pebble-bridge/glycemicgpt/.../ble/connection/{MedtronicBleConnectionManager,AndroidMedtronicPeripheral}.kt`.
   Protocol: `Documentation/bluetooth.md`.
2. **Nicer display / start of a real watchface.** Today it's the 8-line debug log
   (`minimed_sake_app.c` + `minimed_sake_spike_ui.c`). Want a big current-BG number + trend arrow +
   staleness/age. The **trend** is already in the CGM record — we parse glucose at offset 2 but skip
   the trend SFLOAT; see the flag-driven optional fields in the bridge's `CgmMeasurement.parse`. Age
   = now − last-reading time. (No button-scroll on the log yet either, if that helps debugging.)
3. **IOB + pump status.** Port `IddStatusReader` (IOB, active basal, therapy state, reservoir,
   battery). These use the IDD SRCP/SOCP control points whose **requests are SAKE-encrypted**, so
   this is the first real use of `sake_encrypt_for_pump` (ported + host-verified, not yet exercised
   on HW). Ref: `.../ble/read/{IddStatusReader,IddStatus,IddFeatures}.kt`, `Documentation/idd-service.md`.
4. **History / graph backfill.** Multi-record RACP over IDD History → the 184-MTU chunked-streaming
   path (`Documentation/gatt-streaming.md`, note the chunk-index + request-next protocol). Ref:
   `.../ble/read/{HistoryReader,MedtronicHistoryParser}.kt` + `MedtronicSessionReader.reportRecords`.
5. **Dual pump+phone connection** — bump `BLE_MAX_CONNECTIONS` to 2, handle two peers (battery
   implications in Known limitations).
6. **Battery measurement** and **7. phone-bond papercut** — see Known limitations / Gotchas.

The C SAKE module already exposes both directions: `sake_decrypt_from_pump` (used by CGM reads) and
`sake_encrypt_for_pump` (needed for the IDD requests above), and `sake_server` holds the live session.
Any crypto change: re-run the host harness `tools/minimed_sake_hosttest/` (`make run`, 24/24) before
reflashing. Build/flash workflow + the versioned-`.pbz` gotcha are in the build section above; last
shipped is **v14**.

## Reference material on disk

- `../Documentation/` — protocol source of truth (sake.md, bluetooth.md, app-services.md,
  pump-services.md, idd-service.md, cgm-service.md, key_databases.md). **Keep it updated** with the
  hardware-confirmed facts above.
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` — the working Kotlin
  implementation to port (peripheral, SAKE, reads, history parser) + its tests.
- `PebbleOS/` (this repo) — the firmware; spike on branch `spike/minimed-sake`.
- Do NOT comment on GitHub / upstream — Morten writes those himself (he intends to update
  coredevices/PebbleOS issue #853 with the findings).
