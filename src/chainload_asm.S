/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

.text

.globl _chainload_stub_start
.globl _chainload_stub_end
.type _chainload_stub_start, @function

_chainload_stub_start:
    mov x7, x2
    mov x8, x3
    /* A staged image is already in place (and mmu_shutdown() already cleaned D-cache). */
    cmp x1, x2
    b.eq 3f
1:
    ldp x5, x6, [x1], #16
    stp x5, x6, [x2]
    add x2, x2, #16
    subs x3, x3, #16
    b.ne 1b

    mov x2, x7
    mov x3, x8
2:
    dc cvac, x2
    add x2, x2, #64
    subs x3, x3, #64
    b.ne 2b
3:
    dsb sy
    ic iallu
    dsb sy
    isb

    br x4
_chainload_stub_end:
