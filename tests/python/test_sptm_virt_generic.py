# SPDX-License-Identifier: MIT
"""The virtualized SPTM on every machine with real images: what the host plans, per machine.

SPTM_VIRT_IMAGE_SETS=<dir> holds one directory per board and macOS build (j700-25G83, ...), each
with sptm.bin, txm.bin, kernelcache.* and dtree (an IPSW's DeviceTree). For each set this runs
what run_guest_sptm.py runs before it touches a device: the SPTM patch sites (aura/sptm_patch.py),
the firmware quirks, the debug port and the boot plan, and then the sleep path's SoC blocks
(src/sptm_virt_s2r_soc.c with pmgr.c and aura's ADT reader, rust/src/adt.rs, built for the host).

The J700 on 25G83 must plan byte for byte as it did with its old per-chip tables, and T8140 and
T8132 must find the sleep-path blocks their old table named.
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
SETS = os.environ.get("SPTM_VIRT_IMAGE_SETS")

sys.path.insert(0, str(REPO / "proxyclient"))
from aura import sptm_patch  # noqa: E402
from aura import sptm_virt_plan as plan_mod  # noqa: E402

spec = importlib.util.spec_from_file_location("rgs", REPO / "proxyclient/tools/run_guest_sptm.py")
rgs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rgs)

pytestmark = pytest.mark.skipif(not SETS, reason="real image sets not given (SPTM_VIRT_IMAGE_SETS)")

NOP = 0xD503201F
# What the J700 (25G83) used before: its debug-port nodes and SPTM patches (offset from entry,
# replacement).
J700_PORT = ("/arm-io/nub-spmi-a0/hpm0", "/arm-io/usb-drd", "/arm-io/dart-usb", "/arm-io/atc-phy0",
             "/arm-io/atc0-dpxbar", "/arm-io/atc0-dpphy", "/arm-io/i2c2/usb-repeater-i2c",
             "/arm-io/i2c2/usb-hub-i2c")
J700_PATCHES = [(0x3258, NOP), (0x129ec, 0x14000056), (0x128c0, 0x140000A1), (0x12B1C, NOP),
                (0x12B28, NOP), (0x12B34, NOP), (0x12B40, NOP), (0x39164, NOP), (0x39170, NOP),
                (0x391AC, NOP), (0x391B8, NOP)]
# The sleep path's old table: pmgr reg indices, PMC pages, wake status, AMCC, link, report-off.
S2R_OLD = {
    "j700-25G83": dict(soc_ps=21, cluster_ctl=28, pwrgate=22, cllt=27, nub_ap=4, aon=1, clkgen=2,
                       soc_misc=24, aon_pst=31, pms_pmc=(0x302800000, 0x3028A8000),
                       nub_wake_status=0x308004028, amcc=0x220025000, amcc_planes=4,
                       link=["/arm-io/usb-drd", "/arm-io/dart-usb", "/arm-io/atc-phy0"],
                       report_off=[]),
    "j713-25G83": dict(soc_ps=32, cluster_ctl=36, pwrgate=34, cllt=45, nub_ap=4, aon=1, clkgen=2,
                       soc_misc=42, aon_pst=40, pms_pmc=(0x382800000, 0x3828A8000),
                       nub_wake_status=0x388004028, amcc=0, amcc_planes=0,
                       link=["/arm-io/usb-drd0", "/arm-io/dart-usb0", "/arm-io/atc-phy0"],
                       report_off=[0x380700378], port=1),  # #109 ran on port 1 (AURA_DEBUG_PORT)
}
BLOCKS = ("soc_ps", "cluster_ctl", "pwrgate", "cllt", "nub_ap", "aon", "clkgen", "soc_misc",
          "aon_pst")
DRAM = 0x10000000000
PHYS, END = DRAM + 0x18000000, DRAM + 0x1C6E00000  # the J700's window on the KIS proxy


def image_sets():
    return sorted(d for d in Path(SETS).iterdir() if (d / "dtree").exists())


def plan(d, hide, diag=False):
    sptm, txm = (d / "sptm.bin").read_bytes(), (d / "txm.bin").read_bytes()
    xnu = next(d.glob("kernelcache*")).read_bytes()
    hide = tuple(hide) + rgs.STRIP_NODES + (rgs.ERROR_HANDLER,) + rgs.EXCLAVE_NODES
    return plan_mod.plan_boot(sptm, txm, xnu, (d / "dtree").read_bytes(), dram_base=DRAM,
                              phys_base=PHYS, mem_size=END - PHYS, mem_size_actual=0x200000000,
                              blob_base=PHYS, hide_nodes=hide, drop_props=rgs.ROUTES_PROPS,
                              diag_record=diag)


def host_plan(d):
    """run_guest_sptm.py's steps before the device, proxy on KIS."""
    sptm = (d / "sptm.bin").read_bytes()
    mi = plan_mod.macho_info(sptm)
    sp = sptm_patch.plan(sptm, mi.entry - mi.virtlo)
    _release, quirks = rgs.xnu_quirks(next(d.glob("kernelcache*")).read_bytes())
    phys, port = rgs.debug_port((d / "dtree").read_bytes())
    return sp, phys, port, plan(d, port + tuple(quirks["hide"]), rgs.env_on("AURA_SPTM_DIAG"))


