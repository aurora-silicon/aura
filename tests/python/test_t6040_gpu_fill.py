# SPDX-License-Identifier: MIT
"""Run kboot_gpu.c's T6040 GPU fill on real FDTs with a modelled ADT, efuse page and SGX.

The ADT values here are synthetic. The test checks what the fill accepts and refuses, that SGX
power is put back after the register reads, and that nothing is published when it refuses. The public reference case also checks the power model's maximum-power components.
"""

from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2] + "\n"


HARNESS = r'''
#include <assert.h>
#include <math.h>
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
#define MASK(x) ((1UL << (x)) - 1)
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define sysop(op) ((void)0)
enum { GUARD_OFF = 0, GUARD_MARK = 2, GUARD_SILENT = 0x100 };
static int exc_guard, exc_count;
static void *adt = (void *)1;
static unsigned char blob[65536];
static void *dt = blob;
struct fw_version_info { const char *string; };
static struct fw_version_info os_firmware = {"26.6.2"};
static int firmware_set_fdt(void *tree, int node, const char *prop,
                            const struct fw_version_info *version)
{
    return fdt_setprop_string(tree, node, prop, version->string);
}

/* SGX power: the fill must power it only around the register reads and restore it after. */
struct pmgr_saved_modes { int count; };
static bool sgx_on;
static int enables, restores;
static int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved)
{
    assert(!strcmp(path, "/arm-io/sgx"));
    enables++;
    sgx_on = true;
    saved->count = 1;
    return 0;
}
static void pmgr_restore_modes(struct pmgr_saved_modes *saved)
{
    assert(saved->count == 1);
    restores++;
    sgx_on = false;
}
'''

ADT = r'''
#define SGX_BASE 0x400000000UL
#define EFUSE    0x508374000UL
static struct perf_state states[32];
static struct {
    u64 dies, count;
    struct aux_perf_state states[6];
} cs = {.dies = 1, .count = 6}, afr = {.dies = 1, .count = 4};
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, unk; };
static struct adt_segment_ranges segments[2];
static u64 region[6][2];
static u32 base_pstate = 1;
static u32 base_length = sizeof(u32);
static u32 raw_count = 16, raw_tables = 2, raw_max_state = 15;
static bool have_base_pstate, efuse_fault, sgx_fault, missing_asc;
static u32 map_a[16], map_b[16];
static const char *const region_names[5] = {
    "gfx-handoff", "gfx-shared-l2-region", "gfx-shared-region", "gpu-region", "gfx-data",
};

static int adt_path_offset(void *tree, const char *path) { (void)tree; return !missing_asc && !strcmp(path, "/arm-io/gfx-asc") ? 2 : -1; }

static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    trace[0] = 1;
    return strcmp(path, "/arm-io/sgx") ? -1 : 1;
}
static int adt_get_reg(void *tree, int *path, const char *prop, int index, u64 *base, u64 *size)
{
    (void)tree; (void)path; (void)prop; (void)index; (void)size;
    *base = SGX_BASE;
    return 0;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node;
    if (!strcmp(name, "gpu-perf-base-pstate") && have_base_pstate) { *len = base_length; return &base_pstate; }
    if (!strcmp(name, "segment-ranges")) { *len = sizeof(segments); return segments; }
    if (!strcmp(name, "perf-states")) { *len = sizeof(states); return states; }
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
    else if (!strcmp(name, "gpu-perf-base-pstate") && have_base_pstate) value = base_pstate;
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

static u32 read32(u64 addr)
{
    if (addr >= EFUSE && addr < EFUSE + 0x1000) {
        if (efuse_fault) exc_count++;
        switch (addr - EFUSE) {
        case 0xc10: return (1280u << 1) | (1295u << 15);
        case 0xc1c: return 250u << 9;
        case 0xc20: return 80u << 24;
        case 0xc24: return 0;
        case 0xc28: return 252u << 4;
        }
        return 0;
    }
    /* An SGX register read with the GPU off faults. */
    if (!sgx_on || sgx_fault) {
        exc_count++;
        return 0;
    }
    if (addr == SGX_BASE + 0xe01500) return 0xfffff;
    if (addr == SGX_BASE + 0xe01504 || addr == SGX_BASE + 0xe01508) return 0;
    for (int bank = 0; bank < 2; bank++) {
        u64 base = SGX_BASE + (bank ? 0xe01480 : 0xe01400);
        const u32 *map = bank ? map_b : map_a;
        if (addr >= base && addr < base + 32 && !(addr & 3)) {
            u32 i = (addr - base) / 2;
            return map[i] | (map[i + 1] << 16);
        }
    }
    exc_count++;
    return 0;
}
'''

