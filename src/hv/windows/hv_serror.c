/* SPDX-License-Identifier: MIT */

/*
 * T8152 guest SError attribution.  See hv_serror.h.  Written on this branch from the Arm ARM
 * descriptions of the VMSAv8-64 translation regime (TCR_ELx, TTBRn_ELx, descriptor formats)
 * and the A64 loads-and-stores encoding group; no third-party source.
 *
 * The pure parts (the stage-1 walk, the instruction decode, the kind table) are replayed on
 * the host by tests/e2e/windows_serror_host.c; the report part reads only guest RAM and the
 * carrier's own stage-2 table.
 */

#include "hv_serror.h"
#include "hv_t8152.h"
#include "soc.h"
#include "utils.h"

#ifndef HV_SERROR_HOST_TEST
#include "cpu_regs.h"
#include "hv.h"
#endif

/* ---- pure helpers -------------------------------------------------------------------------- */

static const char *const kind_names[HV_SERR_IPA_KINDS] = {
    [HV_SERR_IPA_RAM] = "guest RAM",
    [HV_SERR_IPA_PASSTHROUGH] = "MMIO passthrough (Device-nGnRnE, reaches hardware)",
    [HV_SERR_IPA_EXCEPTION] = "mapped identity exception (reaches memory outside guest RAM)",
    [HV_SERR_IPA_REGISTERED] = "registered identity exception, not mapped (would trap)",
    [HV_SERR_IPA_EMULATED] = "emulated by a carrier hook (traps)",
    [HV_SERR_IPA_UNMAPPED] = "unmapped (a stage-2 abort, not an SError)",
    [HV_SERR_IPA_DENIED] = "denied (never mapped; would trap)",
    [HV_SERR_IPA_OTHER_HW] = "hardware leaf no policy list explains",
};

const char *hv_serror_kind_name(int kind)
{
    if (kind < 0 || kind >= HV_SERR_IPA_KINDS)
        return "?";
    return kind_names[kind];
}

bool hv_serror_kind_candidate(int kind)
{
    return kind == HV_SERR_IPA_PASSTHROUGH || kind == HV_SERR_IPA_EXCEPTION ||
           kind == HV_SERR_IPA_OTHER_HW;
}

int hv_serror_s1_walk(const struct hv_serror_s1 *s1, u64 va, hv_serror_rd64_fn *rd, void *ctx,
                      u64 *ipa)
{
    *ipa = 0;
    if (!(s1->sctlr & 1)) {
        *ipa = va;
        return HV_SERR_S1_OFF;
    }

    bool upper = (va >> 55) & 1;
    u64 tcr = s1->tcr;
    unsigned int tsz = upper ? (tcr >> 16) & 0x3f : tcr & 0x3f;
    bool epd = upper ? (tcr >> 23) & 1 : (tcr >> 7) & 1;
    unsigned int tg = upper ? (tcr >> 30) & 3 : (tcr >> 14) & 3;
    unsigned int g;
    if (upper)
        g = tg == 2 ? 12 : tg == 1 ? 14 : tg == 3 ? 16 : 0;
    else
        g = tg == 0 ? 12 : tg == 2 ? 14 : tg == 1 ? 16 : 0;
    if (!g)
        return HV_SERR_S1_GRANULE;

    unsigned int in = 64 - tsz;
    if (in < 25 || in > 48)
        return HV_SERR_S1_RANGE;
    /* VA[55:in] must all equal VA[55] (the top byte is left to TBI). */
    u64 hi = (va & GENMASK(55, 0)) >> in;
    u64 want = upper ? (BIT(56 - in) - 1) : 0;
    if (hi != want)
        return HV_SERR_S1_RANGE;
    if (epd)
        return HV_SERR_S1_FAULT;

    unsigned int stride = g - 3;
    int levels = (int)((in - g + stride - 1) / stride);
    int first = 4 - levels;
    u64 table = (upper ? s1->ttbr1 : s1->ttbr0) & GENMASK(47, 1) & ~7UL;

    for (int level = first; level <= 3; level++) {
        unsigned int shift = g + stride * (unsigned int)(3 - level);
        unsigned int bits = level == first ? in - shift : stride;
        u64 idx = (va >> shift) & (BIT(bits) - 1);
        u64 desc;
        if (!rd(ctx, table + idx * 8, &desc))
            return HV_SERR_S1_UNREADABLE;
        if (!(desc & 1))
            return HV_SERR_S1_FAULT;
        bool table_bit = desc & 2;
        if (level == 3) {
            if (!table_bit)
                return HV_SERR_S1_FAULT; /* reserved at level 3 */
            *ipa = (desc & GENMASK(47, g)) | (va & (BIT(g) - 1));
            return HV_SERR_S1_OK;
        }
        if (!table_bit) {
            if (level == 0)
                return HV_SERR_S1_FAULT; /* no level-0 blocks without 52-bit/LPA2 */
            *ipa = (desc & GENMASK(47, shift)) | (va & (BIT(shift) - 1));
            return HV_SERR_S1_OK;
        }
        table = desc & GENMASK(47, g);
    }
    return HV_SERR_S1_FAULT;
}

