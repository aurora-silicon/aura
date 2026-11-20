/* SPDX-License-Identifier: MIT */
/*
 * Host test of the T8132 Mac mini (J773g) display handoff on macOS 27.2 (26B123): the internal
 * DCP is not handed over, dcpext0 and dcpext1 are, as on the J575c, each with its own pipe's
 * firmware clocks. tests/python/test_t8132_j773g_27_display_handoff.py has the failure model.
 *
 * Reuses the fake ADT, PMGR, MMIO and checks of t6041_display_host.c, with the T8132 register
 * blocks, stream IDs and nub layouts of the J773g's 27.2 ADT. Physical addresses of firmware
 * segments are synthetic; nothing here is Apple firmware data.
 *
 *   t8132-j773g-27-display-host list
 *   t8132-j773g-27-display-host <scenario>
 */
#define main desktop26_main
#include "t6041_display_host.c"
#undef main

#define J773G_UUID     "B3BA603C-3CA7-3E21-B393-44EB20BD2AEF"
#define OTHER_UUID     "0F1E2D3C-4B5A-3968-8776-A5B4C3D2E1F0"
#define J773G_DCP      0x30ae00000ULL
#define J773G_DART_DCP 0x30a340000ULL
#define J773G_DART_DSP 0x30a300000ULL
#define J773G_PMGR     0x380700000ULL /* synthetic */
#define J773G_PS_DISP  (J773G_PMGR + 0x10000)
#define J773G_FB       (TB + 0x2a0000000ULL)
#define J773G_FB_SIZE  0x200000ULL
#define EXT_TEXT_PHYS  (TB + 0x280000000ULL)
#define EXT_TEXT_REMAP (TB + 0x1c4c000ULL)
#define EXT_TEXT_SIZE  0x10000

static const char J773G[] = "apple,j773g\0apple,t8132\0apple,arm-platform";
static const char J713[] = "apple,j713\0apple,t8132\0apple,arm-platform";
static const char J773G_DISP[] = "apple,t8132-display-subsystem\0apple,t8132-display-diagnostics";
static const char T8132_DART[] = "apple,t8132-dart\0apple,t8110-dart";

static const u64 ext_base[2] = {0x286e00000ULL, 0x48ae00000ULL};
static const u64 ext_dart[2] = {0x286340000ULL, 0x48a340000ULL};
static const u32 ext_clk[2][2] = {{369, 432}, {372, 433}};
#define EXT_HZ 533333328u

/* The internal nub: __TEXT, __DATA, __OS_LOG and three heaps, as on the 27.2 J773g */
static const struct seg dcp_segs[6] = {
    {EXT_TEXT_PHYS, 0, EXT_TEXT_REMAP, EXT_TEXT_SIZE, 1},
    {TB + 0x281000000ULL, 0x10000, TB + 0x2490000, 0x8000, 0},
    {TB + 0x24a4000ULL, 0x18000, TB + 0x24a4000ULL, 0x20000, 0xa},
    {TB + 0x281100000ULL, ~0ULL, TB + 0xb70000, 0x40000, 2},
    {TB + 0x281180000ULL, ~0ULL, TB + 0x1b88000, 0x4000, 2},
    {TB + 0x2811c0000ULL, ~0ULL, TB + 0x1b8c000, 0xc000, 2},
};

static u64 ext_data(int i)
{
    return TB + 0x281200000ULL + i * 0x200000;
}
static u64 ext_heap(int i)
{
    return TB + 0x282200000ULL + i * 0x200000;
}
static u64 ext_oslog(int i)
{
    return TB + 0x28030000ULL + i * 0x40000;
}
/* Every processor has its own DART: data and heap are at the same DVAs on both, as on the Mac */
#define EXT_DATA_REMAP (TB + 0x2490000ULL)
#define EXT_HEAP_REMAP (TB + 0x27bc000ULL)
#define EXT_DATA_SIZE  0x8000
#define EXT_HEAP_SIZE  0x10000

