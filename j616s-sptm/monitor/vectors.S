// SPDX-License-Identifier: GPL-2.0-only
// Lower-EL AArch64 entries of XNU's vector page (VBAR set by XNU and latched by SPTM at FIXUPS_COMPLETE); the
// rest of the page stays XNU's. A non-guarded guest exits here (EL1 -> EL2) with the guest state live in the GPRs and
// ELR/SPSR_EL2, so resume is a plain eret. While no guest was dispatched (ladder fall-through boots) each entry runs
// XNU's original branch to its own handler.
#define CTX 0x120
.macro entry guest, xnu
	.balign 0x80
	stp	x0, x1, [sp, #-16]!
	adrp	x0, native_active
	ldr	x0, [x0, #:lo12:native_active]
	movz	x1, #0x5447
	movk	x1, #0x4e45, lsl #16
	movk	x1, #0x5645, lsl #32
	movk	x1, #0x4e49, lsl #48
	cmp	x0, x1                       // magic: the word is XNU's own bss on fall-through boots
	b.eq	1f
	ldp	x0, x1, [sp], #16
	b	\xnu
1:	ldp	x0, x1, [sp], #16
	b	\guest
.endm
.macro hentry xnu, vec
	.balign 0x80
	stp	x0, x1, [sp, #-16]!
	adrp	x0, host_active
	ldr	x0, [x0, #:lo12:host_active]
	movz	x1, #0x5447
	movk	x1, #0x4e45, lsl #16
	movk	x1, #0x5645, lsl #32
	movk	x1, #0x4e49, lsl #48
	cmp	x0, x1
	b.eq	1f
	ldp	x0, x1, [sp], #16
	b	\xnu
1:	mov	x1, #\vec
	b	host_fault
.endm
	.section .vectors, "ax"
	.globl mon_vectors
mon_vectors:
	// an exception at EL2 in the monitor: SP_EL0 running (+0x000) or SP_EL2 (+0x200). XNU's handlers on a kernel that never initialised stop silently
	hentry	xnu_sp0_sync, 0
	hentry	xnu_sp0_irq, 1
	hentry	xnu_sp0_fiq, 2
	hentry	xnu_sp0_serror, 3
	hentry	xnu_spx_sync, 4
	hentry	xnu_spx_irq, 5
	hentry	xnu_spx_fiq, 6
	hentry	xnu_spx_serror, 7
	entry	guest_sync, xnu_el0_sync   // 0x400 + 0x433*4
	entry	guest_irq, xnu_el0_irq    // 0x480 + 0x491*4
	entry	guest_fiq, xnu_el0_fiq    // 0x500 + 0x4f0*4
	entry	guest_serror, xnu_el0_serror    // 0x580 + 0x54f*4

	.bss
	.balign 8
	.globl native_active, host_active
native_active:
	.space 8
host_active:
	.space 8

	.text
host_fault:
	mov	x4, x1
	mrs	x0, esr_el2
	mrs	x1, elr_el2
	mrs	x2, far_el2
	mrs	x3, spsr_el2
	bl	mon_host_fault
1:	b	1b
guest_serror:
	sub	sp, sp, #CTX
	stp	x0, x1, [sp, #0]
	mov	x1, #3
	b	guest_common
guest_fiq:
	sub	sp, sp, #CTX
	stp	x0, x1, [sp, #0]
	mov	x1, #2
	b	guest_common
guest_irq:
	sub	sp, sp, #CTX
	stp	x0, x1, [sp, #0]
	mov	x1, #1
	b	guest_common
guest_sync:
	sub	sp, sp, #CTX
	stp	x0, x1, [sp, #0]
	mov	x1, #0
guest_common:
	stp	x2, x3, [sp, #16]
	stp	x4, x5, [sp, #32]
	stp	x6, x7, [sp, #48]
	stp	x8, x9, [sp, #64]
	stp	x10, x11, [sp, #80]
	stp	x12, x13, [sp, #96]
	stp	x14, x15, [sp, #112]
	stp	x16, x17, [sp, #128]
	stp	x18, x19, [sp, #144]
	stp	x20, x21, [sp, #160]
	stp	x22, x23, [sp, #176]
	stp	x24, x25, [sp, #192]
	stp	x26, x27, [sp, #208]
	stp	x28, x29, [sp, #224]
	mrs	x2, elr_el2
	stp	x30, x2, [sp, #240]          // x30, pc
	mrs	x2, spsr_el2
	mrs	x3, esr_el2
	stp	x2, x3, [sp, #256]           // spsr, esr
	mrs	x2, far_el2
	mrs	x3, hpfar_el2
	stp	x2, x3, [sp, #272]           // far, hpfar
	mov	x2, #0x300000                // CPTR_EL2.FPEN = 0b11: SPTM's dispatch saves d8-d15
	msr	cptr_el2, x2
	isb
	mov	x0, sp
	bl	mon_guest_exit               // x0 = ctx, x1 = 0 sync / 1 async; returns only to resume
	ldp	x2, x3, [sp, #256]
	msr	spsr_el2, x2
	ldr	x2, [sp, #248]
	msr	elr_el2, x2
	ldp	x0, x1, [sp, #0]
	ldp	x2, x3, [sp, #16]
	ldp	x4, x5, [sp, #32]
	ldp	x6, x7, [sp, #48]
	ldp	x8, x9, [sp, #64]
	ldp	x10, x11, [sp, #80]
	ldp	x12, x13, [sp, #96]
	ldp	x14, x15, [sp, #112]
	ldp	x16, x17, [sp, #128]
	ldp	x18, x19, [sp, #144]
	ldp	x20, x21, [sp, #160]
	ldp	x22, x23, [sp, #176]
	ldp	x24, x25, [sp, #192]
	ldp	x26, x27, [sp, #208]
	ldp	x28, x29, [sp, #224]
	ldr	x30, [sp, #240]
	add	sp, sp, #CTX
	eret
