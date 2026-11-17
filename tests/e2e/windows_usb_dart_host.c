/* SPDX-License-Identifier: MIT */

/*
 * D4 host replay of src/hv/windows/hv_usb_dart.c (compiled unchanged, ASan + UBSan): table
 * counts, the descriptor encoders, a full walk of a 4.5 GiB window, faults outside it, the
 * refusals, and the arenas of three windows written to files for the independent Python model
 * (tests/python/test_windows_t8152_usb.py) to compare byte for byte.  All windows are
 * synthetic: none is the M6's computed guest RAM window.
 *
 * Usage: windows_usb_dart_host OUTDIR.  One JSON line per case on stdout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv_usb_dart.h"

#define GIB       (1UL << 30)
#define MIB       (1UL << 20)
#define WIN_BASE  0x10100000000UL /* synthetic, 64 GiB block 0x10 */
#define ARENA_PA  0x10008000000UL /* synthetic carrier heap below the window */

static const char *outdir;
static int failures;

#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

static u64 *alloc_arena(u32 n)
{
    u64 *a = aligned_alloc(USB_DART_PAGE, (size_t)n * USB_DART_PAGE);
    if (!a)
        abort();
    memset(a, 0xa5, (size_t)n * USB_DART_PAGE); /* the builder must zero it */
    return a;
}

static void case_counts(void)
{
    struct {
        u64 start, end;
        u32 want;
    } c[] = {
        {WIN_BASE, WIN_BASE + 4 * GIB + 512 * MIB, 146},     /* 4.5 GiB: 144 leaves + L2 + L1 */
        {WIN_BASE, WIN_BASE + 0x461400000UL, 563},            /* 17.52 GiB, no AHCI store */
        {WIN_BASE, WIN_BASE + USB_DART_PAGE, 3},              /* one page */
        {0x10ff0000000UL, 0x11010000000UL, 1 + 2 + 16},       /* crosses a 64 GiB boundary */
        {WIN_BASE + 0x4000, WIN_BASE + 32 * MIB + 0x8000, 4}, /* unaligned: two partial leaves */
    };
    for (unsigned int i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        u32 n = 0;
        CHECK(usb_dart_tables_needed(c[i].start, c[i].end, &n) == USB_DART_OK);
        CHECK(n == c[i].want);
        printf("{\"case\":\"count\",\"start\":%lu,\"end\":%lu,\"tables\":%u}\n", c[i].start,
               c[i].end, n);
    }
    u32 n;
    CHECK(usb_dart_tables_needed(WIN_BASE + 1, WIN_BASE + GIB, &n) == USB_DART_EINVAL);
    CHECK(usb_dart_tables_needed(WIN_BASE, WIN_BASE + GIB + 0x100, &n) == USB_DART_EINVAL);
    CHECK(usb_dart_tables_needed(WIN_BASE, WIN_BASE, &n) == USB_DART_EINVAL);
    CHECK(usb_dart_tables_needed(WIN_BASE + GIB, WIN_BASE, &n) == USB_DART_EINVAL);
    CHECK(usb_dart_tables_needed((1UL << 42) - USB_DART_PAGE, (1UL << 42) + USB_DART_PAGE, &n) ==
          USB_DART_EINVAL);
    /* The cap: 640 tables is one L1, one L2 and 638 leaf tables (19.94 GiB). */
    CHECK(usb_dart_tables_needed(0, 638 * 32 * MIB, &n) == USB_DART_OK && n == 640);
    CHECK(usb_dart_tables_needed(0, 638 * 32 * MIB + USB_DART_PAGE, &n) == USB_DART_ETOOBIG);
    printf("{\"case\":\"refusals\",\"result\":\"%s\"}\n", failures ? "failed" : "passed");
}

static void case_encoders(void)
{
    u32 ttbr = 0;
    printf("{\"case\":\"encoders\",\"leaf\":%lu,\"table\":%lu,", usb_dart_leaf_pte(WIN_BASE),
           usb_dart_table_pte(ARENA_PA));
    CHECK(usb_dart_ttbr(ARENA_PA, &ttbr) == USB_DART_OK);
    printf("\"ttbr\":%u,\"tlbi_r2_s0\":%lu,\"tlbi_r2_s1\":%lu,\"tlbi_r2_s14\":%lu,", ttbr,
           usb_dart_tlbi_lo(2, 0), usb_dart_tlbi_lo(2, 1), usb_dart_tlbi_lo(2, 14));
    CHECK(usb_dart_ttbr(ARENA_PA + 0x1000, &ttbr) == USB_DART_EINVAL);
    CHECK(usb_dart_ttbr(1UL << 42, &ttbr) == USB_DART_EINVAL);
    printf("\"tcr_on\":%u,\"tcr_idle\":%u}\n", USB_DART_TCR_ON, USB_DART_TCR_IDLE);
}

