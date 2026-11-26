# SPDX-License-Identifier: MIT
"""dt_set_display()'s T8132 route, from kboot.c's own text.

The external processors follow the internal M4 DCP handoff's reported outcome: there is no root
compatible, chip id or OS release test in the route. dt_set_dcpext_handoff() is given that outcome
whatever it is, with kboot.c's firmware-27 clocks rule (dt_dcp_fw_clocks()). In the ESP Stage 2 it
takes every processor of firmware given its clocks: 1, or -1 if the memory of one it did not hand
over could not all be kept. A Stage 1 has no such handoff (kboot.h's stub declines): on firmware
given its clocks it keeps the memory of every processor the ADT describes (dt_keep_dcpext(), shared
with the ESP Stage 2) after withdrawing what the input tree promised of them, and does not boot if
that memory cannot all be kept. Older firmware keeps the 26.6.2 firmware-ready path
(dt_reserve_j713_dcpext()) after an internal handoff that was not refused.

test_route checks the route's calls against stubs. test_stage1_fdt runs the route with kboot.c's
own firmware-ready publication and kboot_display_handoff.c's own dt_keep_dcpext() (built without
ESP_STAGE2), on tests/t8132_dcpext_host.c's T8132 ADT, PMGR and DART registers and page tables, on
a J713 and a J773g device tree (those of the candidate kernel when J713_DTB and J773G_DTB name
them, else the host's), and checks what each ABI leaves in the tree.
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest
from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def route_block():
    kboot = (ROOT / "src/kboot.c").read_text()
    body = kboot[kboot.index("static int dt_set_display(void)\n{"):]
    start = body.index("    if (chip_id == T8132) {")
    end = body.index("\n    }\n", start) + len("\n    }\n")
    return body[start:end]


def internal_enum():
    header = (ROOT / "src/kboot.h").read_text()
    start = header.index("enum dcp_internal {")
    return header[start:header.index("\n};", start) + 3]


def stage1_stub():
    """kboot.h's dt_set_dcpext_handoff() for a build without ESP_STAGE2 (Stage 1)."""
    header = (ROOT / "src/kboot.h").read_text()
    start = header.index("#ifdef ESP_STAGE2\nint dt_set_dcpext_handoff(")
    stub = header[header.index("#else\n", start) + len("#else\n"):header.index("#endif", start)]
    assert "dt_set_dcpext_handoff" in stub
    return stub


def test_route_has_no_board_or_release_test():
    block = route_block()
    for banned in ("fdt_node_check_compatible", "apple,j", "os_firmware", "firmware_os_components",
                   "V27_", "V26_"):
        assert banned not in block, banned


# (M4 return, M4 outcome, firmware given clocks, dcpext handoff's result, keep's result)
# -> (route result, keep asked, firmware-ready ran)
# The ESP Stage 2's handoff claims exactly the firmware given clocks (1, or -1 if it could not keep
# a processor's memory); kboot.h's Stage 1 stub none.
CASES = [
    (0, "DCP_INTERNAL_COMPLETE", 1, 1, 0, 0, False, False),    # J713 27.2, ESP Stage 2
    (0, "DCP_INTERNAL_NONE", 1, 1, 0, 0, False, False),        # 27.2, walk failed
    (-1, "DCP_INTERNAL_NONE", 1, 1, 0, 0, False, False),       # refused internal handoff
    (-1, "DCP_INTERNAL_NO_PANEL", 1, 1, 0, 0, False, False),   # J773g 27.2: headless
    (0, "DCP_INTERNAL_COMPLETE", 1, -1, 0, -1, False, False),  # ESP Stage 2 could not keep
    (-1, "DCP_INTERNAL_NO_PANEL", 1, -1, 0, -1, False, False),
    (0, "DCP_INTERNAL_COMPLETE", 1, 0, 0, 0, True, False),     # Stage 1: kept
    (0, "DCP_INTERNAL_COMPLETE", 1, 0, -1, -1, True, False),   # Stage 1: could not keep
    (-1, "DCP_INTERNAL_NONE", 1, 0, 0, 0, True, False),        # Stage 1, refused: kept
    (-1, "DCP_INTERNAL_NO_PANEL", 1, 0, -1, -1, True, False),  # Stage 1 J773g: could not keep
    (0, "DCP_INTERNAL_COMPLETE", 0, 0, 0, 0, False, True),     # 26.6.2: firmware-ready
    (0, "DCP_INTERNAL_NONE", 0, 0, 0, 0, False, True),         # 26.6.2 without the maps
    (-1, "DCP_INTERNAL_NO_PANEL", 0, 0, 0, 0, False, False),   # J773g 26.6.2: nothing
    (-1, "DCP_INTERNAL_NONE", 0, 0, 0, 0, False, False),       # refused: nothing
]


