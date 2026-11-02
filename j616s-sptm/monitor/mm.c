// SPDX-License-Identifier: GPL-2.0-only
// Frames, host window mappings and the guest stage-2, all through SPTM (XNU domain calls).
#include "mon.h"

#define HWIN     0xfffffe0040000000UL     /* first candidate host window; one L3 table per 32 MiB */
#define HWIN_TRIES 256                    /* candidate 32 MiB windows (8 GiB of VA) */
#define ST_SUCCESS   0                    /* sptm_map_table/map_page status */
#define ST_MAP_VALID 1
#define L3SPAN   (2048 * PAGE)
#define PTE_HOST 0x0060000000000403UL     /* page, AF, PXN, UXN */
#define PTE_SH   (3UL << 8)

static u64 first_frame, frame_lim;
u64 g_ba_top;                         /* boot_args physBase + memSize */
static u64 next_frame, hroot, hva, hl3_end, attr_dev, attr_mem, spare_l3;
u64 g_sptm_err, g_hwin, g_hwin_skipped;
u64 s2_root, g_s2_bad, g_s2_first;
static u64 s2_l2[40];                 /* L2 (per 64 GiB) tables; index = ipa>>36, up to ~2.5 TiB IPA */
static u64 ident_base;
static u64 ident_lim;                 /* top of the reserved identity window */

static u64 mair_index(int device)
{
	u64 mair = sysreg_read(mair_el1);
	for (u64 i = 0; i < 8; i++, mair >>= 8) {
		u64 b = mair & 0xff;
		if (device ? !(b & 0xf0) : b == 0xff)
			return i << 2;
	}
	return (device ? 3UL : 0UL) << 2;
}

/* libsptm_state inside the bootstrap args (+0x1a0, qemu gdb 2026-10-30): PAPT count pointer, PAPT */
#define B_PAPT_N  ((0x1a0 + 0x08) / 8)
#define B_PAPT    ((0x1a0 + 0x10) / 8)
/* phystokv the way XNU does it: the physmap is the PAPT {pa, va, pages}, not linear */
static const u64 *papt;
static u64 papt_n;
static u64 *g_boot;
#define B_PMEND   2                         /* sptm_bootstrap_args_xnu.physmap_end (+0x10), what XNU uses */
#define L_ROOT    ((0x1a0 + 0x38) / 8)      /* libsptm_state root_table_paddr */
#define OA_MASK   0x0000FFFFFFFFC000UL
static u64 *g_boot;
#define L_PMEND   ((0x1a0 + 0x30) / 8)      /* libsptm_state physmap_end */
#define L_ROOT    ((0x1a0 + 0x38) / 8)      /* libsptm_state root_table_paddr */
#define OA_MASK   0x0000FFFFFFFFC000UL

void mm_init(u64 *bootstrap)
{
	g_boot = bootstrap;
	next_frame = (bootstrap[3] + PAGE - 1) & ~(PAGE - 1);   /* first_avail_phys */
	first_frame = next_frame;
	papt_n = *(const u64 *)bootstrap[B_PAPT_N];
	papt = (const u64 *)bootstrap[B_PAPT];
	hroot = sysreg_read(ttbr1_el1) & 0x0000FFFFFFFFC000UL;
	hva = HWIN;
	hl3_end = HWIN;
	attr_dev = mair_index(1);
	attr_mem = mair_index(0) | PTE_SH;
}

u64 g_sptm_calls;

u64 frames_used(void)
{
	return (next_frame - first_frame) / PAGE;
}

u64 frame_alloc(void)
{
	u64 f = __atomic_fetch_add(&next_frame, PAGE, __ATOMIC_RELAXED);
	if (frame_lim && f >= frame_lim) {       /* the pool ran into guest RAM: stop with a name, do not hand out a guest frame */
		con_puts("mon: frame pool exhausted\n");
		for (;;)
			__asm__ volatile("wfi");
	}
	return f;
}

/* End of usable RAM: the PAPT range holding the frame pool (managed DRAM, HW F1: its end = physBase + memSize = 0x10bccb8c000), cut at
 * physBase + memSize and at the boot fb. iBoot's carveouts (fb, DCP, GPU, ...) sit between that and dram-base + dram-size. */
