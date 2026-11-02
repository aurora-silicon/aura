// SPDX-License-Identifier: GPL-2.0-only
// Linux guest: Image + DTB taken from the KC payload area (unused kext text), copied into stage-2 backed RAM.
#include "mon.h"
#include "gstub.h"

/* All usable RAM, mapped before Linux (MAP_PAGE is only legal outside the guest; SPTM has no install batch: UPDATE_REGION and
 * UPDATE_DISJOINT(_MULTIPAGE) only change fields of a valid PTE, sptm_xnu.h "Flags within the mask field"). RAM_CAP_GIB caps it. */
u64 g_ram_size;
#define RAM_SIZE g_ram_size
#define RAM_LOW    0x20000000UL            /* stub and ANS queue pages sit at the top of the first 512 MiB */
#define DTB_OFF    0x08000000UL            /* DTB    at RAM + 128 MiB */
#define INITRD_OFF 0x0a000000UL            /* initrd at RAM + 160 MiB */
#define PAYLOAD_MAGIC 0x4d4f4e504c4f4144UL   /* "DAOLPNOM" */

/* placeholder low words in the guest DTB /chosen; the monitor rewrites them with the
 * real initrd IPA start/end (big-endian). The hi cell is already 0 in the DTS. */
#define INITRD_START_PH 0x1d7a5700U
#define INITRD_END_PH   0x1d7a57edU
#define PH_HI           0xba5e0001U        /* initrd hi cell placeholder */
#define MEM_BASE_HI_PH  0xba5e0002U        /* /memory reg base hi cell placeholder */
#define MEM_BASE_LO_PH  0xba5e0003U        /* /memory reg base lo cell placeholder */

#ifdef P_PLATFORM
#include "platform-io.h"
#define ADT_OFF 0x14000000UL               /* ADT copy for the late PCIe init, after the 160 MiB initrd slot (kc_patch.py checks the size) */
extern u64 *g_boot_args;
#endif
extern const u64 kc_payload[];            /* linker: the start of kext __TEXT_EXEC, PC-relative */

struct payload { u64 magic, image_off, image_size, dtb_off, dtb_size, initrd_off, initrd_size; };

#define AIC_PA   0x502400000UL           /* AIC3 core + event (board.dtb interrupt-controller@502400000) */
#define AIC_SIZE 0x184000UL

/* the guest DT carries cpu@0 reg = <0 0xdeadbeef>; put the running core's MPIDR affinity there */
static void dtb_patch_mpidr(u64 va, u64 len)
{
	u32 aff = (u32)(sysreg_read(mpidr_el1) & 0xffffff);
	u32 be = __builtin_bswap32(aff);
	for (u64 i = 0; i + 4 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == 0xefbeaddeU)
			w32(va + i, be);
}

/* rewrite the /chosen linux,initrd-{start,end} placeholder low words with the real IPA */
static void dtb_patch_cell64(u64 va, u64 len, u32 lo_ph, u64 val)
{
	/* the DTB stores <hi lo> big-endian; the hi cell is the word immediately before the lo placeholder */
	for (u64 i = 0; i + 4 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(lo_ph)) {
			if (i >= 4)
				w32(va + i - 4, __builtin_bswap32((u32)(val >> 32)));
			w32(va + i, __builtin_bswap32((u32)val));
		}
}

static void dtb_patch_mem(u64 va, u64 len, u64 base)
{
	/* /memory reg = <base_hi base_lo size_hi size_lo>; patch the two base cells (placeholders are
	 * adjacent, hi then lo) to the identity window base */
	for (u64 i = 0; i + 8 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(MEM_BASE_HI_PH) &&
		    *(volatile u32 *)(va + i + 4) == __builtin_bswap32(MEM_BASE_LO_PH)) {
			w32(va + i, __builtin_bswap32((u32)(base >> 32)));
			w32(va + i + 4, __builtin_bswap32((u32)base));
		}
}

#if defined(P_APIF) || defined(P_FB)
/* patch a reserved-memory <hi lo ...> base pair (adjacent hi/lo placeholders) with a real PA */
static void dtb_patch_pa(u64 va, u64 len, u32 ph_hi, u32 ph_lo, u64 pa)
{
	for (u64 i = 0; i + 8 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(ph_hi) &&
		    *(volatile u32 *)(va + i + 4) == __builtin_bswap32(ph_lo)) {
			w32(va + i, __builtin_bswap32((u32)(pa >> 32)));
			w32(va + i + 4, __builtin_bswap32((u32)pa));
		}
}

