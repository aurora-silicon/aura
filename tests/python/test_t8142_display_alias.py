# SPDX-License-Identifier: MIT
# Assisted-by: LLM
"""Run the production T8142 display alias/publication path with real libfdt.

The dispatcher, carveout loop, mapping table and reservation/admission routine
are extracted unchanged. Synthetic ADT/DART boundaries supply carveouts without
physical hardware. Late firmware/vRAM publishers are stubs: this is not a boot
or a validation of their independent mapping policy.
"""
from pathlib import Path
import os
import re
import subprocess

SOURCE = Path(os.environ.get("T8142_ALIAS_SOURCE", Path(__file__).resolve().parents[2]))


def definition(text, signature):
    start = text.index(signature)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    if text[end:end + 1] == ";":
        end += 1
    return text[start:end]


def test_t8142_display_alias_publication(tmp_path):
    source = (SOURCE / "src/kboot.c").read_text()
    dispatcher = definition(source, "static int dt_set_display(void)")
    helpers = "\n".join(definition(source, signature) for signature in (
        "struct disp_mapping {", "struct mem_region {",
        "static int dt_add_reserved_regions(",
        "static int dt_carveout_reserved_regions(",
        "static struct disp_mapping disp_reserved_regions_t8142[] =",
    ))
    chips = "\n".join(f"#define {name} 0x{name[1:]}" for name in
                      sorted(set(re.findall(r"\bT[0-9]+\b", dispatcher)) |
                             {"T8103", "T8112", "T8140", "T8142"}))
    prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
#define UN(x) (void)(x)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX_DCPEXT 8
#define MAX_DISP_MAPPINGS 32
#define V13_5 100
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define bail_cleanup(...) do { printf(__VA_ARGS__); ret = -1; goto err; } while (0)
static unsigned char tree[32768];
static void *dt = tree, *adt;
static u32 chip_id;
static struct { int version; const char *string; } os_firmware = {0, "fixture"};
typedef struct { u32 phandle; } dart_dev_t;
static dart_dev_t darts[8];
static unsigned initialized, released, owner_enables, vram_calls, segment_calls;
static u32 refuse_phandle;
static char selected[32];
static unsigned carveout_calls;
static dart_dev_t *dt_init_dart_by_node(int node, int index)
{
    UN(index);
    u32 phandle = fdt_get_phandle(dt, node);
    if (phandle == refuse_phandle) return NULL;
    assert(initialized < ARRAY_SIZE(darts));
    darts[initialized].phandle = phandle;
    return &darts[initialized++];
}
static void dart_shutdown(dart_dev_t *dart) { assert(dart); released++; }
static int dt_get_iommu_node(int node, int index)
{ assert(node >= 0); UN(index); return fdt_path_offset(dt, "/dart"); }
static int dt_get_or_add_reserved_mem(const char *name, const char *compat,
                                       bool no_map, u64 address, u64 size)
{
    UN(compat); UN(no_map); UN(address); UN(size);
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int node = fdt_add_subnode(dt, parent, name);
    assert(node >= 0);
    u32 phandle;
    assert(!fdt_generate_phandle(dt, &phandle));
    assert(!fdt_setprop_u32(dt, node, "phandle", phandle));
    return node;
}
static int dt_device_set_reserved_mem_from_dart(int node, dart_dev_t *dart,
        const char *alias, const char *name, u32 phandle, u64 address, u64 size)
{
    UN(node); UN(name); UN(address); UN(size);
    assert(dart && dart->phandle == phandle);
    assert(fdt_get_phandle(dt, fdt_path_offset(dt, alias)) == phandle);
    return 0;
}
static int dt_device_add_mem_region(const char *alias, u32 phandle, const char *name)
{
    UN(name);
    int node = fdt_path_offset(dt, alias);
    assert(node >= 0);
    return fdt_appendprop_u32(dt, node, "memory-region", phandle);
}
static int dt_enable_dcp_with_typec_routes(const char *alias)
{
    owner_enables++;
    int node = fdt_path_offset(dt, alias);
    return fdt_setprop_string(dt, node, "status", "okay");
}
static int dt_set_dcp_firmware(const char *alias) { UN(alias); return 0; }
static int adt_path_offset(void *blob, const char *path)
{ UN(blob); assert(!strcmp(path, "/chosen/carveout-memory-map")); return 1; }
static int fixture_carveout(void *blob, int node, const char *name, u64 *values)
{
    UN(blob); assert(node == 1);
    unsigned id = 1;
    if (!strncmp(name, "region-id-", 10)) id = (unsigned)strtoul(name + 10, NULL, 10);
    values[0] = 0x100000 + id * 0x4000;
    values[1] = 0x4000;
    return 2 * sizeof(u64);
}
#define ADT_GETPROP_ARRAY(blob, node, name, out) fixture_carveout(blob, node, name, out)
'''
    # Record the selected argument without duplicating the production selection.
    helpers = helpers.replace("static int dt_carveout_reserved_regions(",
                              "static int real_dt_carveout_reserved_regions(", 1)
    suffix = r'''
