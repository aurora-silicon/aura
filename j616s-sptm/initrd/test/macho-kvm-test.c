// SPDX-License-Identifier: GPL-2.0-only
// Small KVM guest for the macho monitor: entry and exit, guest TSO bit, counter, emulated timer, vGIC interrupt, PSCI.
// Every result is a line on stdout before the next step runs. Exit status 0 only when all pass. Also runs on raw KVM.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/hw_breakpoint.h>
#include <linux/kvm.h>
#include <linux/perf_event.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define RAM_IPA  0x40000000UL
#define RAM_SIZE (2UL << 20)
#define MMIO_IPA 0x10000000UL
#define GICD_IPA 0x08000000UL
#define GICR_IPA 0x080a0000UL

extern const char guest_start[], guest_end[];
__asm__(
".pushsection .rodata\n.balign 4096\n.globl guest_start\nguest_start:\n"
"	mov	x20, #0x10000000\n"
"	mov	x1, #0x1111\n"
"	str	x1, [x20, #8]\n"                /* 1: first exit and re-entry */
"	mrs	x1, actlr_el1\n"
"	str	x1, [x20, #16]\n"               /* 2: ACTLR as found */
"	mrs	x1, actlr_el1\n"
"	orr	x1, x1, #2\n"
"	orr	x1, x1, #0x10\n"
"	msr	actlr_el1, x1\n"
"	isb\n"
"	mrs	x1, actlr_el1\n"
"	str	x1, [x20, #24]\n"               /* 3: ACTLR after |0x12 */
"	mrs	x2, cntvct_el0\n"
"	mov	x3, #0x20000\n"
"1:	subs	x3, x3, #1\n	b.ne	1b\n"
"	mrs	x1, cntvct_el0\n"
"	sub	x1, x1, x2\n"
"	str	x1, [x20, #32]\n"               /* 4: counter delta */
"	movz	x1, #0x4240\n"
"	movk	x1, #0xf, lsl #16\n"           /* 1 ms at 1 GHz */
"	msr	cntv_tval_el0, x1\n"
"	mov	x1, #3\n"                       /* enabled, masked */
"	msr	cntv_ctl_el0, x1\n"
"	isb\n"
"	mov	x3, #0x400000\n"
"2:	mrs	x1, cntv_ctl_el0\n"
"	tbnz	x1, #2, 3f\n"
"	subs	x3, x3, #1\n	b.ne	2b\n"
"3:	str	x1, [x20, #40]\n"               /* 5: CTL with ISTATUS */
"	msr	cntv_ctl_el0, xzr\n"
"	movz	x0, #0x8400, lsl #16\n"         /* PSCI_VERSION */
"	hvc	#0\n"
"	str	x0, [x20, #48]\n"               /* 6: PSCI version */
"	adr	x1, vectors\n"
"	msr	vbar_el1, x1\n"
"	isb\n"
"	mov	x1, #(3 << 20)\n"
"	msr	cpacr_el1, x1\n"
"	isb\n"
"	mov	x1, #0xabcd\n"
"	fmov	d7, x1\n"
"	mov	x1, #0x1111\n"
"	str	x1, [x20, #8]\n"                /* full exit, the host runs: FP must survive */
"	fmov	x1, d7\n"
"	str	x1, [x20, #72]\n"               /* 9: FP across an exit */
"	mov	x26, #0\n"
"	mrs	x1, pmcr_el0\n"
"	cbz	x26, 10f\n"
"	mov	x1, x26\n"
"	orr	x1, x1, #(1 << 63)\n"
"10:	str	x1, [x20, #80]\n"               /* 10: PMCR_EL0, or bit 63 + ESR of the UNDEF: Windows reads it first */
"	mov	x26, #0\n"
"	mrs	x1, pmcr_el0\n	orr	x1, x1, #1\n	msr	pmcr_el0, x1\n"          /* PMCR.E and PMCNTENSET[31], as Windows: a stopped counter would read a constant here */
"	mov	x1, #0x80000000\n	msr	pmcntenset_el0, x1\n	isb\n"
"	mrs	x2, pmccntr_el0\n"
"	mov	x3, #0x20000\n"
"11:	subs	x3, x3, #1\n	b.ne	11b\n"
"	mrs	x1, pmccntr_el0\n"
"	sub	x1, x1, x2\n"
"	cbz	x26, 12f\n"
"	mov	x1, x26\n"
"	orr	x1, x1, #(1 << 63)\n"
"12:	str	x1, [x20, #88]\n"               /* 11: PMCCNTR delta: 0 kills Windows (division by zero, Odaki v8 cover) */
/* 4K stage 1 as Windows has it, then read, write, execute one fresh page: map RO, relax W, relax X, each with AT in the monitor */
"	movz	x1, #0x4000, lsl #16\n"
"	add	x4, x1, #0x4000\n"              /* L1 */
"	movz	x2, #0x0405\n"                  /* block, AF | idx 1 device */
"	movk	x2, #0, lsl #16\n"
"	str	x2, [x4]\n"
"	movz	x2, #0x0701\n"                  /* block, AF, inner shareable, idx 0 normal */
"	orr	x2, x2, x1\n"
"	str	x2, [x4, #8]\n"
"	mov	x2, #0x04ff\n"
"	msr	mair_el1, x2\n"
"	movz	x2, #0x3519\n"                  /* T0SZ 25, IRGN0, ORGN0 WB, SH0 inner, TG0 4K */
"	movk	x2, #0x80, lsl #16\n"           /* EPD1 */
"	movk	x2, #2, lsl #32\n"              /* IPS 40 bit */
"	msr	tcr_el1, x2\n"
"	msr	ttbr0_el1, x4\n"
"	dsb	ish\n	isb\n"
"	mrs	x2, sctlr_el1\n"
"	mov	x3, #0x1005\n"                  /* M C I */
"	orr	x2, x2, x3\n"
"	msr	sctlr_el1, x2\n"
"	isb\n"
"	add	x5, x1, #0x10000\n"             /* never touched by the host */
"	ldr	x2, [x5]\n"                     /* stage 2: read fault, RO */
"	ldr	w2, 15f\n"
"	str	w2, [x5]\n"                     /* permission fault */
"	ldr	w2, 15f+4\n"
"	str	w2, [x5, #4]\n"
"	dc	cvau, x5\n	dsb	ish\n	ic	ivau, x5\n	dsb	ish\n	isb\n"
"	mov	x0, #0\n"
"	blr	x5\n"                           /* XN permission fault: relax X */
"	str	x0, [x20, #96]\n"               /* 12: 0x1234 from the page */
"	b	16f\n"
"15:	movz	x0, #0x1234\n	ret\n"
"16:	mov	x1, #0x10000\n"
"	msr	cntp_tval_el0, x1\n"
"	mov	x1, #3\n"
"	msr	cntp_ctl_el0, x1\n"
"	isb\n"
"	mov	x3, #0x400000\n"
"17:	mrs	x1, cntp_ctl_el0\n"
"	tbnz	x1, #2, 18f\n"
"	subs	x3, x3, #1\n	b.ne	17b\n"
"18:	str	x1, [x20, #104]\n"              /* 13: CNTP_CTL with ISTATUS, EL1PTEN off under the monitor */
"	msr	cntp_ctl_el0, xzr\n"
"	mov	x26, #0\n"
"	mov	x1, #-1\n"
"	msr	s3_0_c9_c14_2, x1\n"           /* PMINTENCLR_EL1: op1 0, Windows writes it before PMCR */
"	mov	x1, x26\n"
"	cbz	x1, 60f\n"
"	orr	x1, x1, #(1 << 63)\n"
"60:	str	x1, [x20, #120]\n"              /* 15: PMINTENCLR write, or bit 63 + ESR */
"	mov	x1, #4\n"
"	msr	pmuserenr_el0, x1\n"           /* CR: what the Windows kernel sets */
"	isb\n"
"	mov	x28, #0\n"
"	mov	x1, #0\n"
"	adr	x2, el0code\n"
"	msr	elr_el1, x2\n"
"	mov	x2, #0x3c0\n"
"	msr	spsr_el1, x2\n"
"	isb\n"
"	eret\n"
"el0code:\n"
"	mrs	x1, pmccntr_el0\n"
"	svc	#0\n"
"	b	el0code\n"
"el0_back:\n"
"	cbz	x28, 61f\n"
"	mov	x1, x28\n"
"	orr	x1, x1, #(1 << 63)\n"
"61:	str	x1, [x20, #128]\n"              /* 16: PMCCNTR read at EL0, or bit 63 + ESR */
"	msr	pmuserenr_el0, xzr\n"
/* what an exit costs here: 1000 of each, counter ticks. Nothing fails on these, they are the numbers for Windows' 2 M reads of each */
"	mrs	x9, cntvct_el0\n	isb\n	mov	x3, #1000\n"
"62:	mrs	x1, mdscr_el1\n	subs	x3, x3, #1\n	b.ne	62b\n"
"	isb\n	mrs	x10, cntvct_el0\n	sub	x1, x10, x9\n"
"	str	x1, [x20, #208]\n"              /* 26: MDSCR_EL1 reads */
"	mov	x26, #0\n"
"	mrs	x9, cntvct_el0\n	isb\n	mov	x3, #1000\n"
"63:	mrs	x1, pmccntr_el0\n	subs	x3, x3, #1\n	b.ne	63b\n"
"	isb\n	mrs	x10, cntvct_el0\n	sub	x1, x10, x9\n"
"	cbz	x26, 64f\n	orr	x1, x1, #(1 << 63)\n"
"64:	str	x1, [x20, #216]\n"              /* 27: PMCCNTR_EL0 reads, bit 63: UNDEF, this is what the injection costs */
"	mrs	x9, cntvct_el0\n	isb\n	mov	x3, #1000\n"
"65:	mrs	x1, dbgbvr0_el1\n	subs	x3, x3, #1\n	b.ne	65b\n"
"	isb\n	mrs	x10, cntvct_el0\n	sub	x1, x10, x9\n"
"	str	x1, [x20, #224]\n"              /* 28: DBGBVR0_EL1 reads: must trap (FGT or TDA) */
"	mrs	x9, cntvct_el0\n	isb\n	mov	x11, #1000\n"     /* an SMCCC call returns in x0-x3 */
"66:	movz	x0, #0x8400, lsl #16\n	hvc	#0\n	subs	x11, x11, #1\n	b.ne	66b\n"
"	isb\n	mrs	x10, cntvct_el0\n	sub	x1, x10, x9\n"
"	str	x1, [x20, #240]\n"              /* 30: HVC handled in KVM: a full exit */
"	cbnz	x19, 7f\n"
"	mrs	x1, s3_0_c12_c12_5\n"           /* ICC_SRE_EL1 */
"	str	x1, [x20, #56]\n"               /* 7 */
"	mov	x21, #0x08000000\n"             /* GICD */
"	mov	w1, #0x12\n"
"	str	w1, [x21]\n"                    /* CTLR: ARE_NS | EnableGrp1NS */
"	movz	x22, #0x080a, lsl #16\n"        /* GICR */
"	str	wzr, [x22, #0x14]\n"            /* WAKER */
"	add	x23, x22, #0x10000\n"           /* SGI frame */
"	mov	w1, #-1\n"
"	str	w1, [x23, #0x80]\n"             /* IGROUPR0 */
"	mov	w1, #(1 << 27)\n"
"	str	w1, [x23, #0x100]\n"            /* ISENABLER0: virtual timer PPI */
"	mov	x1, #0xff\n"
"	msr	s3_0_c4_c6_0, x1\n"             /* ICC_PMR_EL1 */
"	mov	x1, #1\n"
"	msr	s3_0_c12_c12_7, x1\n"           /* ICC_IGRPEN1_EL1 */
"	isb\n"
"	mov	x24, #0\n"
"	movz	x1, #0x4240\n"
"	movk	x1, #0xf, lsl #16\n"
"	msr	cntv_tval_el0, x1\n"
"	mov	x1, #1\n"
"	msr	cntv_ctl_el0, x1\n"
"	isb\n"
"	msr	daifclr, #2\n"
"	mov	x3, #0x1000\n"
"4:	wfi\n"
"	cbnz	x24, 5f\n"
"	subs	x3, x3, #1\n	b.ne	4b\n"
"5:	msr	daifset, #2\n"
"	str	x24, [x20, #64]\n"              /* 8: INTID taken + 1 */
"	mov	w1, #2\n"
"	str	w1, [x23, #0x100]\n"            /* ISENABLER0: SGI 1 */
"	mov	x24, #0\n"
"	movz	x1, #1\n	movk	x1, #0x100, lsl #16\n"      /* SGI 1, target list: this CPU */
"	msr	s3_0_c12_c11_5, x1\n"           /* ICC_SGI1R_EL1 */
"	isb\n"
"	mov	x3, #0x1000\n"
"19:	wfi\n"                                  /* masked: a pending IRQ still wakes it, no lost wakeup */
"	msr	daifclr, #2\n	isb\n	nop\n	nop\n	msr	daifset, #2\n"
"	cbnz	x24, 20f\n"
"	subs	x3, x3, #1\n	b.ne	19b\n"
"20:\n"
"	str	x24, [x20, #112]\n"             /* 14: SGI, INTID + 1 */
"	mrs	x9, cntvct_el0\n	isb\n	mov	x3, #1000\n"
"67:	mrs	x1, s3_0_c12_c12_2\n	subs	x3, x3, #1\n	b.ne	67b\n"   /* ICC_HPPIR1_EL1 */
"	isb\n	mrs	x10, cntvct_el0\n	sub	x1, x10, x9\n"
"	str	x1, [x20, #232]\n"              /* 29: ICC reads: the monitor's fast path */
/* second vCPU, CPU 0 side */
"	mov	x24, #0\n"
"	movz	x0, #0xc400, lsl #16\n"
"	movk	x0, #3\n"                       /* PSCI_CPU_ON_64 */
"	mov	x1, #1\n"
"	adr	x2, second\n"
"	mov	x3, #0x55\n"
"	hvc	#0\n"
"	str	x0, [x20, #136]\n"              /* 17: CPU_ON */
"	mov	x3, #0x1000\n"
"21:	wfi\n"
"	msr	daifclr, #2\n	isb\n	nop\n	nop\n	msr	daifset, #2\n"
"	cbnz	x24, 22f\n"
"	subs	x3, x3, #1\n	b.ne	21b\n"
"22:	str	x24, [x20, #152]\n"             /* 19: SGI from CPU 1, INTID + 1 */
"	movz	x1, #2\n	movk	x1, #0x200, lsl #16\n"
"	msr	s3_0_c12_c11_5, x1\n	isb\n"    /* SGI 2 to CPU 1 */
"	movz	x2, #0x4000, lsl #16\n"
"	add	x2, x2, #0x3000\n"
"	movz	x3, #0x1000, lsl #16\n"
"23:	ldr	x4, [x2]\n	cbnz	x4, 24f\n"     /* CPU 1 says it spins in the guest */
"	subs	x3, x3, #1\n	b.ne	23b\n"
"24:	movz	x1, #2\n	movk	x1, #0x300, lsl #16\n"
"	msr	s3_0_c12_c11_5, x1\n	isb\n"    /* SGI 3 to a running vCPU: this is the kick */
"	mov	x10, #0x100000\n"
"25:	movz	x0, #0xc400, lsl #16\n	movk	x0, #4\n"   /* PSCI_AFFINITY_INFO_64 */
"	mov	x1, #1\n	mov	x2, #0\n	hvc	#0\n"
"	cmp	x0, #1\n	b.eq	26f\n"
"	subs	x10, x10, #1\n	b.ne	25b\n"
"26:	str	x0, [x20, #200]\n"              /* 25: AFFINITY_INFO, 1 = off */
"7:\n"
"	movz	x0, #0x8400, lsl #16\n"
"	movk	x0, #8\n"                       /* PSCI_SYSTEM_OFF */
"	hvc	#0\n"
"6:	b	6b\n"
/* second vCPU, brought up by CPU_ON. MMU on as CPU 0 has it, its own redistributor */
"second:\n"
"	mov	x20, #0x10000000\n"
"	adr	x1, vectors\n	msr	vbar_el1, x1\n	isb\n"
"	mov	x1, #(3 << 20)\n	msr	cpacr_el1, x1\n	isb\n"
"	movz	x1, #0x4000, lsl #16\n"
"	add	x4, x1, #0x4000\n"
"	mov	x2, #0x04ff\n	msr	mair_el1, x2\n"
"	movz	x2, #0x3519\n	movk	x2, #0x80, lsl #16\n	movk	x2, #2, lsl #32\n	msr	tcr_el1, x2\n"
"	msr	ttbr0_el1, x4\n	dsb	ish\n	isb\n"
"	mrs	x2, sctlr_el1\n	mov	x3, #0x1005\n	orr	x2, x2, x3\n	msr	sctlr_el1, x2\n	isb\n"
"	mrs	x1, mpidr_el1\n"
"	str	x1, [x20, #144]\n"              /* 18: CPU 1 ran, MPIDR */
"	movz	x22, #0x080c, lsl #16\n"        /* GICR of CPU 1 */
"	str	wzr, [x22, #0x14]\n"
"	add	x23, x22, #0x10000\n"
"	mov	w1, #-1\n	str	w1, [x23, #0x80]\n"
"	movz	w1, #0x0c\n	movk	w1, #0x800, lsl #16\n"
"	str	w1, [x23, #0x100]\n"            /* ISENABLER0: SGI 2, 3, virtual timer PPI */
"	mov	x1, #0xff\n	msr	s3_0_c4_c6_0, x1\n"
"	mov	x1, #1\n	msr	s3_0_c12_c12_7, x1\n	isb\n"
"	mov	x24, #0\n"
"	movz	x1, #1\n	movk	x1, #0x100, lsl #16\n"
"	msr	s3_0_c12_c11_5, x1\n	isb\n"    /* SGI 1 to CPU 0 */
"	mov	x3, #0x1000\n"
"31:	wfi\n"
"	msr	daifclr, #2\n	isb\n	nop\n	nop\n	msr	daifset, #2\n"
"	cbnz	x24, 32f\n"
"	subs	x3, x3, #1\n	b.ne	31b\n"
"32:	str	x24, [x20, #160]\n"             /* 20: SGI from CPU 0, INTID + 1 */
"	mov	x24, #0\n"
"	msr	daifclr, #2\n	isb\n"
"	movz	x2, #0x4000, lsl #16\n"
"	add	x2, x2, #0x3000\n"
"	mov	x3, #1\n"
"	mrs	x9, cntvct_el0\n"
"	str	x3, [x2]\n	dsb	ish\n"
"	movz	x3, #0x1000, lsl #16\n"
"33:	cbnz	x24, 34f\n"                    /* no exit from here on: only the kick brings the interrupt */
"	subs	x3, x3, #1\n	b.ne	33b\n"
"34:	mrs	x10, cntvct_el0\n"
"	msr	daifset, #2\n"
"	sub	x10, x10, x9\n"
"	lsl	x10, x10, #8\n"
"	orr	x1, x24, x10\n"
"	str	x1, [x20, #168]\n"              /* 21: kick, INTID + 1 in the low byte, counter ticks above */
"	mov	x6, #0\n"
"	movz	x1, #0xbeef\n	fmov	d7, x1\n"
"	mrs	x8, cntvct_el0\n"
"41:	add	x6, x6, #1\n"
"	str	x6, [x20, #176]\n"              /* 22: the host moves this thread to the next CPU */
"	fmov	x1, d7\n	movz	x2, #0xbeef\n	cmp	x1, x2\n	b.ne	49f\n"
"	mrs	x9, cntvct_el0\n	cmp	x9, x8\n	b.lo	48f\n"
"	mov	x8, x9\n"
"	cmp	x6, #40\n	b.lo	41b\n"
"	mov	x1, #0\n	b	47f\n"
"48:	mov	x1, #1\n	b	47f\n"
"49:	mov	x1, #2\n"
"47:	str	x1, [x20, #184]\n"              /* 23: 0 = FP and the counter survived, 1 = counter went back, 2 = FP lost */
"	mov	x24, #0\n"
"	movz	x1, #0x4240\n	movk	x1, #0xf, lsl #16\n	msr	cntv_tval_el0, x1\n"
"	mov	x1, #1\n	msr	cntv_ctl_el0, x1\n	isb\n"
"	str	xzr, [x20, #176]\n"             /* 22 again: one hop with the timer armed */
"	mov	x3, #0x1000\n"
"51:	wfi\n"
"	msr	daifclr, #2\n	isb\n	nop\n	nop\n	msr	daifset, #2\n"
"	cbnz	x24, 52f\n"
"	subs	x3, x3, #1\n	b.ne	51b\n"
"52:	msr	cntv_ctl_el0, xzr\n"
"	str	x24, [x20, #192]\n"             /* 24: timer after a hop, INTID + 1 */
"	movz	x0, #0x8400, lsl #16\n	movk	x0, #2\n	hvc	#0\n"   /* PSCI_CPU_OFF */
"53:	b	53b\n"
".org guest_start + 0x2000\n"
"vectors:\n"
".org vectors + 0x200\n"                    /* current EL, SPx, sync: note ESR, skip */
"	mrs	x26, esr_el1\n"
"	mrs	x27, elr_el1\n"
"	add	x27, x27, #4\n"
"	msr	elr_el1, x27\n"
"	eret\n"
".org vectors + 0x280\n"                    /* current EL, SPx, IRQ */
"	mrs	x25, s3_0_c12_c12_0\n"          /* ICC_IAR1_EL1 */
"	mov	x1, #2\n"
"	msr	cntv_ctl_el0, x1\n"             /* masked: the level drops */
"	isb\n"
"	msr	s3_0_c12_c12_1, x25\n"          /* ICC_EOIR1_EL1 */
"	add	x24, x25, #1\n"
"	eret\n"
".org vectors + 0x400\n"                    /* lower EL, sync: SVC ends the EL0 step, anything else is noted and skipped */
"	mrs	x27, esr_el1\n"
"	lsr	x27, x27, #26\n"
"	cmp	x27, #0x15\n"
"	b.eq	70f\n"
"	mrs	x28, esr_el1\n"
"	mrs	x27, elr_el1\n"
"	add	x27, x27, #4\n"
"	msr	elr_el1, x27\n"
"	eret\n"
"70:	b	el0_back\n"
".org vectors + 0x800\n"
".globl guest_end\nguest_end:\n"
".popsection\n");

