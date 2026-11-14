# SPDX-License-Identifier: MIT
"""Find the virtualized SPTM's patch sites in any SPTM build.

Each site is a short run of SPTM's own instructions, compared with PC-relative immediates and load
and store offsets masked, so one signature holds across builds whose code only moved. A signature
must match exactly once in the image, or the image is refused. The patches are those
run_guest_sptm.py applies before SPTM runs; see the comments there for why each one is needed.
"""

import struct

NOP = 0xD503201F
MTE_MARKER = b"sptm_init_mte_boundaries"


def mask(word):
    """The bits of an instruction that do not change when code moves or a struct grows."""
    if word & 0x7C000000 == 0x14000000:  # B, BL
        return 0xFC000000
    if word & 0xFF000010 == 0x54000000:  # B.cond
        return 0xFF00001F
    if word & 0x7E000000 == 0x34000000:  # CBZ, CBNZ
        return 0xFF00001F
    if word & 0x7E000000 == 0x36000000:  # TBZ, TBNZ
        return 0xFFF8001F
    if word & 0x1F000000 == 0x10000000:  # ADR, ADRP
        return 0x9F00001F
    if word & 0x3B000000 == 0x39000000:  # LDR/STR (unsigned offset)
        return 0xFFC003FF
    if word & 0x3B000000 == 0x18000000:  # LDR (literal)
        return 0xFF00001F
    return 0xFFFFFFFF


# Windows of 25G83's sptm.t8140 (MTE: 26A428's sptm.t8142). `at` is the patched word's index.
APSTS = (0xD5033FDF, 0xD508871F, 0xD5033B9F, 0xD53EFC80, 0x92400001, 0xB4FFFFC1, 0xD53CF080,
         0xB2400000)
# The CTXR CTL values aura answers with (sptm_virt_gl.c) are this window's MOVZ/MOVK immediates.
CTRR_RESET = (0xD2803343, 0xF2A01543, 0xF2F80003, 0xD2805544, 0xF2A01344, 0xF2F80004, 0x796830A6,
              0xD0000225, 0x34000986, 0xD538B193, 0xD538B006)
CTRR = (0x9274CE95, 0xEB06027F, 0xFA4702A0, 0x54002D01, 0xD5384246, 0xF10020DF, 0x54000AC1,
        0xD538B853, 0xD538B6C6)
CTXR_LOCK = (0xD53CF2A9, 0xB24107E8, 0xEB08013F, 0x54001A41, 0xD53CF249, 0xEB08013F, 0x54001AE1,
             0xD53CFB89, 0xEB08013F, 0x54001BA1, 0xD53CFBA9, 0xEB08013F, 0x54001C41, 0xD5033FDF)
AMCC = (0x9ACC21AC, 0x8B0C016C, 0xF0FFFD8D, 0xF94529AD, 0xF0FFFD8F, 0xF9452DEF, 0x8B0901CE,
        0xEB0D01DF, 0x54003E41, 0xCA0C01F0, 0xF140061F, 0x54003DE2, 0xF00000EC, 0x3977A18C,
        0x3700020C, 0xF940314C, 0x9AC8218C, 0xF950314A, 0x9AC82148, 0x8B080168, 0xF0FFFD8A,
        0xF945314A, 0xF0FFFD8B, 0xF945356B, 0x8B090189, 0xEB0A013F, 0x54004A01, 0xCA08016C,
        0xF140059F, 0x540049A2)
# sptm_init_mte_boundaries: the tag storage from the tag-offset register (13 words; the window's
# base register differs between builds), then the comparison with what the window implies, then
# a TBZ on the register's enable bit, which some builds precede with an ADRP of their own.
MTE_HEAD = (0xD538B908, 0x926C5508, 0xF0FFFEF4, 0xF9010688, (0xB9420329, 0xFFFFFC1F), 0xD372C529,
            0x8B080128, 0xF0FFFEEA, 0xF9010948, 0xA9402B88, 0xCB090108, 0x8B0A0108, 0x926CAD08)
