# SPDX-License-Identifier: GPL-2.0-only
"""T8152: this Mac's amp, PMP, camera and GPU performance-state values, ADT to FDT.

The ways this can go wrong, each checked below with production code and a synthetic ADT and
device tree (every value made up):

- a copy runs only in the minimal-boot branch, after /memory is written or after the tree is
  packed, so an installed J804 boot never gets it, or it lands in a tree that has no room;
- the tree has no room: no T8152 dt_bufsize branch;
- a missing or odd ADT value (absent, 8 bytes instead of 4, wrong row count, state 0 not at
  0 Hz, mismatched SRAM table) still writes something, or writes part of a set (one or two
  of the three receiver words, a core table without its SRAM table, half the PMP tunables);
- a refusal stops the boot, or changes a node's status (PMP, ISP);
- the copy reaches a tree it is not for (another board, a node with another compatible);
- the cells come out in the wrong order or byte order (fuse76, fuse532, fuse549; <hz mV>).

The amp copy itself (kboot_audio_amp_config(), six amps matched by I2C address 0x38-0x3d in the
ADT's group order) is covered by tests/run-kboot-audio.sh, scenario six-amps; here only its call
site is checked.
"""

from pathlib import Path
import re
import shutil
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]
LIBFDT = ROOT / "src/libfdt"


def extract(source, start, end):
    first = source.index(start)
    return source[first:source.index(end, first)]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3] + "\n"


def run_c(tmp_path, name, source):
    binary = tmp_path / name
    subprocess.run(
        ["cc", "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(LIBFDT), "-x", "c", "-",
         *map(str, (LIBFDT / f for f in (
             "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
             "fdt_empty_tree.c", "fdt_strerror.c"))), "-o", str(binary)],
        input=source, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


def dtb(tmp_path, name, dts):
    if not shutil.which("dtc"):
        pytest.skip("needs dtc")
    out = tmp_path / (name + ".dtb")
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(out), "-"],
                   input=dts, text=True, check=True)
    return out


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint8_t u8; typedef uint32_t u32; typedef unsigned long u64;
_Static_assert(sizeof(u64) == 8, "the aura host fixture requires an LP64 host");
#define T8152 0x8152
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define UNUSED(x) (void)(x)
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt, *adt;
static u32 chip_id = T8152, board_id = 0x22;
static void used(void) { (void)chip_id; (void)board_id; }
static unsigned char blob[65536], before[65536];

/* A made-up ADT: (node, name) -> bytes. */
struct fake_prop { int node; const char *name; const void *val; u32 len; };
static struct fake_prop props[64];
static int nprops;
static void put(int node, const char *name, const void *val, u32 len)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name)) {
            props[i].val = val; props[i].len = len; return;
        }
    props[nprops++] = (struct fake_prop){node, name, val, len};
}
static void drop(int node, const char *name)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name))
            props[i] = props[--nprops];
}
static const char *paths[8];
static int adt_path_offset(void *tree, const char *path)
{
    for (int i = 1; i < 8; i++)
        if (paths[i] && !strcmp(paths[i], path)) return i;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    for (int i = 0; i < nprops; i++)
        if (props[i].node == node && !strcmp(props[i].name, name)) {
            if (len) *len = props[i].len;
            return props[i].val;
        }
    return NULL;
}
static void load(const char *path)
{
    FILE *f = fopen(path, "rb");
    unsigned char raw[16384];
    size_t n;
    assert(f && (n = fread(raw, 1, sizeof(raw), f)) > 0);
    fclose(f);
    memset(blob, 0, sizeof(blob));
    assert(!fdt_open_into(raw, blob, sizeof(blob)));
    dt = blob;
    nprops = 0;
    memset(paths, 0, sizeof(paths));
}
static void snapshot(void) { memcpy(before, blob, sizeof(blob)); }
static int unchanged(void) { return !memcmp(before, blob, sizeof(blob)); }
static const void *dtprop(const char *path, const char *name, int *len)
{
    return fdt_getprop(dt, fdt_path_offset(dt, path), name, len);
}
'''


def test_t8152_call_sites_and_room():
    source = (ROOT / "src/kboot.c").read_text()
    body = extract(source, "static int dt_prepare_t8152(void)\n{", "\n}\n")
    minimal = extract(body, 'if (fdt_getprop(dt, 0, "aurora,minimal-boot", NULL)) {', "\n    }\n")
    calls = ("kboot_audio_amp_config(dt, adt);", "dt_set_pmp_t8152();",
             "dt_set_isp_adt_t8152();", "dt_set_gpu_perf_states_t8152(dt);")
    for call in calls:
        # Once, outside the minimal-boot branch, never as a condition that can end the boot,
        # before /memory is written and the tree is packed.
        assert call not in minimal, call
        assert body.count(call) == 1, call
        assert re.search(r"\n    " + re.escape(call) + r"\n", body), call
        assert body.index(call) < body.index("dt_set_memory()") < body.index("fdt_pack(dt)")
    prepare = extract(source, "int kboot_prepare_dt(void *fdt)", "dt = memalign(")
    assert re.search(r"if \(chip_id == T8152\)\s*\n\s*dt_bufsize \+= 4 \* SZ_16K;", prepare)
    # The pmp copy writes values only.
    pmp = extract(source, "static int dt_set_pmp_t8152_one(", "static void dt_set_isp_adt_t8152(")
    assert '"status"' not in pmp
    isp = extract(source, "static void dt_set_isp_adt_t8152(", "static int dt_prepare_t8152(")
    assert '"status"' not in isp


ISP_TREE = r'''/dts-v1/;
/ {
    compatible = "apple,j804", "apple,t8152";
    #address-cells = <2>; #size-cells = <2>;
    soc {
        #address-cells = <2>; #size-cells = <2>; ranges;
        isp: isp@4c0000000 {
            compatible = "apple,t8152-isp", "apple,isp";
            apple,lpdprx-cfg-fuses = <0xeeeeeeee 0xeeeeeeee 0xeeeeeeee>;
            apple,cil-config = <0xeeeeeeee>;
            status = "disabled";
        };
    };
};
'''


def test_j804_isp_values(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    j804 = dtb(tmp_path, "isp", ISP_TREE)
    other = dtb(tmp_path, "isp-other", ISP_TREE.replace('"apple,j804", ', '"apple,j873", '))
    run_c(tmp_path, "isp-values", PREFIX + function(kboot, "static int adt_get_u32_exact(")
          + function(kboot, "static void dt_set_isp_adt_t8152(void)") + r'''
