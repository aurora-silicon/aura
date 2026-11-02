// SPDX-License-Identifier: GPL-2.0-only
// apif.c — Aurora Platform Interface (APIF) v2 provider.
//
// Our macho-monitor plays "Aura": the aurora guest kernel's APIF clients (NVMe table-6, DART, SART,
// ...) drive the devices themselves; the provider only (a) forwards every NON-service native selector
// UNCHANGED to the real resident SPTM via genter, and (b) implements the table-0xffffffff ownership
// services locally. Spec: aurora Documentation/driver-api/firmware/aurora-apif.rst.
//
// The register page (guest IPA 0x61ff0000, 16 KiB) is intentionally UNMAPPED in the guest stage-2, so
// every guest access traps to mon_guest_exit (ec 0x24) and is emulated here. A 64-bit write to +0x100
// hands us the guest IPA of a batch buffer and triggers processing.
//
// CROSS-GEN CAVEAT: APIF is T6050; we are T6040. Native selectors the client builds
// must be valid for the real T6040 SPTM (25G83). Forwarding is correct regardless; validity is the
// gate verified before any flash.
#include "mon.h"

#define APIF_BASE   0x61ff0000UL
#define APIF_SIZE   0x4000UL
#define APIF_MAGIC  0x4f494d4d46495041UL      /* "APIFMMIO" */
#define APIF_VER    2UL
#define APIF_MAXOPS 64UL
#define APIF_FLAGS  0x7UL                     /* bit0 native | bit1 memory | bit2 I/O */

#define SVC_TABLE   0xffffffffUL
enum { SVC_TRANSLATE = 1, SVC_ALLOC_FRAME = 2, SVC_FREE_FRAME = 3, SVC_MAP_IO = 4,
       SVC_GUEST_ROOT = 5, SVC_BOOT_IOMMU = 6, SVC_BOOT_TABLE = 7, SVC_FIND_FRAME = 8,
       SVC_TAG_FRAME = 9 };

#define APIF_U64_MAX 0xffffffffffffffffUL
#define APIF_EIO 5                            /* batch status on a failed service or refused call */

struct apif_op { u64 selector; u64 arg[6]; u64 ret; };   /* 64 bytes, matches aurora_apif_op */

static u64 g_batch_ipa;

/* --- guest identity RAM access (IPA==PA): one-entry page cache over hmap, bounded window use --- */
static u64 gm_pg, gm_va;
static u64 gmap(u64 gpa)
{
	u64 pg = gpa & ~(PAGE - 1);
	if (!gm_va || gm_pg != pg) {
		u64 va = hmap(pg, 0);
		if (!va)
			return 0;
		gm_pg = pg;
		gm_va = va;
	}
	return gm_va + (gpa & (PAGE - 1));
}
static u64 gr64(u64 gpa) { u64 v = gmap(gpa); return v ? *(volatile u64 *)v : 0; }
static u32 gr32(u64 gpa) { u64 v = gmap(gpa); return v ? *(volatile u32 *)v : 0; }
static void gw64(u64 gpa, u64 x) { u64 v = gmap(gpa); if (v) *(volatile u64 *)v = x; }
static void gw32(u64 gpa, u32 x) { u64 v = gmap(gpa); if (v) *(volatile u32 *)v = x; }

/* --- ownership frame pool: FIND/TAG keys, ALLOC/FREE bookkeeping, capped at 1024 per spec. Linux retypes an ALLOC_FRAME
 * frame itself (11 -> 24, forwarded), which is what makes SPTM refuse its PAPT alias. So: never touch a frame through the
 * PAPT while it is type 24, and a FREE of one retypes it back first (24 -> 11 is the only way out, sptm "retyping from
 * XNU_IOMMU but not to XNU_DEFAULT"). --- */
#define APIF_POOL 1024
static struct apif_frame { u64 pa, owner, addr; u8 live, iommu; } g_pool[APIF_POOL];
static u32 g_pool_n;
#define SPTM_XNU_IOMMU 24UL

static struct apif_frame *pool_of(u64 pa)
{
	for (u32 i = 0; i < g_pool_n; i++)
		if (g_pool[i].pa == pa)
			return &g_pool[i];
	return 0;
}

static u64 pool_find(u64 owner, u64 addr)
{
	for (u32 i = 0; i < g_pool_n; i++)
		if (g_pool[i].live && g_pool[i].owner == owner && g_pool[i].addr == addr && (owner || addr))
			return g_pool[i].pa;
	return 0;
}