static int expect_tso;
static long vcpu_mmap_size;
static struct kvm_run *volatile run2;
static volatile sig_atomic_t timed_out, joining, done;
static void on_alarm(int s)
{
	static const char m[] = "KVM-TEST: FAIL second vCPU did not stop\n";
	(void)s;
	timed_out = 1;
	if (joining) {
		write(1, m, sizeof(m) - 1);
		_exit(1);
	}
}
static void on_usr1(int s) { (void)s; }
static void say(const char *f, unsigned long a, unsigned long b)
{
	flockfile(stdout);
	printf("KVM-TEST: ");
	printf(f, a, b);
	printf("\n");
	fflush(stdout);
	funlockfile(stdout);
}
#define DIE(what) do { printf("KVM-TEST: FAIL %s: %s\n", what, strerror(errno)); return 1; } while (0)

static int bad, cpus[256], ncpus, cpu0;
static unsigned long got;

/* the next CPU for the second vCPU's thread, never CPU 0's while there is another: a hop between kinds of core and back */
static int hop_next(void)
{
	static int at;
	cpu_set_t set;
	int c = cpus[at++ % ncpus];

	if (c == cpu0 && ncpus > 1)
		c = cpus[at++ % ncpus];
	CPU_ZERO(&set);
	CPU_SET(c, &set);
	if (sched_setaffinity(0, sizeof(set), &set))
		say("sched_setaffinity to cpu %lu failed, stays", c, 0);
	return c;
}

