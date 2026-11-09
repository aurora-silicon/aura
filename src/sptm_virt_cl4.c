/* SPDX-License-Identifier: MIT */

/*
 * The cL4 secure kernel under the virtualized SPTM.
 *
 * Three groups of software share a core, and aura switches between them:
 *
 *   HOST  - SPTM, TXM and XNU. On real hardware they share the EL2 regime (SPTM at GL2, XNU at
 *           EL2); here they run at physical EL1/GL1 as virtual EL2.
 *   CL4   - SPTM's guarded child, the cL4 secure kernel (GL1/EL1 on real hardware), entered by
 *           SPTM's guarded ERET to EL1 and left by any exception it takes to the level above.
 *   GUEST - XNU's normal-world child (a VM, EL1), entered and left in nv2.c.
 *
 * On real hardware there is one EL1 copy of every register, which the parents multiplex through
 * the VNCR page (architected registers) and the _EL12/_GL12 aliases (Apple's per-level ones).
 * Under aura SPTM's own writes to its per-level registers, write-once lock bits included, land on
 * the physical EL1 copies the child runs on, while its _EL12 writes for the child trap and are
 * shadowed. So aura switches these registers with the world, from GL2 through the _EL12/_GL12
 * aliases, which can rewrite the locks: entering a child saves the host's live values and loads
 * the child's shadowed ones; leaving it stores the child's live values back into the shadow and
 * restores the host's.
 */

#include "sptm_virt.h"
#include "cpu_regs.h"
#include "gxf.h"
#include "nv2.h"
#include "smp.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "utils.h"

enum { SPTM_WORLD_HOST, SPTM_WORLD_CL4, SPTM_WORLD_GUEST };

/* The per-level registers that follow the world. VMSA_LOCK is last: it locks the EL1 VBAR and
 * TTBR1, so it is applied after everything else. */
#define CL4_GCFG_REGS(X)                                                                           \
    X(0, "APCTL_EL12", 6, 15, 0)                                                                   \
    X(1, "GXF_CONFIG_EL12", 6, 15, 1)                                                              \
    X(2, "GXF_ENTRY_EL12", 6, 15, 2)                                                               \
    X(3, "GXF_PABENTRY_EL12", 6, 15, 3)                                                            \
    X(4, "SPRR_CONFIG_EL12", 6, 15, 4)                                                             \
    X(5, "SPRR_AMRANGE_EL12", 6, 15, 5)                                                            \
    X(6, "SPRR_PPERM_EL12", 6, 15, 7)                                                              \
    X(7, "SPRR_UPERM_EL02", 4, 5, 2)                                                               \
    X(8, "SPRR_UMPRR_EL12", 4, 8, 0)                                                               \
    X(9, "SPRR_UPERM_SH1_EL12", 4, 8, 1)                                                           \
    X(10, "SPRR_UPERM_SH2_EL12", 4, 8, 2)                                                          \
    X(11, "SPRR_UPERM_SH3_EL12", 4, 8, 3)                                                          \
    X(12, "SPRR_PMPRR_EL12", 6, 6, 0)                                                              \
    X(13, "SPRR_PPERM_SH1_EL12", 6, 6, 1)                                                          \
    X(14, "SPRR_PPERM_SH2_EL12", 6, 6, 2)                                                          \
    X(15, "SPRR_PPERM_SH3_EL12", 6, 6, 3)                                                          \
    X(16, "APIAKeyLo_EL12", 6, 7, 0)                                                               \
    X(17, "APIAKeyHi_EL12", 6, 7, 1)                                                               \
    X(18, "APIBKeyLo_EL12", 6, 7, 2)                                                               \
    X(19, "APIBKeyHi_EL12", 6, 7, 3)                                                               \
    X(20, "APDAKeyLo_EL12", 6, 7, 4)                                                               \
    X(21, "APDAKeyHi_EL12", 6, 7, 5)                                                               \
    X(22, "APDBKeyLo_EL12", 6, 7, 6)                                                               \
    X(23, "APDBKeyHi_EL12", 6, 7, 7)                                                               \
    X(24, "APGAKeyLo_EL12", 6, 2, 1)                                                               \
    X(25, "APGAKeyHi_EL12", 6, 2, 2)                                                               \
    X(26, "KERNKEYLO_EL12", 6, 2, 3)                                                               \
    X(27, "KERNKEYHI_EL12", 6, 2, 4)                                                               \
    X(28, "AGTCNTRDIR_EL12", 4, 14, 6)                                                             \
    X(29, "AMX_CONFIG_EL12", 4, 4, 6)                                                              \
    X(30, "VMSA_LOCK_EL12", 4, 2, 6)

