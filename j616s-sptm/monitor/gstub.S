// SPDX-License-Identifier: GPL-2.0-only
// First guest code: EL1, MMU off, x0 = dtb IPA, x1 = params IPA. Stack in the params page. The C part returns to here,
// which enters Linux per its boot protocol: x0 = dtb, x1-x3 = 0, MMU off.
	.section .text.start, "ax"
	.globl _start
_start:
	mov	x19, x0
	mov	x20, x1
	add	sp, x1, #0x4000
	mov	x0, x1
	bl	stub_main
	mov	x9, x0
	mov	x0, x19
	mov	x1, xzr
	mov	x2, xzr
	mov	x3, xzr
	br	x9