static void world_j773g(void)
{
    chip_id = T8132;
    os_firmware = (struct fw_version_info){V27_2, "27.2", {27, 2, 0}, 3, "mBoot-20457.41.3"};
    ram_base = TB + 0x800000;
    ram_size = 0x200000000ULL - 0x800000;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x300000000ULL);
    mcc_carveout_count = 0;
    fb_base = J773G_FB;
    cur_boot_args.video.stride = 0x4000;
    cur_boot_args.video.height = J773G_FB_SIZE / 0x4000;

    const char *nub = "/arm-io/dcp/iop-dcp-nub";
    prop_set(nub, "segment-ranges", dcp_segs, sizeof(dcp_segs));
    prop_str(nub, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown");
    prop_str(nub, "uuid", J773G_UUID);
    prop_u32(nub, "pre-loaded", 1);
    prop_u32(nub, "running", 1);

    reg_add("/arm-io/dcp", J773G_DCP, 0x88000);
    prop_str("/arm-io/dcp", "compatible", "iop,ascwrap-v6");
    prop_str("/arm-io/dart-dcp", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-dcp", J773G_DART_DCP, 0x20000);
    prop_u64("/arm-io/dart-dcp", "vm-base", TB);
    prop_u64("/arm-io/dart-dcp", "vm-size", 0x30000000000ULL);
    prop_str("/arm-io/dart-disp0", "compatible", "dart,t8110");
    reg_add("/arm-io/dart-disp0", J773G_DART_DSP, 0xc000);
    prop_u64("/arm-io/dart-disp0", "vm-base", TB);
    prop_u64("/arm-io/dart-disp0", "vm-size", 0x30000000000ULL);
    prop_str("/arm-io/disp0", "compatible", "disp0,t8132");

    u32 hz[300] = {0};
    hz[364 - 256] = 300000000;
    hz[ext_clk[0][0] - 256] = EXT_HZ;
    hz[ext_clk[1][0] - 256] = EXT_HZ;
    prop_set("/arm-io", "clock-frequencies", hz, sizeof(hz));
    u32 disp0_ids[] = {364, 431};
    prop_set("/arm-io/disp0", "clock-ids", disp0_ids, sizeof(disp0_ids));

    nfdevs = 0;
    fdev_add(0x10, "DISP_FE", false, J773G_PMGR + 0x208, 0);
    fdev_add(0x11, "DISP_CPU", false, J773G_PS_DISP, 0x10);
    fdev_add(0x12, "DCP_VIRT", true, 0, 0x11);
    u32 dcp_cg[1] = {0x12};
    prop_set("/arm-io/dcp", "clock-gates", dcp_cg, sizeof(dcp_cg));

    region_add(J773G_PMGR, 0x14000);
    poke32(J773G_PS_DISP, 0xff);
    poke32(J773G_PMGR + 0x208, 0xff);
    region_add(J773G_DCP, 0x4000);
    poke32(J773G_DCP + 0x44, 0x10);
    poke32(J773G_DCP + 0x48, 0x3000ac);
    region_add(J773G_DART_DCP, 0x4000);
    region_add(J773G_DART_DSP, 0x4000);
    poke32(J773G_DART_DCP + 0x200, 0x1e1);
    poke32(J773G_DART_DSP + 0x200, 0x1e1);
}

static void world_ext_j773g(int i)
{
    struct seg ext[4] = {
        {EXT_TEXT_PHYS, 0, EXT_TEXT_REMAP, EXT_TEXT_SIZE, 1},
        {ext_data(i), 0x10000, EXT_DATA_REMAP, EXT_DATA_SIZE, 0},
        {ext_oslog(i), 0x18000, ext_oslog(i), 0x4000, 0xa},
        {ext_heap(i), ~0ULL, EXT_HEAP_REMAP, EXT_HEAP_SIZE, 2},
    };
    char path[96];
    snprintf(path, sizeof(path), "/arm-io/dcpext%d", i);
    reg_add(path, ext_base[i], 0x88000);
    snprintf(path, sizeof(path), "/arm-io/dart-dcpext%d", i);
    reg_add(path, ext_dart[i], 0x20000);
    snprintf(path, sizeof(path), "/arm-io/dcpext%d/iop-dcpext%d-nub", i, i);
    prop_set(path, "segment-ranges", ext, sizeof(ext));
    prop_str(path, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown");
    prop_str(path, "uuid", J773G_UUID);
    prop_u32(path, "pre-loaded", 1);
    prop_u32(path, "running", 1);
    snprintf(path, sizeof(path), "/arm-io/dispext%d", i);
    prop_set(path, "clock-ids", ext_clk[i], sizeof(ext_clk[i]));
}

/* ---- the J773g device tree on 27.2, as t8132-dcp.dtsi and t8132-dcpext.dtsi describe it ---- */

enum { PH_J_DART_DCP = 1, PH_J_DART_DISP, PH_J_MBOX, PH_J_STATIC_CLK = 60 };

static int t8132_dart(void *dt, int parent, const char *name, u64 base, u32 ph)
{
    int n = OK(fdt_add_subnode(dt, parent, name));
    OK(fdt_setprop(dt, n, "compatible", T8132_DART, sizeof(T8132_DART)));
    reg2(dt, n, base, 0x4000);
    OK(fdt_setprop_u32(dt, n, "#iommu-cells", 1));
    fdt32_t range[4] = {cpu_to_fdt32(TB >> 32), 0, cpu_to_fdt32(0x10), 0};
    OK(fdt_setprop(dt, n, "apple,dma-range", range, sizeof(range)));
    OK(fdt_setprop_string(dt, n, "status", "disabled"));
    OK(fdt_setprop_u32(dt, n, "phandle", ph));
    return n;
}

static void build_dt_j773g(void *dt, const char *root, size_t root_len)
{
    OK(fdt_create_empty_tree(dt, DT_SIZE));
    OK(fdt_setprop(dt, 0, "compatible", root, root_len));
    cells(dt, 0);
    OK(fdt_add_subnode(dt, 0, "chosen"));
    int resv = OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    cells(dt, resv);
    OK(fdt_setprop_empty(dt, resv, "ranges"));

    int soc = OK(fdt_add_subnode(dt, 0, "soc"));
    cells(dt, soc);
    OK(fdt_setprop_empty(dt, soc, "ranges"));
    t8132_dart(dt, soc, "iommu@30a340000", J773G_DART_DCP, PH_J_DART_DCP);
    t8132_dart(dt, fdt_path_offset(dt, "/soc"), "iommu@30a300000", J773G_DART_DSP, PH_J_DART_DISP);
    int mbox = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "mailbox@30ae08000"));
    OK(fdt_setprop_string(dt, mbox, "compatible", "apple,asc-mailbox-v4"));
    OK(fdt_setprop_string(dt, mbox, "status", "disabled"));
    OK(fdt_setprop_u32(dt, mbox, "phandle", PH_J_MBOX));

    int dcp = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), "dcp@30ae00000"));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t8132-dcp"));
    reg2(dt, dcp, J773G_DCP, 0x4000);
    iommus(dt, dcp, PH_J_DART_DCP, 23);
    OK(fdt_setprop_u32(dt, dcp, "mboxes", PH_J_MBOX));
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    int pio = OK(fdt_add_subnode(dt, dcp, "piodma"));
    iommus(dt, pio, PH_J_DART_DISP, 16);

    int disp = OK(fdt_add_subnode(dt, 0, "display-subsystem"));
    OK(fdt_setprop(dt, disp, "compatible", J773G_DISP, sizeof(J773G_DISP)));
    iommus(dt, disp, PH_J_DART_DISP, 0);
    OK(fdt_setprop_string(dt, disp, "status", "disabled"));

    int aliases = OK(fdt_add_subnode(dt, 0, "aliases"));
    OK(fdt_setprop_string(dt, aliases, "dcp", "/soc/dcp@30ae00000"));
    OK(fdt_setprop_string(dt, aliases, "disp0", "/display-subsystem"));
    OK(fdt_setprop_string(dt, aliases, "disp0_piodma", "/soc/dcp@30ae00000/piodma"));
}

