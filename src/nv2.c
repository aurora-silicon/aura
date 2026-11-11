/* SPDX-License-Identifier: MIT */

#include "../build/build_cfg.h"

#include "nv2.h"
#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "gxf.h"
#include "malloc.h"
#include "memory.h"
#include "smp.h"
#include "sptm_virt.h"
#include "string.h"
#include "utils.h"

/*
 * Names used below: L1 is the OS aura runs at virtual EL2 (real EL1 with
 * HCR_EL2.NV|NV2). A "child" is anything L1 enters with an ERET that is not
 * to itself or its own userspace: its EL1 kernel during early boot (with its
 * stage 2 off) and, after that, its own guests (L2). Only one context runs on
 * a CPU at a time; aura swaps the EL1 register file between L1 and the child
 * at each entry and exit, using the VNCR page as the child's bank.
 *
 * L1's IPAs are physical addresses, so the stage 2 it builds for a child is
 * too, and aura installs the child's VTTBR/VTCR into the hardware as they are.
 *
 * Offsets in the VNCR page are ARM's, measured on T8140 (2026-10-31). The
 * Apple AVNCR page holds L1's VM_TMR_FIQ_ENA_EL2 view at 0x408 and the nested
 * AVNCR pointer at 0x8 (also measured).
 */

bool nv2_active = false;
bool nv2_guarded = false;

/* True only while a proxy nv2_probe_call()/nv2_probe_arm() diagnostic is in
 * flight. It gates the `hvc #1` proxy-return path so a production guest's own
 * `hvc #1` cannot be redirected into proxy-only code. Also set/cleared from
 * nv2_asm.S (nv2_probe_call / nv2_probe_ret). */
bool nv2_probe_active = false;

/* VNCR page offsets */
#define V_VTTBR      0x020
#define V_VTCR       0x040
#define V_VMPIDR     0x050
#define V_CNTVOFF    0x060
#define V_HCR        0x078
#define V_HSTR       0x080
#define V_VPIDR      0x088
#define V_TPIDR      0x090
#define V_HCRX       0x0a0
#define V_VNCR       0x0b0
#define V_CPACR      0x100
#define V_CONTEXTIDR 0x108
#define V_SCTLR      0x110
#define V_ACTLR      0x118
#define V_TCR        0x120
#define V_AFSR0      0x128
#define V_AFSR1      0x130
#define V_ESR        0x138
#define V_MAIR       0x140
#define V_AMAIR      0x148
#define V_MDSCR      0x158
#define V_SPSR       0x160
#define V_CNTV_CVAL  0x168
#define V_CNTV_CTL   0x170
#define V_CNTP_CVAL  0x178
#define V_CNTP_CTL   0x180
#define V_HFGRTR     0x1b8
#define V_HFGWTR     0x1c0
#define V_HFGITR     0x1c8
#define V_HDFGRTR    0x1d0
#define V_HDFGWTR    0x1d8
#define V_SMCR       0x1f0
#define V_TTBR0      0x200
#define V_TTBR1      0x210
#define V_FAR        0x220
#define V_ELR        0x230
#define V_SP_EL1     0x240
#define V_VBAR       0x250
#define V_ICH_LR0    0x400
#define V_ICH_AP0R0  0x480
#define V_ICH_AP1R0  0x4a0
#define V_ICH_HCR    0x4c0
#define V_ICH_VMCR   0x4c8
#define V_WORDS      (0x500 / 8)

#define A_AVNCR          0x008
#define A_PMCR1          0x3c0 /* PMCR1_EL12: the guest's PMC filters */
#define A_VM_TMR_FIQ_ENA 0x408

#define VN(c, off) ((c)->vncr[(off) / 8])
#define AV(c, off) ((c)->avncr[(off) / 8])

#define VNCR_EL2  sys_reg(3, 4, 2, 2, 0)
#define AVNCR_EL2 sys_reg(3, 4, 15, 10, 7)

#define SYS_SPSR_EL12    sys_reg(3, 5, 4, 0, 0)
#define SYS_ELR_EL12     sys_reg(3, 5, 4, 0, 1)
#define SYS_SCTLR_EL12   sys_reg(3, 5, 1, 0, 0)
#define SYS_CPACR_EL12   sys_reg(3, 5, 1, 0, 2)
#define SYS_TTBR0_EL12   sys_reg(3, 5, 2, 0, 0)
#define SYS_TTBR1_EL12   sys_reg(3, 5, 2, 0, 1)
#define SYS_TCR_EL12     sys_reg(3, 5, 2, 0, 2)
#define SYS_AFSR0_EL12   sys_reg(3, 5, 5, 1, 0)
#define SYS_AFSR1_EL12   sys_reg(3, 5, 5, 1, 1)
#define SYS_ESR_EL12     sys_reg(3, 5, 5, 2, 0)
#define SYS_FAR_EL12     sys_reg(3, 5, 6, 0, 0)
#define SYS_MAIR_EL12    sys_reg(3, 5, 10, 2, 0)
#define SYS_AMAIR_EL12   sys_reg(3, 5, 10, 3, 0)
#define SYS_VBAR_EL12    sys_reg(3, 5, 12, 0, 0)
#define SYS_CTXIDR_EL12  sys_reg(3, 5, 13, 0, 1)
#define SYS_CNTKCTL_EL12 sys_reg(3, 5, 14, 1, 0)
#define SYS_CNTP_TVAL02  sys_reg(3, 5, 14, 2, 0)
#define SYS_CNTP_CTL02   sys_reg(3, 5, 14, 2, 1)
#define SYS_CNTP_CVAL02  sys_reg(3, 5, 14, 2, 2)
#define SYS_CNTV_TVAL02  sys_reg(3, 5, 14, 3, 0)
#define SYS_CNTV_CTL02   sys_reg(3, 5, 14, 3, 1)
#define SYS_CNTV_CVAL02  sys_reg(3, 5, 14, 3, 2)
#define SYS_SMCR_EL12    sys_reg(3, 5, 1, 2, 6)
#define SYS_HPFAR_EL2    sys_reg(3, 4, 6, 0, 4)
#define SYS_ICH_HCR      sys_reg(3, 4, 12, 11, 0)
#define SYS_ICH_VTR      sys_reg(3, 4, 12, 11, 1)
#define SYS_ICH_VMCR     sys_reg(3, 4, 12, 11, 7)
#define SYS_ICH_AP0R0    sys_reg(3, 4, 12, 8, 0)
#define SYS_ICH_AP1R0    sys_reg(3, 4, 12, 9, 0)
#define SYS_VMTMR_FIQENA sys_reg(3, 5, 15, 1, 3)
#define SYS_PMCR1_EL12   sys_reg(3, 1, 15, 7, 2)
/* EL1's Apple timer virtual control (CNTV_CTL layout: EN, IMASK, ISTATUS), XNU's kernel timer
 * under aura, as aura's EL2 reaches it (measured on T6050). At EL2 with E2H, S3_1_C15_C0_5 is
 * ACNTHV_CTL_EL2 (S3_1_C15_C7_4) instead. */
#define SYS_AGT_CNTV_CTL_EL02 sys_reg(3, 4, 15, 2, 7)

/* HFGITR_EL2 TLBI VMALLE1OS/IS/VMALLE1: the only EL1 TLBIs whose target L1
 * cannot express without a stage-2 context; L1's own TLBIs stay native. */
#define HFGITR_VMALLE1_ALL (BIT(18) | BIT(28) | BIT(42))
/* Every EL1 TLBI (HFGITR bits 18-47). KVM emulating a guest hypervisor's
 * TLBIs issues all of them under that guest's VMID with its TGE clear, so a
 * CPU that has run a guest hypervisor traps them all from then on. */
#define HFGITR_TLBI_EL1_ALL GENMASK(47, 18)
#define HFGWTR_CPACR_EL1    BIT(12)

/* CPTR_EL2 bits (E2H layout) a guest's trap control is made of. */
#define CPTR_GUEST_MASK (CPTR_E2H_FPEN | CPTR_E2H_SMEN | (3UL << 16) | BIT(28) | BIT(30) | BIT(31))
#define CPTR_TCPAC      BIT(31)

/* Bits of L1's HCR_EL2 view a child may run with. NV/NV1/NV2/TGE/E2H are
 * aura's: a child never sees TGE, E2H is RAO, NV1 is never set. */
#define CHILD_HCR_PASS                                                                             \
    (HCR_VM | HCR_SWIO | HCR_PTW | HCR_FMO | HCR_IMO | HCR_AMO | HCR_VF | HCR_VI | HCR_VSE |       \
     HCR_FB | HCR_BSU | HCR_DC | HCR_TWI | HCR_TWE | HCR_TID0 | HCR_TID1 | HCR_TID2 | HCR_TID3 |   \
     HCR_TSC | HCR_TIDCP | HCR_TACR | HCR_TSW | HCR_TPCP | HCR_TPU | HCR_TTLB | HCR_TVM |          \
     HCR_TDZ | HCR_HCD | HCR_TRVM | HCR_CD | HCR_ID | HCR_TERR | HCR_TEA | HCR_TLOR | HCR_APK |    \
     HCR_API | HCR_FIEN | HCR_FWB | HCR_TID4 | HCR_TICAB | HCR_TOCU | HCR_EnSCXT | HCR_TTLBIS |    \
     HCR_TTLBOS | HCR_ATA | HCR_DCT | HCR_TID5 | HCR_AT)

#define NV2_BANK_WORDS (0x260 / 8)

struct nv2_cpu {
    u64 *vncr, *avncr; /* L1's pages */
    u64 *scratch;      /* AVNCR for a nested hypervisor guest L1 gave none */
    u64 hcr;           /* real HCR_EL2 while L1 runs */
    bool armed;
    /* L1's real EL2 trap controls, restored at every child exit */
    u64 l1_hfgrtr, l1_hfgwtr, l1_hfgitr, l1_hdfgrtr, l1_hdfgwtr, l1_hcrx, l1_cptr, l1_cnthctl;
    u64 agt_hp_ctl;
    /* Set once this CPU has run a guest hypervisor (see nv2_child_enter). */
    bool nested_mode;
    u64 vcptr; /* L1's CPTR_EL2 as written, all bits, in nested mode */

    /* EL2 registers that trap under NV2 and are held here for L1. */
    u64 cnthctl, mdcr, hacr, actlr, hpfar, sre, cntkctl12, spsr32[4];

