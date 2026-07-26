#!/usr/bin/env python3
"""Dump the watch's flash-stored logs over the phone connection.

Unlike `pebble logs`, which only streams lines as they happen, this retrieves logs the watch
already wrote to flash -- including from earlier boots. That matters for the MiniMed spike: in
SPIKE mode there is no phone session at all, so nothing can be watched live. Reproduce a problem
in SPIKE, toggle back to NORMAL, and pull the history here.

Generations count backwards: 0 is the current boot, 1 the previous one, and so on.

Caveat worth knowing before you rely on an old generation: PBL_LOG lines are stored *hashed*, and
the hashes change between firmware builds. Reading back a boot logged by an older firmware needs
that firmware's dictionary, so pass `--dict build/sake-spike-vNN-<desc>.loghash.json` (spike-build.sh
archives one next to every .pbz). With the wrong dict those lines stay as raw `NL:xxxx`.

Usage (needs the USB tunnel and Developer Connection, same as `pebble logs`):

    adb forward tcp:9000 tcp:9000
    tools/dump_flash_logs.py              # this boot
    tools/dump_flash_logs.py -g 1         # previous boot
    tools/dump_flash_logs.py -g 0 -o /tmp/boot.log

Protocol: endpoint 2002, request 0x10 <generation:u8> <cookie:u32>; the watch replies with 0x80
per line, then 0x81 when done or 0x82 if that generation holds nothing
(see src/fw/debug/debug.c).
"""

import argparse
import os
import sys
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "log_hashing"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "libs", "pebble-loghash"))

from libpebble2.communication import PebbleConnection
from libpebble2.communication.transports.websocket import WebsocketTransport
from libpebble2.protocol.logs import (
    LogDumpShipping,
    LogMessage,
    LogMessageDone,
    NoLogMessages,
    RequestLogs,
)

# Matches the level letters `pebble logs` prints, from PBL_LOG_LEVEL_*.
LEVELS = {0: "*", 1: "E", 50: "W", 100: "I", 150: "D", 200: "V"}

COOKIE = 0xFEEDFACE

# PBL_LOG lines are stored hashed ("NL:7b18 ..."), so they are unreadable without the dictionary
# the firmware build emits. Without it you still get the lines, just not their text.
DEFAULT_DICT = os.path.join(os.path.dirname(__file__), "..", "build", "pebbleos_loghash_dict.json")


def make_dehasher(dict_path):
    if not os.path.exists(dict_path):
        print("No loghash dictionary at {} -- lines will stay hashed. Build the firmware first."
              .format(dict_path), file=sys.stderr)
        return None
    try:
        import logdehash
    except ImportError as e:
        print("Could not import logdehash ({}); lines will stay hashed.".format(e), file=sys.stderr)
        return None
    # monitor_dict_file spawns a watcher thread we do not need for a one-shot dump.
    return logdehash.LogDehash(dict_path, monitor_dict_file=False)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-g", "--generation", type=int, default=0,
                    help="boot generation: 0 = current boot (default), 1 = previous, ...")
    ap.add_argument("--phone", default="127.0.0.1",
                    help="phone address (default 127.0.0.1, i.e. the adb USB tunnel)")
    ap.add_argument("-o", "--output", help="also write the lines to this file")
    ap.add_argument("-t", "--timeout", type=float, default=30.0,
                    help="give up this many seconds after the last line (default 30)")
    ap.add_argument("--dict", default=DEFAULT_DICT,
                    help="loghash dictionary emitted by the firmware build")
    ap.add_argument("--raw", action="store_true", help="do not dehash; show the stored lines as-is")
    args = ap.parse_args()

    dehasher = None if args.raw else make_dehasher(args.dict)

    connection = PebbleConnection(WebsocketTransport("ws://{}:9000/".format(args.phone)))
    try:
        connection.connect()
    except Exception as e:
        sys.exit("Could not reach the phone at {}:9000 ({}).\n"
                 "Run `adb forward tcp:9000 tcp:9000`, and check the Pebble app has Developer\n"
                 "Connection enabled and the watch connected (NORMAL mode, not SPIKE)."
                 .format(args.phone, e))
    connection.run_async()

    done = threading.Event()
    lines = []

    def handle(packet):
        payload = packet.data
        if isinstance(payload, LogMessage):
            level = LEVELS.get(payload.level, str(payload.level))
            message = payload.message
            if dehasher:
                try:
                    # The dehasher recognises flash/BLE-shipped lines only with this exact prefix
                    # (parse_line keys on ":0> NL:"); without it they fall through unresolved.
                    d = dehasher.dehash(":0> " + message)
                    message = d.get("formatted_msg", message)
                    # A hashed line carries its real origin in the dictionary, not in the record.
                    if d.get("file"):
                        payload.filename, payload.line = d["file"], d.get("line", payload.line)
                except Exception:
                    pass  # keep the hashed form rather than losing the line
            text = "[{}] {}:{} {}".format(level, payload.filename, payload.line, message)
            lines.append(text)
            print(text, flush=True)
        elif isinstance(payload, LogMessageDone):
            done.set()
        elif isinstance(payload, NoLogMessages):
            print("(the watch reports no logs stored for generation {})".format(args.generation),
                  file=sys.stderr)
            done.set()

    connection.register_endpoint(LogDumpShipping, handle)
    connection.send_packet(
        LogDumpShipping(command=0x10,
                        data=RequestLogs(generation=args.generation, cookie=COOKIE)))

    # The watch streams lines with no overall length up front, so treat a quiet period as the end:
    # a dump that is still arriving keeps resetting the deadline.
    seen = -1
    while not done.wait(timeout=args.timeout):
        if len(lines) == seen:
            print("No further lines for {:.0f}s; stopping. The dump may be incomplete."
                  .format(args.timeout), file=sys.stderr)
            break
        seen = len(lines)

    if args.output:
        with open(args.output, "w") as f:
            f.write("\n".join(lines) + "\n")
        print("Wrote {} lines to {}".format(len(lines), args.output), file=sys.stderr)
    elif not lines:
        print("No lines retrieved.", file=sys.stderr)


if __name__ == "__main__":
    main()
