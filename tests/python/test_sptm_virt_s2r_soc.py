# SPDX-License-Identifier: MIT
"""The sleep path's SoC blocks, found in a real ADT by src/sptm_virt_s2r_soc.c.

Runs aura's ADT reader (rust/src/adt.rs, built for the host), sptm_virt_s2r_soc.c and pmgr.c's
power-domain walk on the ADTs in SPTM_VIRT_ADTS (one directory per machine with its `dtree`,
named after the board: j700-*, j713-*, j813-*, j616c-*), in the layout iBoot hands over. The debug
link is the port run_guest_sptm.py hides. T8140 and T8132 must give what their old per-chip table
gave.
"""

import importlib.util
import os
import re
import struct
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
ADTS = os.environ.get("SPTM_VIRT_ADTS")

sys.path.insert(0, str(REPO / "proxyclient"))
from aura import sptm_virt_plan as plan_mod  # noqa: E402

# The table sptm_virt_s2r.c used before: pmgr reg indices, PMS PMC pages, the AON wake status,
# the AMCC, the debug link's nodes and the link domain reported off at sleep.
OLD = {
    "j700": dict(chip=0x8140, soc_ps=21, cluster_ctl=28, pwrgate=22, cllt=27, nub_ap=4, aon=1,
                 clkgen=2, soc_misc=24, aon_pst=31, pms_pmc=(0x302800000, 0x3028a8000),
                 nub_wake_status=0x308004028, amcc=0x220025000, amcc_planes=4,
                 link=["/arm-io/usb-drd", "/arm-io/dart-usb", "/arm-io/atc-phy0"], report_off=[]),
    "j713": dict(chip=0x8132, soc_ps=32, cluster_ctl=36, pwrgate=34, cllt=45, nub_ap=4, aon=1,
                 clkgen=2, soc_misc=42, aon_pst=40, pms_pmc=(0x382800000, 0x3828a8000),
                 nub_wake_status=0x388004028, amcc=0,
                 link=["/arm-io/usb-drd0", "/arm-io/dart-usb0", "/arm-io/atc-phy0"],
                 report_off=[0x380700378]),
}
CHIPS = {"j700": 0x8140, "j713": 0x8132, "j813": 0x8142, "j616c": 0x6041}
BLOCKS = ("soc_ps", "cluster_ctl", "pwrgate", "cllt", "nub_ap", "aon", "clkgen", "soc_misc",
          "aon_pst")


def build(tmp):
    rs, src = tmp / "rs", tmp / "src"
    rs.mkdir()
    src.mkdir()
    adt_rs = (REPO / "rust/src/adt.rs").read_text()
    # As tests/run-t6031-handoff.sh: c_char is signed on the host.
    adt_rs = adt_rs.replace(
        "CStr::from_bytes_until_nul(&self.name)",
        "CStr::from_bytes_until_nul(unsafe { &*(&self.name as *const [c_char; 32] "
        "as *const [u8; 32]) })")
    (rs / "adt.rs").write_text(adt_rs)
    (rs / "lib.rs").write_text("#![allow(non_camel_case_types, dead_code, unused_imports)]\n"
                               "type c_size_t = usize;\nmod adt;\n")
    subprocess.run(["rustc", "--edition", "2021", "--crate-type", "staticlib", "-O", "-o",
                    str(rs / "libadt_host.a"), str(rs / "lib.rs")], check=True)
    # The host's own size types; the rest of src/types.h unchanged.
    types = "".join(l for l in (REPO / "src/types.h").read_text().splitlines(keepends=True)
                    if not re.match(r"typedef (u64 uintptr_t|s64 ptrdiff_t|s64 ssize_t);", l))
    (src / "types.h").write_text(types)
    for f in ("tests/t6031_host/utils.h", "tests/t6031_host/memory.h", "src/pmgr.c",
              "src/sptm_virt_s2r_soc.c", "src/sptm_virt_s2r_soc.h",
              "tests/sptm_virt_s2r_soc_host.c"):
        (src / Path(f).name).write_text((REPO / f).read_text())
    out = tmp / "host"
    subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall",
                    "-Wno-unused-function", "-Wno-address-of-packed-member", "-Wno-format",
                    "-include", str(src / "types.h"), f"-I{src}", f"-I{REPO / 'src'}",
                    f"-I{REPO}", str(src / "sptm_virt_s2r_soc.c"), str(src / "pmgr.c"),
                    str(src / "sptm_virt_s2r_soc_host.c"), str(rs / "libadt_host.a"), "-o",
                    str(out)], check=True)
    return out


