/* SPDX-License-Identifier: MIT */
/*
 * Host test of the external display processor handoff that the ADT describes
 * (dt_set_dcpext_handoff() in src/kboot_display_handoff.c), on T8132 27.2-shaped trees: the
 * MacBook Air (J713, beside the internal panel's handoff) and the Mac mini (J773g, no panel),
 * through the same code, with no board string in it.
 *
 * The ADT has the structure of the T8132 ADTs: /arm-io/dcpextN (device_type dcp) with an
 * iommu-parent phandle to /arm-io/dart-dcpextN/mapper-dcpextN (dart-mapper, reg = the stream),
 * the DART's sid-count, vm-base and vm-size-<sid>, the pipes /arm-io/dispextN
 * (ext-display-subsystem) whose iommu-parent mappers live under /arm-io/dart-dispextN (streams 0
 * and 16), and the pipes' clock-ids indexing /arm-io clock-frequencies. Register addresses,
 * stream numbers, clock ids and the image uuid are the T8132 ADT's; physical addresses of firmware
 * segments and DART tables are synthetic. No Apple firmware data. The fake ADT, PMGR and MMIO are
 * t6041_display_host.c's; only the PMGR power states, the DARTs (PROTECT, stream count, enable
 * bits, TCR/TTBR) and their tables are mapped, so any other register access fails, and every
 * write is counted (none is expected).
 *
 *   t8132-dcpext-host list
 *   t8132-dcpext-host <scenario>
 *
 * tests/python/test_t8132_dcpext_handoff.py runs every scenario.
 */
#define main t6041_main
#include "t6041_display_host.c"
#undef main

#define UUID27  "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF" /* T8132 27.2 (26B123) iop nub uuid */
#define UUID262 "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E" /* T8132 26.6.2 */
#define UUID_X  "0BC97BDD-E2FE-3A1B-96B4-8EA8FC5B8BE8"
#define READY   "apple,t8132-dcpext-firmware-ready"

static const char J713[] = "apple,j713\0apple,t8132\0apple,arm-platform";
static const char J773G[] = "apple,j773g\0apple,t8132\0apple,arm-platform";
static const char J9XX[] = "apple,j9xx\0apple,t8140\0apple,arm-platform";
static const char T8132_DART[] = "apple,t8132-dart\0apple,t8110-dart";
static const char T8132_EXT[] = "apple,t8132-dcpext";
static const char T8140_EXT[] = "apple,t8140-dcpext\0apple,t8132-dcpext";
static const char T8132_DISP[] = "apple,t8132-display-subsystem\0apple,t8132-display-diagnostics";

#define M4_DCP       0x30ae00000ULL
#define M4_DART_DCP  0x30a340000ULL
#define M4_DART_DISP 0x30a300000ULL
static const u64 ext_coproc[2] = {0x286e00000ULL, 0x48ae00000ULL};
static const u64 ext_dart[2] = {0x286340000ULL, 0x48a340000ULL};
static const u64 pipe_dart[2] = {0x286300000ULL, 0x48a300000ULL};
static const u32 pipe_clk[2][2] = {{369, 432}, {372, 433}};
static const u32 clk_hz[2][2] = {{533333333, 400000000}, {600000000, 0}};

/* ADT phandles, as in the T8132 ADTs' ranges */
enum { APH_MAPPER_DCP = 300, APH_DART_EXT = 319, APH_MAPPER_EXT = 320, APH_MAPPER_DISP = 322 };
static u32 aph_mapper_ext(int i)
{
    return APH_MAPPER_EXT + 10 * i;
}
static u32 aph_mapper_disp(int i, int piodma)
{
    return APH_MAPPER_DISP + 10 * i + piodma;
}

/* DT phandles */
enum {
    PH_EXT_DART = 100,
    PH_PIPE_DART = 104,
    PH_PLACE = 110,
    PH_M4_DART_DCP = 120,
    PH_M4_DART_DISP,
    PH_M4_TEXT,
    PH_EXTRA_DART
};

/* The internal nub: __TEXT (shared by all three processors), __DATA, __OS_LOG, a heap */
static const struct seg synth_segs[4] = {
    {TB + 0x280000000ULL, 0, TB + 0x40000000, 0x10000, 1},
    {TB + 0x280010000ULL, 0x10000, TB + 0x80000000, 0x8000, 0},
    {TB + 0x24a4000ULL, 0x18000, TB + 0x24a4000ULL, 0x20000, 0xa},
    {TB + 0x280020000ULL, ~0ULL, TB + 0x968000, 0x40000, 2},
};

/*
 * The J713's 27.2 layout, as its ADT gives it (the values, not the blob): __TEXT below the
 * boot-args RAM and not a multiple of 16K, __OS_LOG not a multiple of 16K, __DATA and the heaps
 * above the RAM; the internal nub has seven segments, each external one four, whose DVAs are the
 * same for both (each processor has its own DART).
 */
static const struct seg j713_272_segs[7] = {
    {TB + 0x2a8000ULL, 0, TB + 0x5bcc000ULL, 0x843000, 1},
    {TB + 0x3e1874000ULL, 0x843000, TB + 0x6410000ULL, 0x32c000, 0},
    {TB + 0xe4d4000ULL, 0xb6f000, TB + 0xe4d4000ULL, 0x26000, 0xa},
    {TB + 0x3e5c1c000ULL, ~0ULL, TB + 0xb70000ULL, 0x1000000, 2},
    {TB + 0x3e5c18000ULL, ~0ULL, TB + 0x1b98000ULL, 0x4000, 2},
    {TB + 0x3e5b58000ULL, ~0ULL, TB + 0x1b9c000ULL, 0xc0000, 2},
    {TB + 0x3e1be8000ULL, ~0ULL, TB + 0x1c5c000ULL, 0x3f70000, 2},
};
static const u64 j713_272_ext_data[2] = {TB + 0x3e1548000ULL, TB + 0x3e021c000ULL};
static const u64 j713_272_ext_oslog[2] = {TB + 0xe4fc000ULL, TB + 0xe524000ULL};
static const u64 j713_272_ext_heap[2] = {TB + 0x3e0548000ULL, TB + 0x3df21c000ULL};

static bool layout272;
static const struct seg *dcp_segs = synth_segs;
static size_t dcp_nsegs = ARRAY_SIZE(synth_segs);

static void ext_segs(int i, struct seg out[4])
{
    out[0] = dcp_segs[0];
    if (layout272) {
        out[1] = (struct seg){j713_272_ext_data[i], 0x843000, TB + 0x6410000ULL, 0x32c000, 0};
        out[2] = (struct seg){j713_272_ext_oslog[i], 0xb6f000, j713_272_ext_oslog[i], 0x26000, 0xa};
        out[3] = (struct seg){j713_272_ext_heap[i], ~0ULL, TB + 0x673c000ULL, 0x1000000, 2};
        return;
    }
    out[1] = (struct seg){TB + 0x281200000ULL + i * 0x200000, 0, TB + 0x51000000 + i * 0x100000,
                          0x8000, 0};
    out[2] = (struct seg){TB + 0x28030000ULL + i * 0x40000, 0, TB + 0x28030000ULL + i * 0x40000,
                          0x4000, 0xa};
    out[3] = (struct seg){TB + 0x282200000ULL + i * 0x200000, ~0ULL, TB + 0x52000000 + i * 0x100000,
                          0x10000, 2};
}

static void adt_path_of(char *buf, size_t len, const char *fmt, int i)
{
    snprintf(buf, len, fmt, i, i);
}

/* ---- the ADT ---- */

/*
 * PMGR: each pipe's CPU domain (the ADT clock-gate of its processor) under its FE and SYS domains,
 * under a virtual fabric device, as in the T8132 ADT's table. The ids are the J713's; the power
 * state registers are synthetic. iBoot leaves SYS and FE on and the CPU requested but gated by
 * the hardware while idle (target active, auto enable, actual off).
 */
#define T8132_PMGR 0x3e0000000ULL
#define PS_ON      0xffu
#define PS_CPU_REQ (0x1000000fu)
enum { PM_FABRIC = 32, PM_DISP0_SYS = 10, PM_DISP0_FE, PM_DISP0_CPU };
static u16 pm_sys(int i)
{
    return i ? 137 : 40;
}
static u64 ps_reg(u16 id)
{
    return T8132_PMGR + 8 * id;
}

/*
 * DART page tables, in DRAM above the RAM the OS gets (in both layouts): one root and one L2 page
 * per stream, at TABLES + 0x8000 * slot. A scenario moves them into usable RAM by TABLES_SHIFT.
 */
#define TABLES      (TB + 0x3f0000000ULL)
#define TABLES_SIZE 0x80000
static int table_slots;

/* A two-level translation: TTBR -> root, root[1] -> L2, L2[0] -> one page; returns the TTBR */
static u32 dart_tables(u64 at)
{
    u64 root = at, l2 = at + 0x4000;
    poke64(root + 8, (l2 >> 4) | 1);
    poke64(l2, ((TB + 0x280000000ULL) >> 4) | 1);
    return (u32)((root >> 14) << 2) | 1;
}

/*
 * DART registers (T8110 layout): PROTECT, the stream count, and one enabled stream's TCR and TTBR.
 * A translating stream (TCR bit 0) with a valid TTBR gets its own tables; TTBR 1 asks for them.
 */
