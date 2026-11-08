/* SPDX-License-Identifier: MIT */

/*
 * GPU bring-up trace under the virtualized SPTM (SPTM_VIRT_GPU): pass-through stage-2 hooks on
 * the AGX register window (/arm-io/sgx reg 0, which holds the GPU's PMGR, its start control and
 * both ASC coprocessors) and on the PMP's dashboard and mailbox. Every access XNU makes there is
 * performed for real and recorded in a ring in aura's carve-out; writes, the first read of each
 * register and everything in the KSM block are also printed. After a host-set delay from the
 * first GPU access the core that next touches the GPU parks in aura's proxy, so the host can read
 * the ring (sptm_virt_gpu_ring, _head, _nrec).
 */

#include "sptm_virt.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "iodev.h"
#include "malloc.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"

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
u32 sptm_virt_gpu_stop_secs; /* park this long after the first GPU access; 0 = never */
u32 sptm_virt_gpu_print_max = 20000;

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
static volatile bool gpu_parked;

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

static void gpu_park(struct exc_info *ctx)
{
    gpu_parked = true;
    printf("SPTM-virt[gpu]: %u s after the first GPU access: parking cpu%d in the proxy at XNU pc "
           "0x%lx; ring 0x%lx, %lu records of %lu written (head 0x%lx at 0x%lx)\n",
           sptm_virt_gpu_stop_secs, smp_id(), ctx->elr, (u64)sptm_virt_gpu_ring,
           sptm_virt_gpu_head < sptm_virt_gpu_nrec ? sptm_virt_gpu_head : sptm_virt_gpu_nrec,
           sptm_virt_gpu_head, sptm_virt_gpu_head, (u64)&sptm_virt_gpu_head);
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

    if (gpu_stop_t && now >= gpu_stop_t && !gpu_parked)
        gpu_park(ctx);
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
    gpu_hook_range(what, pa + off, size ? size : sz);
}

void sptm_virt_gpu_setup(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_GPU))
        return;
    sptm_virt_gpu_ring = memalign(0x4000, GPU_RING_BYTES);
    if (!sptm_virt_gpu_ring) {
        printf("SPTM-virt[gpu]: no memory for the ring; GPU trace off\n");
        return;
    }
    memset(sptm_virt_gpu_ring, 0, GPU_RING_BYTES);
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
    printf("SPTM-virt[gpu]: ring 0x%lx (%lu records, head at 0x%lx), stop after %u s, print budget "
           "%u\n",
           (u64)sptm_virt_gpu_ring, sptm_virt_gpu_nrec, (u64)&sptm_virt_gpu_head,
           sptm_virt_gpu_stop_secs, sptm_virt_gpu_print_max);
}
