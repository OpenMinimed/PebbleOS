#!/usr/bin/env python3
"""Flash a firmware .pbz onto the watch from the PC, no tapping in the Pebble app.

The transport is the phone's Developer Connection over the adb USB tunnel, the same one
fetch_coredump.py and dump_flash_logs.py use. The phone's Pebble app must be running with
Developer Connection enabled and the watch connected (NORMAL mode), but the app's UI is never
touched and the .pbz does not need to be copied to the phone.

The watch has two firmware slots. The firmware must be flashed to the slot that is NOT running,
and the .pbz must be the one linked for that slot (build/minimed-...-<desc>_slot0.pbz or _slot1.pbz).

Usage:
    tools/flash_firmware.py build/minimed-pt2-v4.36.9-2-g2f4c3a272-predictor-score
        Give the path without _slotN.pbz: the running slot is read from the watch's log and the
        other slot's .pbz is flashed.
    tools/flash_firmware.py build/minimed-...-predictor-score_slot0.pbz
        Flash exactly that file (slot taken from its name; --slot overrides it).

The watch reboots into the new firmware by itself when the transfer completes. Ctrl-C or a
dropped link during the transfer leaves the running firmware untouched.
"""

import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def run_slot_from_logs(phone):
    """The slot the watch booted from, per the current boot's log; None if the log has no line.

    Log lines are stored hashed, so the boot line only reads back with the running build's
    dictionary. Try the newest dictionaries in build/ until one decodes it.
    """
    dicts = sorted(glob.glob(os.path.join(HERE, "..", "build", "*.loghash.json")),
                   key=os.path.getmtime, reverse=True)[:8]
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "log.txt")
        for dict_path in dicts:
            res = subprocess.run(
                [sys.executable, os.path.join(HERE, "dump_flash_logs.py"), "-g", "0", "--phone",
                 phone, "--dict", dict_path, "-o", out],
                check=False, capture_output=True, text=True)
            try:
                text = open(out, errors="replace").read()
            except OSError:
                print((res.stdout + res.stderr).strip())
                return None
            m = re.findall(r"Boot slot: (\d)", text)
            if m:
                return int(m[-1])
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", help=".pbz file, or the path without the _slotN.pbz suffix")
    ap.add_argument("--slot", type=int, choices=[0, 1],
                    help="slot to flash (default: from the file name, else the slot not running)")
    ap.add_argument("--phone", default="127.0.0.1",
                    help="phone address (default 127.0.0.1, the adb USB tunnel)")
    ap.add_argument("--dry-run", action="store_true", help="resolve slot and file, do not flash")
    ap.add_argument("--no-forward", action="store_true", help="do not run adb forward")
    args = ap.parse_args()

    path = args.firmware
    named = re.search(r"_slot([01])\.pbz$", path)
    slot = args.slot
    if named and slot is None:
        slot = int(named.group(1))

    if not args.no_forward and args.phone == "127.0.0.1":
        subprocess.run(["adb", "forward", "tcp:9000", "tcp:9000"], check=True,
                       stdout=subprocess.DEVNULL)

    if slot is None:
        running = run_slot_from_logs(args.phone)
        if running is None:
            print("Could not read the running slot from the watch log; pass --slot N.")
            return 1
        slot = 1 - running
        print("Watch runs slot {}: flashing slot {}.".format(running, slot))

    if not path.endswith(".pbz"):
        path = "{}_slot{}.pbz".format(path, slot)
    elif not named:
        pass
    elif int(named.group(1)) != slot:
        path = re.sub(r"_slot[01]\.pbz$", "_slot{}.pbz".format(slot), path)
        print("File name says slot {}, switching to {}".format(named.group(1), path))
    if not os.path.exists(path):
        print("No such file: {}".format(path))
        return 1

    print("Flashing {} to slot {} ...".format(path, slot))
    cmd = ["pebble", "fw", "--phone", args.phone, "install", "--slot", str(slot), path]
    if args.dry_run:
        print("Dry run: " + " ".join(cmd))
        return 0
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    sys.exit(main())
