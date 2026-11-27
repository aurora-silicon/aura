/* SPDX-License-Identifier: MIT */
/*
 * libFuzzer harness for the IOVA allocator (src/iova.c), built for the host by
 * tests/fuzz/run-iova-fuzz.sh.
 *
 * The input is decoded as one domain (iovad_init() with edge-case or raw base/limit) and a
 * sequence of iova_alloc(), iova_reserve() and iova_free() calls, including zero, unaligned,
 * huge and wrapping sizes, double frees, partial frees and frees of ranges that were never
 * handed out. After every call the allocator's free list is compared with a reference model
 * (a sorted array of free intervals):
 *
 *   - every free block is 16K aligned, non-empty, inside [first, limit) and does not wrap;
 *   - blocks are sorted, disjoint and coalesced (no two blocks touch), so a free range is one
 *     block and iova_alloc() fails only when no free range is large enough;
 *   - iova_alloc() returns the lowest free address that fits (first fit), never 0 on success,
 *     and 0 without touching the domain for a size of 0 or one that does not fit;
 *   - iova_reserve() takes exactly [ALIGN_DOWN(iova), ALIGN_UP(iova + sz)) when all of it is
 *     free and otherwise fails without touching the domain;
 *   - iova_free() of a range that is not entirely in use (double free, outside the domain,
 *     unaligned, wrapping) panics before it touches the domain; a zero-size free does nothing
 *     and a valid free makes the range reusable.
 *
 * iovad_shutdown() runs at the end of every input, with and without a DART, and LeakSanitizer
 * checks that no block is left behind. calloc() can be made to fail for iova_reserve() and
 * iovad_init() to cover the out-of-memory paths.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* src/iova.c's calloc() goes through iova_fuzz_calloc() */
static int calloc_fail_after = -1;

void *iova_fuzz_calloc(size_t n, size_t sz)
{
    if (calloc_fail_after == 0) {
        calloc_fail_after = -1;
        return NULL;
    }
    if (calloc_fail_after > 0)
        calloc_fail_after--;
    return calloc(n, sz);
}

#define calloc iova_fuzz_calloc
#include "../../src/iova.c"
#undef calloc

#define PAGE   ((u64)SZ_16K)
#define MAX_IV 4096

int debug_printf(const char *fmt, ...)
{
    if (getenv("IOVA_FUZZ_VERBOSE")) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
    }
    return 0;
}

/* panic() ends here: iova_free() must panic on an invalid range, and nowhere else */
static jmp_buf panic_jmp;
static bool panic_expected;

void flush_and_reboot(void)
{
    if (panic_expected)
        longjmp(panic_jmp, 1);
    fprintf(stderr, "iova: unexpected panic()\n");
    abort();
}

/* dart_free_l2() stub: iovad_shutdown() must call it once per 32M L2 table of the domain */
static u64 dart_l2_calls, dart_l2_expected, dart_l2_base;

void dart_free_l2(dart_dev_t *dart, uintptr_t iova)
{
    (void)dart;
    if (iova & (SZ_32M - 1)) {
        fprintf(stderr, "dart_free_l2(%#lx) not at an L2 table\n", (unsigned long)iova);
        abort();
    }
    if (iova != dart_l2_base + dart_l2_calls * SZ_32M || ++dart_l2_calls > dart_l2_expected) {
        fprintf(stderr, "dart_free_l2(%#lx): call %lu of %lu, out of order or past the domain\n",
                (unsigned long)iova, (unsigned long)dart_l2_calls, (unsigned long)dart_l2_expected);
        abort();
    }
}

/* ---- reference model: sorted, disjoint, coalesced [start, end) free intervals ---- */

struct iv {
    u64 s, e;
};

static struct iv model[MAX_IV];
static size_t nmodel;
static u64 dom_first, dom_limit;

#define fail(...)                                                                                  \
    do {                                                                                           \
        fprintf(stderr, "iova_fuzz: " __VA_ARGS__);                                                \
        fprintf(stderr, "\n");                                                                     \
        dump_state(iovad);                                                                         \
        abort();                                                                                   \
    } while (0)

static void dump_state(iova_domain_t *iovad)
{
    fprintf(stderr, "domain [%#lx, %#lx)\nmodel:", (unsigned long)dom_first,
            (unsigned long)dom_limit);
    for (size_t i = 0; i < nmodel && i < 32; i++)
        fprintf(stderr, " [%#lx,%#lx)", (unsigned long)model[i].s, (unsigned long)model[i].e);
    fprintf(stderr, "\nfree list:");
    int n = 0;
    for (struct iova_block *b = iovad ? iovad->free_list : NULL; b && n < 32; b = b->next, n++)
        fprintf(stderr, " [%#lx,+%#lx)", (unsigned long)b->iova, (unsigned long)b->sz);
    fprintf(stderr, "\n");
}

