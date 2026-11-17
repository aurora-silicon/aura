# SPDX-License-Identifier: MIT
"""Run kboot_gpu.c's T6041 GPU fill on real FDTs with a modelled ADT.

The accepted case uses the operating points, CS/AFR tables and regions of one J616C (32-core
M4 Max) ADT. The test checks what the fill publishes, what it refuses, and that nothing stays
published when it refuses. The fill never touches the GPU, so no register access is modelled.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8;
typedef uint32_t u32;
typedef int32_t s32;
/* Match aura's aarch64-linux-gnu target, not the host libc's uint64_t alias. */
typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define SZ_16K 0x4000

static void *adt = (void *)1;
static unsigned char blob[65536];
static void *dt = blob;
struct fw_version_info { const char *string; u32 num[3]; size_t num_length; };
static struct fw_version_info os_firmware = {"26.6.2", {26, 6, 2}, 3};
static int firmware_set_fdt(void *tree, int node, const char *prop,
                            const struct fw_version_info *version)
{
    fdt32_t cells[3];
    for (size_t i = 0; i < version->num_length; i++)
        cells[i] = cpu_to_fdt32(version->num[i]);
    return fdt_setprop(tree, node, prop, cells, version->num_length * sizeof(u32));
}
'''

ADT = r'''
static struct perf_state states[64];
static struct {
    u64 dies, count;
    struct aux_perf_state states[8];
} cs = {.dies = 1, .count = 8}, afr = {.dies = 1, .count = 8};
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; };
static struct adt_segment_ranges segments[2];
static u64 region[6][2];
static u32 raw_count, raw_tables, raw_max_state;
static bool missing_sgx, missing_asc;
static const char *const region_names[5] = {
    "gfx-handoff", "gfx-shared-l2-region", "gfx-shared-region", "gpu-region", "gfx-data",
};

static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/arm-io/sgx"))
        return missing_sgx ? -1 : 1;
    if (!strcmp(path, "/arm-io/gfx-asc"))
        return missing_asc ? -1 : 2;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if (node == 2 && !strcmp(name, "segment-ranges")) { *len = sizeof(segments); return segments; }
    if (node != 1) return NULL;
    if (!strcmp(name, "perf-states")) { *len = raw_count * raw_tables * sizeof(states[0]); return states; }
    if (!strcmp(name, "cs-perf-states")) { *len = sizeof(cs); return &cs; }
    if (!strcmp(name, "afr-perf-states")) { *len = sizeof(afr); return &afr; }
    return NULL;
}
static int adt_copy(const char *name, void *out, size_t size)
{
    u32 value;
    if (!strcmp(name, "perf-state-count")) value = raw_count;
    else if (!strcmp(name, "perf-state-table-count")) value = raw_tables;
    else if (!strcmp(name, "gpu-num-perf-states")) value = raw_max_state;
    else {
        for (int i = 0; i < 5; i++) {
            size_t n = strlen(region_names[i]);
            if (!strncmp(name, region_names[i], n) && size == sizeof(u64)) {
                memcpy(out, &region[i][strcmp(name + n, "-base") ? 1 : 0], sizeof(u64));
                return sizeof(u64);
            }
        }
        return -1;
    }
    if (size != sizeof(u32)) return -1;
    memcpy(out, &value, sizeof(value));
    return sizeof(value);
}
#define ADT_GETPROP(a, n, name, ptr) adt_copy(name, (ptr), sizeof(*(ptr)))
'''

MAIN = r'''
/* One J616C (M4 Max, 32 GPU cores), 26.6.2 boot firmware: MHz and mV per cluster. */
static const u32 j616c_mhz[16] = {0, 338, 618, 796, 924, 952, 1056, 1062,
                                  1182, 1182, 1312, 1242, 1380, 1326, 1470, 1578};
static const u32 j616c_mv[4][16] = {
    {125, 600, 650, 695, 720, 770, 770, 815, 815, 870, 870, 910, 910, 950, 950, 1030},
    {125, 595, 645, 690, 720, 760, 760, 805, 805, 850, 850, 890, 890, 930, 930, 1025},
    {125, 615, 660, 705, 725, 775, 775, 805, 805, 855, 855, 895, 895, 935, 935, 1030},
    {125, 600, 660, 700, 715, 770, 770, 805, 805, 860, 860, 900, 900, 940, 940, 1020},
};
static const u32 j616c_aux_mhz[8] = {24, 338, 618, 796, 992, 1188, 1380, 1470};
static const u32 j616c_aux_uv[8] = {585000, 585000, 630000, 680000,
                                    720000, 770000, 865000, 900000};
static const char *const region_paths[6] = {
    "/reserved-memory/uat-handoff", "/reserved-memory/uat-l2-pagetables",
    "/reserved-memory/uat-pagetables", "/reserved-memory/uat-ttbs",
    "/reserved-memory/gpu-fw-data", "/reserved-memory/gpu-fw-text",
};

static void add_opp_table(const char *name, u32 phandle, int count, int volts)
{
    char path[32];
    snprintf(path, sizeof(path), "/%s", name);
    int table = fdt_add_subnode(dt, 0, name);
    assert(table >= 0);
    assert(!fdt_setprop_u32(dt, table, "phandle", phandle));
    /* A new subnode goes before the existing ones, so add them from the last. */
    for (int i = count - 1; i >= 0; i--) {
        char node[16];
        snprintf(node, sizeof(node), "opp%02d", i);
        int opp = fdt_add_subnode(dt, fdt_path_offset(dt, path), node);
        assert(opp >= 0);
        assert(!fdt_setprop_u64(dt, opp, "opp-hz", 0));
        fdt32_t zero[4] = {0, 0, 0, 0};
        assert(!fdt_setprop(dt, opp, "opp-microvolt", zero, volts * sizeof(u32)));
    }
}

static void world_with(const char *compatible, int gpu_opps, int gpu_cells, int aux_opps)
{
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    add_opp_table("gpu-opp", 10, gpu_opps, gpu_cells);
    add_opp_table("cs-opp", 11, aux_opps, 1);
    add_opp_table("afr-opp", 12, aux_opps, 1);
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(resv >= 0);
    for (int i = 0; i < 6; i++) {
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"),
                                   region_paths[i] + strlen("/reserved-memory/"));
        assert(node >= 0 && !fdt_setprop_string(dt, node, "status", "disabled"));
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    int gpu = fdt_add_subnode(dt, soc, "gpu");
    assert(gpu >= 0 && !fdt_setprop_string(dt, gpu, "compatible", compatible));
    assert(!fdt_setprop_string(dt, gpu, "status", "okay"));
    assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 10));
    assert(!fdt_setprop_u32(dt, gpu, "apple,cs-opp", 11));
    assert(!fdt_setprop_u32(dt, gpu, "apple,afr-opp", 12));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu"));

    for (int t = 0; t < 4; t++)
        for (int i = 0; i < 16; i++)
            states[t * 16 + i] = (struct perf_state){j616c_mhz[i] * 1000000u, j616c_mv[t][i]};
    for (int i = 0; i < 8; i++) {
        cs.states[i] = (struct aux_perf_state){j616c_aux_uv[i], j616c_aux_mhz[i] * 1000000UL};
        afr.states[i] = cs.states[i];
    }
    cs.dies = afr.dies = 1;
    cs.count = afr.count = 8;
    /* gfx-handoff, gfx-shared-l2-region, gfx-shared-region, gpu-region, gfx-data, TEXT */
    const u64 j616c_regions[6][2] = {
        {0x108fff70000UL, 0x4000},  {0x108fff74000UL, 0x4000},  {0x108fff78000UL, 0x40000},
        {0x108fffb8000UL, 0x4000},  {0x10001fc4000UL, 0x138000}, {0x10000d30000UL, 0x60000},
    };
    memcpy(region, j616c_regions, sizeof(region));
    segments[0] = (struct adt_segment_ranges){.phys = region[5][0], .iova = 0xfffffc0000000000UL,
                                              .remap = region[5][0], .size = region[5][1], .unk = 1};
    segments[1] = (struct adt_segment_ranges){.phys = region[4][0],
                                              .iova = 0xfffffc0000000000UL + region[5][1],
                                              .remap = region[4][0], .size = region[4][1]};
    raw_count = 16; raw_tables = 4; raw_max_state = 15;
    missing_sgx = missing_asc = false;
}

static void world(void) { world_with("apple,agx-t6041", 16, 4, 8); }

static const char *status(const char *path)
{
    int node = fdt_path_offset(dt, path);
    assert(node >= 0);
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s ? s : "okay";
}

static void expect_refused(const char *what)
{
    assert(dt_set_gpu_t6041(dt) == 0);
    if (strcmp(status("gpu"), "disabled")) {
        fprintf(stderr, "%s: GPU was not disabled\n", what);
        exit(1);
    }
    for (int i = 0; i < 6; i++)
        if (strcmp(status(region_paths[i]), "disabled")) {
            fprintf(stderr, "%s: %s left published\n", what, region_paths[i]);
            exit(1);
        }
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,firmware-version", NULL));
    printf("PASS refused: %s\n", what);
}

static void expect_untouched(const char *what, const char *gpu_status)
{
    assert(dt_set_gpu_t6041(dt) == 0);
    assert(!strcmp(status("gpu"), gpu_status));
    for (int i = 0; i < 6; i++)
        assert(!strcmp(status(region_paths[i]), "disabled"));
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,firmware-version", NULL));
    printf("PASS untouched: %s\n", what);
}

int main(void)
{
    /* The accepted case publishes the unit's tables and the boot's regions. */
    world();
    assert(dt_set_gpu_t6041(dt) == 0);
    assert(!strcmp(status("gpu"), "okay"));
    for (int i = 0; i < 6; i++) {
        assert(!strcmp(status(region_paths[i]), "okay"));
        int len;
        const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, region_paths[i]), "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == region[i][0] &&
               fdt64_ld(&reg[1]) == region[i][1]);
    }
    int table = fdt_path_offset(dt, "/gpu-opp"), opp, n = 0, len;
    fdt_for_each_subnode(opp, dt, table)
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == 16 && hz && fdt64_ld(hz) == j616c_mhz[n] * 1000000ULL);
        for (int c = 0; c < 4; c++)
            assert(fdt32_ld(&uv[c]) == j616c_mv[c][n] * 1000);
        n++;
    }
    assert(n == 16);
    n = 0;
    fdt_for_each_subnode(opp, dt, fdt_path_offset(dt, "/cs-opp"))
    {
        const fdt32_t *uv = fdt_getprop(dt, opp, "opp-microvolt", &len);
        const fdt64_t *hz = fdt_getprop(dt, opp, "opp-hz", NULL);
        assert(uv && len == 4 && fdt32_ld(uv) == j616c_aux_uv[n] &&
               fdt64_ld(hz) == j616c_aux_mhz[n] * 1000000ULL);
        n++;
    }
    assert(n == 8);
    const fdt32_t *version =
        fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,firmware-version", &len);
    assert(version && len == 12 && fdt32_ld(&version[0]) == 26 && fdt32_ld(&version[1]) == 6 &&
           fdt32_ld(&version[2]) == 2);
    puts("PASS accepted: J616C tables and regions");

    world_with("apple,agx-t6040", 16, 2, 8);
    expect_untouched("T6040 GPU node", "okay");
    world(); assert(!fdt_setprop_string(dt, fdt_path_offset(dt, "gpu"), "status", "disabled"));
    expect_untouched("GPU disabled in the device tree", "disabled");

    world_with("apple,agx-t6041", 16, 2, 8);
    expect_refused("two-cell OPP table");
    world_with("apple,agx-t6041", 15, 4, 8);
    expect_refused("OPP table one entry short");
    world_with("apple,agx-t6041", 16, 4, 6);
    expect_refused("CS/AFR tables shorter than the ADT's");
    world(); raw_tables = 2;
    expect_refused("two cluster tables");
    world(); raw_count = 15;
    expect_refused("wrong raw OPP count");
    world(); raw_max_state = 10;
    expect_refused("logical count in the raw ADT count");
    world(); missing_sgx = true;
    expect_refused("missing SGX");
    world(); missing_asc = true;
    expect_refused("missing GFX ASC");
    world(); states[0].freq = 100000000;
    expect_refused("off operating point has a clock");
    world(); states[2 * 16 + 5].freq += 1000000;
    expect_refused("a cluster runs a different clock");
    world(); states[3 * 16 + 9].volt = 1400;
    expect_refused("voltage out of range");
    world(); cs.dies = 2;
    expect_refused("two-die CS table");
    world(); afr.states[3].freq = 0;
    expect_refused("AFR state without a clock");
    world(); cs.count = 9;
    expect_refused("CS count beyond the property");
    world(); segments[1].phys += 0x4000;
    expect_refused("DATA segment disagrees with gfx-data");
    world(); segments[0].phys = region[0][0];
    expect_refused("TEXT overlaps handoff");
    world(); region[4][1] = 0x4100;
    expect_refused("unaligned region");
    world(); region[2][0] = region[1][0];
    expect_refused("overlapping regions");
    world(); region[0][0] = 1UL << 42;
    expect_refused("region outside physical address width");
    world(); region[3][0] = ~0UL & ~(SZ_16K - 1);
    expect_refused("region wraps");
    world(); assert(!fdt_del_node(dt, fdt_path_offset(dt, "/reserved-memory/gpu-fw-data")));
    assert(dt_set_gpu_t6041(dt) == 0 && !strcmp(status("gpu"), "disabled"));
    for (int i = 0; i < 4; i++)
        assert(!strcmp(status(region_paths[i]), "disabled"));
    puts("PASS refused: missing reserved-memory node");
    return 0;
}
'''


def test_t6041_gpu_fill(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    defs = source[source.index("#define MAX_PSTATES"):source.index("int32_t rust_gpu_initdata_size(")]
    t6040 = source[source.index("/*\n * T6040 (M4 Pro)."):]
    opps = t6040[t6040.index("#define T6040_OPPS"):].split("\n", 1)[0] + "\n"
    regions = t6040[t6040.index("static const struct {\n    const char *name;"):]
    regions = regions[:regions.index("};\n") + 3]
    clusters = next(line for line in source.splitlines()
                    if line.startswith("#define T6041_CLUSTERS ")) + "\n"
    t6041 = clusters + function(source, "static int dt_set_gpu_t6041(")
    bail = '#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)\n'
    code = (HARNESS + bail + defs + ADT + opps + function(source, "static int dt_set_resvmem(") +
            function(source, "static int fdt_set_aux_opp(") +
            function(source, "static int t6040_opp_table(") + regions + t6041 + MAIN)
    cfile = tmp_path / "t6041-gpu-fill.c"
    cfile.write_text(code)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "t6041-gpu-fill"
    subprocess.run([*host_cc(), "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(libfdt),
                    str(cfile), *sources, "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 24, result.stdout
