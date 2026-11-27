# SPDX-License-Identifier: MIT
"""tools/auractl/decode.py against a synthetic AURATRC1 file: the header and meta decode, the
marker rows, and the --writes-only / --pa / --base / --csv filters. No aura or Mac needed.

Runs under pytest, or standalone: python3 tests/python/test_auractl_decode.py"""

import struct
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
DECODE = REPO / "tools/auractl/decode.py"

HDR = struct.Struct("<8sIIQQQQQ48s")
REC = struct.Struct("<QQQQ")
CNTFRQ = 24000000
USB = 0x230000000  # a USB controller's reg base, for the --base/--pa checks


def _meta(pa, width_log2, write, cpu, marker=0):
    m = pa & ((1 << 48) - 1)
    m |= (width_log2 & 3) << 48
    m |= (write & 1) << 50
    m |= (cpu & 0xF) << 51
    m |= (marker & 1) << 55
    m |= ((cpu >> 4) & 3) << 56
    return m


def _tag(s):
    return struct.unpack("<Q", s.encode().ljust(8, b"\0")[:8])[0]


def _file(path, recs, first=100, lost=0, build="aura-test"):
    hdr = HDR.pack(b"AURATRC1", 1, REC.size, CNTFRQ, first, len(recs), first + len(recs), lost,
                   build.encode())
    path.write_bytes(hdr + b"".join(REC.pack(*r) for r in recs))


def _run(path, *args):
    r = subprocess.run([sys.executable, str(DECODE), *map(str, args), str(path)],
                       capture_output=True, text=True, timeout=60)
    return r


# t, meta, val, pc
RECS = [
    (0, _meta(USB + 0x10, 2, 1, 0), 0x1, 0xffff0000),           # write, 32-bit, cpu0
    (24, _meta(USB + 0x14, 2, 0, 0), 0xdeadbeef, 0xffff0004),   # read,  32-bit, cpu0
    (48, _meta(0, 0, 0, 5, marker=1), 7, _tag("kbd-plug")),     # marker
    (72, _meta(USB + 0x20, 3, 1, 33), 0x55, 0xffff0010),        # write, 64-bit, cpu33 (6-bit)
    (96, _meta(0x210000000, 2, 0, 1), 0x3, 0xffff0020),         # read on a different page
]


def _mk(d, name="t.trc", **kw):
    p = Path(d) / name
    _file(p, RECS, **kw)
    return p


def test_default_rows():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_mk(d))
        assert r.returncode == 0, r.stderr
        out = r.stdout
        assert "aura aura-test" in out and "5 records from sequence 100" in out
        assert "MARK kbd-plug" in out
        assert "c33" in out  # the 6-bit cpu reassembled
        assert "W64" in out and "R32" in out
        assert "5 rows (5 records in the file)" in r.stderr


def test_writes_only():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_mk(d), "--writes-only")
        # two writes; the marker always shows; the two reads are dropped
        assert r.returncode == 0, r.stderr
        data = [ln for ln in r.stdout.splitlines() if not ln.startswith("#")]
        assert sum("W" in ln and "MARK" not in ln for ln in data) == 2
        assert not any(" R" in ln for ln in data)


def test_pa_filter():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_mk(d), "--pa", hex(USB))
        assert r.returncode == 0, r.stderr
        body = [ln for ln in r.stdout.splitlines() if not ln.startswith("#")]
        # the three USB-page records + the marker (markers are never filtered out)
        assert sum("0x210000000" in ln for ln in body) == 0
        assert any("MARK" in ln for ln in body)


def test_base_offsets():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_mk(d), "--base", hex(USB))
        assert r.returncode == 0, r.stderr
        assert "+0x10" in r.stdout and "+0x20" in r.stdout


def test_csv():
    with tempfile.TemporaryDirectory() as d:
        r = _run(_mk(d), "--csv")
        assert r.returncode == 0, r.stderr
        lines = r.stdout.splitlines()
        assert lines[0] == "seq,t_s,cpu,op,width_bits,pa,val,pc"
        assert any(ln.startswith("102,") and ",MARK," in ln for ln in lines)
        assert any(",W,64," in ln for ln in lines)


def test_rejects_junk():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "bad.trc"
        p.write_bytes(b"NOPETRC1" + b"\0" * 200)
        r = _run(p)
        assert r.returncode != 0 and "not an auractl read file" in r.stderr


def test_count_mismatch_warns():
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "short.trc"
        hdr = HDR.pack(b"AURATRC1", 1, REC.size, CNTFRQ, 0, 99, 99, 0, b"aura-test")
        p.write_bytes(hdr + REC.pack(*RECS[0]))  # header claims 99, only 1 present
        r = _run(p)
        assert r.returncode == 0 and "header says 99 records, file holds 1" in r.stderr


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL {name}: {type(e).__name__}: {str(e)[:600]}")
            else:
                print(f"ok   {name}")
    sys.exit(1 if failed else 0)
