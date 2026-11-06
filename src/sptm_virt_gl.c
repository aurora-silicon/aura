/* SPDX-License-Identifier: MIT */

/*
 * The virtualized SPTM's guarded world. aura holds the real GL2, so SPTM's GENTER lands one level
 * down, in GL1, and everything it does there that needs GL2 traps to aura: accesses to GL2
 * registers, its guarded ERET and its GEXIT to its own normal world (virtual EL2). aura emulates
 * them here against a per-CPU shadow of the guest's GL2 register file.
 *
 * Some constants are SPTM build specific (25G83) and named where they are used.
 */

#include "sptm_virt.h"
#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "malloc.h"
#include "memory.h"
#include "nv2.h"
#include "smp.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define ESR_EC_ERET 0b011010
/* A GEXIT from physical GL1 to SPTM's normal world, which GL1 cannot reach: EC 0x3f, ISS 0x22. */
#define ESR_ISS_GEXIT_UP 0x22

/* Page offset of SPTM's GEXIT into XNU's cold entry (25G83). */
#define SPTM_XNU_ENTRY_GEXIT_PO 0x2d8

/* SPTM's static map of its own image (VA = PA + base) and its physmap of DRAM (25G83). */
#define SPTM_IMAGE_VA_OFFSET 0xfffffd0000000000UL
#define SPTM_PHYSMAP_VA      0xfffffe1000000000UL
#define SPTM_PHYSMAP_VA_END  0xfffffe2000000000UL

/* ---- fault-tolerant reads ------------------------------------------------------------------ */

volatile bool sptm_virt_peek_active_cpu[MAX_CPUS];
volatile bool sptm_virt_peek_faulted_cpu[MAX_CPUS];

/*
 * exc_sync: while a probe is in flight the only code running is its one load or mrs, so a fault is
 * that instruction. Skip it at the level it faulted at: a GL2 fault returns through the mirrored
 * ELR_EL1, a plain EL2 fault through ELR_EL2.
 */
bool sptm_virt_peek_fault(bool in_gl)
{
    int cpu = smp_id();

    if (cpu < 0 || cpu >= MAX_CPUS || !sptm_virt_peek_active_cpu[cpu])
        return false;
    sptm_virt_peek_active_cpu[cpu] = false;
    sptm_virt_peek_faulted_cpu[cpu] = true;
    if (in_gl)
        msr(ELR_EL1, mrs(ELR_EL1) + 4);
    else
        msr(ELR_EL2, mrs(ELR_EL2) + 4);
    sysop("isb");
    return true;
}

bool sptm_virt_peek64(u64 phys, u64 *out)
{
    int cpu = smp_id();

    sptm_virt_peek_active_cpu[cpu] = true;
    sptm_virt_peek_faulted_cpu[cpu] = false;
    sysop("isb");
    u64 v = read64(phys);
    sysop("isb");
    sptm_virt_peek_active_cpu[cpu] = false;
    if (sptm_virt_peek_faulted_cpu[cpu])
        return false;
    *out = v;
    return true;
}

static bool sptm_virt_is_ram(u64 pa)
{
    u64 base = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    return pa >= base && pa < base + mem_size_actual;
}

/* A word of SPTM's memory by SPTM virtual address (its image map or its physmap). */
static bool sptm_virt_guest_rd64(u64 va, u64 *out)
{
    u64 pa;

    if (va >= SPTM_PHYSMAP_VA && va < SPTM_PHYSMAP_VA_END)
        pa = va - SPTM_PHYSMAP_VA + ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
    else if (va >= SPTM_IMAGE_VA_OFFSET)
        pa = va - SPTM_IMAGE_VA_OFFSET;
    else
        return false;
    if (!sptm_virt_is_ram(pa))
        return false;
    return sptm_virt_peek64(pa, out);
}

/* A word at a PA aura may not map: use the mapping if there is one, else map the page read-only
 * for the read. aura's EL2 tables are shared by every core, so the map/unmap is serialized. */
static spinlock_t sptm_pa_read_lock = SPINLOCK_INIT;

