/* SPDX-License-Identifier: MIT */
/*
 * src/sptm_virt_snap.c against a fake guest RAM holding a GPU firmware's page tables and objects
 * (tests/python/test_sptm_virt_snap_host.py compares what it copies with fw272-b2dump.py's rules).
 *
 *   snap_host OUT CHAINS.bin WATCH.bin
 *
 * writes OUT/ram.bin (the fake RAM), OUT/meta.txt (where things are), OUT/snapshot.bin (a B2
 * snapshot and a later kick's) and OUT/small.bin (the B2 one into a buffer too small for it). Any
 * read of a PA sptm_virt_snap_pa_ok would refuse ends the run at once.
 */
#ifndef SNAP_HOST
#define SNAP_HOST
#endif
#include <stdio.h>
#include <stdlib.h>

#include "snap_host.h"

#define RAM      0x10000000000UL
#define RAM_SIZE (32UL << 20)
#define HOLE     (RAM + 0x1ff0000) /* a page inside RAM that may not be read */
#define MMIO     0x280000000UL     /* a PA outside RAM */
#define PG       0x4000UL
#define PA_MASK  0x3ffffffc000UL
#define IAS      ((1UL << 42) - 1)
#define ATTR     0x0060000000000443UL

static u8 *ram;
static u64 bump = RAM + 0x400000;
static u64 tbump = RAM + 0x100000; /* the tables region: page tables come from it */
static u64 l0;

static bool in_ram(u64 pa, u64 len)
{
    return pa >= RAM && pa < RAM + RAM_SIZE && len <= RAM + RAM_SIZE - pa;
}

bool sptm_virt_snap_pa_ok(u64 pa, u64 len)
{
    return len && in_ram(pa, len) && !(pa < HOLE + PG && pa + len > HOLE);
}

const void *sptm_virt_snap_pa(u64 pa)
{
    if (!sptm_virt_snap_pa_ok(pa, 8)) {
        fprintf(stderr, "FAIL: read of PA 0x%lx, which the platform refuses\n", pa);
        exit(2);
    }
    return ram + (pa - RAM);
}

static u64 now;
u64 sptm_virt_snap_ticks(void)
{
    return now += 24;
}

static u64 *at(u64 pa)
{
    return (u64 *)(ram + (pa - RAM));
}

static u64 page(void)
{
    u64 p = bump;
    bump += PG;
    return p;
}

static u64 table(void)
{
    u64 p = tbump;
    tbump += PG;
    return p;
}

/* Map one page; a table pointer may be given for level 1 (to point it somewhere odd). */
static void map(u64 va, u64 pa, u64 attrs)
{
    u64 t = l0;
    static const int sh[3] = {36, 25, 14};
    for (int lvl = 0; lvl < 2; lvl++) {
        u64 *e = at(t) + (((va & IAS) >> sh[lvl]) & (lvl ? 0x7ff : 0x3f));
        if (!*e)
            *e = table() | 3;
        t = *e & PA_MASK;
    }
    at(t)[((va & IAS) >> 14) & 0x7ff] = pa | attrs | 3;
}

/* An object of size bytes at va, in fresh pages: scattered (a gap after each, the attributes of
 * the first page apart), or contiguous with one set of attributes (one run in the walk). */
static u64 obj_at(u64 va, u64 size, bool contig)
{
    u64 first = 0;
    for (u64 o = 0; o < size; o += PG) {
        u64 p = page();
        if (!contig)
            bump += PG;
        if (!o)
            first = p;
        map(va + o, p, ATTR + (o && !contig ? 0x10 : 0));
        for (u64 i = 0; i < PG; i += 8)
            *at(p + i) = 0x0001000000000000UL + o + i; /* never a pointer, at any byte */
    }
    return first;
}

static u64 obj(u64 va, u64 size)
{
    return obj_at(va, size, false);
}

static void put(u64 va, u64 off, u64 value)
{
    /* through the fake tables, like the firmware */
    u64 t = l0;
    static const int sh[3] = {36, 25, 14};
    for (int lvl = 0; lvl < 3; lvl++)
        t = at(t)[(((va + off) & IAS) >> sh[lvl]) & (lvl ? 0x7ff : 0x3f)] & PA_MASK;
    memcpy(ram + (t - RAM) + ((va + off) & (PG - 1)), &value, 8);
}

