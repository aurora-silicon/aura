# SPDX-License-Identifier: MIT
"""The SEP firmware reservation is split around DCP logs wherever they fall inside it.

Runs kboot.c's dt_split_sep_reservations() and its call in kboot_prepare_dt() as they are, on
FDTs built here in the shape the loader leaves them: an M4 Pro (J616s) tree whose DCP OS logs lie
inside the SEP range, a tree whose SEP node sits at another path, and trees on which the split
does not apply and which must come out unchanged.
"""

from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def sources():
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("struct sep_interval {")
    split = source[start:source.index("\n}\n", source.index("dt_split_sep_reservations(void)")) + 3]
    call = source.index("dt_split_sep_reservations())", source.index("int kboot_prepare_dt("))
    begin = source.rindex("\n    if (", 0, call) + 1
    end = source.index(";\n", call) + 2
    return split, source[begin:end]


def test_sep_split_follows_the_reservations(tmp_path):
    split, call = sources()
    run_c(tmp_path, "sep-dcp-log-split", r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long long u64;
typedef uint32_t u32;
#define T6040 0x6040
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt;
static void *adt __attribute__((unused));
/* The machine is a J616s: what a board-keyed call would look at. */
static u32 chip_id __attribute__((unused)) = T6040;
static u32 board_id __attribute__((unused)) = 6;
__attribute__((unused)) static bool adt_is_compatible(const void *tree, int node, const char *compat)
{ (void)tree; (void)node; return !strcmp(compat, "J616sAP"); }
/* run_c() links libfdt without fdt_addresses.c */
int fdt_address_cells(const void *fdt, int node)
{ const fdt32_t *v = fdt_getprop(fdt, node, "#address-cells", NULL); return v ? (int)fdt32_ld(v) : 2; }
int fdt_size_cells(const void *fdt, int node)
{ const fdt32_t *v = fdt_getprop(fdt, node, "#size-cells", NULL); return v ? (int)fdt32_ld(v) : 1; }
''' + split + r'''
static int prepare(void)
{
''' + call + r'''
    return 0;
}

#define SEP_BASE 0x10000000ULL
#define SEP_SIZE 0x00400000ULL
static unsigned char blob[16384], before[16384];

static int reserve(const char *name, u64 base, u64 size, bool no_map, const char *compat)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    int node = fdt_add_subnode(dt, resv, name);
    fdt64_t reg[2] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    assert(node >= 0);
    assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
    if (no_map)
        assert(!fdt_setprop_empty(dt, node, "no-map"));
    if (compat)
        assert(!fdt_setprop_string(dt, node, "compatible", compat));
    return node;
}

/* A tree as dt_set_sep() leaves it, with the SEP node at sep_path. */
static void tree_at(const char *sep_path, u64 base, u64 size)
{
    int root, resv, aliases, sep, fw;
    const char *leaf = strrchr(sep_path, '/') + 1;
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    root = 0;
    resv = fdt_add_subnode(dt, root, "reserved-memory");
    assert(resv >= 0);
    assert(!fdt_setprop_u32(dt, resv, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, resv, "#size-cells", 2));
    aliases = fdt_add_subnode(dt, root, "aliases");
    assert(aliases >= 0);
    assert(!fdt_setprop_string(dt, aliases, "sep", sep_path));
    if (strchr(sep_path + 1, '/')) {
        int parent = fdt_add_subnode(dt, root, "arm-io");
        assert(parent >= 0);
        sep = fdt_add_subnode(dt, parent, leaf);
    } else {
        sep = fdt_add_subnode(dt, root, leaf);
    }
    assert(sep >= 0);
    assert(!fdt_setprop_string(dt, sep, "compatible", "apple,sep"));
    fw = reserve("sep-firmware", base, size, false, "apple,asc-mem");
    assert(!fdt_setprop_u32(dt, fw, "phandle", 7));
    sep = fdt_path_offset(dt, sep_path);
    assert(!fdt_setprop_u32(dt, sep, "memory-region", 7));
    assert(!fdt_setprop(dt, sep, "memory-region-names", "sepfw", 6));
}

static void tree(const char *sep_path)
{
    tree_at(sep_path, SEP_BASE, SEP_SIZE);
}

static int reserved(const char *name)
{
    return fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
}

/* A DCP __OS_LOG reservation as the display handoff makes it: no compatible, the marker, no-map. */
static void oslog(const char *name, u64 base, u64 size)
{
    int node = reserve(name, base, size, true, NULL);
    assert(!fdt_setprop_empty(dt, node, "apple,dcp-os-log"));
}

static u64 retained(u64 base)
{
    char name[64];
    snprintf(name, sizeof(name), "sep-retained@%llx", base);
    int node = reserved(name), len;
    if (node < 0)
        return 0;
    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    assert(reg && len == 16 && fdt64_ld(reg) == base);
    return fdt64_ld(reg + 1);
}

static int count_retained(void)
{
    int node, n = 0;
    fdt_for_each_subnode(node, dt, fdt_path_offset(dt, "/reserved-memory")) {
        const char *name = fdt_get_name(dt, node, NULL);
        n += !strncmp(name, "sep-retained@", 13);
    }
    return n;
}

