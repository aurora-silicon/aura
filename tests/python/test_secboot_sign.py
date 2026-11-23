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
    # Anyone can sign with the published test key: enforce with it only when asked for a test.
    r = run("fill-keys", "--pub", PUB, "--enforce", src, enf)
    assert r.returncode != 0 and "test key" in r.stderr and not enf.exists()
    assert run("fill-keys", "--pub", PUB, "--enforce", "--test-key", src, enf).returncode == 0
    body = enf.read_bytes()[4 + 16:4 + 16 + 144]
    assert struct.unpack_from("<IIII", body) == (1, 1, 0, 1)
    own = tmp_path / "own.pub"
    own.write_bytes(bytes(range(32)))
    assert run("fill-keys", "--pub", own, "--enforce", src, tmp_path / "own.bin").returncode == 0
    r = run("fill-keys", "--pub", own, "--pub", PUB, "--enforce", src, tmp_path / "mixed.bin")
    assert r.returncode != 0 and "test key" in r.stderr
    src.write_bytes(b"no block here")
    assert run("fill-keys", "--pub", PUB, src, tmp_path / "y").returncode != 0


def test_the_tool_knows_the_published_test_key():
    import importlib.util

    spec = importlib.util.spec_from_file_location("secboot_sign", ROOT / "tools/secboot_sign.py")
    secboot_sign = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(secboot_sign)
    assert secboot_sign.TEST_KEY_ID == secboot_sign.key_id(PUB.read_bytes())
    assert secboot_sign.pubkey(KEY.read_bytes()) == PUB.read_bytes()


def aura_stage2(footprint=0x20000):
    """An aura image as the build lays it out: the load header's footprint is the file's size, the
    stack's STACKBOT quad ends it, and its code holds the measure line format string."""
    import struct
    image = bytearray(footprint)
    image[0x808:0x810] = b"AURALOAD"
    struct.pack_into("<IIQ", image, 0x810, 1, 1, footprint)
    fmt = b"chosen.aurora,measure-%d=%s;%s;%s;%s;%s;%u;%s\0"
    image[0x9000:0x9000 + len(fmt)] = fmt
    image[-8:] = b"STACKBOT"
    return bytes(image)


def composed(image, lines=b"chosen.aurora,stage1-flavour=secure\nmitigations=auto\n"):
    # update-m1n1's order: aura, DTBs, gzip'd U-Boot, then the config lines with no separator.
    return image + b"\xd0\x0d\xfe\xed" + bytes(60) + b"\x1f\x8b\x08" + bytes(29) + lines


def test_an_aura_stage2_signs_although_its_code_names_measure_lines(tmp_path):
    src = tmp_path / "boot.bin"
    src.write_bytes(composed(aura_stage2()))
    out = tmp_path / "signed.bin"
    r = run("sign-image", "--key", KEY, src, out)
    assert r.returncode == 0, r.stderr
    assert run("verify", "--pub", PUB, out).returncode == 0


def test_an_appended_measure_line_is_refused(tmp_path):
    image = aura_stage2()
    src = tmp_path / "boot.bin"
    # First line right after U-Boot (no newline before it), and a later line.
    for lines in (b"chosen.aurora,measure-0=stage1;audit;next-stage;verified;;0;" + b"0" * 64 + b"\n",
                  b"mitigations=auto\nchosen.aurora,measure-3=x\n"):
        src.write_bytes(composed(image, lines))
        r = run("sign-image", "--key", KEY, src, tmp_path / "out.bin")
        assert r.returncode != 0 and "measure-" in r.stderr
    # Data that is not an aura image is searched whole.
    src.write_bytes(b"\0" * 0x100 + b"chosen.aurora,measure-0=x\n")
    assert run("sign-image", "--key", KEY, src, tmp_path / "out.bin").returncode != 0


def test_a_built_stage2_signs(tmp_path):
    import pytest
    built = ROOT / "build/aura.bin"
    if not built.exists() or b"chosen.aurora,measure-" not in built.read_bytes():
        pytest.skip("needs a SECBOOT-era build/aura.bin")
    src = tmp_path / "boot.bin"
    src.write_bytes(composed(built.read_bytes()))
    assert run("sign-image", "--key", KEY, src, tmp_path / "out.bin").returncode == 0


def test_a_record_verifies_only_as_the_form_it_was_signed_for(tmp_path):
    import struct
    head, covered = b"\0" * 0x4000, b"dtbs, u-boot and config lines" * 100
    src = tmp_path / "stage2.bin"
    src.write_bytes(head + covered)
    signed = tmp_path / "p.bin"
    assert run("sign-payloads", "--key", KEY, "--head-len", "0x4000", src, signed).returncode == 0
    rec = signed.read_bytes()[0x4000:0x5000]
    # The payload record and its covered bytes, laid out as a signed image.
    image = tmp_path / "image.bin"
    image.write_bytes(covered + rec + struct.pack("<Q", len(covered)) + b"AURSIGT1")
    r = run("verify", "--pub", PUB, image)
    assert r.returncode != 0 and "bad-format" in r.stdout, r.stdout
    # And the other way: an image trailer's record in front of the image as payloads.
    trailer = tmp_path / "t.bin"
    assert run("sign-image", "--key", KEY, src, trailer).returncode == 0
    t = trailer.read_bytes()
    payloads = tmp_path / "payloads.bin"
    payloads.write_bytes(head + t[len(head + covered):-16] + head + covered)
    r = run("verify", "--pub", PUB, "--head-len", "0x4000", payloads)
    assert r.returncode != 0 and "bad-format" in r.stdout, r.stdout


def test_a_record_without_a_form_is_refused(tmp_path):
    import importlib.util
    import struct
    spec = importlib.util.spec_from_file_location("secboot_sign", ROOT / "tools/secboot_sign.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    seed = KEY.read_bytes()
    covered = b"an image" * 64
    # What older signers wrote: flags 0.
    header = tool.BODY_MAGIC + struct.pack("<IIIIQ", 1, 1, 0, 0, len(covered)) + tool.key_id(tool.pubkey(seed))
    sig = tool.sign(seed, tool.DOMAIN + header + tool.hashlib.sha256(covered).digest())
    rec = (tool.TAG + struct.pack("<I", tool.RECORD_SIZE) + header + sig).ljust(tool.RECORD_SIZE, b"\0")
    image = tmp_path / "old.bin"
    image.write_bytes(covered + rec + struct.pack("<Q", len(covered)) + b"AURSIGT1")
    r = run("verify", "--pub", PUB, image)
    assert r.returncode != 0 and "bad-format" in r.stdout, r.stdout


def test_a_caller_from_before_the_form_still_signs_an_image(tmp_path):
    """aurora-secboot stage2 sign calls make_record(seed, data, secver) and appends the footer."""
    import importlib.util
    import struct
    spec = importlib.util.spec_from_file_location("secboot_sign", ROOT / "tools/secboot_sign.py")
    tool = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tool)
    data = b"a stage 2" * 100
    rec, _ = tool.make_record(KEY.read_bytes(), data, 2)
    out = tmp_path / "s2.bin"
    out.write_bytes(data + rec + struct.pack("<Q", len(data)) + tool.FOOTER)
    r = run("verify", "--pub", PUB, out)
    assert r.returncode == 0 and "image trailer: verified" in r.stdout, r.stdout
