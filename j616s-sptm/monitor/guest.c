// SPDX-License-Identifier: GPL-2.0-only
// One vCPU guest under SPTM: guest_state staging, GUEST_DISPATCH (ep27), exits via mon_vectors, GUEST_EXIT (ep28).
#include "mon.h"
#include "mhv.h"

#define GUART_IPA 0x09000000UL           /* emulated PL011: DR (0x00) writes print, FR (0x18) reads TXFE|RXFE */

static int terminated;
static volatile u32 g_apif_lock, g_con_lock;

/* In STATE_XNU_GUEST the only legal SPTM call is GUEST_EXIT (dispatch_state_machine, qemu under the real SPTM; a forward
 * from here is a panic spin). So a call from an exit goes exit -> GUEST_EXIT -> calls -> GUEST_DISPATCH, which takes the
 * guest's state from guest_state. EL1 registers with an _EL12 alias stay live: no SPTM path of this boot touches them
 * (the SK switch is EVENT_CALL_SK and EVENT_BOOTSTRAP_SK). Offsets as SPTM's dispatch tail reads them. */
#define GS_SP_EL0 0x038
#define GS_SP_EL1 0x040
#define GS_MDSCR  0x048
#define GS_TPIDR  0x050
#define GS_TPIDR0 0x058
#define GS_TPIDRRO 0x060
#define GS_PAR    0x068
#define GS_CSSELR 0x070
#define GS_SCXT   0x088
#define GS_FPSR   0x098
#define GS_FPCR   0x0a0
#define GS_Q0     0x1c0
extern void fp_save(u64 q, u64 fpsr, u64 fpcr);

/* The guest's EL1 registers: dispatch takes them as it finds them. World V (mhv.c enter_vm) overwrites them, so they are
 * kept here across a VM run; without a VM this save and restore change nothing (TTBRs, SCTLR, VBAR, ELR, timers, ...). */
#define EL12_REGS(X) X(s3_5_c2_c0_0) X(s3_5_c2_c0_1) X(s3_5_c2_c0_2) X(s3_5_c10_c2_0) X(s3_5_c1_c0_0) X(s3_5_c10_c3_0) \
	X(s3_5_c1_c0_2) X(s3_5_c12_c0_0) X(s3_5_c4_c0_1) X(s3_5_c4_c0_0) X(s3_5_c5_c2_0) X(s3_5_c6_c0_0) X(s3_5_c5_c1_0) \
	X(s3_5_c5_c1_1) X(s3_5_c13_c0_1) X(s3_5_c14_c1_0) X(s3_5_c14_c2_2) X(s3_5_c14_c2_1) X(s3_5_c14_c3_2) X(s3_5_c14_c3_1)

extern void redispatch(u64 gs_pa, u64 sp) __attribute__((noreturn));

/* what the guest entry tail takes from guest_state, for either world */
void world_save(struct gctx *c, u64 gs)
{
	for (int i = 0; i < 31; i++)
		w64(gs + GS_X0 + i * 8, c->x[i]);
	w64(gs + GS_PC, c->pc);
	w64(gs + GS_PSTATE, c->spsr);
	w64(gs + GS_SP_EL0, sysreg_read(sp_el0));
	w64(gs + GS_SP_EL1, sysreg_read(sp_el1));
	w64(gs + GS_MDSCR, sysreg_read(mdscr_el1));
	w64(gs + GS_TPIDR, sysreg_read(tpidr_el1));
	w64(gs + GS_TPIDR0, sysreg_read(tpidr_el0));
	w64(gs + GS_TPIDRRO, sysreg_read(tpidrro_el0));
	w64(gs + GS_PAR, sysreg_read(par_el1));
	w64(gs + GS_CSSELR, sysreg_read(csselr_el1));
	w64(gs + GS_SCXT, sysreg_read(s3_3_c13_c0_7));
	fp_save(gs + GS_Q0, gs + GS_FPSR, gs + GS_FPCR);
}

void guest_to_host(struct gctx *c)
{
	struct mcpu *s = this_cpu();
	world_save(c, s->gs);
	{
		int n = 0;
#define X(r) s->el12[n++] = sysreg_read(r);
		EL12_REGS(X)
#undef X
	}
	s->actlr = sysreg_read(s3_5_c1_c0_1);
	dsb();
	sptm_call_raw(SPTM_GUEST_EXIT, s2_root, 0, 0, 0, 0, 0);
}