MAIN = r'''
static const u32 good_a[16] = {0, 1, 2, 3, 4, 6, 8, 10, 12, 14, 15};
static const u32 good_b[16] = {0, 1, 2, 3, 4, 6, 7, 9, 11, 13, 15};
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
        fdt32_t zero[2] = {0, 0};
        assert(!fdt_setprop(dt, opp, "opp-microvolt", zero, volts * sizeof(u32)));
    }
}

static void world(void)
{
    memset(blob, 0, sizeof(blob));
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    add_opp_table("gpu-opp", 10, 16, 2);
    add_opp_table("cs-opp", 11, 6, 1);
    add_opp_table("afr-opp", 12, 4, 1);
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(resv >= 0);
    for (int i = 0; i < 6; i++) {
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"),
                                   region_paths[i] + strlen("/reserved-memory/"));
        assert(node >= 0 && !fdt_setprop_string(dt, node, "status", "disabled"));
    }
    int soc = fdt_add_subnode(dt, 0, "soc");
    int gpu = fdt_add_subnode(dt, soc, "gpu");
    assert(gpu >= 0 && !fdt_setprop_string(dt, gpu, "compatible", "apple,agx-t6040"));
    assert(!fdt_setprop_string(dt, gpu, "status", "okay"));
    assert(!fdt_setprop_u32(dt, gpu, "operating-points-v2", 10));
    assert(!fdt_setprop_u32(dt, gpu, "apple,cs-opp", 11));
    assert(!fdt_setprop_u32(dt, gpu, "apple,afr-opp", 12));
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0 && !fdt_setprop_string(dt, aliases, "gpu", "/soc/gpu"));

    for (int t = 0; t < 2; t++)
        for (int i = 0; i < 16; i++)
            states[t * 16 + i] = (struct perf_state){i ? 300000000u + i * 100000000u : 0,
                                                     i ? 600u + i * 30u + t * 5u : 0};
    for (int i = 0; i < 6; i++)
        cs.states[i] = (struct aux_perf_state){650000u + i * 50000u, 400000000u + i * 200000000u};
    for (int i = 0; i < 4; i++)
        afr.states[i] = (struct aux_perf_state){700000u + i * 50000u, 500000000u + i * 250000000u};
    for (int i = 0; i < 6; i++) {
        region[i][0] = 0x105fff70000UL + i * 0x40000UL;
        region[i][1] = 0x4000;
    }
    segments[0] = (struct adt_segment_ranges){.phys = region[5][0], .size = region[5][1]};
    segments[1] = (struct adt_segment_ranges){.phys = region[4][0], .size = region[4][1]};
    memcpy(map_a, good_a, sizeof(map_a));
    memcpy(map_b, good_b, sizeof(map_b));
    raw_count = 16; raw_tables = 2; raw_max_state = 15;
    base_pstate = 1; base_length = sizeof(u32);
    have_base_pstate = true;
    efuse_fault = sgx_fault = missing_asc = false;
    sgx_on = false;
    enables = restores = exc_count = 0;
}

static const char *status(const char *path)
{
    int node = fdt_path_offset(dt, path);
    assert(node >= 0);
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return s ? s : "okay";
}

static void expect_refused(const char *what, int expected_enables)
{
    assert(dt_set_gpu_t6040(dt) == 0);
    if (strcmp(status("gpu"), "disabled")) {
        fprintf(stderr, "%s: GPU was not disabled\n", what);
        exit(1);
    }
    for (int i = 0; i < 6; i++)
        if (strcmp(status(region_paths[i]), "disabled")) {
            fprintf(stderr, "%s: %s left published\n", what, region_paths[i]);
            exit(1);
        }
    if (sgx_on || enables != expected_enables || restores != enables) {
        fprintf(stderr, "%s: SGX power enables %d restores %d on %d\n", what, enables, restores,
                sgx_on);
        exit(1);
    }
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "gpu"), "apple,max-power-mw", NULL));
    printf("PASS refused: %s\n", what);
}

int main(void)
{
    /* The accepted case publishes everything and leaves SGX power as it was. */
    world();
    assert(dt_set_gpu_t6040(dt) == 0);
    assert(!strcmp(status("gpu"), "okay"));
    assert(enables == 1 && restores == 1 && !sgx_on);
    for (int i = 0; i < 6; i++) {
        assert(!strcmp(status(region_paths[i]), "okay"));
        int len;
        const fdt64_t *reg = fdt_getprop(dt, fdt_path_offset(dt, region_paths[i]), "reg", &len);
        assert(reg && len == 16 && fdt64_ld(&reg[0]) == region[i][0] &&
               fdt64_ld(&reg[1]) == region[i][1]);
    }
    int gpu = fdt_path_offset(dt, "gpu"), len;
    const fdt32_t *rel = fdt_getprop(dt, gpu, "apple,perf-relative-a", &len);
    assert(rel && len == 16 * 4 && fdt32_ld(&rel[0]) == 0 && fdt32_ld(&rel[10]) == 100);
    for (int i = 1; i < 11; i++)
        assert(fdt32_ld(&rel[i]) >= fdt32_ld(&rel[i - 1]) && fdt32_ld(&rel[i]) <= 100);
    const fdt32_t *map = fdt_getprop(dt, gpu, "apple,perf-map-a", &len);
    assert(map && len == 16 * 4 && fdt32_ld(&map[1]) == 1 && fdt32_ld(&map[10]) == 15);
    for (int i = 11; i < 16; i++) {
        assert(fdt32_ld(&rel[i]) == 0);
        assert(fdt32_ld(&map[i]) == 0);
    }
    const fdt32_t *mw = fdt_getprop(dt, gpu, "apple,max-power-mw", &len);
    assert(mw && len == 4 && fdt32_ld(mw) > 0);
    const fdt32_t *version = fdt_getprop(dt, gpu, "apple,t6040-handoff-version", &len);
    assert(version && len == 4 && fdt32_ld(version) == 2);
    const fdt32_t *mask = fdt_getprop(dt, gpu, "apple,core-mask", &len);
    assert(mask && len == 4 && fdt32_ld(mask) == 0xfffff);
    puts("PASS accepted");

    /* Public J616s reference check, not calibration for the machine under test. */
    assert(t6040_cluster_mw(1030, 1578, 1921.5f, 1.0f) == 29712);
    assert(t6040_cluster_mw(1045, 1578, 1944.0f, 1.0f) == 30830);
    assert(t6040_cs_mw(910, 1470, 251.0f) == 8574);
    assert(29712 + 30830 + 8574 == 69116);
    puts("PASS public power reference");

    world(); map_a[15] = 3; map_b[15] = 7;
    expect_refused("nonzero unused map fields", 1);
    world(); map_a[10] = 14;
    expect_refused("map does not reach the raw top OPP", 1);
    world(); raw_max_state = 10;
    expect_refused("logical count in the raw ADT count", 0);
    world(); raw_count = 15;
    expect_refused("wrong raw OPP count", 0);
    world(); raw_tables = 1;
    expect_refused("wrong cluster table count", 0);
    world(); segments[1].phys += 0x4000;
    expect_refused("DATA segment disagrees with gfx-data", 0);
    world(); segments[0].phys = region[0][0];
    expect_refused("TEXT overlaps handoff", 0);

    world(); states[0].freq = 100000000;
    expect_refused("off operating point has a clock", 0);
    world(); missing_asc = true;
    expect_refused("missing GFX ASC", 0);
    world(); have_base_pstate = false;
    assert(dt_set_gpu_t6040(dt) == 0 && !strcmp(status("gpu"), "okay"));
    puts("PASS absent base uses first-active policy");
    world(); base_length = 0;
    expect_refused("empty present base property", 0);
    world(); base_length = 8;
    expect_refused("oversized present base property", 0);
    world(); region[0][0] = 1UL << 42;
    expect_refused("region outside physical address width", 0);
    world(); map_a[1] = 0;
    expect_refused("A map selects the off state", 1);
    world(); map_b[1] = 0;
    expect_refused("B map selects the off state", 1);
    world(); map_a[5] = 4;
    expect_refused("A map does not climb", 1);
    world(); states[6].freq = states[16 + 6].freq = states[4].freq;
    expect_refused("ADT frequency does not climb along the A map", 1);
    world(); states[7].freq = states[16 + 7].freq = states[6].freq - 1;
    expect_refused("ADT frequency does not climb along the B map", 1);
    world(); states[6].volt = states[16 + 6].volt = 500;
    expect_refused("power falls along the A map", 1);
    world(); have_base_pstate = true; base_pstate = 0;
    expect_refused("base state is the off state", 0);
    world(); have_base_pstate = true; base_pstate = 2;
    expect_refused("base policy differs from the supported firmware encoder", 0);
    world(); have_base_pstate = true; base_pstate = 10;
    expect_refused("base state is the top state", 0);
    world(); region[4][1] = 0x4100;
    expect_refused("unaligned region", 0);
    world(); region[2][0] = region[1][0];
    expect_refused("overlapping regions", 0);
    world(); region[3][0] = ~0UL & ~(SZ_16K - 1);
    expect_refused("region wraps", 0);
    world(); assert(!fdt_del_node(dt, fdt_path_offset(dt, "/reserved-memory/gpu-fw-data")));
    assert(dt_set_gpu_t6040(dt) == 0 && !strcmp(status("gpu"), "disabled") && !enables);
    for (int i = 0; i < 4; i++)
        assert(!strcmp(status(region_paths[i]), "disabled"));
    puts("PASS refused: missing reserved-memory node");
    world(); efuse_fault = true;
    expect_refused("efuse read faults", 0);
    world(); sgx_fault = true;
    expect_refused("SGX read faults", 1);
    return 0;
}
'''