    /* The child running on this CPU, if any. */
    bool child, vm, nested;
    u64 parent[NV2_BANK_WORDS]; /* L1's EL1 register file, VNCR layout */
    u64 parent_sp1, parent_cntkctl, parent_vmtmr, parent_pmcr1;
    u64 gchild_hcr; /* L1's real HCR_EL2 while its guarded child (cL4) runs */
    u64 l1_tmr[4];  /* L1's real EL1 timers: P ctl, P cval, V ctl, V cval */

    /* counters, read by the proxy */
    u64 n_eret, n_sysreg, n_entry, n_exit, n_irq, n_tlbi, n_at, n_unknown, n_tlbi_own;
    u64 last_unknown; /* key | read << 63 of the latest unemulated register */
} ALIGNED(64);

static struct nv2_cpu nv2_cpus[MAX_CPUS];

/*
 * NV2_TRACE development builds log NV2 events to a per-CPU ring at
 * nv2_trace_buf. Nothing is printed unless NV2_TRACE_PRINT: once the OS owns
 * the console a printf from aura can stall a CPU for a second per byte.
 */
#ifdef NV2_TRACE
#define NV2_TRACE_CPU_BYTES 0x10000
#define NV2_TRACE_RECS      (NV2_TRACE_CPU_BYTES / 64 - 1)
struct nv2_trace_rec {
    u64 ts, type, a, b, c, d, e, f;
};
struct nv2_trace_cpu {
    u64 magic, head, cpu, rsvd[5];
    struct nv2_trace_rec rec[NV2_TRACE_RECS];
};
static struct nv2_trace_cpu *nv2_trace_buf;

static void nv2_trace(u64 type, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    if (!nv2_trace_buf)
        return;
    struct nv2_trace_cpu *t = &nv2_trace_buf[smp_id()];
    struct nv2_trace_rec *r = &t->rec[t->head % NV2_TRACE_RECS];
    r->ts = mrs(CNTPCT_EL0);
    r->type = type;
    r->a = a;
    r->b = b;
    r->c = c;
    r->d = d;
    r->e = e;
    r->f = f;
    sysop("dmb ish");
    t->head++;
#ifdef NV2_TRACE_PRINT
    /* Early boot only: aura may print until the OS owns the console. */
    if (t->head <= 300)
        printf("NV2[%d] %lu t%lu %lx %lx %lx %lx %lx %lx\n", smp_id(), t->head, type, a, b, c, d, e,
               f);
#endif
}
#else
#define nv2_trace(...) (void)0
#endif

enum {
    NV2_T_ENTER = 1,
    NV2_T_EXIT,
    NV2_T_SYSREG,
    NV2_T_UNKNOWN,
    NV2_T_TLBI,
    NV2_T_AT,
    NV2_T_ERET,
    NV2_T_INJECT,
    NV2_T_FAIL,
    NV2_T_KICK,
    NV2_T_GIC,
};
static u8 *nv2_pages[MAX_CPUS];

static inline struct nv2_cpu *nv2_this(void)
{
    return &nv2_cpus[smp_id()];
}

void nv2_prepare(void)
{
#ifdef NV2_TRACE
    if (!nv2_trace_buf) {
        nv2_trace_buf = memalign(SZ_16K, MAX_CPUS * NV2_TRACE_CPU_BYTES);
        if (!nv2_trace_buf)
            panic("NV2: no memory for the trace ring\n");
        memset(nv2_trace_buf, 0, MAX_CPUS * NV2_TRACE_CPU_BYTES);
        for (int i = 0; i < MAX_CPUS; i++) {
            nv2_trace_buf[i].magic = 0x4e5632545243ULL; /* "NV2TRC" */
            nv2_trace_buf[i].cpu = i;
        }
    }
#endif
    for (int i = 0; i < MAX_CPUS; i++) {
        /* A guarded guest (SPTM) brings every core up itself, through a reset, so
         * give each ADT core its pages now (the reset path runs with the MMU off). */
        if (nv2_pages[i] ||
            (i != boot_cpu_idx && !smp_is_alive(i) && !(nv2_guarded && smp_cpu_valid(i))))
            continue;
        /* VNCR and AVNCR: adjacent 4 KiB pages in the retained heap. */
        nv2_pages[i] = memalign(SZ_16K, SZ_16K);
        if (!nv2_pages[i])
            panic("NV2: no memory for CPU %d's NV pages\n", i);
        memset(nv2_pages[i], 0, SZ_16K);
    }
}

/*
 * aura's own stage 2 for L1 (the OS at vEL2), to trap a few pages. nv2_enter_os leaves HCR.VM
 * clear, so the guest's own bring-up runs as without it; the caller arms it per CPU with
 * nv2_l1_stage2_enable_here at a point it chooses. A child's exit restores it instead of
 * VTTBR = 0.
 */
static u64 nv2_l1_vtcr, nv2_l1_vttbr;

void nv2_set_l1_stage2_deferred(u64 vtcr, u64 vttbr)
{
    nv2_l1_vtcr = vtcr;
    nv2_l1_vttbr = vttbr;
    dc_cvac_range(&nv2_l1_vtcr, sizeof(nv2_l1_vtcr));
    dc_cvac_range(&nv2_l1_vttbr, sizeof(nv2_l1_vttbr));
}

bool nv2_l1_stage2_enable_here(void)
{
    if (!nv2_l1_vttbr)
        return false;
    struct nv2_cpu *c = nv2_this();
    msr(VTCR_EL2, nv2_l1_vtcr);
    msr(VTTBR_EL2, nv2_l1_vttbr);
    c->hcr |= HCR_VM;
    msr(HCR_EL2, mrs(HCR_EL2) | HCR_VM);
    sysop("isb");
    __asm__ volatile("tlbi vmalls12e1" ::: "memory");
    sysop("dsb nsh");
    sysop("isb");
    return true;
}

/* Keep the per-CPU HCR copy (restored on child exit) in step with a live HCR change. With
 * aura's GXF/SPRR live, HCR_EL2 is writable only from GL2 (a write from plain EL2, e.g. in an
 * FIQ taken from XNU, UNDEFs), so go through gl2_call there. */
static u64 nv2_hcr_write_gl2(u64 v, u64 a, u64 b, u64 c)
{
    UNUSED(a);
    UNUSED(b);
    UNUSED(c);
    msr(HCR_EL2, v);
    sysop("isb");
    return 0;
}

void nv2_hcr_update(u64 set, u64 clr)
{
    struct nv2_cpu *c = nv2_this();
    c->hcr = (c->hcr & ~clr) | set;
    u64 v = (mrs(HCR_EL2) & ~clr) | set;
    if (!in_gl12() && gxf_enabled())
        gl2_call(nv2_hcr_write_gl2, v, 0, 0, 0);
    else
        msr(HCR_EL2, v);
}

/* A core re-entering through reset: drop any stale cached copy of its NV pages. */
void nv2_cpu_reset_invalidate(int cpu)
{
    if (cpu >= 0 && cpu < MAX_CPUS && nv2_pages[cpu])
        dc_ivac_range(nv2_pages[cpu], SZ_16K);
}

/* ---------------------------------------------------------------------- */
/* EL1 register file swap */

#define NV2_BANK(OP)                                                                               \
    OP(SYS_CPACR_EL12, V_CPACR)                                                                    \
    OP(SYS_CTXIDR_EL12, V_CONTEXTIDR)                                                              \
    OP(SYS_TCR_EL12, V_TCR)                                                                        \
    OP(SYS_AFSR0_EL12, V_AFSR0)                                                                    \
    OP(SYS_AFSR1_EL12, V_AFSR1)                                                                    \
    OP(SYS_ESR_EL12, V_ESR)                                                                        \
    OP(SYS_MAIR_EL12, V_MAIR)                                                                      \
    OP(SYS_AMAIR_EL12, V_AMAIR)                                                                    \
    OP(SYS_MDSCR_EL1, V_MDSCR)                                                                     \
    OP(SYS_SPSR_EL12, V_SPSR)                                                                      \
    OP(SYS_TTBR0_EL12, V_TTBR0)                                                                    \
    OP(SYS_TTBR1_EL12, V_TTBR1)                                                                    \
    OP(SYS_FAR_EL12, V_FAR)                                                                        \
    OP(SYS_ELR_EL12, V_ELR)                                                                        \
    OP(SYS_VBAR_EL12, V_VBAR)

static void nv2_bank_save(u64 *b)
{
#define SAVE(reg, off) b[(off) / 8] = _mrs(sr_tkn(reg));
    NV2_BANK(SAVE)
#undef SAVE
    b[V_SCTLR / 8] = mrs(SYS_SCTLR_EL12);
}

static void nv2_bank_load(const u64 *b)
{
    /* Translation off while its controls change under it. */
    msr(SYS_SCTLR_EL12, 0);
    sysop("isb");
#define LOAD(reg, off) _msr(sr_tkn(reg), b[(off) / 8]);
    NV2_BANK(LOAD)
#undef LOAD
    msr(SYS_SCTLR_EL12, b[V_SCTLR / 8]);
    sysop("isb");
}

/*
 * A guarded nested child (cL4 under SPTM). SPTM is an NV2 guest hypervisor, so the EL1 regime it
 * programs for cL4 lands in its VNCR page, like an L2 guest's bank: load it from there when SPTM's
 * guarded ERET enters cL4, and restore SPTM's own when cL4 returns. The guarded bank and the entry
 * PC/PSTATE are the caller's (sptm_virt_cl4.c).
 */
static u64 nv2_child_hcr(u64 vhcr);
static void nv2_timers_enter(struct nv2_cpu *c, bool guarded);
static bool nv2_timers_exit(struct nv2_cpu *c, bool guarded);

/* cL4 is SPTM's nested guest, so it runs under the HCR_EL2 SPTM programmed for it (its VNCR
 * HCR_EL2), exactly as nv2_child_enter runs a KVM L2 - not under SPTM's own HCR. SPTM's HCR
 * carries NV|NV2 (SPTM is a guest hypervisor at virtual EL2); cL4 is not, and with NV set the
 * hardware treats it as a virtual-EL2 guest hypervisor too. SPTM's HCR is restored on exit. Must
 * be called from GL2 (HCR_EL2 is GL2-writable only while aura's GXF is live). */
