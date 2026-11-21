/* SPDX-License-Identifier: MIT */
/*
 * src/sptm_virt_trace.c with a fake device and a fake AIC event register, its cores as threads
 * (tests/python/test_sptm_virt_trace_host.py also decodes the rings it leaves).
 *
 *   trace_host OUT
 *
 * Checks every rule itself (exit 1 and a FAIL line on the first one broken) and writes
 * OUT/blk.bin and OUT/irq.bin: each ring as the host reads it (the header, then cap records).
 */
#ifndef HVTRACE_HOST
#define HVTRACE_HOST
#endif
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#include "trace_host.h"

/* The J773g's pages the first run takes: the PMP PTD window, two of ATC2's core pages (one
 * block), the AP GPIO page; the AIC event register (aic-iack-offset 0x40000). */
#define PTD      0x3803c0000UL
#define ATC2     0x413000000UL
#define ATC2_B   0x413014000UL
#define GPIO     0x39a000000UL
#define AIC_EV   0x381040000UL
#define XNU_PC   0xfffffe0037390000UL
#define SPTM_PC  0xfffffff027100000UL
#define FRQ      24000000UL
#define DEV_SIZE 0x4000

static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            fflush(stdout);                                                                        \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

/* ---- the harness's aura ------------------------------------------------------------------ */

static __thread u32 cur_cpu;
static __thread bool cur_gl;
static u64 clock_ticks = 1000;

u64 host_now(void)
{
    return __atomic_add_fetch(&clock_ticks, 1, __ATOMIC_SEQ_CST);
}

u32 host_cpu(void)
{
    return cur_cpu;
}

bool host_gl(void)
{
    return cur_gl;
}

/* Four device pages, a fake AIC event register (a queue: a read takes the next IRQ, 0 when none
 * is pending, as the AIC acknowledges what it returns), and every access counted. */
static u8 dev[4][DEV_SIZE];
static const u64 dev_base[4] = {PTD, ATC2, ATC2_B, AIC_EV};
static u32 dev_accesses, dev_last_width;
static u64 dev_last_addr;
static u32 aic_queue[8], aic_n, aic_reads;

static u8 *dev_at(u64 addr, u32 bytes)
{
    for (int i = 0; i < 4; i++)
        if (addr >= dev_base[i] && addr + bytes <= dev_base[i] + DEV_SIZE)
            return &dev[i][addr - dev_base[i]];
    CHECK(0, "an access at 0x%lx (%u bytes) outside the device", addr, bytes);
    return NULL;
}

void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width)
{
    u32 bytes = 1u << width;
    dev_accesses++;
    dev_last_width = width;
    dev_last_addr = addr;
    if (addr == AIC_EV && !write) {
        aic_reads++;
        *val = aic_n ? aic_queue[0] : 0;
        if (aic_n)
            memmove(aic_queue, aic_queue + 1, --aic_n * sizeof(aic_queue[0]));
        return;
    }
    u8 *p = dev_at(addr, bytes);
    if (write)
        memcpy(p, val, bytes);
    else {
        *val = 0;
        memcpy(val, p, bytes);
    }
}

void trace_pair(u64 addr, u64 *val, bool write)
{
    dev_accesses++;
    dev_last_width = 4;
    dev_last_addr = addr;
    u8 *p = dev_at(addr, 16);
    if (write)
        memcpy(p, val, 16);
    else
        memcpy(val, p, 16);
}

static struct hvtrace_rec *recs(struct hvtrace_hdr *h)
{
    return (struct hvtrace_rec *)(h + 1);
}

static struct hvtrace_hdr *ring(u64 cap, u32 kind)
{
    u64 bytes = sizeof(struct hvtrace_hdr) + cap * sizeof(struct hvtrace_rec);
    struct hvtrace_hdr *h = malloc(bytes);
    memset(h, 0xa5, bytes); /* sptm_virt_trace_ring_init must clear it all */
    sptm_virt_trace_ring_init(h, bytes, kind, FRQ, 1000);
    CHECK(h->cap == cap && h->head == 0 && h->tail == 0 && h->lost == 0, "a new ring isn't empty");
    for (u64 i = 0; i < cap; i++)
        CHECK(!recs(h)[i].t, "a new ring's slot %lu isn't clear", i);
    return h;
}

