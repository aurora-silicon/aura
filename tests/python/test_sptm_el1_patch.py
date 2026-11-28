# SPDX-License-Identifier: MIT
"""The EL1 rewrite of SPTM and XNU for aura's classic hypervisor (proxyclient/aura/sptm_el1_patch.py).

Synthetic images always run. With SPTM_EL1_IMAGES=<dir with sptm.bin and kernelcache.macho> the
real images are checked too: after the rewrite SPTM's text keeps no architectural EL2 or _EL12
register access except the VM-only SP_EL1 accesses its context switch makes.
"""
import os
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
from aura import sptm_el1_patch as pe  # noqa: E402

NOP = 0xD503201F
TEXT, EXEC = 0xFFFFFFF027004000, 0xFFFFFFF027008000


def macho(text_words, exec_words, entry):
    """A thin arm64 Mach-O: __TEXT (header, then text_words at +0x1000) and __TEXT_EXEC."""
    cmds = b""
    for name, vm, fsz in (("__TEXT", TEXT, 0x4000), ("__TEXT_EXEC", EXEC, 0x4000)):
        cmds += struct.pack("<II16sQQQQIIII", 0x19, 72, name.encode(), vm, fsz, vm - TEXT, fsz,
                            5, 5, 0, 0)
    cmds += struct.pack("<IIII", 5, 16 + 68 * 4, 6, 68) + b"\0" * (32 * 8) + \
        struct.pack("<Q", entry) + b"\0" * (68 * 4 - 32 * 8 - 8)
    hdr = struct.pack("<IIIIIIII", 0xFEEDFACF, 0x0100000C, 0, 2, 3, len(cmds), 0, 0)
    img = bytearray((hdr + cmds).ljust(0x8000, b"\0"))
    struct.pack_into(f"<{len(text_words)}I", img, 0x1000, *text_words)
    struct.pack_into(f"<{len(exec_words)}I", img, 0x4000, *exec_words)
    return img


def mrs(reg, rt):
    return pe.encode(True, reg, rt)


def msr(reg, rt):
    return pe.encode(False, reg, rt)


def words(img, va, n):
    return list(struct.unpack_from(f"<{n}I", img, pe.va_to_off(img, va)))


GXF_CONFIG_EL12 = pe.enc(3, 6, 15, 15, 1)
CTRR_CTL_EL2 = pe.enc(3, 4, 15, 11, 4)
CURRENT_EL = pe.enc(3, 0, 4, 2, 2)


def test_sptm_rules():
    code = [mrs(pe.TPIDR_GL2, 8), mrs(pe.TPIDR_EL2, 3), msr(pe.ELR_EL2, 1), mrs(pe.CPTR_EL2, 5),
            mrs(pe.HCR_EL2, 1), msr(pe.HCR_EL2, 0), msr(pe.enc(3, 4, 2, 1, 0), 0),
            msr(GXF_CONFIG_EL12, 31), mrs(CURRENT_EL, 2), mrs(CTRR_CTL_EL2, 4), mrs(pe.HCR_EL2, 31)]
    data = [mrs(pe.HCR_EL2, 1)]  # an MRS-shaped word outside the executable segments
    img = macho(data, code, EXEC)
    pe.patch("sptm", img)
    assert words(img, EXEC, len(code)) == [
        mrs(pe.TPIDR_GL1, 8), mrs(pe.TPIDR_EL1, 3), msr(pe.ELR_EL1, 1), mrs(pe.CPACR_EL1, 5),
        0xD2800001,  # HCR_EL2 read: mov x1, #0
        NOP, NOP, NOP,  # HCR_EL2, VTTBR_EL2 and GXF_CONFIG_EL12 writes
        mrs(CURRENT_EL, 2),  # kept: SPTM builds its SPSRs from it
        mrs(CTRR_CTL_EL2, 4),  # kept: Apple's EL2 registers trap to the hypervisor
        NOP,  # a read into xzr has nothing to set
    ]
    assert words(img, TEXT + 0x1000, 1) == data


def test_sptm_without_its_per_cpu_accesses_is_refused():
    img = macho([], [mrs(pe.HCR_EL2, 1)], EXEC)
    with pytest.raises(ValueError, match="not an image these rules fit"):
        pe.patch("sptm", img)


def test_xnu_rules_leave_el2_alone():
    pmu = pe.enc(3, 1, 15, 7, 2)
    code = [mrs(pe.TPIDR_EL2, 0), mrs(pe.HCR_EL2, 1), msr(pmu, 17)]
    img = macho([], code, EXEC)
    pe.patch("xnu", img)
    assert words(img, EXEC, 3) == code[:2] + [NOP]


def adrp(rd, pc, target):
    imm = ((target & ~0xFFF) - (pc & ~0xFFF)) >> 12
    return 0x90000000 | ((imm & 3) << 29) | (((imm >> 2) & 0x7FFFF) << 5) | rd


def test_early_vector_trap():
    vbar = EXEC + 0x1000
    body = EXEC + 0x40
    code = [NOP] * 0x1000
    code[0] = 0x14000000 | ((body - EXEC) // 4)  # b body
    i = (body - EXEC) // 4
    code[i:i + 3] = [adrp(0, body, vbar), 0x91000000 | (vbar & 0xFFF) << 10,
                     msr(pe.enc(3, 0, 12, 0, 0), 0)]
    img = macho([], code, EXEC)
    assert pe.early_vector_trap(img, 0x5E00) == vbar
    for k in range(16):
        assert words(img, vbar + 0x80 * k, 2) == [pe.HVC | (0x5E00 + k) << 5, NOP]


def test_early_vector_needs_a_vbar_write():
    img = macho([], [NOP] * 128, EXEC)
    with pytest.raises(ValueError):
        pe.early_vbar(img)


IMAGES = os.environ.get("SPTM_EL1_IMAGES")


@pytest.mark.skipif(not IMAGES, reason="real images not given (SPTM_EL1_IMAGES)")
def test_real_sptm_keeps_no_el2_access():
    img = bytearray((Path(IMAGES) / "sptm.bin").read_bytes())
    pe.patch("sptm", img)
    pe.early_vector_trap(img, 0x5E00)
    left = set()
    for s in pe.exec_segments(img):
        for off in range(s.fileoff, s.fileoff + s.filesize, 4):
            d = pe.decode(struct.unpack_from("<I", img, off)[0])
            if d and d[1][0] == 3 and d[1][1] in (4, 5) and d[1][2] != 15:
                left.add(d[1])
    assert left <= {pe.enc(3, 4, 4, 1, 0)}  # SP_EL1, in its VM context switch only
