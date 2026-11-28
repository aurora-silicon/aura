# SPDX-License-Identifier: MIT
"""Instruction rewrites that let SPTM and XNU run one level down, at EL1/GL1, under aura's
classic hypervisor on SoCs without FEAT_NV2 (M1-M3).

Natively SPTM runs at GL2 and XNU at EL2 with HCR_EL2.E2H set. aura holds EL2 and GL2, so both
run one level lower. Their guarded world then lands in GL1, which the hardware supports as it
did for PPL. What is left are accesses to registers that do not exist one level down. A
non-VHE EL2 register read or written at EL1 is UNDEFINED and is taken at EL1, so the hypervisor
never sees it; each such access is rewritten here, by encoding, in the executable segments:

- Registers that VHE redirects (an _EL1 encoding at EL2 with E2H reaches the _EL2 register)
  become the _EL1 encoding: ELR/SPSR/ESR/FAR/AFSR1, and CPTR_EL2, whose E2H layout is CPACR's.
  Natively both encodings reach the same register, so this keeps native behaviour.
- TPIDR_EL2, SPTM's per-CPU pointer outside its guarded world, becomes TPIDR_EL1. Only SPTM's
  own normal-world code (its bootstrap and reset path) reads it, before XNU owns TPIDR_EL1.
- TPIDR_GL2, SPTM's per-CPU pointer inside its guarded world, becomes TPIDR_GL1: at GL1 that is
  SPTM's own bank. SPTM natively touches TPIDR_GL1 only when it switches a VM's context.
- HCR_EL2 reads become `mov Xt, #0` and writes a NOP. SPTM reads HCR_EL2 to ask whether a
  VM guest took the exception (VM set, TGE clear), which here it never is.
- Registers used only to run VMs (stage 2, the GIC's virtual interface, the counter offset, the
  _EL12 aliases, ...) read as zero and ignore writes: nothing starts a VM.

CurrentEL is left alone: SPTM builds its SPSRs from it and its bootstrap already checks it.
Apple's own EL2 and GL2 registers are left alone: from EL1 and GL1 they trap to the hypervisor.
Each rule's count is reported, and an SPTM without the per-CPU accesses above is refused.

SPTM's early EL1 exception vector (the first VBAR_EL1 its __start sets) records an exception and
halts in WFE, which loses the registers. early_vector_trap() turns its 16 entries into HVCs, so
the hypervisor stops with the guest's state intact.
"""
import struct

from .sptm_virt_plan import macho_info


def enc(op0, op1, crn, crm, op2):
    return (op0, op1, crn, crm, op2)


ELR_EL2, ELR_EL1 = enc(3, 4, 4, 0, 1), enc(3, 0, 4, 0, 1)
SPSR_EL2, SPSR_EL1 = enc(3, 4, 4, 0, 0), enc(3, 0, 4, 0, 0)
ESR_EL2, ESR_EL1 = enc(3, 4, 5, 2, 0), enc(3, 0, 5, 2, 0)
FAR_EL2, FAR_EL1 = enc(3, 4, 6, 0, 0), enc(3, 0, 6, 0, 0)
AFSR1_EL2, AFSR1_EL1 = enc(3, 4, 5, 1, 1), enc(3, 0, 5, 1, 1)
CPTR_EL2, CPACR_EL1 = enc(3, 4, 1, 1, 2), enc(3, 0, 1, 0, 2)
TPIDR_EL2, TPIDR_EL1 = enc(3, 4, 13, 0, 2), enc(3, 0, 13, 0, 4)
TPIDR_GL2, TPIDR_GL1 = enc(3, 6, 15, 11, 1), enc(3, 6, 15, 10, 1)
AFSR1_GL2, AFSR1_GL1 = enc(3, 6, 15, 0, 2), enc(3, 6, 15, 0, 1)
HCR_EL2 = enc(3, 4, 1, 1, 0)

REMAP = {
    ELR_EL2: ELR_EL1, SPSR_EL2: SPSR_EL1, ESR_EL2: ESR_EL1, FAR_EL2: FAR_EL1,
    AFSR1_EL2: AFSR1_EL1, CPTR_EL2: CPACR_EL1, TPIDR_EL2: TPIDR_EL1,
    TPIDR_GL2: TPIDR_GL1, AFSR1_GL2: AFSR1_GL1,
}

