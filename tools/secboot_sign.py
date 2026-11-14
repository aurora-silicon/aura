#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Sign aura images and payloads (docs/secure-boot.md).

  secboot_sign.py keygen <prefix>                 write <prefix>.key (32-byte seed) and <prefix>.pub
  secboot_sign.py c-header <key.pub>              print build/secboot_key.h
  secboot_sign.py sign-payloads --key K --head-len N [--secver V] <in> <out>
        insert (or replace) an aura_sig record at offset N (the end of the aura head) that covers
        every byte after it: the DTBs, U-Boot or kernel, initramfs and config lines
  secboot_sign.py sign-image --key K [--secver V] <in> <out>
        append an aura_sig record and the AURSIGT1 footer that cover the whole image, for the
        stage that chainloads it
  secboot_sign.py fill-keys --pub P [--pub P2 ...] [--min-secver V] [--enforce] <stage1-in> <stage1-out>
        fill the AURORA-S1-KEY01 block of a Stage 1 image (up to 4 keys) before kmutil enrols it;
        --enforce makes an audit build refuse what it cannot verify (the floor applies either way)
  secboot_sign.py verify --pub P <file> [--head-len N]
        check either form and print the verdict, the key id and the SHA-256

Ed25519 here is the RFC 8032 reference algorithm in pure Python, so signing needs nothing beyond
the standard library. Keep production keys off build machines; see the design note.
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

