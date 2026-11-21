/* SPDX-License-Identifier: MIT */

/*
 * Record-only traces of the guest's own MMIO accesses and IRQs under the virtualized SPTM
 * (src/sptm_virt_trace.h has the layout).
 *
 * Blocks (AURA_TRACE_BLKS): the host names up to HVTRACE_RNGS page ranges in up to HVTRACE_BLKS
 * named blocks, HVTRACE_PAGES pages in all. Each page traps in stage 2. Every guest access to
 * one, XNU's and SPTM's (its GL1 runtime calls), is performed exactly once as the guest asked (the
 * width the HV decoded, the guest's value; a read returns what the device returned), then becomes
 * a record: the time once performed, the core, PA, width, direction, value and guest PC. Nothing
 * is emulated, dropped or changed, and nothing the guest didn't access is ever read.
 *
 * IRQs (AURA_IRQ_TRACE): the 16 KiB page that holds the AIC's event register (aic-iack-offset)
 * traps. A read of the event register acknowledges the IRQ it returns, so the hook reads it once
 * and the guest gets that value; a nonzero one (an IRQ the guest takes) becomes a record, a zero
 * one is counted. Every other access to the page is performed once and recorded apart. FIQs (the
 * timers, the fast IPIs) don't come through the AIC's event register, and aren't seen.
 *
 * Each trace has its own ring, so a block the guest polls can't starve the IRQs. A ring never
 * overwrites: a record's slots are claimed with a compare-and-swap on head that refuses while cap
 * records are undrained, and the access is then counted lost (in all and for its block). A
 * record's time is written last, so a slot with a time is complete. At a park the host reads
 * [tail, head) up to the first slot still being written and calls sptm_virt_trace_drain, which
 * zeroes what it read before it moves the tail past it (proxyclient/aura/hvtrace.py). Nothing is
 * printed per access, unless the host asks for the first n of each block.
 *
 * The pages are checked before any is hooked, and one that fails refuses the whole list: whole
 * pages, not RAM, inside an /arm-io device's reg, no other hook's, and nothing of the debug link's
 * USB-C port (its controller, DART, PHY and the rest of its ATC, the bus of its port controller;
 * with the link unknown, nothing of any USB-C port).
 */

#ifdef HVTRACE_HOST
#include "trace_host.h"
#else
#include "adt.h"
#include "aic.h"
#include "exception.h"
#include "gxf.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "sptm_virt_gfxrec.h"
#include "sptm_virt_priv.h"
#include "sptm_virt_trace.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define trace_now() mrs(CNTPCT_EL0)
#define trace_cpu() smp_id()
#define trace_gl()  in_gl12()

/* A 128-bit access (an LDP/STP of two X registers, or a Q register's), as one LDP/STP. */
static void trace_pair(u64 addr, u64 *val, bool write)
{
    if (write)
        __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
    else
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(val[0]), "=r"(val[1]) : "r"(addr) : "memory");
}
#endif

_Static_assert(sizeof(struct hvtrace_hdr) == 1024, "hvtrace_hdr");
_Static_assert(sizeof(struct hvtrace_rec) == 32, "hvtrace_rec");
_Static_assert(sizeof(struct hvtrace_cfg) == 408, "hvtrace_cfg");

struct hvtrace_cfg sptm_virt_trace_cfg;
struct hvtrace_hdr *sptm_virt_trace_blk, *sptm_virt_trace_irq;

static u32 trace_printed[HVTRACE_BLKS];

static struct hvtrace_rec *trace_recs(struct hvtrace_hdr *h)
{
    return (struct hvtrace_rec *)(h + 1);
}

const char *sptm_virt_trace_check(const struct hvtrace_cfg *c)
{
    u32 pages = 0, used = 0;

    if (!c->n_rng || c->n_rng > HVTRACE_RNGS || !c->n_blk || c->n_blk > HVTRACE_BLKS)
        return "1-16 ranges in 1-8 blocks";
    for (u32 i = 0; i < c->n_rng; i++) {
        const struct hvtrace_rng *r = &c->rng[i];
        if (!r->len || ((r->pa | r->len) & 0x3fff) || r->pa >> 48 || r->blk >= c->n_blk)
            return "a range that isn't whole 16 KiB pages below 2^48 in a named block";
        for (u32 k = 0; k < i; k++)
            if (r->pa < c->rng[k].pa + c->rng[k].len && c->rng[k].pa < r->pa + r->len)
                return "two ranges overlap";
        pages += r->len >> 14;
        used |= 1U << r->blk;
    }
    if (pages > HVTRACE_PAGES)
        return "more than 32 pages";
    if (used != (1U << c->n_blk) - 1)
        return "a block with no range";
    return NULL;
}

