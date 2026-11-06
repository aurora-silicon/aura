/* SPDX-License-Identifier: MIT */
/* EL1 microguest: image-local memory only, no stack or direct device accesses. */
.section .text, "ax"
.global _start
.global n1_smoke_results
.global n1_smoke_requests
.global n1_smoke_requests_end
_start:
    msr daifset, #0xf
    adr x19, n1_smoke_results
    str x0, [x19, #56]            /* Loader's boot-arguments pointer. */
    adr x0, .Lmagic
    ldr x0, [x0]
    str x0, [x19]
    mov x0, #1
    str x0, [x19, #8]             /* Result ABI. */
    str x0, [x19, #16]            /* Entered guest. */
    ldr x0, [x19, #32]            /* Host nonce is part of the image hash. */
    str x0, [x19, #40]
    mrs x0, CurrentEL
    str x0, [x19, #48]

    adr x20, n1_smoke_requests
    add x21, x19, #128
    mov x22, #20
    mov x23, #0
.Lread:
    ldp x1, x2, [x20]             /* Physical address and protocol version. */
    ldr x3, [x20, #16]            /* Reserved argument. */
    stp x1, x2, [x21]
    str x3, [x21, #16]
    adr x0, .Lcall
    ldr x0, [x0]
    hvc #0
    stp x0, x1, [x21, #24]        /* First return instruction must save ACK/status. */
    stp x2, x3, [x21, #40]        /* Value and response ABI. */
    add x23, x23, #1
    str x23, [x21, #56]           /* Complete record ordinal. */
    str x23, [x19, #24]
    add x20, x20, #24
    add x21, x21, #64
    cmp x23, x22
    b.lo .Lread

    mov x0, #2
    str x0, [x19, #16]            /* All requests recorded. */
    adr x0, .Ldone
    ldr x0, [x0]
    str x0, [x19, #64]
    mov x0, #3
    str x0, [x19, #16]            /* Results committed before SYSTEM_OFF. */
    dsb sy
    movz w0, #8
    movk w0, #0x8400, lsl #16      /* PSCI_SYSTEM_OFF: exit guest to proxy. */
    smc #0
    mov x0, #4                    /* Returning SYSTEM_OFF is a failure. */
    str x0, [x19, #16]
    dsb sy
.Lfailed:
    wfe
    b .Lfailed

.balign 8
.Lmagic:
    .ascii "AN1SMK01"
.Ldone:
    .ascii "DONEN1HV"
.Lcall:
    .quad 0x4e3152443332          /* Every request uses recognized N1RD32 magic. */
n1_smoke_requests:
    /* All 17 whitelisted census addresses, version 1 and reserved zero. */
    .quad 0x488028208, 1, 0       /* Port link. */
    .quad 0x1cb0000000, 1, 0      /* Root identity. */
    .quad 0x1cb0100000, 1, 0      /* Control identity. */
    .quad 0x1cb0100004, 1, 0      /* Control command. */
    .quad 0x1cb0100010, 1, 0      /* Control BAR0 low. */
    .quad 0x1cb0100014, 1, 0      /* Control BAR0 high. */
    .quad 0x1cb0101000, 1, 0      /* Radio 1 identity. */
    .quad 0x1cb0101004, 1, 0      /* Radio 1 command. */
    .quad 0x1cb0102000, 1, 0      /* Radio 2 identity. */
    .quad 0x1cb0102004, 1, 0      /* Radio 2 command. */
    .quad 0xb80008000, 1, 0       /* ROM stage. */
    .quad 0x488000000, 1, 0       /* DART parameters 1. */
    .quad 0x488000008, 1, 0       /* DART parameters 3. */
    .quad 0x48800000c, 1, 0       /* DART parameters 4. */
    .quad 0x488000200, 1, 0       /* DART protection. */
    .quad 0x488001000, 1, 0       /* SID0 TCR. */
    .quad 0x488001400, 1, 0       /* SID0 TTBR0. */
    /* Recognized calls with deliberately invalid inputs: no raw MMIO fallback. */
    .quad 0x488028208, 2, 0       /* Wrong version. */
    .quad 0x488028209, 1, 0       /* Unaligned address. */
    .quad 0x488000080, 1, 0       /* Forbidden DART command register. */
n1_smoke_requests_end:

/* Fixed result page is part of the guest image, initially all zero. */
.org 0x1000
n1_smoke_results:
    .space 0x1000