def link_phys(adt, chip):
    spec = importlib.util.spec_from_file_location("rgs", REPO / "proxyclient/tools/run_guest_sptm.py")
    rgs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rgs)
    return rgs.link_phys(adt, rgs.KIS_PORT_NODES_BY_CHIP.get(chip, rgs.KIS_PORT_NODES))


def pmgr_regs(adt):
    root, _ = plan_mod.adt_parse(adt)
    armio = dict((k, v) for k, v, _f in plan_mod.adt_find(root, "arm-io").props)["ranges"]
    child, parent, _ = struct.unpack_from("<QQQ", armio, 0)
    reg = dict((k, v) for k, v, _f in plan_mod.adt_find(root, "arm-io/pmgr").props)["reg"]
    return [(a + parent - child, s) for a, s in (struct.unpack_from("<QQ", reg, 16 * i)
                                                 for i in range(len(reg) // 16))]


def run(host, tmp, d, chip):
    # An IPSW DeviceTree flags the properties iBoot fills (size bit 31), which a live tree has
    # cleared; aura's reader reads the live layout.
    root, _ = plan_mod.adt_parse((d / "dtree").read_bytes())
    stack = [root]
    while stack:
        node = stack.pop()
        node.props = [[name, val, 0] for name, val, _f in node.props]
        stack += node.kids
    adt = plan_mod.adt_build(root)
    (tmp / d.name).write_bytes(adt)
    phys = [hex(ph) for ph in link_phys(adt, chip)]
    out = subprocess.run([str(host), str(tmp / d.name)] + phys, check=True,
                         capture_output=True, text=True).stdout
    res = {"link": [], "domains": []}
    for line in out.splitlines():
        f = line.split()
        if f[0] == "rc":
            res["rc"], res["missing"] = int(f[1]), " ".join(f[3:])
        elif f[0] in BLOCKS:
            res[f[0]] = (int(f[1], 16), int(f[2], 16))
        elif f[0] == "pms_pmc":
            res["pms_pmc"] = (int(f[1], 16), int(f[2], 16))
        elif f[0] == "nub_wake_status":
            res["nub_wake_status"] = int(f[1], 16)
        elif f[0] == "amcc":
            res["amcc"], res["amcc_planes"] = int(f[1], 16), int(f[2])
        elif f[0] == "link":
            res["link"].append(f[1])
        elif f[0] == "domain":
            res["domains"].append((int(f[2], 16), int(f[4], 16)))
    res["report_off"] = sorted({a for a, fl in res["domains"] if fl & 2})
    return res


@pytest.mark.skipif(not ADTS, reason="real ADTs not given (SPTM_VIRT_ADTS)")
def test_s2r_soc_from_real_adts(tmp_path):
    host = build(tmp_path)
    seen = set()
    for d in sorted(Path(ADTS).iterdir()):
        board = d.name.split("-")[0]
        if not (d / "dtree").exists() or board not in CHIPS or board in seen:
            continue
        seen.add(board)
        res = run(host, tmp_path, d, CHIPS[board])
        print(d.name, res)
        if board not in OLD:
            continue
        old = OLD[board]
        regs = pmgr_regs((d / "dtree").read_bytes())
        assert res["rc"] == 0, res["missing"]
        for b in BLOCKS:
            assert res[b] == regs[old[b]], b
        for k in ("pms_pmc", "nub_wake_status", "amcc", "link", "report_off"):
            assert res[k] == (list(old[k]) if isinstance(old[k], list) else old[k]), k
        if old["amcc"]:
            assert res["amcc_planes"] == old["amcc_planes"]
    assert {"j700", "j713"} <= seen