/* world L's CNTP and CNTV, as saved in guest_to_host */
#define EL12_TIMER_FIRST 16
void host_timers_restore(void)
{
	struct mcpu *s = this_cpu();
	sysreg_write(s3_5_c14_c2_2, s->el12[EL12_TIMER_FIRST]);
	sysreg_write(s3_5_c14_c2_1, s->el12[EL12_TIMER_FIRST + 1]);
	sysreg_write(s3_5_c14_c3_2, s->el12[EL12_TIMER_FIRST + 2]);
	sysreg_write(s3_5_c14_c3_1, s->el12[EL12_TIMER_FIRST + 3]);
}

/* everything per-CPU a guest needs before GUEST_DISPATCH (Gravity eeaaa7f, as XNU does) */
void guest_cpu_regs(void)
{
	/* the host's own timers (CNTHV, CNTHP under E2H) must not fire into the guest: its FIQ handler cannot see them (Aura hv_exc.c:585 writes them at EL2) */
	sysreg_write(cntv_ctl_el0, 0);
	sysreg_write(cntp_ctl_el0, 0);
	sysreg_write(vmpidr_el2, sysreg_read(mpidr_el1));
	sysreg_write(vpidr_el2, sysreg_read(midr_el1));
	sysreg_write(s3_5_c15_c1_3, 15);
	/* EVNTEN (bit 2), EVNTI 7 (bits 7:4): an event every 256 counter ticks at EL2. SPTM's waits are WFE in a loop on the counter
	 * (the DART poll: 74 WFE, no SEV); under this monitor IRQ and FIQ route to EL1 (HCR IMO = FMO = 0), so nothing
	 * else is sure to end such a WFE. A spurious wake only costs one more loop. F33, inference, not measured. */
	sysreg_write(cnthctl_el2, (sysreg_read(cnthctl_el2) & ~((1UL << 13) | (0xfUL << 4) | (1UL << 3))) | (1UL << 10) | (1UL << 11) |
		     (1UL << 2) | (7UL << 4));
	isb();
}

void host_to_guest(void)
{
	struct mcpu *s = this_cpu();
	{
		int n = 0;
#define X(r) sysreg_write(r, s->el12[n++]);
		EL12_REGS(X)
#undef X
	}
	/* written only when SPTM changed it: this is the thread's TSO bit */
	if (sysreg_read(s3_5_c1_c0_1) != s->actlr)
		sysreg_write(s3_5_c1_c0_1, s->actlr);
	guest_cpu_regs();
	dsb();
	redispatch(s->gs_pa, s->stack);
}

/* back to world L with x0 = v */
void host_return(u64 v)
{
	w64(this_cpu()->gs + GS_X0, v);
	host_to_guest();
}

#define NATIVE_MAGIC 0x4e4956454e455447UL
extern u64 native_active;

void guest_run(u64 entry_ipa, u64 x0, u64 x1)
{
	struct mcpu *s = this_cpu();
	u64 gs_pa = frame_alloc();
	u64 gs = hmap(gs_pa, 0);
	s->gs_pa = gs_pa;
	s->gs = gs;
	if (!gs) {
		con_puts("mon: guest state page not mappable\n");
		return;
	}
	for (u64 i = 0; i < PAGE; i += 8)
		w64(gs + i, 0);
	w64(gs + GS_ROOT, s2_root);
	w64(gs + GS_CPTR, 0x300000);         /* guest FP/SIMD enabled */
	w64(gs + GS_X0, x0);
	w64(gs + GS_X0 + 8, x1);
	w64(gs + GS_PSTATE, PSTATE_EL1H_MASKED);
	w64(gs + GS_PC, entry_ipa);
	dsb();
	/* the guest's MPIDR/MIDR read these: this core's identity, not stale firmware state; SPTM clears VM_TMR_FIQ_ENA at
	 * boot; guest EL1 may read the physical counter and use the EL1 physical timer (CNTHCTL_EL2, E2H=1) */
	guest_cpu_regs();
	con_puts("mon: VM_TMR_FIQ_ENA reads ");
	con_hex(sysreg_read(s3_5_c15_c1_3));
	con_puts("\n");
	smp_boot_cpu_ready();
	con_puts("mon: GUEST_DISPATCH\n");
	native_active = NATIVE_MAGIC;
	dsb();
	s->stack = (u64)__builtin_frame_address(0) - 0x400;
	u64 r = sptm_call(SPTM_GUEST_DISPATCH, gs_pa, 0, 0, 0, 0, 0);
	con_puts("mon: ep27 returned ");
	con_hex(r);
	con_puts(terminated ? " (after terminate)\n" : "\n");
}

