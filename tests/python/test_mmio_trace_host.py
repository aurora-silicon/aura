# SPDX-License-Identifier: MIT
"""End to end on the host: src/sptm_virt_mmio.c (AURA_MMIO_TRACE) set up from a fake ADT, driven
through its own stage-2 hook (tests/run-mmio-trace.sh, tests/mmio_host/harness.c), its records
decoded by tools/auractl/mmio_decode.py, and the same dump read by sep_decode.py and
dcp_afk_decode.py.

What it must catch:
- the node list: a name not parsed (commas, reg index lists 'a-b+c'), an unknown node not
  reported, a coprocessor (iop) node hooked, its pages inside a bigger selected range hooked,
  a DART's page hooked unless that DART is named itself;
- the pages: a range larger than 64 pages hooked without naming its index, a page another tracer
  (or aura) already owns hooked again, a page shared by two ranges or two nodes hooked twice or
  attributed to the wrong node, the page budget overrun, sub-page ranges not rounded to pages;
- the hook: an access not passed through at its width (8/16/32/64, an LDP/STP pair), a read
  not returning the device's value, the guest PC, cpu or direction lost;
- the budget: a polling loop not folded (identical reads) or folded across a change of value,
  a write folded, the per-node cap not applied, applied to other nodes, drops not counted;
- the decoders: an access shown against the wrong reg index or offset, an access outside the
  node's declared ranges not flagged, sep_decode/dcp_afk_decode taking MMIO records for SEP
  mailbox words (an MMIO page offset of 0x800), the joined timeline out of time order.
The harness checks passthrough of every access itself (exit status).
"""
import pathlib
import shutil
import subprocess

import pytest

ROOT = pathlib.Path(__file__).resolve().parents[2]


