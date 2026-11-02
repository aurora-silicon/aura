// SPDX-License-Identifier: GPL-2.0-only
// SMP under the resident SPTM: PSCI over HVC for Linux, per-CPU world state, starting a secondary.
//
// What a secondary needs, on SPTM 26.7.1:
//   reset vector: MPIDR & 0xffff is matched against per_cpu[+0x1410] of slots whose byte +0 is
//   set; no match is a wfe spin (x0 0xdead). So REGISTER_CPU (ep14) first; it panics once stage bit 17 is set.
//   Then the first-start path panics unless bit 17 IS set ("Expected bootstrap stages not reached"), so LOCKDOWN (ep0,
//   the only writer of bit 17) must run between the REGISTER_CPUs and the first start; its own preconditions are bits 19
//   and 20, set at cold boot. The first-start path goes on to (per_cpu[+0x1490] guest save
//   page), (VBAR from the value FIXUPS_COMPLETE latched, TPIDR_EL2, CTRR checks) and a geret to
//   BootKC-entry with x0 = 1. XNU's hook-free order is the same: machine_routines.c:1283, then startup.c:872, then cpu_start.
//
// All of it runs at the first CPU_ON, in the initrd (since F27), not before Linux: a step that stops the machine then stops it with
// the kernel on the screen, and init has already named the CPU.
#include "mon.h"

#define PSCI_VERSION       0x84000000UL
#define PSCI_CPU_OFF       0x84000002UL
#define PSCI_CPU_ON32      0x84000003UL
#define PSCI_CPU_ON        0xc4000003UL
#define PSCI_AFFINITY32    0x84000004UL
#define PSCI_AFFINITY      0xc4000004UL
#define PSCI_MIGRATE_INFO  0x84000006UL
#define PSCI_SYSTEM_RESET  0x84000009UL
#define PSCI_FEATURES      0x8400000aUL
#define R_SUCCESS          0UL
#define R_NOT_SUPPORTED    ((u64)-1)
#define R_INVALID          ((u64)-2)
#define R_ALREADY_ON       ((u64)-4)
#define R_ON_PENDING       ((u64)-5)
#define R_INTERNAL         ((u64)-6)

/* /cpus reg of J616s (ADT, HW, raw boot; t6040.dtsi matches): MPIDR & 0xffff */
static const u16 cpu_regs[] = { 0x000, 0x001, 0x002, 0x003,
                                0x100, 0x101, 0x102, 0x103, 0x104,
                                0x200, 0x201, 0x202, 0x203, 0x204 };
#define NCPUS (sizeof(cpu_regs) / sizeof(cpu_regs[0]))
#define PMGR_CPU_START 0x502888000UL      /* ADT pmgr reg[0] 0x302800000 + arm-io 0x200000000 + CPU_START_OFF_T6031 (m4-loader, Aura smp.c) */
#define WDT_PA 0x50836c000UL

struct mcpu g_cpu[MAX_SLOTS];
u64 g_cpu_top[MAX_SLOTS][2];
static volatile u32 g_psci_lock;
static int g_ready, g_tried;
static u64 g_cpu_start;

extern void guest_cpu_regs(void);

static void kv(const char *k, u64 v)
{
	con_puts(k);
	con_hex(v);
	con_puts("\n");
}

static const u16 *find_reg(u64 mpidr)
{
	if (mpidr & ~0xffffffUL)
		return 0;
	for (u64 i = 0; i < NCPUS; i++)
		if (cpu_regs[i] == (mpidr & 0xffff) && (mpidr >> 16) == (u64)(cpu_regs[i] >= 0x100))
			return &cpu_regs[i];
	return 0;
}

void smp_boot_cpu_ready(void)
{
	struct mcpu *s = this_cpu();
	s->state = PSCI_ON;
	s->started = 1;                           /* a later CPU_ON of this core leaves the park loop, no SPTM reset path */
}

