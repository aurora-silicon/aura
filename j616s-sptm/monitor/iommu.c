// SPDX-License-Identifier: GPL-2.0-only
// DART facts for the guest: unit ids, SIDs, apertures and the retained DCP mappings, all from the live ADT, plus the guard
// in front of table 3. A terminal SPTM violation stops the Mac, so a call the real SPTM would refuse is failed here and named
// on the screen instead (behaviour of the SPTM's t8110dart ops, 26.6.2).
//
// The bootstrap state is not read from the DARTs or their tables. DART registers are XNU_PROTECTED_IO (type 0x1b, table A tag
// 9) and SPTM registers each pt-region-N as a type 0x1b IO range at bootstrap; type 0x1b allows only a
// kernel read-only, PXN+UXN host leaf (type_desc+4 = 0x0800, not run), so a writable host
// or any stage-2 MAP_PAGE of either is terminal. The ADT describes the same mappings:
// iop-dcp-nub segment-ranges. Which segment each stream maps is what the J616s raw handoff found by walking those tables on
// this machine (running DT, dcp-map iommu-addresses, kboot_display_handoff.c dcp6_check_sid).
#include "mon.h"

extern u64 g_ram_size;                    /* linux.c, measured at boot */
#define RAM_SIZE_G g_ram_size
#define NOIPA      0xffffffffffffffffUL

struct dart_unit {
	const char *path;
	u32 id, sids[8];                  /* sid bitmap for ids below 256 */
	u64 base, size;
	int ok;
	u64 err_va[2];                    /* F34: the +0x8000 error block of each instance, mapped read-only, 0 until read */
};
struct seg { u64 phys, iova, remap; u32 size, flags; };
struct run { u64 dva, size, pa, ipa; };
struct rsid { struct dart_unit *u; u32 sid, n; u64 pt_lo; struct run r[8]; };

/* the image runs slid with no relocation pass and no .data: pointers are set at run time */
static struct dart_unit u_mtp, u_disp, u_dcp, u_pci, u_usb, u_aop, u_usb0, u_usb1;
static struct dart_unit *units[8];
static struct rsid rs[3];
static u32 nrs;
static struct seg segs[8];
static u32 g_kb_country;
static u8 inited[256];
int g_mtp_ok, g_dcp_ok, g_pci_ok, g_usb_ok, g_aop_ok, g_usb0_ok, g_usb1_ok;

static int sid_ok(const struct dart_unit *u, u64 sid)
{
	return sid < 256 && (u->sids[sid >> 5] >> (sid & 31) & 1);
}

static void kv(const char *k, u64 v)
{
	con_puts(k);
	con_hex(v);
	con_puts("\n");
}

static int unit_read(struct dart_unit *u)
{
	const void *n = adt_path(u->path);
	u32 l = 0;
	const u32 *s = n ? adt_prop(n, "sid", &l) : 0;
	u32 id = n ? adt_u32(n, "dart-id", 0xffffffff) : 0xffffffff;
	con_puts("mon: ");
	con_puts(u->path);
	if (!n || !s || id > 255) {
		con_puts(n ? ": no dart-id or sid\n" : ": not in the ADT\n");
		return -1;
	}
	u->id = id;
	u->base = adt_u64(n, "vm-base", 0);
	u->size = adt_u64(n, "vm-size", 0);
	for (u32 i = 0; i + 4 <= l; i += 4) {
		u32 v = *(const volatile u32 *)((const u8 *)s + i);
		if (v < 256)
			u->sids[v >> 5] |= 1U << (v & 31);
	}
	u->ok = 1;
	con_puts(" id ");
	con_hex(id);
	con_puts(" vm ");
	con_hex(u->base);
	con_puts(" +");
	con_hex(u->size);
	con_puts("\n");
	return 0;
}