static s64 sext(u64 v, unsigned int bits)
{
    return (s64)(v << (64 - bits)) >> (64 - bits);
}

bool hv_serror_decode_mem(u32 insn, u64 pc, const u64 *regs, u64 sp, bool executed,
                          struct hv_serror_mem *out)
{
    /* Loads and stores: op0 = x1x0, i.e. bit 27 set and bit 25 clear. */
    if (!(insn & BIT(27)) || (insn & BIT(25)))
        return false;

    unsigned int rn = (insn >> 5) & 31;
    u64 base = rn == 31 ? sp : regs[rn];
    unsigned int op29_27 = (insn >> 27) & 7;
    bool v = insn & BIT(26);
    *out = (struct hv_serror_mem){.rn = rn, .dir = -1, .addr = base};

    if (op29_27 == 3) {
        /* Load register (literal): PC + imm19 * 4. */
        out->pc_relative = true;
        out->dir = 0;
        out->addr = pc + (u64)(sext((insn >> 5) & 0x7ffff, 19) * 4);
        return true;
    }

    if (op29_27 == 7) {
        /* Single register: size [31:30], opc [23:22]. */
        unsigned int size = insn >> 30, opc = (insn >> 22) & 3;
        unsigned int scale = (v && size == 0 && (opc & 2)) ? 4 : size;
        out->dir = (!v && opc == 0) || (v && !(opc & 1)) ? 1 : 0;
        if (!v && size == 3 && opc == 2)
            out->dir = -1; /* PRFM: no access */
        if (insn & BIT(24)) {
            out->addr = base + ((u64)((insn >> 10) & 0xfff) << scale);
            return true;
        }
        if (insn & BIT(21)) {
            if (((insn >> 10) & 3) == 2) {
                /* Register offset: Rm extended by option [15:13], shifted by S. */
                u64 rm = regs[(insn >> 16) & 31];
                if (((insn >> 16) & 31) == 31)
                    rm = 0;
                unsigned int option = (insn >> 13) & 7;
                if (option == 2)
                    rm = (u32)rm;
                else if (option == 6)
                    rm = (u64)(s64)(s32)(u32)rm;
                out->addr = base + (rm << ((insn & BIT(12)) ? scale : 0));
            } else {
                out->dir = -1; /* atomic memory operations: address = base */
            }
            return true;
        }
        s64 imm9 = sext((insn >> 12) & 0x1ff, 9);
        switch ((insn >> 10) & 3) {
            case 1: /* post-index: accessed at the old base */
                out->addr = executed ? base - (u64)imm9 : base;
                break;
            case 3: /* pre-index: accessed at the new base */
                out->addr = executed ? base : base + (u64)imm9;
                break;
            default: /* unscaled, unprivileged */
                out->addr = base + (u64)imm9;
                break;
        }
        return true;
    }

    if (op29_27 == 5) {
        /* Register pair: opc [31:30], [24:23] 01 post, 10 offset, 11 pre, 00 no-allocate. */
        unsigned int opc = insn >> 30;
        unsigned int scale = v ? 2 + opc : 2 + (opc >> 1);
        s64 off = sext((insn >> 15) & 0x7f, 7) * (s64)BIT(scale);
        out->dir = (insn & BIT(22)) ? 0 : 1;
        switch ((insn >> 23) & 3) {
            case 1:
                out->addr = executed ? base - (u64)off : base;
                break;
            case 3:
                out->addr = executed ? base : base + (u64)off;
                break;
            default:
                out->addr = base + (u64)off;
                break;
        }
        return true;
    }

    /* Exclusives, ordered, SIMD structures, tags...: the base register is the address. */
    if (op29_27 == 1 && !(insn & BIT(24)) && !v)
        out->dir = (insn & BIT(22)) ? 0 : 1;
    return true;
}

/* ---- the report ------------------------------------------------------------------------------ */

#ifndef HV_SERROR_HOST_TEST
extern uint64_t vaddr_bits;
#define PTE_TYPE_SW GENMASK(52, 50)
#define SPTE_HOOK_TYPE 1

static u64 s2_lookup(u64 ipa)
{
    return ipa < BIT(vaddr_bits) ? hv_pt_walk(ipa) : 0;
}

