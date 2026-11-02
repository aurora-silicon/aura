// SPDX-License-Identifier: GPL-2.0-only
// Paravirtual KVM backend under the resident SPTM: Linux at guest EL1 (world L) asks by HVC for VMs (own stage-2 root,
// VMID) and vCPU runs (world V). L and V are both SPTM guests, every switch is GUEST_EXIT + GUEST_DISPATCH. Nothing here
// runs before Linux issues the first MHV_* call: no state, no SPTM call on a boot that never enables KVM.
#include "mon.h"
#include "mhv.h"

#define MAX_VMS   16                   /* VMID 3, 5, .. 33: the root-table retype wants vmid & 0xff01 == 1 (violation 5); 1 is world L */
#define L2_SLOTS  64                   /* root: 42-bit IPA, 64 GiB per entry, sptm geometry 3 */
#define WIN_PAGES 4
#define WIN_PER_PAGE (PAGE / 16)
#define WIN_SLOTS (WIN_PAGES * WIN_PER_PAGE)   /* 32 MiB windows per VM: 128 GiB of IPA. muvm asks 80 % of RAM and a GPU window of all of it */
#define L3SPAN    (2048 * PAGE)
#define IPA_LIMIT (1UL << 42)
#define UNMAP_CHUNK 64                 /* the region ops validate at most 0x40 pages per call; more is violation 10 */
#define SPTM_UNMAP_TABLE  4
#define SPTM_UNMAP_REGION 7

struct win { u64 ipa, l3; };
struct vm { u64 root, vmid, wins_pa[WIN_PAGES], used; u64 l2[L2_SLOTS]; };
static struct vm vms[MAX_VMS];
static u64 free_frames[4096], nfree;                    /* FRAME_POOL is 64 MiB: a release never finds this full */

/* per CPU once the smp stream makes the monitor per CPU: the run page while world V is live on this core */
static u64 cur_gs_a[MAX_SLOTS];
static struct vm *cur_vm_a[MAX_SLOTS];
#define cur_gs cur_gs_a[mpidr_slot(sysreg_read(mpidr_el1))]
#define cur_vm cur_vm_a[mpidr_slot(sysreg_read(mpidr_el1))]
static volatile u32 mhv_lock_word;

static void mhv_lock(void)
{
	while (__atomic_exchange_n(&mhv_lock_word, 1, __ATOMIC_ACQUIRE))
		__asm__ volatile("wfe");
}

static void mhv_unlock(void)
{
	__atomic_store_n(&mhv_lock_word, 0, __ATOMIC_RELEASE);
	__asm__ volatile("sev");
}

extern u64 g_gpa;
extern u64 g_ram_size;                  /* linux.c, measured at boot */
#define RAM_SIZE g_ram_size
static int in_guest_ram(u64 pa, u64 size)
{
	return pa >= g_gpa && pa + size <= g_gpa + RAM_SIZE && pa + size > pa;
}

static u64 raw(u64 id, u64 a0, u64 a1, u64 a2, u64 a3)
{
	return sptm_call_raw(id, a0, a1, a2, a3, 0, 0);
}

static void frame_release(u64 f)
{
	if (nfree < sizeof(free_frames) / sizeof(free_frames[0]))
		free_frames[nfree++] = f;
}

/* RETYPE zeroes nothing: it only runs IC IVAU over pages of types flagged 0x2020; the pool frames are reused */
static u64 table_frame(void)
{
	u64 f = nfree ? free_frames[--nfree] : frame_alloc_pool();
	u64 va = f ? hmap(f, 0) : 0;
	if (!va) {
		if (f)
			frame_release(f);
		return 0;
	}
	for (u64 i = 0; i < PAGE; i += 8)
		w64(va + i, 0);
	dsb();
	return f;
}

static struct vm *vm_of(u64 handle)
{
	if (!handle || handle > MAX_VMS || !vms[handle - 1].used)
		return 0;
	return &vms[handle - 1];
}