static void run_add(struct rsid *r, const struct seg *g)
{
	u64 sz = ((u64)g->size + PAGE - 1) & ~(PAGE - 1);
	u32 i = r->n++;
	while (i && r->r[i - 1].dva > g->remap) {
		r->r[i] = r->r[i - 1];
		i--;
	}
	/* TEXT is the one mapping the guest gets no CPU alias of: an IPA nothing backs would stop the driver at a fault */
	r->r[i] = (struct run){ g->remap, sz, g->phys, g == &segs[0] ? NOIPA : g->phys };
}

static struct rsid *rsid_new(struct dart_unit *u, u32 sid, const void *node)
{
	/* "pt-region-23": SPTM's own proof that the stream has bootstrap tables */
	char name[16] = "pt-region-";
	u32 l = 0, i = 10;
	if (sid >= 10)
		name[i++] = '0' + sid / 10;
	name[i++] = '0' + sid % 10;
	name[i] = 0;
	const u64 *pt = adt_prop(node, name, &l);
	if (!pt || l != 16)
		return 0;
	struct rsid *r = &rs[nrs++];
	r->u = u;
	r->sid = sid;
	r->n = 0;
	r->pt_lo = *(const volatile u64 *)pt;
	return r;
}

/* Segment order and flags as the J616s firmware profile has them; any other shape publishes nothing, simplefb stays. */
static int dcp_read(void)
{
	static const u32 rows[8] = { 1, 0, 0xa, 2, 2, 2, 2, 2 };
	u32 l = 0;
	const void *dn = adt_path("arm-io/dart-dcp"), *pn = adt_path("arm-io/dart-disp0");
	const u8 *v = adt_prop(adt_path("arm-io/dcp/iop-dcp-nub"), "segment-ranges", &l);
	if (!u_dcp.ok || !u_disp.ok || !v || l != sizeof(segs)) {
		con_puts("mon: DCP segments absent, l=");
		con_hex(l);
		con_puts("\n");
		return -1;
	}
	for (u32 i = 0; i < 8; i++) {
		const u8 *e = v + i * 32;
		segs[i].phys = *(const volatile u64 *)e;
		segs[i].iova = *(const volatile u64 *)(e + 8);
		segs[i].remap = *(const volatile u64 *)(e + 16);
		segs[i].size = *(const volatile u32 *)(e + 24);
		segs[i].flags = *(const volatile u32 *)(e + 28);
		con_puts("mon: dcp seg ");
		con_hex(segs[i].phys);
		con_puts(" dva ");
		con_hex(segs[i].remap);
		con_puts(" size ");
		con_hex(segs[i].size);
		con_puts(" flags ");
		con_hex(segs[i].flags);
		con_puts("\n");
		if (segs[i].flags != rows[i] || (i != 2 && segs[i].remap >> 36 != 0x10))
			return -1;
	}
	if ((segs[6].phys & ~(PAGE - 1)) != (g_vbase & ~(PAGE - 1)) || u_dcp.base != 1UL << 40 || u_dcp.size != 1UL << 36 ||
	    u_disp.base != 1UL << 40 || u_disp.size != 1UL << 36 || !sid_ok(&u_dcp, 23) || !sid_ok(&u_disp, 0) ||
	    !sid_ok(&u_disp, 16)) {
		con_puts("mon: DCP handoff shape differs, stays on simplefb\n");
		return -1;
	}
	struct rsid *d = rsid_new(&u_dcp, 23, dn), *s0 = rsid_new(&u_disp, 0, pn), *s16 = rsid_new(&u_disp, 16, pn);
	if (!d || !s0 || !s16) {
		con_puts("mon: DCP pt-region absent, stays on simplefb\n");
		return -1;
	}
	for (u32 i = 0; i < 8; i++)
		if (i != 2)
			run_add(d, &segs[i]);
	run_add(s0, &segs[4]);
	run_add(s0, &segs[6]);
	run_add(s0, &segs[7]);
	run_add(s16, &segs[5]);
	return 0;
}