struct cl4_gcfg_reg {
    const char *name;
    u8 op1, crm, op2;
};

static const struct cl4_gcfg_reg cl4_gcfg_regs[] = {
#define X(i, n, o1, cm, o2) {n, o1, cm, o2},
    CL4_GCFG_REGS(X)
#undef X
};
#define CL4_GCFG_N    ARRAY_SIZE(cl4_gcfg_regs)
#define CL4_GCFG_VMSA (CL4_GCFG_N - 1)
#define CL4_VMSA_LOCK "s3_4_c15_c2_6"

#define CL4_KEY(op1, crm, op2) ((u16)((3 << 14) | ((op1) << 11) | (15 << 7) | ((crm) << 3) | (op2)))
#define CL4_KEY_VBAR_GL12      CL4_KEY(6, 9, 2)
#define CL4_KEY_SP_GL12        CL4_KEY(6, 10, 0)

struct sptm_world_cpu {
    u8 world;
    u32 seq; /* world writes, mirrored with the flag in CONTEXTIDR_EL2 (sptm_world_flag) */
    bool applied[CL4_GCFG_N]; /* register loaded from the shadow for this child */
    u64 host_lvl[CL4_GCFG_N]; /* the host's live values while a child runs */
    u64 host_vbar_gl12;       /* SPTM's guarded vector and stack (physical GL1 bank) */
    u64 host_sp_gl12;
    /* The guarded exception bank (SPSR/ASPSR/ESR/ELR/FAR/AFSR1 at GL): one physical copy here,
     * GL2's and GL1's on real hardware, so SPTM's and cL4's are kept apart. */
    u64 host_glx[6], cl4_glx[6];
    bool cl4_glx_valid;
};

static struct sptm_world_cpu sptm_world[MAX_CPUS];

static void glx_save(u64 *b)
{
    b[0] = mrs(SYS_IMP_APL_SPSR_GL12);
    b[1] = mrs(SYS_IMP_APL_ASPSR_GL12);
    b[2] = mrs(SYS_IMP_APL_ESR_GL12);
    b[3] = mrs(SYS_IMP_APL_ELR_GL12);
    b[4] = mrs(SYS_IMP_APL_FAR_GL12);
    b[5] = mrs(SYS_IMP_APL_AFSR1_GL12);
}

static void glx_load(const u64 *b)
{
    msr(SYS_IMP_APL_SPSR_GL12, b[0]);
    msr(SYS_IMP_APL_ASPSR_GL12, b[1]);
    msr(SYS_IMP_APL_ESR_GL12, b[2]);
    msr(SYS_IMP_APL_ELR_GL12, b[3]);
    msr(SYS_IMP_APL_FAR_GL12, b[4]);
    msr(SYS_IMP_APL_AFSR1_GL12, b[5]);
}

/*
 * The world flag, mirrored in a register only aura uses (CONTEXTIDR_EL2: the guest's EL2 copy is
 * its VNCR page under NV2, its EL1 one is CONTEXTIDR_EL12). aura's .bss has been seen to lose
 * writes while this guest runs: the flag read "cL4" with SPTM's HCR, vectors and XNU's EL1 bank
 * all live and no cL4 entry in the last 64 transitions, and aura then delivered SPTM's own trap
 * to SPTM as a cL4 exception -- the "[TXM] Unhandled synchronous exception taken from GL0" halt.
 * The register cannot lose a write: every read compares the two, reports a loss once with how
 * stale the memory is, and continues from the register.
 */
