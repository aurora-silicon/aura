# SPDX-License-Identifier: MIT
"""The ESP macOS boot (SPTM_VIRT_ESP) on the M5 Pro: the C planner against plan_boot.

src/sptm_virt_esp_plan.c ports proxyclient/aura/sptm_virt_plan.py's plan_boot for the configuration
the trace lane's launch.sh runs (run_guest_sptm.py: native layout, exclaves kept, boot video kept,
the debug link's port hidden). Both get the same images, tree and live values here and must give
the same regions, loads, device tree and boot_args. Runs when given the real inputs:

  AURA_ESP_FW    the macOS 26A428 directory (sptm.macho, txm.macho, kernelcache.macho,
                 BuildIdentity.plist, stock/Firmware/...)
  AURA_ESP_TC    the native trust cache
  AURA_ESP_ADTS  live device trees of the Mac, ':'-separated

Standalone: python3 tests/python/test_esp_macos.py
"""

import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from aura import esp_macos  # noqa: E402
from aura import sptm_virt_plan as plan_mod  # noqa: E402

DRAM = 0x10000000000
# The managed window and boot video of run cp8 (2026-11-09, ESP Stage 2 g3410c03b)
PHYS = 0x1001E000000
MEM = 0x10FCA400000 - PHYS
ACTUAL = 0x1000000000
VIDEO = (0x10FD310C000, 0, 0x2F40, 3024, 1964, 30)
ANS_DROP = (("/arm-io/ans/iop-ans-nub", "quiesced"), ("/arm-io/ans/iop-ans-nub", "running"))
_host = None


def host():
    global _host
    if _host:
        return _host
    out = Path(tempfile.mkdtemp()) / "esp_plan_host"
    subprocess.check_call(["clang", "-O1", "-g", "-Wall", "-Wextra", "-Wno-unused-parameter",
                           "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                           "-DESP_PLAN_HOST", f"-I{REPO}/src", "-o", str(out),
                           str(REPO / "tests/esp_plan_host.c"), str(REPO / "src/sptm_virt_esp_plan.c"),
                           str(REPO / "src/sha256.c")])
    _host = out
    return out


def copies_from(adt):
    root, _ = plan_mod.adt_parse(adt)
    props = {n: v for n, v, _f in plan_mod.adt_find(root, "chosen/memory-map").props}
    out = []
    for n in ("RTBuddySeg", "SEPFW", "SEPPatches", "uStuff", "preoslog"):
        v = props.get(n)
        if v and len(v) == 16:
            pa, size = struct.unpack("<QQ", v)
            if size and pa != 2**64 - 1 and not pa & 0x3FFF:
                out.append((n, pa, size))
    return out


def c_plan(bundle, adt, lens, tc, copies, video):
    d = Path(tempfile.mkdtemp())
    (d / "manifest").write_bytes(bundle.manifest.encode())
    (d / "adt").write_bytes(adt)
    p = [f"dram_base {DRAM:x}", f"phys_base {PHYS:x}", f"mem_size {MEM:x}",
         f"mem_size_actual {ACTUAL:x}", f"tc_container {int(esp_macos.is_tc_container(tc))}"]
    if video:
        p.append("video " + " ".join(f"{v:x}" for v in video))
    p += [f"file {k} {v:x}" for k, v in lens.items()]
    p += [f"copy {n} {a:x} {s:x}" for n, a, s in copies]
    (d / "params").write_text("\n".join(p) + "\n")
    r = subprocess.run([host(), "plan", d / "manifest", d / "adt", d / "params", d],
                       capture_output=True, text=True,
                       env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))
    assert r.returncode == 0, r.stdout + r.stderr
    regions = [(a, int(b, 16), int(c, 16)) for a, b, c in
               (ln.split() for ln in (d / "regions.txt").read_text().splitlines())]
    loads = [(int(a, 16), b, int(c, 16), int(e, 16), int(f, 16), int(g)) for a, b, c, e, f, g in
             (ln.split() for ln in (d / "loads.txt").read_text().splitlines())]
    info = dict((a, int(b, 16)) for a, b in
                (ln.split() for ln in (d / "info.txt").read_text().splitlines()))
    return regions, loads, (d / "adt.bin").read_bytes(), (d / "boot_args.bin").read_bytes(), info