static int dt_carveout_reserved_regions(const char *dcp, const char *disp,
        const char *piodma, struct disp_mapping *maps, u32 count)
{
    if (strcmp(dcp, "dcp")) return 0;
    assert(piodma);
    snprintf(selected, sizeof(selected), "%s", piodma);
    carveout_calls++;
    return real_dt_carveout_reserved_regions(dcp, disp, piodma, maps, count);
}
static struct disp_mapping disp_reserved_regions_t8103[] = {{"test", "test", true, true, true}};
static struct disp_mapping disp_reserved_regions_t8112[] = {{"test", "test", true, true, true}};
static struct disp_mapping disp_reserved_regions_t600x[1], disp_reserved_regions_t602x[1];
static struct disp_mapping dcpext_reserved_regions_t8103[1], dcpext_reserved_regions_t600x[8][1];
typedef struct { const char *dcp_alias; } display_config_t;
static int dt_set_display_t8140(void) { return 0; }
static void dart_lock_adt(const char *path, int index) { UN(path); UN(index); }
static int dt_set_m4_dcp_handoff(void) { return 0; }
static void dt_reserve_j713_dcpext(void) {}
static int dt_set_display_handoff(void *blob) { UN(blob); return 0; }
static void dt_set_j514s_native(void) {}
static void dt_reserve_dcpext_firmware(bool all) { UN(all); }
static const display_config_t *display_get_config(void) { return NULL; }
static int dt_vram_reserved_region(const char *a, const char *b)
{ UN(a); UN(b); vram_calls++; return 0; }
static int dt_reserve_m5_display_segments(void) { segment_calls++; return 0; }
'''
    cases = r'''