#define CTXIDR_EL2 s3_4_c13_c0_1
void sptm_virt_world_dump(void);

static void world_set(struct sptm_world_cpu *w, u8 world)
{
    w->world = world;
    w->seq++;
    msr(CTXIDR_EL2, ((u64)(w->seq & 0xffffff) << 8) | world);
}

static u8 sptm_world_flag(int cpu)
{
    static u32 losses;
    struct sptm_world_cpu *w = &sptm_world[cpu];
    u64 r = mrs(CTXIDR_EL2) & 0xffffffff;
    u64 m = ((u64)(w->seq & 0xffffff) << 8) | w->world;

    if (r != m) {
        if (losses++ < 16)
            printf("SPTM-virt[world LOST c%d]: memory says %u (seq %u) at %p, the register %lu "
                   "(seq %lu): %ld writes gone\n",
                   cpu, w->world, w->seq & 0xffffff, w, r & 0xff, r >> 8,
                   (long)(r >> 8) - (long)(w->seq & 0xffffff));
        if (losses == 1)
            sptm_virt_world_dump();
        w->world = r & 0xff;
        w->seq = r >> 8;
    }
    return w->world;
}

/* At every guest entry on a core (warm resets included): the register starts UNKNOWN. */
void sptm_virt_world_arm(int cpu)
{
    struct sptm_world_cpu *w = &sptm_world[cpu];
    msr(CTXIDR_EL2, ((u64)(w->seq & 0xffffff) << 8) | w->world);
}

bool sptm_virt_in_cl4(void)
{
    return sptm_world_flag(smp_id()) == SPTM_WORLD_CL4;
}

/*
 * The last world transitions per core, for when aura's flag and the code that runs disagree: the
 * console drops lines under load, so a ring printed once at the mismatch is the record.
 */
#define WORLD_HIST      64
#define WORLD_HIST_CPUS 12 /* aura's image must end below iBoot's SEPFW: keep .bss small */
struct world_ev {
    u64 a, b;
    u32 t; /* CNTPCT, low half: deltas within ~178 s */
    u8 kind, world;
};
static struct world_ev world_hist[WORLD_HIST_CPUS][WORLD_HIST];
static u32 world_hist_n[WORLD_HIST_CPUS];

static const char *const world_ev_names[] = {
    [SPTM_WEV_GERET] = "geret (target, pstate)",
    [SPTM_WEV_GEXIT] = "gexit (pc, target)",
    [SPTM_WEV_CL4_ENTER] = "cl4 enter (target)",
    [SPTM_WEV_CL4_SYNC] = "cl4 -> sptm sync (esr, elr)",
    [SPTM_WEV_CL4_FIQ] = "cl4 -> sptm fiq (-, elr)",
    [SPTM_WEV_CL4_ERET2] = "cl4 -> host by eret to GL2 (target)",
    [SPTM_WEV_CHILD_ENTER] = "nv2 child enter",
    [SPTM_WEV_CHILD_EXIT] = "nv2 child exit",
};

void sptm_virt_world_ev(unsigned kind, u64 a, u64 b)
{
    int cpu = smp_id();
    if (cpu >= WORLD_HIST_CPUS)
        return;
    struct world_ev *e = &world_hist[cpu][world_hist_n[cpu]++ % WORLD_HIST];

    e->t = mrs(CNTPCT_EL0);
    e->a = a;
    e->b = b;
    e->kind = kind;
    e->world = sptm_world_flag(cpu);
}