/* is [s, e) entirely free? (the model is coalesced, so it must sit in one interval) */
static bool model_all_free(u64 s, u64 e)
{
    for (size_t i = 0; i < nmodel; i++)
        if (model[i].s <= s && e <= model[i].e)
            return true;
    return false;
}

static bool model_any_free(u64 s, u64 e)
{
    for (size_t i = 0; i < nmodel; i++)
        if (model[i].s < e && s < model[i].e)
            return true;
    return false;
}

static void model_take(u64 s, u64 e)
{
    for (size_t i = 0; i < nmodel; i++) {
        if (!(model[i].s <= s && e <= model[i].e))
            continue;
        struct iv old = model[i];
        if (old.s == s && old.e == e) {
            memmove(&model[i], &model[i + 1], (nmodel - i - 1) * sizeof(model[0]));
            nmodel--;
        } else if (old.s == s) {
            model[i].s = e;
        } else if (old.e == e) {
            model[i].e = s;
        } else {
            memmove(&model[i + 2], &model[i + 1], (nmodel - i - 1) * sizeof(model[0]));
            model[i].e = s;
            model[i + 1].s = e;
            model[i + 1].e = old.e;
            nmodel++;
        }
        return;
    }
    abort();
}

static void model_give(u64 s, u64 e)
{
    size_t i = 0;
    while (i < nmodel && model[i].e <= s)
        i++;
    /* [s, e) goes before interval i; interval i - 1 may end at s and i start at e */
    memmove(&model[i + 1], &model[i], (nmodel - i) * sizeof(model[0]));
    model[i].s = s;
    model[i].e = e;
    nmodel++;
    if (i > 0 && model[i - 1].e == s) {
        model[i - 1].e = e;
        memmove(&model[i], &model[i + 1], (nmodel - i - 1) * sizeof(model[0]));
        nmodel--;
        i--;
    }
    if (i + 1 < nmodel && model[i + 1].s == model[i].e) {
        model[i].e = model[i + 1].e;
        memmove(&model[i + 1], &model[i + 2], (nmodel - i - 2) * sizeof(model[0]));
        nmodel--;
    }
}

static void check(iova_domain_t *iovad, const char *what)
{
    size_t i = 0;
    u64 prev_end = 0;
    for (struct iova_block *b = iovad->free_list; b; b = b->next, i++) {
        u64 end = b->iova + b->sz;
        if (b->sz == 0 || b->iova % PAGE || b->sz % PAGE)
            fail("%s: empty or unaligned free block [%#lx,+%#lx)", what, (unsigned long)b->iova,
                 (unsigned long)b->sz);
        if (end < b->iova || b->iova < dom_first || end > dom_limit)
            fail("%s: free block [%#lx,+%#lx) outside the domain", what, (unsigned long)b->iova,
                 (unsigned long)b->sz);
        if (i > 0 && b->iova <= prev_end)
            fail("%s: free blocks unsorted, overlapping or not coalesced at %#lx", what,
                 (unsigned long)b->iova);
        if (i >= nmodel || model[i].s != b->iova || model[i].e != end)
            fail("%s: free list differs from the model at block %zu", what, i);
        prev_end = end;
    }
    if (i != nmodel)
        fail("%s: free list has %zu blocks, model %zu", what, i, nmodel);
}

/* ---- input decoding ---- */

struct in {
    const u8 *p;
    size_t n;
};

static u64 get(struct in *in, int bytes)
{
    u64 v = 0;
    for (int i = 0; i < bytes; i++) {
        v <<= 8;
        if (in->n) {
            v |= *in->p++;
            in->n--;
        }
    }
    return v;
}

static u64 get_size(struct in *in)
{
    switch (get(in, 1) % 8) {
        case 0:
            return 0;
        case 1:
            return get(in, 1) * PAGE; /* small, page aligned */
        case 2:
            return get(in, 2) * PAGE + get(in, 2) % PAGE; /* unaligned */
        case 3:
            return UINT64_MAX - get(in, 2); /* wraps when aligned up */
        case 4:
            return get(in, 8); /* raw */
        case 5:
            return (dom_limit - dom_first) - get(in, 1) * PAGE; /* about the domain */
        case 6:
            return get(in, 1) * SZ_32M;
        default:
            return get(in, 1) + 1; /* sub-page */
    }
}