# VM-only registers: read as zero, writes ignored.
RAZ_WI = {
    HCR_EL2: "HCR_EL2",
    enc(3, 4, 1, 1, 1): "MDCR_EL2", enc(3, 4, 1, 1, 7): "HACR_EL2",
    enc(3, 4, 1, 2, 2): "HCRX_EL2", enc(3, 4, 2, 1, 0): "VTTBR_EL2",
    enc(3, 4, 2, 1, 2): "VTCR_EL2", enc(3, 4, 0, 0, 0): "VPIDR_EL2",
    enc(3, 4, 0, 0, 5): "VMPIDR_EL2", enc(3, 4, 6, 0, 4): "HPFAR_EL2",
    enc(3, 4, 14, 0, 3): "CNTVOFF_EL2", enc(3, 4, 14, 1, 0): "CNTHCTL_EL2",
    enc(3, 4, 12, 11, 0): "ICH_HCR_EL2", enc(3, 4, 12, 11, 7): "ICH_VMCR_EL2",
    enc(3, 5, 1, 0, 0): "SCTLR_EL12", enc(3, 5, 1, 0, 2): "CPACR_EL12",
    enc(3, 5, 2, 0, 0): "TTBR0_EL12", enc(3, 5, 2, 0, 1): "TTBR1_EL12",
    enc(3, 5, 2, 0, 2): "TCR_EL12", enc(3, 5, 10, 2, 0): "MAIR_EL12",
    enc(3, 5, 10, 3, 0): "AMAIR_EL12", enc(3, 5, 14, 1, 0): "CNTKCTL_EL12",
    enc(3, 5, 4, 0, 0): "SPSR_EL12", enc(3, 5, 4, 0, 1): "ELR_EL12",
    enc(3, 5, 12, 0, 0): "VBAR_EL12", enc(3, 5, 5, 2, 0): "ESR_EL12",
    enc(3, 5, 6, 0, 0): "FAR_EL12", enc(3, 5, 13, 0, 1): "CONTEXTIDR_EL12",
    # A pair SPTM's normal-world bootstrap writes with one value (a PMU counter configuration,
    # like PMCR1's); both are UNDEFINED at EL1.
    enc(3, 6, 15, 8, 5): "s3_6_c15_c8_5",
    enc(3, 1, 15, 8, 2): "s3_1_c15_c8_2",
}

# Apple's _EL12/_EL02 aliases (an EL1 register reached from EL2, as SPTM configures the level
# below it) do not exist at EL1: UNDEFINED there, e.g. GXF_CONFIG_EL12 in SPTM's GXF setup.
def _apple_aliases():
    import json, os
    path = os.path.join(os.path.dirname(__file__), "apple_regs.json")
    out = {}
    for r in json.load(open(path)):
        n, e = r.get("name", ""), r.get("enc")
        if e and (n.endswith("_EL12") or n.endswith("_EL02")):
            out[tuple(e)] = n
    return out


RAZ_WI.update(_apple_aliases())

# XNU checks CurrentEL before its own EL2 registers, except for this one, which its PMU setup
# writes with PMCR1's value and which is UNDEFINED at EL1.
XNU_RAZ_WI = {
    enc(3, 1, 15, 7, 2): "s3_1_c15_c7_2",
}

NOP = 0xd503201f

# Registers an image must use for these rules to fit it: an SPTM that keeps its per-CPU pointer
# in TPIDR_EL2/TPIDR_GL2 is the SPTM they were written for.
EXPECT = {"sptm": (TPIDR_GL2, TPIDR_EL2), "txm": (), "xnu": ()}


def decode(w):
    """(is_read, (op0, op1, CRn, CRm, op2), Rt) for an MRS/MSR (register), else None."""
    top = w & 0xfff00000
    if top not in (0xd5300000, 0xd5100000):
        return None
    return (top == 0xd5300000,
            (2 + ((w >> 19) & 1), (w >> 16) & 7, (w >> 12) & 15, (w >> 8) & 15, (w >> 5) & 7),
            w & 31)


def encode(is_read, reg, rt):
    op0, op1, crn, crm, op2 = reg
    return ((0xd5300000 if is_read else 0xd5100000) | ((op0 - 2) << 19) | (op1 << 16) |
            (crn << 12) | (crm << 8) | (op2 << 5) | rt)