static void expect_sep_unlinked(const char *sep_path)
{
    int sep = fdt_path_offset(dt, sep_path);
    assert(reserved("sep-firmware") == -FDT_ERR_NOTFOUND);
    assert(!fdt_getprop(dt, sep, "memory-region", NULL));
    assert(!fdt_getprop(dt, sep, "memory-region-names", NULL));
}

static void expect_unchanged(void)
{
    assert(!memcmp(blob, before, sizeof(blob)));
}

#define REFUSED(setup) do { setup; memcpy(before, blob, sizeof(blob)); \
    assert(!prepare()); expect_unchanged(); } while (0)

int main(void)
{
    /* J616s: two DCP OS logs inside the SEP range are split out, without a board list. */
    tree("/sep@406600000");
    oslog("dcp-oslog@10300000", SEP_BASE + 0x300000, 0x40000);
    oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
    assert(!prepare());
    expect_sep_unlinked("/sep@406600000");
    assert(count_retained() == 3);
    assert(retained(SEP_BASE) == 0x100000);
    assert(retained(SEP_BASE + 0x200000) == 0x100000);
    assert(retained(SEP_BASE + 0x340000) == SEP_SIZE - 0x340000);
    assert(reserved("dcp-oslog@10100000") >= 0 && reserved("dcp-oslog@10300000") >= 0);

    /* The SEP node is found through its alias, wherever the tree puts it; a log that runs past
     * the start of the range is clamped to it. */
    tree("/arm-io/sep");
    oslog("dcp-oslog@ff00000", SEP_BASE - 0x100000, 0x200000);
    assert(!prepare());
    expect_sep_unlinked("/arm-io/sep");
    assert(count_retained() == 1);
    assert(retained(SEP_BASE + 0x100000) == SEP_SIZE - 0x100000);

    /* The J616s layout of a 26.6.2 boot, read from the Mac: sep-firmware 0x10005f5c000-
     * 0x100064d0000; the first DCP OS log 0x10005f3c000-0x10005f60000 starts below it, two more
     * follow inside, and the AOP's firmware segment (no-map asc-mem) comes right after them. */
    tree_at("/sep@406600000", 0x10005f5c000ULL, 0x574000);
    oslog("dcp-oslog@10005f3c000", 0x10005f3c000ULL, 0x24000);
    oslog("dcp-oslog@10005f60000", 0x10005f60000ULL, 0x24000);
    oslog("dcp-oslog@10005f84000", 0x10005f84000ULL, 0x24000);
    reserve("asc-firmware@10005fa8000", 0x10005fa8000ULL, 0x2c000, true, "apple,asc-mem");
    assert(!prepare());
    expect_sep_unlinked("/sep@406600000");
    assert(count_retained() == 1);
    assert(retained(0x10005fd4000ULL) == 0x100064d0000ULL - 0x10005fd4000ULL);
    assert(reserved("dcp-oslog@10005f3c000") >= 0 && reserved("asc-firmware@10005fa8000") >= 0);

    /* A Mac whose SEP range overlaps nothing keeps its tree as it is. */
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@20000000", 0x20000000, 0x100000));

    /* Another coprocessor's no-map firmware inside the range, with no DCP OS log there: nothing
     * shows the warm SEP range is shared, so it stays whole. */
    REFUSED(tree("/arm-io/sep");
            reserve("asc-firmware@10100000", SEP_BASE + 0x100000, 0x100000, true, "apple,asc-mem"));
    /* The same firmware beside a DCP OS log is split out with it. */
    tree("/arm-io/sep");
    oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
    reserve("asc-firmware@10300000", SEP_BASE + 0x300000, 0x40000, true, "apple,asc-mem");
    assert(!prepare());
    expect_sep_unlinked("/arm-io/sep");
    assert(count_retained() == 3);
    assert(retained(SEP_BASE + 0x200000) == 0x100000);
    /* Anything else inside the range is refused, and the boot goes on: a mapped asc-mem region, */
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            reserve("asc-firmware@10300000", SEP_BASE + 0x300000, 0x40000, false, "apple,asc-mem"));
    /* a no-map region of another kind, */
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            reserve("other@10300000", SEP_BASE + 0x300000, 0x40000, true, "vendor,other"));
    /* a mapped region that carries the marker, */
    REFUSED(tree("/arm-io/sep");
            assert(!fdt_setprop_empty(dt, reserve("dcp-oslog@10100000", SEP_BASE + 0x100000,
                                                  0x100000, false, NULL), "apple,dcp-os-log")));
    /* or two OS logs that overlap each other. */
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            oslog("dcp-oslog@10180000", SEP_BASE + 0x180000, 0x100000));

    /* A SEP node that does not take sep-firmware as sepfw, or one that is not apple,sep, or a
     * sep-firmware the SEP cannot read (no-map): refused before any change. */
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            assert(!fdt_setprop(dt, fdt_path_offset(dt, "/arm-io/sep"), "memory-region-names",
                                "other", 6)));
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "/arm-io/sep"), "compatible",
                                       "apple,other")));
    REFUSED(tree("/arm-io/sep"); oslog("dcp-oslog@10100000", SEP_BASE + 0x100000, 0x100000);
            assert(!fdt_setprop_empty(dt, reserved("sep-firmware"), "no-map")));

    /* No SEP reservation at all. */
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    memcpy(before, blob, sizeof(blob));
    assert(!prepare());
    expect_unchanged();
    return 0;
}
''')
