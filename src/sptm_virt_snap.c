/* SPDX-License-Identifier: MIT */

/*
 * GPU firmware snapshots under the virtualized SPTM (src/sptm_virt_snap.h has the buffer's layout).
 *
 * The walk and the objects are those of the GPU lane's fw272-b2dump.py, so its tools read either:
 * the firmware's page tables from context 0's TTBR1 in the TTBAT (16 KiB pages, three levels at
 * shifts 36, 25 and 14), and from a root VA every object it points to, two levels deep, up to
 * 64 KiB each and while at most 200 are seen, a pointer being any word at any byte offset of an
 * object's first 0x3000 bytes that the firmware's tables translate and whose top 24 bits are
 * 0xfffffc. The chains are the host's: pointer paths from the InitData root to objects copied
 * whole, whatever their size.
 *
 * Nothing is read unless sptm_virt_snap_pa_ok says so: a PTE that names MMIO, or anything else
 * outside guest RAM, is counted and never read.
 */

#ifdef SNAP_HOST
#include "snap_host.h"
#else
#include "sptm_virt_snap.h"
#include "string.h"
#include "utils.h"
#endif

#define PAGE       0x4000UL
#define PA_MASK    0x3ffffffc000UL
#define IAS        ((1UL << 42) - 1)
#define VA_BASE    0xfffffc0000000000UL
#define OBJ_CAP    0x10000UL
#define SCAN       0x3000UL
#define MAX_SEEN   200 /* a new object only while at most this many are seen */
#define DEPTH      2
#define REGION_CAP (1UL << 20)
#define MAX_TABLES 4096 /* page-table pages one walk reads */

u64 sptm_virt_snap_region[4][2];
u64 sptm_virt_snap_roots[4];
struct snap_chain sptm_virt_snap_chains[SNAP_CHAINS];
struct snap_watch sptm_virt_snap_watch[4];
u32 sptm_virt_snap_parts = SNAP_PART_OBJECTS | SNAP_PART_CHAINS;
u32 sptm_virt_snap_max = 1;

/* The host's reader (proxyclient/aura/hvsnap.py) has these sizes too. */
_Static_assert(sizeof(struct snap_hdr) == 128, "snap_hdr");
_Static_assert(sizeof(struct snap_rec) == 64, "snap_rec");
_Static_assert(sizeof(struct snap_chain) == 80, "snap_chain");
_Static_assert(sizeof(struct snap_watch) == 16, "snap_watch");

static const u8 snap_shift[3] = {36, 25, 14};

static struct {
    u8 *buf;
    u64 cap;
    u64 ttbr1;
    u64 initdata; /* the first snapshot's root: every chain starts there */
    u64 seen[MAX_SEEN + 1];
    u32 nseen, nobj, refused, tables;
    bool full;
    const u8 *chain_data[SNAP_CHAINS];
    u64 chain_len[SNAP_CHAINS];
} sn;

static struct snap_hdr *snap_hdr(void)
{
    return (struct snap_hdr *)sn.buf;
}

void sptm_virt_snap_init(void *buf, u64 bytes)
{
    memset(&sn, 0, sizeof(sn));
    sn.buf = buf;
    sn.cap = bytes;
    if (!buf || bytes < sizeof(struct snap_hdr) + 2 * sizeof(struct snap_rec)) {
        sn.buf = NULL;
        return;
    }
    struct snap_hdr *h = snap_hdr();
    memset(h, 0, sizeof(*h));
    h->magic = SNAP_MAGIC;
    h->version = SNAP_VERSION;
    h->hdr_bytes = sizeof(*h);
    h->capacity = bytes;
    h->used = sizeof(*h);
}

/* Data bytes a new record may take, keeping room for the snapshot's END. */
static u64 snap_room(void)
{
    u64 used = snap_hdr()->used, keep = 2 * sizeof(struct snap_rec);
    return sn.cap >= used + keep ? ALIGN_DOWN(sn.cap - used - keep, 16) : 0;
}

/* A record at the end, not yet counted; NULL (and the buffer full) if there's no room for it. */
static struct snap_rec *snap_begin(u32 kind, bool end)
{
    struct snap_hdr *h = snap_hdr();
    u64 keep = (end ? 1 : 2) * sizeof(struct snap_rec);

    if (sn.cap < h->used + keep) {
        sn.full = true;
        return NULL;
    }
    struct snap_rec *r = (struct snap_rec *)(sn.buf + h->used);
    memset(r, 0, sizeof(*r));
    r->kind = kind;
    return r;
}