void sptm_virt_world_dump(void)
{
    static bool done[MAX_CPUS];
    static const char wn[] = "HCG";
    int cpu = smp_id();
    struct sptm_world_cpu *w = &sptm_world[cpu];

    if (cpu >= WORLD_HIST_CPUS || done[cpu])
        return;
    done[cpu] = true;
    printf("SPTM-virt[world dump c%d]: flag %c seq %u (register 0x%lx) HCR_EL2 0x%lx VBAR_GL12 "
           "0x%lx (host 0x%lx) SP_GL12 0x%lx VBAR_EL12 0x%lx TTBR1_EL12 0x%lx; %u events\n",
           cpu, wn[w->world], w->seq, mrs(CTXIDR_EL2), mrs(HCR_EL2), mrs(SYS_IMP_APL_VBAR_GL12),
           w->host_vbar_gl12, mrs(SYS_IMP_APL_SP_GL12), mrs(SYS_VBAR_EL12), mrs(SYS_TTBR1_EL12),
           world_hist_n[cpu]);
    sptm_virt_world_ring(cpu, 64, debug_printf);
}

/* The last n events of a core's ring, through pf (another core's watchdog passes a lock-free
 * printer: the dead core may hold the console lock). */
void sptm_virt_world_ring(int cpu, unsigned last, int (*pf)(const char *, ...))
{
    static const char wn[] = "HCG?";
    if (cpu < 0 || cpu >= WORLD_HIST_CPUS)
        return;
    u32 now = mrs(CNTPCT_EL0);
    u64 us = mrs(CNTFRQ_EL0) / 1000000;
    u32 n = world_hist_n[cpu];
    if (last > WORLD_HIST)
        last = WORLD_HIST;
    u32 first = n > last ? n - last : 0;
    for (u32 i = first; i < n; i++) {
        struct world_ev *e = &world_hist[cpu][i % WORLD_HIST];
        pf("SPTM-virt[world ring c%d]: #%u -%lu us [%c] %s 0x%lx 0x%lx\n", cpu, i,
           (u64)(u32)(now - e->t) / (us ? us : 1), wn[e->world & 3],
           e->kind < ARRAY_SIZE(world_ev_names) ? world_ev_names[e->kind] : "?", e->a, e->b);
    }
}

#define CL4_STR2(x)         #x
#define CL4_ENC(o1, cm, o2) "s3_" CL4_STR2(o1) "_c15_c" CL4_STR2(cm) "_" CL4_STR2(o2)

static u64 cl4_gcfg_rd(unsigned i)
{
    switch (i) {
#define X(idx, n, o1, cm, o2)                                                                      \
    case idx:                                                                                      \
        return SAFE_MRS(CL4_ENC(o1, cm, o2));
        CL4_GCFG_REGS(X)
#undef X
    }
    return SAFE_MRS_FAILED;
}

/* True if the write faulted. */
static bool cl4_gcfg_wr(unsigned i, u64 v)
{
    switch (i) {
#define X(idx, n, o1, cm, o2)                                                                      \
    case idx:                                                                                      \
        return SAFE_MSR(CL4_ENC(o1, cm, o2), v);
        CL4_GCFG_REGS(X)
#undef X
    }
    return true;
}

static u64 *cl4_slot(int cpu, unsigned i)
{
    const struct cl4_gcfg_reg *r = &cl4_gcfg_regs[i];
    return sptm_virt_gl2_slot(cpu, CL4_KEY(r->op1, r->crm, r->op2));
}

/*
 * Before SPTM's first instruction on a core the physical EL1 copies still hold iBoot's EL1 state:
 * that is the child's starting view, which SPTM's own _EL12 writes then override as on hardware.
 * On a warm reset the shadow is the only copy of what cL4 ran with (SPTM's save and restore of
 * these never reach it), so it is kept.
 */
void sptm_virt_cl4_seed(int cpu)
{
    if (sptm_virt_cpu_entries[cpu] > 1)
        return;
    for (unsigned i = 0; i < CL4_GCFG_N; i++) {
        u64 v = cl4_gcfg_rd(i);
        if (v != SAFE_MRS_FAILED)
            *cl4_slot(cpu, i) = v;
    }
    printf("SPTM-virt[cl4]: seeded the EL1-world per-level state on cpu%d (APCTL 0x%lx VMSA_LOCK "
           "0x%lx GXF_CONFIG 0x%lx SPRR_CONFIG 0x%lx)\n",
           cpu, *cl4_slot(cpu, 0), *cl4_slot(cpu, CL4_GCFG_VMSA), *cl4_slot(cpu, 1),
           *cl4_slot(cpu, 4));
}