void iommu_setup(void)
{
	u_mtp.path = "arm-io/dart-mtp";
	u_disp.path = "arm-io/dart-disp0";
	u_dcp.path = "arm-io/dart-dcp";
	units[0] = &u_mtp;
	units[1] = &u_disp;
	units[2] = &u_dcp;
	u_pci.path = "arm-io/dart-apcie0";
	units[3] = &u_pci;
	u_usb.path = "arm-io/dart-usb2";
	units[4] = &u_usb;
	/* F35: the AOP's DART (sensors, microphones, the admac rings of the speakers). No pt-region: the guest INITs it fresh; the AOP
	 * RTKit itself is pre-loaded and adopted. Fresh facts from the ADT (11-02): id 0xa, sids 0 8 9 10 11, vm 0x10000014000 +0x2fffffec000. */
	u_aop.path = "arm-io/dart-aop";
	units[5] = &u_aop;
	/* F35: the two left USB-C ports, byte for byte the dart-usb2 shape (ids 0xe, 0xf; sids 1, 14; the same window) */
	u_usb0.path = "arm-io/dart-usb0";
	units[6] = &u_usb0;
	u_usb1.path = "arm-io/dart-usb1";
	units[7] = &u_usb1;
	for (u32 i = 0; i < sizeof(units) / sizeof(units[0]); i++)
		unit_read(units[i]);
	g_mtp_ok = u_mtp.ok && sid_ok(&u_mtp, 0) && u_mtp.base == 0x10000004000UL && u_mtp.size == 0xffff0000UL;
	g_aop_ok = u_aop.ok && sid_ok(&u_aop, 0) && sid_ok(&u_aop, 8) && sid_ok(&u_aop, 10) && u_aop.base == 0x10000014000UL &&
		   u_aop.size == 0x2fffffec000UL;
	g_usb0_ok = u_usb0.ok && sid_ok(&u_usb0, 1) && u_usb0.base == 0x10000004000UL && u_usb0.size == 0xffff0000UL;
	g_usb1_ok = u_usb1.ok && sid_ok(&u_usb1, 1) && u_usb1.base == 0x10000004000UL && u_usb1.size == 0xffff0000UL;
	u_pci.sids[0] |= 6;                   /* Wi-Fi and BT: streams 1, 2 (raw DT iommu-map); the ADT gives them out dynamically, its sid list is PIO-DMA and bypass */
	g_pci_ok = u_pci.ok && sid_ok(&u_pci, 17) && u_pci.base == 0x10000100000UL && u_pci.size == 0x3fe00000UL;
	g_usb_ok = u_usb.ok && sid_ok(&u_usb, 1) && u_usb.base == 0x10000004000UL && u_usb.size == 0xffff0000UL;
	/* F34: map the error blocks now, so the panel says before any DMA whether SPTM lets the monitor read them (Table A, type 27) */
	{
		u64 st, ad, sd, in;
		struct { const char *n; struct dart_unit *u; u64 inst; } q[3] = { { "usb0", &u_usb, 0 }, { "usb1", &u_usb, 1 }, { "pci0", &u_pci, 0 } };
		for (int i = 0; i < 3; i++) {
			st = ad = sd = in = 0;
			con_puts("mon: darterr "); con_puts(q[i].n); con_puts(" mask ");
			con_hex(q[i].u->ok ? iommu_dart_err(q[i].u->id, q[i].inst, &st, &ad, &sd, &in) : ~0UL);
			con_puts(" ver "); con_hex(in & 0xffff); con_puts(" nsids "); con_hex(in >> 16); con_puts("\n");
		}
		con_puts("mon: darterr: mask ffff.. = unit, reg or RO map missing; 0 = no SID faults; else the exception bitmap\n");
	}
#ifdef P_DARTERR_QTEST
	/* qemu only: the qemu ADT has no dart-id, so the map above is never tried there; try the fixed J616s page (dart-usb2 inst 0 +0x8000),
	 * map only, no read (qemu has no DART model): the line says whether SPTM accepts a read-only map of a Table A page */
	{
		extern u64 g_sptm_err;
		con_puts("mon: darterr qtest map 0x392f00000 +0x4000 +0x8000 va ");
		con_hex(hmap_dev_ro(0x392f00000UL, 1)); con_puts(" "); con_hex(hmap_dev_ro(0x392f04000UL, 1)); con_puts(" "); con_hex(hmap_dev_ro(0x392f08000UL, 1));
		con_puts(" sptm_err ");
		con_hex(g_sptm_err);
		con_puts("\n");
	}
#endif
#ifdef P_DCP
	g_dcp_ok = !dcp_read();
#else
	(void)dcp_read;
#endif
	const u8 *kb;
	u32 l = 0;
	kb = adt_prop(adt_path("arm-io/dockchannel-mtp/mtp-transport/keyboard"), "kblang-calibration", &l);
	/* m4-loader keyboard_types.h, 39 entries: Japanese 15; U.S., Chinese, Hangul, Zhuyin, Thai 33; the rest ISO 13. Unknown index: 0, not ISO, as the loader */
	if (kb && l >= 2 && kb[1] < 39)
		g_kb_country = kb[1] == 3 ? 15 : (kb[1] == 5 || kb[1] == 11 || kb[1] == 15 || kb[1] == 18 || kb[1] == 33) ? 33 : 13;
	kv("mon: IOMMU mtp ok ", g_mtp_ok);
	kv("mon: IOMMU dcp ok ", g_dcp_ok);
	kv("mon: IOMMU pcie ok ", g_pci_ok);
	kv("mon: IOMMU usb ok ", g_usb_ok);
	kv("mon: IOMMU aop ok ", g_aop_ok);
	kv("mon: IOMMU usb0 ok ", g_usb0_ok);
	kv("mon: IOMMU usb1 ok ", g_usb1_ok);
}