static void snap_commit(struct snap_rec *r, u64 len)
{
    struct snap_hdr *h = snap_hdr();
    r->len = len;
    h->used += sizeof(*r) + ALIGN_UP(len, 16);
    h->nrec++;
}

static bool snap_ok(u64 pa, u64 len)
{
    if (sptm_virt_snap_pa_ok(pa, len))
        return true;
    sn.refused++;
    return false;
}

static const u64 *snap_table(u64 pa)
{
    return snap_ok(pa, PAGE) ? sptm_virt_snap_pa(pa) : NULL;
}

/* The firmware's translation of va (context 0's TTBR1), as fw272-b2dump.py's translate(). */
static bool snap_translate(u64 va, u64 *pa, u64 *attrs)
{
    u64 t = sn.ttbr1;

    if (!t)
        return false;
    for (int lvl = 0; lvl < 3; lvl++) {
        const u64 *tbl = snap_table(t);
        if (!tbl)
            return false;
        u64 pte = tbl[((va & IAS) >> snap_shift[lvl]) & (lvl ? 0x7ff : 0x3f)];
        if (!(pte & 1))
            return false;
        t = pte & PA_MASK;
        if (lvl == 2) {
            *pa = t | (va & (PAGE - 1));
            *attrs = pte & ~PA_MASK;
        }
    }
    return true;
}

/* Up to n bytes at va, page by page; it stops at the first page that doesn't translate. */
static u64 snap_readva(u64 va, u8 *dst, u64 n)
{
    u64 got = 0;

    while (got < n) {
        u64 pa, at;
        if (!snap_translate(va + got, &pa, &at))
            break;
        u64 chunk = min(n - got, PAGE - (pa & (PAGE - 1)));
        if (!snap_ok(pa, chunk))
            break;
        memcpy(dst + got, sptm_virt_snap_pa(pa), chunk);
        got += chunk;
    }
    return got;
}

static bool snap_seen(u64 va)
{
    for (u32 i = 0; i < sn.nseen; i++)
        if (sn.seen[i] == va)
            return true;
    return false;
}

/* fw272-b2dump.py's dump(): the object, then (depth left) every object it points to. */
static void snap_obj(u64 va, u32 depth, u64 parent, u64 off, u32 slot)
{
    if (snap_seen(va) || sn.nseen > MAX_SEEN)
        return;
    struct snap_rec *r = snap_begin(SNAP_OBJ, false);
    if (!r)
        return;
    u64 cap = min(OBJ_CAP, snap_room());
    u8 *data = (u8 *)(r + 1);
    u64 len = snap_readva(va, data, cap);
    if (!len)
        return;
    if (len == cap && cap < OBJ_CAP)
        sn.full = true;
    sn.seen[sn.nseen++] = va;
    u32 me = sn.nobj++;
    r->va = va;
    snap_translate(va, &r->pa, &r->attrs);
    r->a = parent;
    r->b = off;
    r->c = DEPTH - depth;
    r->d = me | (u64)slot << 32;
    snap_commit(r, len);
    if (!depth)
        return;
    u64 lim = min(len, SCAN);
    for (u64 o = 0; o + 8 <= lim; o++) {
        u64 w, p, a;
        memcpy(&w, data + o, sizeof(w));
        if (w >> 40 == 0xfffffc && snap_translate(w, &p, &a) && !snap_seen(w))
            snap_obj(w, depth - 1, me, o, slot);
    }
}

struct snap_walk {
    u64 *runs;
    u64 nruns, maxruns, leaves;
    u32 refused;
    bool truncated;
};

static void snap_leaf(struct snap_walk *w, u64 va, u64 pa, u64 at)
{
    w->leaves++;
    if (w->nruns) {
        u64 *run = &w->runs[4 * (w->nruns - 1)];
        if (run[3] == at && run[1] == va && run[2] + (run[1] - run[0]) == pa) {
            run[1] += PAGE;
            return;
        }
    }
    if (w->nruns == w->maxruns) {
        w->truncated = true;
        return;
    }
    u64 *run = &w->runs[4 * w->nruns++];
    run[0] = va;
    run[1] = va + PAGE;
    run[2] = pa;
    run[3] = at;
}

