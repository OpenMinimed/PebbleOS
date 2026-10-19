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
machinery), `docs/development/qemu.md` (emulator workflow),
`docs/development/integration_tests.md` (pytest suite on QEMU and devices).

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

## MiniMed fork (OpenMinimed/PebbleOS)

This is the OpenMinimed fork: a direct watch <-> Medtronic MiniMed 780G pump link, with no phone
bridge. The watch advertises as a pump peripheral, runs the SAKE handshake, then reads the pump's
CGM and IDD services as a GATT client while the phone stays connected ("DUAL"). The data goes to
an unmodified watchface over the Pebble Glucose Protocol, injected as local AppMessages. The
long-term plan is to move all of it into a watchapp once the SDK exposes BLE; keep new code
portable with that in mind.

Fork notes, in `.agents/`, changed in separate `docs:` commits and never in code commits. Paths
inside them are relative to the repository root:

| File | Contents |
|---|---|
| `.agents/PROGRESS.md` | Current state, code map, remaining work |
| `.agents/TESTING.md` | The build/flash/test loop, on-hardware procedures |
| `.agents/CONNECTIVITY.md` | Bonds and pruning, advertising, the dual link, and the single-slot history |
| `.agents/WATCHFACE.md` | Watchface integration and the launch-crash saga |
| `.agents/BATTERY.md` | Drain measurements, method, ranked hypotheses |
| `.agents/VERSIONS.md` | Every build, newest first, plus the chronological dev log |

### Branches and remotes

- `origin` = OpenMinimed/PebbleOS; `dev/v4.41.1` is the default branch, features branch off it.
- `upstream` = coredevices/PebbleOS. Generic fixes (not MiniMed specific) go in their own
  commits so they can be sent upstream.
- When a rebase shows that you and another contributor fixed the same bug, keep theirs unless it
  is a workaround and yours fixes the cause; say which one was kept in the commit message.
- New files carry `SPDX-FileCopyrightText: <year> <your name>`. When substantially rewriting
  someone else's file, add your line under theirs; never replace it.
- The pre-rebuild fork head is kept as the tag `archive/pre-rebuild-v4.41.0`, alongside the
  archived branches `origin/archive-v4.36.2` and `origin/archive-v4.24.0`. Build hashes in
  `.agents/VERSIONS.md` and `.agents/PROGRESS.md` from before the 2026-10 rebuild resolve against
  those, not against the current tree.

### Build

```sh
./minimed-build.sh -t asterix <desc>   # Pebble 2 Duo (nRF52840), one bundle
./minimed-build.sh -t obelix <desc>    # Pebble Time 2 (SiFli), _slot0.pbz + _slot1.pbz
./minimed-build.sh -t getafix <desc>   # Pebble Round 2 (SiFli), _slot0.pbz + _slot1.pbz
# add --push to share the .pbz to a phone over adb, else kdeconnect
```

- Needs a committed tree: the build identity is the commit (`--allow-dirty` overrides).
- Runs `pbl configure/build/bundle` in the Docker image `pebbleos-minimed:v10` (built on first use
  from `tools/minimed/Dockerfile`), one build directory per board and slot (`build-asterix`,
  `build-pt2-slot0`, `build-pt2-slot1`, `build-getafix-slot0`, `build-getafix-slot1`). Every run
  reconfigures, so a Kconfig change needs no flag.
- Outputs `build/minimed-<board>-<describe><-desc>[_slotN].pbz` (`<board>` is `asterix`, `pt2` for
  obelix, or `getafix`), archives the matching `build/elfs/...elf` per slot and the
  `build/...loghash.json`. Keep those: coredumps and flash logs from that build can only be
  decoded with them.
- `obelix` and `getafix` must be release builds from a release-form tag, one bundle per slot; the
  script moves `RELEASE_TAG` (default `v4.41.9`) to HEAD, then verifies release band 0x01 and a
  version above the stock floor (v4.36.2). `.agents/TESTING.md` explains why every shortcut
  (repacking, manifest edits) fails.
- `asterix` is the tight one: its build turns the voice endpoint off
  (`-DCONFIG_SERVICE_VOICE_ENDPOINT=n`) and links the stand-ins in `fw/services/minimed/stubs/`
  (`CONFIG_MINIMED_STUB_VOICE`, on by itself in such a build) to fit the flash region.