static void ext_path(char *buf, size_t len, int i)
{
    snprintf(buf, len, "/soc/dcp@%llx", (unsigned long long)ext_base[i]);
}

static void build_ext_j773g(void *dt, int i)
{
    char name[64], path[96];
    snprintf(name, sizeof(name), "iommu@%llx", (unsigned long long)ext_dart[i]);
    t8132_dart(dt, fdt_path_offset(dt, "/soc"), name, ext_dart[i], 100 + i);
    snprintf(name, sizeof(name), "dcp@%llx", (unsigned long long)ext_base[i]);
    int dcp = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/soc"), name));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t8132-dcpext"));
    fdt64_t reg[4] = {cpu_to_fdt64(ext_base[i]), cpu_to_fdt64(0x4000),
                      cpu_to_fdt64(i ? 0x288000000ULL : 0x284000000ULL), cpu_to_fdt64(0xc20000)};
    OK(fdt_setprop(dt, dcp, "reg", reg, sizeof(reg)));
    iommus(dt, dcp, 100 + i, 23);
    OK(fdt_setprop_u32(dt, dcp, "apple,dcp-index", i + 1));
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    fdt32_t mr[3];
    static const char *const parts[] = {"text", "data", "heap"};
    for (int j = 0; j < 3; j++) {
        snprintf(name, sizeof(name), "dcpext%d-%s", i, parts[j]);
        int region = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
        OK(fdt_setprop_string(dt, region, "compatible", "apple,asc-mem"));
        reg2(dt, region, 0, 0x4000);
        OK(fdt_setprop_empty(dt, region, "no-map"));
        OK(fdt_setprop_string(dt, region, "status", "disabled"));
        OK(fdt_setprop_u32(dt, region, "phandle", 110 + i * 3 + j));
        mr[j] = cpu_to_fdt32(110 + i * 3 + j);
    }
    ext_path(path, sizeof(path), i);
    dcp = fdt_path_offset(dt, path);
    OK(fdt_setprop(dt, dcp, "memory-region", mr, sizeof(mr)));
    static const char names[] = "text\0data\0heap";
    OK(fdt_setprop(dt, dcp, "memory-region-names", names, sizeof(names)));
}

