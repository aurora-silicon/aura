#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Patch the user's own copy of the macOS 26.7.1 (25G241) J616s BootKC with the monitor, Linux, the guest DT and the initrd.

usage: kc_patch.py KC mon.elf out.macho [Image dtb [initrd]]

KC is the kernelcache of the user's own Mac or of Apple's IPSW for it: either the IM4P file
(kernelcache.release.mac16j) or its decompressed Mach-O. Nothing from Apple is stored in this
repository: the input is checked against the SHA-256 below and only offsets into it are kept here.
The output contains Apple's code. Keep it on your machine, never publish or share it."""
import hashlib, os, struct, subprocess, sys

# Decompressed BootKC of macOS 26.7.1 (25G241) for Mac16,7 (J616s), xnu-12377.161.15.700.19.
SUPPORTED = {'9a1308e87e42563ef8dc83034c87086aea1196a0a269afa6ed5117b965212aaa': '26.7.1 25G241 J616s'}


def load_kc(path):
    d = open(path, 'rb').read()
    if d[:1] == b'\x30' and b'IM4P' in d[:16]:
        i = d.find(b'bvx2')
        if i < 0:
            raise SystemExit(f'{path}: IM4P without an LZFSE payload')
        d = subprocess.run(['lzfse', '-decode'], input=d[i:], stdout=subprocess.PIPE, check=True).stdout
    h = hashlib.sha256(d).hexdigest()
    if h not in SUPPORTED:
        raise SystemExit(f'{path}: unsupported kernelcache {h}; the offsets here are for {", ".join(SUPPORTED.values())} only')
    print(f'kernelcache {SUPPORTED[h]} {h[:16]}')
    return bytearray(d)


if len(sys.argv) < 4:
    raise SystemExit(__doc__)
KC_PATH = sys.argv.pop(1)
TX_VA, TX_FO, TX_END = 0xfffffe000b744000, 0x4740000, 0xfffffe000c0b0000
KX_VA, KX_FO, KX_END = 0xfffffe0008958000, 0x1954000, 0xfffffe0008a58000   # monitor text (first kext __TEXT_EXEC MiB)
VEC_VA, VEC_END = 0xfffffe000b748000, 0xfffffe000b748600                   # XNU current-EL and lower-EL AArch64 vector entries
HOOK_VA, HOOK_FO = 0xfffffe000b911d70, 0x4740000 + 0xb911d70 - 0xb744000   # arm_init+0x88
SEC_VA, SEC_FO = 0xfffffe000c0c0098, 0x50bc098                             # _start warm path, __TEXT_BOOT_EXEC
PAN_VA, PAN_FO = 0xfffffe000c0c001c, 0x50bc01c                             # _start panic reflect
e = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<Q', e, 0x28); shentsize, shnum, shstrndx = struct.unpack_from('<HHH', e, 0x3a)
secs = [struct.unpack_from('<IIQQQQIIQQ', e, shoff + i * shentsize) for i in range(shnum)]
strtab = secs[shstrndx]
name = lambda s: e[strtab[4] + s[0]:].split(b'\0')[0].decode()
kc = load_kc(KC_PATH)
for s in secs:
    nm, typ, flags, addr, off, size = name(s), s[1], s[2], s[3], s[4], s[5]
    if typ != 1 or not (flags & 2) or size == 0:     # PROGBITS + ALLOC
        continue
    if KX_VA <= addr and addr + size <= KX_END:
        fo = KX_FO + addr - KX_VA
    elif VEC_VA <= addr and addr + size <= VEC_END:
        fo = TX_FO + addr - TX_VA
    else:
        raise SystemExit(f'{nm} {addr:#x}+{size:#x} outside the monitor text window and the vector page')
    kc[fo:fo + size] = e[off:off + size]
    print(f'{nm:10s} va {addr:#x} fo {fo:#x} size {size:#x}')
