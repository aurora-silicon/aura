# SPDX-License-Identifier: MIT
"""End to end on the host: src/sptm_virt_dcp_afk.c set up from a fake ADT and driven through its
own mailbox hook (tests/run-dcp-afk-trace.sh, tests/dcp_afk_host/harness.c), its records decoded
by tools/auractl/dcp_afk_decode.py.

What it must catch: a DCP not found or the wrong one hooked (exclave mailbox, a page another
tracer owns, a non-T8110 DART), DART tables in the pt-region above guest RAM not walked (J473
27.2), buffer pages outside guest RAM read, GETBUF/INIT_TX/INIT_RX not turned into rings, ring
entries not copied or copied from the wrong offset, the wrap (dummy header, entry at 0) missed,
the byte budget not applied, OOB buffers not followed, endpoints outside the mask recorded, and
bad pointers or magic not reported. The harness itself checks passthrough of every access.
"""
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_dcp_afk_trace_host(tmp_path):
    if not shutil.which("aarch64-linux-gnu-gcc"):
        pytest.skip("no aarch64-linux-gnu-gcc")
    if not shutil.which("qemu-aarch64") and not pathlib.Path("/proc/sys/fs/binfmt_misc").exists():
        pytest.skip("no qemu-aarch64")
    out = tmp_path / "run"
    p = subprocess.run([str(ROOT / "tests/run-dcp-afk-trace.sh"), str(out)], cwd=ROOT,
                       capture_output=True, text=True, timeout=600)
    if p.returncode == 77:
        pytest.skip(p.stdout)
    assert p.returncode == 0, p.stdout + p.stderr
    log = p.stderr
    assert "pt-region-5 [0x13000000, 0x14000000)" in log
    assert "dcpext: pt-region-0 [0x7000000000, 0x7000100000) outside DRAM; ignored" in log
    assert "dcpext0: mailbox" in log and "already hooked" in log
    assert "dcpext1: no T8110" in log
    assert "dcp-exclave-mailbox" not in log
    assert "2 DCPs, endpoints mask 0x800" in log

    tl = (out / "out/timeline.txt").read_text()
    lines = tl.splitlines()

    def line(*parts):
        hits = [ln for ln in lines if all(x in ln for x in parts)]
        assert hits, (parts, tl)
        return hits

    line("DCP 0 = dcp mailbox")
    line("DCP 1 = dcpext mailbox")
    for d in ("dcp", "dcpext"):
        line(f"{d} ep 0x2b I2A GETBUF size 0x8000 tag 0x7")
        line(f"{d} ep 0x2b A2I GETBUF_ACK dva 0x800104000")
        line(f"{d} ep 0x2b RING tx bufsz 0x3f40 stride 0x40 dva 0x800104000")
        line(f"{d} ep 0x2b RING rx bufsz 0x3f40 stride 0x40 dva 0x800108000")
        line(f"{d} ep 0x2b DCP->AP qe@0x0", "REPORT type 0x11", "dcpdptx-hdcp-interface")
        c = line(f"{d} ep 0x2b AP->DCP qe@0x0", "COMMAND type 0xc0", "token 0x77")[0]
        assert "oob id 0x12 value 0x40 rx 0x800200000/0x40 tx 0x800300004/0x5" in c
        assert "payload 2: aabb" in c
        line(f"{d} ep 0x2b AP->DCP OOB-rx dva 0x800200000 64 bytes: c0c1c2c3")
        line(f"{d} ep 0x2b AP->DCP OOB-tx dva 0x800300004 5 bytes: 0102030405")
    # dcp's tables live in the pt-region only: its entries above prove the walk read them there
    # the wrap on dcpext: the fill entry cut to the budget, then the entry at 0, nothing at 0x3f00
    line("dcpext ep 0x2b DCP->AP qe@0xc0", "TRUNCATED 4112 of 15936")
    w = line("dcpext ep 0x2b DCP->AP qe@0x0", "RESPONSE type 0xc0", "token 0x77")[0]
    assert "payload 32: " + "33" * 32 in w
    assert not [ln for ln in lines if "qe@0x3f00" in ln], tl
    line("dcpext ep 0x2b PROBLEM 3 bad entry magic (0x80)")
    line("dcpext ep 0x2b PROBLEM 2 ring pointer out of range (0x100000041)")
    line("dcp ep 0x00 PROBLEM 6", "(0x13000000)")
    line("dcp ep 0x00 PROBLEM 7", "(0x13800000)")
    assert " ep 0x37 " not in tl
    # the timeline is in time order
    ts = [float(ln.split()[0]) for ln in lines if not ln.startswith("#")]
    assert ts == sorted(ts)
