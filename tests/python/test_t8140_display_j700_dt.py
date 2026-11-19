# SPDX-License-Identifier: MIT
"""Run the T8140 display hand-off on the composed J700 device tree.

The committed fixture is the display subgraph of the SiliconForest Linux J700
DTB (see tests/data/t8140-j700-display.dts). Set T8140_J700_DTB to a complete
composed DTB to run the same scenarios on it, so that suppliers shared with
unrelated consumers are covered as well.

The harness and its ADT, ASC and reservation mocks derive from
test_t8140_display_handoff.py.
"""

from pathlib import Path
import os
import shutil
import subprocess

import pytest


def _run(tmp_path, dtb):
    repo = Path(__file__).resolve().parents[2]
    source = Path(os.environ.get("T8140_HANDOFF_SOURCE", repo / "src/kboot.c")).read_text()
    start = source.index("struct dt_t8140_dram {")
    end = source.index("/* T8142 display carveouts:", start)
    helper = source[start:end]
    iommu_node = source[source.index("static int dt_get_iommu_node("):
                        source.index("static dart_dev_t *dt_init_dart_by_node(")]
    asc_source = (repo / "src/asc.c").read_text()
    asc_defs = asc_source[asc_source.index("#define ASC_CPU_CONTROL"):asc_source.index("struct asc_dev {")]
    asc_check = asc_source[asc_source.index("static bool asc_mailbox_empty("):asc_source.index("bool asc_can_recv(")]
    asc_header = (repo / "src/asc.h").read_text()
    enum_start = asc_header.index("enum asc_inherited_state {")
    asc_enum = asc_header[enum_start:asc_header.index("};", enum_start) + 2]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef unsigned long u64;
typedef uint32_t u32;
#define BIT(n) (1U << (n))
#define GENMASK(msb, lsb) ((BIT((msb) + 1 - (lsb)) - 1) << (lsb))
#define SZ_16K 16384
#define MAX_MEM_REGIONS 32
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define ADT_GETPROP_ARRAY(a, n, p, v) adt_array((n), (p), (v), sizeof(v))
#define ADT_GETPROP(a, n, p, v) adt_getprop_copy((a), (n), (p), (v), sizeof(*(v)))
struct adt_segment_ranges { u64 phys, iova, remap; u32 size, flags; };
struct disp_mapping {
    char region_adt[24], mem_fdt[24];
    bool map_dcp, map_disp, map_piodma;
};
struct mem_region { u64 paddr, size; };
struct run { u64 iova, size, pa; };
''' + asc_defs + r'''
typedef struct asc_dev { uintptr_t cpu_base, base; } asc_dev_t;
struct asc_mock {
    u32 control, status, a2i, i2a, recheck_status;
    bool stop_on_recheck, status_on_recheck;
    unsigned int cpu_reads, status_reads;
};
static struct asc_mock asc_state[2];
static asc_dev_t asc_devs[2] = {{0x1000, 0x9000}, {0x3000, 0xb000}};
static unsigned int polls;
static u64 timeout_calculate(u32 usec) { assert(usec == 200000); polls = 0; return 3; }
static bool timeout_expired(u64 deadline) { return ++polls >= deadline; }
static u32 read32(uintptr_t addr)
{
    unsigned int idx = ((addr >= 0x3000 && addr < 0x4000) || addr >= 0xb000) ? 1 : 0;
    struct asc_mock *state = &asc_state[idx];
    uintptr_t cpu = asc_devs[idx].cpu_base, box = asc_devs[idx].base;
    if (addr == cpu + ASC_CPU_CONTROL) {
        state->cpu_reads++;
        return state->stop_on_recheck && state->cpu_reads > 1 ? 0 : state->control;
    }
    if (addr == cpu + ASC_CPU_STATUS) {
        state->status_reads++;
        return state->status_on_recheck && state->status_reads > 1 ? state->recheck_status
                                                                    : state->status;
    }
    if (addr == box + ASC_MBOX_A2I_CONTROL) return state->a2i;
    if (addr == box + ASC_MBOX_I2A_CONTROL) return state->i2a;
    abort();
}
static asc_dev_t *asc_init(const char *path)
{
    if (!strcmp(path, "/arm-io/dcp")) return &asc_devs[0];
    if (!strcmp(path, "/arm-io/dcpext")) return &asc_devs[1];
    return NULL;
}
static void asc_free(asc_dev_t *asc) { (void)asc; }
static int quiesce_fault, table_fault, init_fault;
static unsigned int quiesce_calls;
static bool rtkit_quiesce_inherited(asc_dev_t *asc)
{
    quiesce_calls++;
    return quiesce_fault != (asc == &asc_devs[1] ? 2 : 1);
}