static const u32 f76 = 0x11223344, f532 = 0x55667788, f549 = 0x99aabbcc, cil = 0x13572468;
static const u64 wide = 0x55667788;
static void world(const char *path)
{
    load(path);
    paths[2] = "/arm-io/isp";
    paths[3] = "/arm-io/eic";
    put(2, "lpdprx_cfg_fuse76", &f76, 4);
    put(2, "lpdprx_cfg_fuse532", &f532, 4);
    put(2, "lpdprx_cfg_fuse549", &f549, 4);
    put(3, "cil-gpio-config", &cil, 4);
}
static void expect(int fuses, int with_cil)
{
    int len;
    const fdt32_t *v = dtprop("/soc/isp@4c0000000", "apple,lpdprx-cfg-fuses", &len);
    assert(v && len == 12);
    if (fuses) {
        /* In the ADT's order, big-endian cells. */
        assert(fdt32_to_cpu(v[0]) == f76 && fdt32_to_cpu(v[1]) == f532 &&
               fdt32_to_cpu(v[2]) == f549);
    } else {
        for (int i = 0; i < 3; i++) assert(fdt32_to_cpu(v[i]) == 0xeeeeeeee);
    }
    v = dtprop("/soc/isp@4c0000000", "apple,cil-config", &len);
    assert(v && len == 4 && fdt32_to_cpu(v[0]) == (with_cil ? cil : 0xeeeeeeee));
    assert(!strcmp(dtprop("/soc/isp@4c0000000", "status", NULL), "disabled"));
}
int main(void)
{
    world(J804); dt_set_isp_adt_t8152(); expect(1, 1);
    /* One receiver word missing: none of the three; cil-config still copied. */
    world(J804); drop(2, "lpdprx_cfg_fuse549"); dt_set_isp_adt_t8152(); expect(0, 1);
    /* One word not exactly four bytes: none of the three. */
    world(J804); put(2, "lpdprx_cfg_fuse532", &wide, 8); dt_set_isp_adt_t8152(); expect(0, 1);
    /* No /arm-io/isp at all. */
    world(J804); paths[2] = NULL; dt_set_isp_adt_t8152(); expect(0, 1);
    /* cil-gpio-config missing or odd: left as the tree had it; the fuses still copied. */
    world(J804); drop(3, "cil-gpio-config"); dt_set_isp_adt_t8152(); expect(1, 0);
    world(J804); put(3, "cil-gpio-config", &f76, 2); dt_set_isp_adt_t8152(); expect(1, 0);
    world(J804); paths[3] = NULL; dt_set_isp_adt_t8152(); expect(1, 0);
    /* Another board's tree: not touched at all. */
    world(OTHER); snapshot(); dt_set_isp_adt_t8152(); assert(unchanged());
    return 0;
}
'''.replace("J804)", '"%s")' % j804).replace("OTHER)", '"%s")' % other))


PMP_TREE = r'''/dts-v1/;
/ {
    compatible = "apple,j804", "apple,t8152";
    #address-cells = <2>; #size-cells = <2>;
    soc {
        #address-cells = <2>; #size-cells = <2>; ranges;
        pmp: pmp@300500000 {
            compatible = "apple,t8152-pmp-v2";
            apple,board-id = <0xee>;
            apple,dram-vendor-id = <0xee>;
            apple,tunable-alpha = [ee ee];
            status = "okay";
        };
    };
};
'''


def test_t8152_pmp_values(tmp_path):
    kboot = (ROOT / "src/kboot.c").read_text()
    good = dtb(tmp_path, "pmp", PMP_TREE)
    other = dtb(tmp_path, "pmp-other", PMP_TREE.replace('"apple,t8152-pmp-v2"', '"apple,pmp"'))
    run_c(tmp_path, "pmp-values", PREFIX + r'''