def test_t6040_gpu_fill(tmp_path):
    source = (ROOT / "src/kboot_gpu.c").read_text()
    defs = source[source.index("#define MAX_PSTATES"):source.index("int32_t rust_gpu_initdata_size(")]
    start = source.index("/*\n * T6040 (M4 Pro).")
    end = source.index("int dt_set_gpu_m4(void *dt);", start)
    # The T6050 section follows T6040's; it is not part of this model.
    t6050 = source.find("/*\n * T6050 (", start, end)
    t6040 = source[start:t6050 if t6050 >= 0 else end]
    # The rounding barrier uses an AArch64 register constraint; a memory one works on any host.
    t6040, replaced = re.subn(r'#define T6040_ROUND\(x\) __asm__\(""\s*:\s*"\+w"\(x\)\)',
                              '#define T6040_ROUND(x) __asm__("" : "+m"(x))', t6040)
    assert replaced == 1
    bail = '#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)\n'
    code = (HARNESS + bail + defs + ADT + function(source, "static int dt_set_resvmem(") +
            function(source, "static int fdt_set_aux_opp(") + t6040 + MAIN)
    cfile = tmp_path / "t6040-gpu-fill.c"
    cfile.write_text(code)
    libfdt = ROOT / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_strerror.c",
    )]
    binary = tmp_path / "t6040-gpu-fill"
    subprocess.run(["cc", "-D_GNU_SOURCE", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(libfdt),
                    str(cfile), *sources, "-lm", "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS") == 30, result.stdout