def test_mmio_trace_host(tmp_path):
    if not shutil.which("aarch64-linux-gnu-gcc"):
        pytest.skip("no aarch64-linux-gnu-gcc")
    if not shutil.which("qemu-aarch64") and not pathlib.Path("/proc/sys/fs/binfmt_misc").exists():
        pytest.skip("no qemu-aarch64")
    out = tmp_path / "run"
    p = subprocess.run([str(ROOT / "tests/run-mmio-trace.sh"), str(out)], cwd=ROOT,
                       capture_output=True, text=True, timeout=600)
    if p.returncode == 77:
        pytest.skip(p.stdout)
    assert p.returncode == 0, p.stdout + p.stderr
    log = p.stderr
    assert "nosuch: not in the ADT" in log
    assert "dcp: a coprocessor node; not traced" in log
    assert "i2c0: page 0x50300000 not hooked: already hooked or not mapped" in log
    assert "dispext0 reg[0] 0x50c00000 +0x200000: 128 pages, over 64; name it dispext0/0" in log
    assert "dispext0: page 0x50404000 not hooked: page budget (131) used up" in log
    assert "7 nodes, 131 pages, at most 50 records per node" in log

    tl = (out / "out/timeline.txt").read_text()
    lines = tl.splitlines()

    def line(*parts):
        hits = [ln for ln in lines if all(x in ln for x in parts)]
        assert hits, (parts, tl)
        return hits

    def nothing(*parts):
        hits = [ln for ln in lines if all(x in ln for x in parts)]
        assert not hits, (parts, hits)

    # setup: ranges and what was refused
    line("# MMIO node 0 dptx-phy reg[0] 0x50040000 +0xc000: 3 pages hooked")
    line("# MMIO node 0 dptx-phy reg[1] 0x50000000 +0x4000: 1 pages hooked")
    line("# MMIO node 2 atc-phy0 reg[1] 0x50204000 +0x200: 1 pages hooked")
    line("# MMIO node 2 atc-phy0 reg[2] 0x50204200 +0x200: 0 pages hooked")  # shares reg[1]'s
    line("# MMIO node 2 atc-phy0 reg[3] 0x50100000 +0x4000: 0 pages hooked")  # atc0-dpxbar's
    nothing("atc-phy0 reg[4]")  # not in the index list
    line("# MMIO node 3 disp0 reg[1] 0x50800000 +0x200000: 123 pages hooked")
    line("# MMIO node 3 disp0 page 0x50880000 not hooked: coprocessor or DART registers")
    line("# MMIO node 3 disp0 page 0x508c0000 not hooked: coprocessor or DART registers")
    line("# MMIO node 4 i2c0 page 0x50300000 not hooked: already hooked or not mapped")
    line("# MMIO node 5 dart-disp0 reg[0] 0x508c0000 +0x4000: 1 pages hooked")
    line("# MMIO node 6 dispext0 reg[0] 0x50c00000 +0x200000: too big (128 pages), not hooked")
    line("# MMIO node 6 dispext0 page 0x50404000 not hooked: page budget used up")
    nothing("dcp reg[")

    # accesses, passed through and attributed
    line("dptx-phy[0]+0x10 W32 0x11223344", "pc 0xfffffe0000a01000")
    line("dptx-phy[0]+0x10 R32 0x11223344", "c3")
    polls = line("dptx-phy[1]+0x4 R32 0x1 pc")
    assert len(polls) == 1, polls
    line("dptx-phy[1]+0x4 R32 0x1 repeated 4 more times")
    line("dptx-phy[1]+0x4 R32 0x3 ")
    line("dptx-phy[1]+0x8 W32 0x5 ")
    assert len(line("dptx-phy[1]+0x8 W32 0x5 ")) == 2  # writes are never folded
    line("dptx-phy[1]+0xc R32 0x6 pc")
    line("dptx-phy[1]+0xc W32 0x6 pc")
    line("atc-phy0[1]+0x10 R8 0x5a")
    line("atc-phy0[1]+0x12 W16 0xbeef")
    line("atc-phy0[2]+0x8 R64 0x123456789abcdef0")
    line("atc-phy0 outside its ranges @0x50205000 R32 0x77")
    line("atc0-dpxbar[0]+0x20 W64 0x1111111122222222")
    line("atc0-dpxbar[0]+0x28 W64 0x3333333344444444")
    line("atc0-dpxbar[0]+0x30 R64 0x5555555566666666")
    line("atc0-dpxbar[0]+0x38 R64 0x7777777788888888")
    line("dart-disp0[0]+0x0 R32 0xd")
    # the flood: 50 records for disp0, then drops; dptx-phy still recorded after it
    assert len([ln for ln in lines if " disp0[1]+" in ln]) == 50
    assert len(line("disp0 DROPPED")) == 1
    line("disp0 DROPPED 10 records so far")
    line("# MMIO disp0: 50 records, 0 repeats folded, 10 dropped")
    line("dptx-phy[0]+0x14 W32 0xabc")
    line("# MMIO dptx-phy:", "4 repeats folded, 0 dropped")
    ts = [float(ln.split()[0]) for ln in lines if not ln.startswith("#")]
    assert ts == sorted(ts)

    # the CSV has every access record
    csv = (out / "out/mmio.csv").read_text().splitlines()
    assert csv[0].startswith("time,cpu,node,reg,offset,dir,bits,value,pc,repeat")
    assert any(",dptx-phy,1,0x4,R,32,0x1,0xfffffe0000a01000,4" in c for c in csv), csv[:20]

    # the other decoders skip MMIO records (an MMIO page offset of 0x800 is in the dump)
    sep = (out / "sep/timeline.txt").read_text()
    assert " A2I " not in sep and " I2A " not in sep, sep
    afk = (out / "afk/timeline.txt").read_text()
    assert " SEP A2I " not in afk and " SEP I2A " not in afk, afk
    joined = (out / "out/joined.txt").read_text().splitlines()
    jts = [float(ln.split()[0]) for ln in joined if not ln.startswith("#")]
    assert jts == sorted(jts) and len(jts) >= len(ts)