static void dump(const char *dir, const char *name, const void *p, u64 n)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n) {
        perror(path);
        exit(1);
    }
    fclose(f);
}

static void load(const char *path, void *dst, u64 max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(1);
    }
    fread(dst, 1, max, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: snap_host OUT CHAINS.bin WATCH.bin\n");
        return 1;
    }
    ram = calloc(1, RAM_SIZE);
    load(argv[2], sptm_virt_snap_chains, sizeof(sptm_virt_snap_chains));
    load(argv[3], sptm_virt_snap_watch, sizeof(sptm_virt_snap_watch));

    /* The regions: tables (page tables come from it), shared-l2 (its second page is the hole's
     * neighbour... the hole itself), TTBAT, handoff. */
    u64 tables = RAM + 0x100000, l2r = HOLE - PG, ttbs = RAM + 0x144000, handoff = RAM + 0x148000;
    for (u64 i = 0; i < PG; i += 8) {
        *at(l2r + i) = 0x12000000 + i;
        *at(handoff + i) = 0x34000000 + i;
    }
    l0 = table();
    *at(ttbs) = (RAM + 0x1800000) | 1; /* context 0's TTBR0: valid, but TTBR1 wins */
    *at(ttbs + 8) = l0 | 1;
    *at(ttbs + 16) = 0xabc;
    *at(ttbs + 24) = 0xdef;
    sptm_virt_snap_region[0][0] = tables;
    sptm_virt_snap_region[0][1] = 0x40000;
    sptm_virt_snap_region[1][0] = l2r;
    sptm_virt_snap_region[1][1] = 2 * PG;
    sptm_virt_snap_region[2][0] = ttbs;
    sptm_virt_snap_region[2][1] = PG;
    sptm_virt_snap_region[3][0] = handoff;
    sptm_virt_snap_region[3][1] = PG;

    const u64 V = 0xfffffc2000000000UL;
    u64 root = V + 0x100000, mainc = V + 0x200000, hw = V + 0x300000, fwd = V + 0x400000;
    u64 a = V + 0x500000, a2 = V + 0x510000, a3 = V + 0x520000, b = V + 0x530000;
    u64 cnot = V + 0x540000, dnot = V + 0x550000, many = V + 0x600000, mm = V + 0x700000;
    u64 unm = V + 0x710000, under = 0xfffffc3f00000000UL, devr = V + 0x800000;
    obj(root, 2 * PG);
    obj(mainc, PG);
    obj_at(hw, 0x12000, true);
    obj(fwd, 0x6000);
    obj(a, PG);
    obj(a2, PG);
    obj(a3, PG);
    obj(b, PG);
    obj(cnot, PG);
    obj(dnot, PG);
    obj(many, PG);
    obj(devr, 0x4000);
    map(mm, MMIO, ATTR);           /* a firmware mapping of MMIO */
    map(V + 0x720000, HOLE, ATTR); /* and of the hole in RAM */
    /* A level-0 entry whose table is outside RAM: nothing under it may be read. */
    at(l0)[((under & IAS) >> 36) & 0x3f] = MMIO | 3;

    put(root, 0x18, mainc);
    put(root, 0x25, a); /* unaligned */
    put(root, 0x40, a); /* again */
    put(root, 0x48, root);
    put(root, 0x50, unm);
    put(root, 0x58, mm);
    put(root, 0x60, under + 0x4000);
    put(root, 0x68, 0xfffffd2000500000UL); /* translates (IAS), but isn't 0xfffffc */
    put(root, 0x70, V + 0x720000);
    put(root, 0xb0, fwd);
    put(root, 0x2f00, many);
    put(a, 0x10, a2);
    put(a2, 0x10, a3); /* depth 3: never copied */
    put(mainc, 0x0, hw);
    for (int i = 0; i < 12; i++) {
        u64 st = V + 0x900000 + i * PG, ring = V + 0xa00000 + i * PG;
        obj(st, PG);
        obj(ring, PG);
        put(mainc, 0x20 + 0x20 * i, st);
        put(mainc, 0x28 + 0x20 * i, st + 0x10);
        put(mainc, 0x30 + 0x20 * i, st + 0x20);
        put(mainc, 0x38 + 0x20 * i, ring);
    }
    u64 dst = V + 0xb00000;
    obj(dst, PG);
    put(mainc, 0x1a0, dst);
    put(mainc, 0x1b8, devr);
    put(dst, 0x20, 7); /* the device-control ring's write index */
    for (int i = 0; i < 4; i++) {
        u64 st = V + 0xc00000 + i * PG, ring = V + 0xd00000 + i * PG;
        obj(st, PG);
        obj(ring, PG);
        put(mainc, 0x1c0 + 0x10 * i, st);
        put(mainc, 0x1c8 + 0x10 * i, ring);
    }
    put(mainc, 0x2ff8, b);    /* the last word the scan reads */
    put(fwd, 0x2ff9, cnot);   /* starts inside the scan, runs past it */
    put(mainc, 0x3100, dnot); /* past it */
    put(hw, 0xfa0, 0x1122334455667788UL);
    put(hw, 0xfa8, 0x8000);
    for (int i = 0; i < 60; i++)
        put(hw, 0x640 + 0x28 * i + 8, mm); /* HW data's MMIO map */
    u64 fst = V + 0xe00000, fring = V + 0xe10000;
    obj(fst, PG);
    obj(fring, PG);
    put(fwd, 0x5168, fst);
    put(fwd, 0x5170, fring);
    for (int i = 0; i < 300; i++) {
        u64 o = V + 0x1000000 + (u64)i * PG;
        obj(o, PG);
        put(many, 8 * i, o);
    }

    u64 cap = 32UL << 20;
    u8 *buf = malloc(cap);
    sptm_virt_snap_init(buf, cap);
    sptm_virt_snap_max = 2;
    struct snap_kick k = {.t = 100,
                          .value = 0x0081000000000000UL | (root & 0xfffffffffffUL),
                          .value_hi = 0x20,
                          .pc = 0xfffffe0012345678UL,
                          .ring_head = 4321,
                          .cpu = 9,
                          .ep = 0x20,
                          .match = 1};
    if (!sptm_virt_snap_take(&k))
        return 3;
    /* A later kick: the firmware moved the device-control ring on; its objects come from a. */
    put(dst, 0x20, 8);
    sptm_virt_snap_roots[0] = a;
    struct snap_kick k2 = {.t = 200,
                           .value = 0x0003000000000005UL,
                           .value_hi = 0x21,
                           .cpu = 2,
                           .ep = 0x21,
                           .match = 7};
    if (!sptm_virt_snap_take(&k2))
        return 3;
    if (sptm_virt_snap_take(&k2))
        return 4; /* over the budget */
    put(dst, 0x20, 7);
    sptm_virt_snap_roots[0] = 0;
    const struct snap_hdr *h = (const void *)buf;
    dump(argv[1], "snapshot.bin", buf, h->used);

    /* The same B2 snapshot into a buffer too small for it, a guard after its end. */
    u64 small = 0x60000;
    u8 *sb = malloc(small + PG);
    memset(sb + small, 0xa5, PG);
    sptm_virt_snap_init(sb, small);
    sptm_virt_snap_max = 1;
    if (!sptm_virt_snap_take(&k))
        return 5;
    for (u64 i = 0; i < PG; i++)
        if (sb[small + i] != 0xa5) {
            fprintf(stderr, "FAIL: the small buffer was written past its end\n");
            return 6;
        }
    dump(argv[1], "small.bin", sb, ((const struct snap_hdr *)sb)->used);

    dump(argv[1], "ram.bin", ram, RAM_SIZE);
    char meta[1024];
    int n = snprintf(meta, sizeof(meta),
                     "ram 0x%lx\nhole 0x%lx\nroot 0x%lx\ntables 0x%lx 0x40000\nshared-l2 0x%lx "
                     "0x8000\nttbs 0x%lx 0x4000\nhandoff 0x%lx 0x4000\nkick_root 0x%lx\n",
                     RAM, HOLE, root, tables, l2r, ttbs, handoff, a);
    dump(argv[1], "meta.txt", meta, n);
    printf("ok: %u records, 0x%lx bytes\n", h->nrec, h->used);
    return 0;
}