def rewrite(w, remap=REMAP, raz_wi=RAZ_WI):
    """The replacement for one instruction word and a label, or None to keep it."""
    d = decode(w)
    if d is None:
        return None
    is_read, reg, rt = d
    if reg in remap:
        return encode(is_read, remap[reg], rt), f"{reg}->{remap[reg]}"
    if reg in raz_wi:
        if is_read:
            return (0xd2800000 | rt if rt != 31 else NOP), f"{raz_wi[reg]} read->0"
        return NOP, f"{raz_wi[reg]} write->nop"
    return None


RULES = {"sptm": (REMAP, RAZ_WI), "txm": ({}, {}), "xnu": ({}, XNU_RAZ_WI)}


def exec_segments(img):
    info = macho_info(bytes(img))
    return [s for s in info.segs.values() if "EXEC" in s.name]


def va_to_off(img, va):
    info = macho_info(bytes(img))
    for s in info.segs.values():
        if s.vmaddr <= va < s.vmaddr + s.filesize:
            return s.fileoff + va - s.vmaddr
    raise ValueError(f"VA {va:#x} is not in a file-backed segment")


def patch(name, img, extra=()):
    """Rewrite img (a bytearray) in place; return one summary line per rule.

    extra: (va, expected word, new word, why) for one-off sites."""
    counts, regs = {}, {}
    remap, raz_wi = RULES[name]
    for s in exec_segments(img):
        for off in range(s.fileoff, s.fileoff + s.filesize, 4):
            (w,) = struct.unpack_from("<I", img, off)
            r = rewrite(w, remap, raz_wi)
            if r is None:
                continue
            new, label = r
            struct.pack_into("<I", img, off, new)
            counts[label] = counts.get(label, 0) + 1
            d = decode(w)
            regs[d[1]] = regs.get(d[1], 0) + 1
    for reg in EXPECT[name]:
        if not regs.get(reg):
            raise ValueError(f"{name}: no access to {reg}: not an image these rules fit")
    out = [f"{k}: {v}" for k, v in sorted(counts.items())]
    for va, old, new, why in extra:
        off = va_to_off(img, va)
        (cur,) = struct.unpack_from("<I", img, off)
        if cur != old:
            raise ValueError(f"{name} {va:#x}: found {cur:#010x}, expected {old:#010x} ({why})")
        struct.pack_into("<I", img, off, new)
        out.append(f"{va:#x}: {old:#010x} -> {new:#010x}  {why}")
    return out


HVC = 0xd4000002


def _adrp_add(words, i, base_va):
    """The address an ADRP at word i and the ADD after it build, if they form a pair."""
    a, b = words[i], words[i + 1]
    if a & 0x9f000000 != 0x90000000 or b & 0xff800000 != 0x91000000:
        return None
    rd = a & 31
    if (b & 31) != rd or ((b >> 5) & 31) != rd:
        return None
    imm = ((a >> 29) & 3) | (((a >> 5) & 0x7ffff) << 2)
    if imm & (1 << 20):
        imm -= 1 << 21
    pc = base_va + 4 * i
    return ((pc & ~0xfff) + (imm << 12) + ((b >> 10) & 0xfff)), rd


def early_vbar(img):
    """The first VBAR_EL1 SPTM's __start sets: follow the entry's branch, then the first
    `adrp; add; msr VBAR_EL1` there."""
    info = macho_info(bytes(img))
    seg = next(s for s in exec_segments(img) if s.vmaddr <= info.entry < s.vmaddr + s.filesize)
    words = struct.unpack_from(f"<{seg.filesize // 4}I", img, seg.fileoff)
    i = (info.entry - seg.vmaddr) // 4
    if words[i] & 0xfc000000 == 0x14000000:  # b <__start body>
        imm = words[i] & 0x3ffffff
        i += imm - (1 << 26) if imm & (1 << 25) else imm
    msr_vbar = encode(False, enc(3, 0, 12, 0, 0), 0)
    for j in range(i, i + 64):
        r = _adrp_add(words, j, seg.vmaddr)
        if r and words[j + 2] == msr_vbar | r[1]:
            return r[0]
    raise ValueError("SPTM's __start sets no VBAR_EL1 from an ADRP/ADD pair")


def early_vector_trap(img, iss_base):
    """Make each of the early vector's 16 entries `hvc #(iss_base + index)`; return its VA."""
    vbar = early_vbar(img)
    for k in range(16):
        struct.pack_into("<I", img, va_to_off(img, vbar + 0x80 * k), HVC | ((iss_base + k) << 5))
    return vbar