static u64 frame_zeroed(void)
{
	/* a freed frame first, then fresh frames from the pool below the guest window */
	u64 pa = 0;
	struct apif_frame *f = 0;
	for (u32 i = 0; i < g_pool_n && !f; i++)
		if (!g_pool[i].live)
			f = &g_pool[i];
	if (f)
		pa = f->pa;
	else if (g_pool_n < APIF_POOL) {
		pa = frame_alloc_checked();
		if (!pa)
			return 0;
		f = &g_pool[g_pool_n++];
		f->pa = pa;
	} else {
		return 0;
	}
	u64 va = hmap(pa, 0);
	if (!va)
		return 0;
	for (u64 i = 0; i < PAGE; i += 8)
		w64(va + i, 0);
	for (u64 p = va; p < va + PAGE; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p));
	dsb();
	f->live = 1;
	f->owner = f->addr = 0;
	f->iommu = 0;
	return pa;
}

static void write_boot_info(u64 ipa, const u64 *out)
{
	for (int i = 0; i < 8; i++)
		gw64(ipa + i * 8, out[i]);
}

/* Returns 1 if the service completed, 0 if it failed (which stops the batch per spec). */
static int apif_service(struct apif_op *op)
{
	u32 ep = (u32)op->selector;
	switch (ep) {
	case SVC_TRANSLATE:
		if (!iommu_guest_ram(op->arg[0], op->arg[1])) {
			op->ret = 0;
			return 0;
		}
		op->ret = op->arg[0];     /* identity window */
		return 1;
	case SVC_GUEST_ROOT:
		op->ret = s2_root;
		return 1;
	case SVC_ALLOC_FRAME:
		op->ret = frame_zeroed();
		if (!op->ret)
			return 0;
		return 1;
	case SVC_FREE_FRAME: {
		struct apif_frame *f = pool_of(op->arg[0]);
		op->ret = 0;
		if (!f || !f->live)
			return 0;
		if (f->iommu) {           /* Linux frees only unattached frames, SPTM panics on a table still in use */
			sptm_call_raw(SPTM_RETYPE, f->pa, SPTM_XNU_IOMMU, XNU_DEFAULT, 0, 0, 0);
			f->iommu = 0;
		}
		f->live = 0;
		return 1;
	}
	case SVC_MAP_IO: {
		u64 ipa = op->arg[0], pa = op->arg[1], size = op->arg[2];
		if (op->arg[3] || (ipa | pa | size) & (PAGE - 1)) {
			op->ret = APIF_U64_MAX;   /* no read-only device mapping here, and no silent writable one */
			return 0;
		}
		for (u64 a = 0; a < size; a += PAGE)
			s2_map(ipa + a, pa + a, 1);
		op->ret = 0;
		return 1;
	}
	case SVC_FIND_FRAME:
		op->ret = pool_find(op->arg[0], op->arg[1]);
		return 1;
	case SVC_TAG_FRAME: {
		struct apif_frame *f = pool_of(op->arg[0]);
		op->ret = 0;
		if (!f || !f->live || pool_find(op->arg[1], op->arg[2]))
			return 0;
		f->owner = op->arg[1];
		f->addr = op->arg[2];
		return 1;
	}
	case SVC_BOOT_IOMMU: {
		u64 out[8];
		op->ret = APIF_U64_MAX;
		if (op->arg[0] != 0 || op->arg[5] < sizeof(out) || !iommu_guest_ram(op->arg[4], sizeof(out)) ||
		    iommu_boot_info(op->arg[1], op->arg[2], op->arg[3], out))
			return 0;
		write_boot_info(op->arg[4], out);
		op->ret = 0;
		return 1;
	}
	case SVC_BOOT_TABLE:
		if (op->arg[0] != 0) {
			op->ret = APIF_U64_MAX;
			return 0;
		}
		op->ret = iommu_boot_table(op->arg[1], op->arg[2], op->arg[3], op->arg[4]);
		return 1;
	default:
		op->ret = APIF_U64_MAX;
		return 0;
	}
}

