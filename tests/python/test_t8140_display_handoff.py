# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the T8140 hand-off against measured synthetic DART translations."""

from pathlib import Path
import subprocess
import os


def test_t8140_display_handoff(tmp_path):
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
static unsigned char tree[65536];
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
    else if (strstr(device, "ext")) { runs = ext; count = ARRAY_SIZE(ext); }
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
    else if (strstr(device, "ext")) { runs = ext; count = ARRAY_SIZE(ext); }
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
static bool provider_present;
static int provider_bad = -1, provider_missing = -1;
static u32 provider_bad_size;
static unsigned char provider_bytes[5][52];
static const char *provider_names[] = {
    "power-lut-data-x", "power-lut-data-y", "power-lut-data-xindex",
    "power-lut-data-yindex", "power-lut-data-lut",
};
static const u32 provider_sizes[] = {4, 4, 52, 4, 52};
static const char *provider_no_space;
static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/chosen/carveout-memory-map")) return 3;
    if (!strcmp(path, "/arm-io/dcp/iop-dcp-nub")) return 4;
    if (!strcmp(path, "/arm-io/dcpext/iop-dcpext-nub")) return 5;
    if (!strcmp(path, "/chosen")) return 6;
    if (!strcmp(path, "/arm-io/disp0") && provider_present) return 7;
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
    if (node == 7) {
        for (int i = 0; i < 5; i++) {
            if (strcmp(prop, provider_names[i]) || i == provider_missing) continue;
            *len = i == provider_bad ? provider_bad_size : provider_sizes[i];
            return provider_bytes[i];
        }
        return NULL;
    }
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
typedef struct {u32 phandle, sid;} dart_dev_t;
static dart_dev_t mock_dart;
static dart_dev_t *dart_init_fdt_locked(void *blob, u32 phandle, u32 sid)
{
    assert(blob == dt && (sid == 0 || sid == 16 || sid == 23) &&
           (phandle == 100 || phandle == 101 || phandle == 104 || phandle == 105));
    if (init_fault && sid == 16)
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
    *count = 0;
    if (table_fault == ((dart->phandle == 100 || dart->phandle == 104) ? 1 : 2) ||
        (table_fault == 3 && dart->sid == 16)) return -1;
    unsigned offset = dart->phandle == 100 ? 0 : dart->phandle == 104 ? 4 :
                      dart->phandle == 101 ? 8 : 12;
    pages[0] = base + (offset + (dart->sid == 16 ? 2 : 0)) * SZ_16K;
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
static int provider_setprop(void *fdt, int node, const char *name, const void *value, int len)
{
    char path[64];
    /* Fail the last property so rollback must also discard a copied prefix. */
    if (provider_no_space && !strcmp(name, "apple,power-lut-data-lut") &&
        !fdt_get_path(fdt, node, path, sizeof(path)) && !strcmp(path, provider_no_space))
        return -FDT_ERR_NOSPACE;
    return fdt_setprop(fdt, node, name, value, len);
}
#define fdt_setprop provider_setprop
''' + ('#define HAVE_DISPLAY_RECHECK 1\n' if 'static int dt_t8140_recheck_display(' in source else '') + helper + r'''
#undef fdt_setprop
static void setup(void)
{
    provider_present = false;
    provider_bad = provider_missing = -1;
    provider_no_space = NULL;
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 52; j++) provider_bytes[i][j] = 0x81 + i + j;
    memset(tree, 0, sizeof(tree));
    assert(fdt_create_empty_tree(dt, sizeof(tree)) == 0);
    assert(fdt_add_subnode(dt, 0, "aliases") >= 0);
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
    assert(fdt_setprop_u32(dt, fdt_path_offset(dt, "/chosen"),
                           "apple,dcp-rtkit-live", 1) == 0);
    assert(fdt_setprop_u32(dt, fdt_path_offset(dt, "/chosen"),
                           "apple,dcp-rtkit-quiesced", 1) == 0);
    assert(fdt_add_subnode(dt, 0, "reserved-memory") >= 0);
    assert(fdt_add_subnode(dt, 0, "soc") >= 0);
    for (int i = 0; i < 4; i++) {
        const char *names[] = {"dcp", "disp0", "dcpext", "dispext0"};
        int soc = fdt_path_offset(dt, "/soc");
        assert(fdt_add_subnode(dt, soc, names[i]) >= 0);
        char path[64];
        snprintf(path, sizeof(path), "/soc/%s", names[i]);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), names[i], path) == 0);
        if (i == 0 || i == 2)
            assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "compatible",
                   i == 0 ? "apple,t8140-dcp" : "apple,t8140-dcpext") == 0);
        if (i == 1 || i == 3)
            assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "compatible",
                                      "apple,display-subsystem") == 0);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "status", "okay") == 0);
    }
    const char *refs[] = {"dart-dcp", "dart-dcpext", "mailbox-dcp", "mailbox-dcpext"};
    const char *aliases[] = {"dcp_dart", "dcpext_dart", "dcp_mbox", "dcpext_mbox"};
    for (int i = 0; i < 4; i++) {
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), refs[i]) >= 0);
        char path[64];
        snprintf(path, sizeof(path), "/soc/%s", refs[i]);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), aliases[i], path) == 0);
        assert(!fdt_setprop_string(dt, fdt_path_offset(dt, path), "compatible",
                                  i < 2 ? "apple,t8140-dart" : "apple,t8140-asc-mailbox"));
        assert(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "phandle", 100 + i) == 0);
        assert(fdt_setprop_u32(dt, fdt_path_offset(dt, path),
                               i < 2 ? "#iommu-cells" : "#mbox-cells", i < 2 ? 1 : 0) == 0);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "status", "okay") == 0);
    }
    for (int i = 0; i < 2; i++) {
        const char *name = i ? "/soc/dcpext" : "/soc/dcp";
        fdt32_t iommu[] = {cpu_to_fdt32(100 + i), 0};
        fdt32_t mbox[] = {cpu_to_fdt32(102 + i)};
        assert(fdt_setprop(dt, fdt_path_offset(dt, name), "iommus", iommu,
                           sizeof(iommu)) == 0);
        assert(fdt_setprop(dt, fdt_path_offset(dt, i ? "/soc/dispext0" : "/soc/disp0"),
                           "iommus", iommu, sizeof(iommu)) == 0);
        assert(fdt_setprop(dt, fdt_path_offset(dt, name), "mboxes", mbox,
                           sizeof(mbox)) == 0);
    }
    for (int i = 0; i < 2; i++) {
        const char *display = i ? "/soc/dispext0" : "/soc/disp0";
        const char *dcp = i ? "/soc/dcpext" : "/soc/dcp";
        const char *dart = i ? "dart-dispext" : "dart-disp";
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), dart) >= 0);
        char path[64];
        snprintf(path, sizeof(path), "/soc/%s", dart);
        assert(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "phandle", 104 + i) == 0);
        assert(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "#iommu-cells", 1) == 0);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "compatible", "apple,t8140-dart") == 0);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, path), "status", "okay") == 0);
        fdt32_t iommu[] = {cpu_to_fdt32(104 + i), 0};
        assert(fdt_setprop(dt, fdt_path_offset(dt, display), "iommus", iommu, sizeof(iommu)) == 0);
        iommu[0] = cpu_to_fdt32(100 + i); iommu[1] = cpu_to_fdt32(23);
        assert(fdt_setprop(dt, fdt_path_offset(dt, dcp), "iommus", iommu, sizeof(iommu)) == 0);
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, dcp), "piodma") >= 0);
        snprintf(path, sizeof(path), "%s/piodma", dcp);
        iommu[0] = cpu_to_fdt32(104 + i); iommu[1] = cpu_to_fdt32(16);
        assert(fdt_setprop(dt, fdt_path_offset(dt, path), "iommus", iommu, sizeof(iommu)) == 0);
    }
    for (int i = 0; i < 2; i++) {
        const char *path = i ? "/soc/dcpext" : "/soc/dcp";
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, path), "typec-routes") >= 0);
        char routes[64];
        snprintf(routes, sizeof(routes), "%s/typec-routes", path);
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, routes), "port0") >= 0);
        snprintf(routes, sizeof(routes), "%s/typec-routes/port0", path);
        assert(fdt_setprop_string(dt, fdt_path_offset(dt, routes), "status", "okay") == 0);
    }
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "usb-pd") >= 0);
    assert(fdt_setprop_string(dt, fdt_path_offset(dt, "/soc/usb-pd"), "status", "okay") == 0);
    int supply = fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "external-power");
    assert(supply >= 0);
    assert(!fdt_setprop_string(dt, supply, "compatible", "apple,t8140-pmgr-pwrstate"));
    assert(!fdt_setprop_u32(dt, supply, "phandle", 200));
    assert(!fdt_setprop_u32(dt, supply, "#power-domain-cells", 0));
    assert(!fdt_setprop_string(dt, supply, "status", "disabled"));
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/dcpext"), "power-domains", 200));
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/chosen"), "framebuffer") >= 0);
    cur_boot_args.phys_base = 0x1000342c000;
    cur_boot_args.mem_size = 0x1e39e8000;
    physical_base = 0x10000000000;
    physical_size = 0x200000000;
    carveouts[3].pa = 0x101f0cd4000;
    carveouts[5].pa = 0x101e8ec8000;
    assert(cur_boot_args.phys_base + cur_boot_args.mem_size < carveouts[0].pa);
    cur_boot_args.video.base = carveouts[0].pa;
    /* The boot framebuffer is a sub-range of the /vram carveout. */
    cur_boot_args.video.stride = 0x1788;
    cur_boot_args.video.height = 2416;
    assert(cur_boot_args.video.stride * cur_boot_args.video.height == 0xde1380);
    reservation_calls = 0;
    quiesce_fault = table_fault = init_fault = 0;
    quiesce_calls = 0;
    ext_power = true;
    hole_dcp_vram = hole_dcp_firmware = hole_ext = false;
    bad_carveout = bad_segment = false;
    for (size_t i = 0; i < ARRAY_SIZE(asc_state); i++) {
        asc_state[i] = (struct asc_mock){
            .control = 0x10, .status = 0x6d, .a2i = 0x2ff01, .i2a = 0x28801,
        };
    }
    /* The external DCP is inherited started but stopped in its idle loop. */
    asc_state[1].status = 0x6e;
}
static void check_claim(bool present)
{
    const char *observations[] = {
        "aurora,external-display-metadata", "aurora,external-display-mappers",
        "aurora,external-display-registers",
    };
    for (size_t i = 0; i < ARRAY_SIZE(observations); i++)
        assert(!fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), observations[i], NULL));
    int len = 0;
    const u32 *cell = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"),
                                  "apple,dcp-rtkit-live", &len);
    if (present) assert(cell && len == sizeof(u32) && fdt32_to_cpu(*cell) == 1);
    else assert(cell == NULL);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,dcp-rtkit-quiesced", NULL));
    int target = fdt_node_offset_by_phandle(dt, 100);
    cell = fdt_getprop(dt, target, "apple,firmware-root-handoff", &len);
    if (present) {
        assert(cell && len == sizeof(u32) && fdt32_to_cpu(*cell) == 1);
        int table = fdt_path_offset(dt, "/reserved-memory/dart-tables@10000000000");
        assert(table >= 0 && !fdt_getprop(dt, table, "no-map", NULL));
        const fdt64_t *reg = fdt_getprop(dt, table, "reg", &len);
        assert(reg && len == 16 && fdt64_to_cpu(reg[0]) == physical_base &&
               fdt64_to_cpu(reg[1]) == 2 * SZ_16K);
        target = fdt_node_offset_by_phandle(dt, 104);
        cell = fdt_getprop(dt, target, "apple,firmware-root-handoff", &len);
        assert(cell && len == sizeof(u32) && fdt32_to_cpu(*cell) == 1);
        table = fdt_path_offset(dt, "/reserved-memory/dart-tables@10000018000");
        assert(table >= 0 && !fdt_getprop(dt, table, "no-map", NULL));
    } else {
        assert(!cell);
        assert(!fdt_getprop(dt, fdt_node_offset_by_phandle(dt, 104),
                            "apple,firmware-root-handoff", NULL));
    }
}
static void check_status(const char *name, const char *want)
{
    char path[64];
    snprintf(path, sizeof(path), name[0] == '/' ? "%s" : "/soc/%s", name);
    const char *status = fdt_getprop(dt, fdt_path_offset(dt, path), "status", NULL);
    if (!status || strcmp(status, want))
        fprintf(stderr, "%s: expected %s, got %s\n", name, want, status ? status : "absent");
    assert(status && !strcmp(status, want));
}
static void check_fallback(const unsigned char *before)
{
    (void)before;
    check_status("dcp", "disabled");
    check_status("disp0", "disabled");
    check_status("dcpext", "disabled");
    check_status("dispext0", "disabled");
    check_status("dart-dcp", "disabled");
    check_status("dart-dcpext", "disabled");
    check_status("mailbox-dcp", "disabled");
    check_status("mailbox-dcpext", "disabled");
    check_claim(false);
    assert(fdt_path_offset(dt, "/chosen/framebuffer") >= 0);
    assert(fdt_path_offset(dt, "/reserved-memory/framebuffer@101eb4b8000") < 0);
    assert(fdt_check_header(dt) == 0);
}
int main(void)
{
    (void)dt_t8140_validate_dcp_asc;
    unsigned char before[sizeof(tree)];
    setup();
    provider_present = true;
    quiesce_fault = 1;
    assert(dt_set_display_t8140() == 0);
    for (int d = 0; d < 2; d++) {
        int node = fdt_path_offset(dt, d ? "dcpext" : "dcp");
        for (int i = 0; i < 5; i++) {
            char name[64]; int len;
            snprintf(name, sizeof(name), "apple,%s", provider_names[i]);
            const void *value = fdt_getprop(dt, node, name, &len);
            assert(value && len == (int)provider_sizes[i]);
            assert(!memcmp(value, provider_bytes[i], len));
        }
        assert(!fdt_getprop(dt, node, "apple,power-lut-vbatt-cur-nominal", NULL));
    }
    assert(quiesce_calls == 0);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,dcp-rtkit-quiesced", NULL));
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,dcp-handoff-reason", NULL), "live-session"));
    check_status("dcp", "okay");
    check_status("disp0", "okay");
    check_status("dcpext", "okay");
    const fdt32_t *preflight = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"),
                                         "aurora,external-display-preflight", NULL);
    assert(preflight && fdt32_to_cpu(*preflight) == 0x100);
    check_status("external-power", "okay");
    check_status("dispext0", "disabled");
    assert(reservation_calls == 11);
    assert(fdt_path_offset(dt, "/reserved-memory/framebuffer@101eb4b8000") >= 0);
    assert(fdt_path_offset(dt, "/reserved-memory/dcp_data_tail@101eb488000") >= 0);
    assert(fdt_path_offset(dt, "/reserved-memory/dcpext0_data_tail@101ea79c000") >= 0);
    assert(fdt_path_offset(dt, "/chosen/framebuffer") >= 0);
    check_claim(true);
    check_status("/soc/dcp/typec-routes/port0", "okay");
    check_status("/soc/dcpext/typec-routes/port0", "okay");
    check_status("/soc/usb-pd", "okay");
    assert(!dt_t8140_disable_dcpext());
    check_status("external-power", "disabled");
    check_status("dcp", "okay");

    /* A missing table remains absent; no static compatibility data is invented. */
    setup();
    assert(dt_set_display_t8140() == 0);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,power-lut-data-x", NULL));
    check_status("dcp", "okay");

    for (int missing = 0; missing < 5; missing++) {
        setup(); provider_present = true; provider_missing = missing;
        assert(dt_set_display_t8140() == 0);
        for (int d = 0; d < 2; d++) {
            int node = fdt_path_offset(dt, d ? "dcpext" : "dcp");
            for (int i = 0; i < 5; i++) {
                char name[64]; int len;
                snprintf(name, sizeof(name), "apple,%s", provider_names[i]);
                const void *value = fdt_getprop(dt, node, name, &len);
                if (i == missing) assert(!value);
                else assert(value && len == (int)provider_sizes[i] &&
                            !memcmp(value, provider_bytes[i], len));
            }
        }
    }
    for (int bad = 0; bad < 5; bad++) {
        for (int size = 0; size < 3; size++) {
            setup(); provider_present = true; provider_bad = bad;
            provider_bad_size = size == 0 ? 0 : provider_sizes[bad] + (size == 1 ? -1 : 1);
            memcpy(before, dt, sizeof(tree));
            assert(dt_set_display_t8140() == 0);
            check_fallback(before);
            assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,power-lut-data-x", NULL));
            assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcpext"), "apple,power-lut-data-x", NULL));
        }
    }
    setup(); provider_present = true; provider_no_space = "/soc/dcp";
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,power-lut-data-x", NULL));

    /* An external property write failure keeps the internal panel usable. */
    setup(); provider_present = true; provider_no_space = "/soc/dcpext";
    assert(dt_set_display_t8140() == 0);
    check_status("dcp", "okay"); check_status("disp0", "okay");
    check_status("dcpext", "disabled"); check_status("external-power", "disabled");
    assert(fdt_getprop(dt, fdt_path_offset(dt, "dcp"), "apple,power-lut-data-x", NULL));
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "dcpext"), "apple,power-lut-data-x", NULL));

    setup();
    assert(!fdt_setprop_u32(dt, fdt_path_offset(dt, "/soc/external-power"),
                           "power-domains", 200));
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() < 0);
    assert(!memcmp(before, dt, sizeof(tree)));

    /* External admission is exactly started, stopped and idle, at both reads.
     * The internal DCP keeps its running-only admission. */
    static const u32 refused_external[] = {
        0x6d, /* running */
        0x6c, /* running, idle bit clear */
        0x4e, /* stopped, not idle */
        0x02, /* stopped only */
        0x00, /* neither */
    };
    for (size_t i = 0; i < ARRAY_SIZE(refused_external) + 1; i++) {
        setup();
        if (i < ARRAY_SIZE(refused_external)) {
            asc_state[1].status = refused_external[i];
        } else {
            asc_state[1].status_on_recheck = true;
            asc_state[1].recheck_status = 0x6d;
        }
        assert(dt_set_display_t8140() == 0);
        check_status("dcp", "okay");
        check_status("disp0", "okay");
        check_status("dcpext", "disabled");
        check_status("/soc/dcpext/typec-routes/port0", "disabled");
        check_claim(true);
    }
    setup();
    asc_state[0].status = 0x6e;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    /* A refused external controller must withdraw only its mode routes. */
    setup();
    asc_state[1].control = 0;
    assert(dt_set_display_t8140() == 0);
    check_status("dcp", "okay");
    check_status("/soc/dcp/typec-routes/port0", "okay");
    check_status("/soc/dcpext/typec-routes/port0", "disabled");
    check_status("/soc/usb-pd", "okay");
    check_claim(true);
    check_status("/soc/dart-dispext", "disabled");

