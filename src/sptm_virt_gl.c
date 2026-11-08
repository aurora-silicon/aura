/* SPDX-License-Identifier: MIT */

/*
 * The virtualized SPTM's guarded world. aura holds the real GL2, so SPTM's GENTER lands one level
 * down, in GL1, and everything it does there that needs GL2 traps to aura: accesses to GL2
 * registers, its guarded ERET and its GEXIT to its own normal world (virtual EL2). aura emulates
 * them here against a per-CPU shadow of the guest's GL2 register file.
 *
 * SPTM's static map and physmap VAs below (for the diagnostics' VA walks) are those of 25G83 and
 * 26A428; the build-specific offsets come from the host (run_guest_sptm.py).
 */

#include "sptm_virt.h"
#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "exception.h"
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

/* SPTM's static map of its own image (VA = PA + base) and its physmap of DRAM (25G83). */
#define SPTM_IMAGE_VA_OFFSET 0xfffffd0000000000UL
#define SPTM_PHYSMAP_VA      0xfffffe1000000000UL
#define SPTM_PHYSMAP_VA_END  0xfffffe2000000000UL

/* ---- fault-tolerant reads ------------------------------------------------------------------ */

volatile bool sptm_virt_peek_active_cpu[MAX_CPUS];
volatile bool sptm_virt_peek_faulted_cpu[MAX_CPUS];
static u64 sptm_peek_esr[MAX_CPUS];

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
    sptm_peek_esr[cpu] = in_gl ? mrs(SYS_IMP_APL_ESR_GL1) : mrs(ESR_EL2);
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
    X(4, 15, 11, 1) /* CTRR_A_UPR_EL2 */                                                           \
    X(0, 11, 9, 0)  /* TagOffset_EL2: the MTE tag storage iBoot set */                             \
    X(0, 11, 9, 1)

#define GL2KEY(o0, o1, cn, cm, o2) (((o0) << 14) | ((o1) << 11) | ((cn) << 7) | ((cm) << 3) | (o2))

/* CTRR-A/B per-core CTL reads locked, as SPTM's ctXr_check_all_regions requires. */
#define SPTM_CTRR_LOCKED 0x8000000000000001UL

/*
 * SPTM's expected bounds, {lwr, upr} for CTRR-C, CTRR-D and CTXR-A..D, at +0xa50 of a stash whose
 * base is x8 at its first ACC_CTRR_C_LWR read, and the CTXR CTL values it compares (25G83).
 */
u32 sptm_virt_ctrr_stash_off = 0xa50; /* set by the host, from the image */
static const u64 sptm_ctxr_ctl[4] = {
    0xc000000000aa019aUL,
    0xc0000000009a02aaUL,
    0xc000000000aa019aUL + 0xd0,
    0xc000000000aa019aUL + 0x10f,
};
static u64 sptm_ctrr_stash;

/*
 * The guest's own view of the CTRR/CTXR range registers.
 *
 * Stage 1 locked the hardware ranges over aura's image, so a guest write cannot reach them and is
 * dropped -- but the guest reads its own values back and acts on them. XNU takes its read-only
 * region from CTRR_D_LWR/UPR_EL2 (26A428: rorgn_begin = LWR, rorgn_end = UPR | 0xfff) and rejects
 * a zalloc_ro_mut whose source lies outside it, which panics a second into userspace. So a write
 * lands in this shadow and the matching read answers from it; registers the guest never wrote
 * still read the hardware.
 */
static struct {
    u16 key;
    u64 val;
} sptm_ctrr_shadow[64];

/*
 * The read-only region XNU is to see, {lwr, upr} as CTRR_D_LWR/UPR_EL2 hold it (upr is the last
 * page's base; XNU ORs in 0xfff). The host sets it to the guest kernelcache's read-only span,
 * because the ranges stage 1 locked in the hardware are aura's own image. Zero: answer from the
 * hardware, as before.
 */
u64 sptm_virt_xnu_rorgn[2];
static bool rorgn_logged;

