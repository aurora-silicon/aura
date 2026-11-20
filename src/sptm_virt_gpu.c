/* SPDX-License-Identifier: MIT */

/*
 * GPU bring-up trace under the virtualized SPTM (SPTM_VIRT_GPU): pass-through stage-2 hooks on
 * the AGX register window (/arm-io/sgx reg 0, which holds the GPU's PMGR, its start control and
 * both ASC coprocessors) and, unless the host says not to, on the PMP's dashboard and mailbox.
 * Every access XNU makes there is performed for real and recorded in a ring in aura's carve-out;
 * writes, the first read of each register and everything in the KSM block are also printed.
 * After a host-set delay from the first GPU access the core that next touches the GPU parks in
 * aura's proxy, so the host can read the ring (sptm_virt_gpu_ring, _head, _nrec, _keep) and the
 * queue configurations (_configs).
 */

#include "sptm_virt.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"

#define GPU_RING_BYTES_MIN (16UL << 20) /* if the heap has no room for the asked size */
#define GPU_RING_KEEP      16384
#define GPU_SEEN_MAX       2048
#define GPU_MAX_RANGES     10

struct gpu_rec {
    u64 t;    /* CNTPCT_EL0 */
    u64 meta; /* pa [47:0] | width [50:48] | write [51] | cpu [55:52] */
    u64 val;  /* value written, or read */
    u64 pc;   /* guest PC */
};

/*
 * For the host, after the park. Record i is in slot i while i < sptm_virt_gpu_keep (the GPU's
 * boot, with the firmware's init messages); later records wrap in the slots after those.
 */
struct gpu_rec *sptm_virt_gpu_ring;
u64 sptm_virt_gpu_nrec;
volatile u64 sptm_virt_gpu_head;
u64 sptm_virt_gpu_keep = GPU_RING_KEEP;

/*
 * Set by the host before entry (run_guest_sptm.py). An ESP boot has no host, so a trace build
 * sets them instead (-DSPTM_VIRT_GPU_STOP_SECS=... and so on).
 */
#ifndef SPTM_VIRT_GPU_STOP_SECS
#define SPTM_VIRT_GPU_STOP_SECS 0
#endif
#ifndef SPTM_VIRT_GPU_STOP_BOOT_SECS
#define SPTM_VIRT_GPU_STOP_BOOT_SECS 0
#endif
#ifndef SPTM_VIRT_GPU_STOP_MSG
#define SPTM_VIRT_GPU_STOP_MSG 0
#endif
#ifndef SPTM_VIRT_GPU_PARK_KICKS
#define SPTM_VIRT_GPU_PARK_KICKS 0
#endif
#ifndef SPTM_VIRT_GPU_PARK_ARM_SECS
#define SPTM_VIRT_GPU_PARK_ARM_SECS 0
#endif
#ifndef SPTM_VIRT_GPU_PARK_BURST
#define SPTM_VIRT_GPU_PARK_BURST 0
#endif
#ifndef SPTM_VIRT_GPU_PARK_BIG
#define SPTM_VIRT_GPU_PARK_BIG 100
#endif
#ifndef SPTM_VIRT_GPU_PARK_GAP_MS
#define SPTM_VIRT_GPU_PARK_GAP_MS 1500
#endif
#ifndef SPTM_VIRT_GPU_PRINT_MAX
#define SPTM_VIRT_GPU_PRINT_MAX 20000
#endif
#ifndef SPTM_VIRT_GPU_RING_MIB
#define SPTM_VIRT_GPU_RING_MIB 64 /* a steady stream of kicks fills 16 MiB in seconds */
#endif
#ifndef SPTM_VIRT_GPU_PMP
#ifdef SPTM_VIRT_GPU_HOP_PAGES
#define SPTM_VIRT_GPU_PMP 0 /* the hop pages alone */
#else
#define SPTM_VIRT_GPU_PMP 1
#endif
#endif
/* Park this long after the first GPU access; 0 = never. */
u32 sptm_virt_gpu_stop_secs = SPTM_VIRT_GPU_STOP_SECS;
/* Park this long after the guest's entry, GPU or not; 0 = never. */
u32 sptm_virt_gpu_stop_boot_secs = SPTM_VIRT_GPU_STOP_BOOT_SECS;
/* Park after a write of this value (a mailbox message); 0 = never. */
u64 sptm_virt_gpu_stop_msg = SPTM_VIRT_GPU_STOP_MSG;
/*
 * Park at the Nth kick publication on an app's queue rings, so the park lands inside that app's
 * own GPU burst while its descriptors are still in the shared pool; 0 = never. An app's rings are
 * the ones configured ARM_SECS or more after the first GPU access: the system configures its
 * rings during boot, and the ring ids differ from boot to boot. Both must be set.
 */