#ifdef HAVE_DISPLAY_RECHECK
    for (int fault = 0; fault < 7; fault++) {
        setup();
        assert(dt_set_display_t8140() == 0);
        if (fault == 0) ext_power = false;
        if (fault == 1) asc_state[1].status = (asc_state[1].status | BIT(1)) & ~BIT(5);
        if (fault == 2) asc_state[1].i2a |= BIT(18);
        if (fault == 3) asc_state[1].stop_on_recheck = true;
        if (fault == 4) asc_state[1].a2i &= ~BIT(0);
        /* Running is not the idle state the kernel adopts. */
        if (fault == 5) asc_state[1].status = 0x6d;
        if (fault == 6) { asc_state[1].status_on_recheck = true; asc_state[1].recheck_status = 0x6d; }
        assert(dt_t8140_recheck_display() == 0);
        check_status("dcp", "okay");
        check_status("disp0", "okay");
        check_status("/soc/dcp/typec-routes/port0", "okay");
        check_status("dcpext", "disabled");
        check_status("dart-dcpext", "disabled");
        check_status("mailbox-dcpext", "disabled");
        check_status("/soc/dart-dispext", "disabled");
        check_status("/soc/dcpext/typec-routes/port0", "disabled");
        check_status("/soc/usb-pd", "okay");
        check_claim(true);
        assert(!fdt_getprop(dt, fdt_node_offset_by_phandle(dt, 101),
                            "apple,firmware-root-handoff", NULL));
        assert(!fdt_getprop(dt, fdt_node_offset_by_phandle(dt, 105),
                            "apple,firmware-root-handoff", NULL));
    }
    setup();
    assert(dt_set_display_t8140() == 0);
    assert(dt_t8140_recheck_display() == 0);
    check_status("dcpext", "okay");
    check_status("/soc/dcpext/typec-routes/port0", "okay");
    check_claim(true);

    setup();
    assert(dt_set_display_t8140() == 0);
    asc_state[0].status |= BIT(1);
    assert(dt_t8140_recheck_display() == 0);
    check_status("dcp", "disabled");
    check_status("dcpext", "disabled");
    check_status("/soc/dcp/typec-routes/port0", "disabled");
    check_status("/soc/dcpext/typec-routes/port0", "disabled");
    check_status("/soc/usb-pd", "okay");
    check_claim(false);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,dcp-handoff-reason", NULL), "withdrawn"));

    setup();
    assert(fdt_delprop(dt, fdt_path_offset(dt, "/chosen"),
                       "apple,dcp-rtkit-live") == 0);
    unsigned char unchanged[sizeof(tree)];
    memcpy(unchanged, dt, sizeof(tree));
    assert(dt_t8140_recheck_display() == 0);
    assert(!memcmp(unchanged, dt, sizeof(tree)));