u64 ram_top(u64 fb_pa)
{
	u64 top = 0;
	const u64 *e = papt;
	for (u64 i = 0; i < papt_n; i++, e += 3)
		if (next_frame >= e[0] && next_frame < e[0] + e[2] * PAGE)
			top = e[0] + e[2] * PAGE;
	if (g_ba_top && (!top || g_ba_top < top))
		top = g_ba_top;
	if (fb_pa > next_frame && fb_pa < top)
		top = fb_pa;
	return top & ~(PAGE - 1);
}

/* a frame for the guest's use, or 0 when the pool below the guest window is used up */
u64 frame_alloc_checked(void)
{
	if (next_frame + PAGE > ident_base)
		return 0;
	return frame_alloc();
}

/* KVM's table frames, after Linux runs: the rest of the pool under the guest window */
extern u64 g_gpa;
u64 frame_alloc_pool(void)
{
	return next_frame + PAGE <= g_gpa ? frame_alloc() : 0;
}

/* The guest window sits above a 64 MiB frame pool (L3 tables, monitor frames), identity IPA==PA for NVMe and RTKit DMA */
#define FRAME_POOL (64UL << 20)
u64 carve_identity(u64 size)
{
	static u64 base;
	if (!base)
		base = (next_frame + FRAME_POOL + 0x1fffffUL) & ~0x1fffffUL;
	frame_lim = base;
	ident_lim = base + size;
	ident_base = base;
	return base;
}

/* Claim the next 32 MiB window whose L2 slot is empty and install our own L3 table there. A table
 * that is already present belongs to SPTM's boot-time kernel layout, which moves with the iBoot
 * slide; its frame type is not ours (type 9 at a slid layout), and MAP_PAGE into it is a terminal
 * SPTM violation. So only a window where MAP_TABLE succeeds is used. */
static int hwin_claim(void)
{
	if (!spare_l3) {
		spare_l3 = frame_alloc();
		sptm_call(SPTM_RETYPE, spare_l3, XNU_DEFAULT, XNU_PAGE_TABLE, 3, 0, 0);
	}
	for (u64 i = 0; i < HWIN_TRIES; i++, hl3_end += L3SPAN) {
		/* stay inside the 64 GiB L1 block that holds the kernel image: its L0/L1 path exists,
		 * a missing L1 on the walk would be a terminal violation (0x23) */
		if ((hl3_end ^ HWIN) >> 36)
			break;
		u64 st = sptm_call(SPTM_MAP_TABLE, hroot, hl3_end, 2, spare_l3 | 3, 0, 0);
		if (st == ST_SUCCESS) {
			if (!g_hwin)
				g_hwin = hl3_end;
			hva = hl3_end;
			hl3_end += L3SPAN;
			spare_l3 = 0;
			return 1;
		}
		g_hwin_skipped++;
		g_sptm_err = (st << 8) | 2;
	}
	return 0;
}

/* start the next mapping at a fresh window (keeps a large object virtually contiguous) */
void hmap_new_window(void)
{
	hva = hl3_end;
}

/* DRAM goes through XNU's physmap: no SPTM call and no edit of the live kernel table tree, which is the part of
 * the map path that hangs on HW (v15-v19). Devices have no physmap alias; only QEMU_CON still maps one (the UART). */
u64 hmap(u64 pa, int device)
{
	if (!device) {
		const u64 *e = papt;
		for (u64 i = 0; i < papt_n; i++, e += 3)
			if (pa >= e[0] && pa < e[0] + e[2] * PAGE)
				return e[1] + (pa - e[0]);
		return 0;
	}
#ifndef QEMU_CON
	return 0;
#endif
	if (hva >= hl3_end && !hwin_claim())
		return 0;
	u64 st = sptm_call(SPTM_MAP_PAGE, hroot, hva, (pa & ~(PAGE - 1)) | PTE_HOST | (device ? attr_dev : attr_mem),
	                   0, 0, 0);
	if (st != ST_SUCCESS && st != ST_MAP_VALID) {
		g_sptm_err = (st << 8) | 3;
		return 0;
	}
	u64 va = hva;
	hva += PAGE;
	return va + (pa & (PAGE - 1));
}

/* [va, va + len) lies in one PAPT range */
int papt_has_va(u64 va, u64 len)
{
	const u64 *e = papt;
	for (u64 i = 0; i < papt_n; i++, e += 3)
		if (va >= e[1] && va + len <= e[1] + e[2] * PAGE)
			return 1;
	return 0;
}