static void add_node(const char *name, u32 phandle)
{
    int node = fdt_add_subnode(dt, 0, name);
    assert(node >= 0);
    assert(!fdt_setprop_u32(dt, node, "phandle", phandle));
    assert(!fdt_setprop_string(dt, node, "status", "disabled"));
}
static void reset(u32 chip, int canonical, int legacy, bool same)
{
    memset(tree, 0, sizeof(tree));
    assert(!fdt_create_empty_tree(dt, sizeof(tree)));
    chip_id = chip;
    const char *compat = chip == T8142 ? "apple,t8142" :
                         chip == T8103 ? "apple,t8103" : "apple,t8112";
    assert(!fdt_setprop_string(dt, 0, "compatible", compat));
    add_node("dcp", 1); add_node("display", 2); add_node("canonical", 3);
    add_node("legacy", 4); add_node("dart", 5);
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    int aliases = fdt_add_subnode(dt, 0, "aliases"); assert(aliases >= 0);
    assert(!fdt_setprop_string(dt, aliases, "dcp", "/dcp"));
    assert(!fdt_setprop_string(dt, aliases, "disp0", "/display"));
    if (canonical) assert(!fdt_setprop_string(dt, aliases, "disp0-piodma",
                                             canonical == 1 ? "/canonical" : "/absent"));
    if (legacy) assert(!fdt_setprop_string(dt, aliases, "disp0_piodma",
                     legacy == 1 ? (same ? "/canonical" : "/legacy") : "/absent"));
    initialized = released = owner_enables = vram_calls = segment_calls = carveout_calls = 0;
    refuse_phandle = 0;
    selected[0] = 0;
}
static bool has_regions(const char *path)
{ return fdt_getprop(dt, fdt_path_offset(dt, path), "memory-region", NULL) != NULL; }
static void run(const char *name, u32 chip, int canonical, int legacy, bool same,
                const char *expected_alias, const char *expected_target)
{
    fprintf(stderr, "case %s\n", name);
    reset(chip, canonical, legacy, same);
    assert(!dt_set_display());
    assert(carveout_calls == 1 && selected[0] && !strcmp(selected, expected_alias));
    assert(initialized == released);
    assert(owner_enables == (expected_target ? 1U : 0U));
    assert(has_regions("/canonical") == (expected_target && !strcmp(expected_target, "/canonical")));
    assert(has_regions("/legacy") == (expected_target && !strcmp(expected_target, "/legacy")));
    int dcp = fdt_path_offset(dt, "/dcp");
    assert(!strcmp(fdt_getprop(dt, dcp, "status", NULL), expected_target ? "okay" : "disabled"));
}
int main(void)
{
    run("legacy only", T8142, 0, 1, false, "disp0_piodma", "/legacy");
    run("canonical only", T8142, 1, 0, false, "disp0-piodma", "/canonical");
    run("both different", T8142, 1, 1, false, "disp0-piodma", "/canonical");
    run("both same", T8142, 1, 1, true, "disp0-piodma", "/canonical");
    run("invalid canonical does not fall back", T8142, 2, 1, false, "disp0-piodma", NULL);
    run("invalid legacy", T8142, 0, 2, false, "disp0_piodma", NULL);
    run("no aliases", T8142, 0, 0, false, "disp0_piodma", NULL);
    run("M1 precedence unchanged", T8103, 1, 1, false, "disp0_piodma", "/legacy");
    run("M2 precedence unchanged", T8112, 1, 1, false, "disp0_piodma", "/legacy");
    reset(T8142, 1, 1, false);
    refuse_phandle = 3;
    assert(dt_set_display() == -1);
    assert(!owner_enables && !vram_calls && !segment_calls);
    assert(!has_regions("/canonical") && !has_regions("/legacy"));
    assert(initialized == released);
    puts("PASS T8142 display alias publication: 10 scenarios");
    return 0;
}
'''
    program = prefix + chips + "\n" + helpers + suffix + dispatcher + cases
    c = tmp_path / "t8142-alias.c"
    c.write_text(program)
    lib = SOURCE / "src/libfdt"
    sources = [lib / name for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c",
        "fdt_strerror.c", "fdt_empty_tree.c", "fdt_check.c",
    )]
    binary = tmp_path / "t8142-alias"
    command = [os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
               "-Werror", "-Wno-unused-function", "-Wno-unused-variable", "-fsanitize=address,undefined",
               "-fno-sanitize-recover=all", "-I", str(lib), str(c), *map(str, sources), "-o", str(binary)]
    compiled = subprocess.run(command, capture_output=True, text=True)
    (tmp_path / "compile.log").write_text(compiled.stdout + compiled.stderr)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    completed = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    (tmp_path / "runtime.log").write_text(completed.stdout + completed.stderr)
    assert completed.returncode == 0, completed.stdout + completed.stderr
    assert "PASS T8142 display alias publication: 10 scenarios" in completed.stdout