void nv2_guarded_child_enter(void)
{
    struct nv2_cpu *c = nv2_this();
    /* The same EL1 file swap as nv2_child_enter. */
    nv2_bank_save(c->parent);
    c->parent_sp1 = mrs(SP_EL1);
    c->parent_cntkctl = mrs(SYS_CNTKCTL_EL12);
    nv2_bank_load(c->vncr);
    msr(SP_EL1, VN(c, V_SP_EL1));
    msr(SYS_CNTKCTL_EL12, c->cntkctl12);
    /* The host's scheduler timers are the physical EL1 timers here (its EL2 timers on real
     * hardware): cL4 gets the ones SPTM programmed for it (VNCR), and the host's deadline
     * moves to aura's EL2 timer. cL4's HCR has no FMO, so that FIQ reaches cL4 natively,
     * as the host's EL2 timer FIQ does on real hardware. */
    nv2_timers_enter(c, true);
    c->gchild_hcr = mrs(HCR_EL2);
    u64 chcr = nv2_child_hcr(VN(c, V_HCR));
    msr(HCR_EL2, chcr);
    sysop("isb");
}

void nv2_guarded_child_exit(void)
{
    struct nv2_cpu *c = nv2_this();
    msr(HCR_EL2, c->gchild_hcr);
    sysop("isb");
    /* A child timer still asserting stays visible in the VNCR page (ISTATUS) for SPTM. */
    nv2_timers_exit(c, true);
    /* The child's EL1 regime goes back to the VNCR page: SPTM reads it there (NV2). */
    nv2_bank_save(c->vncr);
    VN(c, V_SP_EL1) = mrs(SP_EL1);
    c->cntkctl12 = mrs(SYS_CNTKCTL_EL12);
    nv2_bank_load(c->parent);
    msr(SP_EL1, c->parent_sp1);
    msr(SYS_CNTKCTL_EL12, c->parent_cntkctl);
}

/* ---------------------------------------------------------------------- */
/* Timers and the vGIC around a guest */

static void nv2_timers_enter(struct nv2_cpu *c, bool guarded)
{
    c->l1_tmr[0] = mrs(SYS_CNTP_CTL02);
    c->l1_tmr[1] = mrs(SYS_CNTP_CVAL02);
    c->l1_tmr[2] = mrs(SYS_CNTV_CTL02);
    c->l1_tmr[3] = mrs(SYS_CNTV_CVAL02);
    msr(SYS_CNTP_CTL02, 0);
    msr(SYS_CNTV_CTL02, 0);

    /* L1's scheduler deadline (its hyp timers are the real EL1 ones) moves to
     * aura's EL2 physical timer while the guest owns those; its FIQ
     * exits the guest and L1 then finds its own timer firing. L1's CNTVOFF is
     * 0, so the virtual deadline is in the same units. */
    bool p_on = (c->l1_tmr[0] & 3) == 1, v_on = (c->l1_tmr[2] & 3) == 1;
    if (p_on || v_on) {
        u64 at = p_on ? c->l1_tmr[1] : c->l1_tmr[3];
        if (p_on && v_on && (s64)(c->l1_tmr[3] - at) < 0)
            at = c->l1_tmr[3];
        msr(CNTP_CVAL_EL0, at);
        msr(CNTP_CTL_EL0, 1);
    }

    msr(CNTVOFF_EL2, VN(c, V_CNTVOFF));
    msr(SYS_CNTP_CVAL02, VN(c, V_CNTP_CVAL));
    msr(SYS_CNTV_CVAL02, VN(c, V_CNTV_CVAL));
    msr(SYS_CNTP_CTL02, VN(c, V_CNTP_CTL) & 3);
    msr(SYS_CNTV_CTL02, VN(c, V_CNTV_CTL) & 3);
    msr(CNTHCTL_EL2, (c->cnthctl & GENMASK(17, 10)) | (mrs(CNTHCTL_EL2) & GENMASK(9, 0)));
    if (guarded)
        return; /* SPTM's guarded child is not a VM: Apple's VM timer-FIQ gate stays as is */
    c->parent_vmtmr = mrs(SYS_VMTMR_FIQENA);
    msr(SYS_VMTMR_FIQENA, (c->parent_vmtmr & ~3UL) | (AV(c, A_VM_TMR_FIQ_ENA) & 3));
}

/* Returns true if one of the guest's timers is asserting its FIQ. */
static bool nv2_timers_exit(struct nv2_cpu *c, bool guarded)
{
    u64 pctl = mrs(SYS_CNTP_CTL02), vctl = mrs(SYS_CNTV_CTL02);
    VN(c, V_CNTP_CVAL) = mrs(SYS_CNTP_CVAL02);
    VN(c, V_CNTV_CVAL) = mrs(SYS_CNTV_CVAL02);
    /* Keep ISTATUS: L1 reads these through the VNCR page. */
    VN(c, V_CNTP_CTL) = pctl;
    VN(c, V_CNTV_CTL) = vctl;
    msr(SYS_CNTP_CTL02, 0);
    msr(SYS_CNTV_CTL02, 0);

    msr(CNTP_CTL_EL0, 0); /* the EL2 deadline proxy */
    msr(CNTVOFF_EL2, 0);
    msr(SYS_CNTP_CVAL02, c->l1_tmr[1]);
    msr(SYS_CNTV_CVAL02, c->l1_tmr[3]);
    msr(SYS_CNTP_CTL02, c->l1_tmr[0] & 3);
    msr(SYS_CNTV_CTL02, c->l1_tmr[2] & 3);
    msr(CNTHCTL_EL2, (c->l1_cnthctl & GENMASK(17, 10)) | (mrs(CNTHCTL_EL2) & GENMASK(9, 0)));

    u64 ena = guarded ? 3 : AV(c, A_VM_TMR_FIQ_ENA);
    if (!guarded)
        msr(SYS_VMTMR_FIQENA, c->parent_vmtmr);
#define FIRING(ctl) (((ctl) & 7) == 5)
    bool firing = ((ena & 2) && FIRING(pctl)) || ((ena & 1) && FIRING(vctl));
#undef FIRING
    if (firing)
        nv2_trace(NV2_T_KICK, pctl, vctl, ena, VN(c, V_CNTP_CVAL), VN(c, V_CNTV_CVAL),
                  mrs(CNTPCT_EL0) - VN(c, V_CNTVOFF));
    return firing;
}

static u64 nv2_ich_lr_read(int i)
{
    switch (i) {
        case 0:
            return mrs(sys_reg(3, 4, 12, 12, 0));
        case 1:
            return mrs(sys_reg(3, 4, 12, 12, 1));
        case 2:
            return mrs(sys_reg(3, 4, 12, 12, 2));
        case 3:
            return mrs(sys_reg(3, 4, 12, 12, 3));
        case 4:
            return mrs(sys_reg(3, 4, 12, 12, 4));
        case 5:
            return mrs(sys_reg(3, 4, 12, 12, 5));
        case 6:
            return mrs(sys_reg(3, 4, 12, 12, 6));
        default:
            return mrs(sys_reg(3, 4, 12, 12, 7));
    }
}

static void nv2_ich_lr_write(int i, u64 v)
{
    switch (i) {
        case 0:
            msr(sys_reg(3, 4, 12, 12, 0), v);
            break;
        case 1:
            msr(sys_reg(3, 4, 12, 12, 1), v);
            break;
        case 2:
            msr(sys_reg(3, 4, 12, 12, 2), v);
            break;
        case 3:
            msr(sys_reg(3, 4, 12, 12, 3), v);
            break;
        case 4:
            msr(sys_reg(3, 4, 12, 12, 4), v);
            break;
        case 5:
            msr(sys_reg(3, 4, 12, 12, 5), v);
            break;
        case 6:
            msr(sys_reg(3, 4, 12, 12, 6), v);
            break;
        default:
            msr(sys_reg(3, 4, 12, 12, 7), v);
            break;
    }
}

static int nv2_ich_lrs;

static void nv2_gic_enter(struct nv2_cpu *c)
{
    for (int i = 0; i < nv2_ich_lrs; i++)
        nv2_ich_lr_write(i, c->vncr[V_ICH_LR0 / 8 + i]);
    msr(SYS_ICH_AP0R0, VN(c, V_ICH_AP0R0));
    msr(SYS_ICH_AP1R0, VN(c, V_ICH_AP1R0));
    msr(SYS_ICH_VMCR, VN(c, V_ICH_VMCR));
    msr(SYS_ICH_HCR, VN(c, V_ICH_HCR));
}

static void nv2_gic_exit(struct nv2_cpu *c)
{
    nv2_trace(NV2_T_GIC, mrs(sys_reg(3, 4, 12, 11, 2)),
              mrs(sys_reg(3, 4, 12, 11, 3)) | (mrs(sys_reg(3, 4, 12, 11, 5)) << 32),
              mrs(SYS_ICH_VMCR), mrs(SYS_ICH_HCR), nv2_ich_lr_read(0), mrs(SYS_VMTMR_FIQENA));
    VN(c, V_ICH_HCR) = mrs(SYS_ICH_HCR);
    msr(SYS_ICH_HCR, 0);
    for (int i = 0; i < nv2_ich_lrs; i++)
        c->vncr[V_ICH_LR0 / 8 + i] = nv2_ich_lr_read(i);
    VN(c, V_ICH_AP0R0) = mrs(SYS_ICH_AP0R0);
    VN(c, V_ICH_AP1R0) = mrs(SYS_ICH_AP1R0);
    VN(c, V_ICH_VMCR) = mrs(SYS_ICH_VMCR);
}

/* ICH_ELRSR/EISR/MISR_EL2 computed from L1's list registers in the VNCR page
 * (ARM IHI 0069, ICH_{ELRSR,EISR,MISR}_EL2): L1 may have rewritten them since
 * the last exit, so the hardware's values would be stale. */