@pytest.mark.parametrize("m4_ret,outcome,fw_clocks,claims,kept,ret,asked,ready", CASES)
def test_route(tmp_path, m4_ret, outcome, fw_clocks, claims, kept, ret, asked, ready):
    source = f'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define T8132 0x8132
{internal_enum()}
static int chip_id = T8132, seen = -1, ready_calls, keep_calls;
static void *dt;
static int dt_set_m4_dcp_handoff(enum dcp_internal *internal)
{{
    *internal = {outcome};
    return {m4_ret};
}}
static bool dt_dcp_fw_clocks(void) {{ return {fw_clocks}; }}
static int dt_set_dcpext_handoff(void *tree, enum dcp_internal internal, bool fw_clocks)
{{
    (void)tree;
    assert(fw_clocks == {fw_clocks});
    seen = internal;
    return {claims};
}}
static void dt_reserve_j713_dcpext(void) {{ ready_calls++; }}
static int dt_keep_dcpext(void *tree) {{ (void)tree; keep_calls++; return {kept}; }}
static int route(void)
{{
{route_block()}
    return 1;
}}
int main(void)
{{
    assert(route() == {ret});
    assert(seen == {outcome});
    assert(keep_calls == {1 if asked else 0});
    assert(ready_calls == {1 if ready else 0});
    return 0;
}}
'''
    (tmp_path / "route.c").write_text(source)
    binary = tmp_path / "route"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    str(tmp_path / "route.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def test_stage1_stub_declines():
    """kboot.h's Stage 1 stub declines whatever the firmware: the route decides from fw_clocks."""
    stub = stage1_stub()
    assert stub.count("return") == 1 and "return 0;" in stub


# ---- the route on a device tree, with kboot.c's and kboot_display_handoff.c's own code ----


def kboot_function(name, kind="static int ", optional=False):
    source = (ROOT / "src/kboot.c").read_text()
    if optional and kind + name + "(" not in source:
        return ""
    start = source.index(kind + name + "(")
    return source[start:source.index("\n}", start) + 2] + "\n"


def kboot_aliases():
    source = (ROOT / "src/kboot.c").read_text()
    start = source.index("static const char dcpext_aliases")
    return source[start:source.index("\n};", start) + 3] + "\n"


STAGE1_FDT = r'''
/* The T8132 ADT, PMGR and DART registers of the ADT handoff's host test, without its main */
#define T8132_DCPEXT_HOST_WORLD_ONLY
#include "t8132_dcpext_host.c"

/* ---- kboot.c, as far as dt_set_display()'s T8132 route reaches ---- */
#define T8132 0x8132
#define T8132_DCPEXT_READY READY
#ifndef bail
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#endif
/* The versions dt_set_dcp_firmware() maps that the host's firmware.h does not have */
enum { V12_3 = 1, V12_3_1, V12_4, V13_5B4, V13_6_1 };
/* As src/adt.h's, on the host ADT */
static int host_getprop_copy(int node, const char *name, void *out, size_t len)
{
    u32 got = 0;
    const void *p = adt_getprop(adt, node, name, &got);
    if (!p || got != len)
        return -1;
    memcpy(out, p, len);
    return len;
}
#define ADT_GETPROP(a, node, name, val) host_getprop_copy(node, name, (val), sizeof(*(val)))
static void *dt;
static bool chosen_lost;
/* kboot.h's handoff for a build without ESP_STAGE2 (the host's kboot.h declares the real one) */
#define dt_set_dcpext_handoff stage1_dcpext_handoff
@STUB@

/* The T8122/J613 native26 path is never taken on T8132 (native26 false). */
static void dt_ext26_step(const char *step) { (void)step; abort(); }
static int dt_ext26_snapshot(const char *alias) { (void)alias; abort(); }
static int dt_ext26_reserve(const char *alias) { (void)alias; abort(); }
static int dt_ext26_retain_cpu(const char *alias) { (void)alias; abort(); }
static int dt_t8122_dcpext_reserve(const char *p, const char *a) { (void)p; (void)a; abort(); }
/*
 * The T8122 path's device tree driven DART walker, which an earlier Stage 1 keep also called: as
 * if it kept every table, its best case.
 */