def compare(sptm, txm, xnu, tc, adt, exclave, love, *, video=VIDEO, ans_drop=()):
    b = esp_macos.build(sptm, txm, xnu, tc, adt, exclave=exclave, love=love, keep_video=bool(video),
                        ans_drop=ans_drop)
    hide, _phys = esp_macos.hide_nodes(adt, xnu)
    cl4, ce, cv, exic, extc = exclave
    py = plan_mod.plan_boot(
        sptm, txm, xnu, adt, dram_base=DRAM, phys_base=PHYS, mem_size=MEM, mem_size_actual=ACTUAL,
        cmdline=esp_macos.runner().DEFAULT_BOOTARGS, blob_base=PHYS, trustcache=tc,
        iboot_copies=copies_from(adt), hide_nodes=hide, diag_record=False, drop_props=ans_drop,
        xnu_serial="/arm-io/uart0", os_long_version=love, video=video, cl4_regions=cl4.regions,
        cl4_entry=ce, cl4_virt=cv, exclave_integrity=exic, exclave_trustcache=extc)
    lens = {"sptm": len(sptm), "txm": len(txm), "xnu": len(xnu), "tc": len(tc),
            "cl4": len(cl4.payload), "exic": len(exic), "extc": len(extc)}
    regions, loads, c_adt, c_ba, info = c_plan(b, adt, lens, tc, copies_from(adt), video)
    assert regions == [(n, pa, s) for n, (pa, s) in py.regions.items()], (regions, py.regions)
    for k in ("adt_pa", "sptm_pa", "sptm_entry_pa", "txm_pa", "boot_args_pa", "blob_end", "virt_base"):
        assert info[k] == getattr(py, k), (k, hex(info[k]), hex(getattr(py, k)))
    want = []
    for ld in py.loads:
        if ld.image == "bytes" and ld.what == "TrustCache":
            want.append((ld.pa, "tc", 0, len(tc), ld.total, 0 if esp_macos.is_tc_container(tc) else 1))
        elif ld.image == "bytes":
            n = "exic" if "Integrity" in ld.what else "extc"
            want.append((ld.pa, n, 0, len(ld.payload), ld.total, 0))
        elif ld.image == "copy":
            want.append((ld.pa, "copy", ld.fileoff, ld.size, ld.total, 0))
        else:
            want.append((ld.pa, ld.image, ld.fileoff, ld.size, ld.total, 0))
    assert loads == want, (loads, want)
    assert c_ba == py.boot_args
    if c_adt != py.adt:
        for line in adt_diff(c_adt, py.adt)[:12]:
            print("ADT diff:", line)
        n = next((i for i, (x, y) in enumerate(zip(c_adt, py.adt)) if x != y), min(len(c_adt), len(py.adt)))
        raise AssertionError(f"ADT differs at 0x{n:x} (C {len(c_adt)} bytes, Python {len(py.adt)})")
    return py


def adt_diff(a, b):
    """Property-level differences between two trees (C, Python)."""
    out = []

    def walk(x, y, path):
        px = {n: v for n, v, _f in x.props}
        py_ = {n: v for n, v, _f in y.props}
        for n in sorted(set(px) | set(py_)):
            if px.get(n) != py_.get(n):
                fx, fy = px.get(n), py_.get(n)
                out.append(f"{path} {n}: C {fx[:48].hex() if fx is not None else None} "
                           f"Py {fy[:48].hex() if fy is not None else None}")
        kx = {k.name(): k for k in x.kids}
        ky = {k.name(): k for k in y.kids}
        for k in sorted(set(kx) | set(ky)):
            if k in kx and k in ky:
                walk(kx[k], ky[k], path + "/" + k)
            else:
                out.append(f"{path}/{k}: only in {'C' if k in kx else 'Python'}")
        if [k.name() for k in x.kids] != [k.name() for k in y.kids]:
            out.append(f"{path}: child order differs")
        if [n for n, _v, _f in x.props] != [n for n, _v, _f in y.props]:
            out.append(f"{path}: property order differs")

    walk(plan_mod.adt_parse(a)[0], plan_mod.adt_parse(b)[0], "")
    return out


def _real():
    fw, tcp, adts = (os.environ.get(k) for k in ("AURA_ESP_FW", "AURA_ESP_TC", "AURA_ESP_ADTS"))
    if not (fw and tcp and adts):
        return None
    fw = Path(fw)
    st = fw / "stock/Firmware"
    ex = esp_macos.exclaves(st / "image4/exclavecore_bundle.t6050.RELEASE.im4p",
                            st / "043-70822-678.dmg.aea.integrity_catalog",
                            st / "043-70822-678.dmg.aea.trustcache")
    imgs = [(fw / n).read_bytes() for n in ("sptm.macho", "txm.macho", "kernelcache.macho")]
    return (imgs, Path(tcp).read_bytes(), [Path(a).read_bytes() for a in adts.split(":")], ex,
            esp_macos.os_long_version(fw / "BuildIdentity.plist"))


def test_real_plan_matches_python():
    real = _real()
    if not real:
        return "skipped: real inputs not given"
    (sptm, txm, xnu), tc, adts, ex, love = real
    for adt in adts:
        compare(sptm, txm, xnu, tc, adt, ex, love)
        compare(sptm, txm, xnu, tc, adt, ex, love, video=None, ans_drop=ANS_DROP)
    return None


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            print(name, fn() or "ok")
