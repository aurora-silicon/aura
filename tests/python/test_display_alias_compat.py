# SPDX-License-Identifier: GPL-2.0-only
# Generated-by: LLM
# Assisted-by: LLM
"""A display device the DT does not describe is skipped; one it describes must resolve."""

from pathlib import Path
import subprocess

import pytest


@pytest.mark.parametrize("chip", [0x8103, 0x8112, 0x6000, 0x8142, 0x8140],
                         ids=["M1", "M2", "M1Pro", "M5", "A18Pro"])
def test_display_alias_absent_skips_unresolved_refuses(tmp_path, chip):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    def definition(start_text, terminator):
        start = source.index(start_text)
        return source[start:source.index(terminator, start) + len(terminator)]

    helper = "\n".join((
        definition("struct disp_mapping {", "\n};"),
        definition("struct mem_region {", "\n};"),
        definition("static int dt_display_node_unresolved(", "\n}"),
        definition("static int dt_add_reserved_regions(", "\n}"),
    ))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef int dart_dev_t;
#define T8140 0x8140
#define bail_cleanup(...) do { ret = -1; goto err; } while (0)
static void *dt;
static u32 chip_id = CHIP;
static const char *missing;
static bool alias_defined;
static int fdt_path_offset(const void *tree, const char *path)
{ (void)tree; return !strcmp(path, missing) ? -1 : 1; }
static const char *fdt_get_alias(const void *tree, const char *name)
{ (void)tree; return strcmp(name, missing) || alias_defined ? "/soc/somewhere" : NULL; }
static dart_dev_t *dt_init_dart_by_node(int node, int index)
{ static dart_dev_t dart; (void)node; (void)index; return &dart; }
static uint32_t fdt_get_phandle(const void *tree, int node)
{ (void)tree; (void)node; return 1; }
static int dt_get_or_add_reserved_mem(const char *a, const char *b, bool c, u64 d, u64 e)
{ (void)a; (void)b; (void)c; (void)d; (void)e; return 1; }
static int dt_device_set_reserved_mem_from_dart(int node, dart_dev_t *dart,
                                               const char *device, const char *name,
                                               u32 phandle, u64 address, u64 size)
{
    (void)node; (void)dart; (void)device; (void)name;
    (void)phandle; (void)address; (void)size; return 0;
}
static int dt_device_add_mem_region(const char *path, u32 phandle, const char *name)
{ (void)path; (void)phandle; (void)name; return 0; }
static int dt_get_iommu_node(int node, u32 index)
{ (void)node; (void)index; return 1; }
static int fdt_setprop_string(void *tree, int node, const char *name, const char *value)
{ (void)tree; (void)node; (void)name; (void)value; return 0; }
static void dart_shutdown(dart_dev_t *dart) { (void)dart; }
/* Every scenario returns at the missing device before enabling any owner. */
static int dt_enable_dcp_with_typec_routes(const char *alias)
{ (void)alias; assert(0); return -1; }
''' + helper + r'''
int main(void)
{
    (void)chip_id;
    const char *aliases[] = {"dcp", "disp", "piodma"};
    for (int i = 0; i < 3; i++) {
        /* No such alias: an older DT without that display device. */
        missing = aliases[i];
        alias_defined = false;
        assert(dt_add_reserved_regions("dcp", "disp", "piodma", NULL,
                                       NULL, NULL, 0) == 0);
        /* The alias is there but does not resolve: refused. */
        alias_defined = true;
        assert(dt_add_reserved_regions("dcp", "disp", "piodma", NULL,
                                       NULL, NULL, 0) == -1);
    }
    /* An absolute path from the caller always names a described device. */
    missing = "/soc/dcp@1";
    alias_defined = false;
    assert(dt_add_reserved_regions("/soc/dcp@1", NULL, NULL, NULL, NULL, NULL, 0) == -1);
    return 0;
}
'''
    binary = tmp_path / "display-alias-compat"
    subprocess.run(["cc", f"-DCHIP={chip}", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-x", "c", "-", "-o", str(binary)],
                   input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)
