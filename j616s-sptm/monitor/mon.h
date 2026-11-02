// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#pragma GCC visibility push(hidden)   /* PC-relative only: the image runs slid, no GOT */
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

u64 sptm_call_raw(u64 id, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5);
void wdt_sub(u32 what);
/* every SPTM call is counted on the WDT ladder first: a call that never returns (a terminal SPTM
 * violation, as the fb MAP_PAGE was on HW) leaves its index in the reset interval */
static inline u64 sptm_call(u64 id, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5)
{
	wdt_sub(0);
	return sptm_call_raw(id, a0, a1, a2, a3, a4, a5);
}

#define SPTM_LOCKDOWN       0
#define SPTM_RETYPE         1
#define SPTM_MAP_PAGE       2
#define SPTM_MAP_TABLE      3
#define SPTM_REGISTER_CPU   14
#define SPTM_GUEST_DISPATCH 27
#define SPTM_GUEST_EXIT     28

#define XNU_DEFAULT             11
#define XNU_PAGE_TABLE          20
#define XNU_STAGE2_ROOT_TABLE   33
#define XNU_STAGE2_PAGE_TABLE   34

#define PAGE 0x4000UL

#define sysreg_read(r) ({ u64 __v; __asm__ volatile("mrs %0, " #r : "=r"(__v)); __v; })
#define sysreg_write(r, v) __asm__ volatile("msr " #r ", %0" :: "r"((u64)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")
#define dsb() __asm__ volatile("dsb sy" ::: "memory")

/* F12: clean+invalidate a DRAM range to the point of coherency (dc civac), so a non-coherent ANS DMA
 * read of a PA sees the CPU's writes. Used by the NVMe PRP-scratch flush shim. */
