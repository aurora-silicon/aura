/* SPDX-License-Identifier: MIT */

/*
 * Guest MMIO trace of chosen ADT nodes under the virtualized SPTM (SPTM_VIRT_MMIO, debug only).
 *
 * The host names /arm-io children (sptm_virt_mmio_nodes, a comma list; "name" for all of its
 * "reg" ranges, "name/a-b+c" for chosen indices). Every 16 KiB page of those ranges gets a
 * pass-through stage-2 hook: the guest's access is done for real at its width (an LDP/STP pair as
 * a pair), then recorded into the auractl ring with its value, width, direction, cpu, time and
 * guest PC. Nothing is read or written that the guest did not ask for, so registers with read
 * side effects see exactly the guest's accesses.
 *
 * Not hooked, said on the console and in the ring: a coprocessor node (compatible "iop...": the
 * ASC mailboxes AURA_SEP_TRACE, AURA_DCP_AFK_TRACE and AURA_S2R_COPROC use) and any page inside
 * one's or a DART's registers (unless that DART is named itself), a page that is not a plain
 * guest mapping (another tracer's hook, or a page aura hides), a range of more than
 * MMT_BIG_PAGES pages unless its index is named, and pages past the budget (MMT_MAX_PAGES).
 *
 * Budget: identical consecutive reads of a node (same address, width, value and cpu: a polling
 * loop) are folded into one repeat record; at most sptm_virt_mmio_max access records are kept per
 * node, the rest are counted: sptm_virt_mmio_dropped, and one drop record per node written at the
 * first drop whose count is kept up to date in the ring (a new one if the ring has moved on half
 * its size since).
 *
 * Ring records (struct s2r_rec): every one has meta bit 63 set and the node's index in meta
 * [62:58], so the other decoders can skip them.
 *   an access: an ordinary record (pa, width, write, cpu; val; pc = the guest PC); a 16-byte pair
 *        is two 8-byte records
 *   markers (meta bit 55), tag in pc's low word:
 *   DMMN the node's name, 8 bytes per record: pc |= part << 32, val = the bytes
 *   DMMG a reg range: pc |= index << 32 | pages hooked << 40, pa = base, val = size
 *   DMMS a page not hooked: pc |= reason << 32 | index << 40 (1 already hooked or not mapped,
 *        2 coprocessor or DART registers, 3 page budget used up, 4 a range too big: pa = its
 *        base, val = its size), pa = the page
 *   DMMR a folded poll: pa, width and write of the access, val = the repeats after the recorded
 *        one, t = the last repeat's time (written at the next different access of the node, or
 *        after 65536 repeats)
 *   DMMD drops: val = the node's records dropped so far, updated in place
 *   DMMC the setup: pc |= nodes << 32 | pages << 40, val = sptm_virt_mmio_max
 * tools/auractl/mmio_decode.py prints them, alone or joined with the SEP and DCP traces.
 */

#include "../build/build_cfg.h"

#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "smp.h"
#include "sptm_virt.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "utils.h"

/* Host: the nodes (comma list), the access records kept per node. */
char sptm_virt_mmio_nodes[256] = "dptx-phy,atc0-dpxbar,atc0-dpin0,atc0-dpin1,atc1-dpxbar,"
                                 "atc1-dpin0,atc1-dpin1";
u32 sptm_virt_mmio_max = 65536;
/* For the host after a stop: records dropped (all nodes), pages hooked. */
u64 sptm_virt_mmio_dropped;
u32 sptm_virt_mmio_pages;

#ifdef AURACTL

#ifndef MMT_MAX_PAGES
#define MMT_MAX_PAGES 1024
#endif
#define MMT_BIG_PAGES 64
#define MMT_NODES     32 /* meta [62:58] */
#define MMT_EXCL      256
#define MMT_PG        0x4000UL
#define MMT_REP_FLUSH 65536

#define MMT_META(node) (BIT(63) | (u64)(node) << 58)

#define TAG_N 0x4e4d4d44UL /* "DMMN" */
#define TAG_G 0x474d4d44UL /* "DMMG" */
#define TAG_S 0x534d4d44UL /* "DMMS" */
#define TAG_R 0x524d4d44UL /* "DMMR" */
#define TAG_D 0x444d4d44UL /* "DMMD" */
#define TAG_C 0x434d4d44UL /* "DMMC" */

#define WHY_HOOKED 1
#define WHY_EXCL   2
#define WHY_BUDGET 3
#define WHY_BIG    4

