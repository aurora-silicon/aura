/* SPDX-License-Identifier: MIT */

#ifndef NV2_H
#define NV2_H

#include "types.h"

/*
 * FEAT_NV2 nested virtualization for the EL2 monitor. When active, the monitor
 * runs the OS at EL1 as a *virtual EL2* (HCR_EL2.NV|NV2, NV1 clear, VHE only):
 * the guest's EL2 register accesses redirect to a per-CPU VNCR page (Apple
 * IMPDEF ones to the AVNCR page), the rest trap here and are emulated, and its
 * ERETs trap so the monitor can enter the guest's own EL0, or its guest, on its
 * behalf. KVM in the OS can then run hardware-virtualized VMs, whose stage 2
 * the monitor installs directly from the guest's VTTBR/VTCR.
 *
 * Opt-in (aura-nv2, or an NV2_BOOT development build): a plain EL1 boot never
 * sets HCR_EL2.NV2 and the ERET gate in the vectors is a compare and a branch.
 */

extern bool nv2_active;

bool nv2_requested(void);
/* Allocate the per-CPU NV pages; before the OS memory map is fixed. */
void nv2_prepare(void);
void nv2_enter_os(u64 entry, u64 a0, u64 a1, u64 a2) __attribute__((noreturn));

/* Exception hooks: true if handled, and the exception return resumes the
 * virtual-EL2 guest or the guest it runs. kind: 1 IRQ, 2 FIQ, 3 SError. */
bool nv2_handle_sync(u64 *regs);
bool nv2_handle_async(u64 *regs, int kind);

/* NV2_TRACE builds: the event ring, for a reserved-memory node. */
void nv2_trace_region(u64 *base, u64 *size);
void nv2_note_unhandled(u64 esr, u64 elr, u64 spsr);

/* Proxy diagnostics: arm this CPU's NV2 state for an nv2_probe_call(). */
u64 nv2_probe_arm(u64 vncr, u64 avncr, u64 hcr);
void nv2_probe_disarm(void);

#endif
