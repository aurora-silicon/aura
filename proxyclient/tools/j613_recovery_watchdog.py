#!/usr/bin/env python3
"""Arm the firmware-only J613 recovery watchdog and deliberately stop."""
import argparse
import platform

from aura.proxy import AuraProxy, UartInterface


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=int, default=30)
    args = parser.parse_args()
    if platform.node() not in ("alba", "robur"):
        raise RuntimeError("alba or robur only")
    if not 1 <= args.seconds <= 178:
        raise ValueError("seconds must be in [1, 178]")

    iface = UartInterface()
    proxy = AuraProxy(iface)
    if proxy.get_chipid() != 0x8122:
        raise RuntimeError("not T8122/J613")
    proxy.wdt_recovery(args.seconds)
    print(f"J613 recovery watchdog requested for {args.seconds} s")


if __name__ == "__main__":
    main()