static void terminate(struct gctx *c, const char *why)
{
	con_puts("mon: guest terminate: ");
	con_puts(why);
	con_puts(" pc=");
	con_hex(c->pc);
	con_puts(" esr=");
	con_hex(c->esr);
	con_puts(" EL1 esr=");
	con_hex(sysreg_read(s3_5_c5_c2_0));
	con_puts(" elr=");
	con_hex(sysreg_read(s3_5_c4_c0_1));
	con_puts(" far=");
	con_hex(sysreg_read(s3_5_c6_c0_0));
	con_puts(" sp1=");
	con_hex(sysreg_read(sp_el1));
	con_puts("\n");
	terminated = 1;
	bc_set(this_cpu(), 9);
	fb_band(15, RED);
	/* GUEST_EXIT (ep28) teardown is not called: after FIXUPS_COMPLETE its return lands at 0 (qemu, 2026-10-29) */
	for (;;)
		__asm__ volatile("wfi");
}

/* the guest console is the only channel from Linux userspace: turn its milestones into WDT stages */
int g_tso;                                /* guest verdict: 1 TSO on, 2 off */

static int has(const char *s, const char *w)
{
	for (; *s; s++) {
		const char *a = s, *b = w;
		while (*b && *a == *b)
			a++, b++;
		if (!*b)
			return 1;
	}
	return 0;
}

static void guest_line(char ch)      /* under g_con_lock */
{
	static char line[160];
	static u64 n;
	if (ch != '\n') {
		if (n < sizeof(line) - 1)
			line[n++] = ch;
		return;
	}
	line[n] = 0;
	n = 0;
	if (has(line, "TSO-INIT: userspace reached"))
		fb_band(6, CYAN);
	else if (has(line, "PR_SET_MEM_MODEL_TSO = OK")) {
		g_tso = 1;
		fb_band(7, GREEN);
	} else if (has(line, "PR_SET_MEM_MODEL_TSO = FAIL")) {
		g_tso = 2;
		fb_band(7, RED);
	}
	else if (has(line, "M2: timer ok"))
		fb_band(11, GREEN);
	else if (has(line, "M2: timer stuck"))
		fb_band(11, RED);
	else if (has(line, "M2: past WDT"))
		fb_band(12, GREEN);
	else if (has(line, "NVME: controller up"))
		fb_band(8, GREEN);
	else if (has(line, "NVME-ROOT-OK"))
		fb_band(9, GREEN);
	else if (has(line, "LATE: insmod"))
		fb_band(13, CYAN);
	else if (has(line, "Kernel panic"))
		fb_band(15, MAGENTA);
}

static u64 *reg(struct gctx *c, u64 n, u64 *zero)
{
	if (n == 31) {
		*zero = 0;
		return zero;
	}
	return &c->x[n];
}

/* EC 0x3F ISS 0x20: SPTM's guest HACR_EL2 traps IMPDEF sysregs, the MSR-format syndrome is in AFSR1_EL2
 * (Gravity hv_exc.c:1041). AIC's fast IPI registers go to the hardware (XNU uses them at EL2, Gravity hv_exc.c:889-957);
 * everything else Linux reaches here is the PMC masks of aic_init_cpu and aic_handle_fiq. No PMU node in the DT, so a
 * shadow is enough for those and the real registers stay untouched. */