/* The 26.6.2 J773g tree's static firmware clock for dispext0, on dcpext0 */
static void add_static_ext_clock(void *dt, u32 hz)
{
    int clk = OK(fdt_add_subnode(dt, 0, "clock-dcp-firmware-369"));
    OK(fdt_setprop_string(dt, clk, "compatible", "fixed-clock"));
    OK(fdt_setprop_u32(dt, clk, "#clock-cells", 0));
    OK(fdt_setprop_u32(dt, clk, "clock-frequency", hz));
    OK(fdt_setprop_u32(dt, clk, "phandle", PH_J_STATIC_CLK));
    char path[96];
    ext_path(path, sizeof(path), 0);
    OK(fdt_setprop_u32(dt, fdt_path_offset(dt, path), "clocks", PH_J_STATIC_CLK));
}

/* ---- checks ---- */

/* Every property of @path is the same in @in and @dt */
static bool props_unchanged(const void *in, const void *dt, const char *path)
{
    int a = fdt_path_offset(in, path), b = fdt_path_offset(dt, path), prop, n = 0, m = 0;
    if (a < 0 || b < 0)
        return false;
    fdt_for_each_property_offset(prop, in, a)
    {
        const char *name;
        int len, olen;
        const void *v = fdt_getprop_by_offset(in, prop, &name, &len);
        const void *ov = fdt_getprop(dt, b, name, &olen);
        if (!ov || olen != len || memcmp(v, ov, len))
            return false;
        n++;
    }
    fdt_for_each_property_offset(prop, dt, b) m++;
    return n == m;
}

static bool resv_at(const void *dt, const char *prefix, u64 base, u64 size)
{
    char name[64];
    snprintf(name, sizeof(name), "%s@%llx", prefix, (unsigned long long)base);
    int node = fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), name);
    u64 b, s;
    return node >= 0 && reg_of(dt, node, &b, &s) && b == base && s == size &&
           fdt_getprop(dt, node, "no-map", NULL);
}

static bool tuple_for(const void *dt, int region, int dev, u64 dva, u64 size)
{
    u64 got_size = 0;
    return iommu_dva(dt, region, dev, &got_size) == dva && got_size == size;
}

static void check_clock(const void *dt, int ext, int k, u32 id, u32 hz)
{
    int len = 0;
    const fdt32_t *clocks = fdt_getprop(dt, ext, "clocks", &len);
    int node = clocks && len > k * 4 ? fdt_node_offset_by_phandle(dt, fdt32_ld(clocks + k)) : -1;
    char want[40];
    snprintf(want, sizeof(want), "clock-dcp-firmware-%u", id);
    const char *name = node < 0 ? NULL : fdt_get_name(dt, node, NULL);
    const fdt32_t *rate = node < 0 ? NULL : fdt_getprop(dt, node, "clock-frequency", &len);
    const fdt32_t *cc = node < 0 ? NULL : fdt_getprop(dt, node, "#clock-cells", NULL);
    check(node >= 0 && fdt_parent_offset(dt, node) == 0 && name && !strcmp(name, want) &&
              !fdt_node_check_compatible(dt, node, "fixed-clock") && cc && !fdt32_ld(cc) && rate &&
              len == 4 && fdt32_ld(rate) == hz,
          "clock %d is /%s, a fixed-clock at %u Hz", k, want, hz);
}