# --- RFC 8032 Ed25519 (reference) ---------------------------------------------------------------
p = 2**255 - 19
q = 2**252 + 27742317777372353535851937790883648493
d = -121665 * pow(121666, p - 2, p) % p
I = pow(2, (p - 1) // 4, p)


def _inv(x):
    return pow(x, p - 2, p)


def _xrecover(y):
    xx = (y * y - 1) * _inv(d * y * y + 1)
    x = pow(xx, (p + 3) // 8, p)
    if (x * x - xx) % p:
        x = x * I % p
    if x % 2:
        x = p - x
    return x


_By = 4 * _inv(5) % p
B = (_xrecover(_By), _By, 1, _xrecover(_By) * _By % p)


def _add(P, Q):
    A = (P[1] - P[0]) * (Q[1] - Q[0]) % p
    Bv = (P[1] + P[0]) * (Q[1] + Q[0]) % p
    C = 2 * P[3] * Q[3] * d % p
    D = 2 * P[2] * Q[2] % p
    E, F, G, H = Bv - A, D - C, D + C, Bv + A
    return (E * F % p, G * H % p, F * G % p, E * H % p)


def _mul(s, P):
    Q = (0, 1, 1, 0)
    while s:
        if s & 1:
            Q = _add(Q, P)
        P = _add(P, P)
        s >>= 1
    return Q


def _enc(P):
    zi = _inv(P[2])
    x, y = P[0] * zi % p, P[1] * zi % p
    return int.to_bytes(y | ((x & 1) << 255), 32, "little")


def _dec(s):
    y = int.from_bytes(s, "little")
    sign = y >> 255
    y &= (1 << 255) - 1
    x = _xrecover(y)
    if (x & 1) != sign:
        x = p - x
    P = (x, y, 1, x * y % p)
    return P


def _h(m):
    return int.from_bytes(hashlib.sha512(m).digest(), "little")


def _expand(seed):
    h = hashlib.sha512(seed).digest()
    a = int.from_bytes(h[:32], "little")
    a &= (1 << 254) - 8
    a |= 1 << 254
    return a, h[32:]


def pubkey(seed):
    a, _ = _expand(seed)
    return _enc(_mul(a, B))


def sign(seed, msg):
    a, prefix = _expand(seed)
    A = _enc(_mul(a, B))
    r = _h(prefix + msg) % q
    R = _enc(_mul(r, B))
    k = _h(R + A + msg) % q
    return R + int.to_bytes((r + k * a) % q, 32, "little")


def verify_sig(pub, msg, sig):
    if len(sig) != 64:
        return False
    R, s = sig[:32], int.from_bytes(sig[32:], "little")
    if s >= q:
        return False
    A = _dec(pub)
    k = _h(R + pub + msg) % q
    lhs = _enc(_mul(s, B))
    rhs = _enc(_add(_dec(R), _mul(k, A)))
    return lhs == rhs


# --- aura record format -------------------------------------------------------------------------
DOMAIN = b"AURA-SIG-V1\0\0\0\0\0"
BODY_MAGIC = b"AURSIG01"
TAG = b"aura_sig"
FOOTER = b"AURSIGT1"
RECORD_SIZE = 0x1000  # keeps whatever follows the record 4K-aligned


def key_id(pub):
    return hashlib.sha256(pub).digest()[:8]


def make_record(seed, covered, secver, flags=0):
    pub = pubkey(seed)
    header = BODY_MAGIC + struct.pack("<IIIIQ", 1, 1, secver, flags, len(covered)) + key_id(pub)
    digest = hashlib.sha256(covered).digest()
    sig = sign(seed, DOMAIN + header + digest)
    rec = TAG + struct.pack("<I", RECORD_SIZE) + header + sig
    return rec + b"\0" * (RECORD_SIZE - len(rec)), digest


def check_record(pub, rec, covered):
    if rec[:8] not in (TAG, b"m1n1_sig") or rec[12:20] != BODY_MAGIC:
        return "bad-format", None, None
    version, alg, secver, flags, covered_len = struct.unpack("<IIIIQ", rec[20:44])
    kid = rec[44:52]
    if version != 1 or alg != 1 or covered_len != len(covered):
        return "bad-format", secver, kid
    digest = hashlib.sha256(covered).digest()
    if pub is not None and kid != key_id(pub):
        return "unknown-key", secver, kid
    if pub is not None and not verify_sig(pub, DOMAIN + rec[12:52] + digest, rec[52:116]):
        return "bad-signature", secver, kid
    return ("verified" if pub is not None else "signed"), secver, kid


def read_seed(path):
    seed = open(path, "rb").read()
    if len(seed) != 32:
        sys.exit(f"{path}: want a raw 32-byte Ed25519 seed")
    return seed


LOAD_HEADER = 0x808  # start.S: "AURALOAD", version, flags, _image_footprint


def payload_region(data):
    """The bytes after the aura image, where payloads and config lines are appended.

    An aura image runs from _base to _end, the end of its stack (the "STACKBOT" quad), and
    _payload_start is _end: the load header's footprint is the image's own size. The image itself
    holds the "chosen.aurora,measure-%d=" format string, so only what follows it is searched. Data
    that is not an aura image is searched whole.
    """
    if data[LOAD_HEADER:LOAD_HEADER + 8] == b"AURALOAD" and len(data) >= LOAD_HEADER + 24:
        (footprint,) = struct.unpack_from("<Q", data, LOAD_HEADER + 16)
        if LOAD_HEADER + 24 <= footprint <= len(data) and data[footprint - 8:footprint] == b"STACKBOT":
            return data[footprint:]
    return data


def forbid_measure_lines(data):
    # An image must not carry lines that only an earlier stage may append. A config line starts
    # wherever the payload before it ends (the first one follows the compressed U-Boot with no
    # newline), so the whole appended region is searched, not just line starts.
    if b"chosen.aurora,measure-" in payload_region(data):
        sys.exit("refusing to sign: the image carries chosen.aurora,measure- lines")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("prefix")
    c = sub.add_parser("c-header")
    c.add_argument("pub")
    for name in ("sign-payloads", "sign-image"):
        s = sub.add_parser(name)
        s.add_argument("--key", required=True)
        s.add_argument("--secver", type=int, default=0)
        if name == "sign-payloads":
            s.add_argument("--head-len", type=lambda x: int(x, 0), required=True)
        s.add_argument("inp")
        s.add_argument("out")
    f = sub.add_parser("fill-keys")
    f.add_argument("--pub", action="append", required=True)
    f.add_argument("--min-secver", type=int, default=0)
    f.add_argument("--enforce", action="store_true")
    f.add_argument("inp")
    f.add_argument("out")
    v = sub.add_parser("verify")
    v.add_argument("--pub")
    v.add_argument("--head-len", type=lambda x: int(x, 0))
    v.add_argument("file")
    a = ap.parse_args()

    if a.cmd == "keygen":
        seed = os.urandom(32)
        fd = os.open(a.prefix + ".key", os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        os.write(fd, seed)
        os.close(fd)
        open(a.prefix + ".pub", "wb").write(pubkey(seed))
        print(f"key id {key_id(pubkey(seed)).hex()}")
    elif a.cmd == "c-header":
        pub = open(a.pub, "rb").read()
        if len(pub) != 32:
            sys.exit(f"{a.pub}: want a raw 32-byte Ed25519 public key")
        print("/* Generated by tools/secboot_sign.py from %s, key id %s */" % (os.path.basename(a.pub), key_id(pub).hex()))
        print("static const u8 secboot_pubkey[32] = {" + ", ".join(f"0x{b:02x}" for b in pub) + "};")
    elif a.cmd == "sign-payloads":
        data = open(a.inp, "rb").read()
        head, tail = data[: a.head_len], data[a.head_len :]
        if tail[:8] in (TAG, b"m1n1_sig"):  # replace an earlier record
            (size,) = struct.unpack("<I", tail[8:12])
            tail = tail[size:]
        forbid_measure_lines(tail)
        rec, digest = make_record(read_seed(a.key), tail, a.secver)
        open(a.out, "wb").write(head + rec + tail)
        print(f"payloads 0x{len(tail):x} bytes sha256 {digest.hex()} key {key_id(pubkey(read_seed(a.key))).hex()} secver {a.secver}")
    elif a.cmd == "sign-image":
        data = open(a.inp, "rb").read()
        if data[-8:] == FOOTER:
            sys.exit("already signed")
        forbid_measure_lines(data)
        rec, digest = make_record(read_seed(a.key), data, a.secver)
        open(a.out, "wb").write(data + rec + struct.pack("<Q", len(data)) + FOOTER)
        print(f"image 0x{len(data):x} bytes sha256 {digest.hex()} key {key_id(pubkey(read_seed(a.key))).hex()} secver {a.secver}")
    elif a.cmd == "fill-keys":
        img = open(a.inp, "rb").read()
        magic = b"AURORA-S1-KEY01\0"
        if img.count(magic) != 1:
            sys.exit("the Stage 1 key block magic must occur exactly once (SECBOOT=audit/enforce Stage 1)")
        keys = [open(k, "rb").read() for k in a.pub]
        if not 1 <= len(keys) <= 4 or any(len(k) != 32 for k in keys):
            sys.exit("want 1..4 raw 32-byte Ed25519 public keys")
        off = img.index(magic) + 16
        if struct.unpack_from("<I", img, off)[0] != 1:
            sys.exit("unsupported key block version")
        flags = 1 if a.enforce else 0  # KEY_FLAG_ENFORCE
        body = struct.pack("<IIII", 1, len(keys), a.min_secver, flags) + b"".join(keys).ljust(128, b"\0")
        out = img[:off] + body + struct.pack("<I", zlib.crc32(body)) + img[off + len(body) + 4:]
        assert len(out) == len(img)
        open(a.out, "wb").write(out)
        print("keys " + " ".join(key_id(k).hex() for k in keys) + f" min-secver {a.min_secver} policy {'enforce' if a.enforce else 'audit'} sha256 {hashlib.sha256(out).hexdigest()}")
    elif a.cmd == "verify":
        data = open(a.file, "rb").read()
        pub = open(a.pub, "rb").read() if a.pub else None
        if data[-8:] == FOOTER:
            (off,) = struct.unpack("<Q", data[-16:-8])
            verdict, secver, kid = check_record(pub, data[off:-16], data[:off])
            covered = data[:off]
            form = "image trailer"
        elif a.head_len is not None:
            rec = data[a.head_len :]
            (size,) = struct.unpack("<I", rec[8:12])
            covered = rec[size:]
            verdict, secver, kid = check_record(pub, rec[:size], covered)
            form = "payload record"
        else:
            sys.exit("no trailer; pass --head-len for a payload record")
        print(f"{form}: {verdict} key {kid.hex() if kid else '-'} secver {secver} sha256 {hashlib.sha256(covered).hexdigest()}")
        sys.exit(0 if verdict in ("verified", "signed") else 1)


if __name__ == "__main__":
    main()
