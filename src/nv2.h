/* SPDX-License-Identifier: MIT */

#ifndef NV2_H
#define NV2_H

#include "types.h"

/*
 * FEAT_NV2 nested virtualization. aura runs an OS at EL1 as a *virtual EL2*
 * (HCR_EL2.NV|NV2, NV1 clear, VHE only): the guest's EL2 register accesses
 * redirect to a per-CPU VNCR page (Apple IMPDEF ones to the AVNCR page), the
 * rest trap here and are emulated, and its ERETs trap so aura can enter the
 * guest's own EL0, or its guest, on its behalf. The ERET gate sits in the
 * lower-EL sync vector.
 */

extern bool nv2_active;
/* Set only while a proxy nv2_probe_call()/nv2_probe_arm() is in flight. */
extern bool nv2_probe_active;

/*
 * A guarded guest (the stock SPTM, see sptm_virt.c) runs its own GXF/SPRR world natively below
 * EL2. When set before nv2_enter_os, aura leaves its ID registers, its IMPDEF registers and
 * GENTER/GEXIT untrapped, while keeping the FEAT_NV2 EL2 redirection and the ERET gate.
 */
extern bool nv2_guarded;

/* Allocate the per-CPU NV pages; before the OS memory map is fixed. */
void nv2_prepare(void);
void nv2_cpu_reset_invalidate(int cpu);
/* aura's own stage 2 for L1, armed per CPU with nv2_l1_stage2_enable_here. */
void nv2_set_l1_stage2_deferred(u64 vtcr, u64 vttbr);
bool nv2_l1_stage2_enable_here(void);
void nv2_hcr_update(u64 set, u64 clr);
void nv2_enter_os(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));
/* Guarded nested child (cL4 under SPTM): load/restore the child's EL1 regime from
 * SPTM's VNCR page across the SPTM<->cL4 guarded world switch. */
void nv2_guarded_child_enter(void);
void nv2_guarded_child_exit(void);

/* Exception hooks: true if handled, and the exception return resumes the
 * virtual-EL2 guest or the guest it runs. kind: 1 IRQ, 2 FIQ, 3 SError. */
bool nv2_handle_sync(u64 *regs);
bool nv2_handle_async(u64 *regs, int kind);

/* NV2_TRACE builds: record a trap nothing handled. */
void nv2_note_unhandled(u64 esr, u64 elr, u64 spsr);

/* Reflect an unhandled trap from an OS at EL1 back into it as an UNDEF rather
 * than resetting the machine. */
void nv2_reflect_undef(u64 elr, u64 spsr);

/* Proxy diagnostics: arm this CPU's NV2 state for an nv2_probe_call(). */
u64 nv2_probe_arm(u64 vncr, u64 avncr, u64 hcr);
void nv2_probe_disarm(void);

#endif