/*
 * The per-level half of a host <-> EL1-child switch, in two steps around the EL1 bank swap: the
 * VMSA lock is dropped before the bank changes hands and applied last.
 */
/*
 * Who changes the EL1 world's pointer-authentication keys, and when.
 *
 * XNU runs with HCR_EL2.APK set, so its own key writes reach the hardware; aura's per-level
 * bank applies the shadow of SPTM's _EL12 writes at every world entry. If one transition
 * saves the live keys and another does not, the hardware key stops matching what the kernel
 * signed with, and the kernel panics authenticating a return address ("PAC failure from kernel
 * with IB key while returning"). This reports every change with the transition that saw it.
 */
static u64 pac_last[MAX_CPUS][4];
static u32 pac_logs;

void sptm_virt_pac_note(const char *where)
{
    int cpu = smp_id();
    /* The kernel's own returns depend on more than the IB key: APCTL carries Apple's key
     * diversification and KERNKEY is the kernel's own diversifier. */
    u64 v[4] = {SAFE_MRS(CL4_ENC(6, 7, 2)), SAFE_MRS(CL4_ENC(6, 15, 0)), SAFE_MRS(CL4_ENC(6, 2, 3)),
                SAFE_MRS(CL4_ENC(6, 2, 4))};

    if (!memcmp(v, pac_last[cpu], sizeof(v)))
        return;
    memcpy(pac_last[cpu], v, sizeof(v));
    if (pac_logs++ < 64)
        printf("SPTM-virt[pac]: cpu%d IB 0x%lx APCTL 0x%lx KERNKEY 0x%lx:0x%lx at %s\n", cpu, v[0],
               v[1], v[2], v[3], where);
}

void sptm_virt_world_child_pre_enter(void)
{
    struct sptm_world_cpu *w = &sptm_world[smp_id()];

    sptm_virt_pac_note("child pre-enter (the host's own)");
    for (unsigned i = 0; i < CL4_GCFG_N; i++)
        w->host_lvl[i] = cl4_gcfg_rd(i);
    SAFE_MSR(CL4_VMSA_LOCK, 0);
}

static void sptm_virt_world_post_enter(u8 world)
{
    int cpu = smp_id();
    struct sptm_world_cpu *w = &sptm_world[cpu];

    for (unsigned i = 0; i < CL4_GCFG_N; i++) {
        w->applied[i] = false;
        if (w->host_lvl[i] != SAFE_MRS_FAILED)
            w->applied[i] = !cl4_gcfg_wr(i, *cl4_slot(cpu, i));
    }
    if (!w->applied[CL4_GCFG_VMSA]) /* not switched: put the host's back */
        SAFE_MSR(CL4_VMSA_LOCK, w->host_lvl[CL4_GCFG_VMSA]);
    sptm_virt_amx_child();
    sysop("isb");
    world_set(w, world);
    sptm_virt_pac_note(world == SPTM_WORLD_CL4 ? "cL4 entered" : "child entered");
}

void sptm_virt_world_child_post_enter(void)
{
    sptm_virt_world_ev(SPTM_WEV_CHILD_ENTER, mrs(SYS_ELR_EL12), 0);
    sptm_virt_world_post_enter(SPTM_WORLD_GUEST);
}

static void sptm_virt_world_child_pre_exit_cl4(void);

void sptm_virt_world_child_pre_exit(void)
{
    sptm_virt_world_ev(SPTM_WEV_CHILD_EXIT, 0, 0);
    sptm_virt_world_child_pre_exit_cl4();
}

static void sptm_virt_world_child_pre_exit_cl4(void)
{
    int cpu = smp_id();
    struct sptm_world_cpu *w = &sptm_world[cpu];

    for (unsigned i = 0; i < CL4_GCFG_N; i++) {
        if (!w->applied[i])
            continue;
        u64 v = cl4_gcfg_rd(i); /* what the parent's _EL12 read must see */
        if (v != SAFE_MRS_FAILED)
            *cl4_slot(cpu, i) = v;
    }
    sptm_virt_pac_note("child pre-exit (the child's own)");
    SAFE_MSR(CL4_VMSA_LOCK, 0);
}