static bool sptm_virt_pa_read64(u64 pa, u64 *out)
{
    if (sptm_virt_peek64(pa, out))
        return true;
    spin_lock(&sptm_pa_read_lock);
    u64 page = pa & ~0x3fffUL;
    mmu_add_mapping(page, page, 0x4000, MAIR_IDX_NORMAL, PERM_RO);
    *out = read64(pa);
    mmu_rm_mapping(page, 0x4000);
    sysop("dsb ish");
    sysop("tlbi alle2is");
    sysop("dsb ish");
    sysop("isb");
    spin_unlock(&sptm_pa_read_lock);
    return true;
}

/* A word at a virtual address of whatever EL1 regime is loaded (XNU's, or cL4's). */
bool sptm_virt_el1_va_read64(u64 va, u64 *out)
{
    u64 par0 = mrs(PAR_EL1);
    __asm__ volatile("at s1e1r, %0" ::"r"(va) : "memory");
    sysop("isb");
    u64 par = mrs(PAR_EL1);
    msr(PAR_EL1, par0);
    if (par & 1)
        return false;
    return sptm_virt_pa_read64((par & GENMASK(47, 12)) | (va & 0xfff), out);
}

/* ---- the guest's GL2 register file -------------------------------------------------------- */

#define SPTM_GL2_SLOTS 512u
struct sptm_gl2_slot {
    u16 key;
    u64 val;
};
/* [MAX_CPUS], on the heap: .bss must end below the DCP log buffers after stage 1. */
static struct sptm_gl2_slot (*sptm_gl2_shadow)[SPTM_GL2_SLOTS];

int sptm_virt_gl_alloc(void)
{
    if (sptm_gl2_shadow)
        return 0;
    size_t size = sizeof(*sptm_gl2_shadow) * MAX_CPUS;
    sptm_gl2_shadow = memalign(SZ_16K, size);
    if (!sptm_gl2_shadow) {
        printf("SPTM-virt: cannot allocate the GL2 register shadow\n");
        return -1;
    }
    memset(sptm_gl2_shadow, 0, size);
    return 0;
}

/* The shadow of one register (16-bit sysreg encoding) of a core, created on first use. */
u64 *sptm_virt_gl2_slot(int cpu, u16 key)
{
    static u64 no_table;
    if (!sptm_gl2_shadow)
        return &no_table;
    u32 h = (key * 2654435761u) & (SPTM_GL2_SLOTS - 1);
    for (u32 i = 0; i < SPTM_GL2_SLOTS; i++) {
        struct sptm_gl2_slot *s = &sptm_gl2_shadow[cpu][(h + i) & (SPTM_GL2_SLOTS - 1)];
        if (s->key == key)
            return &s->val;
        if (s->key == 0) {
            s->key = key;
            s->val = 0;
            return &s->val;
        }
    }
    return &sptm_gl2_shadow[cpu][h].val; /* full: cannot happen with the registers SPTM uses */
}

/* ---- CTRR and CTXR -------------------------------------------------------------------------- */

/*
 * The CTRR/CTXR range registers are GL2-only, so SPTM's accesses trap here. Reads see the real
 * value and writes are dropped, so SPTM cannot reprogram or lock the ranges stage 1 set. Except
 * for what SPTM's ctrr_ctxr_check_region compares: the hardware ranges stage 1 locked are aura's,
 * not the ones SPTM expects, so those reads answer with SPTM's own expected values.
 */