struct mmt_node {
    char name[24];
    int adt; /* its ADT offset */
    u32 nrec, dropped;
    /* the last access, for folding a poll */
    bool have_last, last_write;
    int last_width, last_cpu;
    u64 last_pa, last_val;
    u32 rep;
    u64 rep_t;
    volatile u64 *drop_val; /* the drop record's value in the ring */
    u64 drop_head;          /* the ring's head when it was written */
};

static struct mmt_node *mmt;
static int mmt_n;
/* on the heap when on, as the other tracers' state: none of it in aura's image */
static struct mmt_page {
    u64 page;
    int node;
} *mmt_pages; /* MMT_MAX_PAGES, sorted by page */
static u32 mmt_npages;
static struct mmt_excl {
    u64 base, end;
    int adt;
} *mmt_excl; /* MMT_EXCL */
static int mmt_nexcl;
static u32 mmt_complaints;
static DECLARE_SPINLOCK(mmt_lock);

AURA_XTEXT static void mmt_marker(int node, u64 t, u64 pa, int width, bool write, u64 val, u64 pc)
{
    sptm_virt_s2r_rec_ext(t, MMT_META(node), pa, width, write, true, val, pc);
}

AURA_XTEXT static int mmt_page_node(u64 page)
{
    u32 lo = 0, hi = mmt_npages;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (mmt_pages[mid].page == page)
            return mmt_pages[mid].node;
        if (mmt_pages[mid].page < page)
            lo = mid + 1;
        else
            hi = mid;
    }
    return -1;
}

/* The poll folded so far, as one record. */
AURA_XTEXT static void mmt_flush_rep(int n)
{
    struct mmt_node *d = &mmt[n];
    if (!d->rep)
        return;
    mmt_marker(n, d->rep_t, d->last_pa, d->last_width, false, d->rep, TAG_R);
    d->rep = 0;
}

AURA_XTEXT static void mmt_record(struct exc_info *ctx, int n, u64 pa, u64 val, bool write,
                                  int width)
{
    struct mmt_node *d = &mmt[n];
    int cpu = smp_id();

    spin_lock(&mmt_lock);
    if (!write && d->have_last && !d->last_write && d->last_pa == pa && d->last_val == val &&
        d->last_width == width && d->last_cpu == cpu) {
        d->rep_t = mrs(CNTPCT_EL0);
        if (++d->rep >= MMT_REP_FLUSH)
            mmt_flush_rep(n);
        spin_unlock(&mmt_lock);
        return;
    }
    mmt_flush_rep(n);
    if (d->nrec >= sptm_virt_mmio_max) {
        d->dropped++;
        sptm_virt_mmio_dropped++;
        if (d->drop_val && sptm_virt_s2r_head - d->drop_head < sptm_virt_s2r_nrec / 2) {
            *d->drop_val = d->dropped;
        } else {
            d->drop_head = sptm_virt_s2r_head;
            d->drop_val =
                sptm_virt_s2r_rec_ext(0, MMT_META(n), 0, 3, false, true, d->dropped, TAG_D);
        }
    } else {
        d->nrec++;
        sptm_virt_s2r_rec_ext(0, MMT_META(n), pa, width, write, false, val, ctx ? ctx->elr : 0);
    }
    d->have_last = true;
    d->last_write = write;
    d->last_pa = pa;
    d->last_val = val;
    d->last_width = width;
    d->last_cpu = cpu;
    spin_unlock(&mmt_lock);
}

AURA_XTEXT static bool mmt_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    int n = mmt_page_node(addr & ~(MMT_PG - 1));

    if (width == 4 || width == 5) {
        /* LDP/STP of X registers (16 bytes), or of Q registers (32): pairs of 8-byte words */
        for (int h = 0; h < width - 3; h++) {
            u64 a = addr + 16 * h, *v = val + 2 * h;
            if (write)
                __asm__ volatile("stp %0, %1, [%2]" ::"r"(v[0]), "r"(v[1]), "r"(a) : "memory");
            else
                __asm__ volatile("ldp %0, %1, [%2]" : "=r"(v[0]), "=r"(v[1]) : "r"(a) : "memory");
            if (n >= 0) {
                mmt_record(ctx, n, a, v[0], write, 3);
                mmt_record(ctx, n, a + 8, v[1], write, 3);
            }
        }
        return true;
    }
    if (width < 0 || width > 3) {
        if (mmt_complaints++ < 8)
            printf("SPTM-virt[mmio]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n",
                   1u << width, addr, ctx ? ctx->elr : 0);
        return false;
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    if (n >= 0)
        mmt_record(ctx, n, addr, *val, write, width);
    return true;
}

AURA_XTEXT static bool mmt_compat_prefix(int node, const char *prefix)
{
    u32 len = 0;
    const char *c = adt_getprop(adt, node, "compatible", &len);
    return c && len > strlen(prefix) && !strncmp(c, prefix, strlen(prefix));
}