static int handle_step(unsigned long n, uint64_t v, unsigned long *found)
{
	static int hops;

	if (n < 64)
		__atomic_fetch_or(&got, 1UL << n, __ATOMIC_RELAXED);
	switch (n) {
	case 1: say("step 1 guest ran, first exit and re-entry: %lx", v, 0); bad |= v != 0x1111; break;
	case 2: say("step 2 guest ACTLR_EL1 %lx", v, 0); *found = v; break;
	case 3: say("step 3 ACTLR after |0x12 %lx (TSO bits %s)", v, (unsigned long)((v & 0x12) == 0x12 ? "set" : "NOT set, expected on a raw boot only"));
		say("      was %lx", *found, 0); bad |= expect_tso && (v & 0x12) != 0x12; break;
	case 4: say("step 4 counter delta %lu", v, 0); bad |= v == 0; break;
	case 5: say("step 5 CNTV_CTL %lx (ISTATUS means the timer works)", v, 0); bad |= !(v & 4); break;
	case 6: say("step 6 PSCI version %lx", v, 0); bad |= (v >> 16) == 0 || (long)v < 0; break;
	case 7: say("step 7 ICC_SRE_EL1 %lx", v, 0); break;
	case 8: say("step 8 vGIC interrupt, INTID+1 = %lu", v, 0); bad |= v != 28; break;
	case 9: say("step 9 FP across an exit %lx", v, 0); bad |= v != 0xabcd; break;
	case 10: say("step 10 PMCR_EL0 %lx (bit 63: UNDEF, ESR below; Windows stops here)", v, 0); break;
	case 11: say("step 11 PMCCNTR delta %lx (bit 63: UNDEF; 0: Windows divides by it)", v, 0); bad |= v == 0; break;
	case 12: say("step 12 4K stage 1, read, write, exec one page: %lx", v, 0); bad |= v != 0x1234; break;
	case 13: say("step 13 CNTP_CTL %lx (ISTATUS means the timer works)", v, 0); bad |= !(v & 4); break;
	case 14: say("step 14 SGI, INTID+1 = %lu", v, 0); bad |= v != 2; break;
	case 15: say("step 15 PMINTENCLR_EL1 write: %lx (bit 63: UNDEF; Windows writes it before PMCR)", v, 0); break;
	case 16: say("step 16 PMCCNTR at EL0 with PMUSERENR.CR: %lx (bit 63: UNDEF; the Windows kernel sets CR)", v, 0); break;
	case 17: say("step 17 CPU_ON returned %lx", v, 0); bad |= v != 0; break;
	case 18: say("step 18 second vCPU ran, MPIDR %lx", v, 0); bad |= (v & 0xff) != 1; break;
	case 19: say("step 19 SGI from CPU 1 to CPU 0, INTID+1 = %lu", v, 0); bad |= v != 2; break;
	case 20: say("step 20 SGI from CPU 0 to CPU 1, INTID+1 = %lu", v, 0); bad |= v != 3; break;
	case 21: say("step 21 SGI to a running vCPU, INTID+1 = %lu after %lu counter ticks (a host tick away is no kick)", v & 0xff, v >> 8); bad |= (v & 0xff) != 4; break;
	case 22: {
		int c = hop_next();

		say("step 22 hop %lu, next entry on cpu %lu", ++hops, c);
		break;
	}
	case 23: say("step 23 after 40 hops %lx (0 = FP and counter survived)", v, 0); bad |= v != 0; break;
	case 24: say("step 24 timer armed before a hop, INTID+1 = %lu", v, 0); bad |= v != 28; break;
	case 25: say("step 25 AFFINITY_INFO %lx (1 = off)", v, 0); bad |= v != 1; break;
	case 26: say("step 26 1000 MDSCR_EL1 reads: %lu ticks", v, 0); break;
	case 27: say("step 27 1000 PMCCNTR_EL0 reads: %lu ticks, UNDEF %lu", v & ~(1UL << 63), v >> 63); break;
	case 28: say("step 28 1000 DBGBVR0_EL1 reads: %lu ticks", v, 0); break;
	case 29: say("step 29 1000 ICC_HPPIR1_EL1 reads: %lu ticks", v, 0); break;
	case 30: say("step 30 1000 HVC in KVM: %lu ticks", v, 0); break;
	default: say("FAIL unknown step %lu", n, 0); return -1;
	}
	return 0;
}