static void dart_regs(u64 base, u32 sid, u32 protect, u32 tcr, u32 ttbr)
{
    if (!mem(base, 4)) {
        region_add(base, 0x4000);
        poke32(base + 0xc, 32);
    }
    poke32(base + 0x200, protect);
    poke32(base + 0xc00 + 4 * (sid / 32), peek32(base + 0xc00 + 4 * (sid / 32)) | 1u << (sid % 32));
    if (ttbr == 1)
        ttbr = dart_tables(TABLES + 0x8000 * table_slots++);
    poke32(base + 0x1000 + 4 * sid, tcr);
    poke32(base + 0x1400 + 4 * sid, ttbr);
}

static struct region *region_of(u64 base)
{
    for (int i = 0; i < nregions; i++)
        if (regions[i].base == base)
            return &regions[i];
    abort();
}

static u32 dart_ttbr(u64 base, u32 sid)
{
    return peek32(base + 0x1400 + 4 * sid);
}

/* The root page a stream's TTBR points at */
static u64 ttbr_root(u32 ttbr)
{
    return (u64)((ttbr >> 2) & 0xfffffff) << 14;
}

static void world_t8132(bool no_panel, const char *uuid)
{
    nfdevs = 0;
    nregions = 0;
    table_slots = 0;
    region_add(T8132_PMGR, 0x1000);
    region_add(TABLES, TABLES_SIZE);
    fdev_add(PM_FABRIC, "AFISOCNI0", true, 0, 0);
    fdev_add(PM_DISP0_SYS, "DISP0_SYS", false, ps_reg(PM_DISP0_SYS), PM_FABRIC);
    fdev_add(PM_DISP0_FE, "DISP0_FE", false, ps_reg(PM_DISP0_FE), PM_DISP0_SYS);
    fdev_add(PM_DISP0_CPU, "DISP0_CPU", false, ps_reg(PM_DISP0_CPU), PM_DISP0_FE);
    poke32(ps_reg(PM_DISP0_SYS), PS_ON);
    poke32(ps_reg(PM_DISP0_FE), PS_ON);
    poke32(ps_reg(PM_DISP0_CPU), PS_CPU_REQ);
    static const char *const pm_names[2][3] = {{"DISPEXT0_SYS", "DISPEXT0_FE", "DISPEXT0_CPU"},
                                               {"DISPEXT1_SYS", "DISPEXT1_FE", "DISPEXT1_CPU"}};
    for (int i = 0; i < 2; i++) {
        u16 sys = pm_sys(i);
        fdev_add(sys, pm_names[i][0], false, ps_reg(sys), PM_FABRIC);
        fdev_add(sys + 1, pm_names[i][1], false, ps_reg(sys + 1), sys);
        fdev_add(sys + 2, pm_names[i][2], false, ps_reg(sys + 2), sys + 1);
        poke32(ps_reg(sys), PS_ON);
        poke32(ps_reg(sys + 1), PS_ON);
        poke32(ps_reg(sys + 2), PS_CPU_REQ);
        /* iBoot locked the firmware DART; stream 23 translates through two-level tables */
        dart_regs(ext_dart[i], 23, 0x1ff, 0x1, 1);
    }
    /* The display subsystem's DART: scanout (0) and piodma (16), locked */
    dart_regs(M4_DART_DISP, 0, 0x1f9, 0x1, 1);
    dart_regs(M4_DART_DISP, 16, 0x1f9, 0x1, 1);
    /* The pipes' DARTs: locked, scanout and piodma set up */
    for (int i = 0; i < 2; i++) {
        dart_regs(pipe_dart[i], 0, 0x1ff, 0x1, 1);
        dart_regs(pipe_dart[i], 16, 0x1ff, 0x1, 1);
    }

    chip_id = 0x8132;
    os_firmware = (struct fw_version_info){V27_2, "27.2", {27, 2, 0}, 3, "mBoot-20457.41.3"};
    ram_base = layout272 ? TB + 0xc00000 : TB + 0x800000;
    ram_size = layout272 ? 0x3dd1a4000ULL - 0xc00000 : 0x200000000ULL - 0x800000;
    fb_base = TB + 0x290000000ULL;
    cur_boot_args.video.stride = 2560 * 4;
    cur_boot_args.video.height = 1664;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x400000000ULL);
    mcc_carveout_count = 0;

    static u32 freq[200];
    for (int i = 0; i < 2; i++)
        for (int k = 0; k < 2; k++)
            freq[pipe_clk[i][k] - 256] = clk_hz[i][k];
    freq[364 - 256] = 300000000;
    prop_set("/arm-io", "clock-frequencies", freq, sizeof(freq));

    prop_str("/arm-io/dcp", "device_type", "dcp");
    reg_add("/arm-io/dcp", M4_DCP, 0x6c000);
    prop_u32("/arm-io/dcp", "iommu-parent", APH_MAPPER_DCP);
    const char *nub = "/arm-io/dcp/iop-dcp-nub";
    prop_set(nub, "segment-ranges", dcp_segs, dcp_nsegs * sizeof(*dcp_segs));
    prop_str(nub, "segment-names",
             layout272 ? "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown"
                       : "__TEXT;__DATA;__OS_LOG;unknown");
    prop_str(nub, "uuid", uuid);
    prop_u32(nub, "pre-loaded", 1);
    prop_u32(nub, "running", 1);
    prop_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-dcp", M4_DART_DCP, 0x20000);
    prop_str("/arm-io/dart-dcp/mapper-dcp", "device_type", "dart-mapper");
    prop_u32("/arm-io/dart-dcp/mapper-dcp", "reg", 23);
    prop_u32("/arm-io/dart-dcp/mapper-dcp", "AAPL,phandle", APH_MAPPER_DCP);
    prop_str("/arm-io/disp0", "device_type", "display-subsystem");
    u32 dcp_gate = PM_DISP0_CPU;
    prop_set("/arm-io/dcp", "clock-gates", &dcp_gate, sizeof(dcp_gate));
    prop_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-disp0", M4_DART_DISP, 0xc000);
    if (no_panel)
        prop_u32("/arm-io/disp0", "external", 1);

    for (int i = 0; i < 2; i++) {
        char p[96];
        struct seg es[4];
        ext_segs(i, es);
        adt_path_of(p, sizeof(p), "/arm-io/dcpext%d", i);
        prop_str(p, "device_type", "dcp");
        prop_str(p, "compatible", "iop,ascwrap-v6");
        reg_add(p, ext_coproc[i], 0x88000);
        u32 gate = pm_sys(i) + 2; /* the processor's clock-gate: DISPEXTn_CPU */
        prop_set(p, "clock-gates", &gate, sizeof(gate));
        prop_u32(p, "iommu-parent", aph_mapper_ext(i));
        adt_path_of(p, sizeof(p), "/arm-io/dcpext%d/iop-dcpext%d-nub", i);
        prop_str(p, "compatible", "iop-nub,rtbuddy-v2");
        prop_set(p, "segment-ranges", es, sizeof(es));
        prop_str(p, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown");
        prop_str(p, "uuid", uuid);
        prop_u32(p, "pre-loaded", 1);
        prop_u32(p, "running", 1);

        adt_path_of(p, sizeof(p), "/arm-io/dart-dcpext%d", i);
        prop_str(p, "compatible", "dart,t8110");
        prop_str(p, "device_type", "dart");
        reg_add(p, ext_dart[i], 0x20000);
        prop_u32(p, "AAPL,phandle", APH_DART_EXT + 10 * i);
        prop_u32(p, "sid-count", 32);
        prop_u64(p, "vm-base", TB);
        prop_u64(p, "vm-size-23", 0x1000000000ULL);
        prop_u64(p, "vm-size", 0x30000000000ULL); /* the whole space: not the stream's window */
        adt_path_of(p, sizeof(p), "/arm-io/dart-dcpext%d/mapper-dcpext%d", i);
        prop_str(p, "device_type", "dart-mapper");
        prop_str(p, "compatible", "iommu-mapper");
        prop_u32(p, "reg", 23);
        prop_u32(p, "AAPL,phandle", aph_mapper_ext(i));

        adt_path_of(p, sizeof(p), "/arm-io/dispext%d", i);
        prop_str(p, "device_type", "ext-display-subsystem");
        prop_u32(p, "external", 1);
        u32 parents[2] = {aph_mapper_disp(i, 0), aph_mapper_disp(i, 1)};
        prop_set(p, "iommu-parent", parents, sizeof(parents));
        prop_set(p, "clock-ids", pipe_clk[i], sizeof(pipe_clk[i]));
        adt_path_of(p, sizeof(p), "/arm-io/dart-dispext%d", i);
        prop_str(p, "compatible", "dart,t8110");
        reg_add(p, pipe_dart[i], 0xc000);
        prop_u32(p, "sid-count", 32);
        prop_u64(p, "vm-base", TB);
        prop_u64(p, "vm-size", 0x1000000000ULL);
        adt_path_of(p, sizeof(p), "/arm-io/dart-dispext%d/mapper-dispext%d", i);
        prop_str(p, "device_type", "dart-mapper");
        prop_u32(p, "reg", 0);
        prop_u32(p, "AAPL,phandle", aph_mapper_disp(i, 0));
        adt_path_of(p, sizeof(p), "/arm-io/dart-dispext%d/mapper-dispext%d-piodma", i);
        prop_str(p, "device_type", "dart-mapper");
        prop_u32(p, "reg", 16);
        prop_u32(p, "AAPL,phandle", aph_mapper_disp(i, 1));
    }
}

