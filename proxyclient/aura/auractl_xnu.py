# SPDX-License-Identifier: MIT
"""Find XNU's acct() syscall in a macOS kernelcache and plan the auractl XNU entry.

The auractl XNU entry (docs/auractl.md, "xnu") lets a guest process reach aura's control hvc
without a kext: the tool calls acct(2) (syscall 51) with its request block in place of the path,
and a small patch to the kernelcache's acct() turns the call into aura's hvc. acct() is a natural
host: it is root-only and otherwise unused on our guests, it takes exactly one user pointer, and
its own epilogue already returns whatever errno sits in w0, so a non-root (or non-auractl) caller
sees the acct() it always saw -- EPERM, or whatever aura answers.

This module locates the site "from the file alone" and FAILS CLOSED: every anchor is a documented,
build-independent fact (the syscall number, acct's root check, its arg save), and anything
ambiguous raises XnuEntryError rather than guess. It carries no Apple bytes: the words it writes
are built from named instruction fields, and the words it expects to overwrite are read from the
kernelcache at plan time, never hard-coded.

The patch, placed at the target of acct's root check (so only a caller that passed the check -- the
tool insists on root -- reaches it; non-root still falls through to EPERM):

    ldr  x1, [uap]              ; x1 = uap->path = the caller's &struct auractl_req
    movz w0, #(MAGIC & 0xffff)  ; w0 = AURACTL_HVC_MAGIC ('ACT1')
    movk w0, #(MAGIC >> 16), lsl #16
    hvc  #AURACTL_HVC_IMM
    b    <acct error-return>    ; acct's own "return the errno in w0" label

`uap` is the callee-saved register acct saves x1 (its second argument) into in the prologue;
`uap->path` is at offset 0 of struct acct_args. The branch target is the instruction right after
acct's `mov w0, #EPERM`, which acct itself branches to from every error path to run the stack-check
and return w0.

Used by proxyclient/tools/run_guest_sptm.py when AURA_XNU_ENTRY=1, and runnable standalone to plan
(and, with --disas, dump) the patch for a kernelcache offline:

    python3 proxyclient/aura/auractl_xnu.py kernelcache.decompressed
"""

import struct
import sys

# The two ABI words the patch needs, mirrored from src/auractl_abi.h (the shared source of truth):
#   w0 = AURACTL_HVC_MAGIC ('ACT1'), hvc #AURACTL_HVC_IMM.
AURACTL_HVC_IMM = 0xAC71
AURACTL_HVC_MAGIC = 0x41435431

# acct is BSD syscall 51; struct acct_args has one field, char *path, at offset 0.
ACCT_SYSCALL = 51
ACCT_NARG = 1

# sysent (bsd/sys/sysent.h): { sy_call; sy_arg_munge32; int32 sy_return_type; int16 sy_narg;
# uint16 sy_arg_bytes }, 24 bytes. The two pointers are chained-fixup slots; the integer fields are
# plain. The low syscalls' argument counts are a stable fingerprint of the table's start:
#   0 syscall 1 exit 2 fork 3 read 4 write 5 open 6 close 7 wait4 ...
SYSENT_STRIDE = 24
SYSENT_NARG_PREFIX = (0, 1, 0, 3, 3, 3, 1, 4)  # syscalls 0..7
SYSENT_MIN_ENTRIES = 400  # the table has ~557; well above any coincidental run

LC_SEGMENT_64 = 0x19
LC_DYLD_CHAINED_FIXUPS = 0x80000034
DYLD_CHAINED_PTR_64_KERNEL_CACHE = 8

EPERM_MOV_W0_1 = 0x52800020  # mov w0, #1  (EPERM; acct's non-root return value)


class XnuEntryError(Exception):
    pass


# --- Mach-O + chained-fixup pointer decode (only what the finder needs) ----------------------- #

