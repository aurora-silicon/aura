#!/usr/bin/env python3
"""Read J613's cache-cleaned entry record from fresh Stage 1, then return to Linux."""
import argparse
import json
import platform

from aura.proxy import AuraProxy, UartInterface


MAGIC = 0x4A363133454E5452  # J613ENTR


def decode_record(words):
    if len(words) != 8:
        raise ValueError("entry record must be eight words")
    magic, sequence, stage, ticks, cpu, check, reserved0, reserved1 = words
    if magic != MAGIC:
        raise ValueError(f"entry record magic mismatch: {magic:#x}")
    if check != MAGIC ^ sequence ^ stage:
        raise ValueError("entry record check word mismatch")
    return dict(magic="J613ENTR", sequence=sequence, stage=stage,
                ticks=ticks, cpu=cpu, reserved=[reserved0, reserved1])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--address", required=True, type=lambda x: int(x, 0))
    parser.add_argument("--reboot", action="store_true",
                        help="send one ordinary P_REBOOT after the read")
    args = parser.parse_args()
    if platform.node() != "robur":
        raise RuntimeError("robur only")
    if args.address & 0xfff or not 0x10000000000 <= args.address < 0x10400000000:
        raise ValueError("address is not a page-aligned J613 DRAM address")

    iface = UartInterface()
    proxy = AuraProxy(iface)
    if proxy.get_chipid() != 0x8122:
        raise RuntimeError("not T8122/J613")
    record = decode_record([proxy.read64(args.address + 8 * i) for i in range(8)])
    record["address"] = args.address
    print(json.dumps(record, sort_keys=True), flush=True)
    if args.reboot:
        proxy.reboot()


if __name__ == "__main__":
    main()
