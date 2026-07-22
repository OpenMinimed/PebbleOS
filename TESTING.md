# On-Watch SAKE Spike — Build, Flash & Test

The streamlined loop for iterating on the spike firmware. See `PROGRESS.md` for status/code map,
`HISTORY.md` for the dev history.

## Build + deploy (one command)

```sh
./spike-build.sh <desc>              # build + bundle -> build/sake-spike-vN-<desc>.pbz, adb-push to phone
./spike-build.sh <desc> --no-push    # skip the adb push
./spike-build.sh <desc> --configure  # add this after Kconfig / app-registry changes
```

- Auto-increments the version number and writes `build/sake-spike-vN-<desc>.pbz`.
- Pushes to the phone's `/sdcard/Download/` over adb (phone must be USB-connected; one device).
- The Docker image `pebbleos-build:local` must exist (see PROGRESS.md "How to build" if it's gone).

## Flash (BT sideload — no dev kit)

1. Watch in **NORMAL mode** (SAKE Spike app → SELECT until `MODE: NORMAL`), connected to the Pebble app.
2. Pebble app → Settings → Show debug options → Devices → the watch → Firmware Update Debug →
   Sideload FW → pick the `.pbz` from Downloads.
3. Wait for install + reboot (boots NORMAL = ordinary Pebble).

Recovery if a build misbehaves: factory reset, or PRF recovery mode (separate slot; bricking very unlikely).

## Test the pump link

1. SAKE Spike app → SELECT → `MODE: SPIKE`. Phone BT **off** (single BLE slot).
2. Pump reconnects on its own (pairing persists across reflashes since v17). Watch the log:
   `paired (persisted): FE81` on boot → `adv EN FE81 …` → `connected` → `HANDSHAKE OK!` →
   `*** BG N.N mmol/L ***` every 60s.
3. First pairing only (or after a mismatch): pump → add device → "Mobile PB".
4. Back to NORMAL to restore the phone link (needed before the next sideload).

## Test the watchface display

- Set the MiniMed watchface active, SPIKE mode on. Each BG reading should show as the big number +
  age. Log: `wf sender up` (loopback session), `wf ready ping` (watchface announced itself).

## When pump can't see watch (FE81/FE82 mismatch)

The watch's paired flag and the pump's bond can disagree, and neither auto-corrects:

- Watch log shows `adv EN FE81 …` but the pump can't find it → pump side is unpaired.
  **Press DOWN (forget pump)** on the watch → drops to FE82 → then add "Mobile PB" on the pump.
- Watch on FE82 but a bonded pump ignores it → remove/re-add the device on the pump.
- Rule of thumb: **pump can't see watch → make both do first-pair** (DOWN on watch + remove/add on pump).

## Reading logs

- The on-watch 8-line log in the SAKE Spike app is the primary debugger (no dev kit).
- Vocabulary: `adv EN FE8x tN` (advertising: service + own-addr type), `adv START FAIL 0xNNNN`
  (advertising failed), `HANDSHAKE OK!`, `*** BG … ***`, `wf sender up` / `wf ready ping`
  (watchface link), `paired (persisted): FE81` (boot with a remembered pump).