static void put_cell(u64 va, u64 len, u32 ph, u32 val)
{
	for (u64 i = 0; i + 4 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(ph))
			w32(va + i, __builtin_bswap32(val));
}

static void put_u64(u64 va, u64 len, u32 ph_hi, u32 ph_lo, u64 val)
{
	for (u64 i = 0; i + 8 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(ph_hi) &&
		    *(volatile u32 *)(va + i + 4) == __builtin_bswap32(ph_lo)) {
			w32(va + i, __builtin_bswap32((u32)(val >> 32)));
			w32(va + i + 4, __builtin_bswap32((u32)val));
		}
}

/* status = "mhold-xxx" is simply not "okay" for the kernel; the first bytes become "okay\0" when the facts passed */
static void enable(u64 va, u64 len, const char *tag)
{
	u64 tl = 0;
	while (tag[tl])
		tl++;
	for (u64 i = 0; i + tl < len; i += 4) {
		const volatile char *p = (const volatile char *)(va + i);
		u64 j = 0;
		while (j < tl && p[j] == tag[j])
			j++;
		if (j == tl) {
			volatile char *w = (volatile char *)(va + i);
			w[0] = 'o'; w[1] = 'k'; w[2] = 'a'; w[3] = 'y'; w[4] = 0;
		}
	}
}