#define SPTM_CTRR_PASSTHROUGH(X)                                                                   \
    X(0, 11, 0, 0)  /* CTRR_C_LWR_EL1 */                                                           \
    X(0, 11, 0, 1)  /* CTRR_C_UPR_EL1 */                                                           \
    X(0, 11, 0, 2)  /* CTRR_D_LWR_EL1 */                                                           \
    X(0, 11, 0, 3)  /* CTRR_D_UPR_EL1 */                                                           \
    X(0, 11, 0, 4)  /* CTRR_C_LWR_EL12 */                                                          \
    X(0, 11, 0, 5)  /* CTRR_C_UPR_EL12 */                                                          \
    X(0, 11, 0, 6)  /* CTRR_D_LWR_EL12 */                                                          \
    X(0, 11, 0, 7)  /* CTRR_D_UPR_EL12 */                                                          \
    X(0, 11, 1, 0)  /* CTRR_C_LWR_EL2 */                                                           \
    X(0, 11, 1, 1)  /* CTRR_C_UPR_EL2 */                                                           \
    X(0, 11, 1, 2)  /* CTRR_D_LWR_EL2 */                                                           \
    X(0, 11, 1, 3)  /* CTRR_D_UPR_EL2 */                                                           \
    X(0, 11, 1, 4)  /* CTRR_C_CTL_EL1 */                                                           \
    X(0, 11, 1, 5)  /* CTRR_D_CTL_EL1 */                                                           \
    X(0, 11, 1, 6)  /* CTRR_C_CTL_EL12 */                                                          \
    X(0, 11, 1, 7)  /* CTRR_D_CTL_EL12 */                                                          \
    X(0, 11, 2, 2)  /* CTXR_A_LWR_EL1 */                                                           \
    X(0, 11, 2, 3)  /* CTXR_A_UPR_EL1 */                                                           \
    X(0, 11, 2, 4)  /* CTXR_B_LWR_EL1 */                                                           \
    X(0, 11, 2, 5)  /* CTXR_B_UPR_EL1 */                                                           \
    X(0, 11, 2, 6)  /* CTXR_C_LWR_EL1 */                                                           \
    X(0, 11, 2, 7)  /* CTXR_C_UPR_EL1 */                                                           \
    X(0, 11, 3, 0)  /* CTXR_D_LWR_EL1 */                                                           \
    X(0, 11, 3, 1)  /* CTXR_D_UPR_EL1 */                                                           \
    X(0, 11, 3, 2)  /* CTXR_A_LWR_EL12 */                                                          \
    X(0, 11, 3, 3)  /* CTXR_A_UPR_EL12 */                                                          \
    X(0, 11, 3, 4)  /* CTXR_B_LWR_EL12 */                                                          \
    X(0, 11, 3, 5)  /* CTXR_B_UPR_EL12 */                                                          \
    X(0, 11, 3, 6)  /* CTXR_C_LWR_EL12 */                                                          \
    X(0, 11, 3, 7)  /* CTXR_C_UPR_EL12 */                                                          \
    X(0, 11, 4, 0)  /* CTXR_D_LWR_EL12 */                                                          \
    X(0, 11, 4, 1)  /* CTXR_D_UPR_EL12 */                                                          \
    X(0, 11, 5, 2)  /* CTXR_A_CTL_EL1 */                                                           \
    X(0, 11, 5, 3)  /* CTXR_B_CTL_EL1 */                                                           \
    X(0, 11, 5, 4)  /* CTXR_C_CTL_EL1 */                                                           \
    X(0, 11, 5, 5)  /* CTXR_D_CTL_EL1 */                                                           \
    X(0, 11, 5, 6)  /* CTXR_A_CTL_EL12 */                                                          \
    X(0, 11, 5, 7)  /* CTXR_B_CTL_EL12 */                                                          \
    X(0, 11, 6, 0)  /* CTXR_C_CTL_EL12 */                                                          \
    X(0, 11, 6, 1)  /* CTXR_D_CTL_EL12 */                                                          \
    X(0, 11, 6, 6)  /* ACC_CTRR_C_LWR_EL2 */                                                       \
    X(0, 11, 6, 7)  /* ACC_CTRR_C_UPR_EL2 */                                                       \
    X(0, 11, 7, 0)  /* ACC_CTRR_D_LWR_EL2 */                                                       \
    X(0, 11, 7, 1)  /* ACC_CTRR_D_UPR_EL2 */                                                       \
    X(0, 11, 7, 2)  /* ACC_CTXR_A_LWR_EL2 */                                                       \
    X(0, 11, 7, 3)  /* ACC_CTXR_A_UPR_EL2 */                                                       \
    X(0, 11, 7, 4)  /* ACC_CTXR_B_LWR_EL2 */                                                       \
    X(0, 11, 7, 5)  /* ACC_CTXR_B_UPR_EL2 */                                                       \
    X(0, 11, 7, 6)  /* ACC_CTXR_C_LWR_EL2 */                                                       \
    X(0, 11, 7, 7)  /* ACC_CTXR_C_UPR_EL2 */                                                       \
    X(0, 11, 8, 0)  /* ACC_CTXR_D_LWR_EL2 */                                                       \
    X(0, 11, 8, 1)  /* ACC_CTXR_D_UPR_EL2 */                                                       \
    X(0, 11, 8, 2)  /* ACC_CTRR_C_CTL_EL2 */                                                       \
    X(0, 11, 8, 3)  /* ACC_CTRR_D_CTL_EL2 */                                                       \
    X(0, 11, 8, 4)  /* ACC_CTXR_A_CTL_EL2 */                                                       \
    X(0, 11, 8, 5)  /* ACC_CTXR_B_CTL_EL2 */                                                       \
    X(0, 11, 8, 6)  /* ACC_CTXR_C_CTL_EL2 */                                                       \
    X(0, 11, 8, 7)  /* ACC_CTXR_D_CTL_EL2 */                                                       \
    X(4, 15, 2, 2)  /* CTRR_B_CTL_EL1 */                                                           \
    X(4, 15, 2, 5)  /* CTRR_A_CTL_EL1 */                                                           \
    X(4, 15, 11, 4) /* ACC_CTRR_A_CTL_EL2 */                                                       \
    X(4, 15, 11, 5) /* ACC_CTRR_B_CTL_EL2 */                                                       \
    X(4, 15, 2, 3)  /* CTRR_A_LWR_EL1 */                                                           \
    X(4, 15, 2, 4)  /* CTRR_A_UPR_EL1 */                                                           \
    X(4, 15, 9, 0)  /* CTRR_A_LWR_EL12 */                                                          \
    X(4, 15, 9, 1)  /* CTRR_A_UPR_EL12 */                                                          \
    X(4, 15, 9, 2)  /* CTRR_B_LWR_EL12 */                                                          \
    X(4, 15, 9, 3)  /* CTRR_B_UPR_EL12 */                                                          \
    X(4, 15, 9, 4)  /* CTRR_CTL_EL12 */                                                            \
    X(4, 15, 9, 5)  /* CTRR_LOCK_EL12 */                                                           \
    X(4, 15, 11, 0) /* CTRR_A_LWR_EL2 */                                                           \
    X(4, 15, 11, 1) /* CTRR_A_UPR_EL2 */