/* ---- the device tree (t8132-dcp.dtsi, t8132-dcpext.dtsi and the boards' placeholders) ---- */

static int t8132_dart(void *dt, int soc, u64 base, u64 size, u32 ph, const char *status)
{
    char name[48];
    snprintf(name, sizeof(name), "iommu@%llx", (unsigned long long)base);
    int n = OK(fdt_add_subnode(dt, soc, name));
    OK(fdt_setprop(dt, n, "compatible", T8132_DART, sizeof(T8132_DART)));
    reg2(dt, n, base, size);
    OK(fdt_setprop_u32(dt, n, "#iommu-cells", 1));
    fdt32_t range[4] = {cpu_to_fdt32(0x100), 0, cpu_to_fdt32(0x10), 0};
    OK(fdt_setprop(dt, n, "apple,dma-range", range, sizeof(range)));
    OK(fdt_setprop_string(dt, n, "status", status));
    OK(fdt_setprop_u32(dt, n, "phandle", ph));
    return n;
}

static void ext_path(char *buf, size_t len, int i)
{
    snprintf(buf, len, "/soc/dcp@%llx", (unsigned long long)ext_coproc[i]);
}

static void build_t8132_ext(void *dt, int i, const char *compat, size_t compat_len, bool enabled,
                            bool placeholders)
{
    const char *status = enabled ? "okay" : "disabled";
    int soc = fdt_path_offset(dt, "/soc");
    t8132_dart(dt, soc, ext_dart[i], 0x20000, PH_EXT_DART + i, status);
    t8132_dart(dt, fdt_path_offset(dt, "/soc"), pipe_dart[i], 0xc000, PH_PIPE_DART + i, status);
    char name[48], path[96];
    snprintf(name, sizeof(name), "dcp@%llx", (unsigned long long)ext_coproc[i]);
    int dcp = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), name));
    OK(fdt_setprop(dt, dcp, "compatible", compat, compat_len));
    reg2(dt, dcp, ext_coproc[i], 0x4000);
    iommus(dt, dcp, PH_EXT_DART + i, 23);
    OK(fdt_setprop_u32(dt, dcp, "apple,dcp-index", 1 + i));
    OK(fdt_setprop_string(dt, dcp, "status", status));
    int pio = OK(fdt_add_subnode(dt, dcp, "piodma"));
    iommus(dt, pio, PH_PIPE_DART + i, 16);
    OK(fdt_setprop_string(dt, pio, "status", "disabled"));
    snprintf(path, sizeof(path), "dcpext%d", i);
    ext_path(name, sizeof(name), i);
    OK(fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), path, name));
    if (!placeholders)
        return;
    fdt32_t mr[3];
    static const char *const parts[] = {"text", "data", "heap"};
    for (int k = 0; k < 3; k++) {
        snprintf(name, sizeof(name), "dcpext%d-%s", i, parts[k]);
        int region = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
        OK(fdt_setprop_string(dt, region, "compatible", "apple,asc-mem"));
        reg2(dt, region, 0, 0x4000);
        OK(fdt_setprop_empty(dt, region, "no-map"));
        OK(fdt_setprop_string(dt, region, "status", "disabled"));
        OK(fdt_setprop_u32(dt, region, "phandle", PH_PLACE + 3 * i + k));
        mr[k] = cpu_to_fdt32(PH_PLACE + 3 * i + k);
    }
    ext_path(path, sizeof(path), i);
    dcp = fdt_path_offset(dt, path);
    OK(fdt_setprop(dt, dcp, "memory-region", mr, sizeof(mr)));
    static const char names[] = "text\0data\0heap";
    OK(fdt_setprop(dt, dcp, "memory-region-names", names, sizeof(names)));
}

static void build_t8132(void *dt, const char *root, size_t root_len, size_t size)
{
    OK(fdt_create_empty_tree(dt, size));
    OK(fdt_setprop(dt, 0, "compatible", root, root_len));
    cells(dt, 0);
    OK(fdt_add_subnode(dt, 0, "chosen"));
    int resv = OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    cells(dt, resv);
    OK(fdt_setprop_empty(dt, resv, "ranges"));
    OK(fdt_add_subnode(dt, 0, "aliases"));
    int soc = OK(fdt_add_subnode(dt, 0, "soc"));
    cells(dt, soc);
    OK(fdt_setprop_empty(dt, soc, "ranges"));
    t8132_dart(dt, soc, M4_DART_DCP, 0x20000, PH_M4_DART_DCP, "disabled");
    t8132_dart(dt, fdt_path_offset(dt, "/soc"), M4_DART_DISP, 0xc000, PH_M4_DART_DISP, "disabled");
    int dcp = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "dcp@30ae00000"));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t8132-dcp"));
    reg2(dt, dcp, M4_DCP, 0x4000);
    iommus(dt, dcp, PH_M4_DART_DCP, 23);
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    int pio = OK(fdt_add_subnode(dt, dcp, "piodma"));
    iommus(dt, pio, PH_M4_DART_DISP, 16);
    OK(fdt_setprop_string(dt, pio, "status", "disabled"));
    int disp = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "display-subsystem"));
    OK(fdt_setprop(dt, disp, "compatible", T8132_DISP, sizeof(T8132_DISP)));
    iommus(dt, disp, PH_M4_DART_DISP, 0);
    OK(fdt_setprop_string(dt, disp, "status", "disabled"));
    int aliases = fdt_path_offset(dt, "/aliases");
    OK(fdt_setprop_string(dt, aliases, "dcp", "/soc/dcp@30ae00000"));
    OK(fdt_setprop_string(dt, aliases, "disp0", "/soc/display-subsystem"));
    OK(fdt_setprop_string(dt, aliases, "disp0_piodma", "/soc/dcp@30ae00000/piodma"));
}

/*
 * What kboot.c's M4 handoff leaves when it is complete: the image's uuid on dcp, and every
 * internal segment reserved as asc-firmware@<phys>, __TEXT with the dcp's mapping.
 */
static void m4_handoff(void *dt, const char *uuid, bool clocks)
{
    int dcp = alias_node(dt, "dcp");
    OK(fdt_setprop_string(dt, dcp, "apple,firmware-uuid", uuid));
    if (clocks) /* firmware 27: /arm-io/disp0's firmware clocks (one is enough here) */
        OK(fdt_setprop_u32(dt, alias_node(dt, "dcp"), "apple,clock-ids", 364));
    OK(fdt_setprop_u32(dt, alias_node(dt, "dcp"), "phandle", 130));
    char name[48];
    snprintf(name, sizeof(name), "asc-firmware@%llx", (unsigned long long)dcp_segs[0].phys);
    int text = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
    OK(fdt_setprop_string(dt, text, "compatible", "apple,asc-mem"));
    reg2(dt, text, dcp_segs[0].phys, ALIGN_UP((u64)dcp_segs[0].size, SZ_16K));
    OK(fdt_setprop_empty(dt, text, "no-map"));
    OK(fdt_setprop_u32(dt, text, "phandle", PH_M4_TEXT));
    fdt32_t tuple[5] = {cpu_to_fdt32(130)};
    fdt64_st(&tuple[1], dcp_segs[0].remap);
    fdt64_st(&tuple[3], ALIGN_UP((u64)dcp_segs[0].size, SZ_16K));
    OK(fdt_setprop(dt, text, "iommu-addresses", tuple, sizeof(tuple)));
}

/* ---- checks ---- */

static const char *image = UUID27; /* the image every nub runs in this scenario */

/* Every property of the node at `path` is the same in both trees (and so are its children's) */
static bool node_same(const void *a, const void *b, const char *path)
{
    int na = fdt_path_offset(a, path), nb = fdt_path_offset(b, path), off;
    if (na < 0 || nb < 0)
        return na < 0 && nb < 0;
    int count_a = 0, count_b = 0;
    fdt_for_each_property_offset(off, a, na)
    {
        const char *name;
        int la, lb;
        const void *va = fdt_getprop_by_offset(a, off, &name, &la);
        const void *vb = fdt_getprop(b, nb, name, &lb);
        if (!vb || la != lb || memcmp(va, vb, la))
            return false;
        count_a++;
    }
    fdt_for_each_property_offset(off, b, nb) count_b++;
    if (count_a != count_b)
        return false;
    int child;
    fdt_for_each_subnode(child, a, na)
    {
        char sub[256];
        snprintf(sub, sizeof(sub), "%s/%s", path, fdt_get_name(a, child, NULL));
        if (!node_same(a, b, sub))
            return false;
    }
    return true;
}

/* A processor the handoff left alone: its node, DARTs and placeholders as they were */
static bool ext_untouched(const void *in, const void *dt, int i)
{
    char path[96];
    ext_path(path, sizeof(path), i);
    bool same = node_same(in, dt, path);
    static const char *const parts[] = {"text", "data", "heap"};
    for (int k = 0; k < 3; k++) {
        snprintf(path, sizeof(path), "/reserved-memory/dcpext%d-%s", i, parts[k]);
        same &= node_same(in, dt, path);
    }
    snprintf(path, sizeof(path), "/clock-dcpext%d-firmware-%u", i, pipe_clk[i][0]);
    return same && fdt_path_offset(dt, path) < 0;
}