void sptm_virt_world_child_post_exit(void)
{
    struct sptm_world_cpu *w = &sptm_world[smp_id()];

    for (unsigned i = 0; i < CL4_GCFG_N; i++) {
        if (!w->applied[i] && i != CL4_GCFG_VMSA)
            continue;
        bool f = cl4_gcfg_wr(i, w->host_lvl[i]);
        if (f || cl4_gcfg_rd(i) != w->host_lvl[i])
            printf("SPTM-virt[world.lvl]: %s restore to host 0x%lx FAILED (fault=%d rb 0x%lx)\n",
                   cl4_gcfg_regs[i].name, w->host_lvl[i], (int)f, cl4_gcfg_rd(i));
        w->applied[i] = false;
    }
    sptm_virt_amx_host();
    sysop("isb");
    world_set(w, SPTM_WORLD_HOST);
    sptm_virt_pac_note("host restored");
}

/* XNU's trapped _EL12 accesses to a per-level register belong in the same store. */
u64 *sptm_virt_vel1_slot(unsigned op1, unsigned crn, unsigned crm, unsigned op2)
{
    if (crn != 15)
        return NULL;
    for (unsigned i = 0; i < CL4_GCFG_N; i++)
        if (cl4_gcfg_regs[i].op1 == op1 && cl4_gcfg_regs[i].crm == crm &&
            cl4_gcfg_regs[i].op2 == op2)
            return cl4_slot(smp_id(), i);
    return NULL;
}

/* HOST -> CL4, at SPTM's guarded ERET to EL1. cL4 also gets the guarded vector and stack SPTM
 * wrote for it (VBAR_GL12/SP_GL12, shadowed); the host's are kept. */
void sptm_virt_cl4_world_enter(u64 target)
{
    static u32 logs;
    int cpu = smp_id();
    struct sptm_world_cpu *w = &sptm_world[cpu];

    sptm_virt_world_ev(SPTM_WEV_CL4_ENTER, target, 0);
    w->host_vbar_gl12 = mrs(SYS_IMP_APL_VBAR_GL12);
    w->host_sp_gl12 = mrs(SYS_IMP_APL_SP_GL12);
    glx_save(w->host_glx);
    if (w->cl4_glx_valid)
        glx_load(w->cl4_glx); /* cL4's own exception state, e.g. a user return it holds */
    sptm_virt_world_child_pre_enter();
    nv2_guarded_child_enter();
    /* A cold entry has no vector yet: aim a zero one at cL4's table so a fault before it sets
     * its own is visible. */
    u64 vbar = *sptm_virt_gl2_slot(cpu, CL4_KEY_VBAR_GL12);
    if (!vbar)
        vbar = target + 0x800;
    msr(SYS_IMP_APL_VBAR_GL12, vbar);
    msr(SYS_IMP_APL_SP_GL12, *sptm_virt_gl2_slot(cpu, CL4_KEY_SP_GL12));
    sptm_virt_world_post_enter(SPTM_WORLD_CL4);
    if (logs++ < 8)
        printf("SPTM-virt[world]: cpu%d host -> cL4 at 0x%lx (VBAR_GL12 0x%lx SP_GL12 0x%lx)\n",
               cpu, target, vbar, mrs(SYS_IMP_APL_SP_GL12));
}

