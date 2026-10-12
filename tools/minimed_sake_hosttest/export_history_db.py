#!/usr/bin/env python3
"""Dump a PythonPumpConnector history.db as one hex record per line, in log order.

    ./export_history_db.py ../../../medtronic_new/PythonPumpConnector/database/history.db > history.hex

`make replay HISTORY=history.hex` then runs the firmware's history decoding and glucose predictor
over it and scores the 30-minute predictions against the readings that followed.
"""
import sqlite3
import sys

con = sqlite3.connect(f"file:{sys.argv[1]}?mode=ro", uri=True)
for seq, raw in con.execute("SELECT seq_number, raw_data FROM history_records ORDER BY seq_number"):
    print(raw)