def _segments(data):
    """{name: (vmaddr, vmsize, fileoff, filesize)} and the chained-fixups file offset."""
    magic = struct.unpack_from("<I", data, 0)[0]
    if magic != 0xFEEDFACF:
        raise XnuEntryError("not a 64-bit little-endian Mach-O kernelcache")
    ncmds = struct.unpack_from("<I", data, 16)[0]
    off, segs, fixups = 32, {}, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].split(b"\0")[0].decode("ascii", "replace")
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            if vmsize:
                segs[name] = (vmaddr, vmsize, fileoff, filesize)
        elif cmd == LC_DYLD_CHAINED_FIXUPS:
            fixups = struct.unpack_from("<I", data, off + 8)[0]
        off += size
    for need in ("__TEXT_EXEC", "__DATA_CONST"):
        if need not in segs:
            raise XnuEntryError(f"kernelcache has no {need} segment")
    return segs, fixups


def _pointer_format(data, fixups, segs):
    """Confirm every fixed-up segment uses the 64-bit kernel-cache pointer format, so a const
    pointer decodes as (cache base + low 30 bits). Fail closed on any other format."""
    if fixups is None:
        raise XnuEntryError("kernelcache has no chained fixups (cannot follow sysent)")
    ver, starts_off = struct.unpack_from("<II", data, fixups)
    if ver != 0:
        raise XnuEntryError(f"unexpected chained-fixups version {ver}")
    base = fixups + starts_off
    seg_count = struct.unpack_from("<I", data, base)[0]
    for soff in struct.unpack_from(f"<{seg_count}I", data, base + 4):
        if soff == 0:
            continue
        fmt = struct.unpack_from("<H", data, base + soff + 6)[0]
        if fmt != DYLD_CHAINED_PTR_64_KERNEL_CACHE:
            raise XnuEntryError(f"unsupported chained-pointer format {fmt}")


def _cache_base(segs):
    return min(vmaddr for vmaddr, _vs, _fo, _fs in segs.values())


def _decode_ptr(raw, cache_base):
    """A DYLD_CHAINED_PTR_64_KERNEL_CACHE rebase: target VA = cache base + the low-30-bit offset."""
    return cache_base + (raw & 0x3FFFFFFF)


def _va_to_off(segs, va):
    for vmaddr, vmsize, fileoff, filesize in segs.values():
        if vmaddr <= va < vmaddr + vmsize and va - vmaddr < filesize:
            return fileoff + (va - vmaddr)
    return None


# --- locate sysent, then acct ----------------------------------------------------------------- #

