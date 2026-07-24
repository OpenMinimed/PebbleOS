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

## Verify the v18 phone-bond fix (PENDING — not yet deliberately tested)

v18's runtime SM reconfig (strict LESC in NORMAL, legacy Just Works only in SPIKE) is bundled
in v20 but has never been verified on its own — v20's HW pass only covered the pump side.

1. NORMAL mode. Forget the watch on the phone and re-pair once (the old bond was formed under
   the weak SM config, so one re-pair is expected). The pairing dialog should be a numeric
   confirm/yes-no prompt — that's LESC working; a silent Just Works pair means the reconfig
   didn't take.
2. Cycle: SPIKE → wait for `HANDSHAKE OK!` → back to NORMAL → phone should reconnect within
   ~a minute with **no re-pair prompt** and no connect/terminate(0x13) loop.
3. Repeat step 2 three times. **Pass:** phone reconnects every cycle without re-pairing.
   **Fail:** any cycle needs a re-pair, or the bond loops — note which, plus the on-watch log.

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