struct adt_property { char name[32]; u32 size; const u8 *value; };
static struct adt_property nub[8];
static int nnub;
#define ADT_FOREACH_PROPERTY(a, node, prop) \
    for (const struct adt_property *prop = nub; prop < nub + nnub; prop++)
''' + extract(kboot, "static const char *excluded_pmp_props[]", "/*\n * Copies this Mac")
          + function(kboot, "static int dt_fill_pmp(int pmp_node)")
          + function(kboot, "static int dt_display_transaction(")
          + extract(kboot, "#define T8152_PMP_PATH", "/*\n * J804 camera") + r'''
static const u32 board = 0x22, vendor = 0x5a, capacity = 0x77;
static const u64 wide = 3;
static const u8 alpha[] = {1, 2, 3}, beta[] = {4, 5, 6, 7, 8};
static const u8 huge[70000];
static void world(const char *path)
{
    load(path);
    paths[1] = "/chosen";
    paths[2] = "/arm-io/pmp/iop-pmp-nub";
    put(1, "board-id", &board, 4);
    put(1, "dram-vendor-id", &vendor, 4);
    put(2, "dram-capacity", &capacity, 4);
    nnub = 0;
    nub[nnub++] = (struct adt_property){"alpha", sizeof(alpha), alpha};
    nub[nnub++] = (struct adt_property){"compatible", 4, (const u8 *)"pmp"};
    nub[nnub++] = (struct adt_property){"beta", sizeof(beta), beta};
    nub[nnub++] = (struct adt_property){"dram-capacity", 4, (const u8 *)&capacity};
}
static u32 cell(const char *name)
{
    int len;
    const fdt32_t *v = dtprop("/soc/pmp@300500000", name, &len);
    assert(v && len == 4);
    return fdt32_to_cpu(*v);
}
int main(void)
{
    int len;
    const u8 *v;

    world(GOOD);
    dt_set_pmp_t8152();
    assert(cell("apple,board-id") == board && cell("apple,dram-vendor-id") == vendor &&
           cell("apple,dram-capacity") == capacity);
    v = dtprop("/soc/pmp@300500000", "apple,tunable-alpha", &len);
    assert(v && len == 3 && !memcmp(v, alpha, 3));
    v = dtprop("/soc/pmp@300500000", "apple,tunable-beta", &len);
    assert(v && len == 5 && !memcmp(v, beta, 5));
    /* Excluded names are not copied; the status is the tree's. */
    assert(!dtprop("/soc/pmp@300500000", "apple,tunable-compatible", NULL));
    assert(!dtprop("/soc/pmp@300500000", "apple,tunable-dram-capacity", NULL));
    assert(!strcmp(dtprop("/soc/pmp@300500000", "status", NULL), "okay"));

    /* A refusal after some values were written leaves the node exactly as it was. */
    world(GOOD); put(1, "dram-vendor-id", &wide, 8); snapshot();
    dt_set_pmp_t8152(); assert(unchanged());
    world(GOOD); paths[2] = NULL; snapshot(); dt_set_pmp_t8152(); assert(unchanged());
    world(GOOD); put(2, "dram-capacity", &wide, 8); snapshot();
    dt_set_pmp_t8152(); assert(unchanged());
    /* A tunable that does not fit, after board-id and alpha were written: all discarded. */
    world(GOOD); nub[nnub++] = (struct adt_property){"huge", sizeof(huge), huge}; snapshot();
    dt_set_pmp_t8152(); assert(unchanged());
    /* A pmp node of another kind: not touched. */
    world(OTHER); snapshot(); dt_set_pmp_t8152(); assert(unchanged());
    return 0;
}
'''.replace("GOOD)", '"%s")' % good).replace("OTHER)", '"%s")' % other))


GPU_TREE = r'''/dts-v1/;
/ {
    compatible = "apple,j804", "apple,t8152";
    #address-cells = <2>; #size-cells = <2>;
    soc {
        #address-cells = <2>; #size-cells = <2>; ranges;
        gpu@580000000 {
            compatible = "apple,agx-t8152", "apple,agx-g18g";
            apple,perf-states = <0xee 0xee>;
            apple,perf-states-sram = <0xee>;
            status = "okay";
        };
    };
};
'''


def test_t8152_gpu_perf_states(tmp_path):
    gpu = (ROOT / "src/kboot_gpu_t8152.c").read_text()
    good = dtb(tmp_path, "gpu", GPU_TREE)
    run_c(tmp_path, "gpu-perf-states", PREFIX
          + function(gpu, "static bool number(int node, const char *prop, u64 *value)")
          + function(gpu, "static bool string_is(int node, const char *prop, const char *value)")
          + extract(gpu, "#define T8152_MAX_PERF_STATES", "int dt_set_gpu_perf_states_t8152(")
          + function(gpu, "int dt_set_gpu_perf_states_t8152(void *dt)") + r'''