/* the first CPU_ON. Outside the guest (guest_to_host done), only one CPU online. */
static int smp_bootstrap(void)
{
	u64 me = mpidr_slot(sysreg_read(mpidr_el1));
	if (g_tried)                              /* LOCKDOWN cannot run twice */
		return -1;
	g_tried = 1;
	con_puts("mon: smp: per-CPU pages\n");
	for (u64 i = 0; i < NCPUS; i++) {
		u64 slot = mpidr_slot(cpu_regs[i]);
		if (slot == me)
			continue;
		struct mcpu *s = &g_cpu[slot];
		u64 pa = frame_alloc(), exc = frame_alloc(), run = frame_alloc();
		s->gs_pa = pa;
		s->gs = hmap(pa, 0);
		u64 e = hmap(exc, 0), r = hmap(run, 0);
		if (!s->gs || !e || !r) {
			con_puts("mon: smp: per-CPU pages not mappable\n");
			return -1;
		}
		s->state = PSCI_OFF;
		s->stack = r + PAGE - 0x400;
		g_cpu_top[slot][0] = e + PAGE;
		g_cpu_top[slot][1] = r + PAGE;
	}
	for (u64 i = 0; i < NCPUS; i++) {
		if (mpidr_slot(cpu_regs[i]) == me)
			continue;
		kv("mon: smp: REGISTER_CPU ", cpu_regs[i]);
		sptm_call(SPTM_REGISTER_CPU, cpu_regs[i], 0, 0, 0, 0, 0);
	}
	con_puts("mon: smp: LOCKDOWN\n");
	kv("mon: smp: LOCKDOWN returned ", sptm_call(SPTM_LOCKDOWN, 0, 0, 0, 0, 0, 0));
	con_puts("mon: smp: map cpu-start\n");
	g_cpu_start = hmap_dev(PMGR_CPU_START, 1);
	kv("mon: smp: cpu-start va ", g_cpu_start);
	if (!g_cpu_start)
		return -1;
	g_ready = 1;
	return 0;
}

/* same order as m4-loader and Aura smp_start_cpu (HW on J616s raw, m4-loader): system bit, then the core */
static void cpu_start_write(u64 reg)
{
	u64 cluster = reg >> 8, core = reg & 0xff;
	dsb();
	w32(g_cpu_start + 0x4, 1U << (4 * cluster + core));
	w32(g_cpu_start + 0x8 + 4 * cluster, 1U << core);
	dsb();
}

/* Enters Linux's secondary_entry per PSCI: MMU off, x0 = context, EL1h, DAIF masked. Registers SPTM leaves are not Linux's
 * problem except these three. */
void mon_secondary_main(void)
{
	struct mcpu *s = this_cpu();
	u64 gs = s->gs;
	kv("mon: smp: alive, MPIDR ", sysreg_read(mpidr_el1));
	kv("mon: smp: TTBR1 ", sysreg_read(ttbr1_el1));
	for (u64 i = 0; i < PAGE; i += 8)
		w64(gs + i, 0);
	w64(gs + GS_ROOT, s2_root);
	w64(gs + GS_CPTR, 0x300000);
	w64(gs + GS_X0, s->ctx);
	w64(gs + GS_PSTATE, PSTATE_EL1H_MASKED);
	w64(gs + GS_PC, s->entry);
	sysreg_write(s3_5_c1_c0_0, SCTLR_EL1_MMU_OFF);   /* SCTLR_EL12 */
	sysreg_write(s3_5_c1_c0_2, 0x300000);            /* CPACR_EL12 */
	sysreg_write(s3_5_c12_c0_0, 0);                  /* VBAR_EL12 */
	guest_cpu_regs();
	s->started = 1;
	s->state = PSCI_ON;
	dsb();
	u64 r = sptm_call(SPTM_GUEST_DISPATCH, s->gs_pa, 0, 0, 0, 0, 0);
	kv("mon: smp: secondary ep27 returned ", r);
	for (;;)
		__asm__ volatile("wfe");
}

/* CPU_OFF: stays in guest state as far as SPTM knows, so coming back needs no SPTM call: wfe here, then eret into the new
 * entry. Linux has masked everything; the AIC keeps nothing for an offlined core. */
static void cpu_off(struct gctx *c, struct mcpu *s)
{
	s->wake = 0;
	s->state = PSCI_OFF;
	dsb();
	while (!__atomic_load_n(&s->wake, __ATOMIC_ACQUIRE))
		__asm__ volatile("wfe");
	for (int i = 0; i < 31; i++)
		c->x[i] = 0;
	c->x[0] = s->ctx;
	c->pc = s->entry;
	c->spsr = PSTATE_EL1H_MASKED;
	sysreg_write(s3_5_c1_c0_0, SCTLR_EL1_MMU_OFF);
	isb();
	s->state = PSCI_ON;
}

