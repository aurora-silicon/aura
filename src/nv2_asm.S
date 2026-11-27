/* SPDX-License-Identifier: MIT */

/*
 * Proxy-driven FEAT_NV2 probe entry. It lives in the image because a
 * secondary CPU on T8140 can execute at EL2 only inside the boot object; the
 * probed EL1 code runs from the heap, where EL1 may execute.
 *
 * nv2_probe_call(ctx) arms NV2 with the context's VNCR/AVNCR pages and HCR
 * bits, then ERETs to ctx->entry at EL1. The EL1 code finishes with `hvc #1`,
 * which exc_sync turns into a return to nv2_probe_ret at EL2. That restores
 * HCR_EL2 and VBAR_EL12, records x0/x1 in the context and returns to the
 * caller with the callee-saved registers it had.
 *
 * Context layout (u64 slots), shared with the proxy scripts:
 *   0x00 entry       0x08 sp_el1    0x10 vncr       0x18 avncr
 *   0x20 hcr_set     0x28 hcr_clr   0x30 arg (x0)   0x38 vbar_el1 (0: keep)
 *   0x40 spsr (0: EL1h, DAIF masked)
 *   0x48 out x0      0x50 out x1    0x58 out done   0x60 saved HCR_EL2
 *   0x68 saved VBAR_EL12
 *   0x70 CNTHCTL_EL2 value  0x78 HFGITR_EL2 value  0x80 which to set (bit 0
 *   CNTHCTL, bit 1 HFGITR)   0x88/0x90 their saved values
 * VPIDR/VMPIDR are set to the real MIDR/MPIDR so the EL1 code reads true IDs.
 */

#define NV2P_ENTRY  0x00
#define NV2P_SP1    0x08
#define NV2P_VNCR   0x10
#define NV2P_AVNCR  0x18
#define NV2P_SET    0x20
#define NV2P_CLR    0x28
#define NV2P_ARG    0x30
#define NV2P_VBAR   0x38
#define NV2P_SPSR   0x40
#define NV2P_X0     0x48
#define NV2P_X1     0x50
#define NV2P_DONE   0x58
#define NV2P_HCR    0x60
#define NV2P_OVBAR  0x68
#define NV2P_HCTL   0x70
#define NV2P_HFGIT  0x78
#define NV2P_WHICH  0x80
#define NV2P_OHCTL  0x88
#define NV2P_OHFGIT 0x90