static u64 rd_guest_ram_base, rd_guest_ram_top;

static bool rd_guest_ram(void *ctx, u64 ipa, u64 *out)
{
    (void)ctx;
    if ((ipa & 7) || ipa < rd_guest_ram_base || ipa >= rd_guest_ram_top)
        return false;
    *out = read64(ipa);
    return true;
}

static bool read_insn(u64 ipa, u32 *out)
{
    if ((ipa & 3) || ipa < rd_guest_ram_base || ipa >= rd_guest_ram_top)
        return false;
    *out = read32(ipa);
    return true;
}
#else
u64 stub_s2_lookup(u64 ipa);
bool stub_rd_guest_ram(void *ctx, u64 ipa, u64 *out);
bool stub_read_insn(u64 ipa, u32 *out);
#define s2_lookup        stub_s2_lookup
#define rd_guest_ram     stub_rd_guest_ram
#define read_insn        stub_read_insn
#define PTE_TYPE_SW      GENMASK(52, 50)
#define SPTE_HOOK_TYPE   1
#endif

/* What an IPA is now: the stage-2 leaf first, then the policy lists for its name. */
static int classify(u64 ipa, const char **name)
{
    int policy = hv_t8152_describe_ipa(ipa, name);
    u64 pte = s2_lookup(ipa);
    if (pte & 1) {
        switch (policy) {
            case HV_T8152_IPA_RAM:
                return HV_SERR_IPA_RAM;
            case HV_T8152_IPA_MMIO:
                return HV_SERR_IPA_PASSTHROUGH;
            case HV_T8152_IPA_EXCEPTION:
                return HV_SERR_IPA_EXCEPTION;
            default:
                return HV_SERR_IPA_OTHER_HW;
        }
    }
    if (pte) {
        /* A software leaf traps: a C hook, or a software map the carrier performs itself. */
        if (((pte & PTE_TYPE_SW) >> 50) == SPTE_HOOK_TYPE && !*name)
            *name = "carrier hook";
        return HV_SERR_IPA_EMULATED;
    }
    if (policy == HV_T8152_IPA_EXCEPTION)
        return HV_SERR_IPA_REGISTERED;
    if (policy == HV_T8152_IPA_DENIED)
        return HV_SERR_IPA_DENIED;
    return HV_SERR_IPA_UNMAPPED;
}

static const char *walk_name(int r)
{
    switch (r) {
        case HV_SERR_S1_FAULT:
            return "stage-1 fault";
        case HV_SERR_S1_RANGE:
            return "outside both VA halves";
        case HV_SERR_S1_GRANULE:
            return "reserved granule";
        case HV_SERR_S1_UNREADABLE:
            return "a table outside guest RAM";
        default:
            return "?";
    }
}

/* Translate and classify one guest VA; true if it reaches a candidate kind. */
static bool attribute_one(const char *what, u64 va, const struct hv_serror_s1 *s1, bool always,
                          unsigned int *candidates)
{
    u64 ipa;
    int r = hv_serror_s1_walk(s1, va, rd_guest_ram, NULL, &ipa);
    if (r < 0) {
        if (always)
            printf("HV: fatal: attribution: %s VA 0x%lx: no translation (%s)\n", what, va,
                   walk_name(r));
        return false;
    }
    const char *name = NULL;
    int kind = classify(ipa, &name);
    if (kind == HV_SERR_IPA_RAM)
        name = NULL; /* the kind says it */
    bool cand = hv_serror_kind_candidate(kind);
    if (cand)
        (*candidates)++;
    if (always || kind != HV_SERR_IPA_RAM)
        printf("HV: fatal: attribution: %s VA 0x%lx -> IPA 0x%lx: %s%s%s%s\n", what, va, ipa,
               hv_serror_kind_name(kind), name ? " (" : "", name ? name : "", name ? ")" : "");
    if (cand)
        printf("HV: fatal: attribution: CANDIDATE %s -> IPA 0x%lx (%s)\n", what, ipa,
               name ? name : hv_serror_kind_name(kind));
    return cand;
}