static bool tuple_for(const void *dt, int region, int dev, u64 dva, u64 size)
{
    u64 got_size = 0;
    return iommu_dva(dt, region, dev, &got_size) == dva && got_size == size;
}

/* A processor handed over: memory, image, release, its pipe's clocks; status as it was */
static void check_ext(const void *in, const void *dt, int i, bool headless)
{
    char path[96];
    struct seg es[4];
    ext_segs(i, es);
    ext_path(path, sizeof(path), i);
    int ext = fdt_path_offset(dt, path), len = 0;
    const char *in_status = fdt_getprop(in, fdt_path_offset(in, path), "status", NULL);
    check(ext >= 0 && in_status && status_is(dt, ext, in_status), "dcpext%d keeps its status (%s)",
          i, in_status ? in_status : "?");
    check(!fdt_getprop(dt, ext, READY, NULL), "dcpext%d has no 26.6.2 firmware-ready marker", i);
    const char *uuid = fdt_getprop(dt, ext, "apple,firmware-uuid", &len);
    check(uuid && !strcmp(uuid, image), "dcpext%d apple,firmware-uuid is the ADT image's", i);
    const fdt32_t *v = fdt_getprop(dt, ext, "apple,firmware-version", &len);
    check(v && len == 12 && fdt32_ld(&v[0]) == 27 && fdt32_ld(&v[1]) == 2 && !fdt32_ld(&v[2]),
          "dcpext%d apple,firmware-version is 27.2.0", i);
    check(!!fdt_getprop(dt, ext, "apple,firmware-compat", NULL), "dcpext%d firmware-compat", i);

    /* memory-region: the shared __TEXT, then this processor's data and heap */
    int text = region_at(dt, ext, 0), data = region_at(dt, ext, 1), heap = region_at(dt, ext, 2);
    u64 base = 0, size = 0;
    char want[48];
    snprintf(want, sizeof(want), headless ? "dcp-text@%llx" : "asc-firmware@%llx",
             (unsigned long long)es[0].phys);
    check(region_count(dt, ext) == 3 && text >= 0 && !strcmp(fdt_get_name(dt, text, NULL), want) &&
              reg_of(dt, text, &base, &size) && base == es[0].phys &&
              size == ALIGN_UP((u64)es[0].size, SZ_16K),
          "dcpext%d text is /reserved-memory/%s", i, want);
    check(tuple_for(dt, text, ext, es[0].remap, ALIGN_UP((u64)es[0].size, SZ_16K)),
          "dcpext%d maps the shared __TEXT", i);
    check(!headless || fdt_getprop(dt, text, "no-map", NULL), "the shared __TEXT is no-map");
    const int seg_of[3] = {0, 1, 3};
    for (int k = 1; k < 3; k++) {
        int r = k == 1 ? data : heap;
        const struct seg *s = &es[seg_of[k]];
        u64 sz = ALIGN_UP((u64)s->size, SZ_16K);
        check(r >= 0 && reg_of(dt, r, &base, &size) && base == s->phys && size == sz &&
                  status_is(dt, r, "okay") && tuple_for(dt, r, ext, s->remap, sz),
              "dcpext%d %s region 0x%llx+0x%llx at DVA 0x%llx", i, k == 1 ? "data" : "heap",
              (unsigned long long)s->phys, (unsigned long long)sz, (unsigned long long)s->remap);
    }
    snprintf(path, sizeof(path), "/reserved-memory/dcpext%d-text", i);
    check(fdt_path_offset(dt, path) < 0, "dcpext%d text placeholder deleted", i);

    /* its own pipe's clocks */
    const fdt32_t *ids = fdt_getprop(dt, ext, "apple,clock-ids", &len);
    check(ids && len == 8 && fdt32_ld(&ids[0]) == pipe_clk[i][0] &&
              fdt32_ld(&ids[1]) == pipe_clk[i][1],
          "dcpext%d apple,clock-ids = <%u %u> (dispext%d)", i, pipe_clk[i][0], pipe_clk[i][1], i);
    const char *names = fdt_getprop(dt, ext, "clock-names", &len);
    check(names && len == 22 && !memcmp(names, "firmware-0\0firmware-1", 22),
          "dcpext%d clock-names firmware-0, firmware-1", i);
    const fdt32_t *clocks = fdt_getprop(dt, ext, "clocks", &len);
    for (int k = 0; k < 2; k++) {
        snprintf(path, sizeof(path), "/clock-dcpext%d-firmware-%u", i, pipe_clk[i][k]);
        int clk = fdt_path_offset(dt, path);
        const fdt32_t *hz = clk < 0 ? NULL : fdt_getprop(dt, clk, "clock-frequency", &len);
        check(clocks && clk >= 0 && fdt32_ld(&clocks[k]) == fdt_get_phandle(dt, clk) &&
                  !fdt_node_check_compatible(dt, clk, "fixed-clock") && hz && len == 4 &&
                  fdt32_ld(hz) == clk_hz[i][k],
              "%s is a %u Hz fixed clock", path, clk_hz[i][k]);
    }
}

static bool oslog_reserved(const void *dt, u64 phys)
{
    char path[64];
    snprintf(path, sizeof(path), "/reserved-memory/dcp-oslog@%llx", (unsigned long long)phys);
    int n = fdt_path_offset(dt, path);
    return n >= 0 && fdt_getprop(dt, n, "no-map", NULL) &&
           fdt_getprop(dt, n, "apple,dcp-os-log", NULL);
}

static void check_oslogs(const void *dt, bool ext0, bool ext1, bool internal)
{
    struct seg es[4];
    ext_segs(0, es);
    check(oslog_reserved(dt, es[2].phys) == ext0, "dcpext0 __OS_LOG %sreserved",
          ext0 ? "" : "not ");
    ext_segs(1, es);
    check(oslog_reserved(dt, es[2].phys) == ext1, "dcpext1 __OS_LOG %sreserved",
          ext1 ? "" : "not ");
    check(oslog_reserved(dt, dcp_segs[2].phys) == internal, "dcp __OS_LOG %sreserved here",
          internal ? "" : "not ");
}

/* The internal handoff (dcp, its piodma, the display subsystem and disp0's DART) as it was */
static void check_internal_untouched(const void *in, const void *dt)
{
    check(node_same(in, dt, "/soc/dcp@30ae00000") && node_same(in, dt, "/soc/iommu@30a340000"),
          "the internal DCP is untouched");
}

static void check_display(const void *in, const void *dt, bool enabled)
{
    int disp = alias_node(dt, "disp0");
    if (!enabled) {
        check(node_same(in, dt, "/soc/display-subsystem") &&
                  node_same(in, dt, "/soc/iommu@30a300000"),
              "the display subsystem and its DART are untouched");
        return;
    }
    char fb[48];
    snprintf(fb, sizeof(fb), "framebuffer@%llx", (unsigned long long)fb_base);
    int r = region_at(dt, disp, 0);
    check(status_is(dt, disp, "okay") &&
              status_is(dt, fdt_path_offset(dt, "/soc/iommu@30a300000"), "okay") && r >= 0 &&
              !strcmp(fdt_get_name(dt, r, NULL), fb),
          "no panel: the display subsystem and its DART are enabled on the boot framebuffer");
}

/* ---- scenarios ---- */

struct t8132_scenario {
    const char *name;
    const char *what;
};