static void snap_walk_table(struct snap_walk *w, u64 t, int lvl, u64 va)
{
    if (sn.tables++ >= MAX_TABLES) {
        w->truncated = true;
        return;
    }
    const u64 *tbl = snap_table(t);
    if (!tbl) {
        w->refused++;
        return;
    }
    for (u32 i = 0; i < (lvl ? 2048U : 64U); i++) {
        u64 pte = tbl[i];
        if (!(pte & 1))
            continue;
        u64 v = va | ((u64)i << snap_shift[lvl]);
        if (lvl == 2)
            snap_leaf(w, v, pte & PA_MASK, pte & ~PA_MASK);
        else
            snap_walk_table(w, pte & PA_MASK, lvl + 1, v);
    }
}

/* The TTBAT's first words, and the tree to walk: context 0's TTBR1, else its TTBR0, else the
 * tables region's first page. */
static u32 snap_ttbr(u64 ttbat[4])
{
    u64 base = sptm_virt_snap_region[2][0];
    const u64 *t = base && snap_ok(base, 4 * sizeof(u64)) ? sptm_virt_snap_pa(base) : NULL;

    for (int i = 0; i < 4; i++)
        ttbat[i] = t ? t[i] : 0;
    if (ttbat[1] & 1)
        sn.ttbr1 = ttbat[1] & PA_MASK;
    else if (ttbat[0] & 1)
        sn.ttbr1 = ttbat[0] & PA_MASK;
    else {
        sn.ttbr1 = sptm_virt_snap_region[0][0];
        return SNAP_W_TTBAT_FALLBACK;
    }
    return 0;
}

static void snap_walk(const u64 ttbat[4], u32 flags)
{
    struct snap_rec *r = snap_begin(SNAP_WALK, false);
    if (!r)
        return;
    struct snap_walk w = {.runs = (u64 *)(r + 1), .maxruns = snap_room() / 32};
    sn.tables = 0;
    if (sn.ttbr1)
        snap_walk_table(&w, sn.ttbr1, 0, VA_BASE);
    if (w.truncated)
        flags |= SNAP_W_TRUNCATED;
    if (w.nruns == w.maxruns)
        sn.full = true;
    r->va = sn.ttbr1;
    r->pa = w.leaves;
    r->attrs = flags | (u64)w.refused << 32;
    r->a = ttbat[0];
    r->b = ttbat[1];
    r->c = ttbat[2];
    r->d = ttbat[3];
    snap_commit(r, w.nruns * 32);
}

static void snap_regions(void)
{
    for (u32 i = 0; i < 4; i++) {
        u64 pa = sptm_virt_snap_region[i][0], size = min(sptm_virt_snap_region[i][1], REGION_CAP);
        if (!pa || !size)
            continue;
        struct snap_rec *r = snap_begin(SNAP_REGION, false);
        if (!r)
            return;
        if (size > snap_room()) {
            sn.full = true;
            size = snap_room();
        }
        u8 *d = (u8 *)(r + 1);
        u32 bad = 0;
        for (u64 o = 0; o < size; o += PAGE) {
            u64 n = min(PAGE, size - o);
            if (snap_ok(pa + o, n)) {
                memcpy(d + o, sptm_virt_snap_pa(pa + o), n);
            } else {
                memset(d + o, 0, n);
                bad++;
            }
        }
        r->va = i;
        r->pa = pa;
        r->a = sptm_virt_snap_region[i][1];
        r->c = bad;
        snap_commit(r, size);
    }
}