static u64 nv2_gic_status(struct nv2_cpu *c, unsigned op2)
{
    u64 empty = 0, eoi = 0;
    unsigned valid = 0;
    bool pending = false;
    for (int i = 0; i < nv2_ich_lrs; i++) {
        u64 lr = c->vncr[V_ICH_LR0 / 8 + i];
        unsigned state = lr >> 62;
        bool eoi_event = !state && !(lr & BIT(61)) && (lr & BIT(41));
        if (!state && !eoi_event)
            empty |= BIT(i);
        if (eoi_event)
            eoi |= BIT(i);
        valid += !!state;
        pending |= !!(state & 1);
    }
    if (op2 == 5)
        return empty;
    if (op2 == 3)
        return eoi;
    u64 hcr = VN(c, V_ICH_HCR), vmcr = VN(c, V_ICH_VMCR);
    u64 misr = !!eoi;
    if ((hcr & BIT(1)) && valid <= 1)
        misr |= BIT(1);
    if ((hcr & BIT(2)) && (hcr & GENMASK(31, 27)))
        misr |= BIT(2);
    if ((hcr & BIT(3)) && !pending)
        misr |= BIT(3);
    for (unsigned group = 0; group < 2; group++) {
        unsigned bit = 4 + 2 * group + !(vmcr & BIT(group));
        if (hcr & BIT(bit))
            misr |= BIT(bit);
    }
    return misr;
}

/* ---------------------------------------------------------------------- */
/* Child entry and exit */

static u64 nv2_child_hcr(u64 vhcr)
{
    u64 hcr = (vhcr & CHILD_HCR_PASS) | HCR_RW | HCR_E2H;
    if (vhcr & HCR_VM)
        hcr |= HCR_TSC | HCR_TWI; /* SMC has no EL3 here; WFI wipes registers */
    return hcr;
}

static bool nv2_child_enter(struct nv2_cpu *c, u64 spsr, u64 elr)
{
    u64 vhcr = VN(c, V_HCR);
    bool vm = vhcr & HCR_VM;

    /*
     * A guest hypervisor (recursion): L1 runs it with NV|NV2 and its own VNCR
     * page, named by an L1 virtual address. Translate that while L1's stage
     * 1 is live; with no stage 2 for L1 the result is the PA the hardware
     * redirects to. NV1 is never set (VHE only), and Linux's KVM does not
     * know AVNCR, so the guest's Apple EL2 registers go to a scratch page.
     */
    u64 nested_vncr = 0;
    bool nested = vhcr & (HCR_NV | HCR_NV1 | HCR_NV2);
    if (nested) {
        if ((vhcr & (HCR_NV | HCR_NV1 | HCR_NV2)) != (HCR_NV | HCR_NV2) || !vm) {
            nv2_trace(NV2_T_FAIL, 0x1a, spsr, elr, vhcr, 1, 0);
            return false;
        }
        u64 va = VN(c, V_VNCR), par = mrs(PAR_EL1);
        __asm__ volatile("at s1e1w, %0" ::"r"(va) : "memory");
        sysop("isb");
        u64 res = mrs(PAR_EL1);
        msr(PAR_EL1, par);
        if ((res & 1) || (va & 0xfff)) {
            nv2_trace(NV2_T_FAIL, 0x1a, spsr, va, res, 8, 0);
            return false;
        }
        nested_vncr = (res & GENMASK(47, 12)) | (va & 0xfff);
        if (!c->nested_mode) {
            /*
             * From now on this CPU traps all of L1's EL1 TLBIs (KVM emulates
             * a guest hypervisor's TLBIs under its VMID with TGE clear) and
             * its CPACR_EL1 writes: L1 writes its CPTR_EL2 through CPACR_EL1,
             * whose hardware drops TCPAC, the bit KVM uses (without
             * FEAT_NV2p1) to trap a guest hypervisor's own CPACR accesses.
             * This first entry's TCPAC is that rule; later ones are exact.
             */
            c->nested_mode = true;
            c->l1_hfgitr = HFGITR_TLBI_EL1_ALL;
            c->l1_hfgwtr |= HFGWTR_CPACR_EL1;
            c->vcptr = mrs(SYS_CPACR_EL12) | CPTR_TCPAC;
        }
    }

    /* Under SPTM the host (SPTM/TXM/XNU) and this EL1 guest also differ in Apple's per-level
     * registers: switch them around the EL1 file swap, as for cL4 (sptm_virt_cl4.c). */
    if (nv2_guarded)
        sptm_virt_world_child_pre_enter();
    nv2_bank_save(c->parent);
    c->parent_sp1 = mrs(SP_EL1);
    c->parent_cntkctl = mrs(SYS_CNTKCTL_EL12);
    nv2_bank_load(c->vncr);
    msr(SP_EL1, VN(c, V_SP_EL1));
    msr(SYS_CNTKCTL_EL12, c->cntkctl12);
    if (nv2_guarded)
        sptm_virt_world_child_post_enter();

    /* L1's CPACR_EL1 is its VHE CPTR_EL2: it is the child's trap control. */
    msr(SYS_CPTR_EL2, (c->nested_mode ? c->vcptr : c->parent[V_CPACR / 8]) & CPTR_GUEST_MASK);
    msr(SYS_HFGITR_EL2, VN(c, V_HFGITR)); /* not L1's VMALLE1 traps */

    if (vm) {
        msr(VTTBR_EL2, VN(c, V_VTTBR));
        msr(VTCR_EL2, VN(c, V_VTCR));
        msr(VPIDR_EL2, VN(c, V_VPIDR));
        msr(VMPIDR_EL2, VN(c, V_VMPIDR));
        msr(HSTR_EL2, VN(c, V_HSTR));
        /* A guarded guest's HACR_EL2 is aura's (sptm_virt_cpu_arm), not the child's. */
        if (!nv2_guarded)
            msr(HACR_EL2, c->hacr);
        msr(MDCR_EL2, c->mdcr);
        msr(SYS_HFGRTR_EL2, VN(c, V_HFGRTR));
        msr(SYS_HFGWTR_EL2, VN(c, V_HFGWTR));
        msr(SYS_HDFGRTR_EL2, VN(c, V_HDFGRTR));
        msr(SYS_HDFGWTR_EL2, VN(c, V_HDFGWTR));
        msr(SYS_HCRX_EL2, VN(c, V_HCRX));
        nv2_timers_enter(c, false);
        nv2_gic_enter(c);
        /* Apple banks PMCR1 by EL: L1 sets its guest's PMC filters through
         * PMCR1_EL12, which lands in the AVNCR page; the hardware applies the
         * EL1 copy, which holds L1's own filters while L1 runs. */
        c->parent_pmcr1 = mrs(SYS_PMCR1_EL12);
        msr(SYS_PMCR1_EL12, AV(c, A_PMCR1));
    }

    if (nested) {
        msr(VNCR_EL2, nested_vncr);
        msr(AVNCR_EL2, (u64)c->scratch);
    }
    msr(HCR_EL2, nv2_child_hcr(vhcr) | (nested ? HCR_NV | HCR_NV2 : 0));
    c->nested = nested;
    msr(SPSR_EL2, spsr);
    msr(ELR_EL2, elr);
    sysop("isb");
    c->child = true;
    c->vm = vm;
    c->n_entry++;
    nv2_trace(NV2_T_ENTER, elr, spsr, vhcr, VN(c, V_VTTBR), VN(c, V_VTCR), VN(c, V_ICH_HCR));
    return true;
}

/* Exception entry PSTATE for L1 at its own vector: EL2h (run as EL1h), DAIF
 * masked, NZCV/DIT kept, PAN per SCTLR.SPAN, SSBS per SCTLR.DSSBS. */
static u64 nv2_entry_pstate(u64 from_spsr)
{
    u64 pstate = (from_spsr & (GENMASK(31, 28) | SPSR_DIT | SPSR_PAN)) | 0x3c5;
    u64 sctlr = mrs(SYS_SCTLR_EL12);
    if (!(sctlr & SCTLR_SPAN))
        pstate |= SPSR_PAN;
    if (sctlr & SCTLR_DSSBS)
        pstate |= SPSR_SSBS;
    else
        pstate &= ~SPSR_SSBS;
    return pstate;
}

/*
 * The child exited to EL2 (sync with esr, or kind 1/2/3 = IRQ/FIQ/SError):
 * hand the exit to L1 at its lower-EL vector as the hardware would have.
 */
static void nv2_child_exit(struct nv2_cpu *c, int kind, u64 esr)
{
    u64 elr = mrs(ELR_EL2), spsr = mrs(SPSR_EL2);
    u64 far = kind ? 0 : mrs(FAR_EL2);
    bool kick = false;

    /* First: the child's FP traps cover EL2 too. */
    msr(SYS_CPTR_EL2, c->l1_cptr);
    sysop("isb");

    if (c->vm) {
        if (!kind)
            c->hpfar = mrs(SYS_HPFAR_EL2);
        nv2_gic_exit(c);
        kick = nv2_timers_exit(c, false);
        msr(VTTBR_EL2, nv2_l1_vttbr);
        if (nv2_l1_vttbr)
            msr(VTCR_EL2, nv2_l1_vtcr);
        msr(VPIDR_EL2, mrs(MIDR_EL1));
        msr(VMPIDR_EL2, mrs(MPIDR_EL1));
        msr(HSTR_EL2, 0);
        if (!nv2_guarded)
            msr(HACR_EL2, 0);
        msr(MDCR_EL2, 0);
        msr(SYS_PMCR1_EL12, c->parent_pmcr1);
        msr(SYS_HFGRTR_EL2, c->l1_hfgrtr);
        msr(SYS_HFGWTR_EL2, c->l1_hfgwtr);
        msr(SYS_HDFGRTR_EL2, c->l1_hdfgrtr);
        msr(SYS_HDFGWTR_EL2, c->l1_hdfgwtr);
        msr(SYS_HCRX_EL2, c->l1_hcrx);
    }

    if (c->nested) {
        msr(VNCR_EL2, (u64)c->vncr);
        msr(AVNCR_EL2, (u64)c->avncr);
        c->nested = false;
    }
    if (nv2_guarded)
        sptm_virt_world_child_pre_exit();
    nv2_bank_save(c->vncr);
    VN(c, V_SP_EL1) = mrs(SP_EL1);
    c->cntkctl12 = mrs(SYS_CNTKCTL_EL12);
    nv2_bank_load(c->parent);
    msr(SP_EL1, c->parent_sp1);
    msr(SYS_CNTKCTL_EL12, c->parent_cntkctl);
    if (nv2_guarded)
        sptm_virt_world_child_post_exit();
    msr(SYS_HFGITR_EL2, c->l1_hfgitr);
    msr(HCR_EL2, c->hcr);

    msr(SYS_ELR_EL12, elr);
    msr(SYS_SPSR_EL12, spsr);
    if (!kind) {
        msr(SYS_ESR_EL12, esr);
        msr(SYS_FAR_EL12, far);
        /* Apple's IMPDEF traps (EC 0x3f: the PMUv3 registers under
         * HACR_EL2[56]) leave their sysreg ISS in AFSR1_EL2. L1's AFSR1_EL2
         * is the EL1 register while it runs. */
        if ((esr >> 26) == ESR_EC_IMPDEF)
            msr(SYS_AFSR1_EL12, mrs(SYS_AFSR1_EL1));
    }
    msr(ELR_EL2, mrs(SYS_VBAR_EL12) + 0x400 + kind * 0x80);
    msr(SPSR_EL2, nv2_entry_pstate(spsr));
    sysop("isb");
    c->child = false;
    c->n_exit++;
    nv2_trace(NV2_T_EXIT, kind | ((u64)kick << 8), esr, elr, far, c->hpfar, mrs(ELR_EL2));

    /* A guest timer asserting at exit is now held only in the VNCR page.
     * Its physical FIQ went with the guest, so raise one for L1 with a
     * self-IPI: L1's interrupt handler then finds the guest timer firing
     * through the VNCR page as it would in hardware. */
    if (kick) {
        msr(SYS_IMP_APL_IPI_RR_LOCAL_EL1, mrs(MPIDR_EL1) & 0xff);
    }
}