static void *second_vcpu(void *arg)
{
	int vcpu = (long)arg;
	struct kvm_run *run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	unsigned long found = 0;
	sigset_t set;

	if (run == MAP_FAILED) {
		printf("KVM-TEST: FAIL mmap run of the second vCPU\n");
		_exit(1);
	}
	run2 = run;
	sigemptyset(&set);
	sigaddset(&set, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &set, NULL);         /* the main thread names the stop */
	say("second vCPU thread on cpu %lu: a kick needs another CPU than CPU 0's", hop_next(), 0);
	for (;;) {
		if (ioctl(vcpu, KVM_RUN, 0) < 0) {
			if (errno == EINTR) {
				if (done)
					return NULL;
				continue;
			}
			printf("KVM-TEST: FAIL KVM_RUN second vCPU: %s\n", strerror(errno));
			_exit(1);
		}
		if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT)
			return NULL;
		if (run->exit_reason != KVM_EXIT_MMIO) {
			say("FAIL second vCPU exit reason %lu hw %lx", run->exit_reason, run->hw.hardware_exit_reason);
			_exit(1);
		}
		uint64_t v;

		memcpy(&v, run->mmio.data, 8);
		if (handle_step((run->mmio.phys_addr - MMIO_IPA) / 8, v, &found))
			_exit(1);
	}
}