def _find_sysent(data, segs):
    """The sysent array in __DATA_CONST: the longest run of 24-byte entries whose sy_call decodes
    into __TEXT_EXEC and whose sy_narg prefix is the syscall table's. Exactly one qualifies."""
    cache_base = _cache_base(segs)
    tx_lo, tx_vs, _tf, _ts = segs["__TEXT_EXEC"][0], *segs["__TEXT_EXEC"][1:]
    tx_hi = segs["__TEXT_EXEC"][0] + segs["__TEXT_EXEC"][1]
    dc_va, _dv, dc_fo, dc_fs = segs["__DATA_CONST"]

    def entry_ok(k):
        raw = struct.unpack_from("<Q", data, dc_fo + k)[0]
        rt, narg = struct.unpack_from("<ih", data, dc_fo + k + 16)
        tgt = _decode_ptr(raw, cache_base)
        return tx_lo <= tgt < tx_hi and 0 <= rt <= 12 and 0 <= narg <= 16

    runs, k = [], 0
    while k + SYSENT_STRIDE <= dc_fs:
        if entry_ok(k):
            start = k
            while k + SYSENT_STRIDE <= dc_fs and entry_ok(k):
                k += SYSENT_STRIDE
            runs.append((start, (k - start) // SYSENT_STRIDE))
        else:
            k += 8
    candidates = []
    for start, count in runs:
        if count < SYSENT_MIN_ENTRIES:
            continue
        nargs = tuple(struct.unpack_from("<h", data, dc_fo + start + i * SYSENT_STRIDE + 20)[0]
                      for i in range(len(SYSENT_NARG_PREFIX)))
        if nargs == SYSENT_NARG_PREFIX:
            candidates.append((start, count))
    if len(candidates) != 1:
        raise XnuEntryError(f"sysent: {len(candidates)} tables match the syscall fingerprint, "
                            "need exactly one")
    start, count = candidates[0]
    if count <= ACCT_SYSCALL:
        raise XnuEntryError(f"sysent has only {count} entries, syscall {ACCT_SYSCALL} is past it")
    return dc_va + start, count


def _acct_va(data, segs):
    sysent_va, _count = _find_sysent(data, segs)
    ent_off = _va_to_off(segs, sysent_va + ACCT_SYSCALL * SYSENT_STRIDE)
    raw, narg = (struct.unpack_from("<Q", data, ent_off)[0],
                 struct.unpack_from("<h", data, ent_off + 20)[0])
    if narg != ACCT_NARG:
        raise XnuEntryError(f"sysent[{ACCT_SYSCALL}].sy_narg = {narg}, expected {ACCT_NARG} "
                            "(acct): the table is not the one this finder knows")
    return _decode_ptr(raw, _cache_base(segs))


# --- AArch64 instruction fields (only the forms acct uses at the site) ------------------------- #

def _is_cbz_w(w):
    """32-bit CBZ (branch when the root check's result is zero)."""
    return (w & 0xFF000000) == 0x34000000


def _cbz_target(va, w):
    imm = (w >> 5) & 0x7FFFF
    if imm & 0x40000:
        imm -= 0x80000
    return va + imm * 4


def _is_branch(w):
    """B / B.cond / CBZ / CBNZ / TBZ / TBNZ -- the conditional and uncond forms that could land
    inside the window. Returns (kind, is_branch)."""
    if (w & 0xFC000000) == 0x14000000:
        return "b"
    if (w & 0xFF000010) == 0x54000000:
        return "bcond"
    if (w & 0x7E000000) == 0x34000000:
        return "cb"
    if (w & 0x7E000000) == 0x36000000:
        return "tb"
    return None


def _branch_target(va, w):
    kind = _is_branch(w)
    if kind == "b":
        imm = w & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
    elif kind == "bcond" or kind == "cb":
        imm = (w >> 5) & 0x7FFFF
        if imm & 0x40000:
            imm -= 0x80000
    elif kind == "tb":
        imm = (w >> 5) & 0x3FFF
        if imm & 0x2000:
            imm -= 0x4000
    else:
        return None
    return va + imm * 4


def _ldr_x_imm0(rt, rn):
    """ldr Xrt, [Xrn]  (unsigned offset 0)."""
    return 0xF9400000 | (rn << 5) | rt


def _movz_w(rd, imm16):
    return 0x52800000 | (imm16 << 5) | rd


def _movk_w_hw1(rd, imm16):
    return 0x72A00000 | (imm16 << 5) | rd


def _hvc(imm16):
    return 0xD4000002 | (imm16 << 5)


def _b(src_va, dst_va):
    return 0x14000000 | (((dst_va - src_va) >> 2) & 0x3FFFFFF)


# --- plan the patch --------------------------------------------------------------------------- #

PROLOGUE_SCAN = 24      # instructions from acct's entry to look for the uap save (mov Xd, x1)
ROOTCHECK_SCAN = 48     # instructions to look for the root check (cbz wN,L ; mov w0,#EPERM)
ACCT_SPAN = 0x400       # bytes of acct to scan for a stray branch into the patch window
N_PATCH = 5             # instructions the patch writes


def find_acct_entry(kernelcache):
    """Plan the auractl XNU entry patch for one kernelcache. Returns a dict:
        acct_va        acct()'s virtual address
        uap_reg        the callee-saved register acct holds uap in
        text_off       offset of the 5-instruction site within __TEXT_EXEC (== its BootKC-rx offset)
        site_va        that site's virtual address
        epilogue_va    the branch target (acct's return-with-errno label)
        expected       the 5 words currently at the site (verified before the patch)
        replacement    the 5 words to write
    Raises XnuEntryError (fail closed) on any ambiguity."""
    segs, fixups = _segments(kernelcache)
    _pointer_format(kernelcache, fixups, segs)
    acct_va = _acct_va(kernelcache, segs)
    tx_va, tx_vs, tx_fo, tx_fs = segs["__TEXT_EXEC"]
    acct_off = _va_to_off(segs, acct_va)
    if acct_off is None or acct_off + ACCT_SPAN > len(kernelcache):
        raise XnuEntryError(f"acct at {acct_va:#x} is outside the kernelcache's file data")

    def word(i):  # i-th instruction from acct's entry
        return struct.unpack_from("<I", kernelcache, acct_off + 4 * i)[0]

    # The uap save: acct copies its second argument (x1 = uap) into a callee-saved register in the
    # prologue. Exactly one `mov Xd, x1` with d in x19..x28.
    uap_regs = [w & 0x1F for i in range(PROLOGUE_SCAN)
                for w in (word(i),) if (w & 0xFFFFFFE0) == 0xAA0103E0 and 19 <= (w & 0x1F) <= 28]
    if len(uap_regs) != 1:
        raise XnuEntryError(f"acct prologue: {len(uap_regs)} `mov Xd,x1` saves of uap, need one")
    uap_reg = uap_regs[0]

    # The root check: `cbz wN, root` immediately followed by `mov w0, #EPERM`. The cbz's target is
    # the root-only path (our site); cbz+8 is acct's return-with-errno label (our branch target).
    checks = [i for i in range(ROOTCHECK_SCAN)
              if _is_cbz_w(word(i)) and word(i + 1) == EPERM_MOV_W0_1]
    if len(checks) != 1:
        raise XnuEntryError(f"acct root check: {len(checks)} `cbz wN,L; mov w0,#1` sites, need one")
    ci = checks[0]
    cbz_va = acct_va + 4 * ci
    site_va = _cbz_target(cbz_va, word(ci))
    epilogue_va = cbz_va + 8
    if not (tx_va <= site_va < tx_va + tx_vs) or site_va <= cbz_va:
        raise XnuEntryError("acct root check branches backwards or out of __TEXT_EXEC")

    site_off = site_va - tx_va
    acct_file_end = acct_off + ACCT_SPAN
    # Nothing inside acct may branch into the middle of the window we overwrite, or the patch would
    # be entered partway. The site's own start (the root-check target) is the only allowed landing.
    for off in range(acct_off, acct_file_end, 4):
        w = struct.unpack_from("<I", kernelcache, off)[0]
        if _is_branch(w):
            tgt = _branch_target(tx_va + (off - tx_fo), w)
            if site_va < tgt < site_va + 4 * N_PATCH:
                raise XnuEntryError(f"a branch targets inside the patch window at {tgt:#x}")

    expected = tuple(struct.unpack_from("<I", kernelcache, tx_fo + site_off + 4 * i)[0]
                     for i in range(N_PATCH))
    replacement = (
        _ldr_x_imm0(1, uap_reg),
        _movz_w(0, AURACTL_HVC_MAGIC & 0xFFFF),
        _movk_w_hw1(0, (AURACTL_HVC_MAGIC >> 16) & 0xFFFF),
        _hvc(AURACTL_HVC_IMM),
        _b(site_va + 4 * 4, epilogue_va),
    )
    return dict(acct_va=acct_va, uap_reg=uap_reg, text_off=site_off, site_va=site_va,
                epilogue_va=epilogue_va, expected=expected, replacement=replacement)


def _main(argv):
    import pathlib

    if not argv:
        print(__doc__.strip().splitlines()[-1])
        return 2
    disas = "--disas" in argv
    path = pathlib.Path([a for a in argv if not a.startswith("-")][0])
    try:
        plan = find_acct_entry(path.read_bytes())
    except XnuEntryError as e:
        print(f"auractl_xnu: FAIL CLOSED: {e}")
        return 1
    print(f"acct()        {plan['acct_va']:#x}  (uap in x{plan['uap_reg']})")
    print(f"patch site    {plan['site_va']:#x}  (__TEXT_EXEC/BootKC-rx +{plan['text_off']:#x})")
    print(f"branch to     {plan['epilogue_va']:#x}  (acct's return-with-errno label)")
    for i, (e, n) in enumerate(zip(plan["expected"], plan["replacement"])):
        print(f"  [{i}] {e:08x} -> {n:08x}")
    return 0


if __name__ == "__main__":
    sys.exit(_main(sys.argv[1:]))
