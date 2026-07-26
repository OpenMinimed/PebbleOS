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

### What the flash costs you in pairings (derived from the code, 2026-07-26)

#### On v36 and later: nothing (HW-VERIFIED 2026-07-26)

v36 stops non-gateway bonds being pruned, so **the pump bond survives a reboot** and a flash costs
no pairings at all. All three checks below passed on hardware the day it was built. The loop becomes: NORMAL → sideload → reboot → **SELECT** into SPIKE → the pump
reconnects on its own (`paired (persisted): FE81` → `HANDSHAKE OK!`). The mode still resets to
NORMAL at boot (it is RAM-only), so the one SELECT press remains.

Read the SAKE Spike app's bond inventory line to confirm, before and after:

- `bond gw1 pmp1 del0` — both bonds present, nothing pruned. This is the pass.
- `pmp0 del1` — something still pruned the pump bond; v36 did not hold.
- `pmp0 del0` — the bond was never stored; a different bug from pruning.
- `gw0` — the phone bond is gone (the v33 regression direction).
- `MODE: SPIKE (FE81)` together with `pmp0` — the FE81/FE82 mismatch, stated outright instead of
  having to be inferred from a `disc reason=0x08` loop half a minute later.

The v36 checks, all passed 2026-07-26: (1) reboot with the pump paired and confirm it reconnects
with no re-add on the pump; (2) forget the watch on the phone and re-pair it, then confirm the pump
bond still survives; (3) with the pump bonded, open Settings → Bluetooth and confirm the pump is not
listed, is not counted in the header, and that pairing a new phone is still offered. Re-run these
after any change to bond storage. If they ever fail, reflash v35 and the v35-and-earlier procedure
below applies again.

Two things learned doing this the first time, both worth repeating:

- **Forget the pump with the app's DOWN button, never from the Bluetooth menu.** The menu deletes
  the SM bond but not the app's paired flag, so the watch advertises FE81 with no bond behind it
  and the pump cannot find it. v36 shows this state directly: `MODE: SPIKE (FE81)` above `pmp0`.
- **Turn phone Bluetooth off while first-pairing the pump.** There is still one connection slot,
  and nothing rejects the *phone* in SPIKE (the v32 rule only keeps the *pump* out of NORMAL), so a
  bonded phone can take the slot by identity address while you are advertising FE82. Also note the
  watch is only pairable while you are actually standing on the Settings → Bluetooth screen, and
  only when its list is empty — forgetting the watch on the phone is not enough, you must forget
  the phone on the watch too.

#### On v35 and earlier: one pump re-pair per flash

**The phone bond survives a flash; the pump bond never does.** `bt_persistent_storage_init` →
`prv_load_ble_pairing_from_prf` re-stores the PRF slot (always the phone — only gateway bonds are
written there) as `is_gateway=true`, and a *gateway* write still prunes every other BLE bond. A
flash is a reboot, so the pump's bond is evicted every time. Same reason the pump re-pairs after any
reboot (remaining-work item 8). So budget **one pump re-pair per flash, and no phone re-pair** —
unless the phone link is already broken before you start, which on pre-v33 firmware it often is.

Order matters, because pairing the phone deletes the pump bond but not vice versa:

1. **Phone first.** NORMAL mode, get a working Pebble-app link (that's what sideloading needs). If
   it loops `connected` → `reason 0x13` or the phone says "error pairing", forget the watch on the
   phone and re-pair. Don't bother protecting the pump bond here — you're about to lose it anyway.
2. **Sideload** the `.pbz` (steps above). The watch reboots.
3. **Pump second.** SPIKE mode, press **DOWN** (forget pump). This is required, not optional: the
   pump's SM bond is gone but the watch's own paired flag lives in a separate settings file
   (`minimedsake`) and still says paired, so it would advertise FE81 with no bond — the classic
   FE81/FE82 mismatch below. DOWN drops it to FE82 first-pair. Then on the pump, remove the old
   entry and add **"Mobile Pebble"**.
4. Wait for `HANDSHAKE OK!`, confirm BG (and IOB), and note the `prm …` line.
5. **Only then** run the phone-bond test (SPIKE→NORMAL toggles). Don't reboot the watch or re-pair
   the phone mid-test — either one costs you step 3 again.

Worth fixing eventually: the watch could clear its own paired flag at boot when no pump bond exists,
which would make step 3's DOWN unnecessary and auto-heal the FE81/FE82 mismatch permanently. (v36's
bond inventory line now *detects* that mismatch — `FE81` with `pmp0` — but deliberately does not
auto-heal it; making the state visible came first.)

## Decisive test: pump must NOT connect in NORMAL (v27 rearchitecture)

The bug this checks: in NORMAL the watch used to keep broadcasting the stale FE81 payload, so the
pump connected + ran SAKE in NORMAL and blocked the phone. The tell is unambiguous — the lines
`HANDSHAKE OK`, `discovering CGM svc`, `polling BG`, `sent reply (stN)`, `wrote N:..` are produced
ONLY by the pump/SAKE path; the phone can never generate them. (`connected` alone is ambiguous —
it fires for any device — so judge by the SAKE-specific lines, not by `connected`.)

