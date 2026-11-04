# SPDX-License-Identifier: MIT
"""The M4 Pro and A18 Pro ANE preload handoff: reservations, memory regions and firmware IOVAs.

Runs kboot.c's dt_set_ane_firmware(), dt_set_ane_node(), dt_ane_fill_region() and
dt_get_or_add_reserved_mem() as they are, on real FDTs built with aura's libfdt, against a
fake ADT. Segment values are the J616s 26.6.2 /arm-io/ane0 and the Neo (J700) 26.6.2
/arm-io/ane segment-ranges.
"""

from pathlib import Path

import pytest
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


BOARDS = {
    "j616s": dict(compat="apple,t6041-ane", node="ane@484000000", adt="/arm-io/ane0",
                  text=0x100009D8000, data=0x1000196C000),
    "j700": dict(compat="apple,t8140-ane", node="ane@400000000", adt="/arm-io/ane",
                 text=0x10000768000, data=0x100019A0000),
}


@pytest.mark.parametrize("board", sorted(BOARDS))
def test_ane_preload_handoff(tmp_path, board):
    b = BOARDS[board]
    source = (ROOT / "src/kboot.c").read_text()

    def function(name):
        start = source.index("static int " + name + "(")
        return source[start:source.index("\n}", start) + 2] + "\n"

    start = source.index("/*\n * The M4 Pro and A18 Pro Neural Engines")
    end = source.index("\n}", source.index("static int dt_set_ane_firmware(void)")) + 2
    production = function("dt_get_or_add_reserved_mem") + source[start:end] + "\n"
    header = (ROOT / "src/adt.h").read_text()
    start = header.index("struct adt_segment_ranges {")
    layout = header[start:header.index("} PACKED;", start) + len("} PACKED;")]
    program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
#define PACKED __attribute__((packed))
#define SZ_16K 16384
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static void *dt, *adt;
static bool el2 = true;
static bool in_el2(void) { return el2; }
static struct { u64 phys_base; } cur_boot_args = { 0x10002000000UL };
''' + layout + r'''
static struct adt_segment_ranges segments[2];
static const char *segment_names;
static u32 names_len, segments_len, preloaded;
static bool have_ane0;
static u64 dram_base;

static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "@ADT@"))
        return have_ane0 ? 1 : -1;
    if (!strcmp(path, "/chosen"))
        return 2;
    return -1;
}
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    assert(node == 1);
    if (!strcmp(name, "segment-ranges")) {
        *len = segments_len;
        return segments;
    }
    assert(!strcmp(name, "segment-names"));
    *len = names_len;
    return segment_names;
}
static int adt_getprop_copy(void *tree, int node, const char *name, void *out, size_t len)
{
    (void)tree;
    if (node == 1 && !strcmp(name, "pre-loaded") && len == 4) {
        memcpy(out, &preloaded, 4);
        return 4;
    }
    if (node == 2 && !strcmp(name, "dram-base") && len == 8) {
        memcpy(out, &dram_base, 8);
        return 8;
    }
    return -1;
}
#define ADT_GETPROP(a, n, k, v) adt_getprop_copy(a, n, k, (v), sizeof(*(v)))
''' + production + r'''
static unsigned char blob[16384], before[16384];

static void reset_adt(void)
{
    el2 = true;
    have_ane0 = true;
    preloaded = 1;
    dram_base = 0x10000000000UL;
    segments[0] = (struct adt_segment_ranges){@TEXT@UL, 0, 0x10000000000UL, 0xc0000, 1};
    segments[1] = (struct adt_segment_ranges){@DATA@UL, 0xc0000, 0x100000c0000UL,
                                              0x2ac000, 0};
    segments_len = sizeof(segments);
    segment_names = "__TEXT;__DATA";
    names_len = 14;
}