void iommu_patch_dtb(u64 va, u64 len)
{
	/* an id that stays a placeholder is above 255: apple-dart-apif rejects it, a disabled node never reads it */
	if (g_mtp_ok) {
		put_cell(va, len, 0xba5e0601, u_mtp.id);
		put_cell(va, len, 0xba5e0604, g_kb_country);
		enable(va, len, "mhold-mtp");
	}
	if (g_pci_ok) {
		put_cell(va, len, 0xba5e0403, u_pci.id);
		enable(va, len, "mhold-pci");
	}
	if (g_usb_ok) {
		put_cell(va, len, 0xba5e0801, u_usb.id);
		enable(va, len, "mhold-usb");
	}
	if (g_aop_ok) {
		put_cell(va, len, 0xba5e0a01, u_aop.id);
		enable(va, len, "mhold-aop");
	}
	/* tags "mhold-ua0"/"mhold-ua1", not "mhold-usb0": enable() matches by prefix and "mhold-usb" is the right port's tag */
	if (g_usb0_ok) {
		put_cell(va, len, 0xba5e0811, u_usb0.id);
		enable(va, len, "mhold-ua0");
	}
	if (g_usb1_ok) {
		put_cell(va, len, 0xba5e0821, u_usb1.id);
		enable(va, len, "mhold-ua1");
	}
	if (g_dcp_ok) {
		/* memory-region order of the running J616s DT: heap, display, piodma, framebuffer, display, text, data */
		static const u8 order[7] = { 3, 4, 5, 6, 7, 0, 1 };
		put_cell(va, len, 0xba5e0602, u_disp.id);
		put_cell(va, len, 0xba5e0603, u_dcp.id);
		for (u32 i = 0; i < 7; i++) {
			const struct seg *g = &segs[order[i]];
			u32 ph = 0xba5e0700 + i * 4;
			put_u64(va, len, ph, ph + 1, g->phys);
			put_u64(va, len, ph + 2, ph + 3, ((u64)g->size + PAGE - 1) & ~(PAGE - 1));
		}
		enable(va, len, "mhold-dcp");
	}
}

/* B:t1a in the io-range tables, the class of the AIC and SMC pages that map on HW (F1-F4). The DART pages and DCP disp-0 are
 * A:t1b, and stay off stage 2: disp-0 is only handed back to the firmware, Linux never dereferences it. */
void iommu_map_devices(void)
{
	if (g_mtp_ok) {
		static const u64 pg[] = { 0x514600000UL, 0x514608000UL, 0x514b14000UL, 0x514b28000UL, 0x514b2c000UL,
					  0x514b30000UL, 0x514b34000UL };
		for (u32 i = 0; i < sizeof(pg) / sizeof(pg[0]); i++)
			s2_map(pg[i], pg[i], 1);
		con_puts("mon: L6 MTP mapped\n");
	}
	if (g_dcp_ok) {
		s2_map(0x402e00000UL, 0x402e00000UL, 1);
		s2_map(0x402e08000UL, 0x402e08000UL, 1);
		for (u32 i = 1; i < 8; i++) {
			if (i == 2)
				continue;
			u64 sz = ((u64)segs[i].size + PAGE - 1) & ~(PAGE - 1);
			for (u64 o = 0; o < sz; o += PAGE)
				s2_map(segs[i].phys + o, segs[i].phys + o, 2);
		}
		con_puts("mon: L7 DCP mapped\n");
	}
}

/* F34: the T8110 error state, read the way XNU reads it: page 0 PARAMS3 (+0x8) & 0xffff is the
 * hw version, PARAMS4 (+0xc) & 0x1ff the SID count (ADT sid-count is only a hint). hw >= 0x202: the exception bitmap at +0x4000 (one
 * u32 per 32 SIDs, XNU's pending bitmap) says which SIDs fault; the per-SID summary at +0x8000 + sid*0x40 (status, +8 address
 * lo, +0xc hi, as XNU collects it) has no valid bit of its own: status 0 with the bitmap bit set is "TTBR invalid".
 * hw < 0x202: ERROR +0x100 (bit 31 valid, 27:20 SID, 28 write), ADDR +0x170/+0x174. All three pages are SPTM Table A (type 27): mapped
 * read-only for the monitor only (qemu-sptm accepted such a map), never into the guest. Returns the fault mask (bitmap, or 1 << SID for
 * the old layout), the lowest faulting SID's status/address/SID, and version | nsids << 16 in *info; ~0UL when nothing could be mapped. */
static u64 le64(const u8 *p)
{
	u64 v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}