- `asterix` is tight on RAM too. The kernel heap is the RAM left after all statics and fixed
  stacks, and an incoming call needs ~17 KB of it on top of ~15 KB idle use. The build fails when
  the heap is below `HEAP_FLOOR` (33 000 B) and prints its size on every build. Treat each new
  static buffer, table or stack as coming out of that heap: prefer small types, one copy of a
  buffer, and transient allocations, and say what a change costs in RAM. Don't lower the floor to
  get a build through. Background in `.agents/PROGRESS.md` ("Kernel heap and incoming calls").
- Extra Kconfig settings go on the build script's command line, e.g.
  `./minimed-build.sh -t asterix -DCONFIG_MINIMED_PREDICTOR=n <desc>`. The build's `.config` is
  saved next to the `.pbz`, so the build stays traceable without committing the flags.
- Quick compile check without bundling: `just build [build-asterix|build-pt2-slot0]` runs
  `pbl build` in the same image against a directory `minimed-build.sh` already configured.
- Host tests for `core/` (no watch, no ARM toolchain): `make -C tools/minimed_sake_hosttest run`.
  They must pass before any firmware build.
- Unit tests of the firmware (host build): `pbl test` in the image, e.g.

  ```sh
  docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp/home -v "$PWD":/pebbleos -w /pebbleos \
    pebbleos-minimed:v10 bash -lc "pbl test --no-images"
  ```

  `pbl test` builds into `build-test/`, so it never touches a firmware build. `--no-images` skips
  the slow image fixtures; the rendering suites then fail, which is expected. `-R REGEX` selects
  tests by name.
- Docker image with everything preinstalled (pip requirements, SDL2 for QEMU, toolchain and QEMU
  on `PATH`): `docker build -t pebbleos-minimed:v10 -f tools/minimed/Dockerfile .`. Use it in
  place of the CI image for local builds and emulator runs, with
  `-u "$(id -u):$(id -g)" -e HOME=/tmp/home` so build outputs stay yours.

### Emulator (QEMU)

`docs/development/qemu.md` is the full workflow. In short:

```sh
pbl configure -b build-qemu --board qemu_emery   # qemu_flint / qemu_gabbro too; emery = PT2
pbl build -b build-qemu
pbl qemu                           # --keep-flash-image keeps installed apps and bondings
pbl console                        # serial prompt (TCP localhost:12345)
pbl screenshot                     # writes <build dir>/screenshot.png; read it to check UI changes
```

- Drive the UI over the socket monitor (`build/qemu-mon.sock`) with `sendkey` (left = back,
  right = select, up, down) rather than the interactive QEMU window.
- Touch boards: `pbl touch X Y` taps, `pbl swipe X1 Y1 X2 Y2 [--steps N --duration S]` swipes.
  Screen-pixel coordinates, single touch only.
- Phone-dependent features get their data from `pbl feed <feed>` (`weather`, `music`, `calendar`,
  `notifications`); the feeds live in `tools/libs/pbl-cli/pbl/feeds/`.
- UART1 output goes to `uart1.log` in the repository root; `pbl debug` attaches gdb.
- QEMU has no Bluetooth: NimBLE runs against a fake controller that never forms a link. With
  `CONFIG_MINIMED_SAKE` the QEMU build starts the MiniMed task against a stand-in link
  (`subsys/bluetooth/minimed_fake_pump.c`) that only proves the task plumbing boots and survives;
  the real pump link needs a watch. See below.

### Emulator smoke test

Run `tools/minimed/qemu_smoke_test.sh [seconds]` before flashing any change to the MiniMed task,
session or transport. It builds `qemu_emery` with MiniMed enabled into `build-qemu/`, boots it
headless, lets the stand-in pump link come up, then fails on any assert, fault, watchdog or reboot
in the serial logs (`build-qemu/qemu_uart*.log`, screenshot in `build-qemu/qemu_shot.ppm`). The
MiniMed task must never be able to take the watch down: it is deliberately not under the task
watchdog, and its init degrades to "pump link off" instead of asserting.

### Flash

- `minimed-build.sh --push` sends the `.pbz` files to the phone over adb, else KDE Connect. If KDE
  Connect says "no reachable device" while the phone is on the LAN, add its IP as a custom device:
  `busctl --user set-property org.kde.kdeconnect /modules/kdeconnect org.kde.kdeconnect.daemon
  customDevices as 1 <phone-ip>`, then call `forceOnNetworkChange` on the same interface.