void sptm_virt_trace_ring_init(struct hvtrace_hdr *h, u64 bytes, u32 kind, u64 cntfrq, u64 t)
{
    memset(h, 0, bytes);
    h->magic = HVTRACE_MAGIC;
    h->version = HVTRACE_VERSION;
    h->hdr_bytes = sizeof(*h);
    h->rec_bytes = sizeof(struct hvtrace_rec);
    h->kind = kind;
    h->cap = (bytes - sizeof(*h)) / sizeof(struct hvtrace_rec);
    h->cntfrq = cntfrq;
    h->t_setup = t;
}

/* n consecutive slots, unless that would leave more than cap records undrained. */
static bool trace_claim(struct hvtrace_hdr *h, u32 n, u64 *first)
{
    u64 head = __atomic_load_n(&h->head, __ATOMIC_RELAXED);

    do {
        if (head + n - __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE) > h->cap)
            return false;
    } while (!__atomic_compare_exchange_n(&h->head, &head, head + n, true, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
    *first = head;
    return true;
}

void sptm_virt_trace_note(struct hvtrace_hdr *h, u64 addr, const u64 *val, bool write, int width,
                          u32 blk, u64 pc, bool gl, u32 cpu, u64 t)
{
    u32 n = width == 4 ? 2 : 1;
    u64 i;

    blk &= 15;
    if (blk < HVTRACE_BLKS)
        __atomic_fetch_add(&h->blk_seen[blk], 1, __ATOMIC_RELAXED);
    if (!trace_claim(h, n, &i)) {
        __atomic_fetch_add(&h->lost, 1, __ATOMIC_RELAXED);
        if (blk < HVTRACE_BLKS)
            __atomic_fetch_add(&h->blk_lost[blk], 1, __ATOMIC_RELAXED);
        if (!__atomic_exchange_n(&h->full, 1, __ATOMIC_RELAXED))
            printf("SPTM-virt[trace]: the %s ring is FULL (%lu records undrained): accesses are "
                   "counted, not recorded, until the host drains it\n",
                   h->kind == HVTRACE_KIND_IRQ ? "IRQ" : "block", h->cap);
        return;
    }
    u64 meta = (addr & 0xffffffffffffUL) | (u64)(width & 7) << 48 | (u64) !!write << 51 |
               (u64)(cpu & 31) << 52 | (u64)blk << 57 | (u64) !!gl << 61;
    for (u32 k = 0; k < n; k++) {
        struct hvtrace_rec *r = &trace_recs(h)[(i + k) % h->cap];
        r->val = val[k];
        r->pc = pc;
        r->meta = (meta + 8 * k) | (u64)k << 62;
        __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);
    }
}

__attribute__((used, retain)) int sptm_virt_trace_drain(struct hvtrace_hdr *h, u64 upto)
{
    if (!h || (h != sptm_virt_trace_blk && h != sptm_virt_trace_irq))
        return -1;
    u64 tail = h->tail;
    if (upto < tail || upto > __atomic_load_n(&h->head, __ATOMIC_ACQUIRE))
        return -2;
    for (u64 j = tail; j < upto; j++)
        if (!__atomic_load_n(&trace_recs(h)[j % h->cap].t, __ATOMIC_ACQUIRE))
            return -3;
    for (u64 j = tail; j < upto; j++)
        memset(&trace_recs(h)[j % h->cap], 0, sizeof(struct hvtrace_rec));
    __atomic_store_n(&h->tail, upto, __ATOMIC_RELEASE);
    h->drains++;
    __atomic_store_n(&h->full, 0, __ATOMIC_RELAXED);
    return 0;
}