static int clock_nodes(const void *dt, u32 id)
{
    int n = 0, node;
    char want[40];
    snprintf(want, sizeof(want), "clock-dcp-firmware-%u", id);
    fdt_for_each_subnode(node, dt, 0)
    {
        const char *name = fdt_get_name(dt, node, NULL);
        if (name && !strcmp(name, want))
            n++;
    }
    return n;
}

static void check_ext(const void *in, const void *dt, int i, bool want)
{
    char path[96];
    ext_path(path, sizeof(path), i);
    int ext = fdt_path_offset(dt, path), len = 0;
    const fdt32_t *mr = ext < 0 ? NULL : fdt_getprop(dt, ext, "memory-region", &len);
    int dart = fdt_node_offset_by_phandle(dt, 100 + i);
    char dart_path[64];
    snprintf(dart_path, sizeof(dart_path), "/soc/iommu@%llx", (unsigned long long)ext_dart[i]);

    if (!want) {
        check(props_unchanged(in, dt, path) && props_unchanged(in, dt, dart_path),
              "dcpext%d refused: its node and DART are unchanged", i);
        for (int j = 0; mr && len == 12 && j < 3; j++) {
            int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
            check(region >= 0 && status_is(dt, region, "disabled") &&
                      !fdt_getprop(dt, region, "iommu-addresses", NULL),
                  "dcpext%d refused: placeholder %d stays disabled", i, j);
        }
        check(clock_nodes(dt, ext_clk[i][0]) == clock_nodes(in, ext_clk[i][0]) &&
                  clock_nodes(dt, ext_clk[i][1]) == clock_nodes(in, ext_clk[i][1]),
              "dcpext%d refused: no clock node added for its pipe", i);
        return;
    }

    check(ext >= 0 && mr && len == 12 && status_is(dt, ext, "disabled") && dart >= 0 &&
              status_is(dt, dart, "disabled"),
          "dcpext%d and its DART stay disabled for the display gate", i);
    if (ext < 0 || !mr || len != 12)
        return;

    const u32 ver[3] = {27, 2, 0};
    const char *uuid = fdt_getprop(dt, ext, "apple,firmware-uuid", NULL);
    check(u32s_are(dt, ext, "apple,firmware-version", ver, 3) &&
              u32s_are(dt, ext, "apple,firmware-compat", ver, 3) && uuid &&
              !strcmp(uuid, J773G_UUID),
          "dcpext%d: firmware 27.2.0 and the pinned UUID", i);
    check(u32s_are(dt, ext, "apple,clock-ids", ext_clk[i], 2) &&
              fdt_stringlist_count(dt, ext, "clock-names") == 2 &&
              fdt_stringlist_search(dt, ext, "clock-names", "firmware-0") == 0 &&
              fdt_stringlist_search(dt, ext, "clock-names", "firmware-1") == 1,
          "dcpext%d: apple,clock-ids are /arm-io/dispext%d's, named firmware-0/1", i, i);
    check_clock(dt, ext, 0, ext_clk[i][0], EXT_HZ);
    check_clock(dt, ext, 1, ext_clk[i][1], 0);

    int text = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[0]));
    const char *tname = text < 0 ? NULL : fdt_get_name(dt, text, NULL);
    char want_text[48];
    snprintf(want_text, sizeof(want_text), "dcp-text@%llx", (unsigned long long)EXT_TEXT_PHYS);
    check(tname && !strcmp(tname, want_text) &&
              !fdt_node_check_compatible(dt, text, "apple,asc-mem") &&
              tuple_for(dt, text, ext, EXT_TEXT_REMAP, EXT_TEXT_SIZE),
          "dcpext%d: text is the shared dcp-text region, mapped for this processor", i);
    char ph_name[24];
    snprintf(ph_name, sizeof(ph_name), "dcpext%d-text", i);
    check(fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), ph_name) < 0,
          "dcpext%d: the text placeholder is gone", i);
    const u64 phys[3] = {0, ext_data(i), ext_heap(i)};
    const u64 dva[3] = {0, EXT_DATA_REMAP, EXT_HEAP_REMAP};
    const u64 size[3] = {0, EXT_DATA_SIZE, EXT_HEAP_SIZE};
    for (int j = 1; j < 3; j++) {
        int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
        u64 b = 0, s = 0;
        check(region >= 0 && status_is(dt, region, "okay") && reg_of(dt, region, &b, &s) &&
                  b == phys[j] && s == size[j] && tuple_for(dt, region, ext, dva[j], size[j]),
              "dcpext%d: %s is the ADT span, enabled, at its DART alias", i,
              j == 1 ? "data" : "heap");
    }
}

