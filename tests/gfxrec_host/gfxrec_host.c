/* SPDX-License-Identifier: MIT */
/*
 * src/sptm_virt_gfxrec.c with the J773g's windows and a fake clock, its cores as threads
 * (tests/python/test_sptm_virt_gfxrec_host.py also reads the buffers it leaves).
 *
 *   gfxrec_host OUT
 *
 * Checks every rule itself (exit 1 and a FAIL line on the first one broken) and writes
 * OUT/window.bin (a window from its doorbell to its end) and OUT/full.bin (a buffer that filled).
 */
#ifndef GFXREC_HOST
#define GFXREC_HOST
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "gfxrec_host.h"

/* The J773g's windows (its ADT, 27.2). */
#define SGX     0x300000000UL
#define GFX     0x302600000UL
#define PMP     0x380e00000UL
#define PS_GFX  0x380700250UL
#define BLK     0x300e00000UL /* MMIO_PROTECTED */
#define SPTM_PC 0xfffffff027100000UL
#define XNU_PC  0xfffffe0037390000UL
#define FRQ     24000000UL
#define WIN_MS  2000
#define WIN     (WIN_MS * (FRQ / 1000))

static int fails;
#define CHECK(c, ...)                                                                              \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            printf("FAIL %s:%d: ", __func__, __LINE__);                                            \
            printf(__VA_ARGS__);                                                                   \
            printf("\n");                                                                          \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)

static struct gfxrec_rec *recs(void)
{
    return (struct gfxrec_rec *)((u8 *)sptm_virt_gfxrec_buf + sizeof(struct gfxrec_hdr));
}

static void *setup(u64 cap, u64 canary_bytes)
{
    u64 bytes = sizeof(struct gfxrec_hdr) + cap * sizeof(struct gfxrec_rec);
    u8 *buf = calloc(1, bytes + canary_bytes);
    memset(buf + bytes, 0xa5, canary_bytes);
    sptm_virt_gfxrec_init(buf, bytes, FRQ, WIN_MS);
    sptm_virt_gfxrec_source(GFXREC_SRC_GFX_ASC, GFX, 0x88000);
    sptm_virt_gfxrec_source(GFXREC_SRC_PMP_ASC, PMP, 0x88000);
    sptm_virt_gfxrec_source(GFXREC_SRC_SGX, SGX, 0x4000000);
    sptm_virt_gfxrec_add_ps(PS_GFX, "GFX", true);
    return buf;
}

/* One access, performed: is it recorded (rec), and as what? */
static void access(bool rec, u64 addr, u64 v0, u64 v1, bool write, int width, u32 cpu, u64 t,
                   u32 src)
{
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    u64 before = h->claimed;
    u64 val[2] = {v0, v1};
    sptm_virt_gfxrec_note(addr, val, write, width, cpu, t);
    CHECK(val[0] == v0 && val[1] == v1, "the access's value changed");
    if (!rec) {
        CHECK(h->claimed == before, "%s 0x%lx w%d c%u t %lu: recorded, shouldn't be",
              write ? "W" : "R", addr, width, cpu, t);
        return;
    }
    CHECK(h->claimed == before + 1, "%s 0x%lx w%d c%u t %lu: not recorded", write ? "W" : "R", addr,
          width, cpu, t);
    struct gfxrec_rec *r = &recs()[before];
    u64 meta = (addr & 0xffffffffffffUL) | (u64)src << 48 | (u64)write << 52 |
               (u64)(width & 7) << 53 | (u64)(cpu & 15) << 56;
    CHECK(r->t == t, "time 0x%lx, not 0x%lx", r->t, t);
    CHECK(r->meta == meta, "meta 0x%lx, not 0x%lx", r->meta, meta);
    CHECK(r->val == v0 && r->hi == (width == 4 ? v1 : 0), "value 0x%lx 0x%lx", r->val, r->hi);
}

static struct gfxblk_rec *brecs(void)
{
    return (struct gfxblk_rec *)((u8 *)sptm_virt_gfxrec_blk + sizeof(struct gfxblk_hdr));
}

static void *blk_setup(u64 cap, u64 canary_bytes)
{
    u64 bytes = sizeof(struct gfxblk_hdr) + cap * sizeof(struct gfxblk_rec);
    u8 *buf = calloc(1, bytes + canary_bytes);
    memset(buf + bytes, 0xa5, canary_bytes);
    sptm_virt_gfxrec_blk_init(buf, bytes, FRQ, BLK, 0x4000, "/arm-io/sgx reg[1] +0x100000");
    return buf;
}