#define APL_SR(op1, crm, op2) ((3UL << 20) | ((u64)(op2) << 17) | ((u64)(op1) << 14) | (15UL << 10) | ((u64)(crm) << 1))
static void impdef_sysreg(struct gctx *c)
{
	struct mcpu *s = this_cpu();
	static u64 nlog;
	u64 iss = sysreg_read(afsr1_el2), zero;
	u64 *r = reg(c, (iss >> 5) & 0x1f, &zero);
	u64 sr = iss & 0x3ffc1e, i, rd = iss & 1;
	if (sr == APL_SR(5, 0, 0) || sr == APL_SR(5, 0, 1) || sr == APL_SR(5, 1, 1) || sr == APL_SR(5, 3, 1)) {
		u64 v = *r;
		if (sr == APL_SR(5, 0, 0) && !rd)
			__asm__ volatile("msr s3_5_c15_c0_0, %0; isb" :: "r"(v));
		else if (sr == APL_SR(5, 0, 1) && !rd)
			__asm__ volatile("msr s3_5_c15_c0_1, %0; isb" :: "r"(v));
		else if (sr == APL_SR(5, 1, 1))
			rd ? (void)(*r = sysreg_read(s3_5_c15_c1_1)) : (void)({ __asm__ volatile("msr s3_5_c15_c1_1, %0; isb" :: "r"(v)); });
		else if (sr == APL_SR(5, 3, 1))
			rd ? (void)(*r = sysreg_read(s3_5_c15_c3_1)) : (void)({ __asm__ volatile("msr s3_5_c15_c3_1, %0; isb" :: "r"(v)); });
		c->pc += 4;
		return;
	}
	for (i = 0; i < 16 && s->sh[i].sr && s->sh[i].sr != sr; i++)
		;
	if (i == 16)
		terminate(c, "impdef sysreg shadow full");
	s->sh[i].sr = sr;
	if (rd)
		*r = s->sh[i].v;
	else
		s->sh[i].v = *r;
	if (++nlog <= 16) {
		con_puts("mon: impdef sysreg iss=");
		con_hex(iss);
		con_puts(" pc=");
		con_hex(c->pc);
		con_puts("\n");
	}
	c->pc += 4;
}

