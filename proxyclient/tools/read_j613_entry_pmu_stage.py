#!/usr/bin/env python3
"""Read the one-byte J613 entry stage mirrored into PMU scratch by firmware."""
import argparse
import json
import platform
import subprocess
from pathlib import Path

from aura.proxy import AuraProxy, UartInterface


def symbols(elf: Path, nm: str) -> dict[str, int]:
    result = {}
    for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
        fields = line.split()
        if len(fields) == 3:
            result[fields[2]] = int(fields[0], 16)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path)
    parser.add_argument("--nm", default="llvm-nm")
    parser.add_argument("--reboot", action="store_true")
    args = parser.parse_args()
    if platform.node() != "robur":
        raise RuntimeError("robur only")

    sym = symbols(args.elf, args.nm)
    needed = {"_base", "sptm_virt_j613_entry_pmu_stage"}
    if not needed <= sym.keys():
        raise RuntimeError(f"missing symbols: {sorted(needed - sym.keys())}")
    iface = UartInterface()
    proxy = AuraProxy(iface)
    try:
        if proxy.get_chipid() != 0x8122:
            raise RuntimeError("not T8122/J613")
        base = proxy.get_base()
        function = base + sym["sptm_virt_j613_entry_pmu_stage"] - sym["_base"]
        stage = proxy.call(function)
        if stage > 0xff:
            raise RuntimeError("firmware PMU stage read failed")
        print(json.dumps(dict(stage=stage, stage_hex=f"0x{stage:02x}"), sort_keys=True),
              flush=True)
    finally:
        if args.reboot:
            proxy.reboot()


if __name__ == "__main__":
    main()