static int inherited_calls;
static int dt_dcpext_inherited_darts(const char *alias) { (void)alias; inherited_calls++; return 0; }

/* The 26.6.2 path's segment reservation (tested elsewhere): its pages, tied to the node */
static int dt_get_or_add_reserved_mem(const char *node_name, const char *compat, bool nomap,
                                      u64 paddr, size_t size);
static int dt_reserve_asc_firmware(const char *adt_path, const char *alt, const char *fdt_path,
                                   bool remap, u64 base)
{
    (void)alt; (void)remap; (void)base;
    u32 len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, adt_path_offset(adt, adt_path),
                                                       "segment-ranges", &len);
    for (u32 i = 0; seg && i < len / sizeof(*seg); i++) {
        char name[64];
        snprintf(name, sizeof(name), "asc-firmware@%llx", (unsigned long long)seg[i].phys);
        int mem = dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, seg[i].phys,
                                             ALIGN_UP((u64)seg[i].size, SZ_16K));
        if (mem < 0 || fdt_setprop_empty(dt, mem, "iommu-addresses") ||
            fdt_appendprop_u32(dt, fdt_path_offset(dt, fdt_path), "memory-region",
                               fdt_get_phandle(dt, mem)))
            return -1;
    }
    return 0;
}
/* The 26.6.2 path's PMGR check by name (the host PMGR has no names to look up) */
static bool dt_j713_dcpext_powered(const char *alias) { (void)alias; return true; }

@FUNCTIONS@

/* kboot.c's M4 internal handoff, as far as the route depends on it */
static enum dcp_internal m4_outcome;
static int m4_ret;
static int dt_set_m4_dcp_handoff(enum dcp_internal *internal)
{
    *internal = m4_outcome;
    if (m4_outcome == DCP_INTERNAL_COMPLETE) {
        const char *path = fdt_get_alias(dt, "dcp");
        int dcp = path ? fdt_path_offset(dt, path) : -1;
        char name[64];
        snprintf(name, sizeof(name), "asc-firmware@%llx", (unsigned long long)dcp_segs[0].phys);
        if (dcp < 0 || fdt_setprop_string(dt, dcp, "apple,firmware-uuid", image) ||
            dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, dcp_segs[0].phys,
                                       ALIGN_UP((u64)dcp_segs[0].size, SZ_16K)) < 0) {
            printf("FAIL the M4 model\n");
            exit(1);
        }
    }
    return m4_ret;
}
static bool fw_clocks;
static bool dt_dcp_fw_clocks(void) { return fw_clocks; }

static int route(void)
{
@ROUTE@
    return 1;
}

/* ---- checks ---- */

static int failures;
static void expect(bool ok, const char *what)
{
    printf("%s %s\n", ok ? "ok" : "FAIL", what);
    failures += !ok;
}

static int ext_node(int i)
{
    int node = -1;
    for (int k = 0; k <= i; k++)
        node = fdt_node_offset_by_compatible(dt, node, T8132_EXT);
    return node;
}

/* What the kernel adopts an external processor by */
static const char *const promises[] = {READY, "apple,firmware-uuid", "apple,firmware-version",
                                       "apple,firmware-compat", "apple,clock-ids"};
static bool promised(int node)
{
    for (size_t i = 0; i < ARRAY_SIZE(promises); i++)
        if (fdt_getprop(dt, node, promises[i], NULL))
            return true;
    return false;
}

static int mr_count(const void *tree, int node)
{
    int len = 0;
    return fdt_getprop(tree, node, "memory-region", &len) ? len / 4 : 0;
}

/* /reserved-memory nodes whose reg starts at phys */
static int reserved_at(u64 phys)
{
    int parent = fdt_path_offset(dt, "/reserved-memory"), node, n = 0;
    fdt_for_each_subnode(node, dt, parent) {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        n += reg && len >= 16 && fdt64_ld(reg) == phys;
    }
    return n;
}