u32 sptm_virt_gpu_park_kicks = SPTM_VIRT_GPU_PARK_KICKS;
u32 sptm_virt_gpu_park_arm_secs = SPTM_VIRT_GPU_PARK_ARM_SECS;
/*
 * An app's GPU work comes in bursts (one per benchmark workload) with quiet gaps between them. A
 * gap of GAP_MS ends a burst, and only bursts of BIG kicks or more count. With BURST = n, the park
 * comes at kick PARK_KICKS of the n-th big burst; 0 = off (PARK_KICKS counts every app kick).
 */
u32 sptm_virt_gpu_park_burst = SPTM_VIRT_GPU_PARK_BURST;
u32 sptm_virt_gpu_park_big = SPTM_VIRT_GPU_PARK_BIG;
u32 sptm_virt_gpu_park_gap_ms = SPTM_VIRT_GPU_PARK_GAP_MS;
volatile u32 sptm_virt_gpu_app_kicks; /* kicks counted on app rings */
u32 sptm_virt_gpu_park_ring;          /* the ring of the latest of them */
u32 sptm_virt_gpu_bursts_done;        /* big app bursts completed */
u32 sptm_virt_gpu_burst_kicks;        /* kicks in the current app burst */
u32 sptm_virt_gpu_print_max = SPTM_VIRT_GPU_PRINT_MAX;
u32 sptm_virt_gpu_ring_mib = SPTM_VIRT_GPU_RING_MIB;
u32 sptm_virt_gpu_pmp = SPTM_VIRT_GPU_PMP; /* also hook the PMP's dashboard and mailbox */

static struct {
    u64 base, size;
    u64 origin; /* offsets print relative to the start of the ADT reg */
    const char *what;
} gpu_ranges[GPU_MAX_RANGES];
static int gpu_nranges;
static u64 gpu_sgx; /* sgx reg 0: offsets print relative to it */

/*
 * Distinct host-port queue configurations (a tag written to sgx +0x4090, then a payload naming an
 * engine at +0x4098), in the order first seen: kept apart from the ring, which wraps long before a
 * park.
 */
#ifndef GPU_CONFIG_MAX
#define GPU_CONFIG_MAX 768
#endif
struct gpu_config {
    u64 t, tag, payload;
};
struct gpu_config sptm_virt_gpu_configs[GPU_CONFIG_MAX];
volatile u32 sptm_virt_gpu_nconfigs;
static u64 gpu_port_tag;
static u64 gpu_ring_cfg_t[128]; /* CNTPCT of each ring's latest configuration */
static u64 gpu_last_app_t;

static u64 gpu_seen[GPU_SEEN_MAX];
static u32 gpu_nseen;
static u64 gpu_first_t, gpu_stop_t;
static volatile u32 gpu_printed;
static volatile bool gpu_parked;
static u64 gpu_boot_stop_t;