/* As sptm_virt_trace_setup lays a block ring out. */
static struct hvtrace_hdr *blk_ring(u64 cap, struct hvtrace_cfg *c)
{
    struct hvtrace_hdr *h = ring(cap, HVTRACE_KIND_BLK);
    h->n_blk = c->n_blk;
    h->n_rng = c->n_rng;
    memcpy(h->rng, c->rng, sizeof(h->rng));
    memcpy(h->name, c->name, sizeof(h->name));
    for (u32 i = 0; i < HVTRACE_BLKS; i++)
        h->blk_base[i] = ~0UL;
    for (u32 i = 0; i < c->n_rng; i++)
        if (c->rng[i].pa < h->blk_base[c->rng[i].blk])
            h->blk_base[c->rng[i].blk] = c->rng[i].pa;
    sptm_virt_trace_blk = h;
    return h;
}

static struct hvtrace_cfg first_run(void)
{
    struct hvtrace_cfg c = {.n_rng = 3, .n_blk = 2};
    c.rng[0] = (struct hvtrace_rng){PTD, 0x4000, 0};
    c.rng[1] = (struct hvtrace_rng){ATC2, 0x4000, 1};
    c.rng[2] = (struct hvtrace_rng){ATC2_B, 0x4000, 1};
    strcpy(c.name[0], "ptd");
    strcpy(c.name[1], "atc2");
    return c;
}

static void save(const char *dir, const char *name, struct hvtrace_hdr *h)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    CHECK(f, "cannot write %s", path);
    fwrite(h, 1, sizeof(*h) + h->cap * sizeof(struct hvtrace_rec), f);
    fclose(f);
}

static void meta_is(const struct hvtrace_rec *r, u64 pa, int width, bool write, u32 cpu, u32 blk,
                    bool gl, bool hi)
{
    u64 m = r->meta;
    CHECK((m & 0xffffffffffffUL) == pa, "pa 0x%lx, not 0x%lx", m & 0xffffffffffffUL, pa);
    CHECK((int)(m >> 48 & 7) == width, "width %lu, not %d", m >> 48 & 7, width);
    CHECK((bool)(m >> 51 & 1) == write, "direction %lu, not %d", m >> 51 & 1, write);
    CHECK((m >> 52 & 31) == cpu, "core %lu, not %u", m >> 52 & 31, cpu);
    CHECK((m >> 57 & 15) == blk, "block %lu, not %u", m >> 57 & 15, blk);
    CHECK((bool)(m >> 61 & 1) == gl, "GL %lu, not %d", m >> 61 & 1, gl);
    CHECK((bool)(m >> 62 & 1) == hi, "hi %lu, not %d", m >> 62 & 1, hi);
}

/* ---- F9: the block list ------------------------------------------------------------------ */

static void test_check(void)
{
    struct hvtrace_cfg c = first_run(), d;
    CHECK(!sptm_virt_trace_check(&c), "the first run's list refused: %s",
          sptm_virt_trace_check(&c));
#define REFUSED(what, ...)                                                                         \
    do {                                                                                           \
        d = c;                                                                                     \
        __VA_ARGS__;                                                                               \
        CHECK(sptm_virt_trace_check(&d), "accepted: %s", what);                                    \
    } while (0)
    REFUSED("a range not page-aligned", d.rng[1].pa += 0x1000);
    REFUSED("a length not whole pages", d.rng[1].len = 0x2000);
    REFUSED("an empty range", d.rng[2].len = 0);
    REFUSED("a PA past 48 bits", d.rng[0].pa |= 1UL << 48);
    REFUSED("two ranges that overlap", d.rng[2].pa = ATC2);
    REFUSED("a range that overlaps the next one's start", d.rng[1].len = 0x18000);
    REFUSED("a range in no block", d.rng[2].blk = 2);
    REFUSED("a block with no range", d.n_blk = 3);
    REFUSED("no range", d.n_rng = 0);
    REFUSED("no block", d.n_blk = 0);
    REFUSED("17 ranges", d.n_rng = 17);
    REFUSED("9 blocks", {
        d.n_rng = d.n_blk = 9;
        for (u32 i = 0; i < 9; i++)
            d.rng[i] = (struct hvtrace_rng){ATC2 + 0x8000UL * i, 0x4000, i};
    });
    REFUSED("33 pages", d.rng[1].pa = 0x420000000UL, d.rng[1].len = 31 * 0x4000);
    d = c;
    d.rng[1].len = 30 * 0x4000;
    d.rng[2].pa = ATC2 + 30 * 0x4000;
    CHECK(!sptm_virt_trace_check(&d), "32 pages refused: %s", sptm_virt_trace_check(&d));
    d = c;
    d.n_rng = 16;
    d.n_blk = 8;
    for (u32 i = 0; i < 16; i++)
        d.rng[i] = (struct hvtrace_rng){ATC2 + 0x8000UL * i, 0x4000, i % 8};
    CHECK(!sptm_virt_trace_check(&d), "16 ranges in 8 blocks refused: %s",
          sptm_virt_trace_check(&d));
}

