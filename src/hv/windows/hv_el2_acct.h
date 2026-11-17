/* SPDX-License-Identifier: MIT */
#ifndef HV_WINDOWS_EL2_ACCT_H
#define HV_WINDOWS_EL2_ACCT_H

/*
 * Causes for the per-CPU EL2 time accounting in hv_exc.c; see
 * docs/windows-el2-accounting.md.  Plain defines because hv_asm.S passes each
 * vector's default cause to hv_el2_enter() before the C handler runs.
 */
#define HV_EL2_TICK   0 /* host CNTHP FIQ: periodic tick or WFI wake expiry */
#define HV_EL2_IRQ    1 /* IRQ vector, guest timer/IPI FIQ, ICC_* sites */
#define HV_EL2_ABS    2 /* other absent-register emulation (PMCCNTR, ...) */
#define HV_EL2_VEC    3 /* guest exception passed through a redirected vector */
#define HV_EL2_SYNC   4 /* every other synchronous trap, and SError */
#define HV_EL2_WFI    5 /* guest WFI held at EL2 */
#define HV_EL2_LOCK   6 /* waiting for bhl; never a stay's own cause */
#define HV_EL2_CAUSES 7

#endif
