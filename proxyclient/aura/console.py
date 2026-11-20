# SPDX-License-Identifier: MIT
"""Record the target's console on the proxy's own device once the proxy has handed off (chainload --vector-only,
linux.py), without a terminal: what arrives is written to a file as it comes, until a deadline, an optional regex,
or the device vanishing (the target resetting). Lines appended to an optional input file are sent to the target,
so a script can type into it.
"""
import os, re, time

import serial


def capture(dev, path, seconds, until=None, input_path=None):
    # Read only what has arrived: a blocking read(n) that the device vanishes under raises and drops the partial
    # chunk, which is the output that matters.
    dev.timeout = 0
    deadline = time.monotonic() + max(0, seconds)
    pattern = re.compile(until.encode()) if until else None
    seen = b""
    sent = 0
    with open(path, "wb") as output:
        while time.monotonic() < deadline:
            if input_path and os.path.exists(input_path):
                with open(input_path, "rb") as f:
                    f.seek(sent)
                    data = f.read()
                for line in data.splitlines(keepends=True):
                    if not line.endswith(b"\n"):
                        break
                    dev.write(line.rstrip(b"\r\n") + b"\r")
                    sent += len(line)
            try:
                data = dev.read(65536)
            except (OSError, serial.SerialException) as e:
                print(f"Capture ended: {e}")
                return
            if data:
                output.write(data)
                output.flush()
                seen = (seen + data)[-4096:]
                if pattern and pattern.search(seen):
                    print(f"Capture ended: console matched {until!r}")
                    return
            else:
                time.sleep(0.01)
    print(f"Capture ended: {seconds} s")