u64 iommu_dart_err(u64 id, u64 inst, u64 *status, u64 *addr, u64 *sid, u64 *info)
{
	struct dart_unit *u = 0;
	for (u32 i = 0; i < sizeof(units) / sizeof(units[0]); i++)
		if (units[i]->ok && units[i]->id == id)
			u = units[i];
	*status = 0; *addr = 0; *sid = 0; *info = 0;
	if (!u || inst > 1)
		return ~0UL;
	if (!u->err_va[inst]) {
		const void *n = adt_path(u->path);
		u32 l = 0;
		const u8 *r = n ? adt_prop(n, "reg", &l) : 0;
		if (!r || l < (inst + 1) * 16)
			return ~0UL;
		u64 pa = 0x200000000UL + le64(r + inst * 16), size = le64(r + inst * 16 + 8);
		if (size < 0xc000)
			return ~0UL;
		u64 v0 = hmap_dev_ro(pa, 1), v1 = hmap_dev_ro(pa + 0x4000, 1), v2 = hmap_dev_ro(pa + 0x8000, 1);
		if (!v0 || !v1 || !v2 || v1 != v0 + PAGE || v2 != v0 + 2 * PAGE)
			return ~0UL;                   /* the three pages must land in one run for the offsets below to hold */
		u->err_va[inst] = v0;
	}
	u64 b = u->err_va[inst];
	u32 ver = r32(b + 8) & 0xffff, nsids = r32(b + 0xc) & 0x1ff;
	if (nsids > 64)
		nsids = 64;
	*info = ver | (u64)nsids << 16;
	u64 mask = 0;
	if (ver >= 0x202) {
		for (u32 w = 0; w < (nsids + 31) / 32; w++)
			mask |= (u64)r32(b + 0x4000 + w * 4) << (32 * w);
		if (nsids < 64)
			mask &= (1UL << nsids) - 1;
		for (u64 s = 0; s < nsids; s++)
			if (mask & (1UL << s)) {
				*status = r32(b + 0x8000 + s * 0x40);
				*addr = r32(b + 0x8000 + s * 0x40 + 8) | (u64)r32(b + 0x8000 + s * 0x40 + 0xc) << 32;
				*sid = s;
				break;
			}
	} else {
		u32 e = r32(b + 0x100);
		if (e & (1U << 31)) {
			*sid = (e >> 20) & 0xff;
			mask = 1UL << *sid;
			*status = e;
			*addr = r32(b + 0x170) | (u64)r32(b + 0x174) << 32;
		}
	}
	return mask;
}

static struct dart_unit *unit_of(u64 id)
{
	for (u32 i = 0; i < sizeof(units) / sizeof(units[0]); i++)
		if (units[i]->ok && units[i]->id == id)
			return units[i];
	return 0;
}

static struct rsid *rsid_of(u64 id, u64 sid)
{
	for (u32 i = 0; i < nrs; i++)
		if (rs[i].u->id == id && rs[i].sid == sid)
			return &rs[i];
	return 0;
}

/* BOOT_IOMMU: 0 and the fields, or -1 when this stream has no retained state */
int iommu_boot_info(u64 id, u64 sid, u64 idx, u64 *out)
{
	struct rsid *r = rsid_of(id, sid);
	if (!r)
		return -1;
	for (u32 i = 0; i < 8; i++)
		out[i] = 0;
	if (idx == NOIPA) {
		out[0] = 1;               /* SPTM holds the level 1 table, TTBR = pt-region base */
		out[1] = r->n;
		out[2] = r->u->base;
		out[3] = r->u->size;
		return 0;
	}
	if (idx >= r->n)
		return -1;
	out[2] = r->r[idx].dva;
	out[3] = r->r[idx].size;
	out[4] = r->r[idx].pa;
	out[5] = r->r[idx].ipa;
	return 0;
}

/* BOOT_TABLE: nonzero means "a bootstrap table exists, do not MAP_TABLE here": a second one is the terminal 0x6000011. Linux
 * only tests it for zero. Level 2 is answered from the runs: a block with no mapping is taken as having no leaf table, which
 * is an inference until the tables can be read (open). */