/* Synchronous exception from a running child. */
static bool nv2_child_sync(struct nv2_cpu *c, u64 esr)
{
    u32 ec = esr >> 26;
    u64 vhcr = VN(c, V_HCR);

    /* What would have reached EL2 had L1 really been there: an SMC L1 does
     * not trap is UNDEFINED (no EL3); everything else is L1's to handle. */
    if (ec == ESR_EC_SMC && !(vhcr & HCR_TSC)) {
        u64 spsr = mrs(SPSR_EL2), elr = mrs(ELR_EL2);
        msr(SYS_ESR_EL12, 0x2000000);
        msr(SYS_ELR_EL12, elr);
        msr(SYS_SPSR_EL12, spsr);
        msr(ELR_EL2, mrs(SYS_VBAR_EL12) + ((spsr & 0xf) ? 0x200 : 0x400));
        msr(SPSR_EL2, (spsr & GENMASK(31, 28)) | 0x3c5);
        return true;
    }
    nv2_child_exit(c, 0, esr);
    return true;
}

/* ---------------------------------------------------------------------- */
/* TLBI and AT on L1's behalf */

#define TLBI1(crm, op2)                                                                            \
    case (crm) * 8 + (op2):                                                                        \
        __asm__ volatile("sys #0, c8, c" #crm ", #" #op2 ", %0" ::"r"(val) : "memory");            \
        return true;
#define TLBI4(crm, op2)                                                                            \
    case (crm) * 8 + (op2):                                                                        \
        __asm__ volatile("sys #4, c8, c" #crm ", #" #op2 ", %0" ::"r"(val) : "memory");            \
        return true;

/* EL1-form TLBIs: L1's own (EL2&0 regime, run as EL1&0 with VMID 0), and
 * those FGT traps (all of them once a guest hypervisor ran). */
static bool nv2_tlbi_el1(unsigned crm, unsigned op2, u64 val)
{
    switch (crm * 8 + op2) {
        TLBI1(1, 0)
        TLBI1(1, 1)
        TLBI1(1, 2)
        TLBI1(1, 3)
        TLBI1(1, 5)
        TLBI1(1, 7)
        TLBI1(3, 0)
        TLBI1(3, 1)
        TLBI1(3, 2)
        TLBI1(3, 3)
        TLBI1(3, 5)
        TLBI1(3, 7)
        TLBI1(7, 0)
        TLBI1(7, 1)
        TLBI1(7, 2)
        TLBI1(7, 3)
        TLBI1(7, 5)
        TLBI1(7, 7)
        TLBI1(2, 1)
        TLBI1(2, 3)
        TLBI1(2, 5)
        TLBI1(2, 7)
        TLBI1(5, 1)
        TLBI1(5, 3)
        TLBI1(5, 5)
        TLBI1(5, 7)
        TLBI1(6, 1)
        TLBI1(6, 3)
        TLBI1(6, 5)
        TLBI1(6, 7)
        default:
            return false;
    }
}

/* EL2-form TLBIs that name the EL1&0 regime of a VMID (or all of them). */
static bool nv2_tlbi_el2(unsigned crm, unsigned op2, u64 val)
{
    switch (crm * 8 + op2) {
        TLBI4(0, 1)
        TLBI4(0, 2)
        TLBI4(0, 5)
        TLBI4(0, 6)
        TLBI4(1, 4)
        TLBI4(1, 6)
        TLBI4(3, 4)
        TLBI4(3, 6)
        TLBI4(4, 0)
        TLBI4(4, 1)
        TLBI4(4, 2)
        TLBI4(4, 3)
        TLBI4(4, 4)
        TLBI4(4, 5)
        TLBI4(4, 6)
        TLBI4(4, 7)
        TLBI4(7, 4)
        TLBI4(7, 6)
        default:
            return false;
    }
}
#undef TLBI1
#undef TLBI4

static bool nv2_tlbi(struct nv2_cpu *c, unsigned op1, unsigned crm, unsigned op2, u64 val)
{
    c->n_tlbi++;
    /* L1's own translations: the trapped VMALLE1 family with its TGE set, and
     * the *E2 forms, whose EL2&0 regime is the real EL1&0 one with VMID 0. */
    bool e2 = op1 == 4 && crm != 0 && crm != 4 && (op2 == 0 || op2 == 1 || op2 == 5);
    if (e2 || (op1 == 0 && (VN(c, V_HCR) & HCR_TGE))) {
        c->n_tlbi_own++;
        return nv2_tlbi_el1(crm, op2, val);
    }

    /* A guest's VMID: run it under the guest's VTTBR. */
    u64 vttbr = mrs(VTTBR_EL2);
    msr(VTTBR_EL2, VN(c, V_VTTBR));
    sysop("isb");
    bool ok = op1 == 0 ? nv2_tlbi_el1(crm, op2, val) : nv2_tlbi_el2(crm, op2, val);
    sysop("dsb ish");
    msr(VTTBR_EL2, vttbr);
    sysop("isb");
    return ok;
}

#define AT_OP(name) __asm__ volatile("at " name ", %0" ::"r"(va) : "memory")

static void nv2_at_exec(unsigned op, u64 va)
{
    switch (op) {
        case 0:
            AT_OP("s1e1r");
            break;
        case 1:
            AT_OP("s1e1w");
            break;
        case 2:
            AT_OP("s1e0r");
            break;
        case 3:
            AT_OP("s1e0w");
            break;
        case 4:
            AT_OP("s12e1r");
            break;
        case 5:
            AT_OP("s12e1w");
            break;
        case 6:
            AT_OP("s12e0r");
            break;
        case 7:
            AT_OP("s12e0w");
            break;
        case 8:
            AT_OP("s1e1rp");
            break;
        case 9:
            AT_OP("s1e1wp");
            break;
    }
}
#undef AT_OP

/* op: 0-3 S1E1R/W S1E0R/W, 4-7 S12E1R/W S12E0R/W, 8/9 S1E1RP/WP. */
static void nv2_at(struct nv2_cpu *c, unsigned op, bool own, u64 va)
{
    c->n_at++;
    if (own) {
        nv2_at_exec(op, va);
        sysop("isb");
        return;
    }
    /* The guest's stage 1 from the VNCR page, its stage 2 and VMID. */
    u64 sctlr = mrs(SYS_SCTLR_EL12), tcr = mrs(SYS_TCR_EL12), t0 = mrs(SYS_TTBR0_EL12),
        t1 = mrs(SYS_TTBR1_EL12), mair = mrs(SYS_MAIR_EL12), hcr = mrs(HCR_EL2),
        vttbr = mrs(VTTBR_EL2), vtcr = mrs(VTCR_EL2);
    msr(SYS_SCTLR_EL12, 0);
    sysop("isb");
    msr(SYS_TCR_EL12, VN(c, V_TCR));
    msr(SYS_TTBR0_EL12, VN(c, V_TTBR0));
    msr(SYS_TTBR1_EL12, VN(c, V_TTBR1));
    msr(SYS_MAIR_EL12, VN(c, V_MAIR));
    msr(VTTBR_EL2, VN(c, V_VTTBR));
    msr(VTCR_EL2, VN(c, V_VTCR));
    msr(HCR_EL2, (hcr & ~(HCR_VM | HCR_DC)) | (VN(c, V_HCR) & (HCR_VM | HCR_DC)));
    msr(SYS_SCTLR_EL12, VN(c, V_SCTLR));
    sysop("isb");
    nv2_at_exec(op, va);
    sysop("isb");
    msr(SYS_SCTLR_EL12, 0);
    msr(HCR_EL2, hcr);
    msr(VTTBR_EL2, vttbr);
    msr(VTCR_EL2, vtcr);
    sysop("isb");
    msr(SYS_TCR_EL12, tcr);
    msr(SYS_TTBR0_EL12, t0);
    msr(SYS_TTBR1_EL12, t1);
    msr(SYS_MAIR_EL12, mair);
    msr(SYS_SCTLR_EL12, sctlr);
    sysop("isb");
}

/* ---------------------------------------------------------------------- */
/* EL2 register accesses that trap (EC 0x18) from L1 */

#define KEY(op0, op1, crn, crm, op2)                                                               \
    (((u64)(op0) << 20) | ((u64)(op2) << 17) | ((u64)(op1) << 14) | ((u64)(crn) << 10) |           \
     ((u64)(crm) << 1))

static void nv2_note_unknown(struct nv2_cpu *c, u64 key, bool read)
{
    c->n_unknown++;
    c->last_unknown = key | ((u64)read << 63);
    nv2_trace(NV2_T_UNKNOWN, key, read, mrs(ELR_EL2), 0, 0, 0);
}

/* L1's ACNTHV_CTL_EL2 write, onto EL1's Apple timer. Prints each core's first one and counts
 * those that turn off a timer whose FIQ is already asserting (EN, not IMASK, ISTATUS). */
static void nv2_agt_ctl_write(u64 value)
{
    static u32 n_first[MAX_CPUS], n_firing;
    u64 old = mrs(SYS_AGT_CNTV_CTL_EL02);
    int cpu = smp_id();

    msr(SYS_AGT_CNTV_CTL_EL02, value);
    sysop("isb");
    bool firing = (old & 7) == 5 && !(value & 1);
    u32 n = firing ? ++n_firing : 0;
    if (!n_first[cpu]++ || (firing && (n <= 16 || !(n & 0xff))))
        printf("NV2: cpu%d ACNTHV_CTL_EL2 <- 0x%lx onto EL1's Apple timer (was 0x%lx, now 0x%lx; "
               "%u masked while firing)\n",
               cpu, value, old, mrs(SYS_AGT_CNTV_CTL_EL02), n_firing);
}

/*
 * Group 3 ID registers under HCR.TID3: the hardware's values, except that
 * ID_AA64MMFR4_EL1 tells L1 the truth about what it runs on here: VHE only
 * (E2H0 = NI_NV1, so it never asks for NV1) and FEAT_NV2 without FEAT_NV
 * (NV_frac = NV2_ONLY). Linux reads these at boot and CPU bring-up only.
 */
#define IDR(crm, op2)                                                                              \
    case (crm) * 8 + (op2):                                                                        \
        return mrs(sys_reg(3, 0, 0, crm, op2));

static u64 nv2_idreg_hw(unsigned crm, unsigned op2)
{
    switch (crm * 8 + op2) {
        IDR(1, 0)
        IDR(1, 1)
        IDR(1, 2)
        IDR(1, 3)
        IDR(1, 4)
        IDR(1, 5)
        IDR(1, 6)
        IDR(1, 7)
        IDR(2, 0)
        IDR(2, 1)
        IDR(2, 2)
        IDR(2, 3)
        IDR(2, 4)
        IDR(2, 5)
        IDR(2, 6)
        IDR(2, 7)
        IDR(3, 0)
        IDR(3, 1)
        IDR(3, 2)
        IDR(3, 3)
        IDR(3, 4)
        IDR(3, 5)
        IDR(3, 6)
        IDR(3, 7)
        IDR(4, 0)
        IDR(4, 1)
        IDR(4, 2)
        IDR(4, 3)
        IDR(4, 4)
        IDR(4, 5)
        IDR(4, 6)
        IDR(4, 7)
        IDR(5, 0)
        IDR(5, 1)
        IDR(5, 2)
        IDR(5, 3)
        IDR(5, 4)
        IDR(5, 5)
        IDR(5, 6)
        IDR(5, 7)
        IDR(6, 0)
        IDR(6, 1)
        IDR(6, 2)
        IDR(6, 3)
        IDR(6, 4)
        IDR(6, 5)
        IDR(6, 6)
        IDR(6, 7)
        IDR(7, 0)
        IDR(7, 1)
        IDR(7, 2)
        IDR(7, 3)
        IDR(7, 4)
        IDR(7, 5)
        IDR(7, 6)
        IDR(7, 7)
        default:
            return 0;
    }
}
#undef IDR

#define MMFR4_NV_FRAC GENMASK(23, 20)
#define MMFR4_E2H0    GENMASK(27, 24)

static u64 nv2_idreg(unsigned crm, unsigned op2)
{
    u64 v = nv2_idreg_hw(crm, op2);
    if (crm == 7 && op2 == 4) /* ID_AA64MMFR4_EL1: report VHE-only, NV2 present */
        v = (v & ~(MMFR4_NV_FRAC | MMFR4_E2H0)) | FIELD_PREP(MMFR4_E2H0, 0xe) |
            FIELD_PREP(MMFR4_NV_FRAC, 1);
    return v;
}

/* The child's timer TVAL views, on its timer in the VNCR page. */
static u64 nv2_tval(struct nv2_cpu *c, bool virt, bool read, u64 value)
{
    u64 now = mrs(CNTPCT_EL0) - (virt ? VN(c, V_CNTVOFF) : 0);
    u64 *cval = &c->vncr[(virt ? V_CNTV_CVAL : V_CNTP_CVAL) / 8];
    if (read)
        return (u32)(*cval - now);
    *cval = now + (s64)(s32)value;
    return 0;
}

static bool nv2_sysreg_do(struct nv2_cpu *c, u64 *regs, u64 esr);

static bool nv2_sysreg(struct nv2_cpu *c, u64 *regs, u64 esr)
{
    if (nv2_sysreg_do(c, regs, esr))
        return true;
    nv2_trace(NV2_T_FAIL, 0x18, esr, mrs(ELR_EL2), regs[(esr >> 5) & 31], 7, 0);
    return false;
}

static bool nv2_sysreg_do(struct nv2_cpu *c, u64 *regs, u64 esr)
{
    u64 key = esr & ESR_ISS & ~(ESR_ISS_MSR_Rt | ESR_ISS_MSR_DIR);
    unsigned rt = (esr >> 5) & 31;
    bool read = esr & 1;
    u64 value = rt == 31 ? 0 : regs[rt];
    unsigned op0 = (esr >> 20) & 3, op1 = (esr >> 14) & 7, crn = (esr >> 10) & 15,
             crm = (esr >> 1) & 15, op2 = (esr >> 17) & 7;
    u64 *slot = NULL;

    c->n_sysreg++;

    if (op0 == 1) {
        if (read)
            return false;
        if (crn == 8 || crn == 9) {
            /* CRn=9 holds the FEAT_XS "NXS" TLBI variants (TLBI *NXS); the
             * invalidation semantics are identical to the CRn=8 forms (NXS only
             * changes which DSB completes them), so emulate them the same way. */
            if (!nv2_tlbi(c, op1, crm, op2, value))
                return false;
        } else if (crn == 7 && (crm == 8 || crm == 9)) {
            bool tge = VN(c, V_HCR) & HCR_TGE;
            if (op1 == 0 && crm == 8 && op2 <= 3)
                nv2_at(c, op2, tge, value);
            else if (op1 == 0 && crm == 9 && op2 <= 1)
                nv2_at(c, 8 + op2, tge, value);
            else if (op1 == 4 && crm == 8 && op2 <= 1)
                nv2_at(c, op2, true, value); /* S1E2R/W: L1's own regime */
            else if (op1 == 4 && crm == 8 && op2 >= 4)
                nv2_at(c, op2, false, value);
            else
                return false;
        } else {
            return false;
        }
        goto done;
    }

    if (op0 == 3 && op1 == 0 && crn == 0 && crm >= 1 && crm <= 7) {
        if (!read)
            return false;
        value = nv2_idreg(crm, op2);
        goto done;
    }

    switch (key) {
        /* VHE aliases: L1's EL2 register is the real EL1 one. */
        case KEY(3, 4, 1, 0, 0):
            if (read)
                value = mrs(SYS_SCTLR_EL12);
            else
                msr(SYS_SCTLR_EL12, value);
            break;
        case KEY(3, 0, 1, 0, 2): /* CPACR_EL1, write-trapped in nested mode */
        case KEY(3, 4, 1, 1, 2): /* CPTR_EL2 (E2H layout) */
            if (read) {
                value = c->nested_mode ? c->vcptr : mrs(SYS_CPACR_EL12);
            } else {
                c->vcptr = value;
                msr(SYS_CPACR_EL12, value);
            }
            break;
        case KEY(3, 4, 1, 2, 6): {
            /* T8140 traps SMCR_EL12 at EL2 while CPACR_EL1 has SME off. */
            u64 cpacr = mrs(SYS_CPACR_EL12);
            msr(SYS_CPACR_EL12, cpacr | CPTR_E2H_SMEN);
            sysop("isb");
            if (read)
                value = mrs(SYS_SMCR_EL12);
            else
                msr(SYS_SMCR_EL12, value);
            msr(SYS_CPACR_EL12, cpacr);
            break;
        }
        case KEY(3, 4, 2, 0, 0):
            if (read)
                value = mrs(SYS_TTBR0_EL12);
            else
                msr(SYS_TTBR0_EL12, value);
            break;
        case KEY(3, 4, 2, 0, 1):
            if (read)
                value = mrs(SYS_TTBR1_EL12);
            else
                msr(SYS_TTBR1_EL12, value);
            break;
        case KEY(3, 4, 2, 0, 2):
            if (read)
                value = mrs(SYS_TCR_EL12);
            else
                msr(SYS_TCR_EL12, value);
            break;
        case KEY(3, 4, 5, 1, 0):
            if (read)
                value = mrs(SYS_AFSR0_EL12);
            else
                msr(SYS_AFSR0_EL12, value);
            break;
        case KEY(3, 4, 5, 1, 1):
            if (read)
                value = mrs(SYS_AFSR1_EL12);
            else
                msr(SYS_AFSR1_EL12, value);
            break;
        case KEY(3, 4, 10, 2, 0):
            if (read)
                value = mrs(SYS_MAIR_EL12);
            else
                msr(SYS_MAIR_EL12, value);
            break;
        case KEY(3, 4, 10, 3, 0):
            if (read)
                value = mrs(SYS_AMAIR_EL12);
            else
                msr(SYS_AMAIR_EL12, value);
            break;
        case KEY(3, 4, 12, 0, 0):
            if (read)
                value = mrs(SYS_VBAR_EL12);
            else
                msr(SYS_VBAR_EL12, value);
            break;
        case KEY(3, 4, 13, 0, 1):
            if (read)
                value = mrs(SYS_CTXIDR_EL12);
            else
                msr(SYS_CTXIDR_EL12, value);
            break;
        /* L1's hyp timers are the real EL1 timers. */
        case KEY(3, 4, 14, 2, 0):
            if (read)
                value = mrs(SYS_CNTP_TVAL02);
            else
                msr(SYS_CNTP_TVAL02, value);
            break;
        case KEY(3, 4, 14, 2, 1):
            if (read)
                value = mrs(SYS_CNTP_CTL02);
            else
                msr(SYS_CNTP_CTL02, value);
            break;
        case KEY(3, 4, 14, 2, 2):
            if (read)
                value = mrs(SYS_CNTP_CVAL02);
            else
                msr(SYS_CNTP_CVAL02, value);
            break;
        case KEY(3, 4, 14, 3, 0):
            if (read)
                value = mrs(SYS_CNTV_TVAL02);
            else
                msr(SYS_CNTV_TVAL02, value);
            break;
        case KEY(3, 4, 14, 3, 1):
            if (read)
                value = mrs(SYS_CNTV_CTL02);
            else
                msr(SYS_CNTV_CTL02, value);
            break;
        case KEY(3, 4, 14, 3, 2):
            if (read)
                value = mrs(SYS_CNTV_CVAL02);
            else
                msr(SYS_CNTV_CVAL02, value);
            break;
        case KEY(3, 4, 14, 1, 0): { /* CNTHCTL_EL2: its EL0 half is CNTKCTL */
            const u64 shared = BIT(17) | GENMASK(9, 0);
            if (read) {
                value = (c->cnthctl & ~shared) | (mrs(SYS_CNTKCTL_EL12) & shared);
            } else {
                c->cnthctl = value;
                msr(SYS_CNTKCTL_EL12, value & shared);
            }
            break;
        }
        /* The child's view through EL12/EL02 encodings that NV2 still traps. */
        case KEY(3, 5, 14, 1, 0):
            slot = &c->cntkctl12;
            break;
        case KEY(3, 5, 14, 2, 0):
            value = nv2_tval(c, false, read, value);
            break;
        case KEY(3, 5, 14, 3, 0):
            value = nv2_tval(c, true, read, value);
            break;
        /* Held for the child. */
        case KEY(3, 4, 1, 0, 1):
            slot = &c->actlr;
            break;
        case KEY(3, 4, 1, 1, 1):
            slot = &c->mdcr;
            break;
        case KEY(3, 4, 1, 1, 7):
            slot = &c->hacr;
            break;
        case KEY(3, 4, 4, 3, 0):
        case KEY(3, 4, 4, 3, 1):
        case KEY(3, 4, 4, 3, 2):
        case KEY(3, 4, 4, 3, 3):
            slot = &c->spsr32[op2];
            break;
        case KEY(3, 4, 6, 0, 4):
            slot = &c->hpfar;
            break;
        case KEY(3, 4, 12, 0, 1): /* RVBAR_EL2 */
            value = 0;
            break;
        case KEY(3, 4, 12, 9, 5):
            slot = &c->sre;
            break;
        case KEY(3, 4, 12, 11, 1):
            if (!read)
                return false;
            value = mrs(SYS_ICH_VTR);
            break;
        case KEY(3, 4, 12, 11, 2):
        case KEY(3, 4, 12, 11, 3):
        case KEY(3, 4, 12, 11, 5):
            if (!read)
                return false;
            value = nv2_gic_status(c, op2);
            break;
        case KEY(3, 1, 15, 13, 4):
            /* AGTCNTHP_CTL from EL2 (XNU's arm_init masks it): a per-CPU shadow. */
            slot = &c->agt_hp_ctl;
            break;
        case KEY(3, 4, 15, 4, 7): /* AMX_CTL_EL2: the guest hypervisor's AMX policy, applied */
            if (read)
                value = nv2_guarded ? sptm_virt_amx_ctl_el2_read() : 0;
            else if (nv2_guarded)
                sptm_virt_amx_ctl_el2_write(value);
            break;
        case KEY(3, 1, 15, 7, 4):
            /* ACNTHV_CTL_EL2: XNU's kernel timer runs at EL2 on bare metal, and L1's hyp timers
             * are the real EL1 timers here, so its control is EL1's Apple timer control. XNU
             * clears EN before a core's offline WFI; left on, that timer's FIQ wakes the WFI
             * forever, the core never powers off and XNU's cluster power-down waits. Qualified
             * on T6050 only; other chips keep the earlier write forward. */
            if (chip_id == T6050) {
                if (read)
                    value = mrs(SYS_AGT_CNTV_CTL_EL02);
                else
                    nv2_agt_ctl_write(value);
            } else if (read) {
                nv2_note_unknown(c, key, read);
                value = 0;
            } else {
                msr(sys_reg(3, 1, 15, 7, 4), value);
            }
            break;
        case KEY(3, 5, 15, 1, 6):
        case KEY(3, 4, 15, 12, 6):
            /* These have only been qualified on T6050. */
            if (chip_id != T6050) {
                nv2_note_unknown(c, key, read);
                value = 0;
                break;
            }
            /* Apple implementation-defined registers (unnamed) that XNU reads and writes from
             * virtual EL2: performed on the hardware, both ways, so a read-modify-write keeps
             * the bits the hardware holds. */
            if (read)
                value = op1 == 5 ? mrs(sys_reg(3, 5, 15, 1, 6)) : mrs(sys_reg(3, 4, 15, 12, 6));
            else if (op1 == 5)
                msr(sys_reg(3, 5, 15, 1, 6), value);
            else
                msr(sys_reg(3, 4, 15, 12, 6), value);
            break;
        case KEY(3, 6, 15, 12, 4):
            /* APSTS_EL1: PAC is up on the hardware; a guest hypervisor (SPTM's __start) polls
             * its bit 0 before it configures APCTL. */
            if (read)
                value = mrs(SYS_IMP_APL_APSTS_EL1);
            break;
        default:
            /* Under SPTM the host's _EL12 view of a per-level register is the store the world
             * switch loads into its EL1 child. */
            if (nv2_guarded && op0 == 3 && (slot = sptm_virt_vel1_slot(op1, crn, crm, op2)))
                break;
            nv2_note_unknown(c, key, read);
            value = 0;
            break;
    }
    if (slot) {
        if (read)
            value = *slot;
        else
            *slot = value;
    }
done:
    if (read && rt != 31)
        regs[rt] = value;
    if (!(op0 == 1 && crn == 8 && op1 == 0 && (VN(c, V_HCR) & HCR_TGE)))
        nv2_trace(NV2_T_SYSREG, esr, value, mrs(ELR_EL2), 0, 0, 0);
    sysop("isb");
    msr(ELR_EL2, mrs(ELR_EL2) + 4);
    return true;
}

/* ---------------------------------------------------------------------- */
/* ERET from L1 (the asm gate in _v_lower_sync takes the common cases) */

static bool nv2_eret(struct nv2_cpu *c)
{
    u64 spsr = mrs(SYS_SPSR_EL12), elr = mrs(SYS_ELR_EL12);
    u64 vhcr = VN(c, V_HCR);
    unsigned m = spsr & 0x1f;

    c->n_eret++;
    nv2_trace(NV2_T_ERET, spsr, elr, vhcr, 0, 0, 0);
    if (m & 0x10) {
        nv2_trace(NV2_T_FAIL, 0x1a, spsr, elr, vhcr, 2, 0); /* AArch32 target */
        return false;
    }
    switch (m) {
        case 0x0:
            if ((vhcr & (HCR_TGE | HCR_E2H)) == (HCR_TGE | HCR_E2H))
                break;
            return nv2_child_enter(c, spsr, elr);
        case 0x8:
        case 0x9:
            spsr -= 4;
            break;
        case 0x4:
        case 0x5:
            if (vhcr & HCR_TGE) {
                nv2_trace(NV2_T_FAIL, 0x1a, spsr, elr, vhcr, 3, 0); /* EL1 with TGE */
                return false;
            }
            return nv2_child_enter(c, spsr, elr);
        default:
            nv2_trace(NV2_T_FAIL, 0x1a, spsr, elr, vhcr, 4, 0);
            return false;
    }
    msr(SPSR_EL2, spsr);
    msr(ELR_EL2, elr);
    return true;
}

/* An exception L1 takes at virtual EL2 from virtual EL2: its current-EL
 * vector (SPx), with its own state recorded in its ELR/SPSR/ESR_EL2. */
static void nv2_inject_self(u64 esr)
{
    u64 elr = mrs(ELR_EL2), spsr = mrs(SPSR_EL2);
    nv2_trace(NV2_T_INJECT, esr, elr, spsr, 0, 0, 0);
    /* It took the exception at virtual EL2: report EL2t/EL2h. */
    u64 vspsr = (spsr & ~0xfUL) | ((spsr & 1) ? 0x9 : 0x8);
    msr(SYS_ELR_EL12, elr);
    msr(SYS_SPSR_EL12, vspsr);
    msr(SYS_ESR_EL12, esr);
    msr(ELR_EL2, mrs(SYS_VBAR_EL12) + ((spsr & 1) ? 0x200 : 0x0));
    msr(SPSR_EL2, nv2_entry_pstate(spsr));
}

/*
 * An exception the guest took at virtual EL2, forwarded by aura's own EL1 vector table: each of
 * its sixteen slots is an `hvc 0x10 + slot`, so the slot says which vector the hardware picked.
 * The guest's state is already where it belongs -- the hardware wrote the EL1 exception registers,
 * which are the guest's vEL2 ones -- so only the vector entry is delivered. Its own VBAR is the
 * one FEAT_NV2 redirected into the VNCR page; the hardware register stays aura's forwarder.
 */
static bool nv2_inject_forwarded(struct nv2_cpu *c, unsigned slot)
{
    u64 vbar = VN(c, V_VBAR);
    static u32 logged;

    if (!vbar) {
        if (logged++ < 4)
            printf("NV2[fwd c%d]: the guest has no vEL2 VBAR yet; its %s at 0x%lx (ESR 0x%lx FAR "
                   "0x%lx) cannot be delivered\n",
                   smp_id(), slot & 3 ? "exception" : "sync fault", mrs(SYS_ELR_EL12),
                   mrs(SYS_ESR_EL12), mrs(SYS_FAR_EL12));
        return false;
    }
    if (logged++ < 8)
        printf("NV2[fwd c%d]: vector slot %u -> 0x%lx (ESR_EL1 0x%lx ELR_EL1 0x%lx FAR_EL1 "
               "0x%lx)\n",
               smp_id(), slot, vbar + slot * 0x80, mrs(SYS_ESR_EL12), mrs(SYS_ELR_EL12),
               mrs(SYS_FAR_EL12));
    msr(ELR_EL2, vbar + slot * 0x80);
    msr(SPSR_EL2, nv2_entry_pstate(mrs(SPSR_EL2)));
    sysop("isb");
    return true;
}

bool nv2_handle_sync(u64 *regs)
{
    struct nv2_cpu *c = nv2_this();
    u64 esr = mrs(ESR_EL2);
    u32 ec = esr >> 26;

    if (!c->armed || (mrs(SPSR_EL2) & 0xc) == 0x8) /* aura's own */
        return false;
    if (c->child)
        return nv2_child_sync(c, esr);

    /* From L1. Its userspace (EL0) runs with real TGE clear and reaches EL2
     * only through traps L1 itself never asked for. */
    if ((mrs(SPSR_EL2) & 0xc) == 0x0) {
        /* From L1's userspace: what L1 would have taken at its EL1 with
         * its TGE set (e.g. an ID register read under TID3). */
        u64 elr = mrs(ELR_EL2), spsr = mrs(SPSR_EL2);
        nv2_trace(NV2_T_INJECT, esr, elr, spsr, 1, 0, 0);
        msr(SYS_ELR_EL12, elr);
        msr(SYS_SPSR_EL12, spsr);
        msr(SYS_ESR_EL12, esr);
        msr(SYS_FAR_EL12, mrs(FAR_EL2));
        msr(ELR_EL2, mrs(SYS_VBAR_EL12) + 0x400);
        msr(SPSR_EL2, nv2_entry_pstate(spsr));
        return true;
    }

    switch (ec) {
        case ESR_EC_DABORT_LOWER:
            /* A stage-2 fault on one of the pages aura traps (sptm_virt_s2.c). */
            return nv2_l1_vttbr && sptm_virt_s2_dabort(regs);
        case 0x1a:
            return nv2_eret(c);
        case ESR_EC_MSR:
            return nv2_sysreg(c, regs, esr);
        case ESR_EC_HVC:
            /* hvc #1 ends a proxy probe; outside one it is the guest's own and
             * is reflected to it like any other hvc, never sent to the proxy
             * return path. */
            if ((esr & 0xffff) == 1 && nv2_probe_active)
                return false;
            /* 0x10..0x1f: aura's EL1 vector table forwarding an exception of the guest's own. */
            if (((esr & 0xffff) - 0x10) < 16)
                return nv2_inject_forwarded(c, (esr & 0xffff) - 0x10);
            nv2_inject_self(esr);
            return true;
        default:
            nv2_trace(NV2_T_FAIL, ec, esr, mrs(ELR_EL2), mrs(SPSR_EL2), 5, 0);
            return false;
    }
}

void nv2_note_unhandled(u64 esr, u64 elr, u64 spsr)
{
    /* The arguments feed only the trace ring, which a non-NV2_TRACE build
     * compiles away. */
    UNUSED(esr);
    UNUSED(elr);
    UNUSED(spsr);
    nv2_trace(NV2_T_FAIL, esr >> 26, esr, elr, spsr, 0xdead, mrs(FAR_EL2));
}

bool nv2_handle_async(u64 *regs, int kind)
{
    UNUSED(regs);
    if (!nv2_active)
        return false;
    struct nv2_cpu *c = nv2_this();
    if (!c->armed || !c->child || (mrs(SPSR_EL2) & 0xc) == 0x8)
        return false;
    c->n_irq++;
    nv2_child_exit(c, kind, 0);
    return true;
}

/* ---------------------------------------------------------------------- */
/* Arming a CPU */

static void nv2_cpu_arm(struct nv2_cpu *c, u64 vncr, u64 avncr, u64 hcr)
{
    /* A guarded guest's core re-arms on every warm reset. Keep its EL1 child's CNTKCTL: nothing
     * else holds it, SPTM has no restore for it and cL4 writes it only at its cold start, so
     * zeroing it takes EL0's counter access away from cL4's tasks after a sleep. */
    u64 cntkctl12 = nv2_guarded && c->armed ? c->cntkctl12 : 0;
    memset(c, 0, sizeof(*c));
    c->cntkctl12 = cntkctl12;
    c->vncr = (u64 *)vncr;
    c->avncr = (u64 *)avncr;
    c->hcr = hcr;
    c->sre = 0xf;
    c->cnthctl = CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN;
    c->l1_hfgrtr = mrs(SYS_HFGRTR_EL2);
    c->l1_hfgwtr = mrs(SYS_HFGWTR_EL2);
    c->l1_hfgitr = HFGITR_VMALLE1_ALL;
    c->l1_hdfgrtr = mrs(SYS_HDFGRTR_EL2);
    c->l1_hdfgwtr = mrs(SYS_HDFGWTR_EL2);
    c->l1_hcrx = mrs(SYS_HCRX_EL2);
    c->l1_cptr = mrs(SYS_CPTR_EL2);
    c->l1_cnthctl = mrs(CNTHCTL_EL2);
    nv2_ich_lrs = (mrs(SYS_ICH_VTR) & 0x1f) + 1;
    c->armed = true;
}

__attribute__((used, retain)) u64 nv2_probe_arm(u64 vncr, u64 avncr, u64 hcr)
{
    nv2_cpu_arm(nv2_this(), vncr, avncr, hcr);
    nv2_this()->scratch = (u64 *)(avncr + SZ_4K);
    msr(SYS_HFGITR_EL2, HFGITR_VMALLE1_ALL);
    nv2_probe_active = true;
    nv2_active = true;
    return (u64)nv2_this();
}

__attribute__((used, retain)) void nv2_probe_disarm(void)
{
    nv2_this()->armed = false;
    nv2_probe_active = false;
    msr(SYS_HFGITR_EL2, 0);
}

void nv2_enter_el1h(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));

/* The EL2 state under an OS at virtual EL2, before nv2_enter_os adds the NV2 traps. */
static void nv2_el2_setup(void)
{
    /* No stage 2. IRQ, FIQ, SError and external aborts all go to EL1, and an SMC is not trapped:
     * with no EL3 it is UNDEFINED, as on the bare machine. */
    reg_clr(HCR_EL2, HCR_TGE | HCR_AMO | HCR_IMO | HCR_FMO | HCR_TEA | HCR_TSC | HCR_VM | HCR_NV |
                         HCR_NV1 | HCR_NV2);
    msr(VTTBR_EL2, 0);
    msr(HACR_EL2, 0);
    msr(HSTR_EL2, 0);
    msr(MDCR_EL2, 0);

    /* With TGE clear, EL1 reads these in place of MIDR_EL1 and MPIDR_EL1. */
    msr(VPIDR_EL2, mrs(MIDR_EL1));
    msr(VMPIDR_EL2, mrs(MPIDR_EL1));

    msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);
    msr(CNTVOFF_EL2, 0);

    u64 cptr = CPTR_E2H_FPEN;
    u64 hfg = 0;
    if (FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1))) {
        cptr |= CPTR_E2H_SMEN;
        hfg |= HFGxTR_nSMPRI_EL1 | HFGxTR_nTPIDR2_EL0;
    }
    msr(SYS_CPTR_EL2, cptr);
    sysop("isb");

    if (hfg) {
        u64 smcr = SMCR_LEN;
        if (mrs(SYS_ID_AA64SMFR0_EL1) & ID_AA64SMFR0_FA64)
            smcr |= SMCR_FA64;
        if (FIELD_GET(ID_AA64SMFR0_SMEVER, mrs(SYS_ID_AA64SMFR0_EL1)))
            smcr |= SMCR_EZT0;
        msr(SYS_SMCR_EL2, smcr);
        if (mrs(SYS_SMIDR_EL1) & SMIDR_SMPS)
            msr(SYS_SMPRIMAP_EL2, 0);
    }

    /* Fine-grained traps: the "n" bits trap when clear. */
    msr(SYS_HFGRTR_EL2, hfg);
    msr(SYS_HFGWTR_EL2, hfg);
    msr(SYS_HFGITR_EL2, 0);
    msr(SYS_HDFGRTR_EL2, 0);
    msr(SYS_HDFGWTR_EL2, 0);

    u64 mmfr1 = mrs(SYS_ID_AA64MMFR1_EL1);
    if (FIELD_GET(ID_AA64MMFR1_HCX, mmfr1))
        msr(SYS_HCRX_EL2, 0);
    if (FIELD_GET(ID_AA64MMFR1_LO, mmfr1))
        msr(SYS_LORC_EL1, 0);

    sysop("isb");
}

