#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the supported-Mac list and print shell KEY=VALUE lines for one model.

Kept in Python so the JSON parsing is not done with sed. Used by preflight.sh; the
1TR enrol/un-enrol steps never call this, so they need no Python.

    supported.py <supported.json> <model-id>

Prints (always HVI_SUPPORTED=0|1 first):
    HVI_SUPPORTED=1
    HVI_SOC=T6041
    HVI_MARKETING='M4 Pro'
    HVI_EXCLAVES=0|1
    HVI_SELF_BOOT=qualifying|no|...
    HVI_SELF_BOOT_NOTE='...'
    HVI_BUILDS='26A428 ...'
    HVI_STAGE2_FLAGS='...'
Exit status is 0 whether or not the model is supported (the caller reads HVI_SUPPORTED);
non-zero only on a bad/missing file.
"""
import json
import shlex
import sys


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: supported.py <supported.json> <model-id>")
    path, model = sys.argv[1], sys.argv[2]
    try:
        with open(path) as f:
            db = json.load(f)
    except (OSError, ValueError) as e:
        sys.exit(f"supported.py: cannot read {path}: {e}")

    soc = db.get("models", {}).get(model)
    out = []
    if not soc or soc not in db.get("socs", {}):
        out.append("HVI_SUPPORTED=0")
        out.append(f"HVI_MODEL={shlex.quote(model)}")
        print("\n".join(out))
        return
    s = db["socs"][soc]
    out.append("HVI_SUPPORTED=1")
    out.append(f"HVI_SOC={shlex.quote(soc)}")
    out.append(f"HVI_MARKETING={shlex.quote(s.get('marketing', soc))}")
    out.append(f"HVI_EXCLAVES={1 if s.get('exclaves') else 0}")
    out.append(f"HVI_SELF_BOOT={shlex.quote(s.get('self_boot', 'no'))}")
    out.append(f"HVI_SELF_BOOT_NOTE={shlex.quote(s.get('self_boot_note', ''))}")
    out.append(f"HVI_BUILDS={shlex.quote(' '.join(s.get('builds_tested', [])))}")
    out.append(f"HVI_STAGE2_FLAGS={shlex.quote(s.get('stage2_build_flags', ''))}")
    print("\n".join(out))


if __name__ == "__main__":
    main()