''' + asc_enum + '\n' + asc_check + r'''
static const struct run dcp[] = {
    {0x11c4000,0xc00000,0x101f0cd4000}, {0x1ddc000,0x4000,0x101f0cd0000},
    {0x1de0000,0xc0000,0x101f0c10000}, {0x1ea0000,0x160000,0x101eb4b8000},
    {0x2000000,0x2000000,0x101eb618000}, {0x4000000,0x2000000,0x101ed618000},
    {0x6000000,0x15f8000,0x101ef618000}, {0x75f8000,0x4d8000,0x10000234000},
    {0x7ad0000,0x530000,0x101ea7d4000}, {0x8000000,0x784000,0x101ead04000},
    {0x8784000,0x30000,0x101ea7a4000}, {0x87b4000,0x8000,0x101eb488000},
};
static const struct run disp[] = {
    {0x1ddc000,0x4000,0x101f0cd0000}, {0x1ea0000,0x160000,0x101eb4b8000},
    {0x2000000,0x2000000,0x101eb618000}, {0x4000000,0x2000000,0x101ed618000},
    {0x6000000,0x15f8000,0x101ef618000},
};
static const struct run ext[] = {
    {0x75f8000,0x4d8000,0x10000234000}, {0x7ad0000,0x530000,0x101e9ae8000},
    {0x8000000,0x784000,0x101ea018000}, {0x8784000,0x20000,0x101e9ac8000},
    {0x87a4000,0x8000,0x101ea79c000}, {0x87ac000,0xc00000,0x101e8ec8000},
};
static struct { const char *id; u64 pa, size; } carveouts[] = {
    {"region-id-14",0x101eb4b8000,0x5758000},
    {"region-id-49",0x10000234000,0x4d8000},
    {"region-id-50",0x101ea7d4000,0xcbc000},
    {"region-id-57",0x101f0cd4000,0xc00000},
    {"region-id-73",0x101e9ae8000,0xcbc000},
    {"region-id-74",0x101e8ec8000,0xc00000},
    {"region-id-94",0x101f0cd0000,0x4000},
    {"region-id-95",0x101f0c10000,0xc0000},
    {"region-id-233",0x101ea7a4000,0x30000},
    {"region-id-234",0x101e9ac8000,0x20000},
};
_Alignas(8) static unsigned char tree[262144];
static void *dt = tree, *adt;
static int dt_bufsize = sizeof(tree);
static struct {
    u64 phys_base, mem_size;
    struct { u64 base, stride, height; } video;
} cur_boot_args;
static u64 physical_base = 0x10000000000, physical_size = 0x200000000;
static u64 mem_size_actual = 0x200000000;
static bool hole_dcp_vram, hole_dcp_firmware, hole_ext, bad_carveout, bad_segment;
static int reservation_calls;
static bool ext_power = true;
static int pmgr_power_is_on(int die, const char *name)
{
    assert(die == 0 && !strcmp(name, "DISPEXT0_CPU"));
    return ext_power;
}