/* One access to the block, performed: recorded (rec, as n records) or not; never in the mailbox
 * record's buffer. */
static void blk_access(bool rec, u64 addr, u64 v0, u64 v1, bool write, int width, u32 cpu, u64 t,
                       bool gl)
{
    struct gfxblk_hdr *b = sptm_virt_gfxrec_blk;
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    u64 before = b->claimed, main_before = h ? h->claimed : 0;
    u64 val[2] = {v0, v1};
    u64 pc = gl ? SPTM_PC : XNU_PC;
    sptm_virt_gfxrec_blk_note(addr, val, write, width, cpu, t, pc, gl);
    CHECK(val[0] == v0 && val[1] == v1, "the access's value changed");
    CHECK(!h || h->claimed == main_before, "the block took a slot of the mailbox record");
    u32 n = width == 4 ? 2 : 1;
    if (!rec) {
        CHECK(b->claimed == before, "block %s 0x%lx w%d t %lu: recorded, shouldn't be",
              write ? "W" : "R", addr, width, t);
        return;
    }
    CHECK(b->claimed == before + n, "block %s 0x%lx w%d t %lu: not recorded", write ? "W" : "R",
          addr, width, t);
    for (u32 k = 0; k < n; k++) {
        struct gfxblk_rec *r = &brecs()[before + k];
        u64 meta = (addr + 8 * k) | (u64)write << 52 | (u64)(n == 2 ? 3 : width) << 53 |
                   (u64)cpu << 56 | (u64)gl << 60 | (u64)(n == 2) << 61;
        CHECK(r->t == t && r->pc == pc && r->val == val[k], "block record %lu: 0x%lx 0x%lx 0x%lx",
              before + k, r->t, r->pc, r->val);
        CHECK(r->meta == meta, "block meta 0x%lx, not 0x%lx", r->meta, meta);
    }
}

static void blk_save(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    struct gfxblk_hdr *b = sptm_virt_gfxrec_blk;
    u64 n = b->claimed < b->cap ? b->claimed : b->cap;
    fwrite(b, 1, sizeof(*b) + n * sizeof(struct gfxblk_rec), f);
    fclose(f);
}

static void save(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, name);
    FILE *f = fopen(p, "wb");
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    u64 n = h->claimed < h->cap ? h->claimed : h->cap;
    fwrite(h, 1, sizeof(*h) + n * sizeof(struct gfxrec_rec), f);
    fclose(f);
}