static struct win *win_at(struct vm *v, u64 h)
{
	return (struct win *)hmap(v->wins_pa[h / WIN_PER_PAGE], 0) + h % WIN_PER_PAGE;
}

static struct win *win_find(struct vm *v, u64 ipa, int create)
{
	u64 base = ipa & ~(L3SPAN - 1), h = (base >> 25) % WIN_SLOTS;
	for (u64 n = 0; n < WIN_SLOTS; n++, h = (h + 1) % WIN_SLOTS) {
		struct win *w = win_at(v, h);
		if (w->l3 && w->ipa == base)
			return w;
		if (!w->l3) {
			if (!create)
				return 0;
			u64 t = table_frame();
			if (!t)
				return 0;
			raw(SPTM_RETYPE, t, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 3);
			raw(SPTM_MAP_TABLE, v->root, base, 2, t | 3);
			w->ipa = base;
			w->l3 = t;
			dsb();
			return w;
		}
	}
	return 0;
}

static long vm_create(void)
{
	for (u64 i = 0; i < MAX_VMS; i++) {
		struct vm *v = &vms[i];
		if (v->used)
			continue;
		u64 r = table_frame();
		if (!r)
			return MHV_E_NOMEM;
		for (u64 p = 0; p < WIN_PAGES; p++)
			if (!(v->wins_pa[p] = table_frame())) {
				while (p--)
					frame_release(v->wins_pa[p]);
				frame_release(r);
				return MHV_E_NOMEM;
			}
		v->vmid = 3 + 2 * i;
		raw(SPTM_RETYPE, r, XNU_DEFAULT, XNU_STAGE2_ROOT_TABLE, (v->vmid << 32) | 3);
		v->root = r;
		v->used = 1;
		return i + 1;
	}
	return MHV_E_BUSY;
}

static void unmap_range(struct vm *v, u64 ipa, u64 size)
{
	u64 end = ipa + size;
	while (ipa < end) {
		u64 next = (ipa + L3SPAN) & ~(L3SPAN - 1);
		if (next > end)
			next = end;
		if (win_find(v, ipa, 0))
			for (; ipa < next; ) {
				u64 n = (next - ipa) / PAGE;
				if (n > UNMAP_CHUNK)
					n = UNMAP_CHUNK;
				raw(SPTM_UNMAP_REGION, v->root, ipa, n, 0);   /* TLBI inside while options bit 8 is clear ; with bit 8 it returns 5 instead */
				ipa += n * PAGE;
			}
		ipa = next;
	}
}

static long vm_destroy(struct vm *v)
{
	for (u64 h = 0; h < WIN_SLOTS; h++) {
		struct win *w = win_at(v, h);
		if (!w->l3)
			continue;
		unmap_range(v, w->ipa, L3SPAN);
		raw(SPTM_UNMAP_TABLE, v->root, w->ipa, 2, 0);
		raw(SPTM_RETYPE, w->l3, XNU_STAGE2_PAGE_TABLE, XNU_DEFAULT, 0);
		frame_release(w->l3);
		w->l3 = 0;
	}
	for (u64 i = 0; i < L2_SLOTS; i++) {
		if (!v->l2[i])
			continue;
		raw(SPTM_UNMAP_TABLE, v->root, i << 36, 1, 0);
		raw(SPTM_RETYPE, v->l2[i], XNU_STAGE2_PAGE_TABLE, XNU_DEFAULT, 0);
		frame_release(v->l2[i]);
		v->l2[i] = 0;
	}
	raw(SPTM_RETYPE, v->root, XNU_STAGE2_ROOT_TABLE, XNU_DEFAULT, 0);  /* the retype out does tlbi vmalls12e1isnxs and clears the VMID bit */
	frame_release(v->root);
	for (u64 p = 0; p < WIN_PAGES; p++)
		frame_release(v->wins_pa[p]);
	v->used = 0;
	return 0;
}