u64 iommu_boot_table(u64 id, u64 sid, u64 dva, u64 level)
{
	if (level == NOIPA)
		return id < 256 ? inited[id] : 0;
	struct rsid *r = rsid_of(id, sid);
	if (!r)
		return 0;
	if (level <= 1)
		return r->pt_lo;
	u64 lo = dva & ~((1UL << 25) - 1), hi = lo + (1UL << 25);
	for (u32 i = 0; i < r->n; i++)
		if (r->r[i].dva < hi && lo < r->r[i].dva + r->r[i].size)
			return r->pt_lo;
	return 0;
}

static int refuse(const char *why, u64 sel, u64 a0, u64 a1)
{
	con_puts("mon: APIF refused ");
	con_puts(why);
	con_puts(" sel ");
	con_hex(sel);
	con_puts(" ");
	con_hex(a0);
	con_puts(" ");
	con_hex(a1);
	con_puts("\n");
	return 0;
}

/* 1 = forward. Table 3 is IOMMU_ID_DART_T8110's XNU-facing table on this SoC: descriptor +0x30 = 3. */
int iommu_guard(u64 sel, const u64 *a)
{
	u64 table = sel >> 32, ep = sel & 0xffffffff;
	if (table != 3)
		return 1;
	struct dart_unit *u = unit_of(a[0]);
	if (!u)
		return refuse("unknown dart", sel, a[0], a[1]);
	switch (ep) {
	case 0:                           /* MAP_TABLE(dart, sid, dva, level, pa) */
		if (!sid_ok(u, a[1]) || a[3] < 1 || a[3] > 2 || a[2] < u->base || a[2] - u->base >= u->size)
			return refuse("map_table", sel, a[1], a[2]);
		if (!inited[u->id] || (a[3] == 1 && rsid_of(u->id, a[1])) ||
		    (a[3] == 2 && iommu_boot_table(u->id, a[1], a[2], 2)))
			return refuse("map_table state", sel, a[1], a[2]);
		return 1;
	case 2:                           /* MAP(dart, sid, dva, list pa, nbytes, deny) */
		if (!sid_ok(u, a[1]) || !inited[u->id] || a[4] > (1UL << 25) || a[5] > 7 || a[2] < u->base ||
		    a[2] + a[4] >= u->base + u->size || ((a[2] & ((1UL << 25) - 1)) + a[4] > (1UL << 25)) ||
		    a[3] < g_gpa || a[3] >= g_gpa + RAM_SIZE_G)
			return refuse("map", sel, a[1], a[2]);
		return 1;
	case 3:                           /* UNMAP(dart, sid, dva, nbytes, flags) */
		if (!sid_ok(u, a[1]) || !inited[u->id] || a[3] > (1UL << 25) || a[2] < u->base ||
		    a[2] + a[3] >= u->base + u->size)
			return refuse("unmap", sel, a[1], a[2]);
		return 1;
	case 5:                           /* POWERUP(dart) */
		return inited[u->id] ? 1 : refuse("powerup before init", sel, a[0], 0);
	case 6:                           /* INIT(dart): once, state byte 0xbe0 must be 0 */
		return inited[u->id] ? 0 : 1;
	case 7:
	case 8:                           /* DISABLE/ENABLE(dart, sid): retained streams keep running */
		if (!sid_ok(u, a[1]) || rsid_of(u->id, a[1]))
			return refuse("sid state", sel, a[1], 0);
		return 1;
	default:
		return refuse("endpoint", sel, a[0], a[1]);
	}
}

void iommu_forwarded(u64 sel, const u64 *a)
{
	if (sel == 0x300000006UL && a[0] < 256) {
		inited[a[0]] = 1;
		kv("mon: DART INIT done, id ", a[0]);
	}
	if (sel == 0x300000005UL)
		kv("mon: DART POWERUP done, id ", a[0]);
}

int iommu_guest_ram(u64 ipa, u64 size)
{
	return ipa >= g_gpa && size <= RAM_SIZE_G && ipa + size <= g_gpa + RAM_SIZE_G;
}