static const char *gpu_range_name(u64 addr, u64 *off)
{
    for (int i = 0; i < gpu_nranges; i++)
        if (addr >= gpu_ranges[i].base && addr < gpu_ranges[i].base + gpu_ranges[i].size) {
            *off = addr - gpu_ranges[i].origin;
            return gpu_ranges[i].what;
        }
    *off = addr;
    return "?";
}

/* The first read of an address (a small open table, full = never first again). */
static bool gpu_first_read(u64 addr)
{
    u32 h = (u32)((addr >> 2) * 2654435761u) & (GPU_SEEN_MAX - 1);
    for (u32 i = 0; i < GPU_SEEN_MAX; i++) {
        u64 *s = &gpu_seen[(h + i) & (GPU_SEEN_MAX - 1)];
        if (*s == addr)
            return false;
        if (!*s) {
            if (gpu_nseen >= GPU_SEEN_MAX * 3 / 4)
                return false;
            *s = addr;
            gpu_nseen++;
            return true;
        }
    }
    return false;
}

static void gpu_note(struct exc_info *ctx, u64 addr, u64 val, bool write, int width)
{
    if (!sptm_virt_gpu_ring)
        return;
    u64 i = __atomic_fetch_add(&sptm_virt_gpu_head, 1, __ATOMIC_RELAXED);
    u64 keep = sptm_virt_gpu_keep;
    u64 slot = i < keep ? i : keep + (i - keep) % (sptm_virt_gpu_nrec - keep);
    struct gpu_rec *r = &sptm_virt_gpu_ring[slot];
    r->t = mrs(CNTPCT_EL0);
    r->meta = (addr & GENMASK(47, 0)) | ((u64)(width & 7) << 48) | ((u64) !!write << 51) |
              ((u64)(smp_id() & 15) << 52);
    r->val = val;
    r->pc = ctx ? ctx->elr : 0;
}

static void gpu_note_config(u64 addr, u64 val, u64 now)
{
    if (addr == gpu_sgx + 0x4090) {
        gpu_port_tag = val;
        return;
    }
    u64 engine = val >> 60;
    if (addr != gpu_sgx + 0x4098 || engine < 1 || engine > 4)
        return;
    gpu_ring_cfg_t[(gpu_port_tag >> 44) & 0x7f] = now;
    u32 n = sptm_virt_gpu_nconfigs;
    for (u32 k = 0; k < n; k++)
        if (sptm_virt_gpu_configs[k].tag == gpu_port_tag && sptm_virt_gpu_configs[k].payload == val)
            return;
    if (n >= GPU_CONFIG_MAX)
        return;
    sptm_virt_gpu_configs[n].t = now;
    sptm_virt_gpu_configs[n].tag = gpu_port_tag;
    sptm_virt_gpu_configs[n].payload = val;
    sptm_virt_gpu_nconfigs = n + 1;
}

/*
 * A kick publication (sgx +0x4090 = (ring << 44) | ... | 0x18, then +0x4098 = 1) on an app's
 * ring: true at the one the kick park waits for.
 */
static bool gpu_note_kick(u64 addr, u64 val, u64 now)
{
    if (addr != gpu_sgx + 0x4098 || val != 1 || (gpu_port_tag & 0xff) != 0x18 ||
        !sptm_virt_gpu_park_kicks || !sptm_virt_gpu_park_arm_secs)
        return false;
    u32 ring = (gpu_port_tag >> 44) & 0x7f;
    u64 frq = mrs(CNTFRQ_EL0);
    if (gpu_ring_cfg_t[ring] < gpu_first_t + (u64)sptm_virt_gpu_park_arm_secs * frq)
        return false;
    sptm_virt_gpu_park_ring = ring;
    u32 total = __atomic_add_fetch(&sptm_virt_gpu_app_kicks, 1, __ATOMIC_RELAXED);
    if (gpu_last_app_t && now - gpu_last_app_t > (u64)sptm_virt_gpu_park_gap_ms * (frq / 1000)) {
        if (sptm_virt_gpu_burst_kicks >= sptm_virt_gpu_park_big)
            sptm_virt_gpu_bursts_done++;
        sptm_virt_gpu_burst_kicks = 0;
    }
    sptm_virt_gpu_burst_kicks++;
    gpu_last_app_t = now;
    if (sptm_virt_gpu_park_burst)
        return sptm_virt_gpu_bursts_done + 1 == sptm_virt_gpu_park_burst &&
               sptm_virt_gpu_burst_kicks >= sptm_virt_gpu_park_kicks;
    return total >= sptm_virt_gpu_park_kicks;
}