static const struct t8132_scenario t8132_scenarios[] = {
    {"j713-both", "J713 27.2 beside a complete internal handoff: both processors"},
    {"j713-adt272-layout", "J713 with the segment layout and RAM range of its 27.2 ADT and boot"},
    {"j713-one", "J713: dcpext1 not running, dcpext0 handed over alone"},
    {"j713-refused-uuid", "J713: dcpext1 runs another image than the internal DCP"},
    {"j713-internal-walk-failed", "J713: the internal SID0/SID16 walk failed: nothing external"},
    {"j713-26-6-2", "J713 on 26.6.2: firmware not given clocks, the firmware-ready path's"},
    {"j713-other-image", "J713: another image given clocks by the OS: handed over, no list"},
    {"j713-dcp-no-clocks", "J713: complete outcome but dcp has no firmware clocks"},
    {"j713-pipe-no-clocks", "J713: dispext1 has no clock-ids"},
    {"j713-dcp-uuid-missing", "J713: complete outcome but dcp has no apple,firmware-uuid"},
    {"j713-sid-mismatch", "J713: the ADT mapper's stream is not the DT iommus cell"},
    {"j713-dart-reg-mismatch", "J713: the DT DART is not at the ADT DART's address"},
    {"j713-dma-range-mismatch", "J713: apple,dma-range is not the ADT vm-base/vm-size-23"},
    {"j713-window-too-small", "J713: dcpext0's DART window is smaller than a segment"},
    {"j713-no-mapper", "J713: the ADT has no mapper for the processor"},
    {"j713-ext-unpowered", "J713: DISPEXT1_FE is off"},
    {"j713-cpu-not-requested", "J713: DISPEXT0_CPU is neither on nor requested"},
    {"j713-no-clock-gates", "J713: dcpext1 has no ADT clock-gates"},
    {"j713-dart-unlocked", "J713: dcpext0's firmware DART is not locked"},
    {"j713-stream-no-root", "J713: dcpext1's stream 23 has no valid root"},
    {"j713-stream-bypass", "J713: dcpext0's stream 23 bypasses the DART"},
    {"j713-dart-read-faults", "J713: reading dcpext1's DART faults"},
    {"j713-no-pipe", "J713: no ADT pipe on the piodma stream"},
    {"j713-two-pipes", "J713: two ADT pipes on the piodma stream"},
    {"j713-no-adt-node", "J713: no ADT processor at the DT address"},
    {"j713-text-not-shared", "J713: dcpext1's __TEXT is not the internal one"},
    {"j713-bad-layout", "J713: dcpext0's segment layout is broken"},
    {"j713-has-clocks", "J713: the DT already gives dcpext0 clocks"},
    {"j713-repeat", "J713: a second call changes nothing"},
    {"j713-dt-full", "J713: no room in the device tree"},
    {"j773g-headless", "J773g 27.2, no panel: dcpext0 and the display subsystem, same code"},
    {"j773g-both", "J773g with both placeholders: both processors"},
    {"j773g-enabled", "J773g: an enabled processor is refused with no panel"},
    {"j773g-disp-dart-unlocked", "J773g: disp0's DART is not locked: nothing handed over"},
    {"j773g-disp-unpowered", "J773g: the internal DCP is not powered: nothing handed over"},
    {"j713-tables-in-ram", "J713: dcpext0's and dispext1's DART tables in usable RAM: reserved"},
    {"j713-pipe-dart-idle", "J713: dispext1's DART unlocked and idle: nothing to keep"},
    {"j713-pipe-dart-live", "J713: dispext1's DART unlocked with a live stream"},
    {"j713-four-level", "J713: dcpext0's stream 23 is a four-level translation"},
    {"j713-l2-outside-dram", "J713: a dispext1 L1 entry points outside DRAM"},
    {"j713-pipe-dart-faults", "J713: reading dispext0's DART faults"},
    {"j713-second-parent-off", "J713: DISPEXT1_CPU's second PMGR parent is off"},
    {"j773g-disp-tables-in-ram", "J773g: disp0's DART tables in usable RAM: reserved"},
    {"j773g-disp-piodma-bypass", "J773g: disp0's piodma stream bypasses its DART"},
    {"j713-stream-word1", "J713: dispext1 stream 40 set up (ENABLE_STREAMS word 1)"},
    {"j713-disabled-live", "J713: a disabled stream with live TCR/TTBR leftovers is not judged"},
    {"j713-zero-streams", "J713: dcpext1's DART reports no streams"},
    {"j713-too-many-tables", "J713: dcpext0's stream 23 has 257 table pages"},
    {"j713-shared-table", "J713: two dispext1 streams share their tables: kept once"},
    {"j713-table-in-reservation", "J713: dcpext0's tables inside an existing no-map reservation"},
    {"j713-table-partial-reservation", "J713: dcpext0's root straddles an existing reservation"},
    {"j713-table-small-reservation", "J713: a no-map reservation smaller than a page starts at dcpext0's root"},
    {"j713-table-small-memreserve", "J713: a /memreserve/ smaller than a page starts at dcpext0's root"},
    {"j713-tables-in-ram-refused", "J713: dcpext0 refused after collecting its tables: kept"},
    {"j713-table-in-carveout", "J713: dcpext0's root page is in a memory carveout"},
    {"j713-extra-child-dart", "J713: a second dcpext1 child on an unlocked, live DART"},
    {"j713-iommu-cells-2", "J713: dispext0's DART takes two iommu cells"},
    {"j713-self-parent", "J713: DISPEXT1_CPU lists itself as its second parent"},
    {"j773g-tables-display-fails", "J773g: disp0 tables in RAM, display subsystem not enabled"},
    {"later-soc", "a later board/SoC with the same block, fallback compatible"},
    {"j713-refused-segs-kept", "J713: dcpext1's __DATA and heap in kernel RAM: refused, kept"},
    {"j713-refused-pipe-kept", "J713: dcpext1 refused by its pipe: its DART tables in RAM kept"},
    {"j713-refused-power-kept", "J713: DISPEXT1_FE targeted on but off: refused, memory kept"},
    {"j713-refused-image-kept", "J713: dcpext1 runs another image: refused, memory kept"},
    {"j713-refused-unlocked-kept", "J713: dcpext0's live, unlocked DART: refused, its tables kept"},
    {"j713-off-nothing-kept", "J713: DISPEXT1_CPU power-gated: not running, nothing kept"},
    {"j713-auto-off-kept", "J713: DISPEXT1_CPU gated but under auto power: memory kept"},
    {"j713-internal-incomplete-kept", "J713: internal handoff incomplete: both kept, none published"},
    {"j713-fw27-no-uuid", "J713 27.2, internal nub without uuid: refused, kept, no 26.6.2 marker"},
    {"j713-26-6-2-no-uuid", "J713 26.6.2, no uuid on the internal nub: the firmware-ready path's"},
    {"j713-no-dt-ext-kept", "J713: the device tree has no external processor: the ADT's kept"},
    {"j713-no-adt-node-kept", "J713: dcpext1's DT address matches no ADT processor: ADT one kept"},
    {"j773g-refused-kept", "J773g: disp0's DART unlocked, nothing handed over: both kept"},
};

/* A page of usable RAM for the scenarios that put tables there */
#define RAM_TABLES (TB + 0x100000000ULL)

static bool resv_page(void *dt, u64 page)
{
    char path[64];
    snprintf(path, sizeof(path), "/reserved-memory/dcpext-tables@%llx", (unsigned long long)page);
    int node = fdt_path_offset(dt, path), len;
    const fdt64_t *reg = node < 0 ? NULL : fdt_getprop(dt, node, "reg", &len);
    return reg && len == 16 && fdt64_ld(&reg[0]) == page && fdt64_ld(&reg[1]) == 0x4000 &&
           fdt_getprop(dt, node, "no-map", NULL) &&
           !fdt_node_check_compatible(dt, node, "apple,asc-mem");
}

/* An existing no-map reservation, as an earlier boot stage or handoff leaves one */
static void resv_add(void *dt, const char *name, u64 base, u64 size)
{
    int node = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
    reg2(dt, node, base, size);
    OK(fdt_setprop_empty(dt, node, "no-map"));
}

static int resv_tables(void *dt)
{
    int resv = fdt_path_offset(dt, "/reserved-memory"), node, n = 0;
    fdt_for_each_subnode(node, dt, resv)
    {
        if (!strncmp(fdt_get_name(dt, node, NULL), "dcpext-tables@", 14))
            n++;
    }
    return n;
}

/* Usable RAM for the scenarios that put a processor's __DATA and heap there */
#define RAM_SEGS (TB + 0x120000000ULL)

/* Move segment k of external processor i to `phys` in its ADT nub */
static void ext_seg_move(int i, int k, u64 phys)
{
    char nub[64];
    struct seg es[4];
    adt_path_of(nub, sizeof(nub), "/arm-io/dcpext%d/iop-dcpext%d-nub", i);
    u32 len = 0;
    const void *cur = adt_getprop(adt, adt_path_offset(adt, nub), "segment-ranges", &len);
    memcpy(es, cur, sizeof(es));
    es[k].phys = phys;
    prop_set(nub, "segment-ranges", es, sizeof(es));
}

/* External processor i's __DATA and heap moved into usable RAM */
static void ext_segs_in_ram(int i)
{
    ext_seg_move(i, 1, RAM_SEGS + 0x1000000ULL * i);
    ext_seg_move(i, 3, RAM_SEGS + 0x1000000ULL * i + 0x100000);
}

/* External processor i's firmware DART and pipe DART tables in usable RAM (four pages) */
static void ext_tables_in_ram(int i)
{
    region_add(RAM_TABLES + 0x10000 * i, 0x10000);
    poke32(ext_dart[i] + 0x1400 + 4 * 23, dart_tables(RAM_TABLES + 0x10000 * i));
    poke32(pipe_dart[i] + 0x1400 + 4 * 16, dart_tables(RAM_TABLES + 0x10000 * i + 0x8000));
}

static int resv_count(void *dt, const char *prefix)
{
    int resv = fdt_path_offset(dt, "/reserved-memory"), node, n = 0;
    fdt_for_each_subnode(node, dt, resv)
    {
        if (!strncmp(fdt_get_name(dt, node, NULL), prefix, strlen(prefix)))
            n++;
    }
    return n;
}

/* /reserved-memory/dcpext-firmware@<base>: exactly base+size, no-map, apple,asc-mem */
static bool resv_firmware(void *dt, u64 base, u64 size)
{
    char path[64];
    snprintf(path, sizeof(path), "/reserved-memory/dcpext-firmware@%llx", (unsigned long long)base);
    int node = fdt_path_offset(dt, path), len;
    const fdt64_t *reg = node < 0 ? NULL : fdt_getprop(dt, node, "reg", &len);
    return reg && len == 16 && fdt64_ld(&reg[0]) == base && fdt64_ld(&reg[1]) == size &&
           fdt_getprop(dt, node, "no-map", NULL) &&
           !fdt_node_check_compatible(dt, node, "apple,asc-mem");
}

/* External processor i's __DATA (2 pages) and heap (4 pages) in RAM are kept */
static bool ext_segs_kept(void *dt, int i)
{
    return resv_firmware(dt, RAM_SEGS + 0x1000000ULL * i, 0x8000) &&
           resv_firmware(dt, RAM_SEGS + 0x1000000ULL * i + 0x100000, 0x10000);
}