#endif

    setup();
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc/dcpext/typec-routes"),
                          "0123456789012345678901234567890") >= 0);
    assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc/dcpext/typec-routes"),
                          "0123456789012345678901234567890-too-long") >= 0);
    asc_state[1].control = 0;
    assert(dt_set_display_t8140() == -1);
    check_claim(false);

    setup();
    for (int i = 1; i <= 4; i++) {
        char name[16];
        snprintf(name, sizeof(name), "port%d", i);
        assert(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc/dcpext/typec-routes"), name) >= 0);
    }
    asc_state[1].control = 0;
    assert(dt_set_display_t8140() == -1);
    check_claim(false);

    for (int fault = 1; fault <= 4; fault++) {
        setup();
        if (fault == 1)
            for (u32 phandle = 100; phandle <= 101; phandle++)
                assert(fdt_setprop_u32(dt, fdt_node_offset_by_phandle(dt, phandle),
                                      "apple,firmware-root-handoff", 1) == 0);
        if (fault <= 2) asc_state[fault - 1].control = 0;
        else table_fault = fault - 2;
        assert(dt_set_display_t8140() == 0);
        if (fault == 1 || fault == 3) check_fallback(before);
        else {
            check_claim(true);
            check_status("dcpext", "disabled");
            assert(!fdt_getprop(dt, fdt_node_offset_by_phandle(dt, 101),
                                "apple,firmware-root-handoff", NULL));
        }
    }

    setup();
    table_fault = 3;
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);
    /* Without its PIODMA child the internal display master cannot be shown
     * to belong to the refused DCP, so refusal fails before any edit. */
    setup();
    assert(fdt_del_node(dt, fdt_path_offset(dt, "/soc/dcp/piodma")) == 0);
    assert(dt_set_display_t8140() < 0);
    setup();
    fdt32_t malformed_child[] = {cpu_to_fdt32(104)};
    assert(fdt_setprop(dt, fdt_path_offset(dt, "/soc/dcp/piodma"), "iommus",
                       malformed_child, sizeof(malformed_child)) == 0);
    /* A malformed reference fails refusal before any edit. */
    assert(dt_set_display_t8140() == -1);

    setup();
    fdt32_t too_large_sid[] = {cpu_to_fdt32(104), cpu_to_fdt32(256)};
    assert(fdt_setprop(dt, fdt_path_offset(dt, "/soc/dcp/piodma"), "iommus",
                       too_large_sid, sizeof(too_large_sid)) == 0);
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    for (int fault = 0; fault < 2; fault++) {
        setup();
        fdt32_t pairs[] = {cpu_to_fdt32(100), cpu_to_fdt32(23),
                           cpu_to_fdt32(104), cpu_to_fdt32(fault ? 16 : 256)};
        assert(fdt_setprop(dt, fdt_path_offset(dt, "/soc/dcp"), "iommus",
                           pairs, sizeof(pairs)) == 0);
        init_fault = fault;
        assert(dt_set_display_t8140() == 0);
        check_fallback(before);
    }

    setup();
    for (int i = 0; i < 31; i++) {
        char name[64];
        snprintf(name, sizeof(name), "normal-table-%d", i);
        assert(dt_get_or_add_reserved_mem(name, "apple,firmware-dart-tables", false,
                                          physical_base + (8 + i) * SZ_16K, SZ_16K) >= 0);
    }
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup();
    cur_boot_args.phys_base = physical_base + SZ_16K;
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup();
    physical_size = carveouts[0].pa - physical_base + carveouts[0].size - SZ_16K;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    assert(reservation_calls == 0);
    check_fallback(before);

    setup();
    assert(fdt_delprop(dt, fdt_path_offset(dt, "/chosen"),
                       "apple,dcp-rtkit-live") == 0);
    asc_state[0].control = 0;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);
    check_claim(false);

    for (int fault = 0; fault < 9; fault++) {
        setup();
        assert(fdt_delprop(dt, fdt_path_offset(dt, "/chosen"),
                           "apple,dcp-rtkit-live") == 0);
        switch (fault) {
        case 0: asc_state[0].status |= BIT(1); break;
        case 1: asc_state[0].a2i |= BIT(16); break;
        case 2: asc_state[0].i2a |= BIT(18); break;
        case 3: asc_state[0].a2i &= ~BIT(17); break;
        case 4: asc_state[0].i2a |= BIT(20); break;
        case 5: asc_state[0].a2i ^= BIT(8); break;
        case 6: asc_state[0].i2a &= ~BIT(0); break;
        case 7: asc_state[0].stop_on_recheck = true; break;
        case 8: asc_state[0].i2a |= BIT(16); break;
        }
        memcpy(before, dt, sizeof(tree));
        assert(dt_set_display_t8140() == 0);
        check_fallback(before);
        check_claim(false);
    }

    setup();
    asc_state[1].i2a |= BIT(18);
    assert(dt_set_display_t8140() == 0);
    check_status("dcpext", "disabled");
    check_status("dart-dcpext", "disabled");
    check_status("mailbox-dcpext", "disabled");
    check_claim(true);
    assert(fdt_path_offset(dt, "/reserved-memory/dcpext0_data_tail@101ea79c000") < 0);

    setup();
    ext_power = false;
    assert(dt_set_display_t8140() == 0);
    check_status("dcp", "okay");
    check_status("dcpext", "disabled");
    check_claim(true);

    setup();
    carveouts[3].pa = physical_base + physical_size;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup();
    carveouts[5].pa = physical_base + physical_size;
    assert(dt_set_display_t8140() == 0);
    check_status("dcp", "okay");
    check_status("disp0", "okay");
    check_status("dcpext", "disabled");
    assert(fdt_path_offset(dt, "/reserved-memory/region74@101e8ec8000") < 0);
    check_claim(true);

    setup(); hole_dcp_vram = true;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup(); hole_dcp_firmware = true;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup(); hole_ext = true;
    assert(dt_set_display_t8140() == 0);
    check_status("dcp", "okay");
    check_status("disp0", "okay");
    check_status("dcpext", "disabled");
    assert(fdt_path_offset(dt, "/reserved-memory/region74@101e8ec8000") < 0);

    setup(); bad_carveout = true;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup(); bad_segment = true;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup();
    assert(fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcp") == 0);
    memcpy(before, dt, sizeof(tree));
    /* Without its dcp alias the tree does not describe the display: keep simplefb. */
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);
    assert(!strcmp(fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apple,dcp-handoff-reason", NULL),
                   "FDT dcp alias absent"));

    setup();
    assert(fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcpext") == 0);
    assert(dt_set_display_t8140() == 0);
    check_status("dcpext", "okay");

    setup();
    cur_boot_args.video.base = carveouts[0].pa + carveouts[0].size - 4096;
    memcpy(before, dt, sizeof(tree));
    assert(dt_set_display_t8140() == 0);
    check_fallback(before);

    setup();
    physical_size = SZ_16K;
    assert(fdt_delprop(dt, fdt_path_offset(dt, "/chosen"),
                       "apple,dcp-rtkit-live") == 0);
    assert(fdt_pack(dt) == 0);
    assert(dt_set_display_t8140() == -1);
    return 0;
}
'''
    (tmp_path / "t8140-display-handoff.c").write_text(harness)
    binary = tmp_path / "t8140-display-handoff"
    libfdt = repo / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-I", str(libfdt),
         "-x", "c", "-", *map(str, sources), "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    assert "DCP enabled with validated framebuffer, firmware and live inherited RTKit session" in result.stdout
    assert "DCP nub segment" not in result.stdout
    assert "framebuffer: sub-range of /vram validated" not in result.stdout
    assert "refused at preflight: /vram lies outside physical DRAM" in result.stdout
    assert "region-id-57 PA 0x10200000000 size 0xc00000 outside physical DRAM" in result.stdout
    assert "refused at /vram DCP/disp0 DART reservation" in result.stdout
    assert "refused at preflight: boot framebuffer lies outside /vram" in result.stdout
