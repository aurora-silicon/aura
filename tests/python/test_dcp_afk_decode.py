# SPDX-License-Identifier: MIT
"""tools/auractl/dcp_afk_decode.py against a synthetic auractl dump of AURA_DCP_AFK_TRACE records
(src/sptm_virt_dcp_afk.c) interleaved with AURA_SEP_TRACE records (src/sptm_virt_sep.c).

What it must catch: DCP and SEP records confused with each other, data words joined to the
wrong copy, copies not cut to their byte count, wrong offsets in the queue entry, AFKv2 and
classic EPIC headers, an OOB copy's alignment, service names from announcements, truncation
not flagged, and sep_decode.py misreading DCP records as SEP messages.
"""
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/auractl/dcp_afk_decode.py"
SEPTOOL = ROOT / "tools/auractl/sep_decode.py"
SEP_MBOX = 0x25E408000

TAG = {k: int.from_bytes(k.encode(), "little") for k in ("DAFM", "DAFR", "DAFH", "DAFD", "DAFX")}
SEPH, SEPD = 0x48504553, 0x44504553


def rec(t, pa, val, pc, write=False, cpu=0, marker=False):
    meta = pa | (3 << 48) | (int(write) << 50) | ((cpu & 15) << 51) | (int(marker) << 55)
    return struct.pack("<4Q", t, meta, val, pc)


class Ring:
    def __init__(self):
        self.recs = []
        self.t = 0
        self.seq = 0

    def add(self, *a, **k):
        self.t += 24
        self.recs.append(rec(self.t, *a, **k))

    def mbox(self, dcp, ep, i2a, m0, elr=0x123456):
        self.add(elr, m0, TAG["DAFM"] | dcp << 32 | i2a << 36 | ep << 40, write=not i2a, cpu=2)

    def copy(self, dcp, ep, ring, kind, pa, data, full=None, skip=0, drop_last=False):
        self.seq += 1
        n = len(data)
        val = dcp | ring << 4 | kind << 5 | ep << 8 | n << 16 | (full if full else n) << 40
        self.add(pa, val, TAG["DAFH"] | self.seq << 32, write=ring == 0)
        raw = bytes(skip) + data
        raw += bytes(-len(raw) % 8)
        words = [raw[i:i + 8] for i in range(0, len(raw), 8)]
        if drop_last:
            words = words[:-1]
        for i, w in enumerate(words):
            self.add(i * 8, int.from_bytes(w, "little"), TAG["DAFD"] | self.seq << 32)

    def dump(self, path):
        hdr = struct.pack("<8sII5Q48s", b"AURATRC1", 1, 32, 24, 0, len(self.recs),
                          len(self.recs), 0, b"test")
        path.write_bytes(hdr + b"".join(self.recs))


def qe(body, channel=0, etype=0):
    return struct.pack("<4I", 0x20504F49, len(body), channel, etype) + body


def v2(seq, intf, cat, typ, payload, token=0, flags=0, length=None):
    ln = len(payload) + 16 if length is None else length
    return struct.pack("<BBHIQBBHI", seq, 0, intf, ln, token, typ, cat, flags, 0) + payload