.globl nv2_probe_call
.type nv2_probe_call, @function
nv2_probe_call:
    stp x29, x30, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    mrs x1, daif
    stp x0, x1, [sp, #-16]!
    msr daifset, #0xf

    /* Mark a proxy probe in flight so exc_sync's `hvc #1` gate opens. */
    ldr x1, =nv2_probe_active
    mov w2, #1
    strb w2, [x1]

    mrs x1, hcr_el2
    str x1, [x0, #NV2P_HCR]
    mrs x2, S3_5_C12_C0_0           // VBAR_EL12
    str x2, [x0, #NV2P_OVBAR]
    ldr x2, [x0, #NV2P_VBAR]
    cbz x2, 1f
    msr S3_5_C12_C0_0, x2           // VBAR_EL12
1:
    mrs x2, mpidr_el1
    msr vmpidr_el2, x2
    mrs x2, midr_el1
    msr vpidr_el2, x2
    ldr x3, [x0, #NV2P_WHICH]
    mrs x2, S3_4_C14_C1_0           // CNTHCTL_EL2
    str x2, [x0, #NV2P_OHCTL]
    tbz x3, #0, 2f
    ldr x2, [x0, #NV2P_HCTL]
    msr S3_4_C14_C1_0, x2
2:
    mrs x2, S3_4_C1_C1_6            // HFGITR_EL2
    str x2, [x0, #NV2P_OHFGIT]
    tbz x3, #1, 3f
    ldr x2, [x0, #NV2P_HFGIT]
    msr S3_4_C1_C1_6, x2
3:
    ldr x2, [x0, #NV2P_VNCR]
    msr S3_4_C2_C2_0, x2            // VNCR_EL2
    ldr x2, [x0, #NV2P_AVNCR]
    msr S3_4_C15_C10_7, x2          // AVNCR_EL2 (Apple)
    ldr x2, [x0, #NV2P_CLR]
    bic x1, x1, x2
    bic x1, x1, #(1 << 27)          // TGE clear: EL1 is a guest
    ldr x2, [x0, #NV2P_SET]
    orr x1, x1, x2
    msr hcr_el2, x1
    isb

    ldr x2, [x0, #NV2P_ENTRY]
    msr elr_el2, x2
    ldr x2, [x0, #NV2P_SPSR]
    mov x3, #0x3c5
    cmp x2, #0
    csel x2, x3, x2, eq
    msr spsr_el2, x2
    ldr x2, [x0, #NV2P_SP1]
    msr sp_el1, x2
    ldr x0, [x0, #NV2P_ARG]
    isb
    eret

/* Entered at EL2h from exc_sync's `hvc #1` handler; x0/x1 are the EL1 code's. */
.globl nv2_probe_ret
.type nv2_probe_ret, @function
nv2_probe_ret:
    ldp x2, x3, [sp], #16           // x2 = ctx, x3 = caller DAIF
    ldr x4, [x2, #NV2P_HCR]
    msr hcr_el2, x4
    ldr x4, [x2, #NV2P_OVBAR]
    msr S3_5_C12_C0_0, x4           // VBAR_EL12
    ldr x4, [x2, #NV2P_OHCTL]
    msr S3_4_C14_C1_0, x4           // CNTHCTL_EL2
    ldr x4, [x2, #NV2P_OHFGIT]
    msr S3_4_C1_C1_6, x4            // HFGITR_EL2
    isb
    str x0, [x2, #NV2P_X0]
    str x1, [x2, #NV2P_X1]
    mov x4, #1
    str x4, [x2, #NV2P_DONE]
    dmb sy
    msr daif, x3
    /* The probe is over; close the `hvc #1` proxy-return gate. */
    ldr x4, =nv2_probe_active
    strb wzr, [x4]
    ldp x19, x20, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret

// nv2_enter_el1h(entry, x0, x1, x2): enter the OS at EL1h with DAIF masked.
.globl nv2_enter_el1h
.type nv2_enter_el1h, @function
nv2_enter_el1h:
    msr elr_el2, x0
    mov x4, #0x3c5
    msr spsr_el2, x4
    msr sp_el1, xzr
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, xzr
    isb
    eret

/*
 * Synchronous exceptions from a lower EL, in place of _v_sp0_sync. At EL2 this
 * is where a FEAT_NV2 guest's ERET traps (EC 0x1a): its returns to its own
 * userspace (EL0t with vHCR.{E2H,TGE}) and to itself (EL2t/EL2h, run as
 * EL1t/EL1h) are register-only and complete here, and anything else (a nested
 * guest entry) takes the full path, nv2_eret. Aura itself can run at EL1 (as a
 * hypervisor guest, or below EL3), where the _EL2 registers are UNDEFINED, so
 * look at CurrentEL first. Exceptions taken at the current EL never come
 * through here.
 */
.globl _v_lower_sync
.type _v_lower_sync, @function
_v_lower_sync:
    msr pan, #0
    stp x0, x1, [sp, #-16]!
    mrs x0, CurrentEL
    cmp x0, #(2 << 2)
    b.ne 9f
    mrs x0, esr_el2
    mov x1, #0x6a000000             // plain ERET; ERETAx need the slow path
    cmp x0, x1
    b.ne 9f
    /* An ERET traps only at EL1 with HCR_EL2.NV, which aura sets only
     * together with NV2: no need to look at HCR_EL2 or SPSR_EL2. */
    mrs x0, S3_5_C4_C0_0            // SPSR_EL12: the guest's requested PSTATE
    tst x0, #0x10                   // AArch32 target: slow path
    b.ne 9f
    and x1, x0, #0xf
    cbz x1, 2f                      // EL0t
    cmp x1, #8                      // EL2t/EL2h: local return
    b.lo 9f
    cmp x1, #9
    b.hi 9f
    sub x0, x0, #4
    b 3f
2:  mrs x1, S3_4_C2_C2_0            // VNCR_EL2
    ldr x1, [x1, #0x78]             // the guest's HCR_EL2
    tbz x1, #27, 9f                 // TGE: its own userspace
    tbz x1, #34, 9f                 // E2H
3:  msr spsr_el2, x0
    mrs x0, S3_5_C4_C0_1            // ELR_EL12
    msr elr_el2, x0
    ldp x0, x1, [sp], #16
    eret
9:  ldp x0, x1, [sp], #16
    b _v_sp0_sync                   // clears PAN again, then the usual path
