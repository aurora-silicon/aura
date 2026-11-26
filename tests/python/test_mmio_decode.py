# SPDX-License-Identifier: MIT
"""tools/auractl/mmio_decode.py on a synthetic auractl dump where AURA_MMIO_TRACE records
(src/sptm_virt_mmio.c) are interleaved with AURA_SEP_TRACE mailbox words and AURA_DCP_AFK_TRACE
records, without the aarch64 toolchain test_mmio_trace_host.py needs.

What it must catch: MMIO records taken for SEP mailbox words by sep_decode.py or dcp_afk_decode.py
(and the reverse: SEP/DCP records shown as MMIO), node names split over several setup records
not joined, an access before its node's setup records (a wrapped ring) dropped instead of shown
by index, a folded poll's count or time lost, the --join/--afk timelines not on the dump's time
base (the first record's) or not merged in time order.
"""
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/auractl/mmio_decode.py"
SEPTOOL = ROOT / "tools/auractl/sep_decode.py"
AFKTOOL = ROOT / "tools/auractl/dcp_afk_decode.py"
SEP_MBOX = 0x25E408000


def tag(s):
    return int.from_bytes(s.encode(), "little")


def rec(t, pa, val, pc, width=3, write=False, cpu=0, marker=False, mmio=None):
    meta = (pa & (1 << 48) - 1) | (width << 48) | (int(write) << 50) | ((cpu & 15) << 51) \
        | (int(marker) << 55)
    if mmio is not None:
        meta |= 1 << 63 | mmio << 58
    return struct.pack("<4Q", t, meta, val, pc)


def name_recs(t, node, name):
    raw = name.encode()
    raw += bytes(-len(raw) % 8)
    return [rec(t, 0, int.from_bytes(raw[i:i + 8], "little"), tag("DMMN") | (i // 8) << 32,
                marker=True, mmio=node) for i in range(0, len(raw), 8)]


def test_mmio_decode_join(tmp_path):
    frq = 1000
    r = []
    # an access of node 1 before any setup record (the ring wrapped past them)
    r.append(rec(1000, 0x23C500004, 0x9, 0xFFFFFE0000001234, width=2, mmio=1))
    r += name_recs(1001, 0, "atc0-dpxbar")
    r.append(rec(1001, 0x38304C000, 0x4000, tag("DMMG") | 0 << 32 | 1 << 40, marker=True, mmio=0))
    # SEP mailbox word (A2I at +0x800) and a DCP AFK mailbox message
    r.append(rec(1002, SEP_MBOX + 0x800, 0x0000000000010E01, 0xFFFFFE0000005555, write=True))
    r.append(rec(1003, 0xFFFFFE0000007777, 0x0089000000000000,
                 tag("DAFM") | 0 << 32 | 1 << 36 | 0x2B << 40))
    # MMIO at page offset 0x800 and 0x830: must not look like SEP words
    r.append(rec(1004, 0x38304C800, 0xAA, 0xFFFFFE0000002000, width=2, write=True, mmio=0))
    r.append(rec(1005, 0x38304C830, 0xBB, 0xFFFFFE0000002004, width=2, mmio=0, cpu=5))
    # a folded poll: shown at its last repeat's time (1009), appended later (1010)
    r.append(rec(1009, 0x38304C830, 7, tag("DMMR"), width=2, marker=True, mmio=0, cpu=5))
    r.append(rec(1010, 0x38304C834, 0xCC, 0xFFFFFE0000002008, width=2, mmio=0))
    r.append(rec(1011, 0, 3, tag("DMMD"), marker=True, mmio=0))
    hdr = struct.pack("<8sII5Q48s", b"AURATRC1", 1, 32, frq, 0, len(r), len(r), 0, b"test")
    dump = tmp_path / "d.trc"
    dump.write_bytes(hdr + b"".join(r))

    out = tmp_path / "mm"
    p = subprocess.run([sys.executable, "-I", str(TOOL), str(dump), str(out), "--afk"],
                       capture_output=True, text=True)
    assert p.returncode == 0, p.stdout + p.stderr
    tl = (out / "timeline.txt").read_text()
    assert "node1 @0x23c500004 R32 0x9 pc 0xfffffe0000001234" in tl, tl
    assert "atc0-dpxbar[0]+0x800 W32 0xaa" in tl
    assert "c5 MMIO atc0-dpxbar[0]+0x830 R32 0xbb" in tl
    assert "atc0-dpxbar[0]+0x830 R32 0xbb repeated 7 more times" in tl
    assert "atc0-dpxbar DROPPED 3 records so far" in tl
    lines = [ln for ln in tl.splitlines() if not ln.startswith("#")]
    assert len(lines) == 6 and not [ln for ln in lines if " MMIO " not in ln], lines
    rep = [ln for ln in lines if "repeated" in ln][0]
    assert abs(float(rep.split()[0]) - 0.009) < 1e-9, rep
    j = (out / "joined.txt").read_text().splitlines()
    body = [ln for ln in j if not ln.startswith("#")]
    ts = [float(ln.split()[0]) for ln in body]
    assert ts == sorted(ts)
    i_sep = next(i for i, ln in enumerate(body) if " SEP A2I " in ln)
    i_dcp = next(i for i, ln in enumerate(body) if "dcp" in ln.lower() and "GETBUF" in ln)
    i_800 = next(i for i, ln in enumerate(body) if "+0x800 W32" in ln)
    assert i_sep < i_dcp < i_800, body
    assert abs(float(body[i_sep].split()[0]) - 0.002) < 1e-9

    # the SEP and DCP decoders see exactly one SEP word and no MMIO record
    sep = tmp_path / "sep"
    subprocess.run([sys.executable, "-I", str(SEPTOOL), str(dump), str(sep)], check=True)
    st = (sep / "timeline.txt").read_text()
    assert st.count(" A2I ") == 1 and " I2A " not in st, st
    afk = tmp_path / "afk"
    subprocess.run([sys.executable, "-I", str(AFKTOOL), str(dump), str(afk), "--sep-mbox",
                    hex(SEP_MBOX)], check=True)
    at = (afk / "timeline.txt").read_text()
    assert at.count(" SEP A2I ") == 1 and "SEP I2A" not in at and "0x38304c" not in at, at