#define GL2KEY(o0, o1, cn, cm, o2) (((o0) << 14) | ((o1) << 11) | ((cn) << 7) | ((cm) << 3) | (o2))

/* CTRR-A/B per-core CTL reads locked, as SPTM's ctXr_check_all_regions requires. */
#define SPTM_CTRR_LOCKED 0x8000000000000001UL

/*
 * SPTM's expected bounds, {lwr, upr} for CTRR-C, CTRR-D and CTXR-A..D, at +0xa50 of a stash whose
 * base is x8 at its first ACC_CTRR_C_LWR read, and the CTXR CTL values it compares (25G83).
 */
#define SPTM_CTRR_STASH_OFF 0xa50
static const u64 sptm_ctxr_ctl[4] = {
    0xc000000000aa019aUL,
    0xc0000000009a02aaUL,
    0xc000000000aa019aUL + 0xd0,
    0xc000000000aa019aUL + 0x10f,
};
static u64 sptm_ctrr_stash;

static bool sptm_virt_ctrr_rw(u64 *regs, u64 esr)
{
    unsigned op0 = (esr >> 20) & 3, op1 = (esr >> 14) & 7, crn = (esr >> 10) & 15;
    unsigned crm = (esr >> 1) & 15, op2 = (esr >> 17) & 7, rt = (esr >> 5) & 31;
    bool read = esr & 1;
    u64 rv = 0;

    if (read && op0 == 3 && op1 == 4 && crn == 15 &&
        ((crm == 2 && (op2 == 5 || op2 == 2)) || (crm == 11 && (op2 == 4 || op2 == 5)))) {
        if (rt != 31)
            regs[rt] = SPTM_CTRR_LOCKED;
        return true;
    }

    if (read && op0 == 3 && op1 == 0 && crn == 11) {
        unsigned enc = (crm << 3) | op2;
        const unsigned first = (6 << 3) | 6, last = (8 << 3) | 1, ctl = (8 << 3) | 4;

        if (enc >= ctl && enc < ctl + 4) {
            if (rt != 31)
                regs[rt] = sptm_ctxr_ctl[enc - ctl];
            return true;
        }
        if (enc >= first && enc <= last) {
            unsigned i = enc - first;
            u64 v;
            if (!i && !sptm_ctrr_stash)
                sptm_ctrr_stash = regs[8];
            if (sptm_ctrr_stash &&
                sptm_virt_guest_rd64(sptm_ctrr_stash + SPTM_CTRR_STASH_OFF + 8 * i, &v)) {
                if (rt != 31)
                    regs[rt] = (i & 1) ? (v & ~0xfffUL) : v;
                return true;
            }
        }
    }

    switch (GL2KEY(op0, op1, crn, crm, op2)) {
#define X(o1, cn, cm, o2)                                                                          \
    case GL2KEY(3, o1, cn, cm, o2):                                                                \
        if (read)                                                                                  \
            rv = mrs(sys_reg(3, o1, cn, cm, o2));                                                  \
        break;
        SPTM_CTRR_PASSTHROUGH(X)
#undef X
        default:
            return false;
    }
    if (read && rt != 31)
        regs[rt] = rv;
    return true;
}

