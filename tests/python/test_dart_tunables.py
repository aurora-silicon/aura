# SPDX-License-Identifier: MIT
"""Exercise the generic ADT-to-DT DART tunable join with synthetic data."""

from pathlib import Path
import subprocess


def extract(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_dart_tunables_map_by_physical_address(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    structs = source[
        source.index("struct adt_tunable_info {"):
        source.index("static const struct adt_tunable_info usb4_nhi_tunables")
    ]
    acio = source[
        source.index("struct acio_tunable {"):
        source.index("static_assert(sizeof(struct acio_tunable)")
    ]
    functions = "\n".join(
        extract(source, signature)
        for signature in (
            "static int dt_append_dart_tunable(",
            "static bool dt_node_is_apple_dart(",
            "static int dt_find_dart_by_address(",
            "static int dt_copy_dart_tunable(",
            "static void dt_copy_dart_tunables(",
        )
    )
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef unsigned long u64;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ADT_FOREACH_CHILD(tree, node) \
    for (int _count = adt_get_child_count(tree, node); _count; _count = 0) \
        for (node = adt_first_child_offset(tree, node); _count--; \
             node = adt_next_sibling_offset(tree, node))
static void *dt, *adt;
''' + structs + acio + r'''

/* Public node names and instance counts model the four lab ADT shapes. The
 * union is exactly 37 tunable-bearing DART nodes. Addresses and rows are wholly
 * synthetic; filter-data-* and smmu-tunables-* are intentionally not modeled.
 * Neo's one structure-only slot reuses a public union name solely to exercise
 * a non-empty fourth shape; it does not assert the live node's identity.
 */
struct fake_dart {
    const char *name;
    unsigned instances[4]; /* M1 Pro, M2, J616s, Neo */
};
static const struct fake_dart darts[] = {
    {"dart-isp0",     {3, 0, 3, 0}}, {"dart-apcie0",    {1, 1, 1, 1}},
    {"dart-apcie1",   {1, 0, 1, 0}}, {"dart-apciec0",   {1, 0, 1, 0}},
    {"dart-acio0",    {1, 0, 1, 0}}, {"dart-apciec1",   {1, 0, 1, 0}},
    {"dart-acio1",    {1, 0, 1, 0}}, {"dart-apciec2",   {1, 0, 1, 0}},
    {"dart-acio2",    {1, 0, 1, 0}}, {"dart-aop",       {1, 1, 1, 0}},
    {"dart-sio",      {2, 1, 1, 0}}, {"dart-pmp",       {1, 1, 1, 0}},
    {"dart-sep",      {1, 1, 1, 0}}, {"dart-usb0",      {2, 2, 2, 0}},
    {"dart-usb1",     {2, 2, 2, 0}}, {"dart-usb2",      {2, 0, 2, 0}},
    {"dart-disp0",    {1, 1, 1, 0}}, {"dart-dcp",       {1, 1, 1, 0}},
    {"dart-scaler0",  {1, 0, 1, 0}}, {"dart-apr0",      {1, 0, 1, 0}},
    {"dart-jpeg0",    {1, 1, 1, 0}}, {"dart-jpeg1",     {1, 1, 1, 0}},
    {"dart-ave0",     {2, 0, 2, 0}}, {"dart-avd0",      {1, 0, 1, 0}},
    {"dart-ane0",     {3, 0, 3, 0}}, {"dart-mtp",       {0, 1, 1, 0}},
    {"dart-scaler",   {0, 1, 0, 0}}, {"dart-ave",       {0, 2, 0, 0}},
    {"dart-avd",      {0, 1, 0, 0}}, {"dart-apr",       {0, 2, 0, 0}},
    {"dart-scdc",     {0, 1, 0, 0}}, {"dart-isp",       {0, 3, 0, 0}},
    {"dart-ane",      {0, 3, 0, 0}}, {"dart-dcpext0",   {0, 0, 1, 0}},
    {"dart-dispext0", {0, 0, 1, 0}}, {"dart-dcpext1",   {0, 0, 1, 0}},
    {"dart-dispext1", {0, 0, 1, 0}},
};
_Static_assert(ARRAY_SIZE(darts) == 37, "lab DART union changed");
static int shape, malformed_node = -1, no_table_node = -1, bad_value_node = -1;
static struct acio_tunable row = {
    .offset = 0x20, .size = 4, .mask = 0x00ff00ff, .value = 0x00550011,
};
static struct acio_tunable bad_row = {
    .offset = 0x24, .size = 4, .mask = 0x0000000f, .value = 0x00000010,
};

static const struct fake_dart *fake_at(int ordinal, int *index)
{
    for (size_t i = 0; i < ARRAY_SIZE(darts); i++) {
        if (!darts[i].instances[shape])
            continue;
        if (!ordinal--) {
            if (index)
                *index = i;
            return &darts[i];
        }
    }
    return NULL;
}
static u64 fake_base(int index, unsigned instance)
{
    return 0x10000000UL * (shape + 1) + (u64)index * 0x10000 + instance * 0x4000;
}

static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    assert(!strcmp(path, "/arm-io"));
    trace[0] = 100;
    trace[1] = 0;
    return 100;
}
static int adt_get_child_count(void *tree, int node)
{
    (void)tree;
    assert(node == 100);
    int count = 0;
    for (size_t i = 0; i < ARRAY_SIZE(darts); i++)
        count += !!darts[i].instances[shape];
    return count;
}
static int adt_first_child_offset(void *tree, int node)
{
    (void)tree; (void)node;
    return 1;
}
static int adt_next_sibling_offset(void *tree, int node)
{
    (void)tree;
    return node + 1;
}
static const char *adt_get_name(void *tree, int node)
{
    (void)tree;
    const struct fake_dart *dart = fake_at(node - 1, NULL);
    return dart ? dart->name : NULL;
}
static int adt_get_reg(void *tree, int *path, const char *prop, int instance,
                       u64 *addr, u64 *size)
{
    (void)tree;
    assert(path[0] == 100 && path[1] > 0 && !strcmp(prop, "reg"));
    int index;
    const struct fake_dart *dart = fake_at(path[1] - 1, &index);
    if (!dart || instance < 0 || (unsigned)instance >= dart->instances[shape])
        return -1;
    *addr = fake_base(index, instance);
    if (size)
        *size = 0x4000;
    return 0;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    const char prefix[] = "dart-tunables-instance-";
    if (strncmp(name, prefix, sizeof(prefix) - 1))
        return NULL;
    unsigned instance = name[sizeof(prefix) - 1] - '0';
    const struct fake_dart *dart = fake_at(node - 1, NULL);
    if (!dart || name[sizeof(prefix)] || instance >= dart->instances[shape])
        return NULL;
    if (node - 1 == no_table_node)
        return NULL;
    if (len)
        *len = node - 1 == malformed_node ? sizeof(row) - 1 : sizeof(row);
    return node - 1 == bad_value_node ? &bad_row : &row;
}
static u64 dt_get_address(void *tree, int node)
{
    const fdt64_t *reg = fdt_getprop(tree, node, "reg", NULL);
    return reg ? fdt64_to_cpu(*reg) : 0;
}
''' + extract(source, "static int dt_append_acio_tunable(") + "\n" + functions + r'''

static int path(const char *name)
{
    int node = fdt_path_offset(dt, name);
    assert(node >= 0);
    return node;
}
static int dart_node(int index, unsigned instance)
{
    char name[40];
    snprintf(name, sizeof(name), "/dart@%llx",
             (unsigned long long)fake_base(index, instance));
    return path(name);
}
static void add_dart(const char *name, u64 base, const char *status)
{
    int node = fdt_add_subnode(dt, 0, name);
    assert(node >= 0);
    fdt64_t reg[] = {cpu_to_fdt64(base), cpu_to_fdt64(0x4000)};
    assert(!fdt_setprop(dt, node, "reg", reg, sizeof(reg)));
    assert(!fdt_setprop_string(dt, node, "compatible", "apple,t8110-dart"));
    if (status)
        assert(!fdt_setprop_string(dt, node, "status", status));
}
static void reset(int which)
{
    static unsigned char blob[32768];
    dt = blob;
    shape = which;
    malformed_node = -1;
    no_table_node = -1;
    bad_value_node = -1;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));

    /* Reverse both node and instance order: phandle/FDT order must be irrelevant. */
    int count = adt_get_child_count(adt, 100);
    for (int i = count - 1; i >= 0; i--)
        for (int j = fake_at(i, NULL)->instances[shape] - 1; j >= 0; j--) {
            char name[40];
            int index;
            fake_at(i, &index);
            snprintf(name, sizeof(name), "dart@%llx",
                     (unsigned long long)fake_base(index, j));
            add_dart(name, fake_base(index, j), NULL);
        }
}
static void verify_all(void)
{
    int count = adt_get_child_count(adt, 100);
    for (int i = 0; i < count; i++) {
        int index;
        const struct fake_dart *dart = fake_at(i, &index);
        for (unsigned j = 0; j < dart->instances[shape]; j++) {
            char name[40];
            int len;
            snprintf(name, sizeof(name), "/dart@%llx",
                     (unsigned long long)fake_base(index, j));
            const fdt32_t *out = fdt_getprop(dt, path(name), "apple,tunable", &len);
            assert(out && len == 12);
            assert(fdt32_to_cpu(out[0]) == 0x20);
            assert(fdt32_to_cpu(out[1]) == 0x00ff00ff);
            assert(fdt32_to_cpu(out[2]) == 0x00550011);
        }
    }
}
int main(void)
{
    for (int i = 0; i < 4; i++) {
        reset(i);
        dt_copy_dart_tunables();
        verify_all();
    }

    reset(0);
    int alpha0 = dart_node(0, 0);
    int beta = dart_node(1, 0);
    const fdt32_t board[] = {cpu_to_fdt32(4), cpu_to_fdt32(1), cpu_to_fdt32(1)};
    assert(!fdt_setprop(dt, beta, "apple,tunable", board, sizeof(board)));
    assert(!fdt_setprop_string(dt, alpha0, "status", "disabled"));
    dt_copy_dart_tunables();
    assert(!fdt_getprop(dt, dart_node(0, 0), "apple,tunable", NULL));
    assert(fdt_getprop(dt, dart_node(0, 1), "apple,tunable", NULL));
    int len;
    const fdt32_t *out = fdt_getprop(dt, dart_node(1, 0), "apple,tunable", &len);
    assert(out && len == sizeof(board) && fdt32_to_cpu(out[0]) == 4);

    /* A malformed node is rolled back without losing a different node's table. */
    reset(0);
    malformed_node = 1;
    dt_copy_dart_tunables();
    assert(fdt_getprop(dt, dart_node(0, 0), "apple,tunable", NULL));
    assert(fdt_getprop(dt, dart_node(0, 1), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, dart_node(1, 0), "apple,tunable", NULL));

    /* A value outside its field mask is rejected; later DARTs still copy. */
    reset(0);
    bad_value_node = 0;
    dt_copy_dart_tunables();
    assert(!fdt_getprop(dt, dart_node(0, 0), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, dart_node(0, 1), "apple,tunable", NULL));
    assert(fdt_getprop(dt, dart_node(1, 0), "apple,tunable", NULL));

    /* Duplicate physical addresses are ambiguous and are never matched by order. */
    reset(0);
    u64 duplicate = fake_base(0, 0);
    add_dart("duplicate", duplicate, NULL);
    dt_copy_dart_tunables();
    assert(!fdt_getprop(dt, dart_node(0, 0), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, path("/duplicate"), "apple,tunable", NULL));

    /* Keep the established PCIe-C empty-table marker when ADT has no rows. */
    reset(0);
    no_table_node = 3; /* dart-apciec0 in the M1 Pro shape */
    dt_copy_dart_tunables();
    out = fdt_getprop(dt, dart_node(3, 0), "apple,tunable", &len);
    assert(out && len == 0);

    puts("PASS: all synthetic DART shapes joined only by physical address");
    return 0;
}
'''
    harness = tmp_path / "dart-tunables.c"
    harness.write_text(code)
    binary = tmp_path / "dart-tunables"
    lib = repo / "src/libfdt"
    files = [
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c",
        "fdt_empty_tree.c", "fdt_addresses.c",
    ]
    subprocess.run([
        "cc", "-std=gnu11", "-Wall", "-Werror", "-Wno-sign-compare",
        "-I", str(lib), str(harness), *[str(lib / f) for f in files],
        "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
