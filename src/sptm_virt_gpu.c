/* SPDX-License-Identifier: MIT */

/*
 * GPU bring-up trace under the virtualized SPTM (SPTM_VIRT_GPU): pass-through stage-2 hooks on
 * the AGX register window (/arm-io/sgx reg 0, which holds the GPU's PMGR, its start control and
 * both ASC coprocessors) and on the PMP's dashboard and mailbox. Every access XNU makes there is
 * performed for real and recorded in a ring in aura's carve-out; writes, the first read of each
 * register and everything in the KSM block are also printed. At a host-set time, or at a write
 * the host names, the core parks in aura's proxy, so the host can read the ring
 * (sptm_virt_gpu_ring, _head, _nrec); the host's P_EXIT resumes the guest. Each of these parks
 * happens once. At every n-th write the host names, before it is performed, the GPU firmware's
 * memory is copied into a snapshot (sptm_virt_snap.c); from the first, every message on the GPU
 * ASCs' mailboxes goes into a log of its own.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "sptm_virt_priv.h"
#include "sptm_virt_snap.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"
#include "xnuboot.h"

#define GPU_RING_BYTES (16UL << 20)
#define GPU_SEEN_MAX   2048
#define GPU_MAX_RANGES 10

struct gpu_rec {
    u64 t;    /* CNTPCT_EL0 */
    u64 meta; /* pa [47:0] | width [50:48] | write [51] | cpu [55:52] */
    u64 val;  /* value written, or read */
    u64 pc;   /* guest PC */
};

/* For the host, after the park. */
struct gpu_rec *sptm_virt_gpu_ring;
u64 sptm_virt_gpu_nrec;
volatile u64 sptm_virt_gpu_head;

/* Set by the host before entry (run_guest_sptm.py). */
u32 sptm_virt_gpu_stop_secs;      /* park this long after the first GPU access; 0 = never */
u32 sptm_virt_gpu_stop_boot_secs; /* park this long after the guest's entry, GPU or not; 0 = never
                                   */
u64 sptm_virt_gpu_stop_msg; /* park after a write of this value (a mailbox message); 0 = never */
/* With stop_msg: compare only these bits of the written value (0 = all of them), and on a 128-bit
 * mailbox write only a message to endpoint stop_ep - 1 (the second word; 0 = any endpoint). An
 * InitData doorbell carries an address the host cannot know before the boot. */
u64 sptm_virt_gpu_stop_msg_mask;
u32 sptm_virt_gpu_stop_ep;
u32 sptm_virt_gpu_print_max = 20000;
u32 sptm_virt_gpu_msg_park = 1;  /* the first write the host names parks; 0: it only snapshots */
u32 sptm_virt_gpu_snap_every;    /* a snapshot at every n-th of those writes; 0: none */
u32 sptm_virt_gpu_snap_mib = 32; /* the snapshot buffer */
u32 sptm_virt_gpu_mbox_max;      /* the mailbox log's records, from the first; 0: no log */
/* The log also ends after the mailbox message that matches these (as stop_msg/_mask/_ep, to the
 * GPU) for the stop_after + 1-th time: the first work submission. 0: only the count ends it. */
u64 sptm_virt_gpu_mbox_stop_msg, sptm_virt_gpu_mbox_stop_mask;
u32 sptm_virt_gpu_mbox_stop_ep, sptm_virt_gpu_mbox_stop_after;

/* For the host, at a park (dump_snapshot.py). */
u8 *sptm_virt_snap_buf;
u64 sptm_virt_snap_bytes;
struct gpu_mbox_rec {
    u64 t;
    u64 msg;   /* the mailbox's first word */
    u64 hi;    /* its second: the endpoint in [7:0] */
    u32 meta;  /* recv [0], asc [1], words [3:2] (0 both, 1 first, 2 second), cpu [11:8] */
    u32 match; /* the named writes before it */
};
struct gpu_mbox_rec *sptm_virt_gpu_mbox;
volatile u32 sptm_virt_gpu_mbox_n;
u32 sptm_virt_gpu_mbox_cap;

static struct {
    u64 base, size;
    const char *what;
} gpu_ranges[GPU_MAX_RANGES];
static int gpu_nranges;
static u64 gpu_sgx; /* sgx reg 0: offsets print relative to it */