#define N 14
static struct t8152_perf_state ps[N], sram[N];
static u32 count = N - 1;
static const u64 wide_count = N - 1;
static void world(void)
{
    load(GOOD);
    chip_id = T8152; board_id = 0x22; count = N - 1;
    paths[4] = "/arm-io/sgx";
    for (int i = 0; i < N; i++) {
        ps[i] = (struct t8152_perf_state){i ? 400000000u + 100000000u * i : 0, 500 + 30 * i};
        sram[i] = (struct t8152_perf_state){ps[i].freq, 700 + 20 * i};
    }
    put(4, "compatible", "gpu,t8152", 10);
    put(4, "gpu-num-perf-states", &count, 4);
    put(4, "perf-states", ps, sizeof(ps));
    put(4, "perf-states-sram", sram, sizeof(sram));
}
static const fdt32_t *table(const char *name, int *len)
{
    return dtprop("/soc/gpu@580000000", name, len);
}
static void refused(void)
{
    assert(dt_set_gpu_perf_states_t8152(dt) == 0); /* never fatal */
    assert(!table("apple,perf-states", NULL) && !table("apple,perf-states-sram", NULL));
    assert(!strcmp(dtprop("/soc/gpu@580000000", "status", NULL), "okay"));
}
int main(void)
{
    int len;
    const fdt32_t *v;

    world();
    assert(dt_set_gpu_perf_states_t8152(dt) == 0);
    v = table("apple,perf-states", &len);
    assert(v && len == N * 8);
    for (int i = 0; i < N; i++)
        assert(fdt32_to_cpu(v[2 * i]) == ps[i].freq && fdt32_to_cpu(v[2 * i + 1]) == ps[i].volt);
    v = table("apple,perf-states-sram", &len);
    assert(v && len == N * 4);
    for (int i = 0; i < N; i++)
        assert(fdt32_to_cpu(v[i]) == sram[i].volt);
    /* gpu-num-perf-states may be 8 bytes, as number() reads it elsewhere. */
    world(); put(4, "gpu-num-perf-states", &wide_count, 8);
    assert(dt_set_gpu_perf_states_t8152(dt) == 0 && table("apple,perf-states", NULL));

    /* Every refusal removes both tables, even the tree's own. */
    world(); count = N; refused();
    world(); count = 0; refused();
    world(); drop(4, "gpu-num-perf-states"); refused();
    world(); put(4, "perf-states", ps, sizeof(ps) - 8); refused();
    world(); put(4, "perf-states-sram", sram, sizeof(sram) - 8); refused();
    world(); drop(4, "perf-states-sram"); refused();
    world(); ps[0].freq = 1; sram[0].freq = 1; refused();
    world(); ps[5].freq = ps[4].freq; sram[5].freq = sram[4].freq; refused();
    world(); sram[7].freq++; refused();
    world(); put(4, "compatible", "gpu,t8122", 10); refused();
    world(); paths[4] = NULL; refused();
    /* Not a J804 (J873g included) or not T8152: not touched. */
    world(); board_id = 0x24; snapshot();
    assert(dt_set_gpu_perf_states_t8152(dt) == 0 && unchanged());
    world(); board_id = 0x30; snapshot();
    assert(dt_set_gpu_perf_states_t8152(dt) == 0 && unchanged());
    world(); chip_id = 0x8122; snapshot();
    assert(dt_set_gpu_perf_states_t8152(dt) == 0 && unchanged());
    return 0;
}
'''.replace("GOOD)", '"%s")' % good))
