#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the device tree the booted aura holds and save it: the raw bytes and a full text listing.

Usage: dump_adt.py ADT.bin ADT.txt

The firmware's tree holds the panel's per-unit data (backlight-calibration, panel-gamma, the color
matrices, display-default-color). Read-only: it does not write to the target.
"""
import sys

from aura.proxy import AuraProxy, UartInterface
from aura.proxyutils import ProxyUtils, bootstrap_port


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    utils = ProxyUtils(p)
    raw = utils.get_adt()
    with open(sys.argv[1], "wb") as f:
        f.write(raw)
    with open(sys.argv[2], "w") as f:
        f.write(str(utils.adt))
    print(f"wrote {len(raw)} bytes to {sys.argv[1]} and the listing to {sys.argv[2]}", file=sys.stderr)


if __name__ == "__main__":
    main()