void mon_guest_exit(struct gctx *c, u64 async)
{
	static u64 nasync;
	static int seen;
	if (mhv_in_vm()) {
		mhv_vm_exit(c, async);
		return;
	}
	if (!seen) {                          /* the guest ran and came back: first exit of any kind */
		seen = 1;
		fb_band(5, CYAN);
	}
	this_cpu()->bc_exits++;
	this_cpu()->bc_ec = c->esr >> 26;
	if (async) {
		if (++nasync <= 4) {
			con_puts("mon: async exit esr=");
			con_hex(c->esr);
			con_puts("\n");
		}
		if (nasync > 1000000)
			terminate(c, "async storm");
		return;
	}
	u64 ec = c->esr >> 26;
	if (ec == 0x01) {                     /* WFI/WFE: no virtual interrupts yet, just continue */
		c->pc += 4;
		return;
	}
	if (ec == 0x3f && (c->esr & 0x1ffffff) == 0x20) {
		impdef_sysreg(c);
		return;
	}
	if (ec == 0x18) {                     /* trapped MSR/MRS */
		/* shadow FPMR (FEAT_FPMR: qemu advertises it, M4 does not) */
		u64 iss = c->esr & 0x1ffffff, zero;
		u64 *r = reg(c, (iss >> 5) & 0x1f, &zero);
		u64 sr = iss & 0x3ffc1e;          /* op0 op2 op1 CRn CRm, direction cleared */
		/* FPMR S3_3_C4_C4_2 is guest-owned FP state; returning 0 on read would corrupt a
		 * read-modify-write (the guest spins). Shadow it so the readback is correct.
		 * HW-portable: no real msr (the reg does not exist on M4, so this never traps there). */
		if (sr == ((3UL << 20) | (2UL << 17) | (3UL << 14) | (4UL << 10) | (4UL << 1))) {
			if (iss & 1)
				*r = this_cpu()->fpmr;
			else
				this_cpu()->fpmr = *r;
			c->pc += 4;
			return;
		}
		/* EL1 registers the guest may own but SPTM traps: write them through the EL12 alias (qemu's cpu
		 * advertises S1PIE/TCR2; the M4 does not, so on hardware these never trap) */
		if (!(iss & 1)) {
			u64 v = *r;
			if (sr == ((3UL << 20) | (2UL << 17) | (10UL << 10) | (2UL << 1)))        /* PIRE0_EL1 */
				__asm__ volatile("msr s3_5_c10_c2_2, %0; isb" :: "r"(v));
			else if (sr == ((3UL << 20) | (3UL << 17) | (10UL << 10) | (2UL << 1)))   /* PIR_EL1 */
				__asm__ volatile("msr s3_5_c10_c2_3, %0; isb" :: "r"(v));
			else if (sr == ((3UL << 20) | (3UL << 17) | (2UL << 10)))                 /* TCR2_EL1 */
				__asm__ volatile("msr s3_5_c2_c0_3, %0; isb" :: "r"(v));
		}
		if (iss & 1) {                    /* read */
			if (sr == ((3UL << 20) | (7UL << 17) | (1UL << 14)))   /* AIDR_EL1 */
				*r = sysreg_read(aidr_el1);
			else
				*r = 0;
		}
		static u64 nsr;
		if (++nsr <= 16) {
			con_puts("mon: sysreg trap iss=");
			con_hex(iss);
			con_puts(" pc=");
			con_hex(c->pc);
			con_puts("\n");
		}
		c->pc += 4;
		return;
	}
	if (ec == 0x16) {                     /* HVC */
		if (smp_hvc(c))
			return;
#ifdef MHV_SELFTEST
		if (c->x[0] == 0x7520) {
			con_puts("mhv-st ");
			con_hex(c->x[1]);
			con_puts(" = ");
			con_hex(c->x[2]);
			con_puts("\n");
			return;
		}
#endif
		if (c->x[0] >= MHV_PROBE && c->x[0] <= MHV_LAST) {
			mhv_hvc(c);
			return;
		}
		if (c->x[0] == 0xDEAD) {
			terminate(c, "hvc 0xDEAD");
		}
#ifdef NVME_SELFTEST
		if (c->x[0] == 0x7502) {             /* round trip with table 6 from the exit path; Linux after it proves the state */
			u64 cur;
			__asm__ volatile("mov %0, sp" : "=r"(cur));
			con_puts("st sp "); con_hex(cur); con_puts(" sp_el0 "); con_hex(sysreg_read(sp_el0));
			con_puts(" sp_el1 "); con_hex(sysreg_read(sp_el1)); con_puts(" x20 "); con_hex(c->x[20]);
			con_puts(" spsel "); con_hex(sysreg_read(spsel)); con_puts("\n");
			guest_to_host(c);
			u64 r0 = sptm_call_raw(0x600000003UL, 253, 2, 0, 0, 0, 0);
			con_puts("st exit path 6/3 r ");
			con_hex(r0);
			con_puts("\n");
			con_puts("st after gs40 "); con_hex((*(volatile u64 *)(this_cpu()->gs + GS_SP_EL1))); con_puts(" gs38 "); con_hex((*(volatile u64 *)(this_cpu()->gs + GS_SP_EL0)));
			con_puts(" spel1 "); con_hex(sysreg_read(sp_el1)); con_puts(" pc "); con_hex((*(volatile u64 *)(this_cpu()->gs + GS_PC))); con_puts(" ps "); con_hex((*(volatile u64 *)(this_cpu()->gs + GS_PSTATE)));
			con_puts(" x20 "); con_hex((*(volatile u64 *)(this_cpu()->gs + GS_X0 + 20*8))); con_puts("\n");
			host_to_guest();
		}
#endif
		if (c->x[0] == 0x7504) {          /* F33: breadcrumb of the core with MPIDR x1, for the guest's silent-core report */
			u64 slot = mpidr_slot(c->x[1]);
			struct mcpu *t;

			if (slot >= MAX_SLOTS) {      /* mpidr_slot() reaches 31, g_cpu has MAX_SLOTS entries */
				c->x[0] = 0xff;
				return;
			}
			t = &g_cpu[slot];
			c->x[0] = (t->bc_stage & 0xff) | (t->bc_ec & 0x3f) << 8;
			c->x[1] = t->bc_sel;
			c->x[2] = t->bc_a0;
			c->x[3] = t->bc_t;
			c->x[4] = t->bc_exits;
			c->x[5] = sysreg_read(cntpct_el0);
			return;
		}
		if (c->x[0] == 0x7505) {          /* F34: T8110 error block of DART x1, instance x2: x0 SID mask (~0 none), x1 status, x2 address, x3 SID */
			/* the first read of a unit maps its error block through SPTM (hmap_dev_ro): never from inside the guest state.
			 * F35.4 HW 11-02: "SPTM PANIC dispatch_state_machine: invalid state transition, STATE_XNU_GUEST, EVENT_CALL_SPTM". */
			u64 id = c->x[1], inst = c->x[2], st = 0, ad = 0, sd = 0, in = 0, m;
			u64 gs = this_cpu()->gs;
			guest_to_host(c);
			spin_lock(&g_apif_lock);
			m = iommu_dart_err(id, inst, &st, &ad, &sd, &in);
			spin_unlock(&g_apif_lock);
			w64(gs + GS_X0, m);
			w64(gs + GS_X0 + 8, st);
			w64(gs + GS_X0 + 16, ad);
			w64(gs + GS_X0 + 24, sd);
			w64(gs + GS_X0 + 32, in);     /* hw version | nsids << 16 */
			host_to_guest();
		}
		if (c->x[0] == 0x7503) {          /* macho-diag.ko: EL2 side of the guest timer */
			c->x[0] = sysreg_read(s3_5_c15_c1_3);
			c->x[1] = sysreg_read(hcr_el2);
			c->x[2] = sysreg_read(hacr_el2);
			c->x[3] = sysreg_read(cnthctl_el2);
			c->x[4] = sysreg_read(cntvoff_el2);
			c->x[5] = sysreg_read(s3_5_c14_c3_1);
			c->x[6] = sysreg_read(s3_5_c14_c2_1);
			return;
		}
		if (c->x[0] == 0x7501) {          /* guest stub: x1 ACTLR at entry, x2-x4 read back after |2, |0x10, |0x12 */
			int ok = (c->x[4] & 0x12) == 0x12;
			con_puts("mon: guest ACTLR_EL1 ");
			con_hex(c->x[1]);
			con_puts(" |2 ");
			con_hex(c->x[2]);
			con_puts(" |10 ");
			con_hex(c->x[3]);
			con_puts(" |12 ");
			con_hex(c->x[4]);
			con_puts("\n");
			fb_band(10, ok ? GREEN : RED);
		}
		return;                           /* ELR already points after the hvc */
	}
	if (ec == 0x24) {                     /* stage-2 data abort: MMIO */
		u64 ipa = ((c->hpfar >> 4) << 12) | (c->far & 0xfff);
		u64 iss = c->esr & 0x1ffffff, zero;
		if (!(iss & (1 << 24)))
			terminate(c, "data abort without ISV");
		u64 *r = reg(c, (iss >> 16) & 0x1f, &zero);
		if (apif_is_reg(ipa)) {               /* APIF v2 provider register page (0x61ff0000) */
			c->pc += 4;
			if (iss & (1 << 6)) {
				u64 v = *r;
				struct mcpu *s = this_cpu();
				guest_to_host(c);
				bc_set(s, 1);
				spin_lock(&g_apif_lock);
				bc_set(s, 2);
				apif_write(ipa, v);
				spin_unlock(&g_apif_lock);
				s->bc_sel = 0;
				bc_set(s, 0);
				host_to_guest();
			}
			spin_lock(&g_apif_lock);
			*r = apif_read(ipa);
			spin_unlock(&g_apif_lock);
			return;
		}
		if ((ipa & ~0xfffUL) == GUART_IPA) {
			if (iss & (1 << 6)) {
				if ((ipa & 0xfff) == 0) {
					spin_lock(&g_con_lock);
					con_putc((char)*r);
					guest_line((char)*r);
					spin_unlock(&g_con_lock);
				}
			} else {
				*r = (ipa & 0xfff) == 0x18 ? 0x90 : 0;
			}
			c->pc += 4;
			return;
		}
		/* a driver on an unmapped page must not end the boot: read 0, drop the write, name it on the screen */
		static u64 nmmio;
		if (++nmmio <= 32) {
			con_puts("mon: unmapped MMIO ipa=");
			con_hex(ipa);
			con_puts(" pc=");
			con_hex(c->pc);
			con_puts("\n");
		}
		if (!(iss & (1 << 6)))
			*r = 0;
		c->pc += 4;
		return;
	}
	terminate(c, "unhandled exit");
}