if len(sys.argv) > 4:
    # Header, dtb and initrd follow the monitor in kext __TEXT_EXEC. The Image goes into __LINKEDIT behind the kernel's
    # symbol table: XNU slides n_value in place at boot (seen as +slide on every 16th byte offset 8, qemu), and the
    # chained fixups stay at the end. The ranges are recomputed from the KC, no address here is trusted.
    PFO, PVA, PLIM = 0x1a54000, 0xfffffe0008a58000, 0x4740000
    LE_VA, LE_FO, LE_END = 0xfffffe000c8dc000, 0x58d8000, 0x71c0000
    img, dtb = open(sys.argv[3], 'rb').read(), open(sys.argv[4], 'rb').read()
    initrd = open(sys.argv[5], 'rb').read() if len(sys.argv) > 5 else b''
    assert len(initrd) <= 0xa000000, f'initrd {len(initrd):#x} reaches the ADT copy, ADT_OFF - INITRD_OFF in linux.c'
    al = lambda n: (n + 0x3fff) & ~0x3fff
    def symtabs():
        n, p, r = struct.unpack_from('<I', kc, 16)[0], 32, set()
        for _ in range(n):
            c, sz = struct.unpack_from('<II', kc, p)
            if c == 0x80000035:
                fo = struct.unpack_from('<Q', kc, p + 16)[0]
                nc, q = struct.unpack_from('<I', kc, fo + 16)[0], fo + 32
                for _ in range(nc):
                    c2, s2 = struct.unpack_from('<II', kc, q)
                    if c2 == 2:
                        so, ns = struct.unpack_from('<II', kc, q + 8)
                        if ns:
                            r.add((so, so + ns * 16))
                    q += s2
            p += sz
        return sorted(r)
    st = symtabs()
    print('symtabs', [(hex(a_), hex(b_)) for a_, b_ in st])
    ifo = al(max(b_ for _, b_ in st))
    assert ifo >= LE_FO and ifo + al(len(img)) <= LE_END, f'Image does not fit behind the symtab: {ifo:#x}+{len(img):#x}'
    do, ro = 0x4000, 0x4000 + al(len(dtb))
    end = ro + al(len(initrd))
    assert PFO + end <= PLIM, 'payload too large'
    blob = bytearray(end)
    io = (LE_VA + ifo - LE_FO - PVA) & (2**64 - 1)
    struct.pack_into('<QQQQQQQ', blob, 0, 0x4d4f4e504c4f4144, io, len(img), do, len(dtb), ro, len(initrd))
    blob[do:do + len(dtb)] = dtb
    blob[ro:ro + len(initrd)] = initrd
    kc[PFO:PFO + end] = blob
    kc[ifo:ifo + len(img)] = img
    s_ = 0
    for i_ in range(0, len(img) & ~7, 8):
        s_ = (((s_ << 1) | (s_ >> 63)) & (2**64 - 1)) ^ int.from_bytes(img[i_:i_ + 8], 'little')
    print(f'Image sum {s_:#018x}')
    print(f'Image fo {ifo:#x} size {len(img):#x}; dtb+initrd {end:#x} at fo {PFO:#x}')
# hook: arm_init+0x88 -> b mon_entry
symsec = [x for x in secs if x[1] == 2][0]
strsec = secs[symsec[6]]
def elf_sym(want):
    for i in range(symsec[5] // 24):
        st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', e, symsec[4] + i * 24)
        if e[strsec[4] + st_name:].split(b'\0')[0] == want:
            return st_value
    raise SystemExit(f'{want} not found')
sym = elf_sym(b'mon_entry')
off = sym - HOOK_VA
assert -(1 << 27) <= off < (1 << 27)
struct.pack_into('<I', kc, HOOK_FO, 0x14000000 | ((off >> 2) & 0x03ffffff))
print(f'hook   va {HOOK_VA:#x} fo {HOOK_FO:#x} -> b {sym:#x}')
# secondary hook: start_warm -> b mon_secondary, in range of the first kext __TEXT_EXEC MiB (distance 0x3768000 < 128 MiB)
sec = elf_sym(b'mon_secondary')
off = sec - SEC_VA
assert -(1 << 27) <= off < (1 << 27)
struct.pack_into('<I', kc, SEC_FO, 0x14000000 | ((off >> 2) & 0x03ffffff))
print(f'hook2  va {SEC_VA:#x} fo {SEC_FO:#x} -> b {sec:#x}')
# third hook: SPTM's panic reflect prints its text before XNU's panic() can stop silently (same distance as hook2)
pan = elf_sym(b'mon_sptm_panic')
off = pan - PAN_VA
assert -(1 << 27) <= off < (1 << 27)
struct.pack_into('<I', kc, PAN_FO, 0x14000000 | ((off >> 2) & 0x03ffffff))
print(f'hook3  va {PAN_VA:#x} fo {PAN_FO:#x} -> b {pan:#x}')
open(sys.argv[2], 'wb').write(kc)
print('sha256', hashlib.sha256(kc).hexdigest())
