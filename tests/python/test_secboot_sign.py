# SPDX-License-Identifier: MIT
"""tools/secboot_sign.py: both signature forms verify, and any changed byte fails."""

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TOOL = [sys.executable, "-I", str(ROOT / "tools/secboot_sign.py")]
KEY = ROOT / "data/secboot/test-ed25519.key"
PUB = ROOT / "data/secboot/test-ed25519.pub"


def run(*args):
    return subprocess.run(TOOL + [str(a) for a in args], capture_output=True, text=True)


def test_payload_record_and_trailer(tmp_path):
    src = tmp_path / "stage2.bin"
    src.write_bytes(bytes(range(256)) * 64 + b"\xd0\x0d\xfe\xed" + b"tail" * 1000)
    signed = tmp_path / "p.bin"
    assert run("sign-payloads", "--key", KEY, "--head-len", "0x4000", "--secver", 3, src, signed).returncode == 0
    ok = run("verify", "--pub", PUB, "--head-len", "0x4000", signed)
    assert ok.returncode == 0 and "verified" in ok.stdout and "secver 3" in ok.stdout
    # Re-signing replaces the record instead of stacking a second one.
    again = tmp_path / "p2.bin"
    assert run("sign-payloads", "--key", KEY, "--head-len", "0x4000", signed, again).returncode == 0
    assert again.stat().st_size == signed.stat().st_size

    trailer = tmp_path / "t.bin"
    assert run("sign-image", "--key", KEY, src, trailer).returncode == 0
    assert run("verify", "--pub", PUB, trailer).returncode == 0
    for path, off in ((signed, 0x4000 + 0x1000 + 10), (trailer, 10)):
        data = bytearray(path.read_bytes())
        data[off] ^= 1
        path.write_bytes(bytes(data))
    assert "bad-signature" in run("verify", "--pub", PUB, "--head-len", "0x4000", signed).stdout
    assert "bad-signature" in run("verify", "--pub", PUB, trailer).stdout


def test_refuses_forged_measurement_lines(tmp_path):
    src = tmp_path / "s.bin"
    src.write_bytes(b"\0" * 0x4000 + b"chosen.aurora,measure-0=stage1;enforce;x;verified\n")
    assert run("sign-image", "--key", KEY, src, tmp_path / "o.bin").returncode != 0


def test_fill_keys(tmp_path):
    import struct
    import zlib
    block = b"AURORA-S1-KEY01\0" + struct.pack("<IIII", 1, 0, 0, 0) + bytes(128) + bytes(4)
    src = tmp_path / "s1.bin"
    src.write_bytes(b"head" + block + b"STACKBOT")
    out = tmp_path / "s1-filled.bin"
    r = run("fill-keys", "--pub", PUB, "--pub", PUB, "--min-secver", 7, src, out)
    assert r.returncode == 0, r.stderr
    d = out.read_bytes()
    assert len(d) == src.stat().st_size and d.endswith(b"STACKBOT")
    body = d[4 + 16:4 + 16 + 144]
    assert struct.unpack_from("<IIII", body) == (1, 2, 7, 0)
    assert body[16:48] == PUB.read_bytes()
    assert struct.unpack_from("<I", d, 4 + 160)[0] == zlib.crc32(body)
    assert run("fill-keys", "--pub", PUB, out, tmp_path / "x").returncode == 0  # refill is fine
    enf = tmp_path / "s1-enforce.bin"
    assert run("fill-keys", "--pub", PUB, "--enforce", src, enf).returncode == 0
    body = enf.read_bytes()[4 + 16:4 + 16 + 144]
    assert struct.unpack_from("<IIII", body) == (1, 1, 0, 1)
    src.write_bytes(b"no block here")
    assert run("fill-keys", "--pub", PUB, src, tmp_path / "y").returncode != 0