def same(a, b):
    return (a.boot_args, a.adt, a.regions, [(x.what, x.pa, x.size, x.total) for x in a.loads]) == \
           (b.boot_args, b.adt, b.regions, [(x.what, x.pa, x.size, x.total) for x in b.loads])


def test_plan_per_image_set():
    rows = []
    for d in image_sets():
        if not (d / "sptm.bin").exists():
            continue
        try:
            sp, phys, port, p = host_plan(d)
        except sptm_patch.SptmPatchError as e:
            rows.append(f"{d.name}: refused, {e}")
            continue
        rows.append(f"{d.name}: boots: {len(sp['patches'])} patch sites, MTE "
                    f"{'rewrite' if sp['mte'] else 'none'}, CTRR stash 0x{sp['ctrr_stash_off']:x}, "
                    f"{len(phys)} debug port(s), {len(port)} port nodes hidden, "
                    f"blob 0x{p.blob_end - p.blob_start:x}")
        if d.name == "j700-25G83":
            assert port == tuple(sorted(J700_PORT))
            assert [(o, n) for _w, o, _m, _e, n in sp["patches"]] == J700_PATCHES
            assert sp["ctrr_stash_off"] == 0xA50 and sp["mte"] is None
            assert same(p, plan(d, J700_PORT))
    print("\n" + "\n".join(rows))
    assert any(r.startswith("j700-25G83: boots") for r in rows)


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


def s2r(host, tmp, d, port=None):
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
    phys, _port = rgs.debug_port(adt, 0, port)
    out = subprocess.run([str(host), str(tmp / d.name)] + [hex(ph) for ph in phys[:2]],
                         check=True, capture_output=True, text=True).stdout
    res = {"link": [], "domains": [], "skipped": []}
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
        elif f[0] == "skipped":
            res["skipped"].append(" ".join(f[1:]))
        elif f[0] == "domain":
            res["domains"].append((int(f[2], 16), int(f[4], 16)))
    res["report_off"] = sorted({a for a, fl in res["domains"] if fl & 2})
    return res


def pmgr_regs(adt):
    root, _ = plan_mod.adt_parse(adt)
    io = dict((k, v) for k, v, _f in plan_mod.adt_find(root, "arm-io").props)["ranges"]
    child, parent, _ = struct.unpack_from("<QQQ", io, 0)
    reg = dict((k, v) for k, v, _f in plan_mod.adt_find(root, "arm-io/pmgr").props)["reg"]
    return [(a + parent - child, s) for a, s in (struct.unpack_from("<QQ", reg, 16 * i)
                                                 for i in range(len(reg) // 16))]


def test_s2r_soc_per_image_set(tmp_path):
    host = build(tmp_path)
    rows = []
    for d in image_sets():
        res = s2r(host, tmp_path, d, S2R_OLD.get(d.name, {}).get("port"))
        state = "on" if res["rc"] == 0 else f"off (no {res['missing']})"
        skipped = f", not traced: {', '.join(res['skipped'])}" if res["skipped"] else ""
        rows.append(f"{d.name}: S2R {state}{skipped}")
        old = S2R_OLD.get(d.name)
        if not old:
            continue
        regs = pmgr_regs((d / "dtree").read_bytes())
        assert res["rc"] == 0 and not res["skipped"], res
        for b in BLOCKS:
            assert res[b] == regs[old[b]], b
        for k in ("pms_pmc", "nub_wake_status", "amcc", "amcc_planes", "link", "report_off"):
            want = list(old[k]) if isinstance(old[k], list) else old[k]
            assert res[k] == want, k
    print("\n" + "\n".join(rows))
