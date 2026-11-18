/* SPDX-License-Identifier: MIT */

/* G2: the carrier's audit of the GPU page tables.  See gpu_t8152_uat.h. */

#include "gpu_t8152_uat.h"
#include "string.h"
#include "utils.h"

#define OA_MASK    (((1UL << 48) - 1) & ~((1UL << 14) - 1)) /* output address, bits 47:14 */
#define BLOCK_MASK (((1UL << 48) - 1) & ~((1UL << 25) - 1)) /* 32 MiB block, bits 47:25 */
#define BLOCK_SIZE (1UL << 25)
#define LEVELS     3

struct walk {
    const struct gpu_uat_scope *s;
    const struct gpu_uat_ops *ops;
    const struct gpu_uat_baseline *known;
    struct gpu_uat_baseline *record;
    struct gpu_uat_report *rep;
    u32 nvisited;
};

const char *gpu_uat_class_name(u32 cls)
{
    static const char *const names[GPU_UC_COUNT] = {"pool", "uat", "data", "fw-mmio", "text",
                                                    "dram", "mmio", "bad", "table"};
    return cls < GPU_UC_COUNT ? names[cls] : "?";
}

static bool within(u64 a, u64 n, const struct gpu_uat_span *s)
{
    return s->size && a >= s->base && n <= s->size && a - s->base <= s->size - n;
}

static bool overlaps(u64 a, u64 n, u64 b, u64 m)
{
    return n && m && a < b + m && b < a + n;
}

static u32 classify(const struct gpu_uat_scope *s, u64 pa, u64 size)
{
    if (within(pa, size, &s->pool))
        return GPU_UC_POOL;
    for (u32 i = 0; i < 4; i++) {
        if (within(pa, size, &s->fw_rw[i]))
            return GPU_UC_UAT;
        if (within(pa, size, &s->fw_ro[i]))
            return GPU_UC_DATA;
    }
    for (u32 i = 0; i < s->nfw_mmio; i++) {
        const struct gpu_range *w = &s->fw_mmio[i];
        struct gpu_uat_span page = {ALIGN_DOWN(w->base, GPU_T8152_PAGE), 0};
        page.size = ALIGN_UP(w->base + w->size, GPU_T8152_PAGE) - page.base;
        if (within(pa, size, &page))
            return GPU_UC_FW_MMIO;
    }
    for (u32 i = 0; i < GPU_T8152_ROLES; i++)
        if (overlaps(pa, size, s->text[i].base, s->text[i].size))
            return GPU_UC_TEXT;
    if (overlaps(pa, size, s->dram_base, s->dram_size))
        return GPU_UC_DRAM;
    return GPU_UC_MMIO;
}

static int cmp(const struct gpu_uat_outlier *a, u64 key, u64 desc)
{
    if (a->key != key)
        return a->key < key ? -1 : 1;
    if (a->desc != desc)
        return a->desc < desc ? -1 : 1;
    return 0;
}