static void gpu_park(struct exc_info *ctx, const char *why)
{
    /* First: of several cores that reach here at once, one owns the console and the proxy. */
    sptm_virt_park_others();
    gpu_parked = true;
    if (sptm_virt_gpu_park_kicks)
        printf("SPTM-virt[gpu]: app kick %u, on ring %u; big bursts done %u, kick %u of this one\n",
               sptm_virt_gpu_app_kicks, sptm_virt_gpu_park_ring, sptm_virt_gpu_bursts_done,
               sptm_virt_gpu_burst_kicks);
    printf("SPTM-virt[gpu]: %s: parking cpu%d in the proxy at XNU pc "
           "0x%lx; ring 0x%lx, %lu records of %lu written (head 0x%lx at 0x%lx); cores held "
           "0x%lx\n",
           why, smp_id(), ctx->elr, (u64)sptm_virt_gpu_ring,
           sptm_virt_gpu_head < sptm_virt_gpu_nrec ? sptm_virt_gpu_head : sptm_virt_gpu_nrec,
           sptm_virt_gpu_head, sptm_virt_gpu_head, (u64)&sptm_virt_gpu_head, sptm_virt_parked_mask);
    iodev_console_flush();
    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = 0x47505521, /* 'GPU!' */
        .info = ctx,
    };
    uartproxy_run(&start);
    while (1)
        sysop("wfe");
}

/*
 * A deadline the host can rely on. The park lives in the GPU hook, so a guest that brings the
 * GPU up and then leaves it alone -- which is every boot that does not reach WindowServer --
 * would never hand the ring back. Any other trapped access can offer the check instead; the
 * reset guard's watchdog writes arrive regularly.
 */
/* Park on demand, for a caller that has found something worth stopping the world for. */
void sptm_virt_gpu_park_now(struct exc_info *ctx, const char *why)
{
    if (gpu_parked)
        return;
    gpu_park(ctx, why);
}

/* Cheap enough to ask on every trap, before building a context to park with. */
bool sptm_virt_gpu_deadline_due(void)
{
    return !gpu_parked && gpu_boot_stop_t && mrs(CNTPCT_EL0) >= gpu_boot_stop_t;
}

void sptm_virt_gpu_deadline(struct exc_info *ctx)
{
    if (gpu_parked || !gpu_boot_stop_t || mrs(CNTPCT_EL0) < gpu_boot_stop_t)
        return;
    gpu_park(ctx, "the deadline the host asked for");
}