/* Build, sweep and dump one window. */
static void case_window(const char *name, u64 start, u64 end, bool sweep)
{
    u32 n;
    u64 root = 0;
    CHECK(usb_dart_tables_needed(start, end, &n) == USB_DART_OK);
    u64 *arena = alloc_arena(n);
    CHECK(usb_dart_build_identity(start, end, arena, ARENA_PA, n - 1, &root) == USB_DART_ESPACE);
    CHECK(usb_dart_build_identity(start, end, arena, ARENA_PA + 0x1000, n, &root) ==
          USB_DART_ESPACE);
    CHECK(usb_dart_build_identity(start, end, NULL, ARENA_PA, n, &root) == USB_DART_ESPACE);
    CHECK(usb_dart_build_identity(start, end, arena, ARENA_PA, n, &root) == USB_DART_OK);
    CHECK(root == ARENA_PA);
    CHECK(usb_dart_self_check(arena, ARENA_PA, n, root, start, end) == 0);

    u64 mapped = 0, bad = 0, pa;
    if (sweep) {
        for (u64 dva = start; dva < end; dva += USB_DART_PAGE) {
            if (usb_dart_walk(arena, ARENA_PA, n, root, dva + 0x10, &pa) != USB_DART_OK ||
                pa != dva + 0x10)
                bad++;
            mapped++;
        }
    }
    /* Every valid leaf is a coherent, readable, writable, whole-page identity entry. */
    u64 leaves = 0, flagged = 0;
    for (u64 i = 0; i < (u64)n * USB_DART_PTES; i++) {
        u64 pte = arena[i];
        if (!pte || i < (u64)USB_DART_PTES)
            continue;
        if ((pte & USB_DART_PTE_SP_END) == USB_DART_PTE_SP_END) {
            leaves++;
            if (pte & (USB_DART_PTE_FLAGS | USB_DART_PTE_SP_STRT))
                flagged++;
        }
    }
    CHECK(flagged == 0);
    CHECK(leaves == (end - start) / USB_DART_PAGE);
    CHECK(bad == 0);

    const u64 outside[] = {start - USB_DART_PAGE, end, 0, (1UL << 41) - USB_DART_PAGE,
                           (1UL << 42) - USB_DART_PAGE, ARENA_PA,
                           ARENA_PA + (u64)n * USB_DART_PAGE - 8, end + 4 * GIB};
    u64 faults = 0;
    for (unsigned int i = 0; i < sizeof(outside) / sizeof(outside[0]); i++) {
        if (outside[i] >= start && outside[i] < end)
            continue;
        int ret = usb_dart_walk(arena, ARENA_PA, n, root, outside[i], &pa);
        CHECK(ret == USB_DART_EFAULT);
        faults += ret == USB_DART_EFAULT;
    }
    CHECK(usb_dart_walk(arena, ARENA_PA, n, root, 1UL << 42, &pa) == USB_DART_EFAULT);

    /* A NO_CACHE (non-coherent) leaf must fail the walker: the self check would catch it. */
    u64 *first_leaf = NULL;
    for (u64 i = (u64)USB_DART_PTES; i < (u64)n * USB_DART_PTES && !first_leaf; i++)
        if ((arena[i] & USB_DART_PTE_SP_END) == USB_DART_PTE_SP_END)
            first_leaf = &arena[i];
    if (first_leaf) {
        u64 saved = *first_leaf;
        *first_leaf |= 0x2;
        CHECK(usb_dart_self_check(arena, ARENA_PA, n, root, start, end) != 0);
        *first_leaf = saved;
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.bin", outdir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(arena, USB_DART_PAGE, n, f) != n)
        abort();
    fclose(f);
    printf("{\"case\":\"window\",\"name\":\"%s\",\"start\":%lu,\"end\":%lu,\"tables\":%u,"
           "\"arena_pa\":%lu,\"root\":%lu,\"swept\":%lu,\"leaves\":%lu,\"faults\":%lu}\n",
           name, start, end, n, ARENA_PA, root, mapped, leaves, faults);
    free(arena);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s OUTDIR\n", argv[0]);
        return 2;
    }
    outdir = argv[1];
    case_counts();
    case_encoders();
    case_window("w45", WIN_BASE, WIN_BASE + 4 * GIB + 512 * MIB, true);
    case_window("cross", 0x10ff0000000UL, 0x11010000000UL, true);
    case_window("ragged", WIN_BASE + 0x4000, WIN_BASE + 32 * MIB + 0x8000, true);
    printf("{\"case\":\"done\",\"result\":\"%s\",\"failures\":%d}\n",
           failures ? "failed" : "passed", failures);
    return failures ? 1 : 0;
}