static void check_display(const void *in, const void *dt, bool want)
{
    int disp = alias_node(dt, "disp0");
    int dart = fdt_node_offset_by_phandle(dt, PH_J_DART_DISP);
    if (!want) {
        check(props_unchanged(in, dt, "/display-subsystem"),
              "display subsystem left as the device tree has it");
        check(dart >= 0 && props_unchanged(in, dt, "/soc/iommu@30a300000"),
              "display DART left as the device tree has it");
        return;
    }
    int fb = fdt_stringlist_search(dt, disp, "memory-region-names", "framebuffer");
    int region = fb >= 0 ? region_at(dt, disp, fb) : -1;
    u64 b = 0, s = 0;
    check(status_is(dt, disp, "okay") && dart >= 0 && status_is(dt, dart, "okay"),
          "display subsystem and its DART enabled");
    check(region >= 0 && !fdt_node_check_compatible(dt, region, "framebuffer") &&
              reg_of(dt, region, &b, &s) && b == J773G_FB && s == J773G_FB_SIZE,
          "the display's framebuffer memory-region is iBoot's boot framebuffer");
}

/* The internal DCP is not handed over: its node, DARTs and mailbox are as the DT had them */
static void check_main_untouched(const void *in, const void *dt)
{
    check(props_unchanged(in, dt, "/soc/dcp@30ae00000") &&
              props_unchanged(in, dt, "/soc/dcp@30ae00000/piodma") &&
              props_unchanged(in, dt, "/soc/iommu@30a340000") &&
              props_unchanged(in, dt, "/soc/mailbox@30ae08000"),
          "internal DCP, its piodma, DART and mailbox unchanged");
    check(mmio_writes == 0, "no MMIO write (dart-dcp not locked)");
}

static void check_oslogs(const void *dt, bool ext0, bool ext1)
{
    check(resv_at(dt, "dcp-oslog", dcp_segs[2].phys, ALIGN_UP((u64)dcp_segs[2].size, SZ_16K)),
          "dcp __OS_LOG reserved");
    check(resv_at(dt, "dcp-oslog", ext_oslog(0), 0x4000) == ext0, "dcpext0 __OS_LOG %sreserved",
          ext0 ? "" : "not ");
    check(resv_at(dt, "dcp-oslog", ext_oslog(1), 0x4000) == ext1, "dcpext1 __OS_LOG %sreserved",
          ext1 ? "" : "not ");
}

/* ---- scenarios ---- */

enum j773g_kind {
    J_PUBLISH, /* expectation per processor in the table */
    J_NOTHING, /* gate: no access at all, the DT byte for byte unchanged */
};

struct j773g_scenario {
    const char *name;
    enum j773g_kind kind;
    bool ext0, ext1, display;
    const char *what;
};

static const struct j773g_scenario j773g_scenarios[] = {
    {"publish", J_PUBLISH, true, true, true, "both external processors, the display enabled"},
    {"dcpext0-only", J_PUBLISH, true, false, true, "a tree with only dcpext0 described"},
    {"repeat", J_PUBLISH, true, true, true, "a second call changes nothing"},
    {"static-clock-nodes", J_PUBLISH, true, true, true,
     "the 26.6.2 tree's static clock node and clocks on dcpext0 are reused"},
    {"display-preenabled", J_PUBLISH, true, true, false,
     "the 26.6.2 tree enables the display: left alone, processors still published"},
    {"ext-uuid-mismatch", J_PUBLISH, true, false, true, "dcpext1 runs another image"},
    {"ext-not-running", J_PUBLISH, false, true, true, "dcpext0 is not running"},
    {"ext-enabled", J_PUBLISH, false, true, true, "dcpext0's node is enabled"},
    {"ext-dart-enabled", J_PUBLISH, false, true, true, "dcpext0's DART is enabled"},
    {"ext-wrong-compat", J_PUBLISH, false, true, true, "dcpext0 is not apple,t8132-dcpext"},
    {"ext-no-placeholders", J_PUBLISH, false, true, true, "dcpext0 has no memory-region"},
    {"ext-text-not-shared", J_PUBLISH, true, false, true, "dcpext1's __TEXT is its own"},
    {"ext-data-in-ram", J_PUBLISH, false, true, true, "dcpext0's __DATA is kernel RAM"},
    {"ext-clock-missing", J_PUBLISH, false, true, true, "/arm-io/dispext0 has no clock-ids"},
    {"ext-clock-zero", J_PUBLISH, false, true, true, "dispext0's first clock has no rate"},
    {"ext-clock-names", J_PUBLISH, false, true, true, "dcpext0 already has clock-names"},
    {"ext-clock-conflict", J_PUBLISH, false, true, true,
     "/clock-dcp-firmware-369 exists with another rate"},
    {"ext-all-refused", J_PUBLISH, false, false, false, "no processor passes: display untouched"},
    {"wrong-chip", J_NOTHING, false, false, false, "chip T6041 with a J773g tree"},
    {"j713", J_NOTHING, false, false, false, "the J713 (also apple,t8132) is not this board"},
    {"os-26-6-2", J_NOTHING, false, false, false, "26.6.2 firmware"},
    {"os-27-0", J_NOTHING, false, false, false, "27.0 firmware"},
    {"wrong-uuid", J_NOTHING, false, false, false, "the internal nub runs another image"},
    {"disp0-clocks-missing", J_NOTHING, false, false, false, "/arm-io/disp0 has no clock-ids"},
    {"no-display-dt", J_NOTHING, false, false, false, "no dcp alias"},
    {"dt-full", J_PUBLISH, false, false, false, "no room: refused without partial edits"},
};

