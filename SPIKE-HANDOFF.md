# On-Watch MiniMed → Pebble SAKE Spike — Handoff

Handoff notes for continuing the "run the MiniMed pipeline on the Pebble 2 Duo, no phone" spike.
Written for a fresh Claude session (or future me). The auto-memory `onwatch-spinoff` has the terse
running log; this file is the detailed operational guide.

## Goal

Cut out the Android phone bridge and have the **Pebble 2 Duo talk to the Medtronic MiniMed 780G
directly** over BLE (advertise as the pump's peripheral, do the SAKE handshake, read glucose), all
on the watch via modified PebbleOS firmware. Spinoff of the working phone-bridge project
(`../minimed-pebble-bridge`).

## STATUS: Spike 1 DONE (proven on real hardware). Now in Spike 2.

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
4. **Distinct filenames:** `./waf bundle` always emits the same name. Morten wants versioned names
   → `cp build/normal_asterix_v4.24.0-dirty.pbz build/sake-spike-vN-<desc>.pbz` and send that.
   Last shipped: **v9** (`sake-spike-v9-toggle-order.pbz`). The last one flashed & tested was v8.
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

**Test procedure (once v9+ is flashed, order no longer matters):**
1. Open SAKE Spike app → SELECT → `MODE: SPIKE`.
2. Turn phone Bluetooth **off** (frees the single BLE connection slot for the pump).
3. On the pump: add/pair a new device → select **"Mobile PB"**.
4. Read the on-watch log: `advertising`/`adv EN …fast` → `connected` → `paired/encrypted` →
   `subscribed` → `wakeup notify rc=0x0000` → `wrote 20:…` (pump's first SAKE frame) → currently
   `disc reason=0x13` (no crypto yet).
5. Toggle back to NORMAL to restore the phone link.

## Code changed (branch `spike/minimed-sake`, committed locally — NOT pushed)

New files:
- `src/bluetooth-fw/nimble/minimed_sake_service.{c,h}` — the SAKE Port GATT server (svc 16-bit
  `0xFE82`; char `0000fe82-0000-1000-0000-009132591325` = Medtronic **vendor** base, Write+Notify),
  the required **custom Device Information service** on vendor UUID `0x0900` (9 std DIS chars w/
  placeholder values), `minimed_sake_build_adv()` (advert payload), and the **deferred wake-up**
  (ble_npl_callout, ~120ms after subscribe → send 20 zero bytes).
- `src/fw/popups/minimed_sake_spike_ui.{c,h}` — spike **core**: on-watch log ring buffer + the
  runtime **mode flag** (`minimed_sake_get_mode`/`toggle_mode`), `minimed_sake_log`,
  `minimed_sake_spike_report`. (Not a popup anymore despite the dir.)
- `src/fw/apps/system/minimed_sake_app.{c,h}` — the "SAKE Spike" launcher app (viewer + SELECT
  toggle).

Modified:
- `src/bluetooth-fw/Kconfig` — adds `config MINIMED_SAKE_SPIKE`.
- `src/bluetooth-fw/nimble/wscript_build` — compiles `minimed_sake_service.c` when the flag is on.
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
- `src/fw/shell/normal/system_app_registry_list.json` — registers the app (id `-200`, enum
  `MINIMED_SAKE`, `ifdefs: [CONFIG_MINIMED_SAKE_SPIKE]`).
- `third_party/nimble/port/include/nrf52/syscfg/syscfg.h` — **SM/pairing config** (spike defaults):
  `BLE_SM_SC_ONLY 0`, `BLE_SM_LEGACY 1`, `BLE_SM_IO_CAP NO_INPUT_OUTPUT`, `BLE_SM_MITM 0`. Needed
  because the pump pairs **legacy Just Works** and does NOT support LE Secure Connections. (One
  permissive config serves both the phone (SC) and the pump (legacy).)

> Committed on branch `spike/minimed-sake` (single spike commit). Nothing is pushed anywhere.

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

## SPIKE 2 — what to do next (the actual remaining work)

Implement the SAKE handshake in C. **Develop and byte-verify it on the laptop first** (plain gcc, no
watch), then integrate into the firmware — so there's no reflash-per-crypto-tweak loop.

1. **Get the reference + test data.** The working bridge wraps `org.openminimed:javasake`
   (GPL-3.0, GitHub `OpenMinimed/JavaSake`) — source/keys are NOT in the tree. First check the
   bridge's own SAKE tests for embedded key databases + handshake vectors:
   `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/src/test/.../ble/sake/MedtronicSakeSessionTest.kt`,
   `.../ble/connection/{ConnectionTestSupport,SakeHandshakeDriverTest}.kt`,
   `.../tools/medtronic-ble-spike/.../{SakeSpikeHarness.java,MedtronicSakeSpikeTest.java}`.
   If the keys + expected bytes are there, you can build+verify the C port entirely locally.
   Otherwise fetch JavaSake and/or capture a real handshake from the running bridge.
2. **Port to C** (a self-contained module, unit-tested on the host): AES-128 ECB + AES-CMAC + AES-CTR
   (reuse a small impl or NimBLE's), `SeqCrypt`, and the 6-message **server-role** state machine
   (watch = server/`MOBILE_APPLICATION`, pump = client). Reference impl to mirror:
   `../minimed-pebble-bridge/glycemicgpt/.../ble/sake/MedtronicSakeSession.kt` +
   `.../connection/SakeHandshakeDriver.kt`. Verify byte-identical to JavaSake before touching HW.
3. **Integrate into firmware:** drive it from `prv_sake_port_access` (the SAKE-port WRITE handler in
   `minimed_sake_service.c`) — feed each pump write into the state machine, notify back each server
   reply. Reuse the deferred-notify path. Then the handshake should complete on-watch (watch for a
   `handshake complete` log instead of `disc 0x13`).
4. **After the handshake:** the encrypted session (SeqCrypt) is up → port the reads (IDD/CGM,
   history backfill) from the bridge's medtronic module, and surface BG to a watchface. That's the
   rest of the on-watch pipeline.

## Reference material on disk

- `../Documentation/` — protocol source of truth (sake.md, bluetooth.md, app-services.md,
  pump-services.md, idd-service.md, cgm-service.md, key_databases.md). **Keep it updated** with the
  hardware-confirmed facts above.
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` — the working Kotlin
  implementation to port (peripheral, SAKE, reads, history parser) + its tests.
- `PebbleOS/` (this repo) — the firmware; spike on branch `spike/minimed-sake`.
- Do NOT comment on GitHub / upstream — Morten writes those himself (he intends to update
  coredevices/PebbleOS issue #853 with the findings).