/* placeholders: the shipping DT form; otherwise an older DT without memory regions. */
static void build(bool placeholders, const char *status, bool ane)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int resv = fdt_add_subnode(dt, 0, "reserved-memory");
    assert(resv >= 0);
    assert(!fdt_setprop_u32(dt, resv, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, resv, "#size-cells", 2));
    assert(!fdt_setprop_empty(dt, resv, "ranges"));
    if (placeholders) {
        const char *names[2] = {"ane-firmware-text@0", "ane-firmware-data@0"};
        for (int i = 0; i < 2; i++) {
            u64 zero[2] = {0, 0};
            resv = fdt_path_offset(dt, "/reserved-memory");
            int n = fdt_add_subnode(dt, resv, names[i]);
            assert(n >= 0);
            assert(!fdt_setprop_string(dt, n, "compatible", "apple,asc-mem"));
            assert(!fdt_setprop(dt, n, "reg", zero, sizeof(zero)));
            assert(!fdt_setprop_empty(dt, n, "no-map"));
            assert(!fdt_setprop_string(dt, n, "status", "disabled"));
            assert(!fdt_setprop_u32(dt, n, "phandle", 0x10 + i));
        }
    }
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);
    if (!ane)
        return;
    int n = fdt_add_subnode(dt, soc, "@NODE@");
    assert(n >= 0);
    assert(!fdt_setprop_string(dt, n, "compatible", "@COMPAT@"));
    assert(!fdt_setprop_string(dt, n, "status", status));
    if (placeholders) {
        fdt32_t regions[2] = {cpu_to_fdt32(0x10), cpu_to_fdt32(0x11)};
        assert(!fdt_setprop(dt, n, "memory-region", regions, sizeof(regions)));
        assert(!fdt_setprop(dt, n, "memory-region-names", "firmware-text\0firmware-data", 28));
    }
}

static const char *status(void)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/soc/@NODE@"), "status", NULL);
}

static void check_region(const fdt32_t *regions, int i, const char *name, u64 phys, u64 size)
{
    int n = fdt_node_offset_by_phandle(dt, fdt32_ld(&regions[i]));
    assert(n >= 0);
    assert(fdt_parent_offset(dt, n) == fdt_path_offset(dt, "/reserved-memory"));
    assert(!strcmp(fdt_get_name(dt, n, NULL), name));
    int len;
    const fdt64_t *reg = fdt_getprop(dt, n, "reg", &len);
    assert(reg && len == 16 && fdt64_ld(&reg[0]) == phys && fdt64_ld(&reg[1]) == size);
    assert(fdt_getprop(dt, n, "no-map", NULL));
    const char *st = fdt_getprop(dt, n, "status", NULL);
    assert(!st || !strcmp(st, "okay"));
}

static const char *marker(void)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,ane-cold-boot", NULL);
}

static void check_handoff(const char *want_status)
{
    int ane = fdt_path_offset(dt, "/soc/@NODE@");
    int len;
    const char *m = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,ane-cold-boot", &len);
    assert(m && len == 28 && !strcmp(m, "raw-no-xnu-ane-untouched-v1"));
    assert(!strcmp(status(), want_status));
    const fdt32_t *regions = fdt_getprop(dt, ane, "memory-region", &len);
    assert(regions && len == 8);
    check_region(regions, 0, "ane-firmware-text@@TEXTN@", @TEXT@UL, 0xc0000);
    check_region(regions, 1, "ane-firmware-data@@DATAN@", @DATA@UL, 0x2ac000);
    const char *names = fdt_getprop(dt, ane, "memory-region-names", &len);
    assert(names && len == 28 && !memcmp(names, "firmware-text\0firmware-data", 28));
    const fdt64_t *iovas = fdt_getprop(dt, ane, "apple,firmware-iovas", &len);
    assert(iovas && len == 16);
    assert(fdt64_ld(&iovas[0]) == 0x10000000000UL && fdt64_ld(&iovas[1]) == 0x100000c0000UL);
}

/* A refused handoff disables the node and never reserves anything. */
static void check_refused(void (*fault)(void), const char *what)
{
    for (int form = 0; form < 2; form++) {
        reset_adt();
        fault();
        build(form, "okay", true);
        assert(dt_set_ane_firmware() == 0);
        assert(!strcmp(status(), "disabled"));
        int resv = fdt_path_offset(dt, "/reserved-memory");
        int count = 0, child;
        fdt_for_each_subnode(child, dt, resv) {
            const char *st = fdt_getprop(dt, child, "status", NULL);
            assert(form && st && !strcmp(st, "disabled"));
            count++;
        }
        assert(count == (form ? 2 : 0));
        assert(!fdt_getprop(dt, fdt_path_offset(dt, "/soc/@NODE@"),
                            "apple,firmware-iovas", NULL));
        assert(!marker());
        printf("refused (%s, %s DT): ok\n", what, form ? "placeholder" : "older");
    }
}