static void window(const char *dir)
{
    setup(256, 0);
    blk_setup(64, 0);
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    CHECK(h->state == GFXREC_ARMED && !sptm_virt_gfxrec_ring_stop, "not armed at the start");
    CHECK(sptm_virt_gfxrec_blk->state == GFXREC_ON, "the block isn't on from the start");
    const u64 t0 = 1000000;

    /* The block from XNU's entry, long before the doorbell: SPTM's and XNU's, nothing outside. */
    blk_access(true, BLK + 0x1500, 0x3ff, 0, false, 2, 6, 150, true);
    blk_access(true, BLK + 0x1400, 0x10000, 0, false, 2, 6, 160, false);
    blk_access(false, BLK - 4, 0x1, 0, false, 2, 6, 170, false);
    blk_access(false, BLK + 0x4000, 0x1, 0, true, 2, 6, 171, false);
    CHECK(sptm_virt_gfxrec_blk->t_first == 150, "its first access isn't kept");

    /* Before the doorbell: nothing. */
    access(false, PMP + 0x8800, 0x0013000000000000, 0x20, true, 4, 2, 100, GFXREC_SRC_PMP_ASC);
    access(false, SGX + 0xd60000, 0, 0, true, 2, 9, 200, GFXREC_SRC_SGX);
    access(false, PS_GFX, 0xf, 0, true, 2, 1, 300, GFXREC_SRC_PMGR);
    CHECK(h->state == GFXREC_ARMED && !h->t_done, "an access before the doorbell changed it");

    CHECK(sptm_virt_gfxrec_trigger(t0, 0x00810c2000100000, 0x20, 0xfffffe0037390000, 9),
          "the doorbell didn't start it");
    CHECK(h->state == GFXREC_ON && h->t_start == t0 && h->t_end == t0 + WIN, "not on");
    CHECK(sptm_virt_gfxrec_ring_stop == t0 + WIN, "the ring doesn't stop at the window's end");
    /* The doorbell, once performed. */
    access(true, GFX + 0x8800, 0x00810c2000100000, 0x20, true, 4, 9, t0 + 10, GFXREC_SRC_GFX_ASC);
    CHECK(!sptm_virt_gfxrec_trigger(t0 + 20, 0x00810c2000200000, 0x20, 0, 3) && h->t_start == t0,
          "a second doorbell restarted the window");

    /* The GPU's mailbox: messages both ways; its other registers, the wrong ways: nothing. */
    access(true, GFX + 0x8830, 0x00100000000c000c, 0, false, 4, 0, t0 + 30, GFXREC_SRC_GFX_ASC);
    access(true, GFX + 0x8800, 0x00b0000000000020, 0, true, 4, 6, t0 + 31, GFXREC_SRC_GFX_ASC);
    access(true, GFX + 0x8800, 0x0083000000000011, 0x21, true, 4, 7, t0 + 32, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x8114, 0x20001, 0, false, 2, 9, t0 + 33, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x8110, 0x21101, 0, false, 2, 9, t0 + 34, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x44, 0x10, 0, true, 2, 9, t0 + 35, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x8800, 0x1, 0x2, false, 4, 9, t0 + 36, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x8830, 0x1, 0x2, true, 4, 9, t0 + 37, GFXREC_SRC_GFX_ASC);
    access(false, GFX + 0x8820, 0x1, 0x2, true, 4, 9, t0 + 38, GFXREC_SRC_GFX_ASC);

    /* The PMP's: one written as two halves with another core's first half between them (the
     * host pairs per core), a pair, a read as two halves. */
    access(true, PMP + 0x8800, 0x0037000000000008, 0, true, 3, 3, t0 + 40, GFXREC_SRC_PMP_ASC);
    access(true, PMP + 0x8800, 0x0013000000000000, 0, true, 3, 5, t0 + 41, GFXREC_SRC_PMP_ASC);
    access(true, PMP + 0x8808, 0x20, 0, true, 3, 3, t0 + 42, GFXREC_SRC_PMP_ASC);
    access(true, PMP + 0x8800, 0x0020f50100000000, 0x20, true, 4, 1, t0 + 43, GFXREC_SRC_PMP_ASC);
    access(true, PMP + 0x8830, 0x0021000000000000, 0, false, 3, 4, t0 + 44, GFXREC_SRC_PMP_ASC);
    access(true, PMP + 0x8838, 0x20, 0, false, 3, 4, t0 + 45, GFXREC_SRC_PMP_ASC);
    access(false, PMP + 0x8114, 0x20001, 0, false, 2, 4, t0 + 46, GFXREC_SRC_PMP_ASC);

    /* sgx: the host block, its generation word and the two start words; nothing else. */
    access(true, SGX + 0xd60000, 0, 0, true, 2, 9, t0 + 50, GFXREC_SRC_SGX);
    access(true, SGX + 0xd60060, 1, 0, true, 2, 9, t0 + 51, GFXREC_SRC_SGX);
    access(true, SGX + 0xd60064, 2, 0, true, 2, 9, t0 + 52, GFXREC_SRC_SGX);
    access(false, SGX + 0xd60068, 3, 0, true, 2, 9, t0 + 53, GFXREC_SRC_SGX);
    access(false, SGX + 0xd5fffc, 4, 0, true, 2, 9, t0 + 54, GFXREC_SRC_SGX);
    access(false, SGX + 0xd60000, 5, 0, false, 2, 9, t0 + 55, GFXREC_SRC_SGX);
    access(false, SGX + 0xd640c0, 6, 0, false, 2, 9, t0 + 56, GFXREC_SRC_SGX);
    access(true, SGX + 0x1000104, 0x10001, 0, true, 2, 0, t0 + 57, GFXREC_SRC_SGX);
    access(true, SGX + 0x1000108, 0x41331, 0, true, 2, 0, t0 + 58, GFXREC_SRC_SGX);
    access(false, SGX + 0x100010c, 7, 0, true, 2, 0, t0 + 59, GFXREC_SRC_SGX);
    access(false, SGX + 0x1000100, 8, 0, true, 2, 0, t0 + 60, GFXREC_SRC_SGX);

    /* The PMGR: GFX's PS word written, and by a 32-bit store pair over it; not its reads, not
     * the next device's word. */
    access(true, PS_GFX, 0xf, 0, true, 2, 1, t0 + 70, GFXREC_SRC_PMGR);
    access(false, PS_GFX, 0xff, 0, false, 2, 1, t0 + 71, GFXREC_SRC_PMGR);
    access(false, PS_GFX + 8, 0xf, 0, true, 2, 1, t0 + 72, GFXREC_SRC_PMGR);
    access(false, PS_GFX + 4, 0xf, 0, true, 2, 1, t0 + 73, GFXREC_SRC_PMGR);
    access(true, PS_GFX - 4, 0x0000000f00000004UL, 0, true, 3, 1, t0 + 74, GFXREC_SRC_PMGR);
    access(false, 0x380700000UL, 0xf, 0, true, 2, 1, t0 + 75, GFXREC_SRC_PMGR);

    /* The block in the window: a write at its last word, SPTM's 128-bit pair, XNU polling. */
    blk_access(true, BLK + 0x3ffc, 0x1, 0, true, 2, 9, t0 + 80, false);
    blk_access(true, BLK + 0x10, 0x1111, 0x2222, false, 4, 3, t0 + 81, true);
    blk_access(true, BLK + 0x1400, 0x10001, 0, false, 2, 6, t0 + 82, false);
    blk_access(true, BLK + 0x1500, 0x3ff, 0, false, 2, 6, t0 + 83, false);
    blk_access(true, BLK + 0x20, 0x5, 0, false, 2, 6, t0 + WIN - 1, false);
    blk_access(false, BLK + 0x20, 0x5, 0, false, 2, 6, t0 + WIN, false);
    CHECK(sptm_virt_gfxrec_blk->state == GFXREC_DONE_TIME &&
              sptm_virt_gfxrec_blk->t_done == t0 + WIN,
          "the block didn't end with the window (state %u)", sptm_virt_gfxrec_blk->state);
    blk_save(dir, "blk.bin");

    /* The window's last tick, then its end: nothing at or after it. */
    access(true, PMP + 0x8800, 0x0080000000000001, 0, true, 4, 2, t0 + WIN - 1, GFXREC_SRC_PMP_ASC);
    access(false, PMP + 0x8800, 0x0080000000000002, 0, true, 4, 2, t0 + WIN, GFXREC_SRC_PMP_ASC);
    CHECK(h->state == GFXREC_DONE_TIME && h->t_done == t0 + WIN, "not ended by time (state %u)",
          h->state);
    access(false, PMP + 0x8800, 0x0080000000000003, 0, true, 4, 2, t0 + 5, GFXREC_SRC_PMP_ASC);
    CHECK(h->claimed == 18, "%lu records, not 18", h->claimed);
    save(dir, "window.bin");
}