/*
 * Enter the OS at virtual EL2. On top of nv2_el2_setup's passthrough, NV|NV2
 * redirect the OS's EL2 register accesses to this CPU's VNCR/AVNCR pages and
 * trap its ERETs, and HCR.AT and the VMALLE1 FGT traps let aura run those
 * under a guest's context. The EL2 MMU stays on so the redirected accesses
 * are cacheable.
 */
void nv2_enter_os(u64 entry, u64 a0, u64 a1, u64 a2)
{
    nv2_el2_setup();
    if (!(mrs(SCTLR_EL1) & SCTLR_M))
        mmu_init_secondary_local();

    int cpu = smp_id();
    if (!nv2_pages[cpu])
        panic("NV2: CPU %d has no NV pages\n", cpu);
    u64 vncr = (u64)nv2_pages[cpu], avncr = vncr + SZ_4K;
    memset(nv2_pages[cpu], 0, SZ_16K);
    ((u64 *)vncr)[V_HCR / 8] = HCR_RW | HCR_E2H;

    /* A guarded guest (SPTM) reads its real IDs and runs GXF/SPRR natively, so
     * it takes neither the ID-register trap nor the ID_AA64MMFR4 NV2-only
     * rewrite the Linux guest needs. The NV|NV2 redirection and ERET gate stay. */
    u64 hcr = (mrs(HCR_EL2) & ~(HCR_TGE | HCR_NV1 | HCR_TTLB)) | HCR_NV | HCR_NV2 | HCR_AT |
              (nv2_guarded ? 0 : HCR_TID3);
    /* A guarded guest on an MTE SoC tags its memory: let EL1 reach the allocation tags. */
    if (nv2_guarded && sptm_virt_mte())
        hcr |= HCR_ATA;
    nv2_cpu_arm(&nv2_cpus[cpu], vncr, avncr, hcr);
    nv2_cpus[cpu].scratch = (u64 *)(vncr + 2 * SZ_4K);

    msr(VNCR_EL2, vncr);
    msr(AVNCR_EL2, avncr);
    msr(SYS_HFGITR_EL2, HFGITR_VMALLE1_ALL);
    msr(SYS_ICH_HCR, 0);
    msr(CNTP_CTL_EL0, 0);
    msr(HCR_EL2, hcr);
    sysop("isb");

    /* The virtualized SPTM: its per-core state, the last thing before the ERET. */
    if (nv2_guarded)
        sptm_virt_cpu_arm();

    nv2_enter_el1h(entry, a0, a1, a2);
}