/* The guest's access, performed once as it asked: the hooks' only access to the device. */
static bool trace_perform(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    if (width == 4) {
        trace_pair(addr, val, write);
        return true;
    }
    if (width > 4) {
        printf("SPTM-virt[trace]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n", 1u << width,
               addr, ctx->elr);
        return false;
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    return true;
}

bool sptm_virt_trace_blk_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    struct hvtrace_hdr *h = sptm_virt_trace_blk;
    u32 b = 15;

    if (!trace_perform(ctx, addr, val, write, width))
        return false;
    u64 t = trace_now();
    for (u32 i = 0; i < h->n_rng; i++)
        if (addr - h->rng[i].pa < h->rng[i].len)
            b = h->rng[i].blk;
    sptm_virt_trace_note(h, addr, val, write, width, b, ctx->elr, trace_gl(), trace_cpu(), t);
    if (b < HVTRACE_BLKS && __atomic_load_n(&trace_printed[b], __ATOMIC_RELAXED) < h->print_max &&
        __atomic_fetch_add(&trace_printed[b], 1, __ATOMIC_RELAXED) < h->print_max)
        printf("TRACE %s c%u %s%u +0x%lx = 0x%lx pc 0x%lx\n", h->name[b], trace_cpu(),
               write ? "W" : "R", 8u << width, addr - h->blk_base[b], val[0], ctx->elr);
    return true;
}

bool sptm_virt_trace_irq_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    struct hvtrace_hdr *h = sptm_virt_trace_irq;

    if (!trace_perform(ctx, addr, val, write, width))
        return false;
    u64 t = trace_now();
    bool event = !write && addr == h->irq_event;
    if (event && !(u32)val[0]) {
        __atomic_fetch_add(&h->irq_zero, 1, __ATOMIC_RELAXED);
        return true;
    }
    sptm_virt_trace_note(h, addr, val, write, width, event ? HVTRACE_IRQ_EVENT : HVTRACE_IRQ_OTHER,
                         ctx->elr, trace_gl(), trace_cpu(), t);
    return true;
}

#ifndef HVTRACE_HOST

extern u32 sptm_virt_link_phys[2]; /* sptm_virt_s2r.c: the debug link's PHYs, from the host */

#define TRACE_DEFAULT_BLK (1U << 19) /* 16 MiB */
#define TRACE_DEFAULT_IRQ (1U << 18) /* 8 MiB */
#define TRACE_MAX_RECS    (1U << 21)

static struct hvtrace_hdr *trace_alloc(u32 want, u32 dflt, u32 kind, const char *what)
{
    u32 n = want ? min(max(want, 2U), TRACE_MAX_RECS) : dflt;
    u64 bytes = ALIGN_UP(sizeof(struct hvtrace_hdr) + (u64)n * sizeof(struct hvtrace_rec), 0x4000);
    struct hvtrace_hdr *h = memalign(0x4000, bytes);

    if (!h)
        printf("SPTM-virt[trace]: no memory for %u records: no %s trace\n", n, what);
    else
        sptm_virt_trace_ring_init(h, bytes, kind, mrs(CNTFRQ_EL0), mrs(CNTPCT_EL0));
    return h;
}

/* A page no other hook has: its first leaf in the identity map is still a valid one (every hook,
 * a sub-page one too, leaves it invalid). */
static bool trace_page_free(u64 page)
{
    return hv_pt_walk(page) & PTE_VALID;
}

