/* SPDX-License-Identifier: MIT */

#include "nv2.h"
#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "malloc.h"
#include "memory.h"
#include "monitor.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

/*
 * Ported from the team's nv2_*.inc (SPTM) and adapted to the no-SPTM monitor.
 * With no SPTM the guest (L1) runs with no stage-2, so its KVM builds a real
 * IPA->PA stage-2; the monitor installs the guest's VTTBR/VTCR directly for L2
 * (no shadow stage-2). This file is built but dormant (nv2_active == false)
 * until the dispatch is complete.
 */

bool nv2_active = false;

#define ESR_ERET 0x6a000000UL /* EC 0x1a (ERET/ERETAA/ERETAB) with IL=1 */

/* SPSR mode fields. */
#define SPSR_M_MASK 0x1fUL
#define SPSR_M_EL1  0x04UL /* M[4:1] == 0b0010 -> EL1 (the vEL2<->vEL1 boundary) */

/*
 * Fast ERET gate (the one constant NV2 cost): when the guest hypervisor erets
 * into its *own* userspace (VHE host EL0), do the eret here instead of taking
 * the slow path. SPTM's "native SK owner" check is dropped: there is no secure
 * kernel under this monitor. The caller has confirmed ec == 0x1a.
 *
 * Returns true and leaves SPSR_EL2/ELR_EL2 set for the guest's EL0 entry; the
 * exception-return path then ERETs with the guest's saved GPRs.
 */
static bool nv2_eret_into_guest_el0(void)
{
    u64 hcr = mrs(HCR_EL2);
    if (!(hcr & HCR_NV) || (hcr & HCR_NV1) || !(hcr & HCR_NV2) || !(hcr & HCR_E2H))
        return false;
    /* The trapped ERET's own SPSR targets the guest's EL1 (vEL2->vEL1). */
    if ((mrs(SPSR_EL2) & 0x1e) != SPSR_M_EL1)
        return false;
    /* The guest's target SPSR must return to EL0t for the fast path. */
    u64 target_spsr = mrs(SPSR_EL12);
    if (target_spsr & SPSR_M_MASK)
        return false;
    /* The guest hypervisor's HCR (from its VNCR page) must say it is a VHE host
     * entering host userspace (E2H && TGE); otherwise this is an L2 entry. */
    u64 vncr_pa;
    __asm__ volatile(".arch armv9.4-a\nmrs %0, VNCR_EL2" : "=r"(vncr_pa));
    u64 guest_hcr = ((const volatile u64 *)vncr_pa)[0x78 / 8];
    if (!(guest_hcr & HCR_E2H) || !(guest_hcr & HCR_TGE))
        return false;

    msr(SPSR_EL2, target_spsr);
    msr(ELR_EL2, mrs(ELR_EL12));
    return true;
}

/* ISS key for a trapped MSR/MRS, Rt and direction masked out. */
#define NV2_KEY(op0, op1, crn, crm, op2)                                                           \
    (((u64)(op0) << 20) | ((u64)(op2) << 17) | ((u64)(op1) << 14) | ((u64)(crn) << 10) |           \
     ((u64)(crm) << 1))

/* Per-CPU shadow for trapped EL2 registers the monitor holds for the guest.
 * Named slots for the ones the guest reads back meaningfully; a small logged
 * table for anything else, so a vEL2 boot reveals what Linux actually touches
 * rather than rebooting on it. */
#define NV2_SHADOW 48
struct nv2_state {
    u64 cnthctl, mdcr, cptr, hacr, sre;
    struct {
        u64 key, val;
    } shadow[NV2_SHADOW];
    int shadow_n;
};
static struct nv2_state nv2_state[MAX_CPUS];

static struct nv2_state *nv2_state_this(void)
{
    u64 mpidr = mrs(MPIDR_EL1) & 0xffffff;
    int idx = boot_cpu_idx;
    for (int i = 0; i < MAX_CPUS; i++)
        if (smp_is_alive(i) && (smp_get_mpidr(i) & 0xffffff) == mpidr) {
            idx = i;
            break;
        }
    return &nv2_state[idx];
}

static u64 *nv2_shadow_slot(struct nv2_state *s, u64 key)
{
    for (int i = 0; i < s->shadow_n; i++)
        if (s->shadow[i].key == key)
            return &s->shadow[i].val;
    if (s->shadow_n < NV2_SHADOW) {
        s->shadow[s->shadow_n].key = key;
        printf("NV2: shadow EL2 reg key %#lx (op0=%lu op1=%lu c%lu c%lu op2=%lu)\n", key,
               (key >> 20) & 3, (key >> 14) & 7, (key >> 10) & 0xf, (key >> 1) & 0xf,
               (key >> 17) & 7);
        return &s->shadow[s->shadow_n++].val;
    }
    return NULL;
}

/*
 * Emulate a trapped EL2-register access (EC 0x18) from a virtual-EL2 guest.
 * VHE-aliased registers are applied to their real _EL12 copies so they take
 * effect; the rest are shadowed. First cut: no timer/GIC/AT emulation yet
 * (added as the vEL2 Linux bring-up needs them). Returns true if handled.
 */