/* Registers of the coprocessors (ASC wrappers, their mailboxes) and of the DARTs. */
AURA_XTEXT static void mmt_excl_setup(void)
{
    int bus = adt_path_offset(adt, "/arm-io");
    if (bus < 0)
        return;
    ADT_FOREACH_CHILD(adt, bus)
    {
        if (!mmt_compat_prefix(bus, "iop") && !mmt_compat_prefix(bus, "dart,"))
            continue;
        char path[64];
        int p[8];
        snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, bus));
        if (adt_path_offset_trace(adt, path, p) < 0)
            continue;
        u64 pa, sz;
        for (int i = 0; i < 64 && mmt_nexcl < MMT_EXCL; i++) {
            if (adt_get_reg(adt, p, "reg", i, &pa, &sz) < 0)
                break;
            mmt_excl[mmt_nexcl].base = pa;
            mmt_excl[mmt_nexcl].end = pa + sz;
            mmt_excl[mmt_nexcl].adt = bus;
            mmt_nexcl++;
        }
    }
}

AURA_XTEXT static bool mmt_excluded(u64 page, int adt_node)
{
    for (int i = 0; i < mmt_nexcl; i++)
        if (mmt_excl[i].adt != adt_node && page < mmt_excl[i].end &&
            page + MMT_PG > mmt_excl[i].base)
            return true;
    return false;
}

AURA_XTEXT static void mmt_skip(int n, u64 page, u32 why, u64 val, int idx)
{
    static const char *const text[] = {"", "already hooked or not mapped",
                                       "coprocessor or DART registers", "page budget"};
    mmt_marker(n, 0, page, 3, false, val, TAG_S | (u64)why << 32 | (u64)idx << 40);
    if (why == WHY_BUDGET)
        printf("SPTM-virt[mmio]: %s: page 0x%lx not hooked: page budget (%u) used up\n",
               mmt[n].name, page, MMT_MAX_PAGES);
    else if (why < WHY_BIG)
        printf("SPTM-virt[mmio]: %s: page 0x%lx not hooked: %s\n", mmt[n].name, page, text[why]);
}

/* Hook one page for node n; false when the budget is used up. */
AURA_XTEXT static bool mmt_add_page(int n, u64 page, u32 *hooked)
{
    if (mmt_page_node(page) >= 0)
        return true; /* another range (of this node or an earlier one) has it */
    if (mmt_excluded(page, mmt[n].adt)) {
        mmt_skip(n, page, WHY_EXCL, 0, 0);
        return true;
    }
    if (sptm_virt_rg_owns(page) || !(hv_pt_walk(page) & PTE_VALID)) {
        mmt_skip(n, page, WHY_HOOKED, 0, 0);
        return true;
    }
    if (mmt_npages >= MMT_MAX_PAGES) {
        mmt_skip(n, page, WHY_BUDGET, 0, 0);
        return false;
    }
    u32 i = mmt_npages;
    while (i && mmt_pages[i - 1].page > page) {
        mmt_pages[i] = mmt_pages[i - 1];
        i--;
    }
    mmt_pages[i].page = page;
    mmt_pages[i].node = n;
    mmt_npages++;
    if (hv_map_hook(page, mmt_hook, MMT_PG) < 0) {
        printf("SPTM-virt[mmio]: %s: cannot hook 0x%lx\n", mmt[n].name, page);
        memmove(&mmt_pages[i], &mmt_pages[i + 1], (mmt_npages - i - 1) * sizeof(mmt_pages[0]));
        mmt_npages--;
        return true;
    }
    (*hooked)++;
    return true;
}

/* "name" or "name/a-b+c": the reg indices wanted (all if none named). */
AURA_XTEXT static bool mmt_parse(const char *tok, size_t len, char *name, size_t nsz, u64 *mask)
{
    size_t i = 0;
    while (i < len && tok[i] != '/')
        i++;
    if (!i || i >= nsz)
        return false;
    memcpy(name, tok, i);
    name[i] = 0;
    *mask = 0;
    if (i == len)
        return true;
    i++;
    while (i < len) {
        u32 a = 0, b;
        bool digit = false;
        while (i < len && tok[i] >= '0' && tok[i] <= '9') {
            a = a * 10 + (tok[i++] - '0');
            digit = true;
        }
        b = a;
        if (i < len && tok[i] == '-') {
            i++;
            b = 0;
            while (i < len && tok[i] >= '0' && tok[i] <= '9')
                b = b * 10 + (tok[i++] - '0');
        }
        if (!digit || b < a || b > 63)
            return false;
        for (u32 k = a; k <= b; k++)
            *mask |= BIT(k);
        if (i < len && tok[i] != '+')
            return false;
        i++;
    }
    return *mask != 0;
}