static long vm_map(struct vm *v, u64 ipa, u64 pa, u64 size, u64 attr)
{
	if (((ipa | pa | size) & (PAGE - 1)) || !size || ipa + size > IPA_LIMIT || (attr & ~MHV_ATTR_MASK) ||
	    !in_guest_ram(pa, size))
		return MHV_E_INVAL;
	for (u64 off = 0; off < size; off += PAGE) {
		u64 a = ipa + off, l = a >> 36;
		if (!v->l2[l]) {
			u64 t = table_frame();
			if (!t)
				return MHV_E_NOMEM;
			raw(SPTM_RETYPE, t, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 2);
			raw(SPTM_MAP_TABLE, v->root, l << 36, 1, t | 3);
			v->l2[l] = t;
		}
		if (!win_find(v, a, 1))
			return MHV_E_NOMEM;
		u64 st = raw(SPTM_MAP_PAGE, v->root, a, (pa + off) | 3 | 0x800 | attr, 0);
		if (st > 1)                      /* 6: another PA is mapped here, unchanged */
			return MHV_E_SPTM;
	}
	return 0;
}

/* permissions of a valid page: MAP_PAGE with the same PA replaces AP and XN and invalidates */
static long vm_relax(struct vm *v, u64 ipa, u64 attr)
{
	struct win *w;
	if ((ipa & (PAGE - 1)) || ipa >= IPA_LIMIT || (attr & ~MHV_ATTR_MASK) || !(w = win_find(v, ipa, 0)))
		return MHV_E_INVAL;
	u64 va = hmap(w->l3, 0);
	if (!va)
		return MHV_E_INVAL;
	u64 pte = *(volatile u64 *)(va + ((ipa >> 14) & 0x7ff) * 8);
	if ((pte & 3) != 3)
		return 0;                        /* unmapped meanwhile: the guest faults again */
	/* pgtable.c stage2_attr_walker: the first execute permission invalidates the I-cache, CTR_EL0.DIC is 0 here */
	if ((pte & (3UL << 53)) && !(attr & (3UL << 53)))
		__asm__ volatile("ic ialluis; dsb ish; isb");
	u64 st = raw(SPTM_MAP_PAGE, v->root, ipa, (pte & 0xfffffffff000UL) | 3 | 0x800 | attr, 0);
	return st > 1 ? MHV_E_SPTM : 0;
}

#define RUNP(gs) ((struct mhv_run *)((gs) + MHV_RUN_OFF))
#define gs_hcr_vse(gs) (*(volatile u64 *)((gs) + MHV_GS_HCR) & (1UL << 8))

static void ich_load(struct mhv_run *r)
{
#ifdef MHV_NO_ICH
	return;
#endif
	u64 nap = 1UL << (((sysreg_read(ich_vtr_el2) >> 26) & 7) + 1 - 5);   /* PREbits, as vtr_to_nr_apr_regs */
	sysreg_write(ich_hcr_el2, r->ich_hcr);
	sysreg_write(ich_vmcr_el2, r->ich_vmcr);
	/* all of them, as XNU 0xb9422e4: the reset value is UNKNOWN and sptm___vgic_v3_init_lrs does nothing */
	for (u64 i = 0; i <= (sysreg_read(ich_vtr_el2) & 0x1f); i++)
		mhv_set_lr(i < r->used_lrs ? r->lr[i] : 0, i);
	for (u64 i = 0; i < nap && i < 4; i++) {
		mhv_set_ap0r(r->ap0r[i], i);
		mhv_set_ap1r(r->ap1r[i], i);
	}
}

