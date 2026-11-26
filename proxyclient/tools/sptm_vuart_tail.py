#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tail aura's CDC Pipe 1 (*-if02, IODEV_USB_VUART) for live XNU serial output.

Raising DTR on CDC Pipe 1 marks pipe[1].ready in aura's DWC3 gadget so XNU's
uart0 console streams live and DWC3 events keep draining Pipe 0 (*-if00).
"""

import argparse
import glob
import os
import signal
import sys
import time
from pathlib import Path


def find_vuart_device():
    for path in sorted(glob.glob("/dev/serial/by-id/*aura_uartproxy*-if02")):
        resolved = os.path.realpath(path)
        if os.path.exists(resolved):
            return resolved
    return None


def tail_vuart(device, out_stream, stop_file=None, timeout=0.0, serial_cls=None,
               sleep_fn=time.sleep, clock_fn=time.monotonic):
    if serial_cls is None:
        import serial
        serial_cls = serial.Serial

    stop_path = Path(stop_file) if stop_file else None
    deadline = (clock_fn() + timeout) if timeout and timeout > 0 else None
    stop_flag = {"stopped": False}

    def _handle_sig(_signum, _frame):
        stop_flag["stopped"] = True

    old_int = signal.getsignal(signal.SIGINT)
    old_term = signal.getsignal(signal.SIGTERM)
    try:
        signal.signal(signal.SIGINT, _handle_sig)
        signal.signal(signal.SIGTERM, _handle_sig)
    except ValueError:
        old_int = old_term = None

    ser = None
    try:
        while not stop_flag["stopped"]:
            if stop_path and stop_path.exists():
                out_stream.write(b"\n--- vuart stop file seen ---\n")
                out_stream.flush()
                break
            if deadline is not None and clock_fn() >= deadline:
                out_stream.write(b"\n--- vuart timeout reached ---\n")
                out_stream.flush()
                break
            try:
                if ser is None or not ser.is_open:
                    dev_path = device or find_vuart_device()
                    if not dev_path:
                        sleep_fn(0.25)
                        continue
                    ser = serial_cls(dev_path, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
                    ser.dtr = True
                    ser.rts = True
                    ts = time.strftime("%H:%M:%S")
                    out_stream.write(f"--- vuart opened {dev_path} at {ts} (DTR=1) ---\n".encode())
                    out_stream.flush()
                n = getattr(ser, "in_waiting", 0)
                data = ser.read(max(1, min(4096, n)))
                if data:
                    out_stream.write(data)
                    out_stream.flush()
            except KeyboardInterrupt:
                break
            except Exception as exc:  # noqa: BLE001
                ts = time.strftime("%H:%M:%S")
                out_stream.write(f"\n--- vuart err {exc} at {ts} ---\n".encode())
                out_stream.flush()
                try:
                    if ser is not None:
                        ser.close()
                except Exception:  # noqa: BLE001
                    pass
                ser = None
                sleep_fn(0.5)
    finally:
        try:
            if ser is not None:
                ser.close()
        except Exception:  # noqa: BLE001
            pass
        if old_int is not None:
            signal.signal(signal.SIGINT, old_int)
        if old_term is not None:
            signal.signal(signal.SIGTERM, old_term)


def parse_args(argv=None):
    p = argparse.ArgumentParser(
        description="Tail aura CDC Pipe 1 (*-if02) vuart with DTR=1 for live guest serial output."
    )
    p.add_argument("--device", default=os.environ.get("AURA_VUART_DEVICE", ""),
                   help="Serial device path (default: auto-detect *aura_uartproxy*-if02)")
    p.add_argument("-o", "--output", default="-",
                   help="Output log file path, or '-' for stdout (default: -)")
    p.add_argument("--stop-file", default="",
                   help="Stop cleanly when this file appears")
    p.add_argument("--timeout", type=float, default=0.0,
                   help="Optional maximum run time in seconds (0 = unlimited)")
    return p.parse_args(argv)


def main(argv=None):
    args = parse_args(argv)
    device = args.device or None
    stop_file = args.stop_file or None
    if args.output == "-":
        tail_vuart(device, sys.stdout.buffer, stop_file=stop_file, timeout=args.timeout)
    else:
        with open(args.output, "wb", buffering=0) as f:
            tail_vuart(device, f, stop_file=stop_file, timeout=args.timeout)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