static void trace_blk_setup(void)
{
    struct hvtrace_cfg *c = &sptm_virt_trace_cfg;
    const char *why = sptm_virt_trace_check(c), *dev = "";
    u64 ram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32)), ram_end = ram + mem_size_actual;
    bool link = sptm_virt_trace_adt_link(sptm_virt_link_phys, ARRAY_SIZE(sptm_virt_link_phys));
    char node[48];
    u32 i;

    for (i = 0; !why && i < c->n_rng; i++) {
        const struct hvtrace_rng *r = &c->rng[i];
        sptm_virt_gfxrec_adt_cover(r->pa, r->len, node, sizeof(node));
        if (r->pa < ram_end && ram < r->pa + r->len)
            why = "it is RAM";
        else if (!node[0])
            why = "no /arm-io device's reg covers it";
        else if ((dev = sptm_virt_trace_adt_refused(r->pa, r->len)))
            why = link ? "it is the debug link's: "
                       : "the debug link is unknown, and it is a USB-C port's: ";
        for (u64 pg = r->pa; !why && pg < r->pa + r->len; pg += 0x4000)
            if (!trace_page_free(pg))
                why = "another of aura's hooks has a page of it";
        if (!dev)
            dev = "";
    }
    if (why) {
        printf("SPTM-virt[trace]: refused: %s%s (range %u, 0x%lx+0x%x); no block hooked\n", why,
               dev, i ? i - 1 : 0, c->rng[i ? i - 1 : 0].pa, c->rng[i ? i - 1 : 0].len);
        return;
    }
    struct hvtrace_hdr *h = trace_alloc(c->blk_max, TRACE_DEFAULT_BLK, HVTRACE_KIND_BLK, "block");
    if (!h)
        return;
    h->n_blk = c->n_blk;
    h->n_rng = c->n_rng;
    h->print_max = c->print_max;
    memcpy(h->rng, c->rng, sizeof(h->rng));
    memcpy(h->name, c->name, sizeof(h->name));
    for (i = 0; i < HVTRACE_BLKS; i++) {
        h->name[i][15] = 0;
        h->blk_base[i] = ~0UL;
    }
    for (i = 0; i < c->n_rng; i++)
        h->blk_base[c->rng[i].blk] = min(h->blk_base[c->rng[i].blk], c->rng[i].pa);
    sptm_virt_trace_blk = h;
    for (i = 0; i < c->n_rng; i++) {
        const struct hvtrace_rng *r = &c->rng[i];
        sptm_virt_gfxrec_adt_cover(r->pa, r->len, node, sizeof(node));
        bool ok = hv_map_hook(r->pa, sptm_virt_trace_blk_hook, r->len) >= 0;
        printf("SPTM-virt[trace]: %s +0x%lx: 0x%lx+0x%x (%s) %s\n", h->name[r->blk],
               r->pa - h->blk_base[r->blk], r->pa, r->len, node, ok ? "hooked" : "NOT HOOKED");
    }
    printf("SPTM-virt[trace]: every access to these pages, performed as the guest asks, into %lu "
           "records at 0x%lx (sptm_virt_trace_blk); %u printed per block\n",
           h->cap, (u64)h, h->print_max);
}

static void trace_irq_setup(void)
{
    struct hvtrace_cfg *c = &sptm_virt_trace_cfg;

    if (!aic || !aic->regs.event) {
        printf("SPTM-virt[trace]: refused: no AIC event register; no IRQ trace\n");
        return;
    }
    u64 ev = aic->base + aic->regs.event, page = ev & ~0x3fffUL;
    if (!trace_page_free(page)) {
        printf("SPTM-virt[trace]: refused: another of aura's hooks has the AIC event page 0x%lx; "
               "no IRQ trace\n",
               page);
        return;
    }
    struct hvtrace_hdr *h = trace_alloc(c->irq_max, TRACE_DEFAULT_IRQ, HVTRACE_KIND_IRQ, "IRQ");
    if (!h)
        return;
    h->irq_event = ev;
    h->irq_max = aic->max_irq;
    h->n_blk = 2;
    h->n_rng = 1;
    h->rng[0].pa = page;
    h->rng[0].len = 0x4000;
    h->rng[0].blk = HVTRACE_IRQ_OTHER;
    strcpy(h->name[HVTRACE_IRQ_EVENT], "aic-event");
    strcpy(h->name[HVTRACE_IRQ_OTHER], "aic-page");
    h->blk_base[HVTRACE_IRQ_EVENT] = ev;
    h->blk_base[HVTRACE_IRQ_OTHER] = page;
    sptm_virt_trace_irq = h;
    bool ok = hv_map_hook(page, sptm_virt_trace_irq_hook, 0x4000) >= 0;
    printf("SPTM-virt[trace]: IRQs: the AIC event register 0x%lx, its page %s: every IRQ the guest "
           "takes through it into %lu records at 0x%lx (sptm_virt_trace_irq); FIQs aren't seen\n",
           ev, ok ? "hooked" : "NOT HOOKED", h->cap, (u64)h);
}

void sptm_virt_trace_setup(void)
{
    if (sptm_virt_trace_cfg.irq)
        trace_irq_setup();
    if (sptm_virt_trace_cfg.n_rng)
        trace_blk_setup();
}

#endif