- Two firmware slots (A/B): the watch runs from one and a firmware update is written to the
  other, so a bad image can never overwrite the running one. Each `.pbz` is linked for one slot's
  flash address, which is why every obelix/getafix build produces `_slot0` and `_slot1`, and the
  Pebble app only accepts the image for the slot that is NOT running. So the file that worked last
  time is refused ("did not parse") once the watch has booted it; flash the other one. The running
  slot is in the boot log (`Boot slot: N`). Keep both files on the phone. Never repack both into
  one bundle or edit the manifest; `.agents/TESTING.md` lists what breaks.
- On the phone: Pebble app -> Settings -> debug options -> Devices -> the watch -> Firmware
  Update Debug -> Sideload FW, and pick the slot the app asks for (it wants the one not running).
- From the PC instead: `tools/flash_firmware.py build/minimed-...-<desc>` picks the right slot.

### Logs and coredumps

All of these go through the Pebble app's Developer Connection (Settings -> Developer Connection),
a WebSocket on port 9000. Over USB: `adb forward tcp:9000 tcp:9000`. Over Wi-Fi, pass
`--phone <phone-ip>`. Flashing or restarting the app usually turns Developer Connection off
again; a refused connection on port 9000 means that.

- Flash logs: `tools/dump_flash_logs.py --phone <ip> -g 0 --dict build/<build>.loghash.json`.
  `-g 0` is the current boot, `-g 1` the previous one. The dict must match the firmware that
  wrote the log, or lines stay as `NL:xxxx`; the boot line `Commit: <hash>` says which build.
  Watchface and app `APP_LOG` lines are in the flash log too, tagged `WF:` / `APP:`.
- Coredumps: `tools/fetch_coredump.py --phone <ip> -o <file>`. A complete fetch also marks the
  dump read on the watch, so the next crash gets a fresh one. Decode with
  `tools/analyze_coredump.py <elf> <core>`, using the archived ELF whose build ID matches the core
  (`arm-none-eabi-readelf -n <elf>`); the dump can be from an older build than the running one.
  The host has neither `construct` nor an ARM GDB, so run `tools/readcore.py` and
  `arm-none-eabi-gdb` (`/opt/pebbleos-sdk/arm-none-eabi/bin`) inside `pebbleos-minimed:v10`.
- The on-watch log (MiniMed app) shows the last few `minimed_sake_log` lines without a phone.

### Watchface (separate repo)

The companion watchface is `pebble-glucose-watchface` (`pebble build`). Its `src/c/protocol.h`
must stay byte-identical to `fw/services/minimed/core/pebble_glucose_protocol.h`. The protocol
spec lives in mortenfyhn/pebble-glucose-protocol (`PROTOCOL.md` is the source; headers are
generated from it). New protocol keys and capability bits use experimental numbers (keys >= 1000),
outside the reserved ranges.

### Code map

- `fw/services/minimed/core/`: plain C, libc only. SAKE crypto, the pump protocol parsers
  (CGM, IDD status, history, annunciations, IOB), the graph, the 30-minute predictor and the hypo
  model, and `pebble_glucose_protocol.h`. No PebbleOS or NimBLE includes are allowed here: this is
  what the watchapp or a standalone library reuses.
- `fw/services/minimed/`: the firmware service.
  - `minimed_task.c`: the dedicated `PebbleTask_Minimed` (priority 1, deliberately not under the
    task watchdog), its event queue and timers.
  - `minimed_session.c`: the pump session (exchange serialiser, setup chain, backfill, alerts,
    reminders). Event-driven; never calls NimBLE.
  - `minimed_forecast.c` and `minimed_hypo_check.c`: the 30-minute forecast and the hypo model,
    fed by the session. Each can be compiled out (`CONFIG_MINIMED_PREDICTOR`,
    `CONFIG_MINIMED_HYPO`, in `fw/services/minimed/Kconfig`); the session then calls empty
    inline stand-ins from their headers. Total IOB's basal part (`core/minimed_basal_iob.c`) does
    not depend on either.
  - `minimed_transport.h`: GATT client ops on named characteristics, results posted as events.
    Mirrors the SDK's `ble_client_*` API on purpose.
  - `minimed_sake_sender.c`: the watchface sink (loopback CommSession, one push per completed op).
  - `minimed_settings.c`: user settings, set in the MiniMed app's menu.