/* ---- F1, F3: the block hook ---------------------------------------------------------------- */

static void blk_access(struct hvtrace_hdr *h, u64 addr, u64 v0, u64 v1, bool write, int width,
                       u32 cpu, bool gl, u64 pc, u32 blk)
{
    struct exc_info ctx = {.elr = pc};
    u64 val[2] = {v0, v1}, before = h->head, seen = h->blk_seen[blk];
    u32 acc = dev_accesses;
    u8 *p = dev_at(addr, 1u << width), expect[16];
    memcpy(expect, p, 1u << width);
    cur_cpu = cpu;
    cur_gl = gl;
    CHECK(sptm_virt_trace_blk_hook(&ctx, addr, val, write, width), "the hook failed an access");
    CHECK(dev_accesses == acc + 1, "%u device accesses for one guest access", dev_accesses - acc);
    CHECK((int)dev_last_width == width && dev_last_addr == addr,
          "performed at width %u at 0x%lx, not %d at 0x%lx", dev_last_width, dev_last_addr, width,
          addr);
    u64 got[2] = {0, 0};
    if (write) {
        CHECK(!memcmp(p, (u64[]){v0, v1}, 1u << width), "the device got another value");
        CHECK(val[0] == v0 && (width < 4 || val[1] == v1), "the write's value changed");
        got[0] = v0;
        got[1] = v1;
    } else {
        memcpy(got, expect, 1u << width);
        CHECK(val[0] == got[0] && (width < 4 || val[1] == got[1]),
              "the guest read 0x%lx, the device had 0x%lx", val[0], got[0]);
    }
    u32 n = width == 4 ? 2 : 1;
    CHECK(h->head == before + n, "%lu records for one access", h->head - before);
    CHECK(h->blk_seen[blk] == seen + 1, "block %u's count", blk);
    for (u32 k = 0; k < n; k++) {
        struct hvtrace_rec *r = &recs(h)[(before + k) % h->cap];
        CHECK(r->t, "a record without its time");
        CHECK(r->val == got[k], "record value 0x%lx, not 0x%lx", r->val, got[k]);
        CHECK(r->pc == pc, "record pc 0x%lx, not 0x%lx", r->pc, pc);
        meta_is(r, addr + 8 * k, width, write, cpu, blk, gl, k == 1);
    }
}

static void test_blk_hook(const char *out)
{
    struct hvtrace_cfg c = first_run();
    struct hvtrace_hdr *h = blk_ring(64, &c);
    for (u32 i = 0; i < DEV_SIZE; i++) {
        dev[0][i] = (u8)(i * 7 + 1);
        dev[1][i] = (u8)(i * 13 + 5);
        dev[2][i] = (u8)(i * 3 + 9);
    }
    blk_access(h, PTD + 0x1d10, 0, 0, false, 2, 3, false, XNU_PC, 0);
    blk_access(h, PTD + 0x10, 0, 0, false, 3, 1, false, XNU_PC + 4, 0);
    blk_access(h, PTD + 0x1880, 0x12345678, 0, true, 2, 2, false, XNU_PC + 8, 0);
    blk_access(h, PTD + 0x1f30, 0, 0, false, 0, 0, true, SPTM_PC, 0);
    blk_access(h, PTD + 0x1c20, 0xbeef, 0, true, 1, 9, false, XNU_PC, 0);
    blk_access(h, ATC2 + 0x804, 0, 0, false, 2, 4, false, XNU_PC, 1);
    blk_access(h, ATC2_B + 0x2008, 0, 0, false, 2, 4, false, XNU_PC, 1); /* +0x16008 */
    blk_access(h, ATC2_B + 0x3090, 0x1111222233334444UL, 0x5555666677778888UL, true, 4, 5, false,
               XNU_PC + 12, 1);
    blk_access(h, ATC2_B + 0x3000, 0, 0, false, 4, 6, false, XNU_PC + 16, 1);
    blk_access(h, ATC2 + 0x260c, 0, 0, false, 2, 31, false, XNU_PC, 1);
    /* A 32-byte access isn't supported: no device access, no record, and the hook fails it. */
    struct exc_info ctx = {.elr = XNU_PC};
    u64 val[4] = {0};
    u32 acc = dev_accesses;
    u64 head = h->head;
    CHECK(!sptm_virt_trace_blk_hook(&ctx, ATC2, val, false, 5), "a 32-byte access passed");
    CHECK(dev_accesses == acc && h->head == head, "a 32-byte access was performed or recorded");
    CHECK(h->blk_base[1] == ATC2, "atc2's base");
    save(out, "blk.bin", h);
}

