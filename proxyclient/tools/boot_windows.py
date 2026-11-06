#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Validate or launch a pinned Mu image with the explicit Aura Windows personality."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from aura.windows import launch_profile, plan_profile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("validate", "boot"))
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--device", help="explicit resident Aura serial path; never inferred")
    parser.add_argument("--guest-serial-device", help="distinct dedicated guest CDC port for serial=cdc")
    parser.add_argument("--out", type=Path, help="new retained evidence directory (required for boot)")
    args = parser.parse_args()
    if args.action == "boot":
        if not args.device or not args.out:
            parser.error("boot requires --device and --out")
        result = launch_profile(args.profile, args.device, args.out,
                                guest_serial_device=args.guest_serial_device)
    else:
        plan = plan_profile(args.profile)
        result = {"result": "profile-valid", "hardware_accessed": False,
                  "target": plan.profile["target"], "images": plan.images,
                  "entry_offset": plan.entry_offset, "alignment": 0x10000,
                  "combined_sha256": hashlib.sha256(plan.image).hexdigest()}
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