/*
 * Step 31, world L itself: a watchpoint of this thread needs L's MDSCR and debug registers, so it stops firing if MDCR or the FGT
 * registers stay world V's after an exit (smp-tso E6: the monitor reads then give 0 and the writes vanish, with only a "mon: sysreg trap" line).
 */
static volatile long wp_var;

static long wp_count(void)
{
	struct perf_event_attr a = {
		.type = PERF_TYPE_BREAKPOINT, .size = sizeof(a), .bp_type = HW_BREAKPOINT_W, .bp_addr = (unsigned long)&wp_var,
		.bp_len = HW_BREAKPOINT_LEN_8, .sample_period = 1, .exclude_kernel = 1, .exclude_hv = 1,
	};
	long n = 0;
	int fd = syscall(SYS_perf_event_open, &a, 0, -1, -1, 0);

	if (fd < 0)
		return -errno;
	for (int i = 0; i < 3; i++)
		wp_var = i;
	if (read(fd, &n, sizeof(n)) != sizeof(n))
		n = -errno;
	close(fd);
	return n;
}

static void wp_after(long before)
{
	unsigned long differ = 0, n = ncpus ? ncpus : 1, first = 0;

	if (before <= 0) {
		say("step 31 no L watchpoint before the VM: count 0 or errno %lu", -before, 0);
		return;
	}
	for (unsigned long i = 0; i < n; i++) {
		if (ncpus) {
			cpu_set_t set;

			CPU_ZERO(&set);
			CPU_SET(cpus[i], &set);
			sched_setaffinity(0, sizeof(set), &set);
		}
		if (wp_count() != before && !differ++)
			first = ncpus ? cpus[i] : 0;
	}
	say("step 31 L watchpoint counts %lu, after the VM %lu cpus differ", before, differ);
	if (differ) {
		say("FAIL L watchpoint gone after the VM, first on cpu %lu", first, 0);
		bad = 1;
	}
}