static void apif_run_batch(u64 batch)
{
	if (batch & 0xf)                       /* must be 16-byte aligned */
		return;
	u32 count = gr32(batch + 0);
#ifdef APIF_TRACE
	con_puts("apif batch ipa=");
	con_hex(batch);
	con_puts(" count=");
	con_hex(count);
	con_puts("\n");
#endif
	if (count > APIF_MAXOPS)
		return;
	u32 done = 0;
	for (; done < count; done++) {
		u64 opa = batch + 16 + (u64)done * sizeof(struct apif_op);
		struct apif_op op;
		op.selector = gr64(opa + 0);
		for (int i = 0; i < 6; i++)
			op.arg[i] = gr64(opa + 8 + i * 8);
		u32 table = (u32)(op.selector >> 32);
#ifdef APIF_TRACE
		con_puts(table == (u32)SVC_TABLE ? "apif svc ep=" : "apif op table=");
		con_hex(table == (u32)SVC_TABLE ? (u64)(u32)op.selector : (u64)table);
		con_puts("\n");
#endif
		if (table == (u32)SVC_TABLE) {
			if (!apif_service(&op)) {
				gw64(opa + 56, op.ret);
				gw32(batch + 8, -APIF_EIO);
				con_puts("mon: APIF service failed ep ");
				con_hex((u64)(u32)op.selector);
				con_puts("\n");
				break;
			}
		} else if (!iommu_guard(op.selector, op.arg) && op.selector != 0x300000006UL) {
			/* a call the real SPTM would answer with a terminal violation: this batch ends, the Mac does not */
			gw64(opa + 56, APIF_U64_MAX);
			gw32(batch + 8, -APIF_EIO);
			break;
		} else if (op.selector == 0x300000006UL && !iommu_guard(op.selector, op.arg)) {
			op.ret = 0;               /* INIT twice would be terminal: the first one stands */
		} else {
			/* native selector: forward UNCHANGED to the real resident SPTM. The first
			 * forwards are instrumented: the last "apif fwd" printed before a hang names the
			 * VIOLATING (table,endpoint) = the M4<->M5 drift point. */
			static u64 nfwd;
			int trace = nfwd++ < 48;
			if (trace) {
				con_puts("apif fwd sel=");
				con_hex(op.selector);
				con_puts(" a0=");
				con_hex(op.arg[0]);
				con_puts("\n");
			}
			this_cpu()->bc_sel = op.selector;
			this_cpu()->bc_a0 = op.arg[0];
			bc_set(this_cpu(), 3);
			op.ret = sptm_call_raw(op.selector, op.arg[0], op.arg[1], op.arg[2],
					       op.arg[3], op.arg[4], op.arg[5]);
			bc_set(this_cpu(), 2);
			if (trace) {
				con_puts("apif ret=");
				con_hex(op.ret);
				con_puts("\n");
			}
			iommu_forwarded(op.selector, op.arg);
			if (op.selector == SPTM_RETYPE && op.arg[1] == XNU_DEFAULT && op.arg[2] == SPTM_XNU_IOMMU) {
				struct apif_frame *f = pool_of(op.arg[0]);
				if (f)
					f->iommu = 1;
			} else if (op.selector == SPTM_RETYPE && op.arg[1] == SPTM_XNU_IOMMU && op.arg[2] == XNU_DEFAULT) {
				/* the client took it back itself: SVC_FREE_FRAME must not retype it a second time, 0x42 is terminal */
				struct apif_frame *f = pool_of(op.arg[0]);
				if (f)
					f->iommu = 0;
			}
		}
		gw64(opa + 56, op.ret);              /* ret at op offset 7*8 */
	}
	if (done == count)
		gw32(batch + 8, 0);                  /* status OK */
	gw32(batch + 12, done);                  /* done = completed prefix */
}

int apif_is_reg(u64 ipa) { return (ipa & ~(APIF_SIZE - 1)) == APIF_BASE; }

/* register-page read (magic/version/max-ops/flags); unknown offsets read 0. */
u64 apif_read(u64 ipa)
{
	switch (ipa & (APIF_SIZE - 1)) {
	case 0x000: return APIF_MAGIC;
	case 0x008: return APIF_VER;
	case 0x010: return APIF_MAXOPS;
	case 0x018: return APIF_FLAGS;
	default:    return 0;
	}
}

/* register-page write; a 64-bit store to +0x100 submits a batch. */
void apif_write(u64 ipa, u64 val)
{
	if ((ipa & (APIF_SIZE - 1)) == 0x100) {
		g_batch_ipa = val;
		apif_run_batch(val);
	}
}