/* ---- AMX ------------------------------------------------------------------------------------ */

/*
 * On real hardware SPTM's and XNU's AMX_CTL_EL2 writes reach the one EL2 register, where AMX is
 * always allowed, and its bit 62 (EN_EL1) grants AMX to the EL1 world below. Here that level is
 * physical EL1, where AMX works only if the real EL2 sets EN_EL1. So while the host runs aura keeps
 * EN_EL1 set and the guest's own value lives in the EL1 copy (the _EL12 alias); while an EL1 child
 * runs, EN_EL1 is the guest's grant.
 */
static u64 sptm_amx_grant[MAX_CPUS];

static void sptm_amx_el2_set_en_el1(bool on)
{
    u64 v = mrs(SYS_IMP_APL_AMX_CTL_EL2);
    u64 nv = on ? (v | AMX_CTL_EN_EL1) : (v & ~AMX_CTL_EN_EL1);
    if (nv != v) {
        msr(SYS_IMP_APL_AMX_CTL_EL2, nv);
        sysop("isb");
    }
}

void sptm_virt_amx_host(void)
{
    sptm_amx_el2_set_en_el1(true);
}

void sptm_virt_amx_ctl_el2_write(u64 v)
{
    sptm_amx_grant[smp_id()] = v & AMX_CTL_EN_EL1;
    msr(SYS_IMP_APL_AMX_CTL_EL12, v & ~AMX_CTL_EN_EL1);
    sptm_virt_amx_host();
}

u64 sptm_virt_amx_ctl_el2_read(void)
{
    return mrs(SYS_IMP_APL_AMX_CTL_EL12) | sptm_amx_grant[smp_id()];
}

/* ---- trapped GL2 accesses ------------------------------------------------------------------- */

static bool sptm_virt_gl2_sysreg(u64 *regs, u64 esr)
{
    unsigned op0 = (esr >> 20) & 3, op1 = (esr >> 14) & 7, crn = (esr >> 10) & 15;
    unsigned crm = (esr >> 1) & 15, op2 = (esr >> 17) & 7, rt = (esr >> 5) & 31;
    bool read = esr & 1;
    bool amx = op0 == 3 && op1 == 4 && crn == 15 && crm == 4 && op2 == 7; /* AMX_CTL_EL2 */

    /* A system instruction. SPTM runs with no stage 2 of its own, so its TLBI (and the FEAT_XS
     * nXS forms, CRn 9) is a broadcast EL1&0 invalidate; cache and AT ops need nothing here. */
    if (op0 == 1) {
        if (crn == 8 || crn == 9) {
            sysop("dsb ishst");
            __asm__ volatile("tlbi vmalle1is" ::: "memory");
            sysop("dsb ish");
            sysop("isb");
        }
        return true;
    }

    if (sptm_virt_ctrr_rw(regs, esr))
        return true;

    /* Anything else: the guest's GL2 register file, so reads see its earlier writes. */
    u16 key = (u16)GL2KEY(op0, op1, crn, crm, op2);
    if (!key)
        key = 0xffff;
    u64 *slot = sptm_virt_gl2_slot(smp_id(), key);
    u64 wv = (rt == 31) ? 0 : regs[rt];
    if (read && amx)
        *slot = sptm_virt_amx_ctl_el2_read();
    if (read) {
        if (rt != 31)
            regs[rt] = *slot;
    } else {
        *slot = wv;
        /* AMX_CTL_EL2 is policy over the levels below and aura is the real EL2: apply it. */
        if (amx)
            sptm_virt_amx_ctl_el2_write(wv);
    }
    return true;
}