Because the old bug was timing-dependent (only bit when advertising was already on air at the
toggle), one clean cycle proves nothing — run several, and provoke the bad timing.

1. Clean start: pump paired + BG in SPIKE; phone paired in NORMAL. (Use DOWN + re-add "Mobile PB"
   to reconcile the pump if it's mismatched; forget/re-pair the phone once if needed.)
2. Toggle to NORMAL. Watch the log ~2 min. **PASS: no fresh SAKE-specific lines appear** (old lines
   lingering on the 8-line buffer from the last SPIKE session don't count — watch for *new* ones).
   The phone should reconnect on its own, no "error pairing".
3. Provoke the old failure: enter SPIKE, and while the pump is mid connect/disconnect loop (i.e.
   the watch is actively advertising FE81, pump not yet handshaked), toggle straight to NORMAL.
   Still no fresh SAKE lines in NORMAL.
4. Repeat 2–3 about 5 times. **PASS:** never any SAKE activity in NORMAL, phone reconnects
   hands-free every time. **If the phone still fails while the pump is confirmed absent from
   NORMAL**, that's a genuinely separate phone-bond bug (then a diagnostic-logging build is
   justified) — but the expectation is it's now fixed, because it was downstream of this.

## Test the gateway-displacement fix (v23) — do this FIRST

This verifies the pump no longer knocks the phone off. It's the whole point of v23.

Before flashing, if you're stuck (phone won't reconnect, pump shows in the watch's Bluetooth
menu), recover on the current fw: SAKE app → SELECT to `MODE: NORMAL`; watch Settings →
Bluetooth → forget the listed pump; on the phone, reconnect / re-pair once. Then:

1. **Flash v23** (see "Flash" below). Boots NORMAL.
2. Open the SAKE app: expect `paired (persisted): FE81`. Confirm the **phone is connected** and
   the watch's Settings → Bluetooth shows the **phone**.
3. SELECT → `MODE: SPIKE`. Phone BT can stay on — the toggle drops the phone link (single slot)
   and switches to the Medtronic advert. Pump reconnects: `adv EN FE81 t3` → `connected` →
   `HANDSHAKE OK!` → `*** BG N.N ***`.
4. **The key check:** SELECT → back to `MODE: NORMAL`. Within ~a minute the **phone reconnects on
   its own, with no re-pair prompt**, and Settings → Bluetooth still shows the **phone** (NOT the
   pump). That's the fix working.
5. Repeat steps 3–4 twice more. **Pass:** the phone comes back every time without re-pairing and
   the pump never appears in the watch's Bluetooth menu. **Fail:** any cycle needs a phone
   re-pair, or the pump shows up in that menu — capture the on-watch log and report it.
6. Reboot check (persistence): with the pump paired, power-cycle the watch. On boot expect
   `paired (persisted): FE81`; go SPIKE and confirm the pump reconnects **without** re-adding
   "Mobile PB" on the pump (its bond survived the reboot as a non-gateway bond).

## Test the pump link

1. SAKE Spike app → SELECT → `MODE: SPIKE`. Phone BT **off** (single BLE slot).
2. Pump reconnects on its own (pairing persists across reflashes since v17). Watch the log:
   `paired (persisted): FE81` on boot → `adv EN FE81 …` → `connected` → `HANDSHAKE OK!` →
   `*** BG N.N mmol/L ***` every 60s.
3. First pairing only (or after a mismatch): pump → add device → "Mobile PB".
4. Back to NORMAL to restore the phone link (needed before the next sideload).

## Verifying IOB (v30) — first HW use of encrypt-for-pump

v30 reads insulin-on-board from the pump and forwards it to the watchface (key 14). This is the
first time the watch ENCRYPTS a request TO the pump (`sake_encrypt_for_pump`), so it's the highest-
risk new path — the decode/parse is host-tested (32/32) but the on-wire exchange is not.

Setup: SPIKE, pump connected, BG flowing (as for the normal pump test).

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

**1. No more vibrations.** The whole point of the change. Toggle SPIKE and let the pump connect
and handshake: the watch must stay **completely still**. Previously this was up to four buzzes
(connect, subscribe, PUMP WROTE, HANDSHAKE OK). Also toggle back to NORMAL and let the phone
reconnect — that used to buzz too. If anything still vibrates on a BT event, note *which* stage
line it coincides with in the log.

**2. Connection parameters** (the battery measurement — this is the one number worth capturing).
On every connect, right after the `conn PUMP m=S …` line, expect:

    prm 30ms lat0 sv720ms

Write down what the pump actually chose. Interval × (latency + 1) is how often the radio must
wake — at latency 0 and a short interval this is the suspected main drain, and the number decides
whether the next step is worth doing (PROGRESS.md remaining-work 6). A second `prm …` line later
means the pump renegotiated mid-session; note that too.

**3. Honest staleness.** Watch two or three consecutive 60 s polls. Because the sensor only
produces a value every ~5 min, most polls should now log:

    BG 6.2 same 3m

