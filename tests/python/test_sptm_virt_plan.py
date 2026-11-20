# SPDX-License-Identifier: MIT
"""The iBoot->SPTM handoff planner (proxyclient/aura/sptm_virt_plan.py).

Synthetic images always run; real ones (25G83, 26A428) run when they are present
(set SPTM_VIRT_IMAGES=<dir with sptm.bin txm.bin kernelcache dtree>).
"""

import importlib.util
import os
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("sptm_virt_plan",
                                              REPO / "proxyclient/aura/sptm_virt_plan.py")
plan_mod = importlib.util.module_from_spec(spec)
sys.modules["sptm_virt_plan"] = plan_mod
spec.loader.exec_module(plan_mod)

PAGE = plan_mod.PAGE
DRAM = 0x10000000000
PHYS = DRAM + 0x342C000
END = DRAM + 0x1E6600000  # aura_base on the Neo's live numbers
STRIDE = plan_mod.SPTM_EXPECTED_STRIDE


def macho(segs, entry):
    """segs: [(name, vmaddr, vmsize, filesize)] laid out flat, fileoff = vmaddr - lowest."""
    lo = min(s[1] for s in segs)
    cmds = b""
    for name, vm, size, fsz in segs:
        cmds += struct.pack("<II16sQQQQIIII", 0x19, 72, name.encode(), vm, size, vm - lo, fsz,
                            7, 7, 0, 0)
    thread = struct.pack("<IIII", 5, 16 + 68 * 4, 6, 68) + b"\0" * (32 * 8) + \
        struct.pack("<Q", entry) + b"\0" * (68 * 4 - 32 * 8 - 8)
    cmds += thread
    hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, len(segs) + 1, len(cmds), 0, 0)
    body = hdr + cmds
    end = max(s[2] + (s[1] - lo) for s in segs)
    return body.ljust(max(end, len(body)), b"\0")


def adt_blob():
    def node(props, kids=()):
        n = plan_mod.RawNode()
        n.props = [[k, v, 0] for k, v in props]
        n.kids = list(kids)
        return n

    def region(name):
        return (name, struct.pack("<QQ", 2**64 - 1, 2**64 - 1))

    names = ["TXM-ro", "TXM-rx", "TXM-bx", "TXM-rw", "TXM-le", "TXM-entry", "TXM-virt", "TrustCache",
             "BootKC-rx", "BootKC-bx", "BootKC-ro", "BootKC-rs", "BootKC-rw", "BootKC-le",
             "BootKC-virt", "BootKC-entry", "DeviceTree", "SPTM-ro", "SPTM-rm", "SPTM-rx", "SPTM-rw",
             "SPTM-le", "SPTM-entry", "SPTM-virt", "BootArgs", "SPTMDebug", "CL4-dummypage",
             "RAMDisk",
             "slide", "CL4-entry", "CL4-virt", "CL4-rx", "CL4-ro"]
    mm = node([("name", b"memory-map\0")] + [region(n) for n in names])
    carve = node([("name", b"carveout-memory-map\0"),
                  ("region-id-24", struct.pack("<QQ", PHYS, 0x1E3A04000))])
    chosen = node([("name", b"chosen\0")], [mm, carve])
    root = node([("name", b"device-tree\0")], [chosen])
    return plan_mod.adt_build(root)


def images():
    sptm = macho([("__TEXT", 0xFFFFFFF027004000, 0x10000, 0x10000),
                  ("__DATA_CONST", 0xFFFFFFF027014000, 0x8000, 0x8000),
                  ("__LATE_CONST", 0xFFFFFFF02701C000, 0x4000, 0x4000),
                  ("__TEXT_EXEC", 0xFFFFFFF02709C000, 0x60000, 0x60000),
                  ("__LAST", 0xFFFFFFF0270FC000, 0x4000, 0x4000),
                  ("__DATA", 0xFFFFFFF027100000, 0x10000, 0x8000),
                  ("__BOOTDATA", 0xFFFFFFF027110000, 0x14000, 0),
                  ("__LINKEDIT", 0xFFFFFFF027124000, 0x4000, 0x20)], 0xFFFFFFF0270AC388)
    txm = macho([("__TEXT", 0xFFFFFFF017004000, 0x10000, 0x10000),
                 ("__DATA_CONST", 0xFFFFFFF017014000, 0x10000, 0x10000),
                 ("__TEXT_EXEC", 0xFFFFFFF017024000, 0x44000, 0x44000),
                 ("__TEXT_BOOT_EXEC", 0xFFFFFFF017068000, 0xC000, 0xC000),
                 ("__DATA", 0xFFFFFFF017074000, 0x4000, 0x4000),
                 ("__LINKEDIT", 0xFFFFFFF017078000, 0x4000, 0x20)], 0xFFFFFFF017070000)
    xnu = macho([("__TEXT", 0xFFFFFE0007004000, 0x8000, 0x8000),
                 ("__PRELINK_TEXT", 0xFFFFFE000700C000, 0x40000, 0x40000),
                 ("__DATA_CONST", 0xFFFFFE000704C000, 0x20000, 0x20000),
                 ("__DATA_SPTM", 0xFFFFFE000706C000, 0x8000, 0x8000),
                 ("__TEXT_EXEC", 0xFFFFFE0007074000, 0x80000, 0x80000),
                 ("__TEXT_BOOT_EXEC", 0xFFFFFE00070F4000, 0x8000, 0x8000),
                 ("__PRELINK_INFO", 0xFFFFFE00070FC000, 0x8000, 0x8000),
                 ("__DATA", 0xFFFFFE0007104000, 0x10000, 0x8000),
                 ("__LINKEDIT", 0xFFFFFE0007114000, 0x10000, 0x100)], 0xFFFFFE00070F4000)
    return sptm, txm, xnu