- `subsys/bluetooth/`: only what needs NimBLE. `minimed_transport_nimble.c` (the transport),
  `minimed_sake_service.c` (SAKE GATT server, handshake, advertising payload, pairing window),
  hooks in `advert.c`, `init.c`, `nimble_store.c`. On radio-less boards (`CONFIG_BT_HCI_FAKE`,
  i.e. QEMU) `minimed_fake_pump.c` replaces the transport and the cipher.
- `fw/popups/minimed_*` and `fw/apps/system/minimed_sake_app.c`: the MiniMed app's menu (log,
  mode, settings, forget pump) and the alert popups.
- `fw/services/minimed/stubs/`: link stand-ins for upstream code a MiniMed build leaves out, each
  behind its own `CONFIG_MINIMED_STUB_*` flag; the stubbed files themselves stay untouched. Today
  only voice (`CONFIG_MINIMED_STUB_VOICE`), which asterix needs to fit its flash.
- Enabled by `CONFIG_MINIMED_SAKE` (`subsys/bluetooth/Kconfig`); everything above is compiled out
  without it. The NimBLE security settings it needs (legacy pairing, two connections) are Kconfig
  defaults in `subsys/bluetooth/Kconfig.defconfig`.

### Keeping the fork easy to rebase

Each upstream release is integrated by rebasing the dev branch onto the new release tag, not by
merging the release in. Write fork code so that this stays cheap:

- Put fork code in new files and directories (`fw/services/minimed/`, `subsys/bluetooth/minimed_*`,
  `fw/popups/minimed_*`, `tools/minimed*`), not in upstream files.
- Change upstream files only with small hooks, such as one include and one call guarded by
  `#ifdef CONFIG_MINIMED_SAKE`. Prefer upstream extension points (a Kconfig default, a CMake
  exclude, a callback) over editing upstream logic.
- Do not reformat, reorder or rename upstream code, and do not fix upstream style in passing.
- Before refactoring fork code, make sure tests cover it: the host tests
  (`make -C tools/minimed_sake_hosttest run`) for `core/`, `pbl test` for upstream code. Add the
  tests first if they are missing, and keep them passing through the refactor.
- Every commit must build for asterix and obelix and pass the host tests and
  `pbl test --no-images`, so that history stays bisectable.

### Environment gotchas

- The pebble tool, gitlint and libpebble2 need Python 3.13. If the system Python moved on, reinstall
  the uv tools with `uv tool install --reinstall --python 3.13 pebble-tool --with construct==2.5.3
  --with pyelftools` (and `gitlint-core`), and recreate the SDK's `.venv` under
  `~/.local/share/pebble-sdk/SDKs/<ver>/` from `sdk-core/requirements.txt`.
- The phone's IP changes between networks; ask for it rather than guessing.
- gitlint's UC3 rule rejects the maintainers' `users.noreply.github.com` author address; that
  warning is expected on every commit in this fork.
- The host tests are blind to locks: `stubs_mutex.h` makes `mutex_lock()` a no-op, so a
  lock-ordering or reentrancy bug passes the suite and then hangs the watch. Do per-bond lookups in
  a second pass after a `for_each_ble_pairing` iteration returns, never from inside its callback.

### Sibling checkouts

Some fork notes cite files in sibling checkouts that are not part of this repository and whose
paths move; check before trusting one:

- `../Documentation/` - the pump protocol source of truth (`sake.md`, `idd-service.md`,
  `cgm-service.md`, `bluetooth.md`, `key_databases.md`, ...). Update it eagerly with confirmed
  facts.
- `../xdrip-pebble-archive/PythonPumpConnector/PythonSake/pysake/` - a readable SAKE reference with
  public test vectors (`constants.py`).
- `../minimed-pebble-bridge/glycemicgpt/plugins/shipped/medtronic/` - the working Kotlin bridge to
  port from.
- Public: [mortenfyhn/pebble-glucose-protocol](https://github.com/mortenfyhn/pebble-glucose-protocol)
  (the wire format and its `PROTOCOL.md` source), and the companion watchface
  [mortenfyhn/pebble-glucose-watchface](https://github.com/mortenfyhn/pebble-glucose-watchface)
  (a sibling checkout when changing the watchface side).

### Git

- Always start AI-written posts (PRs, issues, etc.) with a disclaimer like:
  `🤖 Written by an AI at <Name>'s request`.
- Do not comment on GitHub or upstream; Morten does that himself.