static void snap_chains(u64 root)
{
    for (u32 i = 0; i < SNAP_CHAINS; i++) {
        const struct snap_chain *c = &sptm_virt_snap_chains[i];
        sn.chain_data[i] = NULL;
        sn.chain_len[i] = 0;
        if (!c->len)
            continue;
        u64 va = root, status = SNAP_C_OK, failed = 0;
        for (u32 s = 0; s < min(c->nsteps, (u32)SNAP_CHAIN_STEPS); s++) {
            u64 st = c->step[s], p = 0;
            if (st & SNAP_STEP_ADD) {
                va += st & ~SNAP_STEP_ADD;
                continue;
            }
            if (snap_readva(va + st, (u8 *)&p, sizeof(p)) != sizeof(p)) {
                status = SNAP_C_UNMAPPED;
                failed = s;
                break;
            }
            va = p;
        }
        struct snap_rec *r = snap_begin(SNAP_CHAIN, false);
        if (!r)
            return;
        u64 len = 0;
        if (status == SNAP_C_OK) {
            u64 want = min(c->len, snap_room());
            if (want < c->len)
                sn.full = true;
            len = snap_readva(va, (u8 *)(r + 1), want);
            if (len < c->len)
                status = SNAP_C_SHORT;
            snap_translate(va, &r->pa, &r->attrs);
            sn.chain_data[i] = (const u8 *)(r + 1);
            sn.chain_len[i] = len;
        }
        r->va = va;
        r->a = i | status << 32;
        r->b = failed;
        memcpy(&r->c, c->name, 2 * sizeof(u64));
        snap_commit(r, len);
    }
    struct snap_hdr *h = snap_hdr();
    for (u32 i = 0; i < ARRAY_SIZE(sptm_virt_snap_watch); i++) {
        const struct snap_watch *w = &sptm_virt_snap_watch[i];
        if (!w->chain || w->chain > SNAP_CHAINS || (h->watch_ok & BIT(i)))
            continue;
        u32 ci = w->chain - 1, n = w->bytes == 4 ? 4 : 8;
        if (!sn.chain_data[ci] || w->off + n > sn.chain_len[ci])
            continue;
        u64 v = 0;
        memcpy(&v, sn.chain_data[ci] + w->off, n);
        h->watch[i] = v;
        h->watch_at[i] = ci | (u64)n << 16 | w->off << 32;
        h->watch_ok |= BIT(i);
    }
}

/* fw272-b2dump.py's root: the InitData message's address, or the value as a VA. */
static u64 snap_root(u64 v)
{
    u64 r = (v >> 48) == 0x81 ? v & 0xfffffffffffUL : v;
    return r < BIT(63) ? r | 0xfffff00000000000UL : r;
}

bool sptm_virt_snap_take(const struct snap_kick *k)
{
    struct snap_hdr *h = snap_hdr();

    if (!sn.buf)
        return false;
    if (h->nsnap >= sptm_virt_snap_max || (h->flags & SNAP_F_FULL)) {
        h->skipped++;
        return false;
    }
    u64 t0 = sptm_virt_snap_ticks();
    bool first = !h->nsnap;
    u32 parts = first ? ~0U : sptm_virt_snap_parts;
    u64 root = snap_root(k->value);
    if (first)
        sn.initdata = root;
    sn.nseen = sn.nobj = sn.refused = 0;
    sn.full = false;

    struct snap_rec *r = snap_begin(SNAP_SNAP, false);
    if (!r) {
        h->flags |= SNAP_F_FULL;
        h->skipped++;
        return false;
    }
    r->va = k->value;
    r->pa = k->value_hi;
    r->attrs = k->pc;
    r->a = k->t;
    r->b = k->cpu | (u64)k->ep << 8 | (u64)k->match << 32;
    r->c = k->ring_head;
    r->d = root;
    snap_commit(r, 0);

    u64 ttbat[4];
    u32 wflags = snap_ttbr(ttbat);
    if (parts & SNAP_PART_REGIONS)
        snap_regions();
    if (parts & SNAP_PART_WALK)
        snap_walk(ttbat, wflags);
    if (parts & SNAP_PART_CHAINS)
        snap_chains(sn.initdata);
    if (parts & SNAP_PART_OBJECTS) {
        bool any = false;
        for (u32 s = 0; s < ARRAY_SIZE(sptm_virt_snap_roots); s++)
            if (sptm_virt_snap_roots[s]) {
                any = true;
                snap_obj(sptm_virt_snap_roots[s], DEPTH, ~0UL, 0, s);
            }
        if (!any)
            snap_obj(root, DEPTH, ~0UL, 0, 0);
    }

    struct snap_rec *e = snap_begin(SNAP_END, true);
    u64 flags = (sn.full ? SNAP_E_FULL : 0) | (sn.nseen > MAX_SEEN ? SNAP_E_OBJ_MAX : 0) |
                (sn.refused ? SNAP_E_REFUSED : 0) | (sn.ttbr1 ? 0 : SNAP_E_NO_TABLE);
    if (e) {
        e->va = h->nsnap;
        e->a = sn.nobj;
        e->b = sptm_virt_snap_ticks() - t0;
        e->c = flags;
        e->d = sn.refused;
        snap_commit(e, 0);
    }
    h->nsnap++;
    if (sn.full)
        h->flags |= SNAP_F_FULL;
    return true;
}