/* CL4 -> HOST: cL4's live state back where SPTM reads it, the host's restored. */
void sptm_virt_cl4_world_exit(void)
{
    int cpu = smp_id();
    struct sptm_world_cpu *w = &sptm_world[cpu];

    *sptm_virt_gl2_slot(cpu, CL4_KEY_VBAR_GL12) = mrs(SYS_IMP_APL_VBAR_GL12);
    *sptm_virt_gl2_slot(cpu, CL4_KEY_SP_GL12) = mrs(SYS_IMP_APL_SP_GL12);
    glx_save(w->cl4_glx);
    w->cl4_glx_valid = true;
    sptm_virt_world_child_pre_exit_cl4();
    nv2_guarded_child_exit();
    msr(SYS_IMP_APL_VBAR_GL12, w->host_vbar_gl12);
    msr(SYS_IMP_APL_SP_GL12, w->host_sp_gl12);
    glx_load(w->host_glx);
    sptm_virt_world_child_post_exit();
    sysop("isb");
}

/*
 * An exception cL4 takes to the level above (its HVC calls into SPTM, the traps SPTM configured),
 * or an FIQ that preempts it, arrives at aura's GL2; on real hardware it enters SPTM at GL2.
 * Switch back to SPTM's world, put the syndrome in its guarded exception registers (the physical
 * GL1 bank, _GL12 from here) and enter its GL vector at the lower-EL slot.
 */
static bool cl4_deliver(u64 *regs, u64 esr, bool fiq)
{
    static u32 n;
    struct sptm_world_cpu *w = &sptm_world[smp_id()];

    if (sptm_world_flag(smp_id()) != SPTM_WORLD_CL4)
        return false;
    u64 elr = mrs(ELR_EL1); /* cL4's return PC, mirrored from GL2's bank */
    u64 spsr = mrs(SYS_IMP_APL_SPSR_GL1);
    u64 aspsr = mrs(SYS_IMP_APL_ASPSR_GL1);
    u64 far = mrs(SYS_IMP_APL_FAR_GL1);

    /* cL4's no-return calls into SPTM (x16[39:32] = 0xfe) are its panic reports. */
    if (!fiq && ((esr >> 26) & 0x3f) == ESR_EC_HVC && ((regs[16] >> 32) & 0xff) == 0xfe)
        printf("SPTM-virt[cl4.panic]: no-return call x16=0x%lx x0=0x%lx\n", regs[16], regs[0]);

    sptm_virt_world_ev(fiq ? SPTM_WEV_CL4_FIQ : SPTM_WEV_CL4_SYNC, esr, elr);
    sptm_virt_cl4_world_exit();

    if (!fiq) {
        msr(SYS_IMP_APL_ESR_GL12, esr);
        msr(SYS_IMP_APL_FAR_GL12, far);
    }
    msr(SYS_IMP_APL_ELR_GL12, elr);
    msr(SYS_IMP_APL_SPSR_GL12, spsr);
    msr(SYS_IMP_APL_ASPSR_GL12, aspsr);
    u64 vec = w->host_vbar_gl12 + (fiq ? 0x500 : 0x400); /* lower-EL AArch64 FIQ / sync */
    if (n++ < 8)
        printf("SPTM-virt[cl4->sptm #%u]: %s esr 0x%lx elr 0x%lx -> SPTM vector 0x%lx\n", n,
               fiq ? "FIQ" : "sync", esr, elr, vec);
    msr(ELR_EL1, vec);
    msr(SPSR_EL1, 0x3c5); /* SPTM's virtual EL2h = physical EL1h, DAIF masked */
    u64 own = mrs(SYS_IMP_APL_ASPSR_GL1);
    if (!(own & ASPSR_GUARDED))
        msr(SYS_IMP_APL_ASPSR_GL1, own | ASPSR_GUARDED);
    return true;
}

bool sptm_virt_cl4_reflect(u64 *regs, u64 esr)
{
    return cl4_deliver(regs, esr, false);
}

/*
 * An FIQ while cL4 runs at GL1: the host's scheduler deadline (the world switch moved it to aura's
 * EL2 timer, whose FIQ Apple delivers to EL2 even with FMO clear) or another interrupt aura does
 * not own. On real hardware it would preempt cL4 into SPTM, so deliver it there.
 */
bool sptm_virt_cl4_fiq(u64 *regs)
{
    if (!in_gl12())
        return false;
    return cl4_deliver(regs, 0, true);
}
