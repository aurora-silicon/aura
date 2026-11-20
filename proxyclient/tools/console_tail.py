#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read a device's console into a log file, read-only: it never writes to the device.

Keeps capturing a running guest's console after run_guest_sptm.py's own tail has ended,
for example XNU's console on the CDC vuart port. Reopens the serial device when it goes away
(it disappears across a reset) and timestamps each line.
"""

import argparse
import glob
import time

import serial


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("logfile")
    ap.add_argument("--glob", default="/dev/cu.kis-*",
                    help="device pattern, e.g. /dev/cu.usbmodemP_03 for XNU's console over CDC")
    ap.add_argument("--idle-exit", type=float, default=0.0,
                    help="exit after this many seconds of silence (0: never)")
    ap.add_argument("--max-secs", type=float, default=0.0,
                    help="exit after this many seconds (0: never)")
    args = ap.parse_args()

    out = open(args.logfile, "ab", buffering=0)
    t0 = last = time.time()
    buf = b""
    dev = None
    try:
        while True:
            now = time.time()
            if args.max_secs and now - t0 > args.max_secs:
                out.write(f"--- console_tail: {args.max_secs:g}s elapsed, exiting ---\n".encode())
                return
            if args.idle_exit and now - last > args.idle_exit:
                out.write(f"--- console_tail: silent for {args.idle_exit:g}s, exiting ---\n".encode())
                return
            if dev is None:
                devs = sorted(glob.glob(args.glob))
                if not devs:
                    time.sleep(1)
                    continue
                try:
                    dev = serial.Serial(devs[0], timeout=1)
                    out.write(f"--- console_tail: opened {devs[0]} at {time.ctime()} ---\n".encode())
                except (serial.SerialException, OSError):
                    time.sleep(1)
                    continue
            try:
                chunk = dev.read(4096)
            except (serial.SerialException, OSError) as e:
                out.write(f"--- console_tail: read error ({e}); reopening ---\n".encode())
                try:
                    dev.close()
                except Exception:
                    pass
                dev = None
                continue
            if not chunk:
                continue
            last = time.time()
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                out.write(b"[%.3f] " % last + line.rstrip(b"\r") + b"\n")
    finally:
        if buf:
            out.write(b"[%.3f] " % time.time() + buf.rstrip(b"\r") + b"\n")


if __name__ == "__main__":
    main()