MTE_CMP = (0xD538B909, 0x9240F92A, 0xEB08015F, 0x54002861)
MTE_TBZ = 0xB6F82929
MTE_ADRP = (0xF0FFFED9, 0x9F000000)  # any ADRP
# The tag storage's base and end from the window instead of the tag-offset register, built from
# the head's own instructions: the two ADRPs keep their places, so their results hold.
MTE_ORDER = (4, 5, 2, 9, 10, 11, 12, 7, 3, 6, 8, None, None)


class SptmPatchError(Exception):
    pass


def _words(image):
    return struct.unpack_from("<%dI" % (len(image) // 4), image)


def find(words, sig):
    """Word indices where `sig` matches, PC-relative fields masked. A (word, mask) entry masks
    more of that word."""
    sig_m = []
    for s in sig:
        w, m = s if isinstance(s, tuple) else (s, 0xFFFFFFFF)
        m &= mask(w)
        sig_m.append((w & m, m))
    v0, m0 = sig_m[0]
    hits = []
    for i in range(len(words) - len(sig) + 1):
        if words[i] & m0 != v0:
            continue
        if all(words[i + k] & m == v for k, (v, m) in enumerate(sig_m)):
            hits.append(i)
    return hits


def _one(words, sig, what):
    hits = find(words, sig)
    if len(hits) != 1:
        raise SptmPatchError(f"{what}: {len(hits)} matches in this SPTM, need exactly one")
    return hits[0]


def _branch_target(words, i):
    """Word index a B.cond or CBZ/CBNZ at word i jumps to."""
    imm = (words[i] >> 5) & 0x7FFFF
    if imm & 0x40000:
        imm -= 0x80000
    return i + imm


def _b(src, dst):
    return 0x14000000 | ((dst - src) & 0x3FFFFFF)


def plan(image, entry_off):
    """The patches for one SPTM image: a dict with
    patches: [(what, offset from entry, mask, expected, replacement)],
    ctrr_stash_off: SPTM's CTRR stash offset (aura's sptm_virt_ctrr_stash_off),
    mte: (offset from entry, expected words, replacement words) or None.
    Raises SptmPatchError if a site is missing or not unique."""
    words = _words(image)
    e = entry_off // 4
    out = []

    def patch(what, i, new):
        out.append((what, 4 * (i - e), 0xFFFFFFFF, words[i], new))

    i = _one(words, APSTS, "APSTS poll") + 5
    patch("APSTS poll", i, NOP)
    c = _one(words, CTRR, "CTRR check") + 6
    tail = _branch_target(words, c)  # the check's success tail
    patch("CTRR check", c, _b(c, tail))
    r = _one(words, CTRR_RESET, "CTRR reset-path check") + 8
    patch("CTRR reset-path check", r, _b(r, tail))
    x = _one(words, CTXR_LOCK, "ctXr lock check")
    for k in (3, 6, 9, 12):
        patch("ctXr lock check", x + k, NOP)
    a = _one(words, AMCC, "AMCC CTRR match")
    for k in (8, 11, 26, 29):
        patch("AMCC CTRR match", a + k, NOP)
    # The stash SPTM keeps its expected CTRR bounds in: the offset its AMCC check loads them from.
    stash = ((words[a + 3] >> 10) & 0xFFF) * 8

    mte = None
    if MTE_MARKER in image:
        shapes = [(h, False) for h in find(words, MTE_HEAD + MTE_CMP + (MTE_TBZ,))]
        shapes += [(h, True) for h in find(words, MTE_HEAD + MTE_CMP + (MTE_ADRP, MTE_TBZ))]
        if len(shapes) != 1:
            raise SptmPatchError(f"MTE tag-storage sequence: {len(shapes)} matches in this SPTM, "
                                 "need exactly one")
        m, adrp = shapes[0]
        n = len(MTE_HEAD) + len(MTE_CMP) + 1 + adrp
        found = words[m:m + n]
        new = [NOP if k is None else found[k] for k in MTE_ORDER] + [NOP] * len(MTE_CMP)
        new += [found[n - 2]] if adrp else []
        new.append(NOP)
        mte = (4 * (m - e), tuple(found), tuple(new))
    return dict(patches=out, ctrr_stash_off=stash, mte=mte)
