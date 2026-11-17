// Host-only harness: u64 call_checked(fn, a0, a1, a2, a3, a4, a5)
// Calls fn(a0..a5) with x19-x28 and x29 holding known values; returns a bit mask of the
// callee-saved registers, x29 and SP that did not survive (0 = all intact).
.text
.p2align 2
.globl _call_checked
_call_checked:
    stp x29, x30, [sp, #-16]!
    stp x27, x28, [sp, #-16]!
    stp x25, x26, [sp, #-16]!
    stp x23, x24, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    adrp x9, _glue_sp@PAGE
    mov x10, sp
    str x10, [x9, _glue_sp@PAGEOFF]
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    mov x19, #0x1901
    mov x20, #0x2002
    mov x21, #0x2103
    mov x22, #0x2204
    mov x23, #0x2305
    mov x24, #0x2406
    mov x25, #0x2507
    mov x26, #0x2608
    mov x27, #0x2709
    mov x28, #0x280a
    mov x29, #0x290b
    blr x8
    mov x0, #0
    mov x12, #0x1901
    cmp x19, x12
    cset x13, ne
    orr x0, x0, x13
    mov x12, #0x2002
    cmp x20, x12
    cset x13, ne
    orr x0, x0, x13, lsl #1
    mov x12, #0x2103
    cmp x21, x12
    cset x13, ne
    orr x0, x0, x13, lsl #2
    mov x12, #0x2204
    cmp x22, x12
    cset x13, ne
    orr x0, x0, x13, lsl #3
    mov x12, #0x2305
    cmp x23, x12
    cset x13, ne
    orr x0, x0, x13, lsl #4
    mov x12, #0x2406
    cmp x24, x12
    cset x13, ne
    orr x0, x0, x13, lsl #5
    mov x12, #0x2507
    cmp x25, x12
    cset x13, ne
    orr x0, x0, x13, lsl #6
    mov x12, #0x2608
    cmp x26, x12
    cset x13, ne
    orr x0, x0, x13, lsl #7
    mov x12, #0x2709
    cmp x27, x12
    cset x13, ne
    orr x0, x0, x13, lsl #8
    mov x12, #0x280a
    cmp x28, x12
    cset x13, ne
    orr x0, x0, x13, lsl #9
    mov x12, #0x290b
    cmp x29, x12
    cset x13, ne
    orr x0, x0, x13, lsl #10
    adrp x9, _glue_sp@PAGE
    ldr x10, [x9, _glue_sp@PAGEOFF]
    mov x11, sp
    cmp x10, x11
    cset x13, ne
    orr x0, x0, x13, lsl #11
    mov sp, x10
    ldp x19, x20, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x23, x24, [sp], #16
    ldp x25, x26, [sp], #16
    ldp x27, x28, [sp], #16
    ldp x29, x30, [sp], #16
    ret

.data
.p2align 3
_glue_sp:
    .quad 0
