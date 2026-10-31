/* SPDX-License-Identifier: MIT */

#include "exception.h"
#include "memory.h"

.globl exc_sync
.globl exc_irq
.globl exc_fiq
.globl exc_serr
.globl _vectors_start
.globl el0_stack

.globl _v_sp0_sync
.type _v_sp0_sync, @function
_v_sp0_sync:
    msr pan, #0
    /*
     * FEAT_NV2 ERET gate. A virtual-EL2 guest's ERET traps here (EC 0x1a). Its
     * returns to its own userspace (EL0t with vHCR.{E2H,TGE}) and to itself
     * (EL2t/EL2h, run as EL1t/EL1h) are register-only and complete here;
     * anything else (a nested guest entry) takes the full path. Without NV2
     * this costs a compare and a branch.
     */
    stp x0, x1, [sp, #-16]!
    mrs x0, esr_el2
    lsr x1, x0, #26
    cmp x1, #0x1a
    b.ne 9f
    mov x1, #0x6a000000             // plain ERET; ERETAx need the slow path
    cmp x0, x1
    b.ne 9f
    mrs x0, hcr_el2
    tbz x0, #45, 9f                 // NV2 armed on this CPU
    mrs x0, spsr_el2
    and x0, x0, #0xc
    cmp x0, #4                      // trapped at EL1 (virtual EL2)
    b.ne 9f
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
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _exc_entry
    bl exc_sync

    b _exc_return

.globl _v_sp0_irq
.type _v_sp0_irq, @function
_v_sp0_irq:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _exc_entry
    bl exc_irq

    b _exc_return

.globl _v_sp0_fiq
.type _v_sp0_fiq, @function
_v_sp0_fiq:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _exc_entry
    bl exc_fiq

    b _exc_return

.globl _v_sp0_serr
.type _v_sp0_serr, @function
_v_sp0_serr:
    msr pan, #0
    sub sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)
    str x30, [sp, #-16]!
    bl _exc_entry
    bl exc_serr

    b _exc_return

.globl _exc_entry
.type _exc_entry, @function
_exc_entry:
    stp x28, x29, [sp, #-16]!
    stp x26, x27, [sp, #-16]!
    stp x24, x25, [sp, #-16]!
    stp x22, x23, [sp, #-16]!
    stp x20, x21, [sp, #-16]!
    stp x18, x19, [sp, #-16]!
    stp x16, x17, [sp, #-16]!
    stp x14, x15, [sp, #-16]!
    stp x12, x13, [sp, #-16]!
    stp x10, x11, [sp, #-16]!
    stp x8, x9, [sp, #-16]!
    stp x6, x7, [sp, #-16]!
    stp x4, x5, [sp, #-16]!
    stp x2, x3, [sp, #-16]!
    stp x0, x1, [sp, #-16]!

    mov x0, sp
    ret

.globl _exc_return
.type _exc_return, @function
_exc_return:
    ldp x0, x1, [sp], #16
    ldp x2, x3, [sp], #16
    ldp x4, x5, [sp], #16
    ldp x6, x7, [sp], #16
    ldp x8, x9, [sp], #16
    ldp x10, x11, [sp], #16
    ldp x12, x13, [sp], #16
    ldp x14, x15, [sp], #16
    ldp x16, x17, [sp], #16
    ldr x18, [sp], #8
    /* Reload x19-x29 too: a handler may emulate a load into any of them. */
    ldr x19, [sp], #8
    ldp x20, x21, [sp], #16
    ldp x22, x23, [sp], #16
    ldp x24, x25, [sp], #16
    ldp x26, x27, [sp], #16
    ldp x28, x29, [sp], #16
    ldr x30, [sp], #16

    add sp, sp, #(SIZEOF_EXC_INFO - 32 * 8)

    eret

.globl el0_call
.type el0_call, @function
el0_call:
    stp x29, x30, [sp, #-16]!

    mrs x5, CurrentEL
    cmp x5, #4
    beq 1f

    // Disable EL1
    mrs x5, hcr_el2
    orr x5, x5, #(1 << 27)
    msr hcr_el2, x5
    isb

    1:
    // Capture PSTATE
    mov x6, x0
    brk 1
    mov x29, x0
    bic x5, x0, #0xf
    msr daifset, #0xf
    msr spsr_el1, x5
    mov x0, x6

    ldr x5, =_el0_thunk
    msr elr_el1, x5

    mov x5, #REGION_RWX_EL0
    orr x0, x0, x5

    ldr x5, =el0_stack_base
    ldr x5, [x5]
    mov x6, #REGION_RW_EL0
    orr x5, x5, x6
    msr spsel, #0
    mov sp, x5

    eret

_el0_thunk:
    mov x5, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4

    blr x5

    brk 0
    .long 0

.globl el0_ret
.type el0_ret, @function
el0_ret:
    msr daif, x29
    ldp x29, x30, [sp], #16
    ret

.globl el1_call
.type el1_call, @function
el1_call:
    stp x29, x30, [sp, #-16]!

    mrs x29, CurrentEL
    cmp x29, #4
    beq _el1_thunk

    // Enable EL1, but only if not already done.
    // this check is here because writes to hcr_el2 are only possible from GL2
    // if that mode has been enabled
    mrs x5, hcr_el2
    bic x6, x5, #(1 << 27)
    cmp x5, x6
    beq 1f
    msr hcr_el2, x6
    isb

    1:
    // Capture PSTATE into x0
    mov x29, x0
    brk 1
    bic x5, x0, #0xf
    mov x6, #5
    orr x5, x5, x6 // EL1h
    msr daifset, #0xf
    msr spsr_el1, x5
    mov x0, x29

    // We're not in EL1.
    mov x29, #0

    ldr x5, =_el1_thunk
    msr elr_el2, x5

    ldr x5, =el0_stack_base
    ldr x5, [x5]
    msr sp_el1, x5

    eret

_el1_thunk:
    mov x5, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4

    blr x5

    cmp x29, #4
    beq el1_ret

    hvc 0
    .long 0

.globl el1_ret
.type el1_ret, @function
el1_ret:
    msr daif, x29
    ldp x29, x30, [sp], #16
    ret

// monitor_eret(entry, x0, x1, x2): enter the OS at EL1h with DAIF masked.
.globl monitor_eret
.type monitor_eret, @function
monitor_eret:
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

.align 11
.globl _el1_vectors_start
_el1_vectors_start:
    hvc 0x10
    .align 7
    hvc 0x11
    .align 7
    hvc 0x12
    .align 7
    hvc 0x13
    .align 7

    hvc 0x14
    .align 7
    hvc 0x15
    .align 7
    hvc 0x16
    .align 7
    hvc 0x17
    .align 7

    hvc 0x18
    .align 7
    hvc 0x19
    .align 7
    hvc 0x1a
    .align 7
    hvc 0x1b
    .align 7

    hvc 0x1c
    .align 7
    hvc 0x1d
    .align 7
    hvc 0x1e
    .align 7
    hvc 0x1f

.globl el3_call
.type el3_call, @function
el3_call:
    smc 42
    ret
