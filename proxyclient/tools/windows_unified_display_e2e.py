#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host-only E2E for the unified display selection (docs/windows-unified-display-launcher.md).

Runs the real profile admission and the real HV.setup_adt()/_windows_adt_blob() of this tree and of
2e3a4e6b side by side on the same synthetic ADTs, in separate processes:
- key absent: the guest ADT is byte-identical to 2e3a4e6b's (default, cpufreq, cpufreq+E state);
- key present on the J813 target: exactly one /chosen property is added, 01 00 00 00;
- an inherited marker is removed when the key is absent and replaced when present;
- wrong chip, wrong board, missing board, wrong model, wrong value, wrong type all refuse;
- the launcher tells both HVs and the preflight runs before the target is touched;
- a real boot profile (boot 148's) still passes admission, with and without the key.
Then rejects mutations of the change. Nothing touches hardware."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

TOOLS = Path(__file__).resolve().parent
PROXYCLIENT = TOOLS.parent
ROOT = PROXYCLIENT.parent
BASE = "2e3a4e6bdb9a099a35e8595e00be702f3051a9cd"
PROPERTY = "aura,windows-unified-display-v1"
PROFILE_148 = Path(os.environ.get("AURA_BOOT148_PROFILE") or
                   sys.exit("windows_unified_display_e2e.py: set AURA_BOOT148_PROFILE to boot 148's profile.json"))

# Runs inside a child process with one proxyclient tree on sys.path.
CHILD = r'''
import json, struct, sys
from types import SimpleNamespace
sys.path.insert(0, sys.argv[1])
from aura.adt import load_adt
from aura.hv import HV
from aura.proxy import IODEV
cfg = json.loads(sys.argv[3])
blob = open(sys.argv[2], "rb").read()
hv = HV(None, None, SimpleNamespace(ba=SimpleNamespace(devtree_size=0x10000)), windows=True)
hv.adt = load_adt(blob)
hv.iodev = IODEV.UART
hv.emulate_sprr = False
hv.windows_usb, hv.windows_n1, hv.windows_mtp = "disabled", "disabled", "disabled"
hv.windows_cpufreq = cfg["cpufreq"]
if "unified" in cfg:
    hv.windows_unified_display = cfg["unified"]
try:
    hv.setup_adt()
    print(json.dumps({"ok": True, "blob": hv._windows_adt_blob().hex()}))
except ValueError as error:
    print(json.dumps({"ok": False, "error": str(error)}))
'''

ADMIT = r'''
import json, sys
sys.path.insert(0, sys.argv[1])
from aura.windows import plan_profile
try:
    plan = plan_profile(sys.argv[2])
    print(json.dumps({"ok": True, "unified": getattr(plan, "unified_display", None), "cpufreq": plan.cpufreq,
                      "ecpu": plan.ecpu_boot_state}))
except ValueError as error:
    print(json.dumps({"ok": False, "error": str(error)}))
'''

# The real HardwareSession.prepare() over a model transport: the identity ADT is a fixture, and the first
# target mutation (bootstrap_port) is recorded; HV.init() of the launch HV stops the run.
PREPARE = r'''
import json, sys
from types import SimpleNamespace
sys.path.insert(0, sys.argv[1])
import aura.proxyutils as proxyutils
from aura import windows
from aura.hv import HV
from aura.proxy import IODEV
events = []
class Proxy:
    def hv_windows_capabilities(self): return windows.CAPS_MAGIC | windows.CAPS_ABI << 16 | windows.REQUIRED_CAPS
    def iodev_whoami(self): return IODEV.UART
class Utils:
    def __init__(self, *args, **kwargs): events.append("proxy-heap")
class Stop(Exception): pass
def init(hv):
    events.append("launch-hv=" + str(getattr(hv, "windows_unified_display", None)))
    raise Stop()
proxyutils.bootstrap_port = lambda *args: events.append("bootstrap-port")
proxyutils.ProxyUtils = Utils
HV.init = init
session = object.__new__(windows.HardwareSession)
session.iface, session.proxy, session.hv, session.serial_capture, session.xfer_receipt = None, Proxy(), None, None, None
session._identity_bootargs = SimpleNamespace(devtree_size=0x10000)
session._identity_adt_blob = open(sys.argv[3], "rb").read()
plan = windows.plan_profile(sys.argv[2])
try:
    session.prepare(plan)
    result = "returned"
except Stop:
    result = "stopped"
except ValueError as error:
    result = "refused: " + str(error)
print(json.dumps({"result": result, "events": events}))
'''

MUTATIONS = [  # (name, file, old, new)
    ("marker-kept-when-absent", "aura/hv/__init__.py",
     "            chosen._properties.pop(UNIFIED_DISPLAY_PROPERTY, None)\n", ""),
    ("board-unchecked", "aura/windows.py",
     'require((chip, board, model) == (0x8142, 0x24, "Mac17,3"),\n            "the unified display device',
     'require((chip, model) == (0x8142, "Mac17,3"),\n            "the unified display device'),
    ("marker-eight-bytes", "aura/windows.py",
     '            "the unified display device requires the exact J813 target")\n    return struct.pack("<I", 1)',
     '            "the unified display device requires the exact J813 target")\n    return struct.pack("<Q", 1)'),
    ("marker-big-endian", "aura/windows.py",
     '            "the unified display device requires the exact J813 target")\n    return struct.pack("<I", 1)',
     '            "the unified display device requires the exact J813 target")\n    return struct.pack(">I", 1)'),
    ("live-board-ignored", "aura/hv/__init__.py",
     "            unified = unified_display_marker(self.windows_unified_display, chosen.chip_id,\n"
     "                                             getattr(chosen, \"board_id\", None), self.adt.model)\n",
     "            unified = unified_display_marker(self.windows_unified_display, chosen.chip_id,\n"
     "                                             0x24, self.adt.model)\n"),
    ("disabled-accepted", "aura/windows.py",
     'require(profile["unified_display"] == "j813-v1" and type(profile["unified_display"]) is str,',
     'require(profile["unified_display"] in ("j813-v1", "disabled"),'),
    ("bool-accepted", "aura/windows.py",
     'require(profile["unified_display"] == "j813-v1" and type(profile["unified_display"]) is str,',
     'require(profile["unified_display"] in ("j813-v1", True, 1),'),
    ("profile-target-unchecked", "aura/windows.py",
     '        require(chip == 0x8142 and model == "Mac17,3",\n                "j813-v1 unified display requires',
     '        require(True,\n                "j813-v1 unified display requires'),
    ("key-not-admitted", "aura/windows.py", '"xfer", "ecpu_boot_state", "unified_display"},', '"xfer", "ecpu_boot_state"},'),
    ("preflight-not-told", "aura/windows.py",
     "        preflight.windows_unified_display = plan.unified_display\n", ""),
    ("launch-hv-not-told", "aura/windows.py",
     "        self.hv.windows_unified_display = plan.unified_display\n", ""),
    ("marker-never-written", "aura/hv/__init__.py",
     "                chosen._properties[UNIFIED_DISPLAY_PROPERTY] = unified\n", "                pass\n"),
]


def sha(data):
    return hashlib.sha256(data).hexdigest()


def run(code, *argv):
    result = subprocess.run([sys.executable, "-B", "-c", code, *map(str, argv)], capture_output=True, text=True,
                            timeout=300)
    if result.returncode:
        raise RuntimeError("child failed: " + result.stderr[-800:])
    return json.loads(result.stdout.strip().splitlines()[-1])


def fixtures(work):
    sys.path.insert(0, str(TOOLS))
    sys.path.insert(0, str(PROXYCLIENT))
    import windows_hv_e2e as hve
    from construct import Array, Int32ul
    blobs = {}
    adt, _, _ = hve.fixture()
    blobs["default"] = adt.build()

    def j813(chip=0x8142, board=0x24, model="Mac17,3", inherited=None, board_present=True):
        adt, _, _ = hve.fixture()
        adt.model = model
        adt["/chosen"].chip_id = chip
        if board_present:
            adt["/chosen"].board_id = board
        ladders = ((972000, 1152000, 1584000, 1992000, 2352000, 2700000, 2964000, 3048000),
                   (1308000, 1608000, 1932000, 2220000, 2508000, 2820000, 3072000, 3300000, 3516000, 3720000,
                    3876000, 4020000, 4176000, 4248000, 4272000, 4332000, 4428000, 4416000, 4464000))
        for name, ladder in zip(("voltage-states1-sram", "voltage-states5-sram"), ladders):
            words = [word for khz in ladder for word in (khz, 900)]
            adt["/arm-io/pmgr"]._properties[name] = words
            adt["/arm-io/pmgr"]._types[name] = (Array(len(words), Int32ul), False)
        if inherited is not None:
            adt["/chosen"]._properties[PROPERTY] = inherited
        return adt.build()

    blobs["j813"] = j813()
    blobs["j813-inherited"] = j813(inherited=b"\x02\x00\x00\x00")
    blobs["j813-inherited-short"] = j813(inherited=b"\x01")
    blobs["wrong-board"] = j813(board=0x23)
    blobs["no-board"] = j813(board_present=False)
    blobs["wrong-chip"] = j813(chip=0x8132)
    blobs["wrong-model"] = j813(model="Mac17,1")
    paths = {}
    for name, blob in blobs.items():
        paths[name] = work / ("adt-" + name + ".bin")
        paths[name].write_bytes(blob)
    return paths


def without_marker(blob_hex):
    from aura.adt import load_adt
    adt = load_adt(bytes.fromhex(blob_hex))
    # Raw bytes as the guest reads them (the parser would show a 4-byte value as an integer).
    raw = [p["value"] for p in adt["/chosen"].tostruct()["properties"] if p["name"] == PROPERTY]
    adt["/chosen"]._properties.pop(PROPERTY, None)
    adt["/chosen"]._types.pop(PROPERTY, None)
    return (raw[0] if len(raw) == 1 else raw or None), adt.build()


def check_tree(tree, old, paths, work):
    """All checks against one proxyclient tree; returns the list of passed checks or raises."""
    passed = []

    def check(ok, name):
        if not ok:
            raise AssertionError(name)
        passed.append(name)

    cfgs = {"off": {"cpufreq": "disabled"}, "cpufreq": {"cpufreq": "j813-manual-v1"}}
    for fixture, cfg in (("default", "off"), ("j813", "off"), ("j813", "cpufreq")):
        new = run(CHILD, tree, paths[fixture], json.dumps(cfgs[cfg]))
        base = run(CHILD, old, paths[fixture], json.dumps(cfgs[cfg]))
        check(new["ok"] and base["ok"] and new["blob"] == base["blob"],
              f"key absent: guest ADT byte-identical to {BASE[:8]} ({fixture}, {cfg})")
    for cfg in ("off", "cpufreq"):
        on = run(CHILD, tree, paths["j813"], json.dumps(dict(cfgs[cfg], unified="j813-v1")))
        base = run(CHILD, old, paths["j813"], json.dumps(cfgs[cfg]))
        check(on["ok"], f"key present on the J813 target is accepted ({cfg})")
        value, stripped = without_marker(on["blob"])
        check(value == b"\x01\x00\x00\x00" and stripped.hex() == base["blob"],
              f"key present adds exactly /chosen {PROPERTY} = 01 00 00 00 ({cfg})")
    for fixture in ("j813-inherited", "j813-inherited-short"):
        off = run(CHILD, tree, paths[fixture], json.dumps(cfgs["off"]))
        clean = run(CHILD, tree, paths["j813"], json.dumps(cfgs["off"]))
        check(off["ok"] and off["blob"] == clean["blob"], f"an inherited marker is removed when the key is absent ({fixture})")
        on = run(CHILD, tree, paths[fixture], json.dumps(dict(cfgs["off"], unified="j813-v1")))
        check(on["ok"] and without_marker(on["blob"])[0] == b"\x01\x00\x00\x00",
              f"an inherited marker is replaced by 01 00 00 00 when selected ({fixture})")
    for fixture in ("wrong-board", "no-board", "wrong-chip", "wrong-model"):
        on = run(CHILD, tree, paths[fixture], json.dumps(dict(cfgs["off"], unified="j813-v1")))
        check(not on["ok"] and "J813" in on["error"], f"the marker refuses on {fixture}")
        off = run(CHILD, tree, paths[fixture], json.dumps(cfgs["off"]))
        base = run(CHILD, old, paths[fixture], json.dumps(cfgs["off"]))
        check(off["ok"] == base["ok"] and off.get("blob") == base.get("blob"),
              f"key absent on {fixture}: same as {BASE[:8]}")

    # Profile admission.
    firmware = work / "mu.fd"
    if not firmware.exists():
        firmware.write_bytes(bytes(range(256)) * 32)
    baseline = {"schema": "aura.windows.boot.v1", "name": "unified-fixture",
                "target": {"chip_id": "0x8142", "model": "Mac17,3"},
                "mu": {"path": firmware.name, "sha256": sha(firmware.read_bytes()), "entry_offset": "0x800"},
                "framebuffer": "native"}
    cases = [("absent", None, None, True, "disabled"), ("j813-v1", "j813-v1", None, True, "j813-v1"),
             ("disabled", "disabled", None, False, None), ("other-string", "j813-v2", None, False, None),
             ("upper-case", "J813-V1", None, False, None), ("empty", "", None, False, None),
             ("true", True, None, False, None), ("one", 1, None, False, None), ("null", None, "null", False, None),
             ("object", {"v": 1}, None, False, None), ("wrong-chip", "j813-v1", "chip", False, None),
             ("wrong-model", "j813-v1", "model", False, None)]
    for name, value, special, ok, unified in cases:
        profile = json.loads(json.dumps(baseline))
        if name != "absent":
            profile["unified_display"] = value
        if special == "chip":
            profile["target"]["chip_id"] = "0x8132"
        if special == "model":
            profile["target"]["model"] = "Mac17,1"
        path = work / ("profile-" + name + ".json")
        path.write_text(json.dumps(profile))
        result = run(ADMIT, tree, path)
        check(result["ok"] == ok and (not ok or result["unified"] == unified),
              f"admission: {name} {'accepted' if ok else 'refused'}")
    if PROFILE_148.exists():
        real = run(ADMIT, tree, PROFILE_148)
        check(real["ok"] and real["unified"] == "disabled" and real["cpufreq"] == "j813-manual-v1" and real["ecpu"] == 6,
              "boot 148's profile still passes admission unchanged (cpufreq, ecpu_boot_state 6, legacy display)")
        keyed = json.loads(PROFILE_148.read_text())
        keyed["unified_display"] = "j813-v1"
        path = work / "profile-148-keyed.json"
        path.write_text(json.dumps(keyed))
        real = run(ADMIT, tree, path)
        check(real["ok"] and real["unified"] == "j813-v1", "boot 148's profile with the key passes admission")

    # The real prepare(): the preflight refuses a wrong board before the first target mutation, and
    # both HVs receive the selection.
    keyed, plain = work / "profile-j813-v1.json", work / "profile-absent.json"
    for profile, fixture, expected in (
            (keyed, "j813", ["bootstrap-port", "proxy-heap", "launch-hv=j813-v1"]),
            (plain, "j813", ["bootstrap-port", "proxy-heap", "launch-hv=disabled"]),
            (plain, "wrong-board", ["bootstrap-port", "proxy-heap", "launch-hv=disabled"]),
            (keyed, "wrong-board", []), (keyed, "no-board", [])):
        result = run(PREPARE, tree, profile, paths[fixture])
        ok = (result["result"] == "stopped" if expected else
              result["result"].startswith("refused:") and "J813" in result["result"])
        check(ok and result["events"] == expected,
              f"prepare() with {profile.stem} on {fixture}: {'reaches the launch HV' if expected else 'refuses before the target is touched'}")
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    out = parser.parse_args().out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    receipt = {"hardware_accessed": False, "base": BASE, "mutations": []}
    try:
        old = out / "base"
        old.mkdir()
        archive = subprocess.run(["git", "-C", str(ROOT), "archive", BASE, "proxyclient"], capture_output=True,
                                 check=True).stdout
        subprocess.run(["tar", "-x", "-C", str(old)], input=archive, check=True)
        old_tree = old / "proxyclient"
        work = out / "work"
        work.mkdir()
        paths = fixtures(work)
        receipt["checks"] = check_tree(str(PROXYCLIENT), str(old_tree), paths, work)
        for name, rel, before, after in MUTATIONS:
            mutant = out / ("mutant-" + name)
            shutil.copytree(PROXYCLIENT, mutant / "proxyclient", ignore=shutil.ignore_patterns("__pycache__"))
            target = mutant / "proxyclient" / rel
            text = target.read_text()
            if text.count(before) != 1:
                raise ValueError("mutation anchor not unique: " + name)
            target.write_text(text.replace(before, after))
            try:
                check_tree(str(mutant / "proxyclient"), str(old_tree), paths, work)
            except (AssertionError, RuntimeError, ValueError) as error:
                receipt["mutations"].append({"name": name, "rejected": True, "reason": str(error)[:200]})
                continue
            raise RuntimeError(name + " mutant was not rejected")
        receipt["verdict"] = "PASS"
    except Exception as error:
        receipt["verdict"] = "FAIL"
        receipt["error"] = f"{type(error).__name__}: {error}"
    (out / "result.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"verdict": receipt["verdict"], "checks": len(receipt.get("checks", [])),
                      "mutants_rejected": len(receipt["mutations"])} |
                     ({"error": receipt["error"]} if "error" in receipt else {})))
    return receipt["verdict"] != "PASS"


if __name__ == "__main__":
    raise SystemExit(main())