static void ich_save(struct mhv_run *r)
{
#ifdef MHV_NO_ICH
	return;
#endif
	u64 nap = 1UL << (((sysreg_read(ich_vtr_el2) >> 26) & 7) + 1 - 5);   /* PREbits, as vtr_to_nr_apr_regs */
	u64 elrsr = sysreg_read(ich_elrsr_el2);
	for (u64 i = 0; i < r->used_lrs && i < 16; i++) {
		if (elrsr & (1UL << i))
			r->lr[i] &= ~(3UL << 62);    /* ICH_LR_STATE */
		else
			r->lr[i] = mhv_get_lr(i);
		mhv_set_lr(0, i);
	}
	r->ich_vmcr = sysreg_read(ich_vmcr_el2);
	u64 hcr = sysreg_read(ich_hcr_el2);
	r->ich_hcr = (r->ich_hcr & ~(0x1fUL << 27)) | (hcr & (0x1fUL << 27));   /* EOIcount */
	for (u64 i = 0; i < nap && i < 4; i++) {
		r->ap0r[i] = mhv_get_ap0r(i);
		r->ap1r[i] = mhv_get_ap1r(i);
	}
	sysreg_write(ich_hcr_el2, 0);
	isb();
}

/* World L's MDCR and FGT are what the core reset to: iBoot, SPTM outside its SK switch and KC outside a vCPU load never write them,
 * and CNTHCTL keeps every bit guest_cpu_regs does not set. Nobody has read them on this machine: a line per distinct set. */
static void l_state_log(const u64 *t)
{
	static u64 last[MAX_SLOTS][8], shown[16][8];
	static u32 nshown;
	u64 v[8] = { t[0], t[1], t[2], t[3], t[4], t[5], sysreg_read(cnthctl_el2), sysreg_read(s3_4_c15_c12_6) };
	u64 *l = last[mpidr_slot(sysreg_read(mpidr_el1))];
	int changed = 0;

	/* every vCPU entry comes through here: no lock on the common path, and two sets of values on different cores must not flood the panel */
	for (int i = 0; i < 8; i++)
		changed |= l[i] != v[i];
	if (!changed)
		return;
	for (int i = 0; i < 8; i++)
		l[i] = v[i];
	mhv_lock();
	changed = nshown < 16;
	for (u32 s = 0; s < nshown && changed; s++) {
		int same = 1;
		for (int i = 0; i < 8; i++)
			same &= shown[s][i] == v[i];
		changed = !same;
	}
	if (changed)
		for (int i = 0; i < 8; i++)
			shown[nshown][i] = v[i];
	nshown += changed;
	mhv_unlock();
	if (!changed)
		return;
	con_puts("mon: L mdcr ");
	con_hex(v[0]);
	con_puts(" hfg ");
	con_hex(v[1]);
	con_puts(" ");
	con_hex(v[2]);
	con_puts(" ");
	con_hex(v[3]);
	con_puts(" hdfg ");
	con_hex(v[4]);
	con_puts(" ");
	con_hex(v[5]);
	con_puts(" cnthctl ");
	con_hex(v[6]);
	con_puts(" c12_6 ");
	con_hex(v[7]);
	con_puts("\n");
}