static u64 gpu_seen[GPU_SEEN_MAX];
static u32 gpu_nseen;
static u64 gpu_first_t, gpu_stop_t;
static volatile u32 gpu_printed;
static u64 gpu_boot_stop_t;
static u32 gpu_msg_parks, gpu_now_parks;
static volatile u32 gpu_matches;
static volatile bool gpu_mbox_on;
static u32 gpu_mbox_stops;
static DECLARE_SPINLOCK(gpu_snap_lock);

static const char *gpu_range_name(u64 addr, u64 *off)
{
    for (int i = 0; i < gpu_nranges; i++)
        if (addr >= gpu_ranges[i].base && addr < gpu_ranges[i].base + gpu_ranges[i].size) {
            *off = addr - gpu_ranges[i].base;
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
    struct gpu_rec *r = &sptm_virt_gpu_ring[i % sptm_virt_gpu_nrec];
    r->t = mrs(CNTPCT_EL0);
    r->meta = (addr & GENMASK(47, 0)) | ((u64)(width & 7) << 48) | ((u64) !!write << 51) |
              ((u64)(smp_id() & 15) << 52);
    r->val = val;
    r->pc = ctx ? ctx->elr : 0;
}

static void gpu_park(struct exc_info *ctx, const char *why)
{
    /* First: of several cores that reach here at once, one owns the console and the proxy. */
    sptm_virt_park_others();
    printf("SPTM-virt[gpu]: %s: parking cpu%d in the proxy at XNU pc "
           "0x%lx; ring 0x%lx, %lu records of %lu written (head 0x%lx at 0x%lx); P_EXIT resumes\n",
           why, smp_id(), ctx->elr, (u64)sptm_virt_gpu_ring,
           sptm_virt_gpu_head < sptm_virt_gpu_nrec ? sptm_virt_gpu_head : sptm_virt_gpu_nrec,
           sptm_virt_gpu_head, sptm_virt_gpu_head, (u64)&sptm_virt_gpu_head);
    sptm_virt_park_proxy(ctx, 0x47505521 /* 'GPU!' */);
}

/* A one-shot trigger at time *t: true on the one core that takes it. */
static bool gpu_claim_time(u64 *t, u64 now)
{
    u64 due = __atomic_load_n(t, __ATOMIC_RELAXED);
    return due && now >= due &&
           __atomic_compare_exchange_n(t, &due, 0, false, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED);
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
    if (!__atomic_fetch_add(&gpu_now_parks, 1, __ATOMIC_SEQ_CST))
        gpu_park(ctx, why);
}

/* Cheap enough to ask on every trap, before building a context to park with. */
bool sptm_virt_gpu_deadline_due(void)
{
    return gpu_boot_stop_t && mrs(CNTPCT_EL0) >= gpu_boot_stop_t;
}

void sptm_virt_gpu_deadline(struct exc_info *ctx)
{
    if (gpu_claim_time(&gpu_boot_stop_t, mrs(CNTPCT_EL0)))
        gpu_park(ctx, "the deadline the host asked for");
}

/* The GPU's own ASCs (gfx-asc, gfx1-asc): an endpoint's message is a mailbox write to one of them.
 */
static struct {
    u64 pa, size;
} gpu_asc[2];

static bool gpu_stop_msg_hit(u64 addr, const u64 *pair, u64 v, int width)
{
    u64 mask = sptm_virt_gpu_stop_msg_mask ? sptm_virt_gpu_stop_msg_mask : ~0UL;

    if ((v & mask) != sptm_virt_gpu_stop_msg)
        return false;
    if (!sptm_virt_gpu_stop_ep)
        return true;
    if (width != 4 || !pair || (pair[1] & 0xff) != sptm_virt_gpu_stop_ep - 1)
        return false;
    /* Not the PMP's ASC, also hooked: only a message to the GPU firmware parks. */
    for (unsigned i = 0; i < ARRAY_SIZE(gpu_asc); i++)
        if (gpu_asc[i].size && addr >= gpu_asc[i].pa && addr < gpu_asc[i].pa + gpu_asc[i].size)
            return true;
    return false;
}

/* ---- snapshots and the mailbox log ------------------------------------------------------- */

#define GPU_A2I_SEND 0x8800 /* the message to the ASC, its endpoint word at +8 */
#define GPU_I2A_RECV 0x8830 /* the ASC's message, read */

/* Guest RAM aura maps identity, normal write-back: anything else, MMIO first, is never read. */
bool sptm_virt_snap_pa_ok(u64 pa, u64 len)
{
    u64 ram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32)), end = ram + mem_size_actual;
    u64 buf = (u64)sptm_virt_snap_buf;

    if (!len || pa < ram || pa >= end || len > end - pa)
        return false;
    if (pa < buf + sptm_virt_snap_bytes && pa + len > buf)
        return false;
    u64 save = mrs(PAR_EL1);
    bool ok = true;
    for (u64 pg = pa & ~0x3fffUL; ok && pg < pa + len; pg += 0x4000) {
        __asm__ volatile("at s1e2r, %0" ::"r"(pg) : "memory");
        sysop("isb");
        u64 par = mrs(PAR_EL1);
        ok = !(par & PAR_F) && (par >> 56) == 0xff && (par & GENMASK(47, 14)) == pg;
    }
    msr(PAR_EL1, save);
    sysop("isb");
    return ok;
}