def run(sptm, txm, xnu, adt, phys=PHYS, diag_record=True):
    return plan_mod.plan_boot(sptm, txm, xnu, adt, dram_base=DRAM, phys_base=phys,
                              mem_size=END - phys, mem_size_actual=0x200000000,
                              diag_record=diag_record)


def check(plan, sptm, adt, phys=PHYS):
    PHYS = phys  # noqa: N806 - the window start under test
    # Everything lives inside SPTM's window, page aligned, and never overlaps.
    spans = sorted((ld.pa, ld.pa + ld.total) for ld in plan.loads)
    for pa, end in spans:
        assert pa % PAGE == 0 and PHYS <= pa and end <= END
    for (a0, a1), (b0, b1) in zip(spans, spans[1:]):
        assert a1 <= b0, "blob pieces overlap"
    assert plan.boot_args_pa % PAGE == 0
    assert plan.blob_end <= END

    # TXM must not sit exactly at physBase (PHYS_SLIDE).
    assert plan.txm_pa > PHYS

    # Entry derivation: LC_UNIXTHREAD pc relative to the image base.
    si = plan_mod.macho_info(sptm)
    assert plan.sptm_entry_pa == plan.sptm_pa + (si.entry - si.virtlo)
    assert plan.sptm_pa == plan.regions["SPTM-ro"][0]

    # boot_args: what SPTM's __start loads from x0.
    rev, ver = struct.unpack_from("<HH", plan.boot_args, 0)
    assert (rev, ver) == ((3, 2) if plan.diag_pa else (2, 2))
    virt, phys, size, top = struct.unpack_from("<QQQQ", plan.boot_args, 8)
    assert phys == PHYS and size == END - PHYS and top == plan.blob_end
    assert virt == plan.virt_base
    adt_va, adt_len = struct.unpack_from("<QI", plan.boot_args, 0x60)
    assert adt_va - virt == plan.adt_pa - PHYS and adt_len == len(plan.adt)
    assert plan.boot_args[0x6C:0x6C + 11] == b"serial=3 -v"
    # Revision 3: the diagnostic-record VA at +0x480, in the window, its own blob region.
    # Without a record, __start still reads +0x480 and must find zero.
    (diag,) = struct.unpack_from("<Q", plan.boot_args, 0x480)
    assert len(plan.boot_args) == 0x488
    assert diag == plan.diag_pa  # a PA: SPTM zeroes the record with its MMU off
    if plan.diag_pa:
        assert plan.regions["SPTMDebug"] == (plan.diag_pa, 0x4000)
    else:
        assert "SPTMDebug" not in plan.regions
    # SPTM runs at sptm_pa + virtBase - physBase: that VA must be page aligned.
    assert (plan.sptm_pa - PHYS + virt) % PAGE == 0

    # The edited ADT parses back with every region set, and region-24 is shortened.
    root, _ = plan_mod.adt_parse(plan.adt)
    mm = plan_mod.adt_find(root, "chosen/memory-map")
    got = {n: struct.unpack("<QQ", v) for n, v, _f in mm.props if len(v) == 16}
    for name, val in plan.regions.items():
        assert got[name] == val, name
    carve = plan_mod.adt_find(root, "chosen/carveout-memory-map")
    r24 = [v for n, v, _f in carve.props if n == "region-id-24"][0]
    assert struct.unpack("<QQ", r24) == (PHYS, END - PHYS)
    # SPTM-ro/rx/rw/le are slices of the one SPTM load.
    for k in ("SPTM-ro", "SPTM-rx", "SPTM-rw", "SPTM-le"):
        pa, sz = plan.regions[k]
        assert plan.sptm_pa <= pa and pa + sz <= plan.sptm_pa + (si.virthi - si.virtlo)


def test_synthetic_plan():
    sptm, txm, xnu = images()
    adt = adt_blob()
    plan = run(sptm, txm, xnu, adt)
    check(plan, sptm, adt)
    # A complete memory map gets nothing inserted; the carve-out becomes its own region.
    assert not any(n.startswith("inserted missing") for n in plan.notes)
    carve = _carve(plan)
    aura = [v for k, v in carve.items() if k not in ("region-id-24",)]
    assert aura == [(END, PHYS + 0x1E3A04000 - END)]