/* world L -> world V. L is already out of the guest state machine; only guest_to_host's GUEST_EXIT has run. */
static void enter_vm(u64 run_pa, struct vm *v)
{
	u64 gs = hmap(run_pa, 0);
	struct mhv_run *r = RUNP(gs);
	cur_gs = gs;
	cur_vm = v;
	w64(gs + MHV_GS_ROOT, v->root);
	w64(gs + MHV_GS_CPTR, 0x10300000);   /* FPEN, TTA as XNU: SVE and SME trap and KVM refuses them */
	/* MHV_GS_HACR comes from KVM: bit 56 (PMUv3 registers trap as EC 0x3F, kernel cpu_errata.c, Aura nv2.c). SPTM ORs it in at guest entry */
	host_timers_restore();                /* world L's CNTV stays armed in world V: its FIQ brings the core back */
	sysreg_write(s3_5_c2_c0_0, r->el1[MHV_TTBR0]);
	sysreg_write(s3_5_c2_c0_1, r->el1[MHV_TTBR1]);
	sysreg_write(s3_5_c2_c0_2, r->el1[MHV_TCR]);
	sysreg_write(s3_5_c10_c2_0, r->el1[MHV_MAIR]);
	sysreg_write(s3_5_c1_c0_0, r->el1[MHV_SCTLR]);
	sysreg_write(s3_5_c10_c3_0, r->el1[MHV_AMAIR]);
	sysreg_write(s3_5_c1_c0_2, r->el1[MHV_CPACR]);
	sysreg_write(s3_5_c12_c0_0, r->el1[MHV_VBAR]);
	sysreg_write(s3_5_c4_c0_1, r->el1[MHV_ELR]);
	sysreg_write(s3_5_c4_c0_0, r->el1[MHV_SPSR]);
	sysreg_write(s3_5_c5_c2_0, r->el1[MHV_ESR]);
	sysreg_write(s3_5_c6_c0_0, r->el1[MHV_FAR]);
	sysreg_write(s3_5_c5_c1_0, r->el1[MHV_AFSR0]);
	sysreg_write(s3_5_c5_c1_1, r->el1[MHV_AFSR1]);
	sysreg_write(s3_5_c13_c0_1, r->el1[MHV_CONTEXTIDR]);
	sysreg_write(s3_5_c14_c1_0, r->el1[MHV_CNTKCTL]);
	sysreg_write(s3_5_c1_c0_1, r->el1[MHV_ACTLR]);  /* the vCPU's TSO bit: HCR.TACR is clear, the guest writes it itself */
	sysreg_write(vmpidr_el2, r->vmpidr);
	sysreg_write(vpidr_el2, r->vpidr);
	{
		u64 *t = this_cpu()->l_trap;
		t[0] = sysreg_read(mdcr_el2);
		t[1] = sysreg_read(s3_4_c1_c1_4);
		t[2] = sysreg_read(s3_4_c1_c1_5);
		t[3] = sysreg_read(s3_4_c1_c1_6);
		t[4] = sysreg_read(s3_4_c3_c1_4);
		t[5] = sysreg_read(s3_4_c3_c1_5);
		l_state_log(t);
	}
	sysreg_write(mdcr_el2, r->mdcr);
	sysreg_write(s3_4_c1_c1_4, r->hfgrtr);
	sysreg_write(s3_4_c1_c1_5, r->hfgwtr);
	sysreg_write(s3_4_c1_c1_6, r->hfgitr);
	sysreg_write(s3_4_c3_c1_4, r->hdfgrtr);
	sysreg_write(s3_4_c3_c1_5, r->hdfgwtr);
	/* EL1TVT (FEAT_ECV) and no EL1PTEN: guest CNTV_* and CNTP_* trap and KVM emulates them, CNTVCT is the real counter.
	 * The hardware timers stay world L's. GUEST_EXIT resets none of the above: host_to_guest restores the EL12
	 * registers, VMPIDR, VPIDR and this one, mhv_vm_exit the traps in l_trap */
	sysreg_write(cnthctl_el2, (sysreg_read(cnthctl_el2) & ~((1UL << 10) | (1UL << 11) | (1UL << 13))) | r->cnthctl_set);
	sysreg_write(s3_5_c15_c1_3, 15);
	if (gs_hcr_vse(gs))
		sysreg_write(s3_4_c5_c2_3, r->vsesr);   /* VSESR_EL2: ___activate_traps, RAS is 1 here */
	ich_load(r);
	isb();
	dsb();
	redispatch(run_pa, this_cpu()->stack);
}