/*
 * SPTM's guarded ERET (EC 0x1a from GL1h). Its stubs write the target into the GL1 exception bank
 * and ERET; the trap entry into aura's GL2 then fills GL2's own bank, which is what the c10 _GL1
 * encodings address from here, while the guest's real GL1 bank is reachable as _GL12 (c9). So the
 * target and PSTATE come from ELR_GL12/SPSR_GL12, for any stub. A guarded ERET stays guarded: keep
 * the trap entry's ASPSR, with the guarded bit set. _gxf_exc_return copies ELR/SPSR_EL1 into GL2's
 * bank and gexits.
 */
static bool sptm_virt_guarded_eret(void)
{
    static u32 n;
    u64 target = mrs(SYS_IMP_APL_ELR_GL12);
    u64 pstate = mrs(SYS_IMP_APL_SPSR_GL12);
    u64 aspsr = mrs(SYS_IMP_APL_ASPSR_GL1);
    unsigned m = pstate & 0x1f, el = (m >> 2) & 3;

    if (n++ < 16)
        printf("SPTM-virt[geret#%u c%d]: -> 0x%lx el%u%c ps=0x%lx\n", n, smp_id(), target, el,
               (pstate & 1) ? 'h' : 't', pstate);
    if (m & 0x10)
        return false; /* AArch32: never from SPTM */
    if (el == 2)
        pstate -= 4; /* SPTM's own virtual EL2 runs at physical EL1 */

    msr(ELR_EL1, target);
    msr(SPSR_EL1, pstate);
    if (!(aspsr & ASPSR_GUARDED))
        msr(SYS_IMP_APL_ASPSR_GL1, aspsr | ASPSR_GUARDED);
    return true;
}

/*
 * SPTM's GEXIT out of its guarded world to its own normal world (virtual EL2), which physical GL1
 * cannot reach, so it traps up. It consumes the guest's GL bank: return to ELR/SPSR_GL12 in the
 * world ASPSR_GL12 names, virtual EL2 at physical EL1, copying the guest's ASPSR exactly.
 */
static bool sptm_virt_guarded_gexit(void)
{
    static u32 n;
    u64 pc = mrs(ELR_EL1); /* mirrored from GL2's bank by the trap entry */
    u64 target = mrs(SYS_IMP_APL_ELR_GL12);
    u64 pstate = mrs(SYS_IMP_APL_SPSR_GL12);
    u64 gaspsr = mrs(SYS_IMP_APL_ASPSR_GL12);
    unsigned m = pstate & 0x1f, el = (m >> 2) & 3;
    bool guarded = gaspsr & ASPSR_GUARDED;

    if (n++ < 16)
        printf("SPTM-virt[gexit#%u c%d]: pc=0x%lx -> 0x%lx el%u%c ps=0x%lx (%s)\n", n, smp_id(), pc,
               target, el, (m & 1) ? 'h' : 't', pstate, guarded ? "guarded" : "normal");
    if (m & 0x10 || el == 3)
        return false;
    if (el == 2) {
        pstate -= 4;
    } else if (!guarded) {
        /* A normal-world EL1/EL0 target would be SPTM entering XNU as its child: not emulated. */
        printf("SPTM-virt[gexit]: normal-world EL%u child target not emulated\n", el);
        return false;
    }

    if (!guarded) {
        int cpu = smp_id();
        if ((pc & 0xfff) == SPTM_XNU_ENTRY_GEXIT_PO && !sptm_virt_xnu_live) {
            sptm_virt_xnu_live = true;
            dc_cvac_range((void *)&sptm_virt_xnu_live, sizeof(sptm_virt_xnu_live));
            printf("SPTM-virt: XNU entered on cpu%d\n", cpu);
        }
        sptm_virt_cpu_enters_xnu(cpu);
    }

    msr(ELR_EL1, target);
    msr(SPSR_EL1, pstate);
    msr(SYS_IMP_APL_ASPSR_GL1, gaspsr);
    return true;
}