and only every ~5th poll should log `*** BG 6.2 mmol/L ***`. On the watchface the "ago" counter
must now **climb to ~5 min and reset**, instead of sitting at 0. If *every* poll logs the `***`
form, the Time Offset field isn't behaving as assumed — capture a few `*** BG` lines with timings
and we'll re-read the record. If it *never* logs the `***` form, readings are being wrongly
suppressed (the graph would stay stuck at one point) — that's the failure to report.

**4. Graph.** Starts EMPTY — this is expected, there is no backfill. One point appears per new
reading, so the trace builds up over ~2 h and only then fills the plot area. After ~15 min there
should be 3 visible points. A gap of more than 15 min (a pump dropout) draws as a break in the
line, not a straight bridge. If the graph area stays blank after several `*** BG` lines, look for
`wf dict fail` in the log (dictionary too small — a clean failure, not a crash, but nothing
reaches the watchface then).

**Also confirm:** the pump shows the watch as **"Mobile Pebble"** — only after a remove + re-add on
the pump, since the name is read at pairing time.

**Protocol follow-up if 3 behaves as expected:** that confirms CGM Time Offset (record bytes 4–5,
u16 LE) is a per-reading monotonic minute counter — currently `???` in
`Documentation/cgm-service.md:198`. Update it there once seen on HW.

## Capturing the advert diagnostics (the pump-in-NORMAL question)

v30 also adds passive advert logging to finally settle whether the pump reconnects in NORMAL by
address (advert payload irrelevant) or the advert refresh isn't landing. **Film a video** (the
8-line ring scrolls) of a SPIKE→NORMAL toggle and read:
- `advN <7 hex>` after the toggle — bytes 6-7 (`..b5 b6`): `82fe`/`81fe` = a Medtronic FE82/FE81
  payload still radiating in NORMAL (stale-payload bug); anything else = real Pebble payload
  landed. No `advN` line at all after the toggle = `set_advertising_data` never ran.
- `conn PUMP m=N ..` = a pump reconnecting while in NORMAL (the bug); `conn phone m=N` = the phone.
- Combined verdict: `advN` shows Pebble bytes but you still get `conn PUMP m=N` + a handshake →
  the pump reconnects **by address**, so the fix is to reject the pump's connection in NORMAL by
  its stored identity (not more advert work). `advN` shows `..81fe` → the refresh still isn't
  landing.

## Test the watchface display

- Set the MiniMed watchface active, SPIKE mode on. Each BG reading should show as the big number +
  age. Log: `wf sender up` (loopback session), `wf ready ping` (watchface announced itself).

## Verify the phone-bond fix (v33, shipped in v34 — PENDING HW)

The re-pair-every-cycle papercut had two causes. The pump stealing the single slot is fixed and
HW-verified (v32). The second — the pump's bond **deleting** the phone's, so the phone lost its LTK
and looped connect/terminate(`0x13`) — is fixed in v33 and still unverified. This is the test.

1. Flash, then do the FULL first-time pairing **once**: phone in NORMAL, then pump in SPIKE
   (DOWN to forget, then add "Mobile Pebble" on the pump). This one re-pair is expected.
2. Cycle: SPIKE → wait for `HANDSHAKE OK!` → back to NORMAL. The phone should reconnect within
   ~a minute with **no re-pair prompt** and no `0x13` loop. In the log: `pump conn in NORMAL ->
   drop` for the pump (v32 doing its job), then `connected` + `conn phone m=N` that **stays**.
3. Repeat three times. **Pass:** the phone reconnects every cycle without re-pairing.
   **Fail:** any cycle needs a re-pair, or the bond loops — note which, plus the on-watch log.

Still expected, not a regression: the **pump** re-pairs after a watch reboot, and after an explicit
phone re-pair. v33 only stopped the pump evicting the phone, not the reverse — full coexistence is
remaining-work item 8.

Also unverified from v18: the runtime SM reconfig (strict LESC in NORMAL, legacy Just Works only in
SPIKE). At step 1 the phone's pairing dialog should be a **numeric confirm / yes-no prompt** — that
is LESC working; a silent Just Works pair means the reconfig didn't take.

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
  menu deletes the SM bond but does **not** clear the spike app's persisted paired flag, leaving
  exactly this FE81-without-a-key mismatch. Always forget the pump with the app's **DOWN**
  button, which clears both.
- Watch on FE82 but a bonded pump ignores it → remove/re-add the device on the pump.
- Rule of thumb: **pump can't see watch → make both do first-pair** (DOWN on watch + remove/add on pump).

## Reading logs

- The on-watch 8-line log in the SAKE Spike app is the primary debugger (no dev kit).
- Vocabulary: `adv EN FE8x tN` (advertising: service + own-addr type), `adv START FAIL 0xNNNN`
  (advertising failed), `HANDSHAKE OK!`, `*** BG … ***`, `wf sender up` / `wf ready ping`
  (watchface link), `paired (persisted): FE81` (boot with a remembered pump).