void mhv_hvc(struct gctx *c)
{
	u64 fn = c->x[0];
	long ret = MHV_E_INVAL;
	struct vm *v;

	if (fn == MHV_PROBE) {
		c->x[0] = MHV_MAGIC;
#ifndef MHV_NO_ICH
		c->x[1] = sysreg_read(ich_vtr_el2);
#endif
		c->x[3] = sysreg_read(id_aa64mmfr0_el1);
		return;                          /* no SPTM call: resume by eret */
	}
	if (fn == MHV_RUN) {
		u64 run_pa = c->x[1];
		if ((run_pa & (PAGE - 1)) || !in_guest_ram(run_pa, PAGE) || !(v = vm_of(RUNP(hmap(run_pa, 0))->handle))) {
			c->x[0] = MHV_EXIT_BAD;
			return;
		}
		guest_to_host(c);
		enter_vm(run_pa, v);             /* no return: the exit resumes L with x0 = exit code */
	}
	guest_to_host(c);
	mhv_lock();
	if (fn == MHV_VM_CREATE) {
		ret = vm_create();
	} else if ((v = vm_of(c->x[1]))) {
		if (fn == MHV_VM_DESTROY)
			ret = vm_destroy(v);
		else if (fn == MHV_MAP)
			ret = vm_map(v, c->x[2], c->x[3], c->x[4], c->x[5]);
		else if (fn == MHV_UNMAP) {
			if (!((c->x[2] | c->x[3]) & (PAGE - 1)) && c->x[2] + c->x[3] <= IPA_LIMIT) {
				unmap_range(v, c->x[2], c->x[3]);
				ret = 0;
			}
		} else if (fn == MHV_RELAX)
			ret = vm_relax(v, c->x[2], c->x[3]);
	}
	mhv_unlock();
	host_return((u64)ret);
}

int mhv_in_vm(void)
{
	return cur_gs != 0;
}

#define FSC(esr)        ((esr) & 0x3f)
#define IS_TRANS(esr)   ((FSC(esr) & 0x3c) == 0x04)
#define IS_AF(esr)      ((FSC(esr) & 0x3c) == 0x08)
#define IS_PERM(esr)    ((FSC(esr) & 0x3c) == 0x0c)
#define IS_ADDRSZ(esr)  ((FSC(esr) & 0x3c) == 0x00)
#define S1PTW           (1UL << 7)
#define FNV             (1UL << 10)

/* HPFAR is only valid for these faults, else walk stage 1 (kvm hyp/include/hyp/fault.h __get_fault_info). The guest's
 * translation regime is live here. Returns 0 when stage 1 changed under us: re-enter and let it fault again. */
static int fault_ipa(struct gctx *c, struct mhv_run *r)
{
	u64 esr = c->esr, fsc = FSC(esr), hpfar;
	int sea_walk = (fsc & 0x3c) == 0x14 || fsc == 0x13 || (fsc & 0x3c) == 0x1c;
	r->hpfar = 0;
	if (IS_TRANS(esr) || IS_AF(esr) || IS_ADDRSZ(esr) || ((esr & S1PTW) && IS_PERM(esr)))
		hpfar = c->hpfar;
	else if (sea_walk || (fsc == 0x10 && (esr & FNV)))
		return 1;                        /* not translatable, HPFAR invalid: KVM handles it as an SEA */
	else {
		u64 old = sysreg_read(par_el1), par;
		__asm__ volatile("at s1e1r, %0; isb" :: "r"(c->far));
		par = sysreg_read(par_el1);
		sysreg_write(par_el1, old);
		if (par & 1)
			return 0;
		hpfar = ((par >> 12) & 0xfffffffffUL) << 4;
	}
	r->hpfar = hpfar | (1UL << 63);      /* HPFAR_EL2_NS: KVM's "valid" marker */
	return 1;
}

/* PMCCNTR_EL0 read, Op0 3 Op1 3 CRn 9 CRm 13 Op2 0 Dir 1: Windows 2.4 M of them to the Setup page, each else a full exit.
 * The state stays kvm_sptm_pmu_access's. EL0 without EN or CR goes on to KVM: UNDEF. */
static int pmccntr_read(struct gctx *c, struct mhv_run *r)
{
	u64 rt = (c->esr >> 5) & 0x1f, v;

	if ((c->esr & 0x3ffc1f) != 0x30e41b || (!(c->spsr & 0xf) && !(r->pmuserenr & 5)))
		return 0;
	if (r->pmc_on) {
		__asm__ volatile("isb");
		v = sysreg_read(cntpct_el0) - r->pmc_val;
	} else
		v = r->pmc_val;
	if (rt != 31)
		c->x[rt] = v;
	c->pc += 4;
	return 1;
}