static void full(const char *dir)
{
    const u64 canary = 256;
    u8 *buf = setup(4, canary);
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    u64 end = sizeof(*h) + 4 * sizeof(struct gfxrec_rec);
    sptm_virt_gfxrec_trigger(500, 0x0081000000100000, 0x20, 0, 9);
    for (int i = 0; i < 4; i++)
        access(true, PMP + 0x8830, 0x0012000000004000 + i, 0x20, false, 4, i, 600 + i,
               GFXREC_SRC_PMP_ASC);
    u64 val[2] = {0x13, 0x20};
    sptm_virt_gfxrec_note(PMP + 0x8800, val, true, 4, 0, 700);
    CHECK(h->state == GFXREC_DONE_FULL && h->t_done == 700, "a full buffer isn't said (%u)",
          h->state);
    sptm_virt_gfxrec_note(PMP + 0x8800, val, true, 4, 0, 701);
    CHECK(h->claimed == 5, "%lu slots claimed, not 5", h->claimed);
    for (u64 i = 0; i < canary; i++)
        CHECK(buf[end + i] == 0xa5, "written past the buffer at +%lu", i);
    save(dir, "full.bin");

    /* A block record that fills: a pair that doesn't fit ends it, nothing past its buffer. */
    u8 *bbuf = blk_setup(3, canary);
    struct gfxblk_hdr *b = sptm_virt_gfxrec_blk;
    u64 bend = sizeof(*b) + 3 * sizeof(struct gfxblk_rec);
    blk_access(true, BLK + 0x1400, 0x10000, 0, false, 2, 0, 800, false);
    blk_access(true, BLK + 0x1400, 0x10000, 0, false, 2, 0, 801, false);
    u64 pair[2] = {1, 2};
    sptm_virt_gfxrec_blk_note(BLK + 0x10, pair, true, 4, 0, 802, XNU_PC, false);
    CHECK(b->state == GFXREC_DONE_FULL && b->t_done == 802, "a full block isn't said (%u)",
          b->state);
    sptm_virt_gfxrec_blk_note(BLK + 0x10, pair, true, 2, 0, 803, XNU_PC, false);
    CHECK(b->claimed == 4, "%lu block slots claimed, not 4", b->claimed);
    for (u64 i = 0; i < canary; i++)
        CHECK(bbuf[bend + i] == 0xa5, "written past the block's buffer at +%lu", i);
    blk_save(dir, "blk-full.bin");
}