/* ---- F2: the IRQ hook ---------------------------------------------------------------------- */

static void test_irq_hook(const char *out)
{
    struct hvtrace_hdr *h = ring(32, HVTRACE_KIND_IRQ);
    h->irq_event = AIC_EV;
    h->irq_max = 0x400;
    h->n_blk = 2;
    h->n_rng = 1;
    h->rng[0] = (struct hvtrace_rng){AIC_EV, 0x4000, HVTRACE_IRQ_OTHER};
    strcpy(h->name[0], "aic-event");
    strcpy(h->name[1], "aic-page");
    h->blk_base[0] = AIC_EV;
    h->blk_base[1] = AIC_EV;
    sptm_virt_trace_irq = h;
    /* IRQs 892, 894 and 895 (type 1, die 0), then one on die 1 (as a two-die AIC would give). */
    u32 events[] = {0x1037c, 0x1037e, 0x1037f, 0x1010005};
    memcpy(aic_queue, events, sizeof(events));
    aic_n = 4;
    struct exc_info ctx = {.elr = XNU_PC};
    for (u32 i = 0; i < 6; i++) {
        u64 val[2] = {0xdead, 0};
        u32 reads = aic_reads, acc = dev_accesses;
        u64 head = h->head, zero = h->irq_zero;
        cur_cpu = i;
        CHECK(sptm_virt_trace_irq_hook(&ctx, AIC_EV, val, false, 2), "the IRQ hook failed");
        CHECK(aic_reads == reads + 1 && dev_accesses == acc + 1,
              "the event register read %u times for one guest read", aic_reads - reads);
        u32 want = i < 4 ? events[i] : 0;
        CHECK(val[0] == want, "the guest got 0x%lx, the AIC gave 0x%x", val[0], want);
        if (!want) {
            CHECK(h->head == head && h->irq_zero == zero + 1,
                  "a read that returned 0 was recorded or not counted");
            continue;
        }
        CHECK(h->head == head + 1, "an IRQ not recorded");
        struct hvtrace_rec *r = &recs(h)[head];
        CHECK(r->val == want && r->pc == XNU_PC, "the IRQ's record");
        meta_is(r, AIC_EV, 2, false, i, HVTRACE_IRQ_EVENT, false, false);
    }
    /* The page's other registers: performed once, recorded apart, nothing taken off the AIC. */
    dev[3][0x100] = 0x5a;
    u32 reads = aic_reads, acc = dev_accesses;
    u64 head = h->head;
    u64 val[2] = {0, 0};
    cur_cpu = 2;
    CHECK(sptm_virt_trace_irq_hook(&ctx, AIC_EV + 0x100, val, false, 2), "the IRQ hook failed");
    CHECK(val[0] == 0x5a && dev_accesses == acc + 1 && aic_reads == reads,
          "another register of the page: 0x%lx, %u accesses", val[0], dev_accesses - acc);
    val[0] = 0x77;
    CHECK(sptm_virt_trace_irq_hook(&ctx, AIC_EV + 0x104, val, true, 2), "the IRQ hook failed");
    CHECK(dev[3][0x104] == 0x77 && dev_accesses == acc + 2, "a write on the page not performed");
    /* A write to the event register's own offset is not a read: performed, recorded apart. */
    val[0] = 1;
    CHECK(sptm_virt_trace_irq_hook(&ctx, AIC_EV, val, true, 2), "the IRQ hook failed");
    CHECK(aic_reads == reads && dev_accesses == acc + 3, "a write to the event register");
    CHECK(h->head == head + 3, "the page's other accesses: %lu records", h->head - head);
    meta_is(&recs(h)[head], AIC_EV + 0x100, 2, false, 2, HVTRACE_IRQ_OTHER, false, false);
    meta_is(&recs(h)[head + 1], AIC_EV + 0x104, 2, true, 2, HVTRACE_IRQ_OTHER, false, false);
    meta_is(&recs(h)[head + 2], AIC_EV, 2, true, 2, HVTRACE_IRQ_OTHER, false, false);
    save(out, "irq.bin", h);
}