/* External processor i's four table pages in RAM (ext_tables_in_ram()) are kept */
static bool ext_tables_kept(void *dt, int i)
{
    bool all = true;
    for (int k = 0; k < 4; k++)
        all &= resv_page(dt, RAM_TABLES + 0x10000 * i + 0x4000 * k);
    return all;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(t8132_scenarios); i++)
            printf("%s\n", t8132_scenarios[i].name);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s list | <scenario>\n", argv[0]);
        return 2;
    }
    const char *s = argv[1];
    const struct t8132_scenario *sc = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(t8132_scenarios); i++)
        if (!strcmp(s, t8132_scenarios[i].name))
            sc = &t8132_scenarios[i];
    if (!sc) {
        fprintf(stderr, "unknown scenario %s\n", s);
        return 2;
    }
    printf("%s: %s\n", s, sc->what);

    bool j773g = !strncmp(s, "j773g", 5), later = is(s, "later-soc");
    bool unknown = is(s, "j713-26-6-2") || is(s, "j713-26-6-2-no-uuid");
    if (is(s, "j713-other-image"))
        image = UUID_X;
    if (unknown)
        image = UUID262;
    if (is(s, "j713-adt272-layout")) {
        layout272 = true;
        dcp_segs = j713_272_segs;
        dcp_nsegs = ARRAY_SIZE(j713_272_segs);
    }
    world_t8132(j773g, image);
    if (unknown)
        os_firmware =
            (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};

    static u8 dt[DT_SIZE], in[DT_SIZE];
    size_t size = DT_SIZE;
    const char *root = j773g ? J773G : later ? J9XX : J713;
    build_t8132(dt, root, j773g ? sizeof(J773G) : sizeof(J713), size);
    for (int i = 0; i < 2 && !is(s, "j713-no-dt-ext-kept"); i++)
        build_t8132_ext(dt, i, later ? T8140_EXT : T8132_EXT,
                        later ? sizeof(T8140_EXT) : sizeof(T8132_EXT),
                        !j773g || is(s, "j773g-enabled"), !is(s, "j773g-headless") || i == 0);
    enum dcp_internal internal = j773g ? DCP_INTERNAL_NO_PANEL : DCP_INTERNAL_COMPLETE;
    if (!j773g && !is(s, "j713-dcp-uuid-missing"))
        m4_handoff(dt, image, !unknown && !is(s, "j713-dcp-no-clocks"));
    if (is(s, "j713-internal-walk-failed") || is(s, "j713-internal-incomplete-kept"))
        internal = DCP_INTERNAL_NONE;

    /* ADT changes */
    if (is(s, "j713-one"))
        prop_u32("/arm-io/dcpext1/iop-dcpext1-nub", "running", 0);
    if (is(s, "j713-refused-uuid"))
        prop_str("/arm-io/dcpext1/iop-dcpext1-nub", "uuid", UUID_X);
    if (is(s, "j713-sid-mismatch")) {
        prop_u32("/arm-io/dart-dcpext0/mapper-dcpext0", "reg", 22);
        prop_u64("/arm-io/dart-dcpext0", "vm-size-22", 0x1000000000ULL);
    }
    if (is(s, "j713-dart-reg-mismatch"))
        node_get("/arm-io/dart-dcpext1")->reg[0][0] += 0x100000;
    if (is(s, "j713-dma-range-mismatch"))
        prop_u64("/arm-io/dart-dcpext0", "vm-size-23", 0x800000000ULL);
    if (is(s, "j713-window-too-small")) {
        /* ADT and DT agree on a one-page window: the 16K-rounded segments do not fit in it */
        prop_u64("/arm-io/dart-dcpext0", "vm-size-23", 0x4000);
        int dart = fdt_node_offset_by_phandle(dt, PH_EXT_DART);
        fdt32_t range[4] = {cpu_to_fdt32(0x100), 0, 0, cpu_to_fdt32(0x4000)};
        OK(fdt_setprop(dt, dart, "apple,dma-range", range, sizeof(range)));
    }
    if (is(s, "j713-no-mapper"))
        node_drop("/arm-io/dart-dcpext1/mapper-dcpext1");
    if (is(s, "j713-ext-unpowered"))
        poke32(ps_reg(pm_sys(1) + 1), 0x0f); /* target on, actual off */
    if (is(s, "j713-cpu-not-requested"))
        poke32(ps_reg(pm_sys(0) + 2), 0x0);
    if (is(s, "j713-no-clock-gates"))
        prop_drop("/arm-io/dcpext1", "clock-gates");
    if (is(s, "j713-dart-unlocked"))
        poke32(ext_dart[0] + 0x200, 0x1e0);
    if (is(s, "j713-stream-no-root"))
        poke32(ext_dart[1] + 0x1400 + 4 * 23, 0x40000);
    if (is(s, "j713-stream-bypass"))
        poke32(ext_dart[0] + 0x1000 + 4 * 23, 0x3);
    if (is(s, "j713-dart-read-faults"))
        region_of(ext_dart[1])->base = 0x7f0000000ULL; /* dcpext1's DART: nothing answers */
    if (is(s, "j773g-disp-dart-unlocked"))
        poke32(M4_DART_DISP + 0x200, 0x1e0);
    if (is(s, "j773g-disp-unpowered"))
        poke32(ps_reg(PM_DISP0_CPU), 0x0);
    if (is(s, "j713-pipe-no-clocks"))
        prop_drop("/arm-io/dispext1", "clock-ids");
    if (is(s, "j713-no-pipe"))
        node_drop("/arm-io/dispext1");
    if (is(s, "j713-two-pipes")) {
        u32 parents[1] = {aph_mapper_disp(0, 1)};
        prop_str("/arm-io/dispext9", "device_type", "ext-display-subsystem");
        prop_set("/arm-io/dispext9", "iommu-parent", parents, sizeof(parents));
    }
    if (is(s, "j713-no-adt-node"))
        node_get("/arm-io/dcpext1")->reg[0][0] = 0x50ae00000ULL;
    if (is(s, "j713-text-not-shared")) {
        struct seg es[4];
        ext_segs(1, es);
        es[0].phys += 0x4000000;
        prop_set("/arm-io/dcpext1/iop-dcpext1-nub", "segment-ranges", es, sizeof(es));
    }
    if (is(s, "j713-bad-layout"))
        prop_str("/arm-io/dcpext0/iop-dcpext0-nub", "segment-names", "__TEXT;__OS_LOG;__DATA;x");
    if (is(s, "j713-has-clocks")) {
        char path[96];
        ext_path(path, sizeof(path), 0);
        OK(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "clocks", 7));
    }
    if (is(s, "j713-dt-full"))
        OK(fdt_pack(dt));
    if (is(s, "j713-tables-in-ram") || is(s, "j773g-disp-tables-in-ram")) {
        region_add(RAM_TABLES, 0x10000);
        if (j773g) {
            poke32(M4_DART_DISP + 0x1400 + 4 * 16, dart_tables(RAM_TABLES));
        } else {
            poke32(ext_dart[0] + 0x1400 + 4 * 23, dart_tables(RAM_TABLES));
            poke32(pipe_dart[1] + 0x1400 + 4 * 16, dart_tables(RAM_TABLES + 0x8000));
        }
    }
    if (is(s, "j713-pipe-dart-idle") || is(s, "j713-pipe-dart-live")) {
        poke32(pipe_dart[1] + 0x200, 0x1e0);
        poke32(pipe_dart[1] + 0x1000, 0);
        poke32(pipe_dart[1] + 0x1000 + 4 * 16, is(s, "j713-pipe-dart-live") ? 0x1 : 0);
    }
    if (is(s, "j713-four-level"))
        poke32(ext_dart[0] + 0x1000 + 4 * 23, 0x9);
    if (is(s, "j713-l2-outside-dram"))
        poke64(ttbr_root(dart_ttbr(pipe_dart[1], 0)) + 16, (0x7f0000000ULL >> 4) | 1);
    if (is(s, "j713-pipe-dart-faults"))
        region_of(pipe_dart[0])->base = 0x7e0000000ULL;
    if (is(s, "j713-second-parent-off")) {
        u16 cpu = pm_sys(1) + 2, aux = pm_sys(1) + 3;
        fdev_add(aux, "DISPEXT1_AUX", false, ps_reg(aux), 0);
        for (int i = 0; i < nfdevs; i++)
            if (fdevs[i].id == cpu)
                fdevs[i].parent[1] = aux;
        poke32(ps_reg(aux), 0x0);
    }
    if (is(s, "j773g-disp-piodma-bypass"))
        poke32(M4_DART_DISP + 0x1000 + 4 * 16, 0x3);
    if (is(s, "j713-stream-word1")) {
        poke32(pipe_dart[1] + 0xc, 64);
        dart_regs(pipe_dart[1], 40, 0x1ff, 0x1, 1);
    }
    if (is(s, "j713-disabled-live")) {
        /* stream 5: translating through a root outside DRAM, but its enable bit is clear */
        poke32(ext_dart[0] + 0x1000 + 4 * 5, 0x1);
        poke32(ext_dart[0] + 0x1400 + 4 * 5, 0x7);
    }
    if (is(s, "j713-zero-streams"))
        poke32(ext_dart[1] + 0xc, 0);
    if (is(s, "j713-too-many-tables")) {
        u64 base = TB + 0x3f8000000ULL;
        region_add(base, 258 * 0x4000);
        for (int i = 0; i < 256; i++)
            poke64(base + 8 * i, ((base + 0x4000 * (u64)(i + 1)) >> 4) | 1);
        poke32(ext_dart[0] + 0x1400 + 4 * 23, (u32)((base >> 14) << 2) | 1);
    }
    if (is(s, "j713-shared-table"))
        poke32(pipe_dart[1] + 0x1400 + 4 * 16, dart_ttbr(pipe_dart[1], 0));
    if (is(s, "j713-table-in-reservation") || is(s, "j713-table-partial-reservation")) {
        region_add(RAM_TABLES, 0x10000);
        poke32(ext_dart[0] + 0x1400 + 4 * 23, dart_tables(RAM_TABLES));
        if (is(s, "j713-table-in-reservation"))
            resv_add(dt, "fw-test@10100000000", RAM_TABLES, 0x8000);
        else
            resv_add(dt, "fw-test@10100002000", RAM_TABLES + 0x2000, 0x4000);
    }
    if (is(s, "j713-table-small-reservation") || is(s, "j713-table-small-memreserve")) {
        region_add(RAM_TABLES, 0x10000);
        poke32(ext_dart[0] + 0x1400 + 4 * 23, dart_tables(RAM_TABLES));
        if (is(s, "j713-table-small-reservation"))
            resv_add(dt, "fw-test@10100000000", RAM_TABLES, 0x2000);
        else
            OK(fdt_add_mem_rsv(dt, RAM_TABLES, 0x2000));
    }
    if (is(s, "j713-tables-in-ram-refused")) {
        region_add(RAM_TABLES, 0x10000);
        poke32(ext_dart[0] + 0x1400 + 4 * 23, dart_tables(RAM_TABLES));
        poke32(pipe_dart[1] + 0x1400 + 4 * 16, dart_tables(RAM_TABLES + 0x8000));
        char path[96];
        ext_path(path, sizeof(path), 0);
        OK(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "clocks", 7));
    }
    if (is(s, "j713-table-in-carveout")) {
        mcc_carveouts[0] = (struct mcc_carveout){ttbr_root(dart_ttbr(ext_dart[0], 23)), 0x4000};
        mcc_carveout_count = 1;
    }
    if (is(s, "j713-extra-child-dart")) {
        const u64 extra = 0x48a380000ULL;
        dart_regs(extra, 3, 0x1e0, 0x1, 0);
        t8132_dart(dt, fdt_path_offset(dt, "/soc"), extra, 0x4000, PH_EXTRA_DART, "disabled");
        char path[96];
        ext_path(path, sizeof(path), 1);
        int child = OK(fdt_add_subnode(dt, fdt_path_offset(dt, path), "extra"));
        iommus(dt, child, PH_EXTRA_DART, 3);
    }
    if (is(s, "j713-iommu-cells-2"))
        OK(fdt_setprop_u32(dt, fdt_node_offset_by_phandle(dt, PH_PIPE_DART), "#iommu-cells", 2));
    if (is(s, "j713-self-parent")) {
        u16 cpu = pm_sys(1) + 2;
        for (int i = 0; i < nfdevs; i++)
            if (fdevs[i].id == cpu)
                fdevs[i].parent[1] = cpu;
    }
    if (is(s, "j773g-tables-display-fails")) {
        region_add(RAM_TABLES, 0x10000);
        poke32(M4_DART_DISP + 0x1400 + 4 * 16, dart_tables(RAM_TABLES));
        OK(fdt_setprop_string(dt, alias_node(dt, "disp0"), "status", "okay"));
    }

    /* A refused processor's memory in the RAM the OS gets */
    if (is(s, "j713-refused-segs-kept"))
        ext_segs_in_ram(1);
    if (is(s, "j713-refused-pipe-kept")) {
        node_drop("/arm-io/dispext1");
        ext_tables_in_ram(1);
    }
    if (is(s, "j713-refused-power-kept") || is(s, "j713-off-nothing-kept") ||
        is(s, "j713-auto-off-kept")) {
        ext_segs_in_ram(1);
        ext_tables_in_ram(1);
        u16 sys = pm_sys(1);
        if (is(s, "j713-refused-power-kept"))
            poke32(ps_reg(sys + 1), 0x0f); /* FE: target on, actual off */
        else
            poke32(ps_reg(sys + 2), is(s, "j713-auto-off-kept") ? 0x10000000u : 0x0);
    }
    if (is(s, "j713-refused-image-kept")) {
        prop_str("/arm-io/dcpext1/iop-dcpext1-nub", "uuid", UUID_X);
        ext_segs_in_ram(1);
        ext_tables_in_ram(1);
    }
    if (is(s, "j713-refused-unlocked-kept")) {
        ext_tables_in_ram(0);
        poke32(ext_dart[0] + 0x200, 0x1e0);
    }
    if (is(s, "j713-internal-incomplete-kept") || is(s, "j713-fw27-no-uuid") ||
        is(s, "j713-26-6-2-no-uuid") || is(s, "j713-no-dt-ext-kept") ||
        is(s, "j773g-refused-kept")) {
        for (int i = 0; i < 2; i++) {
            ext_segs_in_ram(i);
            ext_tables_in_ram(i);
        }
    }
    if (is(s, "j713-fw27-no-uuid") || is(s, "j713-26-6-2-no-uuid"))
        prop_drop("/arm-io/dcp/iop-dcp-nub", "uuid");
    if (is(s, "j713-no-adt-node-kept")) {
        node_get("/arm-io/dcpext1")->reg[0][0] = 0x50ae00000ULL;
        ext_segs_in_ram(1);
    }
    if (is(s, "j773g-refused-kept"))
        poke32(M4_DART_DISP + 0x200, 0x1e0);

    memcpy(in, dt, fdt_totalsize(dt));
    mmio_reads = mmio_writes = 0;
    bool fw_clocks = os_firmware.version >= V27_0; /* kboot.c's dt_dcp_fw_clocks() */
    int ret = dt_set_dcpext_handoff(dt, internal, fw_clocks);
    if (is(s, "j713-repeat")) {
        static u8 once[DT_SIZE];
        memcpy(once, dt, fdt_totalsize(dt));
        check(dt_set_dcpext_handoff(dt, internal, fw_clocks) == 1, "the second call is ours too");
        check(fdt_totalsize(dt) == fdt_totalsize(once) && node_same(once, dt, "/"),
              "the second call changes nothing");
    }

    check(!mmio_writes, "no register written (%d reads, %d writes)", mmio_reads, mmio_writes);
    check(fdt_check_header(dt) == 0, "the device tree is valid");
    check(ret == !unknown, "returns %d (%s)", ret, unknown ? "not ours" : "ours");

    if (is(s, "j713-both") || is(s, "j713-repeat") || layout272 || is(s, "j713-other-image")) {
        check(!resv_tables(dt), "tables outside usable RAM are not reserved (%d)", resv_tables(dt));
        check_ext(in, dt, 0, false);
        check_ext(in, dt, 1, false);
        check_oslogs(dt, true, true, false);
        check_display(in, dt, false);
        check(fdt_getprop(dt, alias_node(dt, "dcp"), "apple,firmware-uuid", NULL) &&
                  node_same(in, dt, "/soc/dcp@30ae00000"),
              "the internal DCP node is untouched");
        u64 sz;
        char tpath[64];
        snprintf(tpath, sizeof(tpath), "/reserved-memory/asc-firmware@%llx",
                 (unsigned long long)dcp_segs[0].phys);
        int text = fdt_path_offset(dt, tpath);
        check(iommu_dva(dt, text, alias_node(dt, "dcp"), &sz) == dcp_segs[0].remap,
              "the internal __TEXT keeps the dcp's mapping");
    } else if (is(s, "j713-one") || is(s, "j713-refused-uuid") || is(s, "j713-dart-reg-mismatch") ||
               is(s, "j713-no-mapper") || is(s, "j713-no-pipe") || is(s, "j713-no-adt-node") ||
               is(s, "j713-text-not-shared") || is(s, "j713-pipe-no-clocks") ||
               is(s, "j713-ext-unpowered") || is(s, "j713-no-clock-gates") ||
               is(s, "j713-stream-no-root") || is(s, "j713-dart-read-faults") ||
               is(s, "j713-pipe-dart-live") || is(s, "j713-l2-outside-dram") ||
               is(s, "j713-second-parent-off")) {
        check_ext(in, dt, 0, false);
        check(ext_untouched(in, dt, 1), "dcpext1 is left as it was");
        check_internal_untouched(in, dt);
        check_display(in, dt, false);
    } else if (is(s, "j713-sid-mismatch") || is(s, "j713-dma-range-mismatch") ||
               is(s, "j713-window-too-small") || is(s, "j713-cpu-not-requested") ||
               is(s, "j713-dart-unlocked") || is(s, "j713-stream-bypass") ||
               is(s, "j713-bad-layout") || is(s, "j713-has-clocks") || is(s, "j713-two-pipes") ||
               is(s, "j713-four-level") || is(s, "j713-pipe-dart-faults")) {
        check(ext_untouched(in, dt, 0), "dcpext0 is left as it was");
        check_ext(in, dt, 1, false);
        check_internal_untouched(in, dt);
    } else if (is(s, "j713-internal-walk-failed") || is(s, "j713-dcp-uuid-missing") ||
               is(s, "j713-dcp-no-clocks") || is(s, "j713-dt-full")) {
        check(ext_untouched(in, dt, 0) && ext_untouched(in, dt, 1),
              "no external processor is handed over");
        check_internal_untouched(in, dt);
        check_display(in, dt, false);
        if (!is(s, "j713-dt-full"))
            check_oslogs(dt, true, true, false);
    } else if (unknown) {
        check(fdt_totalsize(in) == fdt_totalsize(dt) && !memcmp(in, dt, fdt_totalsize(dt)),
              "the device tree is byte for byte unchanged");
    } else if (is(s, "j773g-headless")) {
        check_ext(in, dt, 0, true);
        check(ext_untouched(in, dt, 1), "dcpext1 (no placeholders in this tree) is left alone");
        check_oslogs(dt, true, true, true);
        check_display(in, dt, true);
        check(node_same(in, dt, "/soc/dcp@30ae00000"), "the internal DCP node is untouched");
    } else if (is(s, "j773g-both") || later) {
        check_ext(in, dt, 0, j773g);
        check_ext(in, dt, 1, j773g);
        check_display(in, dt, j773g);
    } else if (is(s, "j713-stream-word1") || is(s, "j713-disabled-live") ||
               is(s, "j713-shared-table") || is(s, "j713-table-in-reservation") ||
               is(s, "j713-self-parent")) {
        check_ext(in, dt, 0, false);
        check_ext(in, dt, 1, false);
        check_internal_untouched(in, dt);
        check(!resv_tables(dt), "no table page is reserved (%d)", resv_tables(dt));
    } else if (is(s, "j713-zero-streams") || is(s, "j713-extra-child-dart")) {
        check_ext(in, dt, 0, false);
        check(ext_untouched(in, dt, 1), "dcpext1 is left as it was");
        check_internal_untouched(in, dt);
    } else if (is(s, "j713-iommu-cells-2") || is(s, "j713-too-many-tables") ||
               is(s, "j713-table-partial-reservation") || is(s, "j713-tables-in-ram-refused") ||
               is(s, "j713-table-small-reservation") || is(s, "j713-table-small-memreserve") ||
               is(s, "j713-table-in-carveout")) {
        check(ext_untouched(in, dt, 0), "dcpext0 is left as it was");
        check_ext(in, dt, 1, false);
        check_internal_untouched(in, dt);
        if (is(s, "j713-tables-in-ram-refused"))
            check(resv_page(dt, RAM_TABLES) && resv_page(dt, RAM_TABLES + 0x4000) &&
                      resv_page(dt, RAM_TABLES + 0x8000) && resv_page(dt, RAM_TABLES + 0xc000) &&
                      resv_tables(dt) == 4,
                  "the handed-over and the refused processor's table pages are reserved (%d)",
                  resv_tables(dt));
        else if (is(s, "j713-table-small-reservation") || is(s, "j713-table-small-memreserve"))
            check(!resv_page(dt, RAM_TABLES) && resv_page(dt, RAM_TABLES + 0x4000) &&
                      resv_tables(dt) == 1,
                  "the refused processor's L2 page is kept, its part-held root is not (%d)",
                  resv_tables(dt));
        else
            check(!resv_tables(dt), "no table page is reserved (%d)", resv_tables(dt));
    } else if (is(s, "j773g-tables-display-fails")) {
        check(!resv_tables(dt), "disp0's tables are not reserved without the display (%d)",
              resv_tables(dt));
        check(node_same(in, dt, "/soc/iommu@30a300000"), "disp0's DART is untouched");
    } else if (is(s, "j713-tables-in-ram") || is(s, "j713-pipe-dart-idle")) {
        check_ext(in, dt, 0, false);
        check_ext(in, dt, 1, false);
        check_internal_untouched(in, dt);
        if (is(s, "j713-tables-in-ram"))
            check(resv_page(dt, RAM_TABLES) && resv_page(dt, RAM_TABLES + 0x4000) &&
                      resv_page(dt, RAM_TABLES + 0x8000) && resv_page(dt, RAM_TABLES + 0xc000) &&
                      resv_tables(dt) == 4,
                  "exactly the four table pages in usable RAM are reserved (%d)", resv_tables(dt));
        else
            check(!resv_tables(dt), "no table page is reserved (%d)", resv_tables(dt));
    } else if (is(s, "j773g-disp-tables-in-ram")) {
        check_ext(in, dt, 0, true);
        check_display(in, dt, true);
        check(resv_page(dt, RAM_TABLES) && resv_page(dt, RAM_TABLES + 0x4000) &&
                  resv_tables(dt) == 2,
              "disp0's two table pages in usable RAM are reserved (%d)", resv_tables(dt));
    } else if (is(s, "j773g-enabled") || is(s, "j773g-disp-dart-unlocked") ||
               is(s, "j773g-disp-unpowered") || is(s, "j773g-disp-piodma-bypass")) {
        check(ext_untouched(in, dt, 0) && ext_untouched(in, dt, 1),
              "enabled processors are refused with no panel");
        check_display(in, dt, false);
    }
    if (is(s, "j713-refused-segs-kept") || is(s, "j713-refused-pipe-kept") ||
        is(s, "j713-refused-power-kept") || is(s, "j713-refused-image-kept") ||
        is(s, "j713-off-nothing-kept") || is(s, "j713-auto-off-kept") ||
        is(s, "j713-no-adt-node-kept")) {
        bool off = is(s, "j713-off-nothing-kept");
        check_ext(in, dt, 0, false);
        check(ext_untouched(in, dt, 1), "dcpext1 is not published (no uuid, version, clocks, "
                                        "memory-region or 26.6.2 marker)");
        check_internal_untouched(in, dt);
        /* No device tree node at the ADT processor's address: its __OS_LOG is kept as firmware */
        bool unmatched = is(s, "j713-no-adt-node-kept");
        check_oslogs(dt, true, !unmatched, false);
        if (is(s, "j713-refused-segs-kept") || is(s, "j713-refused-power-kept") ||
            is(s, "j713-refused-image-kept") || is(s, "j713-auto-off-kept") || unmatched)
            check(ext_segs_kept(dt, 1) && resv_count(dt, "dcpext-firmware@") == 2 + unmatched,
                  "dcpext1's __DATA and heap in RAM are reserved, no-map (%d)",
                  resv_count(dt, "dcpext-firmware@"));
        else
            check(!resv_count(dt, "dcpext-firmware@"), "no firmware page is reserved (%d)",
                  resv_count(dt, "dcpext-firmware@"));
        if (is(s, "j713-refused-pipe-kept") || is(s, "j713-refused-image-kept"))
            check(ext_tables_kept(dt, 1) && resv_tables(dt) == 4,
                  "dcpext1's four DART table pages in RAM are reserved (%d)", resv_tables(dt));
        else /* not running, or not fully powered: its DARTs are not read */
            check(!resv_tables(dt), "no table page is reserved (%d)", resv_tables(dt));
        if (off)
            check(!resv_count(dt, "dcpext-"), "a processor that is not running keeps nothing");
    } else if (is(s, "j713-refused-unlocked-kept")) {
        check(ext_untouched(in, dt, 0), "dcpext0 is not published");
        check_ext(in, dt, 1, false);
        check(ext_tables_kept(dt, 0) && resv_tables(dt) == 4,
              "the unlocked DART's live tables are reserved (%d)", resv_tables(dt));
    } else if (is(s, "j713-internal-incomplete-kept") || is(s, "j713-fw27-no-uuid") ||
               is(s, "j713-no-dt-ext-kept") || is(s, "j773g-refused-kept")) {
        if (!is(s, "j713-no-dt-ext-kept"))
            check(ext_untouched(in, dt, 0) && ext_untouched(in, dt, 1),
                  "no external processor is published (no uuid, clocks or 26.6.2 marker)");
        /* With no device tree node there is no dcp-oslog@ reservation: __OS_LOG is kept too */
        int fw_nodes = is(s, "j713-no-dt-ext-kept") ? 6 : 4;
        check(ext_segs_kept(dt, 0) && ext_segs_kept(dt, 1) &&
                  resv_count(dt, "dcpext-firmware@") == fw_nodes,
              "both processors' __DATA and heap in RAM are reserved (%d)",
              resv_count(dt, "dcpext-firmware@"));
        bool tables = !is(s, "j713-no-dt-ext-kept");
        check(tables ? ext_tables_kept(dt, 0) && ext_tables_kept(dt, 1) && resv_tables(dt) == 8
                     : resv_page(dt, RAM_TABLES) && resv_page(dt, RAM_TABLES + 0x10000) &&
                           resv_tables(dt) == 4,
              "their DART table pages in RAM are reserved (%d; with no device tree node, the ADT "
              "DART's only)",
              resv_tables(dt));
        if (!is(s, "j773g-refused-kept"))
            check_internal_untouched(in, dt);
        else
            check_display(in, dt, false);
    } else if (is(s, "j713-26-6-2-no-uuid")) {
        check(fdt_totalsize(in) == fdt_totalsize(dt) && !memcmp(in, dt, fdt_totalsize(dt)),
              "the device tree is byte for byte unchanged");
    }
    if (failures) {
        printf("%s: %d check(s) failed\n", s, failures);
        return 1;
    }
    printf("%s: pass\n", s);
    return 0;
}