static inline void mon_civac(u64 va, u64 len)
{
	for (u64 p = va & ~63UL; p < va + len; p += 64)
		__asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

static inline void w32(u64 a, u32 v) { *(volatile u32 *)a = v; }
static inline u32 r32(u64 a) { return *(volatile u32 *)a; }
static inline void w64(u64 a, u64 v) { *(volatile u64 *)a = v; }

struct gctx {            /* layout fixed by vectors.S */
	u64 x[31];
	u64 pc, spsr, esr, far, hpfar;
};

/* per-CPU monitor state. A slot is cluster << 3 | core of MPIDR, so asm needs no table and SPTM's clobbers don't matter:
 * MPIDR is the one thing still valid after a GENTER on the exit path. */
#define MAX_SLOTS 24
#define PSCI_OFF 1
#define PSCI_ON 0
#define PSCI_PENDING 2
struct mcpu {
	u64 gs_pa, gs, stack;             /* guest state page, sp for redispatch */
	u64 el12[32], actlr;
	u64 entry, ctx;                   /* CPU_ON arguments */
	volatile u32 state, wake;
	u32 started;                      /* SPTM reset path done once: later CPU_ONs leave the park loop */
	u64 fpmr;
	u64 l_trap[6];                    /* world L's MDCR and FGT registers while world V runs: GUEST_EXIT leaves them as they are */
	struct { u64 sr, v; } sh[16];     /* IMPDEF sysreg shadow (PMC masks) */
	/* F33 breadcrumb, read by another core with HVC 0x7504: where this core is at EL2. stage 0 guest, 1 waiting for the APIF
	 * lock, 2 in an APIF batch, 3 in SPTM for an op, 9 terminated. Plain stores, no lock: the reader may see a torn set. */
	volatile u64 bc_stage, bc_ec, bc_sel, bc_a0, bc_t, bc_exits;
};
static inline void bc_set(struct mcpu *s, u64 stage)
{
	s->bc_t = sysreg_read(cntpct_el0);
	s->bc_stage = stage;
}
extern struct mcpu g_cpu[MAX_SLOTS];
extern u64 g_cpu_top[MAX_SLOTS][2];       /* exception sp, run sp; read by mon_secondary */
static inline u64 mpidr_slot(u64 m) { return (((m >> 8) & 3) << 3) | (m & 7); }
static inline struct mcpu *this_cpu(void) { return &g_cpu[mpidr_slot(sysreg_read(mpidr_el1))]; }
static inline void spin_lock(volatile u32 *l)
{
	while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
		__asm__ volatile("isb");
}
static inline void spin_unlock(volatile u32 *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

/* smp.c */
int smp_hvc(struct gctx *c);
void smp_boot_cpu_ready(void);
#define GS_ROOT  0x000
#define GS_CPTR  0x090
#define GS_X0    0x0a8
#define GS_PSTATE 0xa40
#define GS_PC    0xa48
#define SCTLR_EL1_MMU_OFF 0x30500800UL    /* Linux INIT_SCTLR_EL1_MMU_OFF */
#define PSTATE_EL1H_MASKED 0x3c5UL
void guest_to_host(struct gctx *c);
void host_to_guest(void) __attribute__((noreturn));

/* mm.c */
void mm_init(u64 *bootstrap);
u64 frame_alloc(void);
u64 frames_used(void);
u64 ram_top(u64 fb_pa);
u64 first_frame_hint(void);
extern u64 g_ba_top, g_sptm_calls;
u64 frame_alloc_pool(void);             /* 0 when the pool below the guest window is used up */
u64 hmap_dev(u64 pa, u64 npages);
u64 hmap_dev_ro(u64 pa, u64 npages);     /* F34: read-only device page, Table A frames */
u64 iommu_dart_err(u64 id, u64 inst, u64 *status, u64 *addr, u64 *sid, u64 *info);   /* F34: T8110 fault state, ~0UL when unmapped; info = ver | nsids << 16 */
u64 carve_identity(u64 size);
u64 hmap(u64 pa, int device);
u64 hmap_fb(u64 pa, u64 npages);          /* boot fb, Normal-NC, in a pre-built L3 above physmap_end */          /* map one 16 KiB frame into the host window, return its VA or 0 */
int papt_has_va(u64 va, u64 len);
void hmap_new_window(void);
u64 hmap_fb(u64 pa, u64 npages);
extern u64 g_sptm_err, g_hwin, g_hwin_skipped;   /* (status << 8) | 2 MAP_TABLE / 3 MAP_PAGE */
void s2_init(void);
void s2_map(u64 ipa, u64 pa, int device);
extern u64 s2_root, g_s2_bad, g_s2_first;

/* fb.c */
void fb_init(u64 *boot_args);
void fb_off(void);
extern int fb_ok, fb_fail;
extern u64 fb;
void fb_putc(char c);
void fb_band(u64 k, u32 colour);
void fb_putc(char c);
enum { WHITE = 0x3FFFFFFF, YELLOW = 0x3FFFFC00, MAGENTA = 0x3FF003FF, RED = 0x3FF00000,
       GREEN = 0x000FFC00, CYAN = 0x000FFFFF, BLUE = 0x000003FF, GREY = 0x1FF7FDFF };

/* con.c */
void con_init(u64 *boot_args);
void con_putc(char c);
void con_puts(const char *s);
void con_hex(u64 v);
/* live DCP scanout geometry captured from XNU boot_args.video at monitor entry (base/stride/width/height);
 * the guest simplefb targets this so it paints the real scanout iBoot left live, not a stale hardcode. */
extern u64 g_vbase, g_vstride, g_vw, g_vh;
void con_replay(void);
void con_fault_unlock(void);
u64 con_snapshot(char *dst, u64 max);
extern int g_tso;

/* linux.c */
int linux_load(u64 *entry, u64 *dtb_ipa);


/* adt.c — live ADT, read only */
int adt_init(u64 *boot_args);
const void *adt_path(const char *path);
const void *adt_prop(const void *node, const char *name, u32 *len);
u32 adt_u32(const void *node, const char *name, u32 dflt);
u64 adt_u64(const void *node, const char *name, u64 dflt);
int adt_carveout_span(u64 from, u64 lim, u64 *base, u64 *end);

/* apif.c — Aurora Platform Interface v2 provider (register page @guest IPA 0x61ff0000) */
int apif_is_reg(u64 ipa);
u64 apif_read(u64 ipa);
void apif_write(u64 ipa, u64 val);
int iommu_boot_info(u64 id, u64 sid, u64 idx, u64 *out);
u64 iommu_boot_table(u64 id, u64 sid, u64 dva, u64 level);
int iommu_guard(u64 sel, const u64 *a);
void iommu_forwarded(u64 sel, const u64 *a);
int iommu_guest_ram(u64 ipa, u64 size);
u64 frame_alloc_checked(void);
void iommu_setup(void);                   /* DART facts from the ADT, before the guest DT is patched */
void iommu_patch_dtb(u64 va, u64 len);    /* publish them into the guest DT, nodes stay disabled on any doubt */
void iommu_map_devices(void);             /* stage-2 MMIO of the devices that were published */
extern u64 g_gpa;

/* guest.c, mhv.c, vgic_cpuif.c */
void world_save(struct gctx *c, u64 gs);
void guest_to_host(struct gctx *c);
void host_to_guest(void) __attribute__((noreturn));
void host_timers_restore(void);
void host_return(u64 v) __attribute__((noreturn));
void redispatch(u64 gs_pa, u64 sp) __attribute__((noreturn));
void mhv_hvc(struct gctx *c);
void mhv_vm_exit(struct gctx *c, u64 async);
int mhv_in_vm(void);
struct mhv_run;
int mhv_cpuif_access(struct gctx *c, struct mhv_run *r);
u64 mhv_get_lr(unsigned int lr);
void mhv_set_lr(u64 val, unsigned int lr);
u64 mhv_get_ap0r(unsigned int n);
u64 mhv_get_ap1r(unsigned int n);
void mhv_set_ap0r(u64 val, unsigned int n);
void mhv_set_ap1r(u64 val, unsigned int n);
/* guest.c */
void guest_run(u64 entry_ipa, u64 x0, u64 x1);
int ladder_draw(u64 *boot_args, u64 *args);
void mon_guest_exit(struct gctx *c, u64 async);
int wdt_arm(u32 seconds);
void wdt_disarm(void);
void wdt_stage(u32 seconds);
/* WDT progress ladder (seconds). The interval between two WDT resets (panicmedic-timestamps) is the
 * last stage's timeout plus the iBoot share (v7b 127.846 s, v8 127.842 s for 120). Inside a stage the
 * n-th SPTM call re-arms with +100 + 1.2 * n ms first (n from 0 at each stage, capped at 2000), so a
 * call that never returns is identified by index; the qemu-sptm run of the same build lists the calls. */
#define WDT_SUB_CALL0  100
#define WDT_HOST_FAULT 145                /* monitor took a host exception */
#define WDT_ENTRY      120                /* WDT armed */
#define WDT_ALIVE      110                /* first re-arm (proves the ladder itself on HW) */
#define WDT_HOST_LOCK  90                 /* host ACTLR probe done, TSO bits locked; then s2 + Linux load */
#define WDT_HOST_TSO   80                 /* host ACTLR probe done, a TSO bit flipped */
#define WDT_GUEST_IN   70
#define WDT_GUEST_EXIT 60
#define WDT_USERSPACE  50                 /* guest console: "TSO-INIT: userspace reached" */
#define WDT_TSO_ON     46                 /* "PR_SET_MEM_MODEL_TSO = OK", or guest_test probe 0x12 */
#define WDT_TSO_OFF    42
#define WDT_NVME_MODS  38                 /* guest console: "NVME: modules loaded" */
#define WDT_NVME_CTRL  34                 /* "NVME: controller up" (apple-nvme/RTKit probed on HW) */
#define WDT_NVME_ROOT  28                 /* "NVME-ROOT-OK" = omarchy root read RO = disk milestone done */
#define WDT_TERMINATE  20
#define WDT_PANIC      10                 /* guest console: "Kernel panic" */
#define WDT_HOST_BACK  160                /* guest returned to the monitor */
