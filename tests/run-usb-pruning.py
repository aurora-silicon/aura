#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the production pruning function on shared USB PHY/DART fixtures.

Failure modes: disabled OTG prunes a live DRD supplier; skipping all missing
nodes leaves active absent hardware exposed; a present DRD is pruned.
Requires a host C compiler, dtc and libfdt (Homebrew paths supported).
"""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS, MAIN = (ROOT / "tests/usb_pruning_host.c").read_text().split("/* PRODUCTION ROUTINE */")
source = (ROOT / "src/kboot.c").read_text()
start = source.index("static int dt_disable_missing_devs(")
end = source.index("\n__attribute__", start)
with tempfile.TemporaryDirectory(prefix="aura-usb-pruning-") as tmp:
    t = Path(tmp)
    (t / "test.c").write_text(HARNESS + source[start:end] + MAIN)
    subprocess.run([os.environ.get("CC", "cc"), "-I/opt/homebrew/include",
                    str(t / "test.c"), "-L/opt/homebrew/lib", "-lfdt",
                    "-o", str(t / "test")], check=True)
    for disabled in (True, False):
        status = 'status = "disabled";' if disabled else ''
        dts = '/dts-v1/; / { #address-cells=<2>; #size-cells=<2>; soc { #address-cells=<2>; #size-cells=<2>; ranges; phy: phy@703000000 { reg=<7 0x03000000 0 0x1000>; #phy-cells=<1>; }; dart: iommu@702f00000 { reg=<7 0x02f00000 0 0x1000>; #iommu-cells=<1>; }; usb@702280000 { reg=<7 0x02280000 0 0x1000>; phys=<&phy 3>; iommus=<&dart 0>; }; usb@702100000 { reg=<7 0x02100000 0 0x1000>; phys=<&phy 3>; iommus=<&dart 0>; ' + status + ' }; }; };'
        (t / "in.dts").write_text(dts)
        subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(t / "in.dtb"), str(t / "in.dts")], check=True)
        subprocess.run([str(t / "test"), str(t / "in.dtb"), str(t / "out.dtb")], check=True)
        for node in ("phy@703000000", "iommu@702f00000"):
            r = subprocess.run(["fdtget", "-t", "s", str(t / "out.dtb"), "/soc/" + node, "status"], capture_output=True, text=True)
            assert (r.stdout.strip() == "disabled") == (not disabled), (disabled, node, r.stdout)
        r = subprocess.run(["fdtget", "-t", "s", str(t / "out.dtb"), "/soc/usb@702280000", "status"], capture_output=True, text=True)
        assert r.stdout.strip() != "disabled"
print("PASS: disabled OTG preserves shared suppliers; active missing device prunes; present DRD survives")