static u64 p2v(u64 pa)
{
	const u64 *e = papt;
	for (u64 i = 0; i < papt_n; i++, e += 3)
		if (pa >= e[0] && pa < e[0] + e[2] * PAGE)
			return e[1] + (pa - e[0]);
	return 0;
}

static u64 tent(u64 table_pa, u64 idx)
{
	u64 va = p2v(table_pa);
	return va ? *(volatile u64 *)(va + idx * 8) : 0;
}

/* Boot framebuffer for the host, the way XNU maps it: SPTM pre-builds empty L3 tables from ALIGN_UP(physmap_end,
 * 32 MiB) for XNU's first mappings, with a 64 MiB term for the boot framebuffer (Gravity #8 hv_sptm_boot.c:548-558);
 * XNU's first 518 SPTM calls are MAP_PAGEs there without MAP_TABLE. So: read the
 * kernel tree through the PAPT and start where XNU starts, dynamic_memory_begin = ROUND_TWIG(SPTMArgs->physmap_end)
 * (xnu arm_init_sptm.c:1705,1925; the L3s right above libsptm's physmap_end are SPTM type 9, a MAP_PAGE there is
 * VIOLATION_INVALID_PT in qemu on the real sptm). Take the first pre-built L3 from there whose first npages are empty,
 * MAP_PAGE each page Normal-NC (Gravity mmu_map_framebuffer), publish for EL2 (aura memory.c, Gravity #9 ALLE2IS). */
static u64 hmap_run2(u64 pa, u64 npages, int device, int ro)
{
	if (!g_boot || npages > 2048)
		return 0;
	u64 root = g_boot[L_ROOT], pme = g_boot[B_PMEND];
	u64 l1 = tent(root, (pme >> 36) & 0x7ff);
	if ((l1 & 3) != 3)
		return 0;
	u64 l2pa = l1 & OA_MASK, i2 = (pme >> 25) & 0x7ff;
	if (pme & (L3SPAN - 1))
		i2++;
	u64 want = device ? 0x00 : 0x44, attr = 1UL << 2;
	for (u64 i = 0, m = sysreg_read(mair_el1); i < 8; i++, m >>= 8)
		if ((m & 0xff) == want) { attr = i << 2; break; }
	for (; i2 < 2048; i2++) {
		u64 d = tent(l2pa, i2);
		if ((d & 3) != 3)
			return 0;                          /* end of the pre-built run */
		u64 l3 = d & OA_MASK, busy = 0;
		for (u64 j = 0; j < npages && !busy; j++)
			busy = tent(l3, j) & 1;
		if (busy)
			continue;
		u64 va = (pme & ~((1UL << 36) - 1)) | (i2 << 25);
		for (u64 j = 0; j < npages; j++) {
			u64 st = sptm_call(SPTM_MAP_PAGE, root, va + j * PAGE, (pa + j * PAGE) | PTE_HOST | attr | (ro ? 1UL << 7 : 0), 1, 0, 0);
			if (st > ST_MAP_VALID) {
				g_sptm_err = (st << 8) | 4;
				return 0;
			}
		}
		__asm__ volatile("dsb ishst; tlbi vmalle1is; tlbi alle2is; dsb ish; isb" ::: "memory");
		return va;
	}
	return 0;
}

static u64 hmap_run(u64 pa, u64 npages, int device)
{
	return hmap_run2(pa, npages, device, 0);
}

u64 hmap_fb(u64 pa, u64 npages)
{
	return hmap_run(pa, npages, 0);
}

/* F34: a read-only device page (AP[2] = 1) for SPTM Table A frames (type 27 XNU_PROTECTED_IO) that XNU itself reads
 * directly, e.g. the T8110 DART error block; a write there would be the violation, a read-only map is what XNU holds */
u64 hmap_dev_ro(u64 pa, u64 npages)
{
	return hmap_run2(pa, npages, 1, 1);
}

/* device pages through the same pre-built tables as the fb: MAP_PAGE decides by the frame's io-range entry */
u64 hmap_dev(u64 pa, u64 npages)
{
	return hmap_run(pa, npages, 1);
}

/* stage 2: root (L1, type 33, attr 3 | vmid 1), L2 per 64 GiB, L3 per 32 MiB (type 34) */
void s2_init(void)
{
	s2_root = frame_alloc();
	sptm_call(SPTM_RETYPE, s2_root, XNU_DEFAULT, XNU_STAGE2_ROOT_TABLE, 0x100000003UL, 0, 0);
}