static bool gpu_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 now = mrs(CNTPCT_EL0);
    bool kick_park = false;

    if (!gpu_first_t) {
        gpu_first_t = now;
        if (sptm_virt_gpu_stop_secs)
            gpu_stop_t = now + (u64)sptm_virt_gpu_stop_secs * mrs(CNTFRQ_EL0);
        printf("SPTM-virt[gpu]: first GPU access, %s 0x%lx pc 0x%lx\n", write ? "wr" : "rd", addr,
               ctx->elr);
    }

    if (width == 4) {
        /* A trapped LDP/STP pair (the ASC mailboxes take their messages so). */
        if (write)
            __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
        else
            __asm__ volatile("ldp %0, %1, [%2]"
                             : "=r"(val[0]), "=r"(val[1])
                             : "r"(addr)
                             : "memory");
        gpu_note(ctx, addr, val[0], write, 3);
        gpu_note(ctx, addr + 8, val[1], write, 3);
    } else if (width > 4) {
        printf("SPTM-virt[gpu]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n", 1u << width,
               addr, ctx->elr);
        return false;
    } else {
        sptm_virt_mmio_rw(addr, val, write, width);
        gpu_note(ctx, addr, *val, write, width);
        if (write && gpu_sgx) {
            gpu_note_config(addr, *val, now);
            kick_park = gpu_note_kick(addr, *val, now);
        }
    }

    u64 off;
    const char *what = gpu_range_name(addr, &off);
    bool ksm = gpu_sgx && addr >= gpu_sgx + 0x21000 && addr < gpu_sgx + 0x22000;
    if ((write || ksm || gpu_first_read(addr)) && gpu_printed < sptm_virt_gpu_print_max) {
        gpu_printed++;
        printf("GPU %s c%d %s+0x%lx %s%u 0x%lx pc 0x%lx t %lu\n", write ? "W" : "R", smp_id(), what,
               off, width == 4 ? "x" : "", width == 4 ? 128 : 8u << width,
               width == 4 ? val[0] : *val, ctx->elr, now - gpu_first_t);
        if (width == 4)
            printf("GPU %s c%d %s+0x%lx hi 0x%lx\n", write ? "W" : "R", smp_id(), what, off + 8,
                   val[1]);
        if (gpu_printed == sptm_virt_gpu_print_max)
            printf("SPTM-virt[gpu]: print budget used up; the ring keeps recording\n");
    }

    if (gpu_parked)
        return true;
    if (gpu_boot_stop_t && now >= gpu_boot_stop_t)
        gpu_park(ctx, "the deadline the host asked for");
    /* A message the host names is a repeatable park point; the seconds are not (the guest's own
     * timing varies from boot to boot by more than the whole bring-up lasts). */
    if (sptm_virt_gpu_stop_msg && write && (width == 4 ? val[0] : *val) == sptm_virt_gpu_stop_msg)
        gpu_park(ctx, "the write the host asked for");
    else if (gpu_stop_t && now >= gpu_stop_t)
        gpu_park(ctx, "the time the host asked for");
    else if (kick_park)
        gpu_park(ctx, "the app kick the host asked for");
    return true;
}

static void gpu_hook_range(const char *what, u64 origin, u64 base, u64 size)
{
    if (gpu_nranges >= GPU_MAX_RANGES || !size)
        return;
    base &= ~0x3fffUL;
    size = ALIGN_UP(size, 0x4000);
    gpu_ranges[gpu_nranges].base = base;
    gpu_ranges[gpu_nranges].origin = origin & ~0x3fffUL;
    gpu_ranges[gpu_nranges].size = size;
    gpu_ranges[gpu_nranges].what = what;
    gpu_nranges++;
    if (hv_map_hook(base, gpu_hook, size) < 0)
        printf("SPTM-virt[gpu]: cannot hook %s 0x%lx+0x%lx\n", what, base, size);
    else
        printf("SPTM-virt[gpu]: hooked %s 0x%lx+0x%lx\n", what, base, size);
}

static void gpu_hook_adt(const char *path, int idx, const char *what, u64 off, u64 size)
{
    int p[8];
    u64 pa, sz;

    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", idx, &pa, &sz) < 0) {
        printf("SPTM-virt[gpu]: no %s reg[%d]; %s not traced\n", path, idx, what);
        return;
    }
    if (!strcmp(what, "sgx"))
        gpu_sgx = pa;
    gpu_hook_range(what, pa, pa + off, size ? size : sz);
}