/* ---- F6, F7, F8: full, drain --------------------------------------------------------------- */

static void note(struct hvtrace_hdr *h, u64 v, int width, u32 blk)
{
    u64 val[2] = {v, v + 1};
    sptm_virt_trace_note(h, PTD + 8 * (v % 64), val, false, width, blk, XNU_PC, false, 0,
                         host_now());
}

static void test_full(void)
{
    struct hvtrace_cfg c = first_run();
    struct hvtrace_hdr *h = blk_ring(5, &c);
    for (u64 v = 0; v < 4; v++)
        note(h, v, 2, 0);
    /* A 128-bit access needs two slots, and one is left: lost whole, not half. */
    note(h, 100, 4, 1);
    CHECK(h->head == 4 && h->lost == 1 && !recs(h)[4].t, "a pair recorded at the ring's edge");
    CHECK(h->blk_lost[1] == 1 && h->blk_lost[0] == 0 && h->blk_seen[1] == 1, "the pair's block");
    note(h, 4, 2, 0);
    note(h, 5, 2, 0);
    note(h, 6, 2, 1);
    CHECK(h->head == 5 && h->lost == 3, "a full ring: head %lu, lost %lu", h->head, h->lost);
    CHECK(h->blk_lost[0] == 1 && h->blk_lost[1] == 2 && h->blk_seen[0] == 6,
          "lost per block %lu %lu", h->blk_lost[0], h->blk_lost[1]);
    CHECK(h->full, "the ring didn't say it was full");
    for (u64 i = 0; i < 5; i++)
        CHECK(recs(h)[i].t && recs(h)[i].val == i, "an undrained record overwritten: slot %lu", i);

    /* Drains. */
    CHECK(sptm_virt_trace_drain(NULL, 0) < 0, "a drain of no ring accepted");
    struct hvtrace_hdr *other = ring(8, HVTRACE_KIND_BLK);
    note(other, 1, 2, 0);
    CHECK(sptm_virt_trace_drain(other, 1) < 0 && other->tail == 0,
          "a drain of a ring that isn't aura's accepted");
    CHECK(sptm_virt_trace_drain(h, 6) < 0, "a drain past head accepted");
    u64 t3 = recs(h)[3].t;
    recs(h)[3].t = 0; /* as if a core were still writing it */
    CHECK(sptm_virt_trace_drain(h, 4) < 0, "a drain past a record being written accepted");
    CHECK(h->tail == 0 && recs(h)[0].t, "a refused drain changed the ring");
    CHECK(sptm_virt_trace_drain(h, 3) == 0, "a drain of complete records refused");
    CHECK(h->tail == 3 && h->drains == 1 && !h->full, "the drain: tail %lu", h->tail);
    for (u64 i = 0; i < 3; i++)
        CHECK(!recs(h)[i].t && !recs(h)[i].val && !recs(h)[i].meta, "slot %lu not cleared", i);
    recs(h)[3].t = t3;
    CHECK(sptm_virt_trace_drain(h, 2) < 0, "a drain behind the tail accepted");
    /* The freed slots take the next records: index 5 in slot 0, ... */
    note(h, 7, 2, 0);
    note(h, 8, 4, 0);
    CHECK(h->head == 8 && h->lost == 3, "after a drain: head %lu lost %lu", h->head, h->lost);
    CHECK(recs(h)[0].val == 7 && recs(h)[1].val == 8 && recs(h)[2].val == 9,
          "the records after the wrap");
    CHECK(recs(h)[3].val == 3 && recs(h)[4].val == 4,
          "an undrained record overwritten by the wrap");
    note(h, 9, 2, 0);
    CHECK(h->head == 8 && h->lost == 4, "a full ring after the wrap");
}

/* ---- F4, F5, F7: cores at once, the host draining ---------------------------------------- */

#define CORES 6
#define PER   150000
#define CAP   61

static struct hvtrace_hdr *mt;
static u32 writers_done;

