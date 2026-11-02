// SPDX-License-Identifier: GPL-2.0-only
/* macho hypervisor ABI: Linux at guest EL1 <-> this monitor. Same file in the kernel as asm/macho_hv.h.
 * Only a trusted Linux calls this. x0 = function, x1.. args, x0 = result (0 or positive = ok). */
#ifndef MACHO_HV_H
#define MACHO_HV_H
#define MHV_PROBE      0x7510   /* -> x0 MHV_MAGIC, x1 ICH_VTR_EL2, x2 ICC_SRE_EL2, x3 ID_AA64MMFR0. SMCCC 1.1: x4-x17 preserved by all calls */
#define MHV_VM_CREATE  0x7511   /* -> x0 handle (> 0) or negative */
#define MHV_VM_DESTROY 0x7512   /* x1 handle */
#define MHV_MAP        0x7513   /* x1 handle, x2 ipa, x3 pa, x4 size, x5 stage-2 attrs (MHV_ATTR_MASK) */
#define MHV_UNMAP      0x7514   /* x1 handle, x2 ipa, x3 size */
#define MHV_RELAX      0x7515   /* x1 handle, x2 ipa, x3 attrs: new permissions on a valid page */
#define MHV_RUN        0x7516   /* x1 PA of struct mhv_run -> x0 MHV_EXIT_* */
#define MHV_LAST       0x7516
#define MHV_MAGIC      0x3156484dUL   /* "MHV1" */

#define MHV_EXIT_TRAP   0
#define MHV_EXIT_IRQ    1
#define MHV_EXIT_SERROR 2
#define MHV_EXIT_BAD    3            /* the run page or handle was refused, the guest did not run */

#define MHV_E_INVAL   (-22L)
#define MHV_E_NOMEM   (-12L)
#define MHV_E_BUSY    (-16L)
#define MHV_E_SPTM    (-5L)

/* AP, SH, AF, MemAttr, XN[54:53]. Bit 11 (FnXS) is forced on: UNMAP_REGION and a valid-PTE MAP_PAGE on a 0x21 root panic
 * "FNXS is expected to be set" without it. */
#define MHV_ATTR_MASK 0x00600000000007fcUL

/* the SPTM guest_state occupies the first 0xa60 bytes of the page handed to GUEST_DISPATCH */
#define MHV_GS_ROOT    0x000
#define MHV_GS_HCR     0x010
#define MHV_GS_HACR    0x018
#define MHV_GS_HCRX    0x020
#define MHV_GS_SP_EL0  0x038
#define MHV_GS_SP_EL1  0x040
#define MHV_GS_MDSCR   0x048
#define MHV_GS_TPIDR   0x050
#define MHV_GS_TPIDR0  0x058
#define MHV_GS_TPIDRRO 0x060
#define MHV_GS_PAR     0x068
#define MHV_GS_CSSELR  0x070
#define MHV_GS_SCXT    0x088
#define MHV_GS_CPTR    0x090
#define MHV_GS_FPSR    0x098
#define MHV_GS_FPCR    0x0a0
#define MHV_GS_X0      0x0a8
#define MHV_GS_Q0      0x1c0
#define MHV_GS_PSTATE  0xa40
#define MHV_GS_PC      0xa48
#define MHV_GS_TLBFLAG 0xa58           /* bit 0: tlbi vmalle1 on this core before entry */

#define MHV_RUN_OFF    0x1000
/* order of el1[]: the monitor's EL12 accessors in the same order */
enum { MHV_TTBR0, MHV_TTBR1, MHV_TCR, MHV_MAIR, MHV_SCTLR, MHV_AMAIR, MHV_CPACR, MHV_VBAR, MHV_ELR, MHV_SPSR, MHV_ESR,
       MHV_FAR, MHV_AFSR0, MHV_AFSR1, MHV_CONTEXTIDR, MHV_CNTKCTL, MHV_ACTLR, MHV_NR_EL1 };

struct mhv_run {                       /* at page + MHV_RUN_OFF, in guest EL1's RAM, identity PA */
	unsigned long handle;
	unsigned long el1[MHV_NR_EL1];
	unsigned long vmpidr, vpidr, mdcr, cnthctl_set;
	unsigned long hfgrtr, hfgwtr, hfgitr, hdfgrtr, hdfgwtr;
	unsigned long ich_hcr, ich_vmcr, used_lrs, ap0r[4], ap1r[4], lr[16];
	unsigned long vsesr;
	unsigned long pmc_on, pmc_val, pmuserenr;   /* PMCCNTR: val = base when on, the count when off */
	/* filled on exit */
	unsigned long esr, far, hpfar, nr_cpuif_fast, hcr;
};
#endif