/* ---- cores at once ------------------------------------------------------------------------- */

#define CORES 8
#define EACH  50000

static volatile u64 clock_t0;
static volatile int go, stop_reader;
static volatile u32 trig_wins[CORES];

static void *core(void *arg)
{
    u32 cpu = (u32)(uintptr_t)arg;
    while (!go)
        ;
    for (u64 s = 0; s < EACH; s++) {
        u64 val[2] = {(u64)(cpu + 1) << 32 | s, 0x20};
        u64 t = __atomic_add_fetch(&clock_t0, 1, __ATOMIC_RELAXED);
        sptm_virt_gfxrec_note(PMP + ((s & 1) ? 0x8800 : 0x8830), val, s & 1, 4, cpu, t);
    }
    return NULL;
}

/* While the cores record: a slot with its time must be complete. */
static void *reader(void *arg)
{
    (void)arg;
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    while (!stop_reader) {
        u64 n = __atomic_load_n(&h->claimed, __ATOMIC_ACQUIRE);
        n = n < h->cap ? n : h->cap;
        for (u64 i = n > 4096 ? n - 4096 : 0; i < n; i++) {
            struct gfxrec_rec *r = &recs()[i];
            if (!__atomic_load_n(&r->t, __ATOMIC_ACQUIRE))
                continue;
            if (!r->val || !r->meta) {
                printf("FAIL reader: slot %lu has its time but not its value\n", i);
                exit(1);
            }
        }
    }
    return NULL;
}

static void *racer(void *arg)
{
    u32 cpu = (u32)(uintptr_t)arg;
    while (!go)
        ;
    trig_wins[cpu] = sptm_virt_gfxrec_trigger(1000 + cpu, 0x0081000000100000, 0x20, 0, cpu);
    return NULL;
}

static void cores(void)
{
    pthread_t th[CORES + 1];

    /* Doorbells on every core at once: one starts the window. */
    setup(16, 0);
    go = 0;
    for (u32 c = 0; c < CORES; c++)
        pthread_create(&th[c], NULL, racer, (void *)(uintptr_t)c);
    go = 1;
    u32 wins = 0, winner = 0;
    for (u32 c = 0; c < CORES; c++) {
        pthread_join(th[c], NULL);
        if (trig_wins[c]) {
            wins++;
            winner = c;
        }
    }
    CHECK(wins == 1, "%u doorbells started the window", wins);
    CHECK(sptm_virt_gfxrec_buf->trig_cpu == winner &&
              sptm_virt_gfxrec_buf->t_start == 1000 + winner,
          "the header isn't the winner's");

    /* Every core recording at once: each access once, none lost. */
    setup((u64)CORES * EACH, 0);
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    sptm_virt_gfxrec_trigger(1, 0x0081000000100000, 0x20, 0, 0);
    clock_t0 = 10;
    go = 0;
    stop_reader = 0;
    for (u32 c = 0; c < CORES; c++)
        pthread_create(&th[c], NULL, core, (void *)(uintptr_t)c);
    pthread_create(&th[CORES], NULL, reader, NULL);
    go = 1;
    for (u32 c = 0; c < CORES; c++)
        pthread_join(th[c], NULL);
    stop_reader = 1;
    pthread_join(th[CORES], NULL);
    CHECK(h->claimed == (u64)CORES * EACH && h->state == GFXREC_ON, "%lu claimed, state %u",
          h->claimed, h->state);
    u8 *seen = calloc((u64)CORES * EACH, 1);
    for (u64 i = 0; i < h->claimed; i++) {
        struct gfxrec_rec *r = &recs()[i];
        CHECK(r->t, "slot %lu never written", i);
        u32 cpu = (r->val >> 32) - 1;
        u64 s = r->val & 0xffffffff;
        CHECK(cpu < CORES && s < EACH && ((r->meta >> 56) & 15) == cpu, "slot %lu: 0x%lx", i,
              r->val);
        CHECK(((r->meta >> 52) & 1) == (s & 1), "slot %lu: direction", i);
        CHECK(!seen[cpu * EACH + s]++, "core %u's access %lu recorded twice", cpu, s);
    }
    free(seen);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s OUT\n", argv[0]);
        return 2;
    }
    window(argv[1]);
    full(argv[1]);
    cores();
    printf("ok %d\n", fails);
    return 0;
}