/* ---- SPTM's park ---------------------------------------------------------------------------- */

/* Any register that points at text in SPTM's memory: its panic message is among them. */
static void sptm_virt_park_strings(u64 *regs)
{
    for (int r = 0; r < 30; r++) {
        u64 p = regs[r] - SPTM_IMAGE_VA_OFFSET, probe;
        if (regs[r] < SPTM_IMAGE_VA_OFFSET || !sptm_virt_is_ram(p) || !sptm_virt_peek64(p, &probe))
            continue;
        dc_civac_range((void *)p, 0x100);
        sysop("dsb sy");
        const char *s = (const char *)p;
        u64 maxlen = 0x1000 - (p & 0xfff); /* stay within the page the probe confirmed */
        int printable = 0, lim = maxlen < 64 ? (int)maxlen : 64;
        for (int i = 0; i < lim; i++)
            if (s[i] >= 0x20 && s[i] < 0x7f)
                printable++;
        if (printable < 24)
            continue;
        printf("SPTM-virt[park]: str@x%d(0x%lx)=\"", r, regs[r]);
        for (int i = 0; i < 200 && (u64)i < maxlen && s[i]; i++)
            printf("%c", (s[i] >= 0x20 && s[i] < 0x7f) ? s[i] : '.');
        printf("\"\n");
    }
}

/*
 * A WFE before this core's XNU entry, trapped by TWE: SPTM's panic halt is `wfe; b .`, which
 * under aura would wait forever. Report it once per core and step over it; a WFE may complete
 * early anyway, so SPTM's own waits are unaffected.
 */
static void sptm_virt_park_report(u64 *regs, u64 pc)
{
    static bool done[MAX_CPUS];
    int cpu = smp_id();

    if (done[cpu])
        return;
    done[cpu] = true;
    printf("SPTM-virt[park c%d]: wfe at pc=0x%lx x0=0x%lx x1=0x%lx x2=0x%lx x30=0x%lx\n", cpu, pc,
           regs[0], regs[1], regs[2], regs[30]);
    sptm_virt_park_strings(regs);
}

/* A synchronous exception into aura's GL2 from SPTM at GL1. */
bool sptm_virt_gl_sync(u64 *regs, u64 esr, u64 elr, u64 spsr)
{
    u32 elsp = spsr & SPSR_M_EL;
    u32 ec = (esr & ESR_EC) >> ESR_EC_SHIFT;
    u32 iss = esr & ESR_ISS;

    if (elsp != SPSR_M_EL1H && elsp != SPSR_M_EL1T)
        return false;
    switch (ec) {
        case ESR_EC_WFI:
            sptm_virt_park_report(regs, elr);
            /* The report's fault-tolerant reads rewrite the mirrored return state. */
            msr(SPSR_EL1, spsr);
            msr(ELR_EL1, elr + 4);
            goto done;
        case ESR_EC_ERET:
            if (sptm_virt_guarded_eret())
                goto done;
            break;
        case ESR_EC_IMPDEF:
            if (iss == ESR_ISS_GEXIT_UP && sptm_virt_guarded_gexit())
                goto done;
            break;
        case ESR_EC_DABORT_LOWER:
            if (sptm_virt_xnu_live && sptm_virt_s2_dabort_gl(regs))
                goto done;
            break;
        case ESR_EC_MSR:
            if (sptm_virt_gl2_sysreg(regs, esr)) {
                /* _gxf_exc_return copies ELR_EL1 back to ELR_GL1. */
                msr(ELR_EL1, elr + 4);
                sysop("isb");
                sysop("dsb sy");
                return true;
            }
            break;
    }
    return false;

done:
    sysop("isb");
    return true;
}

/* A WFE from the guest at virtual EL2 (physical EL1), trapped by TWE. */
bool sptm_virt_el1_wfx(u64 *regs, u64 esr)
{
    UNUSED(esr);
    u64 pc = mrs(ELR_EL2);
    sptm_virt_park_report(regs, pc);
    msr(ELR_EL2, pc + 4);
    return true;
}