static u64 *sptm_ctrr_shadow_slot(u16 key, bool create)
{
    for (unsigned i = 0; i < ARRAY_SIZE(sptm_ctrr_shadow); i++) {
        if (sptm_ctrr_shadow[i].key == key)
            return &sptm_ctrr_shadow[i].val;
        if (!sptm_ctrr_shadow[i].key) {
            if (!create)
                return NULL;
            sptm_ctrr_shadow[i].key = key;
            return &sptm_ctrr_shadow[i].val;
        }
    }
    return NULL;
}

/*
 * Hand XNU its read-only region in the register it reads it from.
 *
 * XNU's rorgn setup is a plain MRS of CTRR_D_LWR/UPR_EL2 from its own non-guarded EL1, which
 * reaches the hardware without passing through aura's GL2 (only the guest's guarded accesses trap
 * there). Natively iBoot leaves the kernelcache's span in those two registers; stage 1 left aura's
 * own image in the EL1 bank and the EL2 bank at zero, which XNU reads as an empty region. So write
 * the span the host computed into the EL2 bank on the core that is entering XNU, before it runs.
 * The write is guarded: if these are locked, the boot continues as before.
 */
void sptm_virt_ctrr_publish_rorgn(void)
{
    if (!sptm_virt_xnu_rorgn[0])
        return;

    int exc_before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    msr(SYS_IMP_APL_CTRR_D_LWR_EL2, sptm_virt_xnu_rorgn[0]);
    msr(SYS_IMP_APL_CTRR_D_UPR_EL2, sptm_virt_xnu_rorgn[1]);
    sysop("isb");
    u64 lwr = mrs(SYS_IMP_APL_CTRR_D_LWR_EL2), upr = mrs(SYS_IMP_APL_CTRR_D_UPR_EL2);
    exc_guard = GUARD_OFF;

    if (!rorgn_logged) {
        rorgn_logged = true;
        printf("SPTM-virt[ctrr]: XNU's read-only region 0x%lx-0x%lx -> CTRR_D_*_EL2 reads "
               "0x%lx-0x%lx%s\n",
               sptm_virt_xnu_rorgn[0], sptm_virt_xnu_rorgn[1] | 0xfff, lwr, upr | 0xfff,
               exc_count != exc_before ? " (the write faulted)" : "");
    }
}

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
            if (!i && !sptm_ctrr_stash) {
                sptm_ctrr_stash = regs[8];
                printf("SPTM-virt: CTRR stash base 0x%lx (+0x%x)\n", sptm_ctrr_stash,
                       sptm_virt_ctrr_stash_off);
            }
            if (sptm_ctrr_stash &&
                sptm_virt_guest_rd64(sptm_ctrr_stash + sptm_virt_ctrr_stash_off + 8 * i, &v)) {
                if (rt != 31)
                    regs[rt] = (i & 1) ? (v & ~0xfffUL) : v;
                return true;
            }
        }
    }

    u16 key = (u16)GL2KEY(op0, op1, crn, crm, op2);

    switch (key) {
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
    if (!read) {
        static u32 writes;
        u64 wv = (rt == 31) ? 0 : regs[rt];
        u64 *shadow = sptm_ctrr_shadow_slot(key, true);

        if (shadow)
            *shadow = wv;
        if (writes++ < 48)
            printf("SPTM-virt[ctrr]: s3_%u_c%u_c%u_%u <- 0x%lx (the guest's own; the hardware "
                   "keeps stage 1's)\n",
                   op1, crn, crm, op2, wv);
        return true;
    }
    static u32 reads;
    if (reads++ < 48)
        printf("SPTM-virt[ctrr]: s3_%u_c%u_c%u_%u read by %s c%d\n", op1, crn, crm, op2,
               sptm_virt_cpu_in_xnu() ? "XNU" : "SPTM", smp_id());
    u64 *shadow = sptm_ctrr_shadow_slot(key, false);
    if (shadow) {
        rv = *shadow;
    } else if (sptm_virt_xnu_rorgn[0] &&
               (key == GL2KEY(3, 0, 11, 1, 2) || key == GL2KEY(3, 0, 11, 1, 3))) {
        /* XNU's read-only region. Nothing in the guest programs these: on a native boot iBoot
         * locked them over the kernelcache, and stage 1 locked aura's own image instead. */
        rv = sptm_virt_xnu_rorgn[key == GL2KEY(3, 0, 11, 1, 3)];
        if (!rorgn_logged) {
            rorgn_logged = true;
            printf("SPTM-virt[ctrr]: the guest's read-only region is 0x%lx-0x%lx\n",
                   sptm_virt_xnu_rorgn[0], sptm_virt_xnu_rorgn[1] | 0xfff);
        }
    }
    if (rt != 31)
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

void sptm_virt_amx_child(void)
{
    sptm_amx_el2_set_en_el1(sptm_amx_grant[smp_id()] != 0);
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
    static u32 n, world_logs;
    u64 target = mrs(SYS_IMP_APL_ELR_GL12);
    u64 pstate = mrs(SYS_IMP_APL_SPSR_GL12);
    u64 aspsr = mrs(SYS_IMP_APL_ASPSR_GL1);
    unsigned m = pstate & 0x1f, el = (m >> 2) & 3;

    if (n++ < 16)
        printf("SPTM-virt[geret#%u c%d]: -> 0x%lx el%u%c ps=0x%lx\n", n, smp_id(), target, el,
               (pstate & 1) ? 'h' : 't', pstate);
    if (m & 0x10)
        return false; /* AArch32: never from SPTM */
    if (el == 2) {
        /* SPTM's own virtual EL2 runs at physical EL1, leaving cL4 if it was there. */
        pstate -= 4;
        if (sptm_virt_in_cl4()) {
            sptm_virt_cl4_world_exit();
            if (world_logs++ < 8)
                printf("SPTM-virt[world]: cL4 -> host by SPTM's own ERET to GL2\n");
        }
    } else if (el == 1 || el == 3) {
        /*
         * Into cL4, SPTM's guarded child. An ERET to EL0 is left alone deliberately: once cL4 has
         * started a task, SPTM erets straight to it (high VAs in cL4's own map, PSTATE EL0t)
         * having just programmed the EL1 regime for it through the _EL1 names, which land on the
         * live bank. Switching worlds there loads cL4's shadowed bank over exactly that regime,
         * and the task's first instruction fetch then takes a level-0 translation fault.
         */
        if (!sptm_virt_in_cl4())
            sptm_virt_cl4_world_enter(target);
    }

    msr(ELR_EL1, target);
    msr(SPSR_EL1, pstate);
    if (!(aspsr & ASPSR_GUARDED))
        msr(SYS_IMP_APL_ASPSR_GL1, aspsr | ASPSR_GUARDED);
    return true;
}

/* XNU's frame chain at a GEXIT into it, at most every 5 s per core. Nothing XNU sees changes:
 * SPTM preserves x29, and XNU's EL1 regime is the one loaded. */
static void sptm_virt_xnu_bt(u64 *regs, u64 target)
{
    static u64 next[MAX_CPUS];
    static u32 nbt[MAX_CPUS];
    int cpu = smp_id();
    u64 now = mrs(CNTPCT_EL0);

    if (now < next[cpu] || nbt[cpu] >= 2000)
        return;
    next[cpu] = now + 5 * mrs(CNTFRQ_EL0);
    nbt[cpu]++;
    char line[320];
    int len = snprintf(line, sizeof(line), "SPTM-virt[xbt c%d #%u]: ret 0x%lx x0 0x%lx |", cpu,
                       nbt[cpu], target, regs[0]);
    u64 fp = regs[29];
    for (int i = 0; i < 12 && fp && len < (int)sizeof(line) - 24; i++) {
        u64 nfp, lr;
        if (!sptm_virt_el1_va_read64(fp, &nfp) || !sptm_virt_el1_va_read64(fp + 8, &lr))
            break;
        len += snprintf(line + len, sizeof(line) - len, " %lx", lr | 0xffff000000000000UL);
        if (nfp <= fp)
            break;
        fp = nfp;
    }
    printf("%s\n", line);
}

/*
 * An Apple context instruction, 0x00201340 | Rt, that SPTM issues at GL2 on a context switch.
 * At physical GL1 it traps up (EC 0x3f, ISS 0x20000 | Rt); it runs at aura's GL2 and at GL1
 * outside the guest. Issue it here, at the level SPTM would have, with the guest's register.
 */
#define ESR_ISS_CTX_OP 0x20000
#define CTX_OP_INSN    0x00201340

static bool sptm_virt_ctx_op(u64 *regs, u64 elr, u64 spsr, u32 iss)
{
    static bool seen[MAX_CPUS];
    int cpu = smp_id();
    unsigned rt = iss & 0x1f;
    u64 word;

    if (rt == 31 || !sptm_virt_guest_rd64(elr & ~7UL, &word) ||
        ((word >> ((elr & 4) * 8)) & 0xffffffff) != (CTX_OP_INSN | rt))
        return false;

    register u64 arg __asm__("x13") = regs[rt];
    sptm_virt_peek_faulted_cpu[cpu] = false;
    sptm_virt_peek_active_cpu[cpu] = true;
    sysop("isb");
    __asm__ volatile(".inst 0x0020134d" : "+r"(arg)::"memory");
    sysop("isb");
    sptm_virt_peek_active_cpu[cpu] = false;
    if (sptm_virt_peek_faulted_cpu[cpu]) {
        printf("SPTM-virt[ctxop c%d]: x%u=0x%lx faults at GL2 too (esr 0x%lx)\n", cpu, rt, regs[rt],
               sptm_peek_esr[cpu]);
        return false;
    }
    if (!seen[cpu]) {
        seen[cpu] = true;
        printf("SPTM-virt[ctxop c%d]: pc=0x%lx x%u=0x%lx -> 0x%lx at GL2\n", cpu, elr, rt, regs[rt],
               arg);
    }
    regs[rt] = arg;
    /* _gxf_exc_return copies the mirrored ELR_EL1/SPSR_EL1 back to the GL1 bank. */
    msr(SPSR_EL1, spsr);
    msr(ELR_EL1, elr + 4);
    return true;
}

/*
 * SPTM's GEXIT out of its guarded world to its own normal world (virtual EL2), which physical GL1
 * cannot reach, so it traps up. It consumes the guest's GL bank: return to ELR/SPSR_GL12 in the
 * world ASPSR_GL12 names, virtual EL2 at physical EL1, copying the guest's ASPSR exactly.
 */
static bool sptm_virt_guarded_gexit(u64 *regs)
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
        /* XNU's cold entry: SPTM's first exit to its normal world at EL2h. */
        if (el == 2 && (m & 1) && !sptm_virt_xnu_live) {
            sptm_virt_xnu_live = true;
            dc_cvac_range((void *)&sptm_virt_xnu_live, sizeof(sptm_virt_xnu_live));
            printf("SPTM-virt: XNU entered on cpu%d\n", cpu);
        }
        sptm_virt_cpu_enters_xnu(cpu);
        if (sptm_virt_flags & SPTM_VIRT_S2R)
            sptm_virt_s2r_xnu_entry(cpu, target);
        if ((sptm_virt_flags & SPTM_VIRT_XNU_BT) && el == 2 && !sptm_virt_s2r_quiet)
            sptm_virt_xnu_bt(regs, target);
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
/*
 * SPTM's diagnostic record, whose PA the host passes (boot_args +0x480, revision 3): its EL1
 * exception vector writes the exception it took there before it halts. Offsets: +0 magic 'SPTM',
 * +0x20 ESR, +0x28 ELR, +0x30 FAR (the guarded vector stores the GL1 copies instead).
 */
u64 sptm_virt_diag_pa;

static void sptm_virt_park_diag(void)
{
    u64 magic, esr, elr, far;

    if (!sptm_virt_diag_pa || !sptm_virt_peek64(sptm_virt_diag_pa, &magic) ||
        !sptm_virt_peek64(sptm_virt_diag_pa + 0x20, &esr) ||
        !sptm_virt_peek64(sptm_virt_diag_pa + 0x28, &elr) ||
        !sptm_virt_peek64(sptm_virt_diag_pa + 0x30, &far))
        return;
    dc_civac_range((void *)sptm_virt_diag_pa, 0x40);
    sysop("dsb sy");
    magic = read64(sptm_virt_diag_pa);
    esr = read64(sptm_virt_diag_pa + 0x20);
    elr = read64(sptm_virt_diag_pa + 0x28);
    far = read64(sptm_virt_diag_pa + 0x30);
    printf("SPTM-virt[park]: SPTM's record at 0x%lx: magic 0x%lx ESR 0x%lx (EC 0x%lx) ELR 0x%lx "
           "FAR 0x%lx\n",
           sptm_virt_diag_pa, magic, esr, (esr >> 26) & 0x3f, elr, far);
}

static void sptm_virt_park_report(u64 *regs, u64 pc)
{
    static bool done[MAX_CPUS];
    int cpu = smp_id();

    if (done[cpu])
        return;
    done[cpu] = true;
    printf("SPTM-virt[park c%d]: wfe at pc=0x%lx\n", cpu, pc);
    for (int r = 0; r < 31; r += 4)
        printf("SPTM-virt[park]: x%-2d-x%-2d: 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n", r,
               r + 3 < 30 ? r + 3 : 30, regs[r], r + 1 < 31 ? regs[r + 1] : 0,
               r + 2 < 31 ? regs[r + 2] : 0, r + 3 < 31 ? regs[r + 3] : 0);
    sptm_virt_park_diag();
    sptm_virt_park_strings(regs);
}

/* A synchronous exception into aura's GL2 from SPTM (or cL4) at GL1. */
bool sptm_virt_gl_sync(u64 *regs, u64 esr, u64 elr, u64 spsr)
{
    u32 elsp = spsr & SPSR_M_EL;
    u32 ec = (esr & ESR_EC) >> ESR_EC_SHIFT;
    u32 iss = esr & ESR_ISS;

    if (elsp != SPSR_M_EL1H && elsp != SPSR_M_EL1T)
        return false;
    /*
     * The deadline the host asked for, offered from the busiest path there is: SPTM's guarded
     * ERETs and GEXITs arrive all the while the guest runs, so a guest that leaves the GPU and
     * the watchdog alone -- every boot that has not reached WindowServer -- still hands the
     * proxy back instead of running on with aura's console gone.
     */
    if (sptm_virt_gpu_deadline_due()) {
        struct exc_info pctx;
        sptm_virt_fill_ctx(&pctx, regs, true);
        sptm_virt_gpu_deadline(&pctx);
    }
    /* cL4 took an exception to the level above, SPTM's GL2: deliver it there. Its TLB
     * maintenance (op0 1) is performed here instead, as SPTM would. */
    if (sptm_virt_in_cl4() && !(ec == ESR_EC_MSR && ((iss >> 20) & 3) == 1) &&
        sptm_virt_cl4_reflect(regs, esr))
        goto done;

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
            if (iss == ESR_ISS_GEXIT_UP && sptm_virt_guarded_gexit(regs))
                goto done;
            if ((iss & ~0x1fU) == ESR_ISS_CTX_OP && sptm_virt_ctx_op(regs, elr, spsr, iss))
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

/* A WFI or WFE from the guest at virtual EL2 (physical EL1), trapped by TWE or TWI. */
bool sptm_virt_el1_wfx(u64 *regs, u64 esr)
{
    if (sptm_virt_s2r_wfi(regs, esr))
        return true;
    u64 pc = mrs(ELR_EL2);
    sptm_virt_park_report(regs, pc);
    msr(ELR_EL2, pc + 4);
    return true;
}
