/* SPDX-License-Identifier: MIT */

#include "monitor.h"
#include "arm_cpu_regs.h"
#include "cpu_regs.h"
#include "exception.h"
#include "malloc.h"
#include "memory.h"
#include "smp.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

/*
 * The Aura monitor stays resident at EL2 and runs the OS at EL1.
 *
 * A boot object with no __TEXT_BOOT_EXEC (kcbz 0) runs without SPTM, but iBoot
 * leaves CTXR windows armed and locked on the secondary CPUs. At EL2 they can
 * then execute only inside the boot object, so an OS cannot run there. At EL1
 * the rule inverts: they can execute anywhere except inside it.
 */

void monitor_eret(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));

static const u32 probe_insns[] = {
    0xd2800020, /* mov x0, #1 */
    0xd65f03c0, /* ret */
};

/* Ask a secondary to run code outside the boot object. */
bool monitor_required(void)
{
    static int required = -1;

    if (required >= 0)
        return required;
    required = 0;
    if (chip_id != T8140)
        return false;

    int cpu;
    for (cpu = 0; cpu < MAX_CPUS; cpu++)
        if (cpu != boot_cpu_idx && smp_is_alive(cpu))
            break;
    if (cpu == MAX_CPUS)
        return false;

    u32 *probe = memalign(SZ_16K, SZ_16K);
    if (!probe)
        return false;
    memcpy(probe, probe_insns, sizeof(probe_insns));
    dc_cvau_range(probe, sizeof(probe_insns));
    ic_ivau_range(probe, sizeof(probe_insns));
    sysop("dsb ish");
    sysop("isb");

    u64 ret = 0;
    exc_guard = GUARD_RETURN | GUARD_SILENT;
    if (!smp_call1(cpu, probe, 0))
        smp_wait(cpu, &ret);
    exc_guard = GUARD_OFF;
    free(probe);

    required = ret != 1;
    if (required)
        printf("monitor: secondaries are confined at EL2, entering the OS at EL1\n");
    return required;
}

/*
 * Leave EL2 the way the kernel's own init_el2_state and finalise_el2_state
 * would, so the OS takes no traps here. Each feature is set up only when the
 * ID registers report it: the chips this runs on have no SVE, GCS, MTE, MPAM,
 * SPE, TRBE or BRBE, and their PMU is IMPDEF, which HACR_EL2 leaves to EL1.
 */
void monitor_enter_os(u64 entry, u64 a0, u64 a1, u64 a2)
{
    /* No stage 2. IRQ, FIQ, SError and external aborts all go to EL1. */
    reg_clr(HCR_EL2, HCR_TGE | HCR_AMO | HCR_IMO | HCR_FMO | HCR_TEA | HCR_VM);
    msr(VTTBR_EL2, 0);
    msr(HACR_EL2, 0);
    msr(HSTR_EL2, 0);
    msr(MDCR_EL2, 0);

    /* With TGE clear, EL1 reads these in place of MIDR_EL1 and MPIDR_EL1. */
    msr(VPIDR_EL2, mrs(MIDR_EL1));
    msr(VMPIDR_EL2, mrs(MPIDR_EL1));

    msr(CNTHCTL_EL2, CNTHCTL_EL1PTEN | CNTHCTL_EL1PCTEN);
    msr(CNTVOFF_EL2, 0);
    /* One counter rate on every CPU, including secondaries the firmware never ran. */
    if (monitor_cntfrq)
        msr(CNTFRQ_EL0, monitor_cntfrq);
    /* With TGE clear the OS's timers are the guest timers: let them raise their FIQs. */
    reg_set(SYS_IMP_APL_VM_TMR_FIQ_ENA_EL2, VM_TMR_FIQ_ENA_ENA_V | VM_TMR_FIQ_ENA_ENA_P);

    u64 cptr = CPTR_E2H_FPEN;
    u64 hfg = 0;
    bool sme = FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1));
    if (sme) {
        cptr |= CPTR_E2H_SMEN;
        hfg |= HFGxTR_nSMPRI_EL1 | HFGxTR_nTPIDR2_EL0;
    }
    msr(SYS_CPTR_EL2, cptr);
    sysop("isb");

    if (sme) {
        u64 smcr = SMCR_LEN;
        if (mrs(SYS_ID_AA64SMFR0_EL1) & ID_AA64SMFR0_FA64)
            smcr |= SMCR_FA64;
        if (FIELD_GET(ID_AA64SMFR0_SMEVER, mrs(SYS_ID_AA64SMFR0_EL1)))
            smcr |= SMCR_EZT0;
        msr(SYS_SMCR_EL2, smcr);
        if (mrs(SYS_SMIDR_EL1) & SMIDR_SMPS)
            msr(SYS_SMPRIMAP_EL2, 0);
    }

    /* Every kernel with S1PIE support programs PIR_EL1 and PIRE0_EL1. */
    u64 mmfr3 = mrs(SYS_ID_AA64MMFR3_EL1);
    if (FIELD_GET(ID_AA64MMFR3_S1PIE, mmfr3))
        hfg |= HFGxTR_nPIR_EL1 | HFGxTR_nPIRE0_EL1;

    /* Fine-grained traps: the "n" bits trap when clear. */
    u64 fgt = FIELD_GET(ID_AA64MMFR0_FGT, mrs(SYS_ID_AA64MMFR0_EL1));
    msr(SYS_HFGRTR_EL2, hfg);
    msr(SYS_HFGWTR_EL2, hfg);
    msr(SYS_HFGITR_EL2, 0);
    msr(SYS_HDFGRTR_EL2, 0);
    msr(SYS_HDFGWTR_EL2, 0);
    /* AMUSERENR_EL0 is written by every kernel that knows the AMU. */
    if (FIELD_GET(ID_AA64PFR0_AMU, mrs(SYS_ID_AA64PFR0_EL1)))
        msr(SYS_HAFGRTR_EL2, 0);
    if (fgt >= 2) {
        msr(SYS_HDFGRTR2_EL2, 0);
        msr(SYS_HDFGWTR2_EL2, 0);
        msr(SYS_HFGRTR2_EL2, 0);
        msr(SYS_HFGWTR2_EL2, 0);
        msr(SYS_HFGITR2_EL2, 0);
    }

    /* As the kernel's __init_el2_hcrx: TCR2_EL1, FEAT_MOPS and FPMR untrapped where present. */
    u64 mmfr1 = mrs(SYS_ID_AA64MMFR1_EL1);
    if (FIELD_GET(ID_AA64MMFR1_HCX, mmfr1)) {
        u64 hcrx = 0;
        if (FIELD_GET(ID_AA64MMFR3_TCRX, mmfr3))
            hcrx |= HCRX_TCR2En;
        if (FIELD_GET(ID_AA64ISAR2_MOPS, mrs(SYS_ID_AA64ISAR2_EL1)))
            hcrx |= HCRX_MSCEn;
        if (FIELD_GET(ID_AA64PFR2_FPMR, mrs(SYS_ID_AA64PFR2_EL1)))
            hcrx |= HCRX_EnFPM;
        msr(SYS_HCRX_EL2, hcrx);
    }
    if (FIELD_GET(ID_AA64MMFR1_LO, mmfr1))
        msr(SYS_LORC_EL1, 0);

    sysop("isb");
    monitor_eret(entry, a0, a1, a2);
}