static u64 translate(const char *device, u64 iova)
{
    const struct run *runs;
    size_t count;
    if (!strcmp(device, "disp0")) { runs = disp; count = ARRAY_SIZE(disp); }
    else if (strstr(device, "ext") || strstr(device, "416e00000")) { runs = ext; count = ARRAY_SIZE(ext); }
    else { runs = dcp; count = ARRAY_SIZE(dcp); }
    if (hole_dcp_vram && runs == dcp && iova == 0x4000000) return 0;
    if (hole_dcp_firmware && runs == dcp && iova == 0x11c4000) return 0;
    if (hole_ext && runs == ext && iova == 0x87ac000) return 0;
    for (size_t i = 0; i < count; i++)
        if (iova >= runs[i].iova && iova - runs[i].iova < runs[i].size)
            return runs[i].pa + iova - runs[i].iova;
    return 0;
}
static u64 search_range(const char *device, u64 pa, u64 size)
{
    const struct run *runs;
    size_t count;
    if (!strcmp(device, "disp0")) { runs = disp; count = ARRAY_SIZE(disp); }
    else if (strstr(device, "ext") || strstr(device, "416e00000")) { runs = ext; count = ARRAY_SIZE(ext); }
    else { runs = dcp; count = ARRAY_SIZE(dcp); }
    for (size_t i = 0; i < count; i++) {
        if (pa < runs[i].pa || pa - runs[i].pa >= runs[i].size) continue;
        u64 iova = runs[i].iova + pa - runs[i].pa;
        bool complete = true;
        for (u64 off = 0; off < size; off += SZ_16K)
            if (translate(device, iova + off) != pa + off) complete = false;
        if (complete) return iova;
    }
    return UINT64_MAX;
}
static int adt_path_offset_trace(void *a, const char *path, int *trace)
{
    (void)a;
    if (strcmp(path, "/vram")) return -1;
    trace[0] = 1; trace[1] = 2; trace[2] = 0;
    return 2;
}
static int adt_get_reg(void *a, int *trace, const char *prop, int index, u64 *base, u64 *size)
{
    (void)a; (void)trace;
    assert(!strcmp(prop, "reg") && index == 0);
    *base = carveouts[0].pa; *size = carveouts[0].size;
    return 0;
}
static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/chosen/carveout-memory-map")) return 3;
    if (!strcmp(path, "/arm-io/dcp/iop-dcp-nub")) return 4;
    if (!strcmp(path, "/arm-io/dcpext/iop-dcpext-nub")) return 5;
    if (!strcmp(path, "/chosen")) return 6;
    return -1;
}
static int adt_getprop_copy(void *a, int node, const char *prop, void *out, size_t len)
{
    (void)a;
    if (node != 6 || len != sizeof(u64)) return -1;
    u64 value;
    if (!strcmp(prop, "dram-base")) value = physical_base;
    else if (!strcmp(prop, "dram-size")) value = physical_size;
    else return -1;
    memcpy(out, &value, len);
    return len;
}
static int adt_array(int node, const char *prop, void *out, size_t len)
{
    if (node != 3 || len != 16) return -1;
    for (size_t i = 0; i < ARRAY_SIZE(carveouts); i++) {
        if (strcmp(prop, carveouts[i].id)) continue;
        u64 pair[2] = {carveouts[i].pa, carveouts[i].size};
        if (bad_carveout && !strcmp(prop, "region-id-14")) pair[1] /= 2;
        memcpy(out, pair, len);
        return len;
    }
    return -1;
}
static const void *adt_getprop(void *a, int node, const char *prop, u32 *len)
{
    (void)a;
    static struct adt_segment_ranges seg[7];
    if ((node != 4 && node != 5) || strcmp(prop, "segment-ranges")) return NULL;
    memset(seg, 0, sizeof(seg));
    seg[0].phys = node == 4 ? carveouts[8].pa : carveouts[9].pa;
    seg[0].size = node == 4 ? carveouts[8].size : carveouts[9].size;
    seg[1].phys = node == 4 ? 0x101eb488000 : 0x101ea79c000;
    seg[1].size = bad_segment ? 0x4000 : 0x8000;
    if (node == 4) {
        seg[6].phys = carveouts[0].pa; seg[6].size = carveouts[0].size;
        *len = sizeof(seg);
    } else *len = 4 * sizeof(*seg);
    return seg;
}
static int dt_add_reserved_regions(const char *dcp_path, const char *disp_path,
                                   const char *piodma, const char *compat,
                                   struct disp_mapping *maps, struct mem_region *regions,
                                   u32 count)
{
    (void)piodma;
    assert(dcp_path && !strcmp(compat, "apple,asc-mem"));
    reservation_calls++;
    for (u32 i = 0; i < count; i++) {
        if (search_range(dcp_path, regions[i].paddr, regions[i].size) == UINT64_MAX ||
            (maps[i].map_disp && (!disp_path ||
             search_range(disp_path, regions[i].paddr, regions[i].size) == UINT64_MAX)))
            return -1;
        char name[64];
        snprintf(name, sizeof(name), "%s@%lx", maps[i].mem_fdt, (unsigned long)regions[i].paddr);
        int parent = fdt_path_offset(dt, "/reserved-memory");
        int n = fdt_subnode_offset(dt, parent, name);
        if (n < 0) n = fdt_add_subnode(dt, parent, name);
        assert(n >= 0);
        u64 reg[2] = {cpu_to_fdt64(regions[i].paddr), cpu_to_fdt64(regions[i].size)};
        assert(fdt_setprop(dt, n, "reg", reg, sizeof(reg)) == 0);
        assert(fdt_setprop_string(dt, n, "compatible", compat) == 0);
        assert(fdt_setprop_empty(dt, n, "no-map") == 0);
    }
    return 0;
}
static int dt_vram_reserved_region(const char *dcp_path, const char *disp_path)
{
    assert(!strcmp(dcp_path, "dcp") && !strcmp(disp_path, "disp0"));
    if (search_range(dcp_path, carveouts[0].pa, carveouts[0].size) == UINT64_MAX ||
        search_range(disp_path, carveouts[0].pa, carveouts[0].size) == UINT64_MAX)
        return -1;
    struct disp_mapping map = {0};
    struct mem_region region = {carveouts[0].pa, carveouts[0].size};
    strcpy(map.mem_fdt, "framebuffer");
    map.map_dcp = map.map_disp = true;
    reservation_calls++;
    int n = fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"),
                            "framebuffer@101eb4b8000");
    assert(n >= 0);
    u64 reg[2] = {cpu_to_fdt64(region.paddr), cpu_to_fdt64(region.size)};
    assert(fdt_setprop(dt, n, "reg", reg, sizeof(reg)) == 0);
    assert(fdt_setprop_string(dt, n, "compatible", "framebuffer") == 0);
    return 0;
}
static int dt_set_dcp_firmware(const char *path)
{
    (void)path;
    return 0;
}
''' + iommu_node + r'''
/* Locked DARTs by phandle and stream. A DART left out of `live` is not locked. */
typedef struct {u32 phandle, sid;} dart_dev_t;
static dart_dev_t mock_dart;
static u32 not_live[4];
static struct { u32 phandle, sid; } collected[64];
static size_t n_collected;
static dart_dev_t *dart_init_fdt_locked(void *blob, u32 phandle, u32 sid)
{
    assert(blob == dt && sid < 256);
    for (size_t i = 0; i < ARRAY_SIZE(not_live); i++)
        if (not_live[i] && not_live[i] == phandle)
            return NULL;
    mock_dart.phandle = phandle;
    mock_dart.sid = sid;
    return &mock_dart;
}
static void dart_shutdown(dart_dev_t *dart) { assert(dart == &mock_dart); }
static int dart_collect_locked_tables(dart_dev_t *dart, u64 base, u64 size,
                                     u64 *pages, size_t capacity, size_t *count)
{
    assert(base == physical_base && size == physical_size && capacity == 2049);
    assert(n_collected < ARRAY_SIZE(collected));
    collected[n_collected].phandle = dart->phandle;
    collected[n_collected++].sid = dart->sid;
    /* Two distinct table pages per stream, below the boot-args window. */
    pages[0] = base + ((dart->phandle & 0xff) * 8 + (dart->sid & 0x1f) * 2) * SZ_16K;
    pages[1] = pages[0] + SZ_16K;
    *count = 2;
    return 0;
}
static int dt_get_or_add_reserved_mem(const char *name, const char *compat, bool nomap,
                                      u64 base, u64 size)
{
    assert(!nomap && !strcmp(compat, "apple,firmware-dart-tables"));
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int n = fdt_subnode_offset(dt, parent, name);
    if (n < 0) n = fdt_add_subnode(dt, parent, name);
    if (n < 0) return n;
    u64 reg[] = {cpu_to_fdt64(base), cpu_to_fdt64(size)};
    if (fdt_setprop(dt, n, "reg", reg, sizeof(reg))) return -1;
    n = fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
    if (fdt_setprop_string(dt, n, "compatible", compat)) return -1;
    return fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
}
''' + ('#define HAVE_DISPLAY_RECHECK 1\n' if 'static int dt_t8140_recheck_display(' in source else '') + helper + r'''
#define DCP "/soc/dcp@412e00000"
#define DCPEXT "/soc/dcp@416e00000"
#define DISPLAY "/display-subsystem"
#define DCP_DART "/soc/iommu@412340000"
#define DISP_DART "/soc/iommu@412300000"
#define EXT_DART "/soc/iommu@416340000"
#define DISPEXT_DART "/soc/iommu@416300000"
#define DCP_MBOX "/soc/mailbox@412e08000"
#define EXT_MBOX "/soc/mailbox@416e08000"
#define PMGR "/soc/power-management@300700000"
#define ROUTE DCPEXT "/typec-routes/route@0"
static const char *const ext_power_paths[] = {
    PMGR "/power-controller@328", PMGR "/power-controller@320", PMGR "/power-controller@288",
};
static unsigned char original[sizeof(tree)];
static int off(const char *path)
{
    int n = fdt_path_offset(dt, path);
    if (n < 0) fprintf(stderr, "missing %s\n", path);
    assert(n >= 0);
    return n;
}
static const char *status_of(const char *path)
{
    const char *status = fdt_getprop(dt, off(path), "status", NULL);
    return status ? status : "okay";
}
static void expect(const char *path, const char *want)
{
    if (strcmp(status_of(path), want))
        fprintf(stderr, "%s: expected %s, got %s\n", path, want, status_of(path));
    assert(!strcmp(status_of(path), want));
}
static u32 phandle_of(const char *path) { return fdt_get_phandle(dt, off(path)); }
static bool marked(const char *path)
{
    int len;
    const fdt32_t *cell = fdt_getprop(dt, off(path), "apple,firmware-root-handoff", &len);
    return cell && len == sizeof(*cell) && fdt32_to_cpu(*cell) == 1;
}
static bool live_claim(void)
{
    return fdt_getprop(dt, off("/chosen"), "apple,dcp-rtkit-live", NULL) != NULL;
}
static u32 preflight(void)
{
    const fdt32_t *cell = fdt_getprop(dt, off("/chosen"), "aurora,external-display-preflight", NULL);
    assert(cell);
    return fdt32_to_cpu(*cell);
}
static bool was_collected(u32 phandle, u32 sid)
{
    for (size_t i = 0; i < n_collected; i++)
        if (collected[i].phandle == phandle && collected[i].sid == sid)
            return true;
    return false;
}
static bool touched(u32 phandle)
{
    for (size_t i = 0; i < n_collected; i++)
        if (collected[i].phandle == phandle)
            return true;
    return false;
}
/* Every (DART, stream) that an enabled consumer references has reserved tables. */
static void expect_reserved_streams(const char *consumer)
{
    if (strcmp(status_of(consumer), "okay"))
        return;
    int len;
    const fdt32_t *cells = fdt_getprop(dt, off(consumer), "iommus", &len);
    assert(cells && len % 8 == 0);
    for (int i = 0; i < len / 8; i++) {
        u32 phandle = fdt32_to_cpu(cells[2 * i]), sid = fdt32_to_cpu(cells[2 * i + 1]);
        if (!was_collected(phandle, sid))
            fprintf(stderr, "%s: stream %u of phandle %u has no reserved tables\n", consumer, sid, phandle);
        assert(was_collected(phandle, sid));
        int dart = fdt_node_offset_by_phandle(dt, phandle);
        const fdt32_t *mark = fdt_getprop(dt, dart, "apple,firmware-root-handoff", NULL);
        assert(mark && fdt32_to_cpu(*mark) == 1);
    }
}
static void expect_display_iommus(bool with_external)
{
    int len;
    const fdt32_t *cells = fdt_getprop(dt, off(DISPLAY), "iommus", &len);
    u32 want[] = {phandle_of(DISP_DART), 0, phandle_of(DISPEXT_DART), 0};
    int cells_wanted = with_external ? 4 : 2;
    if (!cells || len != cells_wanted * 4)
        fprintf(stderr, "display iommus: %d bytes, expected %d\n", cells ? len : -1, cells_wanted * 4);
    assert(cells && len == cells_wanted * 4);
    for (int i = 0; i < cells_wanted; i++)
        assert(fdt32_to_cpu(cells[i]) == want[i]);
}
static void setup(const char *dtb_path)
{
    FILE *f = fopen(dtb_path, "rb");
    assert(f);
    size_t size = fread(original, 1, sizeof(original), f);
    fclose(f);
    assert(size > 0 && size < sizeof(original) && fdt_check_header(original) == 0);
    assert(fdt_open_into(original, tree, sizeof(tree)) == 0);
    cur_boot_args.phys_base = 0x1000342c000;
    cur_boot_args.mem_size = 0x1e39e8000;
    physical_base = 0x10000000000;
    physical_size = 0x200000000;
    cur_boot_args.video.base = carveouts[0].pa;
    cur_boot_args.video.stride = 0x1788;
    cur_boot_args.video.height = 2416;
    reservation_calls = 0;
    quiesce_fault = table_fault = init_fault = 0;
    ext_power = true;
    hole_dcp_vram = hole_dcp_firmware = hole_ext = false;
    bad_carveout = bad_segment = false;
    memset(not_live, 0, sizeof(not_live));
    n_collected = 0;
    for (size_t i = 0; i < ARRAY_SIZE(asc_state); i++)
        asc_state[i] = (struct asc_mock){
            .control = 0x10, .status = 0x6d, .a2i = 0x2ff01, .i2a = 0x28801,
        };
    asc_state[1].status = 0x6e;
}
static void expect_internal_only(void)
{
    expect(DCP, "okay");
    expect(DCP_DART, "okay");
    expect(DISP_DART, "okay");
    expect(DISPLAY, "okay");
    assert(marked(DCP_DART) && marked(DISP_DART));
    expect_display_iommus(false);
    expect(DCPEXT, "disabled");
    expect(EXT_DART, "disabled");
    expect(DISPEXT_DART, "disabled");
    assert(!marked(EXT_DART) && !marked(DISPEXT_DART));
    expect(EXT_MBOX, "disabled");
    for (size_t i = 0; i < ARRAY_SIZE(ext_power_paths); i++)
        expect(ext_power_paths[i], "disabled");
    expect(ROUTE, "disabled");
    expect(DCP_MBOX, "okay");
    assert(live_claim());
    expect_reserved_streams(DCP);
    expect_reserved_streams(DCP "/piodma");
    expect_reserved_streams(DISPLAY);
}
static void expect_withdrawn(void)
{
    static const char *const owned[] = {
        DCP, DCPEXT, DISPLAY, DCP_DART, DISP_DART, EXT_DART, DISPEXT_DART, EXT_MBOX, ROUTE,
    };
    for (size_t i = 0; i < ARRAY_SIZE(owned); i++)
        expect(owned[i], "disabled");
    assert(!marked(DCP_DART) && !marked(DISP_DART) && !marked(EXT_DART) && !marked(DISPEXT_DART));
    assert(!live_claim());
    for (size_t i = 0; i < ARRAY_SIZE(ext_power_paths); i++)
        expect(ext_power_paths[i], "disabled");
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *dtb = argv[1];

    /* An idle external DCP is published with its complete dependency group. */
    setup(dtb);
    assert(dt_set_display_t8140() == 0);
    assert(dt_t8140_recheck_display() == 0);
    static const char *const enabled[] = {
        DCP, DCP_DART, DISP_DART, DISPLAY, DCPEXT, EXT_DART, DISPEXT_DART, EXT_MBOX, ROUTE,
    };
    for (size_t i = 0; i < ARRAY_SIZE(enabled); i++)
        expect(enabled[i], "okay");
    for (size_t i = 0; i < ARRAY_SIZE(ext_power_paths); i++)
        expect(ext_power_paths[i], "okay");
    expect_display_iommus(true);
    assert(marked(DCP_DART) && marked(DISP_DART) && marked(EXT_DART) && marked(DISPEXT_DART));
    static const char *const consumers[] = {DCP, DCP "/piodma", DISPLAY, DCPEXT, DCPEXT "/piodma"};
    for (size_t i = 0; i < ARRAY_SIZE(consumers); i++)
        expect_reserved_streams(consumers[i]);
    assert(live_claim() && preflight() == 0x100);
    printf("PASS external published\n");

    /* FDT reg values need only four-byte alignment, including regions
     * already reserved by an earlier stream. Force that legal layout before
     * re-running the real existing-reservation merge under UBSan. */
    u64 table_base = physical_base + ((phandle_of(DCP_DART) & 0xff) * 8 + 23 * 2) * SZ_16K;
    char table_path[128];
    snprintf(table_path, sizeof(table_path), "/reserved-memory/dart-tables@%lx", table_base);
    int table_node = off(table_path), table_len;
    const fdt64_t *table_reg = fdt_getprop(dt, table_node, "reg", &table_len);
    assert(table_reg && table_len == 2 * sizeof(*table_reg));
    if (!((uintptr_t)table_reg & 7))
        assert(fdt_setprop_empty(dt, table_node, "test-align-padding") == 0);
    table_reg = fdt_getprop(dt, off(table_path), "reg", &table_len);
    assert(((uintptr_t)table_reg & 7) == 4);
    struct dt_t8140_dram dram = {.base = physical_base, .size = physical_size};
    struct dt_t8140_external_darts ext_darts;
    assert(dt_t8140_collect_external_darts(&ext_darts) == 0);
    assert(dt_t8140_reserve_tables("dcp", &dram, &ext_darts, false) == 0);
    printf("PASS four-byte-aligned existing reservation\n");

    /* The DART helpers read <phandle stream> pairs; a wider provider fails closed. */
    static const char *const one_cell[] = {DISP_DART, EXT_DART, DCP_DART};
    for (size_t i = 0; i < ARRAY_SIZE(one_cell); i++) {
        setup(dtb);
        if (i == 0) {
            /* A published display audits clean until its DART widens. */
            assert(dt_set_display_t8140() == 0);
            assert(dt_t8140_audit_iommus(off(DISPLAY)) == 0);
        }
        assert(!fdt_setprop_u32(dt, off(one_cell[i]), "#iommu-cells", 2));
        n_collected = 0;
        if (i == 0)
            assert(dt_t8140_audit_iommus(off(DISPLAY)) == -1);
        if (i == 1)
            assert(dt_t8140_collect_external_darts(&ext_darts) == -1);
        if (i == 2) {
            ext_darts.count = 0;
            assert(dt_t8140_reserve_tables("dcp", &dram, &ext_darts, false) == -1);
        }
        assert(n_collected == 0);
    }
    printf("PASS two-cell DART specifiers fail closed\n");

    /* A running external DCP is refused. The internal display keeps a valid
     * provider group: only the external scanout specifier is withdrawn, and
     * the internal reservation never locks an external DART. */
    setup(dtb);
    asc_state[1].status = 0x6d;
    assert(dt_set_display_t8140() == 0);
    expect_internal_only();
    assert(!touched(phandle_of(EXT_DART)) && !touched(phandle_of(DISPEXT_DART)));
    assert(preflight() == 7);
    printf("PASS external running refused\n");

    /* External DARTs that firmware did not leave locked refuse only the
     * external display. */
    setup(dtb);
    not_live[0] = phandle_of(EXT_DART);
    not_live[1] = phandle_of(DISPEXT_DART);
    assert(dt_set_display_t8140() == 0);
    expect_internal_only();
    assert(preflight() == 8);
    printf("PASS external tables unavailable\n");

    setup(dtb);
    not_live[0] = phandle_of(DISPEXT_DART);
    assert(dt_set_display_t8140() == 0);
    expect_internal_only();
    printf("PASS external scanout tables unavailable\n");

    /* Power off before validation. */
    setup(dtb);
    ext_power = false;
    assert(dt_set_display_t8140() == 0);
    expect_internal_only();
    assert(preflight() == 1);
    printf("PASS external powered off\n");

    /* The external DCP leaves the idle state before kernel entry. */
    setup(dtb);
    assert(dt_set_display_t8140() == 0);
    asc_state[1].status = 0x6d;
    assert(dt_t8140_recheck_display() == 0);
    expect_internal_only();
    printf("PASS external recheck withdrawal\n");

    /* An internal failure withdraws the complete display, including the
     * published external group and every firmware-root claim. */
    setup(dtb);
    asc_state[0].status = 0x6f;
    assert(dt_set_display_t8140() == 0);
    expect_withdrawn();
    printf("PASS internal refusal\n");

    setup(dtb);
    assert(dt_set_display_t8140() == 0);
    asc_state[0].status = 0x6f;
    assert(dt_t8140_recheck_display() == 0);
    expect_withdrawn();
    printf("PASS internal recheck withdrawal\n");

    /* A DART is never enabled for a consumer without reserved tables. */
    setup(dtb);
    unsigned char unchanged[sizeof(tree)];
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_t8140_enable_consumer("dcp") < 0);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    printf("PASS unreserved DART not enabled\n");

    /* Without the external PIODMA child, ownership of the external scanout
     * DART cannot be established. The display master would keep a DART that
     * is never published, so the handoff fails closed. */
    setup(dtb);
    assert(fdt_del_node(dt, off(DCPEXT "/piodma")) == 0);
    assert(dt_set_display_t8140() == -1);
    printf("PASS ambiguous external scanout fails closed\n");

    /* After an external withdrawal at the final recheck, the published
     * display is audited again. A DART that lost its reserved-table handoff
     * withdraws the whole display. */
    setup(dtb);
    assert(dt_set_display_t8140() == 0);
    assert(fdt_delprop(dt, off(DISP_DART), "apple,firmware-root-handoff") == 0);
    asc_state[1].status = 0x6d;
    assert(dt_t8140_recheck_display() == 0);
    expect_withdrawn();
    printf("PASS recheck audit\n");

    /* Each alias the handoff follows must name one absolute path. libfdt
     * follows a relative alias recursively, so anything else fails closed
     * before the first edit. */
    static const struct {
        const char *name, *value;
        int length;
    } bad_aliases[] = {
        {"dcpext", "dcpext", 0},
        {"dcp", "dcp", 0},
        {"disp0", "display-subsystem", 0},
        {"dcpext", "loop", 0},
        {"dcpext", "/soc/dcp@416e00000", 18},
        {"dcpext", "/soc/absent", 0},
        {"dcp", "", 0},
        {"dcp", DCPEXT, 0},
        {"dcp", DISPLAY, 0},
        {"dcpext", DCP, 0},
        {"dcpext", DISPLAY, 0},
        {"disp0", DCP, 0},
        {"disp0", DCPEXT, 0},
    };
    for (size_t i = 0; i < ARRAY_SIZE(bad_aliases); i++) {
        setup(dtb);
        int aliases = off("/aliases");
        if (bad_aliases[i].length)
            assert(!fdt_setprop(dt, aliases, bad_aliases[i].name, bad_aliases[i].value,
                                bad_aliases[i].length));
        else
            assert(!fdt_setprop_string(dt, aliases, bad_aliases[i].name, bad_aliases[i].value));
        if (!strcmp(bad_aliases[i].value, "loop"))
            assert(!fdt_setprop_string(dt, off("/aliases"), "loop", "dcpext"));
        memcpy(unchanged, tree, sizeof(tree));
        assert(dt_set_display_t8140() == -1);
        assert(!memcmp(unchanged, tree, sizeof(tree)));
        /* Refused before any reservation reads a DART. */
        assert(n_collected == 0 && reservation_calls == 0);
    }
    printf("PASS display alias contract\n");

    /* A second node of either controller role cannot be selected by alias. */
    static const char *const duplicate_roles[] = {"apple,t8140-dcp", "apple,t8140-dcpext"};
    for (size_t i = 0; i < ARRAY_SIZE(duplicate_roles); i++) {
        setup(dtb);
        int duplicate = fdt_add_subnode(dt, 0, "duplicate-controller");
        assert(duplicate >= 0);
        assert(!fdt_setprop_string(dt, duplicate, "compatible", duplicate_roles[i]));
        memcpy(unchanged, tree, sizeof(tree));
        assert(dt_set_display_t8140() == -1);
        assert(!memcmp(unchanged, tree, sizeof(tree)));
        assert(n_collected == 0 && reservation_calls == 0);
    }
    printf("PASS duplicate controller roles refused\n");

    /* The unique internal master is established by its complete IOMMU tuples. */
    setup(dtb);
    int tuple_length;
    const void *tuples = fdt_getprop(dt, off(DISPLAY), "iommus", &tuple_length);
    assert(tuples && tuple_length > 0 && tuple_length <= 256);
    unsigned char tuple_copy[256];
    memcpy(tuple_copy, tuples, tuple_length);
    int duplicate = fdt_add_subnode(dt, 0, "duplicate-master");
    assert(duplicate >= 0);
    assert(!fdt_setprop_string(dt, duplicate, "compatible", "apple,display-subsystem"));
    assert(!fdt_setprop(dt, off("/duplicate-master"), "iommus", tuple_copy, tuple_length));
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_set_display_t8140() == -1);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    assert(n_collected == 0 && reservation_calls == 0);
    printf("PASS duplicate internal master refused\n");

    /* A well-formed alias to an external-only master is still the wrong role. */
    setup(dtb);
    tuples = fdt_getprop(dt, off(DCPEXT "/piodma"), "iommus", &tuple_length);
    assert(tuples && tuple_length > 0 && tuple_length <= 256);
    memcpy(tuple_copy, tuples, tuple_length);
    int external_master = fdt_add_subnode(dt, 0, "external-master");
    assert(external_master >= 0);
    assert(!fdt_setprop_string(dt, external_master, "compatible", "apple,display-subsystem"));
    assert(!fdt_setprop(dt, off("/external-master"), "iommus", tuple_copy, tuple_length));
    /* The unrelated external-only master does not make the internal role ambiguous. */
    const char *absent = NULL;
    assert(dt_t8140_display_alias_roles(&absent) == 0 && !absent);
    assert(!fdt_setprop_string(dt, off("/aliases"), "disp0", "/external-master"));
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_set_display_t8140() == -1);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    assert(n_collected == 0 && reservation_calls == 0);
    printf("PASS external-only master cannot impersonate internal\n");

    /* One node cannot fill both controller roles. */
    setup(dtb);
    assert(!fdt_del_node(dt, off(DCPEXT)));
    static const char both_roles[] = "apple,t8140-dcp\0apple,t8140-dcpext";
    assert(!fdt_setprop(dt, off(DCP), "compatible", both_roles, sizeof(both_roles)));
    assert(!fdt_setprop_string(dt, off("/aliases"), "dcpext", DCP));
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_set_display_t8140() == -1);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    assert(n_collected == 0 && reservation_calls == 0);
    printf("PASS controller roles must be distinct\n");

    /* An absolute external alias names the same controller. */
    setup(dtb);
    assert(!fdt_setprop_string(dt, off("/aliases"), "dcpext", DCPEXT));
    assert(dt_set_display_t8140() == 0);
    expect(DCPEXT, "okay");
    expect_display_iommus(true);
    assert(live_claim() && preflight() == 0x100);
    printf("PASS absolute external alias\n");

    /* A tree that does not describe the internal display keeps simplefb, as
     * before the role check: the handoff is refused, nothing is reserved and
     * no DART is read. */
    const struct {
        const char *const *nodes, *const *aliases, *reason;
    } absent_cases[] = {
        {(const char *const[]){NULL}, (const char *const[]){"dcp", NULL}, "FDT dcp alias absent"},
        {(const char *const[]){NULL}, (const char *const[]){"disp0", NULL},
         "FDT disp0 alias absent"},
        {(const char *const[]){DCP, DCPEXT, DISPLAY, NULL},
         (const char *const[]){"dcp", "disp0", "disp0-piodma", NULL}, "FDT T8140 DCP absent"},
    };
    for (size_t i = 0; i < ARRAY_SIZE(absent_cases); i++) {
        setup(dtb);
        for (const char *const *n = absent_cases[i].nodes; *n; n++)
            assert(!fdt_del_node(dt, off(*n)));
        for (const char *const *a = absent_cases[i].aliases; *a; a++)
            assert(!fdt_delprop(dt, off("/aliases"), *a));
        assert(dt_set_display_t8140() == 0);
        assert(n_collected == 0 && reservation_calls == 0);
        assert(!live_claim());
        const char *why = fdt_getprop(dt, off("/chosen"), "apple,dcp-handoff-reason", NULL);
        assert(why && !strcmp(why, absent_cases[i].reason));
        static const char *const owned[] = {DCP, DCPEXT, DISPLAY, DCP_DART, DISP_DART,
                                            EXT_DART, DISPEXT_DART};
        for (size_t j = 0; j < ARRAY_SIZE(owned); j++)
            if (fdt_path_offset(dt, owned[j]) >= 0)
                expect(owned[j], "disabled");
        assert(!marked(DCP_DART) && !marked(DISP_DART));
    }
    printf("PASS absent display keeps simplefb\n");

    /* Removing the nodes but keeping their aliases is not an absent display:
     * the aliases no longer name a node, so the handoff fails closed. */
    setup(dtb);
    assert(!fdt_del_node(dt, off(DCPEXT)));
    assert(!fdt_del_node(dt, off(DISPLAY)));
    assert(!fdt_del_node(dt, off(DCP)));
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_set_display_t8140() == -1);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    printf("PASS dangling display aliases fail closed\n");

    /* An absent internal description does not excuse an inconsistent part that is present. */
    const struct {
        const char *name, *value;
    } bad_parts[] = {
        {"dcpext", "dcpext"}, {"dcpext", "/soc/absent"}, {"dcpext", DISPLAY},
        {"disp0", DCPEXT},    {"duplicate", NULL},
    };
    for (int without_controller = 0; without_controller < 2; without_controller++) {
        for (size_t i = 0; i < ARRAY_SIZE(bad_parts); i++) {
            setup(dtb);
            int aliases = off("/aliases");
            assert(!fdt_delprop(dt, aliases, "dcp"));
            if (without_controller) {
                assert(!fdt_delprop(dt, off("/aliases"), "disp0-piodma"));
                assert(!fdt_del_node(dt, off(DCP)));
            }
            if (bad_parts[i].value) {
                assert(!fdt_setprop_string(dt, off("/aliases"), bad_parts[i].name,
                                           bad_parts[i].value));
            } else {
                int second = fdt_add_subnode(dt, 0, "second-external");
                assert(second >= 0);
                assert(!fdt_setprop_string(dt, second, "compatible", "apple,t8140-dcpext"));
            }
            memcpy(unchanged, tree, sizeof(tree));
            assert(dt_set_display_t8140() == -1);
            assert(!memcmp(unchanged, tree, sizeof(tree)));
            assert(n_collected == 0 && reservation_calls == 0);
        }
    }
    printf("PASS absent internal display with inconsistent parts fails closed\n");

    /* Without the internal PIODMA child the scanout owner is unknown. */
    setup(dtb);
    assert(!fdt_del_node(dt, off(DCP "/piodma")));
    memcpy(unchanged, tree, sizeof(tree));
    assert(dt_set_display_t8140() == -1);
    assert(!memcmp(unchanged, tree, sizeof(tree)));
    printf("PASS missing internal PIODMA fails closed\n");

    /* A controller without a display pipeline: no PIODMA scanout, no master, no disp0. */
    for (int keep_piodma = 0; keep_piodma < 2; keep_piodma++) {
        setup(dtb);
        if (keep_piodma)
            assert(!fdt_delprop(dt, off(DCP "/piodma"), "iommus"));
        else
            assert(!fdt_del_node(dt, off(DCP "/piodma")));
        assert(!fdt_del_node(dt, off(DISPLAY)));
        assert(!fdt_delprop(dt, off("/aliases"), "disp0"));
        if (!keep_piodma)
            assert(!fdt_delprop(dt, off("/aliases"), "disp0-piodma"));
        assert(dt_set_display_t8140() == 0);
        assert(n_collected == 0 && reservation_calls == 0);
        const char *why = fdt_getprop(dt, off("/chosen"), "apple,dcp-handoff-reason", NULL);
        assert(why && !strcmp(why, "FDT disp0 alias absent"));
        expect(DCP, "disabled");
        assert(!live_claim());
    }
    printf("PASS controller without a display pipeline keeps simplefb\n");
    return 0;
}

'''
    source_file = tmp_path / "t8140-j700-dt.c"
    source_file.write_text(harness)
    binary = tmp_path / "t8140-j700-dt"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
         "-fsanitize=undefined", "-fno-sanitize-recover=undefined", "-I", str(libfdt),
         str(source_file), *map(str, sources), "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary), str(dtb)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.count("PASS ") == 24, result.stdout


def test_t8140_display_j700_fixture(tmp_path):
    if not shutil.which("dtc"):
        pytest.skip("dtc is required to compile the J700 device-tree fixture")
    repo = Path(__file__).resolve().parents[2]
    dtb = tmp_path / "t8140-j700-display.dtb"
    subprocess.run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", str(dtb),
                    str(repo / "tests/data/t8140-j700-display.dts")], check=True)
    _run(tmp_path, dtb)


def test_t8140_display_j700_composed_dtb(tmp_path):
    dtb = os.environ.get("T8140_J700_DTB")
    if not dtb:
        pytest.skip("set T8140_J700_DTB to a composed J700 DTB")
    _run(tmp_path, Path(dtb))