static bool baseline_has(const struct gpu_uat_baseline *b, u64 key, u64 desc)
{
    if (!b->sorted) {
        for (u32 i = 0; i < b->n; i++)
            if (!cmp(&b->e[i], key, desc))
                return true;
        return false;
    }
    u32 lo = 0, hi = b->n;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        int c = cmp(&b->e[mid], key, desc);
        if (!c)
            return true;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

/* In-place heapsort by (key, desc): no allocation, O(n log n) for a baseline of any size. */
static void sift(struct gpu_uat_outlier *e, u32 root, u32 n)
{
    for (;;) {
        u32 child = 2 * root + 1;
        if (child >= n)
            return;
        if (child + 1 < n && cmp(&e[child], e[child + 1].key, e[child + 1].desc) < 0)
            child++;
        if (cmp(&e[root], e[child].key, e[child].desc) >= 0)
            return;
        struct gpu_uat_outlier t = e[root];
        e[root] = e[child];
        e[child] = t;
        root = child;
    }
}

static void baseline_sort(struct gpu_uat_baseline *b)
{
    for (u32 i = b->n / 2; i-- > 0;)
        sift(b->e, i, b->n);
    for (u32 end = b->n; end > 1; end--) {
        struct gpu_uat_outlier t = b->e[0];
        b->e[0] = b->e[end - 1];
        b->e[end - 1] = t;
        sift(b->e, 0, end - 1);
    }
    b->sorted = true;
}

u32 gpu_uat_baseline_cap(const struct gpu_uat_scope *s)
{
    u64 entries = 2 * GPU_UAT_CONTEXTS + 1024;
    for (u32 i = 1; i < ARRAY_SIZE(s->tables); i++)
        entries += s->tables[i].size / sizeof(u64);
    return entries > (1u << 20) ? 1u << 20 : (u32)entries;
}

/* Who writes the table an entry lies in. */
enum owner {
    OWN_FW = 0,    /* the firmware's own tables (page-table region, extra roots' lower levels) */
    OWN_HOST = 1,  /* the shared-L2 table: host-owned (Linux maps kernel-high VAs there), seeded
                      by the boot chain, so what it holds at PROBE is the baseline */
    OWN_GUEST = 2, /* pool tables and context-table roots: only the guest writes them */
};

static void outlier(struct walk *w, u64 key, u64 desc, u32 cls, u32 level, u32 owner)
{
    struct gpu_uat_report *rep = w->rep;
    bool in_pool = owner != OWN_FW;
    struct gpu_uat_outlier o = {key, desc, (u8)cls, (u8)level, in_pool};
    rep->leaves[cls]++;
    rep->outliers++;
    if (w->record && owner != OWN_GUEST) {
        /*
         * Recording the baseline: this is what "known" will mean.  Only firmware tables count:
         * an outlier in a pool table or a context-table root (the guest's, possibly stale from
         * an earlier boot) is never known, so the guest cannot inherit it.
         */
        if (w->record->n < w->record->cap)
            w->record->e[w->record->n++] = o;
        else
            w->record->overflow = true;
        if (!w->known) {
            rep->known++;
            return;
        }
    }
    if (w->known && baseline_has(w->known, key, desc)) {
        rep->known++;
        return;
    }
    rep->fresh++;
    if (in_pool)
        rep->fresh_in_pool++;
    if (rep->nfirst < GPU_UAT_REPORT_MAX)
        rep->first[rep->nfirst++] = o;
}

static bool table_readable(const struct gpu_uat_scope *s, u64 pa)
{
    for (u32 i = 0; i < ARRAY_SIZE(s->tables); i++)
        if (within(pa, GPU_T8152_PAGE, &s->tables[i]))
            return true;
    return false;
}

static void leaf(struct walk *w, u64 pa, u64 size, u64 key, u64 desc, u32 level, u32 owner)
{
    u32 cls = classify(w->s, pa, size);
    if (cls >= GPU_UC_TEXT)
        outlier(w, key, desc, cls, level, owner);
    else
        w->rep->leaves[cls]++;
}

/* A table at pa, reached through entry key (descriptor desc) of a table of owner `from`. */
static void walk_table(struct walk *w, u64 pa, u32 level, u64 key, u64 desc, u32 from)
{
    const struct gpu_uat_scope *s = w->s;
    for (u32 i = 0; i < w->nvisited; i++)
        if (s->visited[i] == pa)
            return;
    if (!table_readable(s, pa)) {
        outlier(w, key, desc, GPU_UC_TABLE, level - 1, from);
        return;
    }
    if (w->nvisited >= s->visited_cap) {
        w->rep->capped = true;
        return;
    }
    s->visited[w->nvisited++] = pa;
    const u64 *t = w->ops->page(pa, w->ops->arg);
    if (!t) {
        outlier(w, key, desc, GPU_UC_TABLE, level - 1, from);
        return;
    }
    w->rep->tables++;
    u32 owner = within(pa, GPU_T8152_PAGE, &s->pool)      ? OWN_GUEST :
                within(pa, GPU_T8152_PAGE, &s->tables[2]) ? OWN_HOST :
                                                            OWN_FW;
    for (u32 i = 0; i < GPU_UAT_ENTRIES; i++) {
        u64 d = t[i];
        if (!(d & 1))
            continue;
        u64 k = pa | i;
        if (level < LEVELS && (d & 3) == 3)
            walk_table(w, d & OA_MASK, level + 1, k, d, owner);
        else if (level == 2 && (d & 3) == 1)
            leaf(w, d & BLOCK_MASK, BLOCK_SIZE, k, d, level, owner);
        else if (level == LEVELS && (d & 3) == 3)
            leaf(w, d & OA_MASK, GPU_T8152_PAGE, k, d, level, owner);
        else
            outlier(w, k, d, GPU_UC_BAD, level, owner);
    }
}

u32 gpu_uat_audit(const struct gpu_uat_scope *s, const struct gpu_uat_ops *ops,
                  const struct gpu_uat_baseline *known, struct gpu_uat_baseline *record,
                  struct gpu_uat_report *rep)
{
    struct walk w = {s, ops, known, record, rep, 0};
    memset(rep, 0, sizeof(*rep));
    if (record) {
        record->n = 0;
        record->overflow = false;
        record->sorted = false;
    }
    u64 roots[2 * GPU_UAT_CONTEXTS + GPU_UAT_MAX_ROOTS];
    u64 keys[ARRAY_SIZE(roots)];
    u32 n = 0, nctx = 0;
    const u64 *ctx = s->ctx_table.size >= 2 * GPU_UAT_CONTEXTS * sizeof(u64) ?
                         ops->page(ALIGN_DOWN(s->ctx_table.base, GPU_T8152_PAGE), ops->arg) :
                         NULL;
    if (!ctx) {
        outlier(&w, s->ctx_table.base, 0, GPU_UC_TABLE, 0, OWN_GUEST); /* no audit possible */
    } else {
        ctx += (s->ctx_table.base & (GPU_T8152_PAGE - 1)) / sizeof(u64);
        for (u32 i = 0; i < 2 * GPU_UAT_CONTEXTS; i++)
            if (ctx[i] & 1) {
                keys[n] = s->ctx_table.base + i * sizeof(u64);
                roots[n++] = ctx[i];
            }
        nctx = n;
    }
    for (u32 i = 0; i < s->nroots && i < GPU_UAT_MAX_ROOTS; i++) {
        keys[n] = s->roots[i];
        roots[n++] = s->roots[i] | 3;
    }
    for (u32 i = 0; i < n; i++) {
        rep->roots++;
        /* The context table is the guest's: a root it names counts like a pool table's entry. */
        walk_table(&w, roots[i] & OA_MASK, 1, keys[i], roots[i], i < nctx ? OWN_GUEST : OWN_FW);
    }
    if (rep->capped) {
        /* An incomplete walk proves nothing: count it as one fresh outlier, in the pool. */
        rep->fresh++;
        rep->fresh_in_pool++;
    }
    if (record && !known)
        baseline_sort(record);
    return rep->fresh;
}