void sptm_virt_gpu_setup(void)
{
    /* The deadline park is offered from every GL trap (sptm_virt_gl_sync), GPU trace or not. */
    if (sptm_virt_gpu_stop_boot_secs) {
        gpu_boot_stop_t = mrs(CNTPCT_EL0) + (u64)sptm_virt_gpu_stop_boot_secs * mrs(CNTFRQ_EL0);
        printf("SPTM-virt[park]: the guest parks in the proxy at its first GL trap after %u s\n",
               sptm_virt_gpu_stop_boot_secs);
    }
    if (!(sptm_virt_flags & SPTM_VIRT_GPU))
        return;
    u64 bytes = (u64)sptm_virt_gpu_ring_mib << 20;
    sptm_virt_gpu_ring = bytes ? memalign(0x4000, bytes) : NULL;
    if (!sptm_virt_gpu_ring) {
        bytes = GPU_RING_BYTES_MIN;
        sptm_virt_gpu_ring = memalign(0x4000, bytes);
    }
    if (!sptm_virt_gpu_ring) {
        printf("SPTM-virt[gpu]: no memory for the ring; GPU trace off\n");
        return;
    }
    memset(sptm_virt_gpu_ring, 0, bytes);
    sptm_virt_bss_canary = 0xa5a5c0ffee5a5a5aUL; /* read back while the guest runs */
    dc_cvac_range((void *)&sptm_virt_bss_canary, sizeof(sptm_virt_bss_canary));
    sptm_virt_gpu_nrec = bytes / sizeof(struct gpu_rec);
    if (sptm_virt_gpu_keep >= sptm_virt_gpu_nrec)
        sptm_virt_gpu_keep = 0;
    sptm_virt_gpu_head = 0;

#ifdef SPTM_VIRT_GPU_HOP_PAGES
    /*
     * T6050: only the pages the GPU driver uses once the GPU runs: the host port, two status
     * pages, the core gating control, the GPU's PMGR and both ASC mailboxes. Hooking the whole
     * window also traps SPTM's own setup of the DARTs in it, and XNU can then take an LLC decode
     * error at boot.
     */
    static const u64 sgx_pages[] = {0x4000, 0x20000, 0xd04000, 0xd10000, 0xe80000};
    for (unsigned int i = 0; i < ARRAY_SIZE(sgx_pages); i++)
        gpu_hook_adt("/arm-io/sgx", 0, "sgx", sgx_pages[i], 0x4000);
    gpu_hook_adt("/arm-io/gfx-asc", 0, "gfx-asc", 0x8000, 0x4000);
    gpu_hook_adt("/arm-io/gfx1-asc", 0, "gfx1-asc", 0x8000, 0x4000);
#else
    /* sgx reg 0 holds the GPU's PMGR (+0xe80000), its start control and both ASCs. */
    gpu_hook_adt("/arm-io/sgx", 0, "sgx", 0, 0);
    /* Both GPU coprocessors' ASC windows: the mailboxes the driver and firmware talk over. */
    gpu_hook_adt("/arm-io/gfx-asc", 0, "gfx-asc", 0, 0);
    gpu_hook_adt("/arm-io/gfx1-asc", 0, "gfx1-asc", 0, 0);
#endif
    /* The PMP dashboard (PTD records, update words) and the PMP's mailbox. */
    if (sptm_virt_gpu_pmp) {
        gpu_hook_adt("/arm-io/mcc", 8, "pmp-ptd", 0, 0);
        gpu_hook_adt("/arm-io/mcc", 9, "pmp-ptd-upd", 0, 0);
        gpu_hook_adt("/arm-io/pmp", 0, "pmp-asc", 0, 0);
    }
    printf("SPTM-virt[gpu]: ring 0x%lx (%lu records, the first %lu kept, head at 0x%lx), stop "
           "after %u s or a write of 0x%lx, print budget %u\n",
           (u64)sptm_virt_gpu_ring, sptm_virt_gpu_nrec, sptm_virt_gpu_keep,
           (u64)&sptm_virt_gpu_head, sptm_virt_gpu_stop_secs, sptm_virt_gpu_stop_msg,
           sptm_virt_gpu_print_max);
}