static u64 get_addr(struct in *in)
{
    switch (get(in, 1) % 6) {
        case 0:
            return dom_first + get(in, 2) * PAGE; /* inside, aligned */
        case 1:
            return dom_first + get(in, 2) * PAGE + get(in, 2) % PAGE; /* inside, unaligned */
        case 2:
            return dom_limit - get(in, 1) * PAGE; /* the top edge */
        case 3:
            return get(in, 8);
        case 4:
            return get(in, 1) * PAGE; /* the bottom of the address space, incl. 0 */
        default:
            return UINT64_MAX - get(in, 2);
    }
}

/* ranges handed out by iova_alloc()/iova_reserve() and not yet freed, and ones freed since */
static struct iv live[MAX_IV], dead[64];
static size_t nlive, ndead;

static void forget_live(u64 s, u64 e)
{
    /* drop or trim every live record overlapping [s, e) after a (possibly partial) free */
    for (size_t i = 0; i < nlive;) {
        if (live[i].e <= s || e <= live[i].s) {
            i++;
            continue;
        }
        struct iv old = live[i];
        live[i] = live[--nlive];
        if (old.s < s && nlive < MAX_IV)
            live[nlive++] = (struct iv){old.s, s};
        if (e < old.e && nlive < MAX_IV)
            live[nlive++] = (struct iv){e, old.e};
    }
}

static void do_free(iova_domain_t *iovad, u64 iova, u64 sz)
{
    u64 asz = ALIGN_UP(sz, PAGE);
    u64 end = iova + asz;
    bool valid = sz && asz >= sz && iova % PAGE == 0 && end > iova && iova >= dom_first &&
                 end <= dom_limit && !model_any_free(iova, end);

    if (valid || !sz) {
        iova_free(iovad, iova, sz);
    } else {
        panic_expected = true;
        if (!setjmp(panic_jmp)) {
            iova_free(iovad, iova, sz);
            fail("iova_free(%#lx, %#lx) of a range that is not in use did not panic",
                 (unsigned long)iova, (unsigned long)sz);
        }
        panic_expected = false;
    }
    if (valid) {
        model_give(iova, end);
        forget_live(iova, end);
        dead[ndead++ % ARRAY_SIZE(dead)] = (struct iv){iova, end};
    }
    check(iovad, valid ? "iova_free" : "rejected iova_free");
}