def test_missing_memory_map_property_is_inserted():
    sptm, txm, xnu = images()
    root, _ = plan_mod.adt_parse(adt_blob())
    mm = plan_mod.adt_find(root, "chosen/memory-map")
    mm.props = [p for p in mm.props if p[0] != "SPTM-rw"]
    plan = run(sptm, txm, xnu, plan_mod.adt_build(root))
    assert plan.adt_grew and any("SPTM-rw" in n for n in plan.notes)


def test_window_too_small_is_refused():
    sptm, txm, xnu = images()
    with pytest.raises(ValueError):
        plan_mod.plan_boot(sptm, txm, xnu, adt_blob(), dram_base=DRAM, phys_base=PHYS,
                           mem_size=0x1000000, mem_size_actual=0x200000000)


@pytest.mark.parametrize("phys", [DRAM + 0x1A000000, DRAM + 0x1A000000 + 0x3FFC000, DRAM + 0x342C000])
def test_window_raised_above_aura(phys):
    """aura's image/heap sit at the bottom of iBoot's window; SPTM's window starts above them."""
    sptm, txm, xnu = images()
    adt = adt_blob()
    plan = run(sptm, txm, xnu, adt, phys)
    check(plan, sptm, adt, phys)
    first_mapped = (phys + 0x2000000) & ~0x1FFFFFF
    assert plan.txm_pa >= first_mapped  # TXM below SPTM's first 32 MiB block has no mapping
    low = min(ld.pa for ld in plan.loads)
    assert low >= phys, "nothing may be written below the SPTM window"


@pytest.mark.skipif(not os.environ.get("SPTM_VIRT_IMAGES"), reason="real images not given")
@pytest.mark.parametrize("diag_record", (False, True))
def test_real_images(diag_record):
    d = Path(os.environ["SPTM_VIRT_IMAGES"])
    sptm, txm = (d / "sptm.bin").read_bytes(), (d / "txm.bin").read_bytes()
    xnu = next(d.glob("kernelcache*")).read_bytes()
    adt = (d / "dtree").read_bytes()
    plan = run(sptm, txm, xnu, adt, diag_record=diag_record)
    check(plan, sptm, adt)
    assert plan.sptm_entry_pa - plan.sptm_pa == 0xA8388  # __start in __TEXT_EXEC


def _carve(plan):
    root, _ = plan_mod.adt_parse(plan.adt)
    node = plan_mod.adt_find(root, "chosen/carveout-memory-map")
    return {n: struct.unpack("<QQ", v) for n, v, _f in node.props if len(v) == 16}


def test_native_layout_copies_iboot_regions():
    """iBoot's data regions follow the images, copied device-side, inside the window."""
    sptm, txm, xnu = images()
    phys = DRAM + 0x1A000000
    copies = [("RTBuddySeg", DRAM + 0x6898000, 0x88000), ("SEPFW", DRAM + 0x626C000, 0x5DC000)]
    plan = plan_mod.plan_boot(sptm, txm, xnu, adt_blob(), dram_base=DRAM, phys_base=phys,
                              mem_size=END - phys, mem_size_actual=0x200000000, blob_base=phys,
                              iboot_copies=copies)
    check(plan, sptm, adt_blob(), phys)
    got = [(ld.what, ld.fileoff, ld.size) for ld in plan.loads if ld.image == "copy"]
    assert [(w.split()[0], src, size) for w, src, size in got] == copies
    assert all(phys <= ld.pa < END for ld in plan.loads if ld.image == "copy")
    assert plan.regions["SEPFW"][1] == 0x5DC000


def test_exclave_layout():
    """With cL4, SPTM gets laid-out CL4 regions and the data window's read-only copy."""
    sptm, txm, xnu = images()
    regions = {"CL4-rx": (0, 0x8000), "CL4-rw": (0x8000, 0x4000), "CL4-le": (0xC000, 0x4000),
               "CL4-ro": (0x10000, 0x4000)}
    plan = plan_mod.plan_boot(sptm, txm, xnu, adt_blob(), dram_base=DRAM, phys_base=PHYS,
                              mem_size=END - PHYS, mem_size_actual=0x200000000,
                              cl4_regions=regions, cl4_entry=0xFFFFFC0000000800,
                              cl4_virt=0xFFFFFC0000000000, exclave_integrity=b"i" * 0x1234,
                              exclave_trustcache=b"t" * 0x100)
    check(plan, sptm, adt_blob())
    r = plan.regions
    assert r["CL4-entry"] == (0xFFFFFC0000000800, 0) and r["CL4-virt"] == (0xFFFFFC0000000000, 0)
    assert r["CL4-rx"][0] + r["CL4-rx"][1] == r["CL4-ro"][0]  # SPTM wants CL4-ro after CL4-rx
    rw, le = r["CL4-rw"], r["CL4-le"]
    assert le[0] == rw[0] + rw[1]
    assert le[1] == 0x4000 + 0x4000  # CL4-le covers the read-only tail's copy
    assert any(ld.what == "CL4-ro (data-window copy)" and ld.pa == le[0] + 0x4000
               for ld in plan.loads)
    assert "ExclaveOSIntegrityCatalog" in r and "ExclaveOSTrustCache" in r