const void *sptm_virt_snap_pa(u64 pa)
{
    return (const void *)pa;
}

u64 sptm_virt_snap_ticks(void)
{
    return mrs(CNTPCT_EL0);
}

static int gpu_asc_of(u64 addr)
{
    for (unsigned i = 0; i < ARRAY_SIZE(gpu_asc); i++)
        if (gpu_asc[i].size && addr >= gpu_asc[i].pa && addr < gpu_asc[i].pa + gpu_asc[i].size)
            return i;
    return -1;
}

/* After the access: a message on a GPU ASC's mailbox, either way, into the log. */
static void gpu_mbox_note(u64 addr, const u64 *val, bool write, int width)
{
    int asc = gpu_asc_of(addr);
    if (asc < 0)
        return;
    u64 off = addr - gpu_asc[asc].pa, box = write ? GPU_A2I_SEND : GPU_I2A_RECV;
    if ((off & ~8UL) != box || width < 3 || (width == 4 && off != box))
        return;
    u32 words = width == 4 ? 0 : off == box ? 1 : 2;
    u64 msg = words == 2 ? 0 : val[0], hi = words == 0 ? val[1] : words == 2 ? val[0] : 0;
    u32 i = __atomic_fetch_add(&sptm_virt_gpu_mbox_n, 1, __ATOMIC_RELAXED);
    if (i >= sptm_virt_gpu_mbox_cap) {
        gpu_mbox_on = false;
        return;
    }
    struct gpu_mbox_rec *r = &sptm_virt_gpu_mbox[i];
    r->t = mrs(CNTPCT_EL0);
    r->msg = msg;
    r->hi = hi;
    r->meta = !write | asc << 1 | words << 2 | (smp_id() & 15) << 8;
    r->match = gpu_matches;
    if (i + 1 >= min(sptm_virt_gpu_mbox_max, sptm_virt_gpu_mbox_cap))
        gpu_mbox_on = false;
    u64 mask = sptm_virt_gpu_mbox_stop_mask ? sptm_virt_gpu_mbox_stop_mask : ~0UL;
    if (write && words == 0 && sptm_virt_gpu_mbox_stop_ep &&
        (hi & 0xff) == sptm_virt_gpu_mbox_stop_ep - 1 &&
        (msg & mask) == sptm_virt_gpu_mbox_stop_msg &&
        __atomic_fetch_add(&gpu_mbox_stops, 1, __ATOMIC_RELAXED) == sptm_virt_gpu_mbox_stop_after) {
        gpu_mbox_on = false;
        printf("SPTM-virt[mbox]: the log ends at its stop message 0x%lx (%u records)\n", msg,
               i + 1);
    }
}

