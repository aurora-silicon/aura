/* SPDX-License-Identifier: MIT */

#ifndef NV2_H
#define NV2_H

#include "types.h"

/*
 * FEAT_NV2 nested virtualization for the EL2 monitor. When active, the monitor
 * runs Linux at EL1 as a *virtual EL2* (HCR_EL2.NV|NV2|E2H): the guest's EL2
 * register accesses redirect to a per-CPU VNCR page (Apple IMPDEF ones to the
 * AVNCR page), and its ERETs trap here so the monitor can enter the guest's own
 * EL0, or its nested guest (L2), on its behalf. This lets KVM in the Linux
 * guest run VMs. Confirmed on T8140: redirection at the ARM VNCR offsets and
 * the ERET trap both work with no SPTM.
 *
 * Opt-in: off until the dispatch is complete, so the plain EL1 boot is
 * unaffected.
 */

struct nv2_cpu {
    u64 vncr_pa;  /* native VNCR page for this CPU */
    u64 avncr_pa; /* Apple AVNCR page (the page after VNCR) */
};

extern bool nv2_active;

/* From exc_sync on a guest sync exception when nv2_active. ec is the decoded
 * ESR_EL2.EC. Returns true if handled: the caller returns and the normal
 * exception-return ERET resumes the guest (or its EL0/L2). */
void nv2_enter_os(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));
bool nv2_handle_sync(u64 *regs, u32 ec);

#endif