/* patch a single <cell> placeholder with a value */
static void dtb_patch_cell(u64 va, u64 len, u32 ph, u32 val)
{
	for (u64 i = 0; i + 4 <= len; i += 4)
		if (*(volatile u32 *)(va + i) == __builtin_bswap32(ph))
			w32(va + i, __builtin_bswap32(val));
}
#endif

static void clean_dcache(u64 va, u64 len)
{
	for (u64 p = va & ~63UL; p < va + len; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");   /* dc cvac is a no-op on Apple Silicon (Gravity bbcc3c0) */
	dsb();
}

static void copy_into(u64 dst_va, const u8 *src, u64 n)
{
	for (u64 i = 0; i < n; i += 8)
		w64(dst_va + i, *(const u64 *)(src + i));
}

#define STUB_OFF (RAM_LOW - 6 * PAGE)        /* below the 4 ANS queue pages: stub, then its params page */
extern const u8 gstub_start[], gstub_end[];

/* the guest starts in gstub.c, which returns Linux's entry; x1 of the guest = params page */
static int stub_load(u64 gpa, u64 linux_entry)
{
	u64 sv = hmap(gpa + STUB_OFF, 0), pv = hmap(gpa + STUB_OFF + PAGE, 0);
	u64 n = gstub_end - gstub_start;
	if (!sv || !pv || n > PAGE)
		return -1;
	copy_into(sv, gstub_start, (n + 7) & ~7UL);
	clean_dcache(sv, n);
	struct gstub_params *p = (void *)pv;
	p->fb = g_vbase;
	p->stride = g_vstride;
	p->width = g_vw;
	p->height = g_vh;
	p->entry = linux_entry;
	p->host_fb = fb != 0;
	con_puts("mon: entering guest stub\n");
	p->log_len = con_snapshot((char *)pv + sizeof(*p), 0x3000 - sizeof(*p));
	clean_dcache(pv, PAGE);
	return 0;
}


u64 g_gpa;

int linux_load(u64 *entry, u64 *dtb_ipa)
{
	const struct payload *p = (const void *)kc_payload;
	if (p->magic != PAYLOAD_MAGIC) {
		con_puts("mon: no Linux payload\n");
		return -1;
	}
	const u8 *base = (const u8 *)kc_payload;
	const u8 *img = base + p->image_off, *dtb = base + p->dtb_off;
	const u8 *initrd = base + p->initrd_off;
	con_puts("mon: L1 carve\n");
	u64 gpa = carve_identity(0);             /* identity window, IPA==PA */
	g_gpa = gpa;
	u64 top = ram_top(g_vbase);
#ifdef QEMU_RAM_CAP
	if (top > first_frame_hint() + QEMU_RAM_CAP)
		top = first_frame_hint() + QEMU_RAM_CAP;
#endif
	RAM_SIZE = (top - gpa) & ~(PAGE - 1);
#ifdef RAM_CAP_GIB
	if (RAM_SIZE > (u64)RAM_CAP_GIB << 30)
		RAM_SIZE = (u64)RAM_CAP_GIB << 30;
#endif
	carve_identity(RAM_SIZE);
	con_puts("mon: RAM base ");
	con_hex(gpa);
	con_puts(" top ");
	con_hex(gpa + RAM_SIZE);
	con_puts(" pages ");
	con_hex(RAM_SIZE / PAGE);
	con_puts("\n");
	u64 t0 = sysreg_read(cntpct_el0), n0 = g_sptm_calls;
	u64 text_off = *(const u64 *)(img + 8);
	u64 img_ipa = gpa + text_off;
	u64 initrd_ipa = gpa + INITRD_OFF;
	con_puts("mon: Linux Image size ");
	con_hex(p->image_size);
	con_puts(" dtb ");
	con_hex(p->dtb_size);
	con_puts(" initrd ");
	con_hex(p->initrd_size);
	con_puts("\n");
	for (u64 off = 0; off < RAM_SIZE; off += PAGE) {
		u64 ipa = gpa + off, pa = ipa;   /* identity */
		const u8 *src = 0;
		if (ipa >= img_ipa && ipa < img_ipa + p->image_size) {
			src = img + (ipa - img_ipa);
		} else if (off >= DTB_OFF && off < DTB_OFF + p->dtb_size) {
			src = dtb + (off - DTB_OFF);
		} else if (p->initrd_size && off >= INITRD_OFF &&
			   off < INITRD_OFF + p->initrd_size) {
			src = initrd + (off - INITRD_OFF);
		}
		if (src) {
			u64 va = hmap(pa, 0);
			if (!va)
				return -1;
			copy_into(va, src, PAGE);   /* the payload area is page padded */
			clean_dcache(va, PAGE);
		}
		s2_map(ipa, pa, 0);
		if (!(off & ((4UL << 30) - 1))) {   /* a terminal violation stops here: the last line names the GiB */
			con_puts("mon: RAM mapped to ");
			con_hex(ipa);
			con_puts("\n");
		}
	}
	{
		u64 t1 = sysreg_read(cntpct_el0);
		con_puts("mon: RAM map calls ");
		con_hex(g_sptm_calls - n0);
		con_puts(" ticks ");
		con_hex(t1 - t0);
		con_puts(" cntfrq ");
		con_hex(sysreg_read(cntfrq_el0));
		con_puts(" frames used ");
		con_hex(frames_used());
		con_puts("\n");
#ifdef RAM_STOP
		for (;;)                         /* qemu measurement build: leave the line on the panel */
			__asm__ volatile("wfi");
#endif
	}
	con_puts("mon: L2 pages copied\n");
	{	/* the DTB outgrew one page (42 KiB with the SMC nodes): patch it whole, over its contiguous PAPT alias */
		u64 va = hmap(gpa + DTB_OFF, 0), len = p->dtb_size;
		if (!va || hmap(gpa + DTB_OFF + len - 1, 0) != va + len - 1) {
			con_puts("mon: DTB not contiguous in the PAPT\n");
			return -1;
		}
		dtb_patch_mpidr(va, len);
		dtb_patch_mem(va, len, gpa);
		dtb_patch_pa(va, len, 0xba5e0004, 0xba5e0005, RAM_SIZE);
		if (p->initrd_size) {
			dtb_patch_cell64(va, len, INITRD_START_PH, initrd_ipa);
			dtb_patch_cell64(va, len, INITRD_END_PH, initrd_ipa + p->initrd_size);
		}
#ifdef P_APIF
		{
			/* 4 ANS queue pages reserved at the top of the identity window;
			 * patch the reserved-memory placeholders with their PA (==IPA). */
			u64 q = gpa + RAM_LOW - 4 * PAGE;
			dtb_patch_pa(va, len, 0xba5e0100, 0xba5e0101, q);
			dtb_patch_pa(va, len, 0xba5e0102, 0xba5e0103, q + PAGE);
			dtb_patch_pa(va, len, 0xba5e0104, 0xba5e0105, q + 2 * PAGE);
			dtb_patch_pa(va, len, 0xba5e0106, 0xba5e0107, q + 3 * PAGE);
		}
#endif
		{
			/* Reserve the CTRR / AMCC read-only region in the guest DT. The AMCC enforces this
			 * physical range read-only (writable only from the boot CPU), so an NVMe DMA target
			 * (a READ's destination = ANS DMA write) that Linux places inside it has its write
			 * blocked by the memory controller -> the command fails as a device I/O error, NOT an
			 * SPTM violation (so the machine resets, it does not halt). Matches Gravity kboot.c:377.
			 * Reserving a range outside guest RAM is a harmless no-op. CTRR_LOWER/UPPER_EL1 read at
			 * EL2 as m1n1/Gravity/Asahi do; a hang here would print after "reading CTRR". */
			con_puts("mon: reading CTRR\n");
			u64 clo = sysreg_read(S3_0_C11_C0_0);   /* CTRR_LOWER_EL1 */
			u64 chi = sysreg_read(S3_0_C11_C0_1);   /* CTRR_UPPER_EL1 */
			con_puts("mon: CTRR ");
			con_hex(clo);
			con_puts("..");
			con_hex(chi);
			con_puts("\n");
			u64 cstart, csize;
			if (chi > clo && clo >= (1UL << 40)) {   /* plausible J616s DRAM PA */
				cstart = clo & ~(PAGE - 1);
				csize = ((chi + 0x4000 + PAGE - 1) & ~(PAGE - 1)) - cstart;
				if (cstart < gpa + RAM_SIZE && cstart + csize > gpa)
					con_puts("mon: CTRR OVERLAPS guest RAM (reserving to keep NVMe DMA out)\n");
				else
					con_puts("mon: CTRR outside guest RAM (reservation is a no-op)\n");
			} else {
				cstart = gpa + STUB_OFF;   /* unreadable/implausible: re-reserve the stub page, harmless */
				csize = PAGE;
				con_puts("mon: CTRR implausible, reserving the stub page only\n");
			}
			dtb_patch_pa(va, len, 0xba5e0200, 0xba5e0201, cstart);
			dtb_patch_pa(va, len, 0xba5e0202, 0xba5e0203, csize);
		}
#ifdef P_APIF
		dtb_patch_cell(va, len, 0xba5e0210, 0);   /* apple,nvme-shim: no PRP-scratch shim, the guest driver caps transfers */
#endif
#ifdef P_FB
		/* guest simplefb reg base -> live boot_args.video.base scanout PA */
		dtb_patch_pa(va, len, 0xba5e0300, 0xba5e0301, g_vbase);
#endif
#ifdef P_IOMMU
		iommu_patch_dtb(va, len);
#endif
		clean_dcache(va, len);
	}
	{	/* the payload as it arrived in guest RAM, against the sum kc_patch.py prints */
		u64 sum = 0, n = p->image_size & ~7UL;
		for (u64 off = 0; off < n; off += PAGE) {
			u64 va = hmap(img_ipa + off, 0);
			for (u64 i = 0; i < PAGE && off + i < n; i += 8)
				sum = (sum << 1 | sum >> 63) ^ *(volatile u64 *)(va + i);
		}
		con_puts("mon: Image sum ");
		con_hex(sum);
		con_puts("\n");
	}
	/* the guest's arch timer and AIC3 are the real ones, passed through 1:1 (IMO/FMO stay 0 in the guest HCR) */
	for (u64 a = AIC_PA; a < AIC_PA + AIC_SIZE; a += PAGE)
		s2_map(a, a, 1);
#ifdef P2_VIRTIO
	s2_map(0x510000000UL, 0x510000000UL, 1);   /* P2 (qemu only): virtio-mmio block transport, device 1:1.
	                                            * NOT on HW: this PA is not a registered io-range -> MAP_PAGE
	                                            * would be a terminal SPTM violation. */
#endif
	con_puts("mon: L3 AIC mapped\n");
#ifdef P_PLATFORM
	{	/* boot_args: virtBase [1], physBase [2], deviceTreeP [12], deviceTreeLength [13] (xnu arm/boot.h) */
		u64 *ba = g_boot_args, len = ba[13] & 0xffffffffUL, pa = ba[12] - ba[1] + ba[2], done = 0;
		con_puts("mon: ADT pa ");
		con_hex(pa);
		con_puts(" len ");
		con_hex(len);
		con_puts("\n");
		if (len && len < 0x1000000UL && hmap(pa, 0)) {
			u64 dst = hmap(gpa + ADT_OFF, 0);    /* range 1 is linear */
			for (u64 o = 0; o < len; o += 8) {
				u64 s = hmap(pa + o, 0);
				if (!s)
					break;
				w64(dst + o, *(volatile u64 *)s);
				done = o + 8;
			}
			clean_dcache(dst, len);
		}
		u64 dv = hmap(gpa + DTB_OFF, 0);
		dtb_patch_pa(dv, p->dtb_size, 0xba5e0400, 0xba5e0401, done ? gpa + ADT_OFF : gpa + INITRD_OFF);
		dtb_patch_cell64(dv, p->dtb_size, 0xba5e0402, done ? ((len + PAGE - 1) & ~(PAGE - 1)) : PAGE);
#ifdef P_IOMMU
		{	/* as m4-loader metadata.c: the iBoot carveouts stay out of Linux. Below RAM_LOW they hold the payload itself (HW F18+) */
			u64 from = gpa + RAM_LOW;
			for (u32 i = 0; i < 3; i++) {
				u64 b, e;
				if (adt_carveout_span(from, gpa + RAM_SIZE, &b, &e) || e - b > (2UL << 30)) {
					b = gpa + STUB_OFF;
					e = b + PAGE;
				} else {
					from = e;
				}
				dtb_patch_pa(dv, p->dtb_size, 0xba5e0910 + i * 3, 0xba5e0911 + i * 3, b);
				dtb_patch_cell64(dv, p->dtb_size, 0xba5e0912 + i * 3, e - b);
				con_puts("mon: carveout ");
				con_hex(b);
				con_puts("..");
				con_hex(e);
				con_puts("\n");
			}
			{
				u64 b, e;
				if (!adt_carveout_span(from, gpa + RAM_SIZE, &b, &e)) {
					con_puts("mon: carveout NOT RESERVED ");
					con_hex(b);
					con_puts("\n");
				}
			}
		}
		{	/* as m4-loader fixups.c:243: iBoot's /chosen/random-seed becomes rng-seed, the CRNG is ready at early init */
			u32 l = 0;
			const u32 *s = adt_prop(adt_path("chosen"), "random-seed", &l);
			for (u32 i = 0; i < 8; i++)
				dtb_patch_cell(dv, p->dtb_size, 0xba5e0900 + i, s && l >= 32 ? s[i] : (u32)sysreg_read(cntpct_el0) * 0x9e3779b1U + i);
			con_puts("mon: rng-seed bytes in ADT ");
			con_hex(l);
			con_puts("\n");
		}
#endif
		clean_dcache(dv, p->dtb_size);
		con_puts("mon: ADT copied ");
		con_hex(done);
		con_puts("\n");
		for (u64 i = 0; i < sizeof(platform_io) / sizeof(platform_io[0]); i++) {
			for (u64 a = platform_io[i].pa; a < platform_io[i].pa + platform_io[i].size; a += PAGE)
				s2_map(a, a, platform_io[i].pa == 0xb80000000UL ? 3 : 1);
			con_puts("mon: io mapped ");
			con_hex(platform_io[i].pa);
			con_puts(" bad ");
			con_hex(g_s2_bad);
			con_puts("\n");
		}
#ifdef PLAT_STOP
		for (;;)                         /* qemu measurement build: leave the lines on the panel */
			__asm__ volatile("wfi");
#endif
	}
#endif
#ifdef P_WDT
	s2_map(0x50836c000UL, 0x50836c000UL, 1);   /* iBoot watchdog: the guest's apple_wdt pings it (else reset ~128 s) */
	con_puts("mon: WDT mapped for the guest\n");
#endif
#ifdef P_APIF
	/* APIF disk: the guest maps only the nvme controller + ans coprocessor control (incl. the ASC
	 * mailbox) + the pmgr pwrstate page. The real SPTM owns the NVMMU and SART (driven via APIF
	 * table-6/table-5), so they are deliberately NOT mapped into the guest. */
	s2_map(0x502280000UL, 0x502280000UL, 1);                               /* pmgr pwrstate (ON) */
#ifndef APIF_SKIP_DEVMAP
	con_puts("mon: P_APIF nvme\n");
	con_puts("mon: P_APIF ans\n");
	for (u64 a = 0x409600000UL; a < 0x409600000UL + 0xc000UL; a += PAGE)    /* ans ctrl + ASC mailbox */
		s2_map(a, a, 1);
#endif
	/* controller registers as XNU uses them: its register base is ADT ans reg[3] (map index 3), which also holds the linear doorbells at +0x2490c. The 0x28000+ and reg[9] windows are
	 * XNU_PROTECTED_IO, used only by SPTM's table 6. */
	for (u64 a = 0x40dcc0000UL; a < 0x40dcc0000UL + 0x28000UL; a += PAGE)
		s2_map(a, a, 1);
	/* SMC: controller and mailbox, its SRAM window (fans, sensors; t6040.dtsi). Both MISS in the io-range tables */
	for (u64 a = 0x50c600000UL; a < 0x50c600000UL + 0x88000UL; a += PAGE)
		s2_map(a, a, 1);
	for (u64 a = 0x50de70000UL; a < 0x50de70000UL + 0x100000UL; a += PAGE)
		s2_map(a, a, 1);
	con_puts("mon: L4 APIF-ANS mapped\n");
	/* TODO(quiesced-ANS bringup): confirm iBoot left ANS quiesced (CPU_CONTROL.RUN clear), then
	 * register the 4 reserved queue pages (top of window) with the NVMMU TCB bases
	 * (nvmmu+0x28108 ASQ, +0x28110 IOSQ) via the Gravity src/nvme.c stopped-handoff sequence. This
	 * programming may itself route through the real SPTM (table-6) — resolve in the qemu cross-gen
	 * test. The 4 pages are already identity-mapped (part of the guest-RAM loop) and carved
	 * no-map in the DT. */
#endif
#ifdef P_GPU
	{ extern void gpu_s2_map(void); gpu_s2_map(); }
#endif
#ifdef P3_ANS
	/* P3: Apple NVMe/ANS passthrough for the guest apple-nvme driver (CPU-visible PAs, board-163.dts).
	 * Device 1:1; the ANS main window covers the ASC mailbox at 0x409608000. */
	for (u64 a = 0x409600000UL; a < 0x409600000UL + 0x88000UL; a += PAGE)  /* ANS main + ASC mailbox */
		s2_map(a, a, 1);
	for (u64 a = 0x40dcc0000UL; a < 0x40dcc0000UL + 0x61000UL; a += PAGE)  /* NVMMU */
		s2_map(a, a, 1);
	for (u64 a = 0x40dc50000UL; a < 0x40dc50000UL + 0xc000UL; a += PAGE)   /* SART */
		s2_map(a, a, 1);
	for (u64 a = 0x44dcc0000UL; a < 0x44dcc0000UL + 0x10000UL; a += PAGE)  /* NVMe */
		s2_map(a, a, 1);
	s2_map(0x502280000UL, 0x502280000UL, 1);   /* PMGR pwrstate page (regs already ON from iBoot) */
#endif
	con_puts("mon: L4 ANS mapped\n");
#ifdef P_FB
	/* simplefb (AH path 1a): iBoot/DCP live scanout carveout 0x10bd1474000 (+0x1d74000 = 1885 pages,
	 * scanout 0x1d73c00). Map identity as Normal-NC (device=2): with FWB 1 (ep27 forces HCR 0x400480000001) NC holds whatever the
	 * guest's stage 1 says and DCP sees the writes from DRAM. PA is inside [dram-base,dram-base+dram-size); map_page accepts type-0
	 * into STAGE2_PT. Same L1 slot (ipa>>36 = 0x10) as the guest-RAM window, so s2_l2[16] is reused. */
	{
		u64 fb0 = g_vbase & ~(PAGE - 1);
		for (u64 a = fb0; a < g_vbase + g_vstride * g_vh; a += PAGE)
			s2_map(a, a, 2);   /* Normal-NC: guest fb writes must reach DRAM for the scanout DMA */
	}
	con_puts("mon: L5 fb mapped\n");
#endif
#ifdef P_IOMMU
	iommu_map_devices();
#endif
	con_puts("mon: stage-2 status: bad ");
	con_hex(g_s2_bad);
	con_puts(" first ");
	con_hex(g_s2_first);
	con_puts("\n");
	if (stub_load(gpa, img_ipa))
		return -1;
	/* publish the stage-2 maps: SPTM MAP_PAGE publishes internally, but add an explicit guest stage-1&2
	 * TLBI (VMID 1) as insurance against a stale/speculative entry — aura notes T6040 EL2 publication
	 * needs an explicit TLBI (host = ALLE2IS; guest stage-2 = VMALLS12E1IS). dsb before/after + isb. */
	__asm__ volatile("dsb ish; tlbi vmalls12e1is; dsb ish; ic ialluis; dsb ish; isb" ::: "memory");
	*entry = gpa + STUB_OFF;
	*dtb_ipa = gpa + DTB_OFF;
	con_puts("mon: guest RAM identity base ");
	con_hex(gpa);
	con_puts("\n");
	return 0;
}