static bool nv2_sysreg(u64 *regs, u64 esr)
{
    u64 key = esr & ESR_ISS & ~(ESR_ISS_MSR_Rt | ESR_ISS_MSR_DIR);
    unsigned rt = (esr >> 5) & 31;
    bool read = esr & 1;
    u64 value = rt == 31 ? 0 : regs[rt];
    struct nv2_state *s = nv2_state_this();
    u64 *slot = NULL;

    switch (key) {
        case NV2_KEY(3, 4, 1, 0, 0): /* SCTLR_EL2 -> SCTLR_EL12 */
            if (read)
                value = mrs(SCTLR_EL12);
            else {
                msr(SCTLR_EL12, value);
                sysop("isb");
            }
            break;
        case NV2_KEY(3, 4, 12, 0, 0): /* VBAR_EL2 -> VBAR_EL12 */
            if (read)
                value = mrs(VBAR_EL12);
            else {
                msr(VBAR_EL12, value);
                sysop("isb");
            }
            break;
        case NV2_KEY(3, 4, 1, 2, 6): /* SMCR_EL2 -> SMCR_EL12 */
            if (read)
                value = mrs(sys_reg(3, 5, 1, 2, 6));
            else {
                msr(sys_reg(3, 5, 1, 2, 6), value);
                sysop("isb");
            }
            break;
        case NV2_KEY(3, 4, 14, 1, 0):
            slot = &s->cnthctl;
            break;
        case NV2_KEY(3, 4, 1, 1, 1):
            slot = &s->mdcr;
            break;
        case NV2_KEY(3, 0, 1, 0, 2): /* VHE CPACR -> CPTR */
        case NV2_KEY(3, 4, 1, 1, 2):
            slot = &s->cptr;
            break;
        case NV2_KEY(3, 4, 1, 1, 7):
            slot = &s->hacr;
            break;
        case NV2_KEY(3, 4, 12, 9, 5): /* ICC_SRE_EL2 */
            slot = &s->sre;
            break;
        default:
            slot = nv2_shadow_slot(s, key);
            if (!slot)
                return false;
            break;
    }
    if (slot) {
        if (read)
            value = *slot;
        else
            *slot = value;
    }
    if (read && rt != 31)
        regs[rt] = value;
    msr(ELR_EL2, mrs(ELR_EL2) + 4);
    return true;
}

static bool nv2_sync_slow(u64 *regs, u32 ec)
{
    if (ec == 0x18)
        return nv2_sysreg(regs, mrs(ESR_EL2));
    /* TODO: TLBI/AT forwarding, stage-2 install for L2 entry, fault injection. */
    return false;
}

bool nv2_handle_sync(u64 *regs, u32 ec)
{
    if (ec == 0x1a && nv2_eret_into_guest_el0())
        return true;
    return nv2_sync_slow(regs, ec);
}

static struct nv2_cpu nv2_cpus[MAX_CPUS];

static struct nv2_cpu *nv2_this_cpu(void)
{
    u64 mpidr = mrs(MPIDR_EL1) & 0xffffff;
    int idx = boot_cpu_idx;
    for (int i = 0; i < MAX_CPUS; i++)
        if (smp_is_alive(i) && (smp_get_mpidr(i) & 0xffffff) == mpidr) {
            idx = i;
            break;
        }
    struct nv2_cpu *c = &nv2_cpus[idx];
    if (!c->vncr_pa) {
        /* VNCR and AVNCR are adjacent 4 KiB pages, in aura's heap (reserved
         * from the OS), so the guest cannot reach the monitor's NV state. */
        u8 *pages = memalign(SZ_16K, 2 * SZ_4K);
        memset(pages, 0, 2 * SZ_4K);
        c->vncr_pa = (u64)pages;
        c->avncr_pa = (u64)pages + SZ_4K;
    }
    return c;
}

/*
 * Enter the OS at virtual EL2 (FEAT_NV2) instead of plain EL1. The base EL2
 * passthrough is the same as monitor_enter_os; additionally NV|NV2 redirect the
 * guest's EL2-register accesses to its VNCR/AVNCR pages and trap its ERETs, and
 * TTLB|AT let the monitor run the guest's TLBI/AT under its own stage-2 for L2.
 * The guest starts believing it is a VHE host (its HCR in VNCR = RW|E2H|TGE).
 */
void nv2_enter_os(u64 entry, u64 a0, u64 a1, u64 a2)
{
    monitor_el2_setup();

    struct nv2_cpu *c = nv2_this_cpu();
    volatile u64 *vncr = (volatile u64 *)c->vncr_pa;
    memset((void *)c->vncr_pa, 0, SZ_4K);
    vncr[0x78 / 8] = HCR_RW | HCR_E2H | HCR_TGE; /* guest HCR_EL2: VHE host */
    dc_cvac_range((void *)c->vncr_pa, SZ_4K);
    sysop("dsb sy");

    __asm__ volatile(".arch armv9.4-a\nmsr VNCR_EL2, %0" : : "r"(c->vncr_pa));
    __asm__ volatile("msr S3_4_C15_C10_7, %0" : : "r"(c->avncr_pa)); /* AVNCR_EL2 */
    reg_clr(HCR_EL2, HCR_NV1);
    reg_set(HCR_EL2, HCR_NV | HCR_NV2 | HCR_TTLB | HCR_AT);
    sysop("isb");

    monitor_eret(entry, a0, a1, a2);
}
