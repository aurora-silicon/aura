/* SPDX-License-Identifier: MIT */
/* Position independent EL1 guest. No stack, MMIO, allocator or device drivers. */
.section .text, "ax"
.global _start
.global smoke_results
_start:
    msr daifset, #0xf
    adr x19, smoke_results
    str x0, [x19, #88]             /* Record the loader's boot-arguments pointer. */
    adr x0, .Lmagic
    ldr x0, [x0]
    str x0, [x19, #0]
    mov x0, #1
    str x0, [x19, #8]              /* Result ABI. */
    str x0, [x19, #16]             /* Entered guest. */
    ldr x0, [x19, #64]             /* Per-run host nonce, part of the image hash. */
    str x0, [x19, #72]
    mrs x0, CurrentEL
    str x0, [x19, #80]

    movz w0, #0x8400, lsl #16      /* PSCI_VERSION */
    smc #0
    str w0, [x19, #24]             /* Must be the first instruction after SMC. */
    movz w0, #0xffff
    movk w0, #0x8400, lsl #16      /* Unassigned PSCI function. */
    smc #0
    str w0, [x19, #32]
    mov x0, #2
    str x0, [x19, #16]

    movz w0, #0x8400, lsl #16      /* Check HVC return-PC handling separately. */
    hvc #0
    str w0, [x19, #40]
    movz w0, #0xffff
    movk w0, #0x8400, lsl #16
    hvc #0
    str w0, [x19, #48]
    mov x0, #3
    str x0, [x19, #16]

    adr x0, .Ldone
    ldr x0, [x0]
    str x0, [x19, #96]
    mov x0, #4
    str x0, [x19, #16]             /* Every observation committed before exit. */
    dsb sy
    movz w0, #8
    movk w0, #0x8400, lsl #16      /* PSCI_SYSTEM_OFF: virtual guest exit only. */
    smc #0
    mov x0, #5                    /* A returning SYSTEM_OFF is a test failure. */
    str x0, [x19, #16]
    dsb sy
.Lfailed:
    wfe
    b .Lfailed

.balign 8
.Lmagic:
    .ascii "AWINSMK1"
.Ldone:
    .ascii "DONEPSCI"

/* Keep the result block inside the image, with zero-initialized observations. */
.org 0x1000
smoke_results:
    .space 128
.org 0x2000