int main(int argc, char **argv)
{
    if (argc == 2 && is(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(j773g_scenarios); i++)
            printf("%s\n", j773g_scenarios[i].name);
        return 0;
    }
    if (argc != 2)
        return 2;
    const struct j773g_scenario *sc = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(j773g_scenarios); i++)
        if (is(argv[1], j773g_scenarios[i].name))
            sc = &j773g_scenarios[i];
    if (!sc)
        return 2;
    const char *s = sc->name;

    world_j773g();
    static u8 dt[DT_SIZE], in[DT_SIZE];
    _Static_assert(sizeof(J713) < sizeof(J773G), "J713 root fits");
    if (is(s, "j713"))
        build_dt_j773g(dt, J713, sizeof(J713));
    else
        build_dt_j773g(dt, J773G, sizeof(J773G));
    int next = is(s, "dcpext0-only") ? 1 : 2;
    for (int i = 0; i < next; i++) {
        build_ext_j773g(dt, i);
        world_ext_j773g(i);
    }
    if (is(s, "dcpext0-only"))
        world_ext_j773g(1); /* the ADT has it; the tree does not describe it */

    char path[96];

    /* ADT side */
    if (is(s, "wrong-chip"))
        chip_id = T6041;
    if (is(s, "os-26-6-2"))
        os_firmware =
            (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
    if (is(s, "os-27-0"))
        os_firmware = (struct fw_version_info){V27_0, "27.0", {27, 0, 0}, 3, "mBoot-20457.1.29"};
    if (is(s, "wrong-uuid"))
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", OTHER_UUID);
    if (is(s, "ext-uuid-mismatch"))
        prop_str("/arm-io/dcpext1/iop-dcpext1-nub", "uuid", OTHER_UUID);
    if (is(s, "disp0-clocks-missing"))
        prop_drop("/arm-io/disp0", "clock-ids");
    if (is(s, "ext-not-running"))
        prop_u32("/arm-io/dcpext0/iop-dcpext0-nub", "running", 0);
    if (is(s, "ext-text-not-shared") || is(s, "ext-data-in-ram")) {
        int i = is(s, "ext-text-not-shared") ? 1 : 0;
        struct seg ext[4] = {
            {EXT_TEXT_PHYS, 0, EXT_TEXT_REMAP, EXT_TEXT_SIZE, 1},
            {ext_data(i), 0x10000, EXT_DATA_REMAP, EXT_DATA_SIZE, 0},
            {ext_oslog(i), 0x18000, ext_oslog(i), 0x4000, 0xa},
            {ext_heap(i), ~0ULL, EXT_HEAP_REMAP, EXT_HEAP_SIZE, 2},
        };
        if (i == 1)
            ext[0].phys = TB + 0x284000000ULL;
        else
            ext[1].phys = TB + 0x10000000ULL;
        snprintf(path, sizeof(path), "/arm-io/dcpext%d/iop-dcpext%d-nub", i, i);
        prop_set(path, "segment-ranges", ext, sizeof(ext));
    }
    if (is(s, "ext-clock-missing"))
        prop_drop("/arm-io/dispext0", "clock-ids");
    if (is(s, "ext-clock-zero")) {
        u32 ids[] = {380, 432};
        prop_set("/arm-io/dispext0", "clock-ids", ids, sizeof(ids));
    }
    if (is(s, "ext-all-refused")) {
        prop_u32("/arm-io/dcpext0/iop-dcpext0-nub", "running", 0);
        prop_u32("/arm-io/dcpext1/iop-dcpext1-nub", "running", 0);
    }

    /* device tree side */
    if (is(s, "static-clock-nodes"))
        add_static_ext_clock(dt, EXT_HZ);
    if (is(s, "ext-clock-conflict"))
        add_static_ext_clock(dt, 600000000);
#define EXT0_NODE fdt_path_offset(dt, "/soc/dcp@286e00000")
    if (is(s, "display-preenabled")) {
        OK(fdt_setprop_string(dt, alias_node(dt, "disp0"), "status", "okay"));
        OK(fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, PH_J_DART_DISP), "status",
                              "okay"));
        OK(fdt_setprop_string(dt, alias_node(dt, "dcp"), "status", "okay"));
    }
    if (is(s, "ext-enabled"))
        OK(fdt_setprop_string(dt, EXT0_NODE, "status", "okay"));
    if (is(s, "ext-dart-enabled"))
        OK(fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, 100), "status", "okay"));
    if (is(s, "ext-wrong-compat"))
        OK(fdt_setprop_string(dt, EXT0_NODE, "compatible", "apple,t6041-dcpext"));
    if (is(s, "ext-no-placeholders")) {
        OK(fdt_delprop(dt, EXT0_NODE, "memory-region"));
        OK(fdt_delprop(dt, EXT0_NODE, "memory-region-names"));
    }
    if (is(s, "ext-clock-names"))
        OK(fdt_setprop_string(dt, EXT0_NODE, "clock-names", "firmware-0"));
    if (is(s, "no-display-dt"))
        OK(fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcp"));
    if (is(s, "dt-full")) {
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, fdt_totalsize(dt) + 256));
    } else {
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, DT_SIZE));
    }
    memcpy(in, dt, fdt_totalsize(dt));

    printf("== %s: %s\n", s, sc->what);
    fflush(stdout);
    mmio_reads = mmio_writes = 0;
    int ret = dt_set_display_handoff(dt);
    fflush(stdout);
    check(ret == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO access faulted");

    if (sc->kind == J_NOTHING) {
        check(fdt_totalsize(dt) == fdt_totalsize(in) && !memcmp(in, dt, fdt_totalsize(in)),
              "nothing done: the device tree is byte for byte unchanged");
        check(mmio_reads == 0 && mmio_writes == 0, "nothing done: no MMIO access at all");
    } else {
        check_main_untouched(in, dt);
        if (is(s, "dt-full")) {
            for (int i = 0; i < 2; i++)
                check_ext(in, dt, i, false);
            check_display(in, dt, false);
        } else {
            check_oslogs(dt, true, true);
            check_ext(in, dt, 0, sc->ext0);
            if (!is(s, "dcpext0-only"))
                check_ext(in, dt, 1, sc->ext1);
            check_display(in, dt, sc->display);
        }
        if (is(s, "static-clock-nodes"))
            check(clock_nodes(dt, ext_clk[0][0]) == 1 &&
                      fdt_node_offset_by_phandle(dt, PH_J_STATIC_CLK) >= 0,
                  "the static clock node is reused, not duplicated");
        if (is(s, "ext-uuid-mismatch") || is(s, "ext-text-not-shared")) {
            int text = -1;
            char want[48];
            snprintf(want, sizeof(want), "dcp-text@%llx", (unsigned long long)EXT_TEXT_PHYS);
            text = fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), want);
            ext_path(path, sizeof(path), 1);
            u64 sz = 0;
            check(text >= 0 && iommu_dva(dt, text, fdt_path_offset(dt, path), &sz) == ~0ULL,
                  "the refused dcpext1 has no tuple on the shared text");
        }
        if (is(s, "repeat")) {
            static u8 first[DT_SIZE];
            memcpy(first, dt, fdt_totalsize(dt));
            int writes = mmio_writes;
            fflush(stdout);
            ret = dt_set_display_handoff(dt);
            fflush(stdout);
            check(ret == 0 && fdt_totalsize(first) == fdt_totalsize(dt) &&
                      !memcmp(first, dt, fdt_totalsize(first)),
                  "second call returns 0 and changes nothing in the device tree");
            check(mmio_writes == writes, "second call makes no MMIO write");
        }
    }

    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