static void system_reset(struct gctx *c)
{
	con_puts("mon: PSCI SYSTEM_RESET\n");
	guest_to_host(c);
	u64 w = hmap_dev(WDT_PA, 1);
	kv("mon: wdt va ", w);
	if (w) {                                  /* as apple_wdt_restart: reset enable, bite now */
		w32(w + 0x1c, 4);
		w32(w + 0x14, 0);
		w32(w + 0x10, 0);
		dsb();
		(void)r32(w + 0x10);
		for (;;)
			__asm__ volatile("wfe");
	}
	/* no mapping: PSCI returns, Linux falls through to the next restart handler, apple_wdt */
	w64(this_cpu()->gs + GS_X0, R_INTERNAL);
	host_to_guest();
}

static void cpu_on(struct gctx *c)
{
	const u16 *reg = find_reg(c->x[1]);
	if (!reg) {
		c->x[0] = R_INVALID;
		return;
	}
	struct mcpu *s = &g_cpu[mpidr_slot(*reg)];
	spin_lock(&g_psci_lock);
	if (g_ready && s->state == PSCI_ON) {
		c->x[0] = R_ALREADY_ON;
	} else if (s == this_cpu()) {
		c->x[0] = R_ALREADY_ON;
	} else if (g_ready && s->state == PSCI_PENDING) {
		c->x[0] = R_ON_PENDING;
	} else if (g_ready && s->started) {       /* parked in cpu_off */
		s->entry = c->x[2];
		s->ctx = c->x[3];
		s->state = PSCI_PENDING;
		dsb();
		__atomic_store_n(&s->wake, 1, __ATOMIC_RELEASE);
		__asm__ volatile("dsb sy; sev");
		c->x[0] = R_SUCCESS;
	} else if (g_ready) {
		s->entry = c->x[2];
		s->ctx = c->x[3];
		s->state = PSCI_PENDING;
		dsb();
		kv("mon: smp: start ", *reg);
		cpu_start_write(*reg);
		c->x[0] = R_SUCCESS;
	} else {                                  /* first one: SPTM calls, so through the exit path */
		c->x[0] = R_SUCCESS;              /* into the guest state page with the rest */
		guest_to_host(c);
		if (smp_bootstrap()) {
			w64(this_cpu()->gs + GS_X0, R_INTERNAL);
		} else {
			s->entry = c->x[2];
			s->ctx = c->x[3];
			s->state = PSCI_PENDING;
			dsb();
			kv("mon: smp: start ", *reg);
			cpu_start_write(*reg);
		}
		spin_unlock(&g_psci_lock);
		host_to_guest();
	}
	spin_unlock(&g_psci_lock);
}

/* 1 = handled. Anything in the SMCCC fast-call space is ours; the monitor's own numbers (0x7501..) are not in it. */
int smp_hvc(struct gctx *c)
{
	u64 fn = c->x[0] & 0xffffffffUL;
	if (!(fn & 0x80000000UL))
		return 0;
	switch (fn) {
	case PSCI_VERSION:
		c->x[0] = 0x10000;                /* 1.0 */
		break;
	case PSCI_FEATURES:
		switch (c->x[1] & 0xffffffffUL) {
		case PSCI_VERSION: case PSCI_CPU_OFF: case PSCI_CPU_ON32: case PSCI_CPU_ON:
		case PSCI_AFFINITY32: case PSCI_AFFINITY: case PSCI_MIGRATE_INFO:
		case PSCI_SYSTEM_RESET: case PSCI_FEATURES:
			c->x[0] = R_SUCCESS;
			break;
		default:
			c->x[0] = R_NOT_SUPPORTED;
		}
		break;
	case PSCI_MIGRATE_INFO:
		c->x[0] = 2;                      /* no trusted OS */
		break;
	case PSCI_AFFINITY: case PSCI_AFFINITY32: {
		const u16 *reg = find_reg(c->x[1]);
		if (!reg || c->x[2])
			c->x[0] = R_INVALID;
		else if (&g_cpu[mpidr_slot(*reg)] == this_cpu())
			c->x[0] = PSCI_ON;
		else if (!g_ready)
			c->x[0] = PSCI_OFF;
		else
			c->x[0] = g_cpu[mpidr_slot(*reg)].state;
		break;
	}
	case PSCI_CPU_ON: case PSCI_CPU_ON32:
		cpu_on(c);
		break;
	case PSCI_CPU_OFF:
		cpu_off(c, this_cpu());
		break;
	case PSCI_SYSTEM_RESET:
		system_reset(c);
		break;
	default:
		c->x[0] = R_NOT_SUPPORTED;
	}
	return 1;
}