/* Before the access: a write the host names. Counts it, and takes the snapshot due. */
static void gpu_kick(struct exc_info *ctx, const u64 *val, int width)
{
    u32 n = __atomic_add_fetch(&gpu_matches, 1, __ATOMIC_SEQ_CST);

    if (n == 1 && sptm_virt_gpu_mbox)
        gpu_mbox_on = true;
    if (!sptm_virt_gpu_snap_every || !sptm_virt_snap_buf || (n - 1) % sptm_virt_gpu_snap_every)
        return;
    struct snap_kick k = {
        .t = mrs(CNTPCT_EL0),
        .value = val[0],
        .value_hi = width == 4 ? val[1] : 0,
        .pc = ctx->elr,
        .ring_head = sptm_virt_gpu_head,
        .cpu = smp_id(),
        .ep = width == 4 ? val[1] & 0xff : 0xff,
        .match = n,
    };
    spin_lock(&gpu_snap_lock);
    bool took = sptm_virt_snap_take(&k);
    spin_unlock(&gpu_snap_lock);
    const struct snap_hdr *h = (const struct snap_hdr *)sptm_virt_snap_buf;
    if (!took) {
        if (h->skipped == 1)
            printf("SPTM-virt[snap]: match %u: no snapshot (budget %u, buffer %s)\n", n,
                   sptm_virt_snap_max, (h->flags & SNAP_F_FULL) ? "full" : "not full");
        return;
    }
    printf("SPTM-virt[snap]: #%u at match %u (0x%lx, pc 0x%lx), before the write; %lu of %lu "
           "bytes, flags 0x%x, in %lu us\n",
           h->nsnap - 1, n, val[0], ctx->elr, h->used, h->capacity, h->flags,
           (mrs(CNTPCT_EL0) - k.t) / (mrs(CNTFRQ_EL0) / 1000000));
    for (u32 i = 0; h->nsnap == 1 && i < ARRAY_SIZE(h->watch); i++)
        if (h->watch_ok & BIT(i))
            printf("SPTM-virt[snap]: watch %u (%s +0x%lx) = 0x%lx\n", i,
                   sptm_virt_snap_chains[sptm_virt_snap_watch[i].chain - 1].name,
                   sptm_virt_snap_watch[i].off, h->watch[i]);
}

static bool gpu_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 now = mrs(CNTPCT_EL0);

    if (!gpu_first_t) {
        gpu_first_t = now;
        if (sptm_virt_gpu_stop_secs)
            gpu_stop_t = now + (u64)sptm_virt_gpu_stop_secs * mrs(CNTFRQ_EL0);
        printf("SPTM-virt[gpu]: first GPU access, %s 0x%lx pc 0x%lx\n", write ? "wr" : "rd", addr,
               ctx->elr);
    }
    /* The value decides, so the write the host names is known before it is performed. */
    bool hit = sptm_virt_gpu_stop_msg && write &&
               gpu_stop_msg_hit(addr, width == 4 ? val : NULL, *val, width);
    if (hit)
        gpu_kick(ctx, val, width);

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
        if (gpu_mbox_on)
            gpu_mbox_note(addr, val, write, width);
    } else if (width > 4) {
        printf("SPTM-virt[gpu]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n", 1u << width,
               addr, ctx->elr);
        return false;
    } else {
        sptm_virt_mmio_rw(addr, val, write, width);
        gpu_note(ctx, addr, *val, write, width);
        if (gpu_mbox_on)
            gpu_mbox_note(addr, val, write, width);
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

    sptm_virt_gpu_deadline(ctx);
    /* A message the host names is a repeatable park point; the seconds are not (the guest's own
     * timing varies from boot to boot by more than the whole bring-up lasts). */
    if (hit && sptm_virt_gpu_msg_park && !__atomic_fetch_add(&gpu_msg_parks, 1, __ATOMIC_SEQ_CST))
        gpu_park(ctx, "the write the host asked for");
    else if (gpu_claim_time(&gpu_stop_t, now))
        gpu_park(ctx, "the time the host asked for");
    return true;
}

static void gpu_hook_range(const char *what, u64 base, u64 size)
{
    if (gpu_nranges >= GPU_MAX_RANGES || !size)
        return;
    base &= ~0x3fffUL;
    size = ALIGN_UP(size, 0x4000);
    gpu_ranges[gpu_nranges].base = base;
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
    if (!strcmp(what, "gfx-asc") || !strcmp(what, "gfx1-asc")) {
        gpu_asc[what[3] == '1'].pa = pa;
        gpu_asc[what[3] == '1'].size = sz;
    }
    gpu_hook_range(what, pa + off, size ? size : sz);
}

/* /arm-io/sgx's <name>-base and -size. */
static void gpu_snap_region(int idx, const char *name)
{
    char prop[48];
    int node = adt_path_offset(adt, "/arm-io/sgx");
    u64 v[2] = {0, 0};

    for (int i = 0; i < 2 && node >= 0; i++) {
        u32 len = 0;
        snprintf(prop, sizeof(prop), "%s-%s", name, i ? "size" : "base");
        const void *val = adt_getprop(adt, node, prop, &len);
        if (val && len >= sizeof(u64))
            memcpy(&v[i], val, sizeof(u64));
        else if (val && len == sizeof(u32))
            v[i] = *(const u32 *)val;
    }
    sptm_virt_snap_region[idx][0] = v[0];
    sptm_virt_snap_region[idx][1] = v[1];
}

