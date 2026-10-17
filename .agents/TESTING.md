# On-Watch MiniMed — Build, Flash & Test

The streamlined loop for iterating on the MiniMed firmware. See `PROGRESS.md` for status/code map,
`VERSIONS.md` for the build and dev history.

> **The mode toggle is NORMAL⇄DUAL since v59, and DUAL is the normal working mode.** The watch
> holds the phone and the pump at the same time, so the old single-slot advice ("phone BT off",
> "the toggle drops the phone link") no longer applies. NORMAL is the kill switch: a full Bluetooth
> stack restart, phone-only. Sideloading does not need it — the phone session survives with the
> pump connected (HW-verified 2026-09-03, v62) — so `pebble logs`, `dump_flash_logs.py` and
> sideload all work mid-session. The app is the **MiniMed app**, and its on-watch mode lines read
> `MODE: NORMAL` / `MODE: DUAL (FE81|FE82)`. Sections below that name an old version (v23, v27,
> v30, v34, v36, v41) are kept because the procedure is still the way to re-verify that behaviour;
> where one says "SPIKE", read DUAL.

## Build + deploy (one command)

```sh
./minimed-build.sh -t asterix <desc>   # Pebble 2 Duo -> build/minimed-asterix-<describe>-<desc>.pbz
./minimed-build.sh -t obelix <desc>    # Pebble Time 2 -> ..._slot0.pbz + ..._slot1.pbz
./minimed-build.sh -t getafix <desc>   # Pebble Round 2 -> ..._slot0.pbz + ..._slot1.pbz
# --push shares the bundles to the phone; --allow-dirty bypasses the clean-tree check
```

- Refuses to build on a dirty tree (build identity = commit, so it must be committed first;
  `--allow-dirty` bypasses this). Writes `build/minimed-<board>-<describe>-<desc>[_slotN].pbz`,
  where `<board>` is the script's short board name (`asterix`, `pt2` for obelix, `getafix`) and
  `<describe>` is `git describe --dirty --always` (e.g. `v4.41.1-95-g38a8012b2`). That's the
  build-script/filename identity; separately, the running firmware's boot log and the MiniMed app
  top line always show the bare short commit hash (`Commit: <hash>`,
  `TINTIN_METADATA.version_short`) regardless of how the build was invoked — the two usually share
  the same hash suffix, but the runtime one is the unconditional source of truth for "what commit
  is this watch actually running" (describe's shape can collapse to just a tag name with no hash
  at all if `HEAD` is sitting exactly on one, e.g. rebuilding an unchanged commit on the obelix path).
- Shares to the phone over **adb** (`/sdcard/Download`), falling back to kdeconnect, only when
  `--push` is given.
- Builds with CMake (`pbl configure/build/bundle`) in the `pebbleos-minimed:v10` Docker image
  (`tools/minimed/Dockerfile`: the official CI image plus the Python requirements and SDL2, built
  on first use), one build directory per board and slot (`build-asterix`, `build-pt2-slot0`,
  `build-pt2-slot1`, `build-getafix-slot0`, `build-getafix-slot1`). Every run reconfigures, which
  is quick, so Kconfig changes need no flag.
- **asterix**: one bundle, no release band, and no voice dictation. Stock upstream v4.41.x already
  fills about 97% of the 992 KB flash region and the pump link adds roughly 59 KB, so the
  build turns the voice endpoint off (`CONFIG_SERVICE_VOICE_ENDPOINT=n`, which takes the voice
  service and speex with it) and links the stand-ins in `fw/services/minimed/stubs/`
  (`CONFIG_MINIMED_STUB_VOICE=y`), because upstream calls into voice unconditionally. With the
  set and sensor reminders it lands at about 99% (roughly 10 KB left). Dictation then fails as on
  a watch without a microphone.
- **obelix** and **getafix**: two single-slot bundles, release build, release-band check. The rest
  of this section is that recipe.

### User-facing settings

