# PebbleOS

PebbleOS is the operating system running on Pebble smartwatches.

## Organization

- `docs`: project documentation
- `drivers`: hardware drivers
- `fw`: firmware source
- `resources`: firmware resources (icons, fonts, etc.)
- `sdk`: application SDK generation files
- `subsys`: OS subsystems, e.g. logging
- `tests`: tests
- `third_party`: third-party code in git submodules, also includes glue code
- `tools`: a variety of tools or scripts used in multiple areas, from build
  system, tests, etc.
- `tools/libs`: Python packages used in multiple areas, e.g. log dehashing,
  console, etc.
- `tools/libs/pbl-cli`: the `pbl` developer CLI
- `tools/cmake`: generators the CMake builds shell out to
- `sdk/waftools`: waf plugins bundled into the SDK app developers build with

## Documentation

Contributor documentation lives in `docs/` (published at
https://pebbleos-core.readthedocs.io). Prefer pointing to or extending those
pages over duplicating knowledge here: `docs/development/contributing.md`
(DCO, commit and AI-usage rules), `docs/development/pbl.md` (the `pbl`
CLI, and how to extend it), `docs/development/sdk_export.md` (SDK export
machinery), `docs/development/qemu.md` (emulator workflow, summarised
below), `docs/development/integration_tests.md` (pytest suite on QEMU and
devices).

## Code style

- clang-format for C code
- ruff for Python code
- Keep code comments short and concise. Extended descriptions can be kept in
  the Git commit message.
- Do not put references to issues in the code, only add those to the Git commit message.

## Logging

- `PBL_LOG_WRN` / `PBL_LOG_ERR` are for warnings and errors — use them as
  the names suggest.
- Default to `PBL_LOG_DBG` for routine lifecycle / state-transition logs.
  Reserve `PBL_LOG_INFO` for events that genuinely warrant attention in a
  default-level log capture; if a code path can fire repeatedly under
  normal use (e.g. play/pause spam, frequent state changes), it must not
  log at INFO.

## Firmware development

- Configure: `pbl configure --board BOARD_NAME`

  - Board names can be obtained from `pbl configure --help`
  - `-DCONFIG_RELEASE=y` enables release mode
  - `-DCONFIG_MFG=y` enables manufacturing mode
  - `--variant=normal|prf` selects build variant (default: normal)

- Build firmware: `pbl build`
- Run tests: `pbl test`

## Submodules in agent workspaces

The submodules are large. In disposable or isolated agent workspaces, do not
initialize every submodule recursively. Initialize only the submodule needed
for the current build or task, using a shallow clone:

```
git submodule update --init --depth 1 -- path/to/submodule
```

Add `--recursive` only when that specific submodule requires its own nested
submodules. If a task needs submodule history, deepen only that submodule.
Workspace providers should reuse local Git object stores and copy-on-write
checkouts when available; agents must not symlink a writable submodule checkout
from another workspace because that breaks workspace isolation.

## Emulator (QEMU)

`docs/development/qemu.md` is the full workflow. In short:

```sh
pbl configure --board qemu_emery   # or qemu_flint / qemu_gabbro; qemu_emery = Pebble Time 2
pbl build
pbl qemu                           # --keep-flash-image keeps installed apps
pbl console                        # serial prompt (TCP localhost:12345)
pbl screenshot                     # -> build/screenshot.png; read it to check UI changes
```

- Drive the UI through the socket monitor (`build/qemu-mon.sock`) with `sendkey` (left = back,
  right = select, up, down), not the interactive QEMU window.
- Touch boards: `pbl touch X Y` taps, `pbl swipe X1 Y1 X2 Y2 [--steps N --duration S]`
  swipes. Screen-pixel coordinates; single touch only.
- UART1 output goes to `uart1.log` in the repository root; `pbl debug` attaches gdb.
- QEMU has no Bluetooth: NimBLE runs against a fake controller that never forms a link. With
  `CONFIG_MINIMED_SAKE` the QEMU build starts the MiniMed task against a stand-in link
  (`subsys/bluetooth/minimed_fake_pump.c`) that only proves the task plumbing boots and survives;
  the real pump link needs a watch. See "Emulator smoke test" below.

## Adding a new SDK function

Exposing a function to third-party apps requires three coordinated changes
(applib wrapper + syscall, `exported_symbols.json` registration, SDK
revision bump) — the firmware build alone won't surface it to apps. Follow
`docs/development/sdk_export.md` whenever an `applib/` function should
become callable from user apps.

## Git rules

Main rules:

- Commit using `-s` git option, so commits have `Signed-Off-By`
- Always indicate commit is co-authored by the current AI model
- Commit in small chunks, trying to preserve bisectability
- Commit format is `area: short description`, with longer description in the
  body if necessary
- Run `gitlint` on every commit to verify rules are followed

Others:

- If fixing Linear or GitHub issues, include in the commit body a line with
  `Fixes XXX`, where XXX is the issue number.
- Always start AI-written posts (PRs, issues, etc) with a disclaimer like:
  `🤖 Written by an AI at <Name>'s request`.

## MiniMed fork (OpenMinimed/PebbleOS)

This fork adds a direct watch <-> Medtronic MiniMed 780G pump link. The watch advertises as a
pump peripheral, runs the SAKE handshake, then reads the pump's CGM and IDD services as a GATT
client while the phone stays connected ("DUAL" mode). The data goes to an unmodified watchface
over the Pebble Glucose Protocol, injected as local AppMessages. The long-term plan (issue #25)
is to move all of it into a watchapp once the SDK exposes BLE; keep new code portable with that
in mind.

### Branches and remotes

- `origin` = OpenMinimed/PebbleOS; `dev/v4.36.2` is the default branch, features branch off it.
- `upstream` = coredevices/PebbleOS. Generic fixes (not MiniMed specific) go in their own
  commits so they can be sent upstream.
- When a merge shows that you and another contributor fixed the same bug, keep theirs unless it
  is a workaround and yours fixes the cause; say which one was kept in the merge commit.
- New files carry `SPDX-FileCopyrightText: <year> <your name>`. When substantially rewriting
  someone else's file, add your line under theirs; never replace it.

### Code map

- `fw/services/minimed/core/`: plain C, libc only. SAKE crypto, the pump protocol parsers
  (CGM, IDD status, history, annunciations, IOB), the graph, the 30-minute predictor and the hypo
  model, and `pebble_glucose_protocol.h`. No PebbleOS or NimBLE includes are allowed here: this is
  what the watchapp or a standalone library reuses.
- `fw/services/minimed/`: the firmware service.
  - `minimed_task.c`: the dedicated `PebbleTask_Minimed` (priority 1, deliberately not under the
    task watchdog), its event queue and timers.
  - `minimed_session.c`: the pump session (exchange serialiser, setup chain, backfill, models,
    alerts). Event-driven; never calls NimBLE.
  - `minimed_transport.h`: GATT client ops on named characteristics, results posted as events.
    Mirrors the SDK's `ble_client_*` API on purpose.
  - `minimed_sake_sender.c`: the watchface sink (loopback CommSession, one push per completed op).
  - `minimed_settings.c`: user settings set from the watchface's Clay page.
- `subsys/bluetooth/`: only what needs NimBLE. `minimed_transport_nimble.c` (the transport),
  `minimed_sake_service.c` (SAKE GATT server, handshake, advertising payload, pairing window),
  hooks in `advert.c`, `init.c`, `nimble_store.c`. On radio-less boards (`CONFIG_BT_HCI_FAKE`,
  i.e. QEMU) `minimed_fake_pump.c` replaces the transport and the cipher.
- `fw/popups/minimed_*` and `fw/apps/system/minimed_sake_app.c`: mode toggle, on-watch log, alert
  popups.
- `fw/services/minimed/stubs/`: link stand-ins for upstream code a MiniMed build leaves out, each
  behind its own `CONFIG_MINIMED_STUB_*` flag; the stubbed files themselves stay untouched. Today
  only voice (`CONFIG_MINIMED_STUB_VOICE`), which asterix needs to fit its flash.
- Enabled by `CONFIG_MINIMED_SAKE` (`subsys/bluetooth/Kconfig`); everything above is compiled out
  without it. The NimBLE security settings it needs (legacy pairing, two connections) are Kconfig
  defaults in `subsys/bluetooth/Kconfig.defconfig`.
- Deeper notes: `TESTING.md` (build, flash and test procedures), `CONNECTIVITY.md`, `WATCHFACE.md`,
  `BATTERY.md`, `PROGRESS.md`, `VERSIONS.md` (history).

### Build

```sh
./minimed-build.sh <desc> --pt2          # Pebble Time 2 (obelix): _slot0.pbz + _slot1.pbz
./minimed-build.sh <desc>                # Pebble 2 Duo (asterix)
```

- Needs a committed tree (the build identity is the commit; `--allow-dirty` overrides).
- Runs `pbl configure/build/bundle` in Docker (`pebbleos-minimed:v10`, built on first use from the
  CI image), in one build directory per board and slot (`build-pt2-slot0`, `build-pt2-slot1`,
  `build-asterix`). Outputs `build/minimed-<board>-<git describe>-<desc>[_slotN].pbz`, archives
  the matching `build/elfs/...elf` per slot and `build/...loghash.json`. Keep those: coredumps and flash logs
  from that build can only be decoded with them.
- PT2 firmware must be a release build from a release-form tag, one bundle per slot; the script
  handles it. `TESTING.md` explains why every shortcut (repacking, manifest edits) fails.
- Quick compile check without bundling: `just build [build-asterix|build-pt2-slot0]` runs
  `pbl build` in the same image against a directory `minimed-build.sh` already configured.
- Host tests for `core/` (no watch, no ARM toolchain): `make -C tools/minimed_sake_hosttest run`.
  They must pass before any firmware build.
- Unit tests of the firmware (host build): `pbl test` in the image (add `--no-images` to skip the
  slow image fixtures; the rendering suites then fail, which is expected).
- Docker image with everything preinstalled (pip requirements, SDL2 for QEMU, toolchain and
  QEMU on `PATH`): `docker build -t pebbleos-minimed:v10 -f tools/minimed/Dockerfile .` Use it
  in place of the CI image for local builds and emulator runs. Run it with
  `-u "$(id -u):$(id -g)" -e HOME=/tmp/home` so build outputs stay yours.

### Emulator smoke test

Run `tools/minimed/qemu_smoke_test.sh [seconds]` before flashing any change to the MiniMed task,
session or transport. It builds `qemu_emery` with MiniMed enabled into `build-qemu/`, boots it
headless, lets the stand-in pump link come up, then fails on any assert, fault, watchdog or reboot
in the serial logs (`build-qemu/qemu_uart*.log`, screenshot in `build-qemu/qemu_shot.ppm`). The
MiniMed task must never be able to take the
watch down: it is deliberately not under the task watchdog, and its init degrades to "pump link
off" instead of asserting.

### Flash

- `minimed-build.sh` pushes both slot files to the phone over adb, else KDE Connect. If KDE
  Connect says "no reachable device" while the phone is on the LAN, add its IP as a custom device:
  `busctl --user set-property org.kde.kdeconnect /modules/kdeconnect org.kde.kdeconnect.daemon
  customDevices as 1 <phone-ip>`, then call `forceOnNetworkChange` on the same interface.
- Two firmware slots (A/B): the watch runs from one and a firmware update is written to the
  other, so a bad image can never overwrite the running one. Each `.pbz` is linked for one slot's
  flash address, which is why every build produces `_slot0` and `_slot1`, and the Pebble app only
  accepts the image for the slot that is NOT running. So the file that worked last time is
  refused ("did not parse") once the watch has booted it; flash the other one. The running slot
  is in the boot log (`Boot slot: N`). Keep both files on the phone. Never repack both into one
  bundle or edit the manifest; `TESTING.md` lists what breaks.
- On the phone: Pebble app -> Settings -> debug options -> Devices -> the watch -> Firmware
  Update Debug -> Sideload FW, and pick the slot the app asks for (it wants the one not running).
- From the PC instead: `tools/flash_firmware.py build/minimed-...-<desc>` picks the right slot.

### Logs and coredumps

All of these go through the Pebble app's Developer Connection (Settings -> Developer
Connection), a WebSocket on port 9000. Over USB: `adb forward tcp:9000 tcp:9000`. Over Wi-Fi,
pass `--phone <phone-ip>`. Flashing or restarting the app usually turns Developer Connection off
again; a refused connection on port 9000 means that.

- Flash logs: `tools/dump_flash_logs.py --phone <ip> -g 0 --dict build/<build>.loghash.json`.
  `-g 0` is the current boot, `-g 1` the previous one. The dict must match the firmware that
  wrote the log, or lines stay as `NL:xxxx`; the boot line `Commit: <hash>` says which build.
  Watchface and app `APP_LOG` lines are in the flash log too, tagged `WF:` / `APP:`.
- Coredumps: `tools/fetch_coredump.py --phone <ip> -o <file>`. A complete fetch also marks the
  dump read on the watch, so the next crash gets a fresh one. Decode with
  `tools/analyze_coredump.py <elf> <core>`, using the archived ELF whose build ID matches the core
  (`arm-none-eabi-readelf -n <elf>`); the dump can be from an older build than the running one.
- The on-watch log (MiniMed app) shows the last few `minimed_sake_log` lines without a phone.

### Watchface (separate repo)

The companion watchface is `pebble-glucose-watchface` (`pebble build`, SDK 4.33). Its
`src/c/protocol.h` must stay byte-identical to `fw/services/minimed/core/
pebble_glucose_protocol.h`. The protocol spec lives in mortenfyhn/pebble-glucose-protocol
(`PROTOCOL.md` is the source; headers are generated from it). New protocol keys and capability
bits use experimental numbers (keys >= 1000), outside the reserved ranges.

### Environment gotchas

- The pebble tool, gitlint and libpebble2 need Python 3.13. If the system Python moved on, reinstall
  the uv tools with `uv tool install --reinstall --python 3.13 pebble-tool --with construct==2.5.3
  --with pyelftools` (and `gitlint-core`), and recreate the SDK's `.venv` under
  `~/.local/share/pebble-sdk/SDKs/<ver>/` from `sdk-core/requirements.txt`.
- The phone's IP changes between networks; ask for it rather than guessing.
- gitlint's UC3 rule rejects the maintainers' `users.noreply.github.com` author address; that
  warning is expected on every commit in this fork.