/* An exit of world V. Everything but the ICC trap and PMCCNTR ends in Linux; resume-by-return keeps the world live. */
void mhv_vm_exit(struct gctx *c, u64 async)
{
	u64 gs = cur_gs;
	struct mhv_run *r = RUNP(gs);
	u64 code = MHV_EXIT_TRAP;

	if (async) {
		code = async == 3 ? MHV_EXIT_SERROR : MHV_EXIT_IRQ;
	} else {
		u64 ec = c->esr >> 26;
		/* XNU's guest exception handler: only ISS 0x20 carries a sysreg ISS in AFSR1. SPTM copies AFSR1_GL1 there.
		 * The other ISS values of EC 0x3F go on as they are and KVM makes them UNDEF. */
		if (ec == 0x3f && (c->esr & 0x1ffffff) == 0x20) {
			c->esr = (0x18UL << 26) | (1UL << 25) | (sysreg_read(afsr1_el2) & 0x1ffffff);
			ec = 0x18;
		}
		if (ec == 0x18 && mhv_cpuif_access(c, r))
			return;                      /* a vGIC register on a machine with a broken SEIS, all of them trap */
		if (ec == 0x18 && pmccntr_read(c, r))
			return;
		if ((ec == 0x20 || ec == 0x24) && !fault_ipa(c, r))
			return;
	}
	r->esr = c->esr;
	r->far = c->far;
	r->hcr = sysreg_read(hcr_el2);
	ich_save(r);
	r->el1[MHV_TTBR0] = sysreg_read(s3_5_c2_c0_0);
	r->el1[MHV_TTBR1] = sysreg_read(s3_5_c2_c0_1);
	r->el1[MHV_TCR] = sysreg_read(s3_5_c2_c0_2);
	r->el1[MHV_MAIR] = sysreg_read(s3_5_c10_c2_0);
	r->el1[MHV_SCTLR] = sysreg_read(s3_5_c1_c0_0);
	r->el1[MHV_AMAIR] = sysreg_read(s3_5_c10_c3_0);
	r->el1[MHV_CPACR] = sysreg_read(s3_5_c1_c0_2);
	r->el1[MHV_VBAR] = sysreg_read(s3_5_c12_c0_0);
	r->el1[MHV_ELR] = sysreg_read(s3_5_c4_c0_1);
	r->el1[MHV_SPSR] = sysreg_read(s3_5_c4_c0_0);
	r->el1[MHV_ESR] = sysreg_read(s3_5_c5_c2_0);
	r->el1[MHV_FAR] = sysreg_read(s3_5_c6_c0_0);
	r->el1[MHV_AFSR0] = sysreg_read(s3_5_c5_c1_0);
	r->el1[MHV_AFSR1] = sysreg_read(s3_5_c5_c1_1);
	r->el1[MHV_CONTEXTIDR] = sysreg_read(s3_5_c13_c0_1);
	r->el1[MHV_CNTKCTL] = sysreg_read(s3_5_c14_c1_0);
	r->el1[MHV_ACTLR] = sysreg_read(s3_5_c1_c0_1);
	world_save(c, gs);
	dsb();
	raw(SPTM_GUEST_EXIT, cur_vm->root, 0, 0, 0);
	cur_gs = 0;
	cur_vm = 0;
	{
		u64 *t = this_cpu()->l_trap;
		sysreg_write(mdcr_el2, t[0]);
		sysreg_write(s3_4_c1_c1_4, t[1]);
		sysreg_write(s3_4_c1_c1_5, t[2]);
		sysreg_write(s3_4_c1_c1_6, t[3]);
		sysreg_write(s3_4_c3_c1_4, t[4]);
		sysreg_write(s3_4_c3_c1_5, t[5]);
	}
	host_return(code);
}
