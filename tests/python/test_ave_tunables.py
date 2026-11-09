# SPDX-License-Identifier: MIT
"""Test AVE ADT tunable handoff with synthetic values and a real FDT."""

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


def test_ave_tunable_handoff(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    assert "dt_reserve_asc_firmware(adt_path, NULL, path, use_remap, dva_base)" in source
    structs = source[
        source.index("struct adt_tunable_info {"):
        source.index("static const struct adt_tunable_info usb4_nhi_tunables")
    ]
    acio = source[
        source.index("struct acio_tunable {"):
        source.index("static_assert(sizeof(struct acio_tunable)")
    ]
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
static void *dt, *adt;
''' + structs + acio + r'''

static struct acio_tunable dart0[] = {
    { .offset = 0x60, .size = 4, .mask = 0xff, .value = 0x21 },
};
static struct acio_tunable dart1[] = {
    { .offset = 0x68, .size = 4, .mask = 0xffff, .value = 0x4321 },
    { .offset = 0x6c, .size = 4, .mask = 0xff, .value = 0x44 },
};

/* Synthetic register windows, listed in the opposite order to the DT. */
static u64 adt_regs[] = { 0x200030000ULL, 0x200040000ULL };

static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    if (!strcmp(path, "/arm-io/dart-ave"))
        return -1;
    if (!strcmp(path, "/arm-io/dart-ave0")) {
        trace[0] = 1;
        return 1;
    }
    return -1;
}

static int adt_get_reg(void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)tree; (void)prop; (void)size;
    if (path[0] != 1 || idx < 0 || idx >= (int)ARRAY_SIZE(adt_regs))
        return -1;
    *addr = adt_regs[idx];
    return 0;
}

static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    const void *value = NULL;
    u32 size = 0;
    if (node == 1 && !strcmp(name, "dart-tunables-instance-0")) {
        value = dart0; size = sizeof(dart0);
    } else if (node == 1 && !strcmp(name, "dart-tunables-instance-1")) {
        value = dart1; size = sizeof(dart1);
    }
    if (value && len)
        *len = size;
    return value;
}
''' + extract(source, "static int dt_append_acio_tunable(") + "\n" + extract(
        source, "static int dt_ave_dart_instance("
    ) + "\n" + extract(
        source, "static int dt_copy_ave_tunables("
    ) + r'''

static int node(const char *path)
{
    int n = fdt_path_offset(dt, path);
    assert(n >= 0);
    return n;
}

static void reset(void)
{
    static unsigned char blob[16384];
    fdt32_t refs[] = {
        cpu_to_fdt32(10), cpu_to_fdt32(0),
        cpu_to_fdt32(11), cpu_to_fdt32(0),
    };
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(fdt_add_subnode(dt, 0, "dart1") >= 0);
    assert(fdt_add_subnode(dt, 0, "dart0") >= 0);
    assert(fdt_add_subnode(dt, 0, "ave") >= 0);
    assert(!fdt_setprop_string(dt, node("/dart0"), "compatible", "apple,t6000-dart"));
    assert(!fdt_setprop_string(dt, node("/dart1"), "compatible", "apple,t8110-dart"));
    assert(!fdt_setprop_u32(dt, node("/dart0"), "phandle", 10));
    assert(!fdt_setprop_u32(dt, node("/dart1"), "phandle", 11));
    fdt32_t reg0[] = { cpu_to_fdt32(2), cpu_to_fdt32(0x40000), cpu_to_fdt32(0), cpu_to_fdt32(0x4000) };
    fdt32_t reg1[] = { cpu_to_fdt32(2), cpu_to_fdt32(0x30000), cpu_to_fdt32(0), cpu_to_fdt32(0x4000) };
    assert(!fdt_setprop(dt, node("/dart0"), "reg", reg0, sizeof(reg0)));
    assert(!fdt_setprop(dt, node("/dart1"), "reg", reg1, sizeof(reg1)));
    assert(!fdt_setprop(dt, node("/ave"), "iommus", refs, sizeof(refs)));
    assert(!fdt_setprop_string(dt, node("/ave"), "compatible", "apple,t8112-ave"));
    dart1[1].size = 4;
}

int main(void)
{
    int len;
    reset();
    assert(!dt_copy_ave_tunables("/ave"));
    /* Joined by register window: DT dart0 is ADT reg[1], dart1 is reg[0]. */
    const fdt32_t *out = fdt_getprop(dt, node("/dart1"), "apple,tunable", &len);
    assert(out && len == 12);
    assert(fdt32_to_cpu(out[0]) == 0x60 && fdt32_to_cpu(out[2]) == 0x21);
    out = fdt_getprop(dt, node("/dart0"), "apple,tunable", &len);
    assert(out && len == 24 && fdt32_to_cpu(out[0]) == 0x68);
    const void *raw = fdt_getprop(dt, node("/ave"), "apple,tunable-axi2af", &len);
    assert(raw && len == 0);

    /* Existing board data is retained instead of being appended to. */
    reset();
    const fdt32_t board[] = {cpu_to_fdt32(4), cpu_to_fdt32(1), cpu_to_fdt32(1)};
    assert(!fdt_setprop(dt, node("/dart0"), "apple,tunable", board, sizeof(board)));
    assert(!dt_copy_ave_tunables("/ave"));
    out = fdt_getprop(dt, node("/dart0"), "apple,tunable", &len);
    assert(out && len == (int)sizeof(board) && fdt32_to_cpu(out[0]) == 4);

    /* A DART whose window is not in the ADT reg list is refused, with rollback. */
    reset();
    fdt32_t stray[] = { cpu_to_fdt32(2), cpu_to_fdt32(0x50000), cpu_to_fdt32(0), cpu_to_fdt32(0x4000) };
    assert(!fdt_setprop(dt, node("/dart1"), "reg", stray, sizeof(stray)));
    assert(dt_copy_ave_tunables("/ave") < 0);
    assert(!fdt_getprop(dt, node("/dart0"), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, node("/dart1"), "apple,tunable", NULL));

    /* No T6000 AXI2AF rows are inferred from the unrelated PMGR array. */
    reset();
    assert(!fdt_setprop_string(dt, node("/ave"), "compatible", "apple,t6000-ave"));
    assert(!dt_copy_ave_tunables("/ave"));
    assert(!fdt_getprop(dt, node("/ave"), "apple,tunable-axi2af", NULL));

    /* A malformed later list rolls back every property this call wrote. */
    reset();
    dart1[1].size = 8;
    assert(dt_copy_ave_tunables("/ave") < 0);
    assert(!fdt_getprop(dt, node("/dart0"), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, node("/dart1"), "apple,tunable", NULL));
    assert(!fdt_getprop(dt, node("/ave"), "apple,tunable-axi2af", NULL));
    puts("PASS: AVE DART handoff and explicit T8112 AXI2AF no-op");
    return 0;
}
'''
    harness = tmp_path / "ave-tunables.c"
    harness.write_text(code)
    binary = tmp_path / "ave-tunables"
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
