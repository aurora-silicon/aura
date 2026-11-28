# SPDX-License-Identifier: MIT
"""tools/auractl/dcp_pkt_decode.py against a synthetic AURATRC1 file: an AP call copied whole
(input) and its reply (output), as src/s2r_dcp_pkt.c records them. No aura or Mac needed.

Runs under pytest, or standalone: python3 tests/python/test_dcp_pkt_decode.py"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECODE = REPO / "tools/auractl/dcp_pkt_decode.py"
HDR = struct.Struct("<8sIIQQQQQ48s")
REC = struct.Struct("<QQQQ")
CPKT = 0x544B5043


def _rec(t, kind, word, val, a2i=1):
    meta = 0x1000 + 8 * word | 3 << 48 | a2i << 50
    return REC.pack(t, meta, val, CPKT | 0 << 32 | kind << 36 | word << 40)


def _marker(t, tag):
    return REC.pack(t, 1 << 55, 1, int.from_bytes(tag.encode().ljust(8, b"\0"), "little"))


def _packet(in_bytes, out_bytes, tag="A407"):
    head = tag[::-1].encode() + struct.pack("<II", len(in_bytes), len(out_bytes))
    return head + in_bytes + out_bytes


def _words(b, start=0):
    b = b + b"\0" * (-len(b) % 8)
    return [int.from_bytes(b[i : i + 8], "little") for i in range(start, len(b), 8)]


def test_call_and_reply():
    inp = bytes(range(37))  # odd length: the output starts unaligned
    outp = bytes(range(100, 111))
    pkt = _packet(inp, outp)
    recs = [_marker(1, "edr1")]
    recs += [_rec(10 + i, 0, i, w) for i, w in enumerate(_words(pkt[: 12 + len(inp)]))]
    base = (12 + len(inp)) & ~7
    w = _words(pkt)
    reply = [w[0], w[1]] + _words(pkt[base:])
    recs += [_rec(50 + i, 2, i, v, 0) for i, v in enumerate(reply)]
    blob = HDR.pack(b"AURATRC1", 1, REC.size, 24000000, 0, len(recs), len(recs), 0, b"test")
    blob += b"".join(recs)
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "t.trc"
        f.write_bytes(blob)
        r = subprocess.run([sys.executable, "-I", str(DECODE), str(f), str(Path(d) / "o"), "--hex", "64"],
                           capture_output=True, text=True, check=True)
        lines = [l for l in r.stdout.splitlines() if not l.startswith("#")]
        assert len(lines) == 2, r.stdout
        assert "edr1" in lines[0] and "call " in lines[0] and "A407" in lines[0], lines[0]
        assert inp.hex() in lines[0], lines[0]
        assert "call-out" in lines[1] and outp.hex() in lines[1], lines[1]
        bins = sorted((Path(d) / "o").glob("*.bin"))
        assert [b.read_bytes() for b in bins] == [inp, outp]


if __name__ == "__main__":
    test_call_and_reply()
    print("ok")