int LLVMFuzzerTestOneInput(const u8 *data, size_t size)
{
    struct in in = {data, size};
    iova_domain_t *iovad = NULL;

    nmodel = nlive = ndead = 0;
    calloc_fail_after = -1;

    /* the domain */
    u64 base, limit;
    switch (get(&in, 1) % 4) {
        case 0:
            base = 0;
            break;
        case 1:
            base = get(&in, 2) * SZ_32M;
            break;
        case 2:
            base = get(&in, 8);
            break;
        default:
            base = 0 - (get(&in, 1) + 1) * (u64)SZ_32M;
            break;
    }
    switch (get(&in, 1) % 4) {
        case 0:
            limit = base + get(&in, 1) * PAGE;
            break;
        case 1:
            limit = base + get(&in, 1) * (u64)SZ_32M;
            break;
        case 2:
            limit = get(&in, 8);
            break;
        default:
            limit = 0 - get(&in, 1) * PAGE;
            break;
    }
    bool init_inject = get(&in, 1) == 0xff;
    if (init_inject)
        calloc_fail_after = get(&in, 1) % 2;

    u64 first = base ? base : PAGE;
    bool init_ok = base % SZ_32M == 0 && limit % PAGE == 0 && limit > first;
    iovad = iovad_init(base, limit);
    bool oom = init_inject && calloc_fail_after == -1;
    calloc_fail_after = -1;
    if (!init_ok && iovad)
        fail("iovad_init(%#lx, %#lx) accepted a bad domain", (unsigned long)base,
             (unsigned long)limit);
    if (!iovad) {
        if (init_ok && !oom)
            fail("iovad_init(%#lx, %#lx) failed", (unsigned long)base, (unsigned long)limit);
        return 0;
    }
    dom_first = first;
    dom_limit = limit;
    model[0] = (struct iv){first, limit};
    nmodel = 1;
    check(iovad, "iovad_init");

    for (int ops = 0; in.n && ops < 512; ops++) {
        if (nmodel + 2 >= MAX_IV)
            break;
        switch (get(&in, 1) % 8) {
            case 0:
            case 1: {
                u64 sz = get_size(&in);
                u64 asz = ALIGN_UP(sz, PAGE);
                u64 want = 0;
                if (sz && asz >= sz)
                    for (size_t i = 0; i < nmodel; i++)
                        if (model[i].e - model[i].s >= asz) {
                            want = model[i].s;
                            break;
                        }
                u64 got = iova_alloc(iovad, sz);
                if (got != want)
                    fail("iova_alloc(%#lx) = %#lx, model says %#lx", (unsigned long)sz,
                         (unsigned long)got, (unsigned long)want);
                if (got) {
                    model_take(got, got + asz);
                    if (nlive < MAX_IV)
                        live[nlive++] = (struct iv){got, got + asz};
                }
                check(iovad, "iova_alloc");
                break;
            }
            case 2: {
                /* free a whole live range */
                if (!nlive)
                    break;
                struct iv r = live[get(&in, 2) % nlive];
                do_free(iovad, r.s, r.e - r.s);
                break;
            }
            case 3: {
                /* free part of a live range, with the size as a caller may pass it (unaligned) */
                if (!nlive)
                    break;
                struct iv r = live[get(&in, 2) % nlive];
                u64 pages = (r.e - r.s) / PAGE;
                u64 off = get(&in, 2) % pages;
                u64 len = get(&in, 2) % (pages - off) + 1;
                u64 sz = len * PAGE - (get(&in, 1) % 2 ? get(&in, 2) % PAGE : 0);
                do_free(iovad, r.s + off * PAGE, sz);
                break;
            }
            case 4: {
                /* free a range freed before (double free) */
                if (!ndead)
                    break;
                struct iv r =
                    dead[get(&in, 1) % (ndead < ARRAY_SIZE(dead) ? ndead : ARRAY_SIZE(dead))];
                do_free(iovad, r.s, r.e - r.s);
                break;
            }
            case 5: {
                /* free anything */
                u64 iova = get_addr(&in);
                do_free(iovad, iova, get_size(&in));
                break;
            }
            case 6: {
                u64 iova = get_addr(&in);
                u64 sz = get_size(&in);
                bool inject = get(&in, 1) == 0xff;
                u64 s = ALIGN_DOWN(iova, PAGE);
                u64 e = ALIGN_UP(iova + sz, PAGE);
                bool wraps = sz && (iova + sz < iova || e < iova + sz);
                if (s == 0)
                    s = PAGE;
                bool empty = !sz || (!wraps && e <= s);
                bool want = empty || (!wraps && model_all_free(s, e));
                bool split = want && !empty;
                if (split) {
                    /* a reserve in the middle of a free range needs a new block */
                    split = false;
                    for (size_t i = 0; i < nmodel; i++)
                        if (model[i].s < s && e < model[i].e)
                            split = true;
                }
                if (inject)
                    calloc_fail_after = 0;
                bool got = iova_reserve(iovad, iova, sz);
                bool oom_hit = inject && split;
                calloc_fail_after = -1;
                if (oom_hit)
                    want = false;
                if (got != want)
                    fail("iova_reserve(%#lx, %#lx) = %d, model says %d", (unsigned long)iova,
                         (unsigned long)sz, got, want);
                if (got && !empty) {
                    model_take(s, e);
                    if (nlive < MAX_IV)
                        live[nlive++] = (struct iv){s, e};
                }
                check(iovad, "iova_reserve");
                break;
            }
            default: {
                /* free everything still live, which must leave one block covering the domain */
                while (nlive) {
                    struct iv r = live[0];
                    do_free(iovad, r.s, r.e - r.s);
                }
                if (nmodel != 1 || model[0].s != dom_first || model[0].e != dom_limit)
                    fail("domain not whole after freeing everything");
                break;
            }
        }
    }

    /* the L2 tables iovad_shutdown() hands back: one per 32M from base up to limit */
    dart_l2_calls = 0;
    dart_l2_base = base;
    dart_l2_expected = (limit - base - 1) / SZ_32M + 1;
    /* walking a huge domain's L2 slots takes as long as it takes; keep the stub to small ones */
    bool with_dart = (size & 1) && dart_l2_expected <= 65536;
    iovad_shutdown(iovad, with_dart ? (dart_dev_t *)&dart_l2_calls : NULL);
    if (with_dart && dart_l2_calls != dart_l2_expected) {
        fprintf(stderr, "iovad_shutdown freed %lu L2 tables, domain has %lu\n",
                (unsigned long)dart_l2_calls, (unsigned long)dart_l2_expected);
        abort();
    }
    return 0;
}