static void no_ane0(void) { have_ane0 = false; }
static void not_preloaded(void) { preloaded = 0; }
static void three_segments(void) { segments_len = sizeof(segments) + 32; }
static void wrong_names(void) { segment_names = "__DATA;__TEXT"; }
static void unterminated_names(void) { segment_names = "__TEXT;__DATAX"; names_len = 14; }
static void above_os(void) { segments[1].phys = cur_boot_args.phys_base - 0x4000; }
static void below_dram(void) { segments[0].phys = dram_base - 0x100000; }
static void unaligned(void) { segments[0].phys += 0x1000; }
static void no_remap(void) { segments[1].remap = 0; }
static void overlap(void) { segments[1].phys = segments[0].phys + 0x4000; }
static void iova_overlap(void) { segments[1].remap = segments[0].remap; }
static void empty(void) { segments[0].size = 0; }

int main(void)
{
    /* Shipping DT: disabled node, placeholders filled in place, status kept. */
    reset_adt();
    build(true, "disabled", true);
    assert(dt_set_ane_firmware() == 0);
    check_handoff("disabled");
    printf("placeholders filled, node stays disabled: ok\n");

    /* An enabled node is handed over and stays enabled. */
    reset_adt();
    build(true, "okay", true);
    assert(dt_set_ane_firmware() == 0);
    check_handoff("okay");
    printf("enabled node keeps okay: ok\n");

    /* An older DT without memory regions gets them added. */
    reset_adt();
    build(false, "okay", true);
    assert(dt_set_ane_firmware() == 0);
    check_handoff("okay");
    printf("older DT gets regions added: ok\n");

    /* Names without the terminating NUL are accepted. */
    reset_adt();
    names_len = 13;
    build(true, "okay", true);
    assert(dt_set_ane_firmware() == 0);
    check_handoff("okay");
    printf("unterminated ADT names: ok\n");

    /* No ANE node: the DT is not touched. */
    reset_adt();
    build(true, "okay", false);
    memcpy(before, blob, sizeof(blob));
    assert(dt_set_ane_firmware() == 0);
    assert(!memcmp(before, blob, sizeof(blob)));
    printf("no ANE node, DT unchanged: ok\n");

    /* Under a hypervisor (EL1 guest) the regions are handed over without the marker. */
    reset_adt();
    el2 = false;
    build(true, "okay", true);
    assert(dt_set_ane_firmware() == 0);
    assert(!marker());
    assert(fdt_getprop(dt, fdt_path_offset(dt, "/soc/@NODE@"), "apple,firmware-iovas", NULL));
    printf("EL1 guest: regions handed over, no cold-boot marker: ok\n");

    check_refused(no_ane0, "no ADT node");
    check_refused(not_preloaded, "not preloaded");
    check_refused(three_segments, "three segments");
    check_refused(wrong_names, "segment order");
    check_refused(unterminated_names, "segment names");
    check_refused(above_os, "segment reaches the OS");
    check_refused(below_dram, "segment below DRAM");
    check_refused(unaligned, "unaligned segment");
    check_refused(no_remap, "no device address");
    check_refused(overlap, "overlapping segments");
    check_refused(iova_overlap, "overlapping device addresses");
    check_refused(empty, "empty segment");

    /* A placeholder phandle outside /reserved-memory is refused. */
    reset_adt();
    build(true, "okay", true);
    int soc = fdt_path_offset(dt, "/soc");
    int stray = fdt_add_subnode(dt, soc, "stray");
    assert(stray >= 0 && !fdt_setprop_u32(dt, stray, "phandle", 0x20));
    fdt32_t regions[2] = {cpu_to_fdt32(0x10), cpu_to_fdt32(0x20)};
    assert(!fdt_setprop(dt, fdt_path_offset(dt, "/soc/@NODE@"), "memory-region", regions,
                        sizeof(regions)));
    assert(dt_set_ane_firmware() == 0);
    assert(!strcmp(status(), "disabled"));
    printf("stray placeholder refused: ok\n");
    return 0;
}
'''
    for key, value in (("@ADT@", b["adt"]), ("@COMPAT@", b["compat"]), ("@NODE@", b["node"]),
                       ("@TEXTN@", "%x" % b["text"]), ("@DATAN@", "%x" % b["data"]),
                       ("@TEXT@", "0x%x" % b["text"]), ("@DATA@", "0x%x" % b["data"])):
        program = program.replace(key, value)
    run_c(tmp_path, "ane-preload-" + board, program)
