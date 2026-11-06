/* SPDX-License-Identifier: MIT */
/* AP data-abort smoke. No stack, device accesses, or physical reset. */
.section .text, "ax"
.global _start
.global abort_results
.global abort_fault
_start:
    msr daifset, #0xf
    adr x19, abort_results
    str x0, [x19, #136]
    adr x0, magic
    ldr x0, [x0]
    str x0, [x19]
    mov x0, #1
    str x0, [x19, #8]
    str x0, [x19, #16]
    ldr x0, [x19, #32]
    str x0, [x19, #40]
    mrs x0, mpidr_el1
    str x0, [x19, #64]
    mrs x20, cntfrq_el0
    str x20, [x19, #152]
    mrs x21, cntpct_el0
    mov x0, #10
    madd x21, x20, x0, x21
    movz w0, #3
    movk w0, #0xc400, lsl #16
    ldr x1, [x19, #56]
    adr x2, secondary
    mov x3, x19
    smc #0
    str x0, [x19, #24]
    cbnz x0, failed
    mov x0, #2
    str x0, [x19, #16]
    mov x0, #1
    str x0, [x19, #144]
    dsb sy
    mov x22, #0
heartbeat:
    add x22, x22, #1
    str x22, [x19, #80]
    mrs x0, cntpct_el0
    cmp x0, x21
    b.lo heartbeat
failed:
    mov x0, #1
    str x0, [x19, #128]
    dsb sy
    b poweroff

secondary:
    msr daifset, #0xf
    mov x19, x0
    mrs x0, mpidr_el1
    str x0, [x19, #72]
    ldr x0, [x19, #32]
    str x0, [x19, #48]
    mov x0, #3
    str x0, [x19, #16]
wait_bsp:
    ldr x0, [x19, #144]
    cbz x0, wait_bsp
    dmb ish
    ldr x0, [x19, #80]
    cmp x0, #100
    b.lo wait_bsp
    str x0, [x19, #88]
    ldr x20, [x19, #96]
    adr x0, abort_fault
    str x0, [x19, #104]
    mov x0, #1
    str x0, [x19, #112]
    mov x0, #4
    str x0, [x19, #16]
    dsb sy
abort_fault:
    ldr w0, [x20]
    mov x0, #1
    str x0, [x19, #120]
poweroff:
    dsb sy
    movz w0, #8
    movk w0, #0x8400, lsl #16
    smc #0
1:  wfe
    b 1b
.balign 8
magic: .ascii "AWABORT1"
.org 0x1000
abort_results: .space 256
.org 0x2000