static void dump(void)
{
    for (int i = 0; i < 2; i++) {
        int node = ext_node(i), off;
        char path[96] = "?";
        fdt_get_path(dt, node, path, sizeof(path));
        printf("%s (%d memory-region):", path, mr_count(dt, node));
        fdt_for_each_property_offset(off, dt, node) {
            const char *name;
            fdt_getprop_by_offset(dt, off, &name, NULL);
            for (size_t k = 0; k < ARRAY_SIZE(promises); k++)
                if (!strcmp(name, promises[k]))
                    printf(" %s", name);
        }
        printf("\n");
    }
}

/* iBoot left external processor i off with its pipe: CPU, FE and SYS power-gated, targeted so */
static void processor_off(int i)
{
    for (int k = 0; k < 3; k++)
        poke32(ps_reg(pm_sys(i) + k), 0x0);
}

static bool has(const char *s, const char *part)
{
    return strstr(s, part) != NULL;
}

/* The boots that must not start the OS: memory a processor may still use cannot all be kept */
static bool stage1_no_boot(const char *s)
{
    return is(s, "fw27-overlap") || is(s, "fw27-pipe-left-on") || is(s, "fw27-cpu-off-no-parent") ||
           is(s, "fw27-power-unknown") || is(s, "fw27-pmgr-faults") ||
           is(s, "fw27-no-clock-gates") || is(s, "fw27-dart-faults") ||
           is(s, "fw27-refused-dt-full") || has(s, "-no-mappers");
}

/* External processor i's __DATA and heap moved into the RAM the OS gets, apart */
static u64 seg_in_ram(int i, int k)
{
    return RAM_SEGS + 0x2000000ULL * i + (k == 3 ? 0x400000 : 0);
}
static void segs_in_ram(int i)
{
    ext_seg_move(i, 1, seg_in_ram(i, 1));
    ext_seg_move(i, 3, seg_in_ram(i, 3));
}

/* A 16K page an enabled no-map /reserved-memory node holds */
static bool page_kept(u64 page)
{
    int resv = fdt_path_offset(dt, "/reserved-memory"), node;
    fdt_for_each_subnode(node, dt, resv) {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (!reg || len < 16 || !fdt_getprop(dt, node, "no-map", NULL) ||
            (status && !strcmp(status, "disabled")))
            continue;
        u64 base = fdt64_ld(&reg[0]), size = fdt64_ld(&reg[1]);
        if (page >= base && page + 0x4000 <= base + size)
            return true;
    }
    return false;
}

/* Every page of external processor i's ADT firmware segments in the OS's RAM is kept, but `skip` */
static bool segs_kept(int i, u64 skip)
{
    char nub[64];
    u32 len = 0;
    adt_path_of(nub, sizeof(nub), "/arm-io/dcpext%d/iop-dcpext%d-nub", i);
    const u8 *raw = adt_getprop(adt, adt_path_offset(adt, nub), "segment-ranges", &len);
    for (u32 k = 0; raw && k < len / sizeof(struct seg); k++) {
        struct seg sg;
        memcpy(&sg, raw + k * sizeof(sg), sizeof(sg));
        for (u64 page = sg.phys & ~0x3fffULL; page < ALIGN_UP(sg.phys + sg.size, 0x4000);
             page += 0x4000)
            if (page >= ram_base && page + 0x4000 <= ram_base + ram_size && page != skip &&
                !page_kept(page))
                return false;
    }
    return raw != NULL;
}