/* MAP_TABLE / MAP_PAGE: 0 ok, 1 already valid, 8 table present; anything else goes to the log */
static void s2_st(u64 r)
{
	if (r == 0 || r == 1 || r == 8)
		return;
	if (!g_s2_bad++)
		g_s2_first = r;
}


#ifdef S2_TRACE
/* device-map tracer: the violating sptm_call terminal-gexits to XNU (never returns here), so we print
 * BEFORE each call for device maps only; the LAST line before the hang names the op+ipa+pa that violated. */
static void s2t(const char *op, u64 ipa, u64 pa) { con_puts(op); con_puts(" ipa="); con_hex(ipa); con_puts(" pa="); con_hex(pa); con_puts("\n"); }
#define S2T(op, ipa, pa) do { if (device) s2t(op, ipa, pa); } while (0)
#else
#define S2T(op, ipa, pa) ((void)0)
#endif

void s2_map(u64 ipa, u64 pa, int device)
{
	u64 l1i = ipa >> 36;                /* 64 GiB per L1 entry */
	if (l1i >= 40)
		return;
	if (!s2_l2[l1i]) {
		u64 t = frame_alloc();
		S2T("s2 RETYPE-L2", ipa, t);
		sptm_call(SPTM_RETYPE, t, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 2, 0, 0);
		S2T("s2 MAPTAB-L1", ipa & ~((1UL << 36) - 1), t);
		s2_st(sptm_call(SPTM_MAP_TABLE, s2_root, ipa & ~((1UL << 36) - 1), 1, t | 3, 0, 0));
		s2_l2[l1i] = t;
	}
	u64 win = ipa & ~(L3SPAN - 1);
	/* windows are touched in any order since RAM is mapped on fault: one L3 table per window, kept in a small hash */
	static u64 s2_wins[4096];
	u64 h = (win >> 25) & 4095;
	while (s2_wins[h] && s2_wins[h] != win)
		h = (h + 1) & 4095;
	if (!s2_wins[h]) {
		u64 t = frame_alloc();
		S2T("s2 RETYPE-L3", win, t);
		sptm_call(SPTM_RETYPE, t, XNU_DEFAULT, XNU_STAGE2_PAGE_TABLE, 3, 0, 0);
		S2T("s2 MAPTAB-L2", win, t);
		s2_st(sptm_call(SPTM_MAP_TABLE, s2_root, win, 2, t | 3, 0, 0));
		s2_wins[h] = win;
	}
	/* valid|page, AF, S2AP RW + MemAttr: 0=Normal-WB (SH inner), 1=Device-nGnRnE, 2=Normal-NC (OSH).
	 * The fb carveout MUST be device=2 (Normal-NC): the scanout DMA reads DRAM, so a cacheable stage-2
	 * map would let the guest's writes sit in cache and the DCP would keep scanning the stale (logo)
	 * frame. NC forces the writes straight to DRAM regardless of the guest's stage-1 attr (aura
	 * mmu_map_framebuffer = MAIR_NORMAL_NC). SPTM dispatches the guest with HCR 0x400480000001: FWB is 1, and 0, 5, 0xf
	 * mean the same types in that encoding. Device-nGnRE is 1 << 2 in both. */
	u64 attr = device == 1 ? 0UL                            /* Device-nGnRnE */
	         : device == 3 ? (1UL << 2)                     /* Device-nGnRE: PCI BAR, pmap-io-ranges eICP 0x80000027 VM_WIMG_POSTED, no ioremap_np for PCI */
	         : device == 2 ? ((2UL << 8) | (0x5UL << 2))    /* Normal Non-cacheable, outer-shareable */
	                       : ((3UL << 8) | (0xfUL << 2));   /* Normal Write-Back, inner-shareable */
	u64 pte = pa | 0x3 | (1UL << 10) | (3UL << 6) | attr;
	S2T("s2 MAPPAGE", ipa, pte);
	g_sptm_calls++;
	s2_st(sptm_call(SPTM_MAP_PAGE, s2_root, ipa, pte, 0, 0, 0));
	S2T("s2 OK", ipa, pa);
}

u64 first_frame_hint(void) { return next_frame; }