static void gpu_snap_setup(void)
{
    if (sptm_virt_gpu_snap_every) {
        sptm_virt_snap_bytes = (u64)sptm_virt_gpu_snap_mib << 20;
        sptm_virt_snap_buf = memalign(0x4000, sptm_virt_snap_bytes);
        if (!sptm_virt_snap_buf) {
            printf("SPTM-virt[snap]: no memory for %u MiB; no snapshots\n", sptm_virt_gpu_snap_mib);
            sptm_virt_snap_bytes = 0;
        } else {
            sptm_virt_snap_init(sptm_virt_snap_buf, sptm_virt_snap_bytes);
            gpu_snap_region(0, "gfx-shared-region");
            gpu_snap_region(1, "gfx-shared-l2-region");
            gpu_snap_region(2, "gpu-region");
            gpu_snap_region(3, "gfx-handoff");
            u32 chains = 0;
            for (u32 i = 0; i < SNAP_CHAINS; i++)
                chains += !!sptm_virt_snap_chains[i].len;
            printf("SPTM-virt[snap]: every %u-th named write, %u in all, into %u MiB at 0x%lx "
                   "(sptm_virt_snap_buf); %u chains; TTBAT 0x%lx\n",
                   sptm_virt_gpu_snap_every, sptm_virt_snap_max, sptm_virt_gpu_snap_mib,
                   (u64)sptm_virt_snap_buf, chains, sptm_virt_snap_region[2][0]);
        }
    }
    if (sptm_virt_gpu_mbox_max) {
        sptm_virt_gpu_mbox_cap = min(sptm_virt_gpu_mbox_max, 65536U);
        sptm_virt_gpu_mbox = memalign(0x4000, sptm_virt_gpu_mbox_cap * sizeof(struct gpu_mbox_rec));
        if (!sptm_virt_gpu_mbox) {
            sptm_virt_gpu_mbox_cap = 0;
            printf("SPTM-virt[mbox]: no memory for the mailbox log\n");
        } else {
            printf("SPTM-virt[mbox]: logging up to %u GPU mailbox messages from the first named "
                   "write (sptm_virt_gpu_mbox)\n",
                   sptm_virt_gpu_mbox_cap);
        }
    }
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
    sptm_virt_gpu_ring = memalign(0x4000, GPU_RING_BYTES);
    if (!sptm_virt_gpu_ring) {
        printf("SPTM-virt[gpu]: no memory for the ring; GPU trace off\n");
        return;
    }
    memset(sptm_virt_gpu_ring, 0, GPU_RING_BYTES);
    sptm_virt_bss_canary = 0xa5a5c0ffee5a5a5aUL; /* read back while the guest runs */
    dc_cvac_range((void *)&sptm_virt_bss_canary, sizeof(sptm_virt_bss_canary));
    sptm_virt_gpu_nrec = GPU_RING_BYTES / sizeof(struct gpu_rec);
    sptm_virt_gpu_head = 0;

    /* sgx reg 0 holds the GPU's PMGR (+0xe80000), its start control and both ASCs. */
    gpu_hook_adt("/arm-io/sgx", 0, "sgx", 0, 0);
    /* Both GPU coprocessors' ASC windows: the mailboxes the driver and firmware talk over. */
    gpu_hook_adt("/arm-io/gfx-asc", 0, "gfx-asc", 0, 0);
    gpu_hook_adt("/arm-io/gfx1-asc", 0, "gfx1-asc", 0, 0);
    /* The PMP dashboard (PTD records, update words) and the PMP's mailbox. */
    gpu_hook_adt("/arm-io/mcc", 8, "pmp-ptd", 0, 0);
    gpu_hook_adt("/arm-io/mcc", 9, "pmp-ptd-upd", 0, 0);
    gpu_hook_adt("/arm-io/pmp", 0, "pmp-asc", 0, 0);
    gpu_snap_setup();
    printf("SPTM-virt[gpu]: ring 0x%lx (%lu records, head at 0x%lx), stop after %u s or a write of "
           "0x%lx (mask 0x%lx, endpoint %d), print budget %u\n",
           (u64)sptm_virt_gpu_ring, sptm_virt_gpu_nrec, (u64)&sptm_virt_gpu_head,
           sptm_virt_gpu_stop_secs, sptm_virt_gpu_stop_msg, sptm_virt_gpu_stop_msg_mask,
           (int)sptm_virt_gpu_stop_ep - 1, sptm_virt_gpu_print_max);
}