int main(int argc, char **argv)
{
	long wp_before = wp_count();
	int novgic = argc > 1 && !strcmp(argv[1], "--no-vgic");
	for (int i = 1; i < argc; i++)
		expect_tso |= !strcmp(argv[i], "--expect-tso");
	struct kvm_userspace_memory_region mem = { 0 };
	struct kvm_vcpu_init init;
	int kvm, vm, vcpu, vcpu1 = -1, gic;
	pthread_t th;
	struct sigaction sa = { .sa_handler = on_usr1 };

	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = on_alarm;
	sigaction(SIGALRM, &sa, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	say("open /dev/kvm", 0, 0);
	if ((kvm = open("/dev/kvm", O_RDWR)) < 0)
		DIE("open /dev/kvm");
	if ((vm = ioctl(kvm, KVM_CREATE_VM, 0)) < 0)
		DIE("KVM_CREATE_VM");
	say("VM created", 0, 0);
	/* the capabilities QEMU needs before anything else: a missing one failed no test here while QEMU could not start */
	{
		static const struct { int cap; const char *name; } caps[] = {
			{ KVM_CAP_USER_MEMORY, "USER_MEMORY" }, { KVM_CAP_ONE_REG, "ONE_REG" }, { KVM_CAP_READONLY_MEM, "READONLY_MEM" },
			{ KVM_CAP_IOEVENTFD, "IOEVENTFD" }, { KVM_CAP_ARM_PSCI_0_2, "PSCI_0_2" }, { KVM_CAP_MP_STATE, "MP_STATE" },
			{ KVM_CAP_IRQCHIP, "IRQCHIP" },
		};
		for (unsigned c = 0; c < sizeof(caps) / sizeof(caps[0]); c++)
			if (ioctl(kvm, KVM_CHECK_EXTENSION, caps[c].cap) <= 0) {
				printf("KVM-TEST: FAIL capability %s missing\n", caps[c].name);
				bad = 1;
			}
	}
	void *ram = mmap(NULL, RAM_SIZE * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ram == MAP_FAILED)
		DIE("mmap");
	ram = (void *)(((uintptr_t)ram + RAM_SIZE - 1) & ~(RAM_SIZE - 1));
	memcpy(ram, guest_start, guest_end - guest_start);
	mem.guest_phys_addr = RAM_IPA;
	mem.memory_size = RAM_SIZE;
	mem.userspace_addr = (uintptr_t)ram;
	if (ioctl(vm, KVM_SET_USER_MEMORY_REGION, &mem))
		DIE("KVM_SET_USER_MEMORY_REGION");
	if ((vcpu = ioctl(vm, KVM_CREATE_VCPU, 0)) < 0)
		DIE("KVM_CREATE_VCPU");
	if (ioctl(vm, KVM_ARM_PREFERRED_TARGET, &init))
		DIE("KVM_ARM_PREFERRED_TARGET");
	init.features[0] |= 1 << KVM_ARM_VCPU_PSCI_0_2;
	if (ioctl(vcpu, KVM_ARM_VCPU_INIT, &init))
		DIE("KVM_ARM_VCPU_INIT");

	gic = -1;
	if (novgic)
		goto no_vgic;
	/* before the vGIC is initialised: every vCPU gets a redistributor then */
	if ((vcpu1 = ioctl(vm, KVM_CREATE_VCPU, 1)) < 0)
		DIE("KVM_CREATE_VCPU 1");
	init.features[0] |= 1 << KVM_ARM_VCPU_POWER_OFF;
	if (ioctl(vcpu1, KVM_ARM_VCPU_INIT, &init))
		DIE("KVM_ARM_VCPU_INIT 1");
	say("second vCPU created", 0, 0);
	struct kvm_create_device cd = { .type = KVM_DEV_TYPE_ARM_VGIC_V3 };
	if (ioctl(vm, KVM_CREATE_DEVICE, &cd))
		DIE("KVM_CREATE_DEVICE vgic");
	gic = cd.fd;
	uint64_t d = GICD_IPA, r = GICR_IPA;
	struct kvm_device_attr a = { .group = KVM_DEV_ARM_VGIC_GRP_ADDR, .attr = KVM_VGIC_V3_ADDR_TYPE_DIST, .addr = (uintptr_t)&d };
	if (ioctl(gic, KVM_SET_DEVICE_ATTR, &a))
		DIE("vgic dist");
	a.attr = KVM_VGIC_V3_ADDR_TYPE_REDIST;
	a.addr = (uintptr_t)&r;
	if (ioctl(gic, KVM_SET_DEVICE_ATTR, &a))
		DIE("vgic redist");
	a.group = KVM_DEV_ARM_VGIC_GRP_CTRL;
	a.attr = KVM_DEV_ARM_VGIC_CTRL_INIT;
	a.addr = 0;
	if (ioctl(gic, KVM_SET_DEVICE_ATTR, &a))
		DIE("vgic init");
	say("vGIC ready", 0, 0);
no_vgic:;
	uint64_t x19 = novgic;
	struct kvm_one_reg rx19 = { .id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
				    KVM_REG_ARM_CORE_REG(regs.regs[19]), .addr = (uintptr_t)&x19 };
	if (ioctl(vcpu, KVM_SET_ONE_REG, &rx19))
		DIE("set x19");
	uint64_t pc = RAM_IPA;
	struct kvm_one_reg reg = { .id = KVM_REG_ARM64 | KVM_REG_SIZE_U64 | KVM_REG_ARM_CORE |
				   KVM_REG_ARM_CORE_REG(regs.pc), .addr = (uintptr_t)&pc };
	if (ioctl(vcpu, KVM_SET_ONE_REG, &reg))
		DIE("set pc");
	vcpu_mmap_size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
	struct kvm_run *run = mmap(NULL, vcpu_mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
	if (run == MAP_FAILED)
		DIE("mmap run");

	if (vcpu1 >= 0) {
		cpu_set_t set;

		if (sched_getaffinity(0, sizeof(set), &set))
			DIE("sched_getaffinity");
		for (int c = 0; c < CPU_SETSIZE && ncpus < 256; c++)
			if (CPU_ISSET(c, &set))
				cpus[ncpus++] = c;
		cpu0 = sched_getcpu();
		CPU_ZERO(&set);
		CPU_SET(cpu0, &set);
		sched_setaffinity(0, sizeof(set), &set);
		say("CPU 0 stays on cpu %lu, %lu cpus to hop over", cpu0, ncpus);
		if (pthread_create(&th, NULL, second_vcpu, (void *)(long)vcpu1))
			DIE("pthread_create");
	}
	say("first KVM_RUN", 0, 0);
	alarm(30);
	unsigned long found = 0, seen = 0;
	for (;;) {
		if (ioctl(vcpu, KVM_RUN, 0) < 0) {
			if (errno == EINTR && timed_out) {
				say("FAIL no exit for 30 s, last step on this vCPU %lu, steps seen %lx", seen, got);
				return 1;
			}
			if (errno == EINTR)
				continue;
			DIE("KVM_RUN");
		}
		if (run->exit_reason == KVM_EXIT_SYSTEM_EVENT) {
			say("guest powered off, system event %lu", run->system_event.type, 0);
			break;
		}
		if (run->exit_reason != KVM_EXIT_MMIO) {
			say("FAIL exit reason %lu hw %lx", run->exit_reason, run->hw.hardware_exit_reason);
			return 1;
		}
		uint64_t v;
		memcpy(&v, run->mmio.data, 8);
		seen = (run->mmio.phys_addr - MMIO_IPA) / 8;
		if (handle_step(seen, v, &found))
			return 1;
	}
	if (vcpu1 >= 0) {
		/* stopped by CPU_OFF or SYSTEM_OFF, it sleeps in KVM_RUN */
		done = 1;
		joining = 1;
		if (run2)
			run2->immediate_exit = 1;
		pthread_kill(th, SIGUSR1);
		pthread_join(th, NULL);
	}
	alarm(0);
	wp_after(wp_before);
	unsigned long want = 0;
	for (int i = 1; i <= 13; i++)
		want |= 1UL << i;
	want &= ~((1UL << 7) | (1UL << 8));
	want |= (1UL << 15) | (1UL << 16) | (1UL << 26) | (1UL << 27) | (1UL << 28) | (1UL << 30);
	if (!novgic)
		for (int i = 7; i <= 25; i++)
			want |= 1UL << i;
	if (!novgic)
		want |= 1UL << 29;
	if ((got & want) != want) {
		say("FAIL steps missing %lx", want & ~got, 0);
		bad = 1;
	}
	printf("KVM-TEST: %s\n", bad ? "FAIL" : "PASS");
	if (gic >= 0)
		close(gic);
	if (vcpu1 >= 0)
		close(vcpu1);
	close(vcpu);
	close(vm);
	say("VM destroyed", 0, 0);
	return bad;
}