int main(int argc, char **argv)
{
    static _Alignas(8) u8 tree[1 << 20], in[1 << 20];
    if (argc != 4) {
        fprintf(stderr, "usage: %s j713|j773g <dtb>|- <scenario>\n", argv[0]);
        return 2;
    }
    const char *board = argv[1], *file = argv[2], *s = argv[3];
    bool j773g = !strcmp(board, "j773g"), legacy = !strncmp(s, "fw26", 4);
    printf("%s on %s (%s)\n", s, board, file);

    layout272 = true;
    dcp_segs = j713_272_segs;
    dcp_nsegs = ARRAY_SIZE(j713_272_segs);
    image = legacy ? UUID262 : UUID27;
    world_t8132(j773g, image);
    if (legacy)
        os_firmware =
            (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
    fw_clocks = !legacy;

    if (!strcmp(file, "-")) {
        /* The host's J713 or J773g tree: the candidate kernel's external nodes, as it builds them */
        build_t8132(tree, j773g ? J773G : J713, j773g ? sizeof(J773G) : sizeof(J713), sizeof(tree));
        for (int i = 0; i < 2; i++)
            build_t8132_ext(tree, i, T8132_EXT, sizeof(T8132_EXT), !j773g, true);
    } else {
        static u8 blob[1 << 20];
        FILE *f = fopen(file, "rb");
        size_t got = f ? fread(blob, 1, sizeof(blob), f) : 0;
        if (f)
            fclose(f);
        if (!got || fdt_open_into(blob, tree, sizeof(tree))) {
            printf("FAIL cannot read %s\n", file);
            return 1;
        }
    }
    dt = tree;
    m4_outcome = j773g ? DCP_INTERNAL_NO_PANEL : DCP_INTERNAL_COMPLETE;
    m4_ret = j773g ? -1 : 0;
    if (has(s, "-refused"))
        m4_outcome = DCP_INTERNAL_NONE, m4_ret = -1;

    /*
     * The machine. iBoot leaves both processors running (as the J713's iBoot does) unless a
     * scenario says otherwise; their __DATA and heap, and their firmware and pipe DART tables, are
     * in the RAM the OS gets.
     */
    size_t slen = strlen(s);
    bool off = slen > 4 && !strcmp(s + slen - 4, "-off");
    bool runs[2] = {!off, !off};
    if (off || is(s, "fw27-one-running") || is(s, "fw27-power-unknown") ||
        is(s, "fw27-pmgr-faults") || is(s, "fw27-no-clock-gates"))
        processor_off(0), runs[0] = false;
    if (off)
        processor_off(1);
    if (!legacy && !off)
        for (int i = 0; i < 2; i++) {
            segs_in_ram(i);
            ext_tables_in_ram(i);
        }
    if (is(s, "fw27-pipe-left-on"))
        /* Both CPUs are off but their pipes' FE and SYS are on; no DART of theirs answers */
        for (int i = 0; i < 2; i++) {
            poke32(ps_reg(pm_sys(i) + 2), 0x0);
            region_of(ext_dart[i])->base = 0x7e0000000ULL + 0x200000ULL * i;
            region_of(pipe_dart[i])->base = 0x7e0100000ULL + 0x200000ULL * i;
        }
    if (is(s, "fw27-cpu-off-no-parent"))
        /*
         * Both CPUs are off and the PMGR table gives them no parent: their pipes' FE and SYS are
         * on and their DARTs answer, translating through tables in the OS's RAM
         */
        for (int i = 0; i < 2; i++) {
            u16 cpu = pm_sys(i) + 2;
            poke32(ps_reg(cpu), 0x0);
            for (int k = 0; k < nfdevs; k++)
                if (fdevs[k].id == cpu)
                    fdevs[k].parent[0] = fdevs[k].parent[1] = 0;
        }
    if (is(s, "fw27-power-unknown"))
        poke32(ps_reg(pm_sys(1) + 2), 0x44); /* clock-gated, targeted so: neither on nor off */
    if (is(s, "fw27-pmgr-faults"))
        region_of(T8132_PMGR)->base = 0x7f0000000ULL; /* nothing answers at the power states */
    if (is(s, "fw27-no-clock-gates"))
        prop_drop("/arm-io/dcpext1", "clock-gates");
    if (is(s, "fw27-dart-unlocked"))
        poke32(pipe_dart[1] + 0x200, 0x1e0); /* unlocked with a live stream: still walked */
    if (is(s, "fw27-dart-faults"))
        region_of(ext_dart[1])->base = 0x7e0000000ULL; /* dcpext1's firmware DART: no answer */
    /* The ADT lists no mapper for dcpext1's pipe (or for dcpext1): that DART is not known */
    if (has(s, "-pipe-no-mappers"))
        prop_drop("/arm-io/dispext1", "iommu-parent");
    if (is(s, "fw27-ext-no-mappers"))
        prop_drop("/arm-io/dcpext1", "iommu-parent");

    /* The device tree */
    if (has(s, "-no-alias")) {
        int aliases = fdt_path_offset(dt, "/aliases");
        if (aliases < 0 || fdt_delprop(dt, aliases, "dcpext1")) {
            printf("FAIL no dcpext1 alias to delete\n");
            return 1;
        }
    }
    if (has(s, "-no-dt-ext")) {
        /* The device tree has no external processor: the ADT's are kept all the same */
        int node;
        while ((node = fdt_node_offset_by_compatible(dt, -1, T8132_EXT)) >= 0)
            if (fdt_del_node(dt, node))
                return 1;
    }
    if (is(s, "fw27-overlap")) {
        /* A reservation the OS may map holds the second page of dcpext1's heap */
        int node = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), "other@0");
        fdt64_t reg[2] = {cpu_to_fdt64(seg_in_ram(1, 3) + 0x4000), cpu_to_fdt64(0x4000)};
        if (node < 0 || fdt_setprop(dt, node, "reg", reg, sizeof(reg)))
            return 1;
    }
    if (has(s, "-prepopulated")) {
        /* An input tree that already promises both processors, as a 26.6.2 or ESP Stage 2 run would */
        for (int i = 0; i < 2; i++) {
            int node = ext_node(i);
            fdt32_t ids[2] = {cpu_to_fdt32(pipe_clk[i][0]), cpu_to_fdt32(pipe_clk[i][1])};
            fdt32_t ver[3] = {cpu_to_fdt32(27), cpu_to_fdt32(2), 0};
            if (fdt_setprop_u32(dt, node, READY, 1) ||
                fdt_setprop_string(dt, ext_node(i), "apple,firmware-uuid", UUID27) ||
                fdt_setprop(dt, ext_node(i), "apple,firmware-version", ver, sizeof(ver)) ||
                fdt_setprop(dt, ext_node(i), "apple,firmware-compat", ver, sizeof(ver)) ||
                fdt_setprop(dt, ext_node(i), "apple,clock-ids", ids, sizeof(ids)))
                return 1;
            node = ext_node(i);
            int len = 0;
            const fdt32_t *mr = fdt_getprop(dt, node, "memory-region", &len);
            if (mr && fdt_stringlist_count(dt, node, "memory-region-names") >= 0) {
                u32 first = fdt32_ld(mr);
                for (int k = 0; k < 4; k++)
                    if (fdt_appendprop_u32(dt, ext_node(i), "memory-region", first))
                        return 1;
            }
        }
    }
    if (has(s, "-dt-full") && fdt_pack(dt)) /* no room left for a reservation */
        return 1;
    memcpy(in, tree, sizeof(in));
    int ins[2] = {ext_node(0), ext_node(1)};
    int writes = mmio_writes;

    int ret = route();
    dump();
    expect(mmio_writes == writes, "no MMIO write");
    bool faults = is(s, "fw27-pmgr-faults") || is(s, "fw27-dart-faults");
    expect(faults ? exc_count > 0 : exc_count == 0,
           "no register read faulted, but in the scenarios that make one fault");

    u64 text = dcp_segs[0].phys;
    struct seg es[2][4];
    ext_segs(0, es[0]);
    ext_segs(1, es[1]);
    if (legacy) {
        /* The 26.6.2 firmware-ready description, unchanged: on the J713 beside its panel */
        bool ran = !j773g && !has(s, "-refused");
        expect(ret == 0, "the route returns 0");
        for (int i = 0; i < 2; i++) {
            int node = ext_node(i), len = 0;
            const fdt32_t *ready = fdt_getprop(dt, node, READY, &len);
            const fdt32_t *ver = fdt_getprop(dt, node, "apple,firmware-version", NULL);
            const char *uuid = fdt_getprop(dt, node, "apple,firmware-uuid", NULL);
            expect(ran == (ready && len == 4 && fdt32_ld(ready) == 1), "the ready marker");
            expect(ran == (ver && fdt32_ld(ver) == 26 && uuid && !strcmp(uuid, UUID262)),
                   "the 26.6.2 image and firmware version");
            expect(mr_count(dt, node) == mr_count(in, ins[i]) + (ran ? 4 : 0),
                   "a memory-region entry per segment");
        }
        if (failures)
            return 1;
        printf("%s: pass\n", s);
        return 0;
    }

    /* Firmware given its clocks: nothing is promised of any processor, whatever the input said */
    for (int i = 0; i < 2; i++) {
        int node = ext_node(i);
        if (node < 0)
            continue;
        int named = fdt_stringlist_count(dt, node, "memory-region-names");
        expect(!promised(node), "no ready marker, uuid, firmware version, compat or clock ids");
        expect(mr_count(dt, node) == (named >= 0 ? named : mr_count(in, ins[i])),
               "memory-region: the ones the tree names");
    }
    expect(reserved_at(text) == (m4_outcome == DCP_INTERNAL_COMPLETE),
           "the shared __TEXT (below the OS's RAM) reserved by the internal handoff only");
    expect(inherited_calls == 0, "no device tree driven DART walk");
    if (stage1_no_boot(s)) {
        expect(ret == -1, "the route refuses: no boot");
        /* What can be kept is kept all the same */
        bool cpu_off = is(s, "fw27-pipe-left-on") || is(s, "fw27-cpu-off-no-parent");
        if (runs[0] && !has(s, "-dt-full") && !cpu_off)
            expect(segs_kept(0, 0) && ext_tables_kept(dt, 0), "dcpext0's memory is kept");
        if (cpu_off)
            expect(segs_kept(0, 0) && !resv_tables(dt),
                   "dcpext0's segments are kept; no DART is read, with only its CPU off");
        if (is(s, "fw27-overlap"))
            expect(segs_kept(1, seg_in_ram(1, 3) + 0x4000) &&
                       !page_kept(seg_in_ram(1, 3) + 0x4000) && ext_tables_kept(dt, 1),
                   "dcpext1's tables and every firmware page but the mappable one are kept");
        if (cpu_off || is(s, "fw27-power-unknown") || is(s, "fw27-no-clock-gates"))
            expect(segs_kept(1, 0) && !resv_page(dt, RAM_TABLES + 0x10000) &&
                       !resv_page(dt, RAM_TABLES + 0x18000),
                   "dcpext1's segments are kept; its DARTs are not read");
        if (is(s, "fw27-dart-faults") || is(s, "fw27-ext-no-mappers"))
            expect(segs_kept(1, 0) && resv_page(dt, RAM_TABLES + 0x18000) &&
                       !resv_page(dt, RAM_TABLES + 0x10000),
                   "dcpext1's segments and pipe tables are kept, not its own DART's");
        if (has(s, "-pipe-no-mappers"))
            expect(segs_kept(1, 0) && resv_page(dt, RAM_TABLES + 0x10000) &&
                       resv_page(dt, RAM_TABLES + 0x14000) && !resv_page(dt, RAM_TABLES + 0x18000) &&
                       resv_tables(dt) == 6,
                   "dcpext1's segments and own DART tables are kept, not its unnamed pipe DART's");
    } else if (!off) {
        /* Running, kept: it boots */
        int tables = 0;
        expect(ret == 0, "the route returns 0: boot");
        for (int i = 0; i < 2; i++) {
            if (!runs[i])
                continue;
            expect(segs_kept(i, 0), "its firmware segments in RAM are reserved, no-map");
            expect(ext_tables_kept(dt, i),
                   "its firmware and pipe DART table pages in RAM are reserved, no-map");
            tables += 4;
        }
        expect(resv_tables(dt) == tables, "no other table page is reserved");
        if (is(s, "fw27-one-running"))
            for (int k = 1; k < 4; k++)
                expect(!reserved_at(es[0][k].phys), "none of the off dcpext0's segments reserved");
    } else {
        /* None runs: it boots, with nothing of theirs reserved */
        expect(ret == 0, "the route returns 0: boot");
        expect(!resv_count(dt, "dcpext-"), "nothing of theirs is reserved");
        for (int i = 0; i < 2; i++)
            for (int k = 1; k < 4; k++)
                expect(!reserved_at(es[i][k].phys), "none of its own segments reserved");
    }
    if (failures)
        return 1;
    printf("%s: pass\n", s);
    return 0;
}
'''

FDT_SCENARIOS = [
    "fw26", "fw26-refused",
    # iBoot left them running (as the J713's iBoot does): kept, a Stage 1 boots
    "fw27", "fw27-refused", "fw27-one-running", "fw27-no-alias", "fw27-dart-unlocked",
    "fw27-prepopulated", "fw27-no-dt-ext",
    # running, and some of their memory cannot be kept: no boot
    "fw27-overlap", "fw27-pipe-left-on", "fw27-power-unknown", "fw27-pmgr-faults",
    "fw27-no-clock-gates", "fw27-dart-faults", "fw27-refused-dt-full",
    # their CPUs off, with no PMGR parent to tell their pipes' power: no boot
    "fw27-cpu-off-no-parent",
    # the ADT names no DART for a running processor's pipe (or the processor): no boot
    "fw27-pipe-no-mappers", "fw27-no-dt-ext-pipe-no-mappers", "fw27-ext-no-mappers",
    # iBoot left them off: a Stage 1 boots, promising and keeping nothing
    "fw27-off", "fw27-refused-off", "fw27-no-alias-off", "fw27-prepopulated-off",
]
NO_BOOT = {"fw27-overlap", "fw27-pipe-left-on", "fw27-cpu-off-no-parent", "fw27-power-unknown",
           "fw27-pmgr-faults", "fw27-no-clock-gates", "fw27-dart-faults", "fw27-refused-dt-full",
           "fw27-pipe-no-mappers", "fw27-no-dt-ext-pipe-no-mappers", "fw27-ext-no-mappers"}
BOARDS = {"j713": "J713_DTB", "j773g": "J773G_DTB"}


def stage1_fdt_source():
    functions = (kboot_aliases() +
                 kboot_function("dt_display_transaction") +
                 kboot_function("dt_get_or_add_reserved_mem") +
                 kboot_function("dt_set_dcp_firmware") +
                 kboot_function("dt_ext26_existing_reservation") +
                 kboot_function("dt_reserve_dcpext_ready") +
                 kboot_function("dt_reserve_j713_dcpext_one") +
                 kboot_function("dt_reserve_j713_dcpext", "static void ") +
                 kboot_function("dt_withdraw_j713_dcpext", optional=True) +
                 # an earlier Stage 1's keep, so that this test can be run on it
                 kboot_function("dt_keep_j713_dcpext_one", optional=True) +
                 kboot_function("dt_keep_j713_dcpext", "static void ", optional=True))
    return (STAGE1_FDT.replace("@STUB@", stage1_stub()).replace("@FUNCTIONS@", functions)
            .replace("@ROUTE@", route_block()))


@pytest.fixture(scope="module")
def stage1_fdt(tmp_path_factory):
    from test_t6041_display_handoff import LIBFDT
    cc = host_cc()
    if not shutil.which(cc[0]):
        pytest.skip("no host C compiler")
    tmp = tmp_path_factory.mktemp("stage1-fdt")
    src = tmp / "src"
    src.mkdir()
    for name in ("t6041_display_host.c", "t8132_dcpext_host.c"):
        shutil.copy(ROOT / "tests" / name, src)
    for header in (ROOT / "tests/t8122_display_host").glob("*.h"):
        shutil.copy(header, src)
    shutil.copy(ROOT / "src/kboot_display_handoff.c", src)
    (src / "stage1_route_host.c").write_text(stage1_fdt_source())
    binary = tmp / "stage1-fdt"
    # A Stage 1: kboot_display_handoff.c without ESP_STAGE2
    subprocess.run(
        [*cc, "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wno-unused-function",
         "-Wno-format", "-Wno-format-truncation", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined", "-DCHAINLOADING",
         "-I", str(src), "-I", str(ROOT / "src"), str(src / "stage1_route_host.c"),
         str(src / "kboot_display_handoff.c"),
         *(str(ROOT / "src/libfdt" / f) for f in LIBFDT), "-o", str(binary)],
        check=True)
    return binary


@pytest.mark.parametrize("board", sorted(BOARDS))
@pytest.mark.parametrize("scenario", FDT_SCENARIOS)
def test_stage1_fdt(stage1_fdt, board, scenario):
    """A Stage 1's route on a device tree: the 26.6.2 firmware-ready description for older
    firmware, unchanged. On firmware given its clocks, every processor the ADT describes that may
    still run keeps its firmware segments and its firmware and pipe DART table pages, found from
    the ADT whatever the aliases, the device tree's nodes, power domains or reservations say, and
    nothing is promised of any (even when the input tree had it): it boots. If some of that memory
    cannot be kept (a page a mappable reservation holds, a power state that does not show a
    running processor, a DART read that faults, no room), it does not boot, with what can be kept
    kept. A processor that is off with its pipe keeps nothing. DARTs are read only once the
    processor's power is checked, and no register is written."""
    tree = os.environ.get(BOARDS[board], "-")
    result = subprocess.run([str(stage1_fdt), board, tree, scenario], capture_output=True,
                            text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
    refused = ("external display memory that may still be in use could not all be kept from the "
               "OS (reasons in the DCP: dcpext lines above): not booting")
    assert (refused in result.stdout) == (scenario in NO_BOOT), result.stdout
    if scenario.startswith("fw27") and not scenario.endswith("-off"):
        assert "not handed over, may run: " in result.stdout, result.stdout
    if scenario.startswith("fw27"):
        assert "firmware reserved, left to the kernel" not in result.stdout
