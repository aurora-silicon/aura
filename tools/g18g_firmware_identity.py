#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print the src/kboot_gpu_t8152_profile.h entry for one G18G firmware build.

usage: g18g_firmware_identity.py RECEIPT GFX.bin GFX1.bin

GFX.bin and GFX1.bin are the decoded FUD payloads (rkosftab) of the stub's
Preboot usr/standalone/firmware/FUD/GFX.img4 and Ap,GFX1Firmware.img4.
J873 loads their b000 and b010 images. Each identity is the image's LC_UUID
(the ADT iop nub uuid), its __TEXT size, the offset of the patchbay TLVs
and the SHA-256 of __TEXT with the boot-local TLV values and the two
loader fields at 0x224/0x23c zeroed, as kboot_gpu_t8152.c measures it.
Paste the entry into builds[] and run clang-format on the header.
"""
import hashlib
import re
import struct
import sys
import uuid

HEADER = open(__file__.replace("tools/g18g_firmware_identity.py", "src/kboot_gpu_t8152_profile.h")).read()
TLVS = [(t, int(s), e == "true") for t, s, e in
        re.findall(r'\{"(.{4})", (\d+), (true|false)\}', HEADER.split("tlvs[]", 1)[1])]


def image(path, tag):
    d = open(path, "rb").read()
    if d[0x20:0x28] != b"rkosftab":
        sys.exit(f"{path}: not an rkosftab payload")
    for i in range(struct.unpack_from("<I", d, 0x28)[0]):
        t, off, size, _ = struct.unpack_from("<4sIII", d, 0x30 + i * 16)
        if t != tag:
            continue
        m = d[off:off + size]
        o, text, ident = 32, None, None
        for _ in range(struct.unpack_from("<I", m, 16)[0]):
            cmd, cmdsize = struct.unpack_from("<II", m, o)
            if cmd == 0x19 and m[o + 8:o + 24].rstrip(b"\0") == b"__TEXT":
                fileoff, filesize = struct.unpack_from("<QQ", m, o + 40)
                text = bytearray(m[fileoff:fileoff + filesize])
            elif cmd == 0x1b:
                ident = str(uuid.UUID(bytes=bytes(m[o + 8:o + 24]))).upper()
            o += cmdsize
        if text is None or ident is None:
            sys.exit(f"{path}: {tag.decode()} has no __TEXT or LC_UUID")
        return text, ident
    sys.exit(f"{path}: no {tag.decode()} image")


def identity(path, tag):
    text, ident = image(path, tag)
    first = TLVS[0][0].encode() + struct.pack("<I", TLVS[0][1])
    patchbay = text.find(first)
    if patchbay < 0 or text.find(first, patchbay + 1) >= 0:
        sys.exit(f"{path}: patchbay not found exactly once")
    offset = patchbay
    for name, size, exclude in TLVS:
        if text[offset:offset + 4] != name.encode() or struct.unpack_from("<I", text, offset + 4)[0] != size:
            sys.exit(f"{path}: TLV {name} not at {offset:#x}")
        if exclude:
            text[offset + 8:offset + 8 + size] = bytes(size)
        offset += 8 + size
    if offset != patchbay + 0x23a:
        sys.exit(f"{path}: patchbay is not 0x23a bytes")
    text[0x224:0x228] = bytes(4)
    text[0x23c:0x244] = bytes(8)
    return ident, len(text), patchbay, hashlib.sha256(text).digest()


def entry(role, path, tag):
    ident, size, patchbay, digest = identity(path, tag)
    rows = [", ".join(f"0x{b:02x}" for b in digest[i:i + 11]) for i in range(0, 32, 11)]
    body = ",\n          ".join(rows)
    return (f'        {{"{role}",\n         "{ident}",\n         {size:#x},\n         {patchbay:#x},\n'
            f'         {{{body}}}}}')


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    receipt, gfx, gfx1 = sys.argv[1:]
    print(f'    {{"{receipt}",\n     {{\n{entry("gfx-asc", gfx, b"b000")},\n'
          f'{entry("gfx1-asc", gfx1, b"b010")},\n     }}}},')
