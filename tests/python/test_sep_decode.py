# SPDX-License-Identifier: MIT
"""tools/auractl/sep_decode.py against a synthetic auractl dump of AURA_SEP_TRACE records."""
import pathlib
import struct
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TOOL = ROOT / "tools/auractl/sep_decode.py"
MBOX = 0x25E408000


def rec(t, pa, val, pc, write=False, cpu=0, marker=False):
    meta = pa | (3 << 48) | (int(write) << 50) | (cpu << 51) | (int(marker) << 55)
    return struct.pack("<4Q", t, meta, val, pc)


def test_decodes_messages_names_and_buffers(tmp_path):
    recs = [
        rec(0, 0, 1, int.from_bytes(b"BLESS-0\0", "little"), marker=True),
        # discovery: ep 0xfd type 0 param 0x15 data 'pnon'
        rec(24, MBOX + 0x830, 0xFD | (0x15 << 24) | (int.from_bytes(b"pnon", "big") << 32), 0),
        # a send to ep 0x15 and a 16-byte in-buffer copy
        rec(48, MBOX + 0x800, 0x15 | (0x05 << 8) | (0x20 << 32), 0x1234, write=True, cpu=4),
        rec(49, 0x9C000, 0x15 | (2 << 16), 0x48504553 | (7 << 32), write=False),
        rec(49, 0, 0x4250636811223344, 0x44504553 | (7 << 32)),
        rec(49, 8, 0x5566, 0x44504553 | (7 << 32)),
        # the reply
        rec(72, MBOX + 0x830, 0x15 | (0x05 << 8) | (0x01 << 16), 0),
    ]
    hdr = struct.pack("<8sII5Q48s", b"AURATRC1", 1, 32, 24, 0, len(recs), len(recs), 0, b"test")
    dump = tmp_path / "sep.trc"
    dump.write_bytes(hdr + b"".join(recs))
    out = tmp_path / "out"
    subprocess.run([sys.executable, "-I", str(TOOL), str(dump), str(out)], check=True)
    tl = (out / "timeline.txt").read_text()
    assert "MARK BLESS-0" in tl
    assert "A2I ep 0x15 pnon tag 0x05" in tl
    assert "I2A ep 0x15 pnon tag 0x05 type 0x01" in tl
    assert "BUF seq 7 ep 0x15 pnon in @send iova 0x9c000 16 bytes" in tl
    assert "0x15=pnon" in tl
    buf = (out / "buf" / "000007-ep15-in-send.bin").read_bytes()
    assert buf == struct.pack("<QQ", 0x4250636811223344, 0x5566)