Anything a user (not just a developer) should be able to change lives in
`minimed_settings.{c,h}`, persisted to flash and set from the watchface's Settings page in the
Pebble mobile app (Clay; see `pebble-glucose-watchface/src/pkjs`) rather than a compile flag.
Which pump alert categories pop up on the watch (issue #15) is the first example. Adding a new
one: add a bit to `pebble_glucose_protocol.h`'s `KEY_SETTINGS_ALERTS`-style key (or a new key, if
it's not a bitmask), read it in `minimed_glucose_announce.c`, cache/persist it in
`minimed_settings.c`, and add a Clay item to `config.js` on the watchface side.

Some behaviour is still gated behind a plain `#define` rather than Kconfig, for things that are
genuinely a developer/debug knob rather than a user preference. To flip one of those on a personal
build without it ever showing up in `git diff`: create
`subsys/bluetooth/minimed_local_overrides.h` (gitignored) and `#define` it there — a source
file `#include`s it if present (guarded by `__has_include`) and only falls back to its own default
otherwise. There is no example left as of this writing (`MINIMED_ALERT_POPUPS` moved to the
Settings page above); the mechanism remains for the next one.

### The PT2 build recipe — read before deviating

The only images that **parse in the Pebble app** AND **boot on the PT2** are RAW single-slot
bundles built **release** (`CONFIG_RELEASE=y`) from a **release-form git tag** — one bundle per
slot. `minimed-build.sh` builds both `_slot0.pbz` and `_slot1.pbz` (and shares them with `--push`).

**Slot race (why a working file suddenly "does not parse"):** the app resolves a sideload to the
slot *not* currently running (`updateToSlot = 1 - runningSlot`) and its safety check requires
`firmware.slot == updateToSlot`. So a slot0-only pbz parses when the watch runs slot1, but once
the watch boots that slot0 image it runs slot0 and the app demands slot1 — same file, now a parse
error. Hence: keep both slot builds on the phone and flash the one the app asks for.

| Variant | Symptom | Why |
|---|---|---|
| Non-release build (`CONFIG_RELEASE` dropped) | black screen, no splash | non-release paths hung the obelix boot/display |
| Dual-slot repack (one pbz, slot0+slot1 dirs) | black screen | each slot shares a link layout; the repacked slot1 copy was the slot0-linked image, mislinked at slot1 |
| Manifest `versionTag` rewrite | "did not parse" | manifest-only patch corrupts what the app expects |
| Dev band (dirty/non-release git describe) | "did not parse" | app's version parser needs a release-form tag |
| Single slot0-only build when watch runs slot0 | "did not parse" | app wants slot1 (slot race) — not a build fault |

So: **one correctly-linked bundle per slot, release build, release-form tag. Do not repack, do
not rewrite the manifest, do not run non-release.** `minimed-build.sh` moves the annotated tag
`RELEASE_TAG` (default `v4.41.9`) to HEAD each run, builds slot0 and slot1 separately, and verifies
each is release band 0x01 with a version above stock before it is pushed (`--allow-dirty` skips that
check). The same recipe applies to getafix.

## Flash (BT sideload — no dev kit)

1. Watch in **DUAL**, connected to the Pebble app. Pre-toggling NORMAL is not required since v62:
   the phone session stays up alongside the pump, so the app can sideload mid-DUAL. (Not re-run
   under DUAL — if the app cannot see the watch, toggle NORMAL first.)
2. Pebble app → Settings → Show debug options → Devices → the watch → Firmware Update Debug →
   Sideload FW → pick the `.pbz` from Downloads.
3. Wait for install + reboot. It lands in NORMAL, since the mode is RAM-only.
4. **While still in NORMAL with the phone connected, launch the glucose watchface once.** A flash
   (and the phone's follow-up AppDB flush) leaves the app's *binary* off the watch, so the first
   launch has to fetch it from the phone. NORMAL is the known-good path for that fetch; see
   `WATCHFACE.md`.

Recovery if a build misbehaves: factory reset, or PRF recovery mode (separate slot; bricking very unlikely).

### What the flash costs you in pairings — nothing (HW-VERIFIED 2026-07-26, v36)

v36 stopped non-gateway bonds being pruned, so **the pump bond survives a reboot** and a flash costs
no pairings at all. The loop is: NORMAL → sideload → reboot → **SELECT** into DUAL → the pump
reconnects on its own (`paired (persisted): FE81` → `HANDSHAKE OK!`). The mode resets to NORMAL at
boot (it is RAM-only), so the one SELECT press remains.

Read the MiniMed app's bond inventory line to confirm, before and after:

- `bond gw1 pmp1 del0` — both bonds present, nothing pruned. This is the pass.
- `pmp0 del1` — something still pruned the pump bond.
- `pmp0 del0` — the bond was never stored; a different bug from pruning.
- `gw0` — the phone bond is gone.
- `MODE: DUAL (FE81)` together with `pmp0` — the FE81/FE82 mismatch, stated outright instead of
  having to be inferred from a `disc reason=0x08` loop half a minute later.

The three checks, all passed 2026-07-26: (1) reboot with the pump paired and confirm it reconnects
with no re-add on the pump; (2) forget the watch on the phone and re-pair it, then confirm the pump
bond still survives; (3) with the pump bonded, open Settings → Bluetooth and confirm the pump is not
listed, is not counted in the header, and that pairing a new phone is still offered. Re-run these
after any change to bond storage.

**Day-to-day: press SELECT, do nothing else.** No action on the phone or the pump — no pairing, no
forgetting, no Bluetooth menus. Since v62 both links are up at once, so the phone never contests the
slot and the Pebble app stays usable throughout. If the pump does not come back, give it up to
~1–2 min (it scans slowly to save battery); after a long absence its reconnect scan backs off and it
can take ~15 min.

One rule worth repeating:

- **Forget the pump with the app's DOWN button, never from the Bluetooth menu.** The menu deletes
  the SM bond but not the app's paired flag, so the watch advertises FE81 with no bond behind it
  and the pump cannot find it. The inventory line shows that state outright: `MODE: DUAL (FE81)`
  above `pmp0`. Note also that the watch is only pairable while you are actually standing on the
  Settings → Bluetooth screen, and only when its bond list is empty — forgetting the watch on the
  phone is not enough, you must forget the phone on the watch too.

Historical note (pre-v36, fixed): a flash evicted the pump's bond every time, because
`prv_load_ble_pairing_from_prf` re-stores the phone's PRF slot as a gateway bond on every boot, and
a *gateway* write then pruned every other BLE bond. That cost one pump re-pair per flash.

## Standing check: the pump must NOT connect in NORMAL

The bug this guards: in NORMAL the watch used to keep broadcasting the stale FE81 payload, so the
pump connected, ran SAKE in NORMAL and blocked the phone. NORMAL is phone-only now, and a bonded
pump that connects anyway is rejected by its persisted identity (the v32 rule, moved from RAM to
flash in v61). The tell is unambiguous — `HANDSHAKE OK`, `discovering CGM svc`, `polling BG`,
`sent reply (stN)`, `wrote N:..` are produced ONLY by the pump/SAKE path; the phone can never
generate them. (`connected` alone is ambiguous — it fires for any device — so judge by the SAKE
lines.)

The old bug was timing-dependent (it only bit when advertising was already on air at the toggle),
so one clean cycle proves nothing — run several, and provoke the bad timing.

1. Clean start: pump paired + BG in DUAL; phone paired. (Use DOWN + re-add "Mobile PB" to reconcile
   the pump if it is mismatched; forget/re-pair the phone once if needed.)
2. Toggle to NORMAL. Watch the log ~2 min. **PASS: no fresh SAKE-specific lines appear** (old lines
   lingering on the 8-line buffer from the last DUAL session do not count — watch for *new* ones).
   The phone should reconnect on its own, no "error pairing".
3. Provoke the old failure: enter DUAL, and while the pump is mid connect/disconnect loop (i.e. the
   watch is actively advertising FE81, pump not yet handshaked), toggle straight to NORMAL.
   Still no fresh SAKE lines in NORMAL.
4. Repeat 2–3 about 5 times. **PASS:** never any SAKE activity in NORMAL, and the phone reconnects
   hands-free every time. If the phone still fails while the pump is confirmed absent from NORMAL,
   that is a separate phone-bond bug, and a diagnostic-logging build is justified.

## Test the pump link

1. MiniMed app → SELECT → `MODE: DUAL`. The phone can stay connected (since v62 both links coexist);
   toggle to NORMAL only if the pump never appears.
2. Pump reconnects on its own (the bond persists across reflashes since v17). Watch the log:
   `paired (persisted): FE81` on boot → `adv EN FE81 …` → `connected` → `HANDSHAKE OK!` →
   `*** BG N.N mmol/L ***` every 60s, or `BG N.N same Nm` between readings.
3. First pairing only (or after a mismatch): pump → add device → **"Mobile PB"**.
4. NORMAL is only needed if the pump misbehaves (it is the kill switch), not for the next sideload.

## Verifying IOB (v30) — first HW use of encrypt-for-pump

v30 reads insulin-on-board from the pump and forwards it to the watchface (key 14). This was the
first time the watch ENCRYPTS a request TO the pump (`minimed_sake_encrypt`), so it is the
highest-risk path — the decode/parse is host-tested, the on-wire exchange needs a watch.

Setup: DUAL, pump connected, BG flowing (as for the normal pump test).

1. On boot/handshake the log should show `polling BG + IOB` (IDD service found) — or `polling BG
   only` / `no IDD svc 0x100` / `no IDD SRCP chr` if IOB discovery failed (BG still works; that's
   the intended fallback, not a crash).
2. Each ~60 s poll, after the BG line, expect `*** IOB N.N U ***`. **Cross-check that number
   against the pump's own "Active Insulin" display** — a match within seconds is unambiguous proof
   of the whole encrypt→write→decrypt→parse path.
3. On the watchface, IOB should appear (top-right) once both a BG and an IOB have arrived.
4. **If IOB fails, triage by the log line** (BG keeps working regardless):
   - `IOB encrypt failed` → session cipher not ready (shouldn't happen post-handshake).
   - `SRCP write err/rc=…` → the write to char 0x105 was rejected (discovery/handle/permission).
   - `SRCP decrypt failed` → response didn't authenticate (cipher-counter desync — watch whether
     it recovers next poll — or an unexpected E2E trailer).
   - `IOB bad N:xxxx…` → decrypted but wrong opcode/length; the hex is the first bytes the pump
     actually returned (compare to expected `fc 03 00 <medfloat32>`).
   - No IOB line at all after a BG poll → the SRCP exchange never completed; check for a `no IDD…`
     line at setup.
5. Known-benign: the first IOB can lag the first BG by one poll (the sender drops an IOB push that
   arrives before any BG exists); it catches up on the next reading.

## Verifying v34 (graph, staleness, quiet, connection params)

Four independent things; each shows up on its own, so a failure in one doesn't invalidate the rest.

**1. No more vibrations.** Toggle DUAL and let the pump connect and handshake: the watch must stay
**completely still**. Before v34 this was up to four buzzes (connect, subscribe, PUMP WROTE,
HANDSHAKE OK), and the phone reconnecting used to buzz too. If anything still vibrates on a BT
event, note *which* stage line it coincides with in the log.

**2. Connection parameters** (the battery measurement — this is the one number worth capturing).
On every connect, right after the `conn PUMP m=S …` line, expect:

    prm 30ms lat0 sv720ms

Write down what the pump actually chose. Interval × (latency + 1) is how often the radio must
wake — at latency 0 and a short interval this is the suspected main drain, and the number decides
whether the next step is worth doing (PROGRESS.md remaining-work 6). A second `prm …` line later
means the pump renegotiated mid-session; note that too.

**3. Honest staleness.** Watch two or three consecutive 60 s polls. Because the sensor only
produces a value every ~5 min, most polls should log:

    BG 6.2 same 3m

and only every ~5th poll should log `*** BG 6.2 mmol/L ***`. On the watchface the "ago" counter
must **climb to ~5 min and reset**, instead of sitting at 0. If *every* poll logs the `***` form,
the Time Offset field isn't behaving as assumed — capture a few `*** BG` lines with timings and
re-read the record. If it *never* logs the `***` form, readings are being wrongly suppressed (the
graph would stay stuck at one point) — that's the failure to report.

**4. Graph.** Since 2026-09-26 the graph also backfills on connect from the pump's event log
(`backfill N of M samples ... anchor=match`); without a backfill it starts empty and grows one point
per reading, filling the plot area over ~2 h. After ~15 min there should be 3 visible points. A gap
of more than 15 min (a pump dropout) draws as a break in the line, not a straight bridge. If the
graph area stays blank after several `*** BG` lines, look for `wf dict fail` in the log (dictionary
too small — a clean failure, not a crash, but nothing reaches the watchface then).

**Also confirm:** the pump shows the watch as **"Mobile PB"** — only after a remove + re-add on the
pump, since the name is read at pairing time. Do not lengthen the advertised name; see the warning
in `minimed_sake_service.c`.

**Protocol follow-up if 3 behaves as expected:** that confirms CGM Time Offset (record bytes 4–5,
u16 LE) is a per-reading monotonic minute counter. Make sure `../Documentation/cgm-service.md`
records it as confirmed rather than unknown.

## Test the watchface display

- Set the glucose watchface active, DUAL mode on. Each BG reading should show as the big number +
  age. Log: `wf sender up` (loopback session), `wf ready ping` (watchface announced itself).

## Pairing-dialog check (never independently verified)

With the runtime Security Manager flip (`minimed_sake_apply_sm_config`), a **phone** pairing should
show a **numeric confirm / yes-no prompt** — that is strict LESC. A silent Just Works pair means the
flip did not take effect. The pump is the opposite case by design: it pairs legacy Just Works inside
the pairing window, so no prompt there.

## When pump can't see watch (FE81/FE82 mismatch)

The watch's paired flag and the pump's bond can disagree, and neither auto-corrects:

- Watch log shows `adv EN FE81 …` but the pump can't find it → pump side is unpaired.
  **Press DOWN (forget pump)** on the watch → drops to FE82 → then add "Mobile PB" on the pump.
- **`adv EN FE81 t3` → `connected` → (silence) → `disc reason=0x08` (supervision timeout),
  looping, never reaching the handshake.** This is the mismatch's other face: the watch still
  advertises FE81 and believes it's paired, but has **no LTK** for the pump (e.g. its bond was
  deleted while the paired flag stayed set), so the pump connects, can't encrypt, and the link
  just times out. **Fix: DOWN (forget pump) on the watch, then remove/re-add "Mobile PB" on the
  pump** — a clean FE82 first-pair. (Don't misread this as a link/radio problem.)
- **Trap that causes the above:** forgetting the pump via the watch's **Settings → Bluetooth**
  menu deletes the SM bond but does **not** clear the MiniMed app's persisted paired flag (it lives
  in the separate `minimedsake` settings file), leaving exactly this FE81-without-a-key mismatch.
  Always forget the pump with the app's **DOWN** button, which clears both.
- Watch on FE82 but a bonded pump ignores it → remove/re-add the device on the pump.
- Rule of thumb: **pump can't see watch → make both do first-pair** (DOWN on watch + remove/add on pump).

## Reading logs

- The on-watch 8-line log in the MiniMed app is the primary debugger (no dev kit); everything that
  matters also goes to the flash log via `minimed_sake_log_evt()` (v65) — see `dump_flash_logs.py`.
- Vocabulary: `adv EN FE8x tN` (advertising: service + own-addr type), `adv START FAIL 0xNNNN`
  (advertising failed), `HANDSHAKE OK!`, `*** BG … ***`, `wf sender up` / `wf ready ping`
  (watchface link), `paired (persisted): FE81` (boot with a remembered pump).