def test_timeline(tmp_path):
    r = Ring()
    r.add(0, 1, int.from_bytes(b"HDCP-0\0\0", "little"), marker=True)
    # ep 0x2b on DCP 1: GETBUF (I2A), GETBUF_ACK (A2I), INIT_TX, INIT_RX, the rings
    r.mbox(1, 0x2B, 1, 0x89 << 48 | 0x200 << 16 | 0x7)
    r.mbox(1, 0x2B, 0, 0xA1 << 48 | 0x800104000)
    r.mbox(1, 0x2B, 1, 0x8A << 48 | 0 << 32 | 0x100 << 16 | 0x7)
    r.add(0x800104000, 0x3F40 | 0x40 << 32, TAG["DAFR"] | 1 << 32 | 0 << 36 | 0x2B << 40)
    # a SEP message in between (untagged note, pc = guest PC)
    r.add(SEP_MBOX + 0x800, 0x0E | 0x05 << 8 | 0x20 << 32, 0xFFFFFE0001234, write=True, cpu=4)
    # DCP -> AP: announce of interface 3 "dispext0:dcpdptx-hdcp-interface:1" (cut to 32 bytes)
    name = b"dispext0:dcpdptx-hdcp-interface:1"[:31].ljust(32, b"\0")
    ann = qe(v2(1, 3, 0, 0x11, name, flags=1))
    r.mbox(1, 0x2B, 1, 0x85 << 48 | 0x80)
    r.copy(1, 0x2B, 1, 0, 0x0, ann)
    # AP -> DCP: an OOB command on interface 3, tx buffer at an unaligned DVA
    cmd = bytes([1, 0x12, 0, 0]) + struct.pack("<I", 0x40) + struct.pack(
        "<QQII", 0x800200000, 0x800300004, 0x40, 5)
    ent = qe(v2(9, 3, 1, 0xC0, cmd + b"\xaa\xbb", token=0x77))
    r.mbox(1, 0x2B, 0, 0xA2 << 48 | 0xC0)
    r.copy(1, 0x2B, 0, 0, 0x80, ent)
    r.copy(1, 0x2B, 0, 2, 0x800300004, b"\x01\x02\x03\x04\x05", skip=4)
    # a truncated entry, then a classic EPIC entry (channel 2, type 3) at ring offset 0x830,
    # which sep_decode.py must not take for a SEP reply
    r.copy(1, 0x2B, 1, 0, 0x100, qe(v2(2, 3, 2, 0xC0, bytes(64)))[:40], full=88)
    classic = struct.pack("<BHBIQ", 2, 5, 0, 0, 0) + struct.pack(
        "<IBBHQHHI", 4, 4, 0x30, 0xC0, 0, 0x11, 0, 0) + b"\xde\xad\xbe\xef"
    r.copy(0, 0x2B, 0, 0, 0x830, qe(classic, channel=2, etype=3))
    # a copy whose last word is missing from the ring
    r.copy(0, 0x2B, 0, 0, 0x200, qe(v2(3, 1, 0, 0x40, bytes(16))), drop_last=True)
    r.add(0, 0x2B | 3 << 32, TAG["DAFX"] | 1 << 32 | 2 << 36 | 0x2B << 40)
    # a SEP reply and a SEP buffer copy
    r.add(SEP_MBOX + 0x830, 0x0E | 0x05 << 8 | 0x01 << 16, 0)
    r.add(0x9C000, 0x0E | (1 << 16), SEPH | (7 << 32))
    r.add(0, 0x1122334455667788, SEPD | (7 << 32))

    dump = tmp_path / "mix.trc"
    r.dump(dump)
    out = tmp_path / "out"
    subprocess.run([sys.executable, "-I", str(TOOL), str(dump), str(out)], check=True)
    tl = (out / "timeline.txt").read_text()
    lines = tl.splitlines()

    def line(*parts):
        hits = [ln for ln in lines if all(p in ln for p in parts)]
        assert hits, (parts, tl)
        return hits[0]

    assert "MARK HDCP-0" in tl
    line("dcp1 ep 0x2b I2A GETBUF", "size 0x8000", "tag 0x7")
    line("dcp1 ep 0x2b A2I GETBUF_ACK", "dva 0x800104000")
    line("dcp1 ep 0x2b I2A INIT_TX", "off 0x0", "size 0x4000")
    line("dcp1 ep 0x2b RING tx", "bufsz 0x3f40", "stride 0x40", "dva 0x800104000")
    line("SEP A2I ep 0x0e", "tag 0x05")
    a = line("dcp1 ep 0x2b DCP->AP", "qe@0x0", "v2 seq 1 intf 3", "REPORT type 0x11")
    assert "dcpdptx-hdcp-interface" in a
    c = line("dcp1 ep 0x2b AP->DCP", "qe@0x80", "intf 3 dispext0:dcpdptx-hdcp-interface",
             "COMMAND type 0xc0", "token 0x77")
    assert "oob id 0x12 value 0x40 rx 0x800200000/0x40 tx 0x800300004/0x5" in c
    assert "payload 2: aabb" in c
    line("dcp1 ep 0x2b AP->DCP OOB-tx", "dva 0x800300004", "5 bytes", "0102030405")
    line("dcp1 ep 0x2b DCP->AP", "qe@0x100", "TRUNCATED 40 of 88")
    k = line("dcp0 ep 0x2b AP->DCP", "qe@0x830", "classic ch 2 type 3", "cat 0x30 type 0xc0",
             "tag 0x11")
    assert "payload 4: deadbeef" in k
    line("dcp0 ep 0x2b", "qe@0x200", "words missing")
    line("dcp1 ep 0x2b PROBLEM 2")
    line("SEP I2A ep 0x0e", "type 0x01")
    line("SEP BUF seq 7 ep 0x0e")
    # one timeline, in record order: the SEP send sits between the ring placement and the announce
    order = [i for i, ln in enumerate(lines) if "RING tx" in ln or "SEP A2I" in ln or "qe@0x0 " in ln]
    assert len(order) == 3 and order == sorted(order)
    # each copy's bytes, cut to their length
    blobs = sorted((out / "dcp").iterdir())
    assert any(b.read_bytes() == ent for b in blobs)
    assert any(b.read_bytes() == b"\x01\x02\x03\x04\x05" for b in blobs)

    # sep_decode.py on the same dump: SEP lines only, no DCP record taken for a SEP message
    out2 = tmp_path / "sep"
    subprocess.run([sys.executable, "-I", str(SEPTOOL), str(dump), str(out2)], check=True)
    stl = (out2 / "timeline.txt").read_text()
    assert stl.count(" A2I ") == 1 and stl.count(" I2A ") == 1, stl