void hv_serror_attribute(const u64 *regs, const u64 *sp, u64 esr, u64 elr, u64 spsr)
{
    if (chip_id != T8152 || !regs)
        return;

#ifndef HV_SERROR_HOST_TEST
    u64 rbase, rsize;
    if (!hv_t8152_guest_ram(&rbase, &rsize)) {
        printf("HV: fatal: attribution: guest RAM window not set; skipped\n");
        return;
    }
    rd_guest_ram_base = rbase;
    rd_guest_ram_top = rbase + rsize;
    struct hv_serror_s1 s1 = {
        .sctlr = mrs(SCTLR_EL12),
        .tcr = mrs(SYS_TCR_EL12),
        .ttbr0 = mrs(SYS_TTBR0_EL12),
        .ttbr1 = mrs(SYS_TTBR1_EL12),
    };
#else
    extern struct hv_serror_s1 stub_s1;
    struct hv_serror_s1 s1 = stub_s1;
#endif

    unsigned int candidates = 0;
    u64 iss = esr & GENMASK(24, 0);
    printf("HV: fatal: attribution: architectural signal: %s\n",
           iss == 0       ? "none (ESR ISS 0: no class, no address; FAR/HPFAR are not written)"
           : esr & BIT(24) ? "IMPLEMENTATION DEFINED ISS only (IDS=1); no address"
           : (esr & BIT(9)) ? "ISS EA=1 (external abort: an error response from the fabric); "
                              "no address"
                            : "ISS class only (see the ESR line); no address");
    printf("HV: fatal: attribution: guest stage 1 SCTLR 0x%lx TCR 0x%lx TTBR0 0x%lx TTBR1 0x%lx; "
           "candidates are circumstantial (registers as they are now)\n",
           s1.sctlr, s1.tcr, s1.ttbr0, s1.ttbr1);

    /* The guest PC and the loads/stores just before it (and at it, not yet executed). */
    u64 pc_ipa = 0;
    int r = hv_serror_s1_walk(&s1, elr, rd_guest_ram, NULL, &pc_ipa);
    u64 guest_sp = (spsr & 1) && (spsr & 0xc) ? sp[1] : sp[0];
    if (r < 0) {
        printf("HV: fatal: attribution: guest PC 0x%lx: no translation (%s)\n", elr,
               walk_name(r));
    } else {
        const char *name = NULL;
        int kind = classify(pc_ipa, &name);
        printf("HV: fatal: attribution: guest PC 0x%lx -> IPA 0x%lx: %s\n", elr, pc_ipa,
               hv_serror_kind_name(kind));
        for (int i = -8; i <= 0; i++) {
            u64 pc = elr + (u64)(s64)(i * 4);
            u64 ipa;
            u32 insn;
            /* Same page as ELR, or its own walk when the window crosses a page boundary. */
            if (hv_serror_s1_walk(&s1, pc, rd_guest_ram, NULL, &ipa) < 0 || !read_insn(ipa, &insn))
                continue;
            struct hv_serror_mem m;
            if (!hv_serror_decode_mem(insn, pc, regs, guest_sp, i < 0, &m))
                continue;
            char what[96], base[8];
            if (m.pc_relative)
                snprintf(base, sizeof(base), "pc");
            else if (m.rn == 31)
                snprintf(base, sizeof(base), "sp");
            else
                snprintf(base, sizeof(base), "x%u", m.rn);
            snprintf(what, sizeof(what), "PC%s0x%x insn %08x %s via %s%s", i < 0 ? "-" : "+",
                     (unsigned int)(i < 0 ? -i * 4 : i * 4), insn,
                     m.dir == 1 ? "store" : m.dir == 0 ? "load" : "access", base,
                     i == 0 ? " (at the PC, not executed)" : "");
            attribute_one(what, m.addr, &s1, true, &candidates);
        }
    }

    /* Registers: as guest VAs (non-RAM results only), and as raw IPAs of listed regions. */
    for (int i = 0; i <= 32; i++) {
        u64 v = i < 31 ? regs[i] : sp[i - 31];
        if (v < 0x10000)
            continue;
        char what[16];
        if (i < 31)
            snprintf(what, sizeof(what), "x%d", i);
        else
            snprintf(what, sizeof(what), "SP_EL%d", i - 31);
        attribute_one(what, v, &s1, false, &candidates);
        if (v >> 44)
            continue;
        const char *name = NULL;
        int policy = hv_t8152_describe_ipa(v, &name);
        if (policy == HV_T8152_IPA_MMIO || policy == HV_T8152_IPA_EXCEPTION ||
            policy == HV_T8152_IPA_DENIED)
            printf("HV: fatal: attribution: %s 0x%lx is itself the address of %s (%s)\n", what, v,
                   name ? name : "?",
                   policy == HV_T8152_IPA_MMIO        ? "MMIO passthrough"
                   : policy == HV_T8152_IPA_EXCEPTION ? "identity exception"
                                                      : "denied region");
    }

    if (candidates)
        printf("HV: fatal: attribution: %u candidate(s) reach a passthrough or identity region; "
               "a guest device access is possible (not proven: the SError has no address)\n",
               candidates);
    else
        printf("HV: fatal: attribution: no guest address near the SError reaches a passthrough "
               "or identity region (weighs against a guest device access)\n");
}