AURA_XTEXT static void mmt_add(const char *tok, size_t len)
{
    char name[24], path[48];
    int p[8];
    u64 mask;

    if (!mmt_parse(tok, len, name, sizeof(name), &mask)) {
        printf("SPTM-virt[mmio]: '%.*s': not a node name[/indices]; ignored\n", (int)len, tok);
        return;
    }
    snprintf(path, sizeof(path), "/arm-io/%s", name);
    int node = adt_path_offset_trace(adt, path, p);
    if (node < 0) {
        printf("SPTM-virt[mmio]: %s: not in the ADT\n", name);
        return;
    }
    if (mmt_compat_prefix(node, "iop")) {
        printf("SPTM-virt[mmio]: %s: a coprocessor node; not traced\n", name);
        return;
    }
    if (mmt_n >= MMT_NODES) {
        printf("SPTM-virt[mmio]: %s: more than %d nodes; not traced\n", name, MMT_NODES);
        return;
    }
    int n = mmt_n++;
    struct mmt_node *d = &mmt[n];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->adt = node;
    for (size_t off = 0; off < strlen(d->name); off += 8) {
        u64 v = 0;
        memcpy(&v, d->name + off, min(strlen(d->name) - off, 8UL));
        mmt_marker(n, 0, 0, 3, false, v, TAG_N | (u64)(off / 8) << 32);
    }

    bool room = true;
    for (int i = 0; i < 64 && room; i++) {
        u64 pa, sz;
        if (adt_get_reg(adt, p, "reg", i, &pa, &sz) < 0)
            break;
        if (mask && !(mask & BIT(i)))
            continue;
        u64 first = ALIGN_DOWN(pa, MMT_PG), end = ALIGN_UP(pa + sz, MMT_PG);
        u64 pages = sz ? (end - first) / MMT_PG : 0;
        if (!mask && pages > MMT_BIG_PAGES) {
            printf("SPTM-virt[mmio]: %s reg[%d] 0x%lx +0x%lx: %lu pages, over %d; name it %s/%d "
                   "to hook it\n",
                   name, i, pa, sz, pages, MMT_BIG_PAGES, name, i);
            mmt_skip(n, pa, WHY_BIG, sz, i);
            continue;
        }
        u32 hooked = 0;
        for (u64 page = first; page < end && room; page += MMT_PG)
            room = mmt_add_page(n, page, &hooked);
        mmt_marker(n, 0, pa, 3, false, sz, TAG_G | (u64)i << 32 | (u64)hooked << 40);
        printf("SPTM-virt[mmio]: %d %s reg[%d] 0x%lx +0x%lx: %u pages hooked\n", n, name, i, pa, sz,
               hooked);
    }
}

AURA_XTEXT void sptm_virt_mmio_setup(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_MMIO))
        return;
    mmt = calloc(MMT_NODES, sizeof(*mmt));
    mmt_pages = calloc(MMT_MAX_PAGES, sizeof(*mmt_pages));
    mmt_excl = calloc(MMT_EXCL, sizeof(*mmt_excl));
    if (!mmt || !mmt_pages || !mmt_excl || !sptm_virt_s2r_ring_ensure()) {
        printf("SPTM-virt[mmio]: no ring; MMIO trace off\n");
        return;
    }
    if (!sptm_virt_mmio_max)
        sptm_virt_mmio_max = 65536;
    mmt_excl_setup();
    sptm_virt_mmio_nodes[sizeof(sptm_virt_mmio_nodes) - 1] = 0;
    const char *s = sptm_virt_mmio_nodes;
    while (*s) {
        const char *e = s;
        while (*e && *e != ',')
            e++;
        if (e > s)
            mmt_add(s, e - s);
        s = *e ? e + 1 : e;
    }
    sptm_virt_mmio_pages = mmt_npages;
    sptm_virt_s2r_rec_ext(0, MMT_META(0), 0, 3, false, true, sptm_virt_mmio_max,
                          TAG_C | (u64)mmt_n << 32 | (u64)mmt_npages << 40);
    printf("SPTM-virt[mmio]: %d nodes, %u pages, at most %u records per node, ring %lu records\n",
           mmt_n, mmt_npages, sptm_virt_mmio_max, (unsigned long)sptm_virt_s2r_nrec);
}
#else
void sptm_virt_mmio_setup(void)
{
    if (sptm_virt_flags & SPTM_VIRT_MMIO)
        printf("SPTM-virt[mmio]: MMIO trace needs an AURACTL=1 build\n");
}
#endif