static void *writer(void *arg)
{
    u32 cpu = (u32)(uintptr_t)arg;
    cur_cpu = cpu;
    for (u64 s = 0; s < PER; s++) {
        u64 v = (u64)cpu << 40 | s;
        u64 val[2] = {v, ~v};
        int width = s % 8 == 7 ? 4 : 2;
        sptm_virt_trace_note(mt, PTD + 16 * (s % 512), val, s & 1, width, s % 3 ? 0 : 1,
                             v ^ 0x5a5a5a5a, false, cpu, host_now());
    }
    __atomic_add_fetch(&writers_done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* The host's read and drain, as hvtrace.py does them at a park, while the writers go on (a core
 * that was inside aura when the park began may still be writing). */
static void test_threads(void)
{
    struct hvtrace_cfg c = first_run();
    mt = blk_ring(CAP, &c);
    u64 next[CORES] = {0}, got = 0;
    pthread_t th[CORES];
    struct timespec t0, t1;

    for (u32 i = 0; i < CORES; i++)
        pthread_create(&th[i], NULL, writer, (void *)(uintptr_t)i);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    while (1) {
        bool done = __atomic_load_n(&writers_done, __ATOMIC_SEQ_CST) == CORES;
        u64 tail = mt->tail, head = __atomic_load_n(&mt->head, __ATOMIC_ACQUIRE), upto = tail;
        while (upto < head) {
            struct hvtrace_rec *r = &recs(mt)[upto % CAP];
            if (!__atomic_load_n(&r->t, __ATOMIC_ACQUIRE))
                break;
            u64 m = r->meta, cpu = m >> 52 & 31;
            CHECK(cpu < CORES, "a torn record: core %lu", cpu);
            if (m >> 62 & 1) {
                struct hvtrace_rec *lo = &recs(mt)[(upto + CAP - 1) % CAP];
                CHECK(upto > tail, "a pair's second word without its first");
                CHECK(r->val == ~lo->val && r->pc == lo->pc && (lo->meta >> 52 & 31) == cpu &&
                          (lo->meta >> 48 & 7) == 4,
                      "a pair's words apart: 0x%lx 0x%lx", lo->val, r->val);
            } else {
                u64 s = r->val & ((1UL << 40) - 1);
                CHECK(r->val >> 40 == cpu && r->pc == (r->val ^ 0x5a5a5a5a),
                      "a torn record: val 0x%lx pc 0x%lx core %lu", r->val, r->pc, cpu);
                CHECK(s >= next[cpu], "core %lu's access %lu twice or out of order (after %lu)",
                      cpu, s, next[cpu]);
                next[cpu] = s + 1;
                got++;
            }
            upto++;
        }
        /* A pair whose second word isn't written yet is read whole next time. */
        if (upto > tail && upto < head) {
            u64 m = recs(mt)[(upto - 1) % CAP].meta;
            if ((m >> 48 & 7) == 4 && !(m >> 62 & 1)) {
                upto--;
                got--;
                next[m >> 52 & 31]--;
            }
        }
        CHECK(sptm_virt_trace_drain(mt, upto) == 0, "the host's drain of [%lu, %lu) refused", tail,
              upto);
        if (done && upto == head)
            break;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        CHECK(t1.tv_sec - t0.tv_sec < 60,
              "stalled: a record never completed (tail %lu head %lu, its slot's time %lu)",
              mt->tail, mt->head, recs(mt)[mt->tail % CAP].t);
    }
    for (u32 i = 0; i < CORES; i++)
        pthread_join(th[i], NULL);
    u64 total = (u64)CORES * PER;
    CHECK(got + mt->lost == total, "%lu recorded + %lu lost != %lu accesses", got, mt->lost, total);
    CHECK(mt->blk_lost[0] + mt->blk_lost[1] == mt->lost, "lost per block doesn't add up");
    CHECK(mt->blk_seen[0] + mt->blk_seen[1] == total, "seen per block doesn't add up");
    CHECK(got > total / 100, "only %lu of %lu recorded: the drains don't free the ring", got,
          total);
    printf("threads: %lu recorded, %lu lost, %lu drains\n", got, mt->lost, mt->drains);
}

int main(int argc, char **argv)
{
    CHECK(argc == 2, "usage: trace_host OUT");
    test_check();
    test_blk_hook(argv[1]);
    test_irq_hook(argv[1]);
    test_full();
    test_threads();
    printf("OK%s\n", fails ? " (with failures)" : "");
    return fails ? 1 : 0;
}
