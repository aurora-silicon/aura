// SPDX-License-Identifier: GPL-2.0-only
// Monitor entry from the hook in XNU arm_init, 26.7.1 (hook site in tools/kc_patch.py). arm_init's prefix has run:
// boot_args copied to x19 (0xfffffe0007d75138), SPTM bootstrap args copied to x20 (0xfffffe0007d755b8),
// REGISTER_EXC_RETURN called (0xb911d28). The hook site holds `b mon_entry`. mon_main returns only on a ladder draw:
// replay the displaced `adrp x22` and continue XNU (same as the v18 cave).
	.section .text.start, "ax"
	.globl mon_entry
mon_entry:
	bti	c
	stp	x29, x30, [sp, #-16]!
	mov	x0, x19
	mov	x1, x20
	bl	mon_main
	ldp	x29, x30, [sp], #16
	adrp	x22, xnu_displaced_page
	b	xnu_arm_init_cont

// Secondary hook: XNU _start's warm path (`mov x20, x3`). SPTM's first-start path gerets here with
// x0 = 1, x1 = SPTM args, x2 = an SPTM pointer, EL2h, DAIF masked, on no usable sp. On a boot that is not native
// (ladder fall-through) the displaced instruction runs and XNU continues.
	.section .text.start, "ax"
	.globl mon_secondary
mon_secondary:
	adrp	x9, native_active
	ldr	x9, [x9, #:lo12:native_active]
	movz	x10, #0x5447
	movk	x10, #0x4e45, lsl #16
	movk	x10, #0x5645, lsl #32
	movk	x10, #0x4e49, lsl #48
	cmp	x9, x10
	b.eq	1f
	mov	x20, x3
	b	xnu_start_warm_cont
1:	mrs	x9, mpidr_el1
	ubfx	x10, x9, #8, #2
	and	x9, x9, #7
	add	x9, x9, x10, lsl #3          // slot
	adrp	x10, g_cpu_top
	add	x10, x10, #:lo12:g_cpu_top
	add	x10, x10, x9, lsl #4
	ldp	x11, x12, [x10]
	cbz	x11, .                       // a core nobody started
	msr	spsel, #1
	mov	sp, x11
	msr	spsel, #0
	mov	sp, x12
	mov	x2, #0x300000                // CPTR_EL2.FPEN = 0b11: SPTM's dispatch saves d8-d15
	msr	cptr_el2, x2
	isb
	bl	mon_secondary_main
2:	wfe
	b	2b

// SPTM panic reflect, _start+0x1c (`b panic_from_sptm`): SPTM gerets to _start with x0 = 4, x1 = string, x2 = coredump
// flag (xnu start_sptm.s), EL2h, DAIF masked, SP_EL2 whatever it was, and _start has done mov x0, x1. XNU's panic() on a kernel that
// never ran arm_init is a silent stop on a native boot; on a ladder fall-through the original branch runs.
	.section .text.start, "ax"
	.globl mon_sptm_panic
mon_sptm_panic:
	adrp	x9, host_active
	ldr	x9, [x9, #:lo12:host_active]
	movz	x10, #0x5447
	movk	x10, #0x4e45, lsl #16
	movk	x10, #0x5645, lsl #32
	movk	x10, #0x4e49, lsl #48
	cmp	x9, x10
	b.eq	1f
	b	xnu_panic_from_sptm
1:	mrs	x9, mpidr_el1
	ubfx	x10, x9, #8, #2
	and	x9, x9, #7
	add	x9, x9, x10, lsl #3          // slot, as mon_secondary
	add	x9, x9, #1
	adrp	x10, g_panic_stack
	add	x10, x10, #:lo12:g_panic_stack
	add	x10, x10, x9, lsl #11        // 2 KiB per slot, the end of this slot's
	mov	sp, x10
	bl	mon_sptm_panic_c
2:	b	2b

// u64 sptm_call_raw(u64 id, u64 a0, ..., u64 a5): GENTER into SPTM. SPTM follows AAPCS (x0-x18 clobbered).
	.text
	.arch_extension fp
	.arch_extension simd
	.globl fp_save
fp_save:
	stp	q0, q1, [x0, #0]
	stp	q2, q3, [x0, #32]
	stp	q4, q5, [x0, #64]
	stp	q6, q7, [x0, #96]
	stp	q8, q9, [x0, #128]
	stp	q10, q11, [x0, #160]
	stp	q12, q13, [x0, #192]
	stp	q14, q15, [x0, #224]
	stp	q16, q17, [x0, #256]
	stp	q18, q19, [x0, #288]
	stp	q20, q21, [x0, #320]
	stp	q22, q23, [x0, #352]
	stp	q24, q25, [x0, #384]
	stp	q26, q27, [x0, #416]
	stp	q28, q29, [x0, #448]
	stp	q30, q31, [x0, #480]
	mrs	x3, fpsr
	str	x3, [x1]
	mrs	x3, fpcr
	str	x3, [x2]
	ret

// GUEST_DISPATCH from an exit: a fresh stack, nothing comes back here
	.globl redispatch
redispatch:
	mov	sp, x1
	mov	x1, x0
	mov	x0, #27
	bl	sptm_call_raw
1:	b	1b

	.globl sptm_call_raw
sptm_call_raw:
	stp	x29, x30, [sp, #-96]!
	stp	x19, x20, [sp, #16]
	stp	x21, x22, [sp, #32]
	stp	x23, x24, [sp, #48]
	stp	x25, x26, [sp, #64]
	stp	x27, x28, [sp, #80]
	bl	call_sp_slot
	mov	x10, sp
	str	x10, [x9]
	mov	x16, x0
	mov	x0, x1
	mov	x1, x2
	mov	x2, x3
	mov	x3, x4
	mov	x4, x5
	mov	x5, x6
	.inst	0x00201420
	// from the exit path SPTM comes back on another sp (GUEST_EXIT and later calls, qemu under the real SPTM)
	mrs	x9, mpidr_el1
	ubfx	x10, x9, #8, #2
	and	x9, x9, #7
	add	x9, x9, x10, lsl #3
	adrp	x10, g_call_sp
	add	x10, x10, #:lo12:g_call_sp
	ldr	x10, [x10, x9, lsl #3]
	mov	sp, x10
	ldp	x19, x20, [sp, #16]
	ldp	x21, x22, [sp, #32]
	ldp	x23, x24, [sp, #48]
	ldp	x25, x26, [sp, #64]
	ldp	x27, x28, [sp, #80]
	ldp	x29, x30, [sp], #96
	ret

// x9 = &g_call_sp[this slot]; only x9, x10 change
call_sp_slot:
	mrs	x9, mpidr_el1
	ubfx	x10, x9, #8, #2
	and	x9, x9, #7
	add	x9, x9, x10, lsl #3
	adrp	x10, g_call_sp
	add	x10, x10, #:lo12:g_call_sp
	add	x9, x10, x9, lsl #3
	ret

	.bss
	.balign 8
g_call_sp:
	.space 8 * 24
	.text

// the guest stub (EL1, MMU off, IPA-independent), copied into guest RAM by linux.c
	.section .rodata
	.balign 16
	.globl gstub_start, gstub_end
gstub_start:
	.incbin GSTUB_BIN
gstub_end:
