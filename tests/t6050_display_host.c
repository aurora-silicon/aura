/* SPDX-License-Identifier: MIT */
/*
 * Host integration test for the T6050 (J714c-shaped) display handoff of
 * src/kboot_display_handoff.c: the 27.3 beta image row, whose DCP nodes get 27.3.0 rather than the
 * OS firmware numbers, and a device tree that enables dcp and disp0 already, where only the
 * external engines are handed off and the PMC metadata node they point into is published or
 * reused.
 *
 * Reuses the fake ADT, MMIO and libfdt harness of t6041_display_host.c. This file models the T6050
 * helpers that t6050_unmodelled.h makes abort. Every address and value is synthetic; the image id
 * is the one the T6050 image table pins.
 *
 *   t6050-display-host list
 *   t6050-display-host <scenario>
 */
#include "xnuboot.h"

#define T6050_UNMODELLED_H /* modelled below */
#define main               t6041_main
#include "t6041_display_host.c"
#undef main

#define T50_UUID      "58CB3FFC-B2BA-352E-B20B-71A91B15FA9F"
#define T50_STATUS    "asahi,t6050-dcp-handoff"
#define T50_COPROC    0x512e00000ULL
#define T50_DART_DCP  0x512340000ULL
#define T50_DART_DISP 0x512300000ULL
#define T50_DISP      0x510200000ULL /* disp0 reg[0..3], 0x100000 apart */
#define T50_PMC       0x510f00000ULL /* disp0 reg[4] */
#define T50_PMC_SIZE  0x100000ULL
#define T50_EXT       0x522e00000ULL
#define T50_EXT_DCP   0x522340000ULL /* dart-dcpext0 */
#define T50_EXT_DISP  0x522300000ULL /* dart-dispext0 */
#define T50_PS_EXT    (PMGR_BASE + 0x300)
#define T50_L1        (TB + 0x2f0000000ULL) /* the five four-level roots, 0x100000 apart */
#define T50_VM_SIZE   0x1000000000ULL
#define T50_BW        (0x20000 + 2 * 0x4000) /* bandwidth agent 2: the panel's */
#define T50_BW_EXT    (0x20000 + 5 * 0x4000) /* bandwidth agent 5: dcpext0's */
#define T50_DCP_PATH  "/soc/dcp@512e00000"
#define T50_EXT_PATH  "/soc/dcp@522e00000"
#define T50_PMC_NAME  "pmc-metadata@510f00000"
#define T50_PMC_PATH  "/soc/" T50_PMC_NAME

enum {
    T50_PH_DART_DCP = 1,
    T50_PH_DART_DISP,
    T50_PH_MBOX,
    T50_PH_EXT_DCP,
    T50_PH_EXT_DISP,
    T50_PH_EXT_MBOX
};

/* ---- the T6050 helpers of src/kboot.c, src/adt.c and src/pmgr.c ---- */

struct boot_args cur_boot_args;

/* As src/kboot.c: an alias or an absolute path, each component matched by its whole name */
int kboot_fdt_path_exact(const void *fdt, const char *path)
{
    if (path && path[0] != '/')
        path = fdt_get_alias(fdt, path);
    if (!path || path[0] != '/')
        return -FDT_ERR_NOTFOUND;

    int node = 0;
    while (node >= 0 && *path) {
        while (*path == '/')
            path++;
        const char *end = strchr(path, '/');
        int len = end ? end - path : (int)strlen(path), child, nlen;
        if (!len)
            break;
        int found = -FDT_ERR_NOTFOUND;
        fdt_for_each_subnode(child, fdt, node)
        {
            const char *name = fdt_get_name(fdt, child, &nlen);
            if (name && nlen == len && !memcmp(name, path, len)) {
                found = child;
                break;
            }
        }
        node = found;
        path += len;
    }
    return node;
}

int adt_getprop_copy(const void *tree, int node, const char *name, void *out, size_t len)
{
    u32 plen = 0;
    const void *p = adt_getprop(tree, node, name, &plen);
    if (!p || plen != len)
        return -1;
    memcpy(out, p, len);
    return len;
}

int pmgr_adt_is_powered(const char *path)
{
    return adt_path_offset(adt, path) < 0 ? -1 : 1;
}

/* dcpext0 has one power word; nothing else is asked for */
int pmgr_adt_collect_psregs(const char *path, u64 *out, u8 *flags, int max)
{
    UNUSED(flags);
    if (strcmp(path, "/arm-io/dcpext0") || max < 1)
        return -1;
    out[0] = T50_PS_EXT;
    return 1;
}

/* ---- the J714c-shaped world: the panel's DCP and one external engine, both left running ---- */

/* A DART as iBoot leaves it: locked, `sid` translating through the four-level root `l1` */
static void t50_dart(const char *path, u64 base, u32 sid, u64 l1)
{
    char name[16];
    if (node_find(path) < 0) {
        reg_add(path, base, 0x4000);
        prop_u64(path, "vm-base", TB);
        prop_u64(path, "vm-size", T50_VM_SIZE);
        region_add(base, 0x4000);
        poke32(base + 0x200, 1);
    }
    snprintf(name, sizeof(name), "l1-tt-%u", sid);
    prop_pair(path, name, l1, 1);
    poke32(base + 0x1000 + 4 * sid, 0x9);
    poke32(base + 0x1400 + 4 * sid, ttbr_of(l1));
}

/* A bandwidth agent record: kind 1, the agent's index, its interrupt */
static void t50_agent(u8 *rec, u8 index, u32 birq)
{
    rec[0] = 1;
    rec[2] = index;
    memcpy(rec + 4, &birq, 4);
}

static void world_t6050(void)
{
    chip_id = T6050;
    os_firmware = (struct fw_version_info){V27_3B, "27.3b1", {27, 2, 98, 1}, 4, NULL};
    ram_base = TB + 0x800000;
    ram_size = 0x200000000ULL - 0x800000;
    cur_boot_args.phys_base = ram_base;
    cur_boot_args.mem_size = ram_size;
    cur_boot_args.video.base = fb_base = segs[SEG_VRAM].phys + 0x4000;
    cur_boot_args.video.stride = 0x1000;
    cur_boot_args.video.height = 0x40;
    prop_u64("/chosen", "dram-base", TB);
    prop_u64("/chosen", "dram-size", 0x300000000ULL);
    mcc_carveout_count = 0;

    const char *nub = "/arm-io/dcp/iop-dcp-nub";
    prop_set(nub, "segment-ranges", segs, sizeof(segs));
    prop_str(nub, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown;unknown");
    prop_str(nub, "uuid", T50_UUID);
    prop_u32(nub, "running", 1);
    reg_add("/vram", segs[SEG_VRAM].phys, segs[SEG_VRAM].size);

    reg_add("/arm-io/dcp", T50_COPROC, 0x6c000);
    reg_add("/arm-io/dcp", T50_COPROC + 0x100000, 0x4000);
    for (int i = 0; i < 4; i++)
        reg_add("/arm-io/disp0", T50_DISP + i * 0x100000ULL, 0x4000);
    reg_add("/arm-io/disp0", T50_PMC, T50_PMC_SIZE);
    region_add(T50_COPROC, 0x4000);
    poke32(T50_COPROC + 0x44, 0x10);
    t50_dart("/arm-io/dart-dcp", T50_DART_DCP, 23, T50_L1);
    t50_dart("/arm-io/dart-disp0", T50_DART_DISP, 0, T50_L1 + 0x100000);
    t50_dart("/arm-io/dart-disp0", T50_DART_DISP, 16, T50_L1 + 0x200000);

    /* The PMGR's bandwidth agents, and the interrupt each display block names */
    u8 agents[2 * 24] = {0};
    u32 fn[3] = {77, 0, 0x30};
    t50_agent(agents, 2, 0x30);
    t50_agent(agents + 24, 5, 0x41);
    prop_u32("/arm-io/pmgr", "AAPL,phandle", 77);
    prop_set("/arm-io/pmgr", "bwr-agent", agents, sizeof(agents));
    prop_set("/arm-io/disp0", "function-bw_req_interrupt0", fn, sizeof(fn));
    fn[2] = 0x41;
    prop_set("/arm-io/dispext0", "function-bw_req_interrupt0", fn, sizeof(fn));

    u32 ids[] = {300, 301}, hz[64] = {0};
    hz[300 - 256] = 600000000;
    hz[301 - 256] = 24000000;
    prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    prop_set("/arm-io", "clock-frequencies", hz, sizeof(hz));
    prop_u32("/backlight", "user-accessible-max-nits", 500u << 16);

    /* dcpext0, running the panel's image */
    struct seg ext[2] = {
        {TB + 0x281200000ULL, 0, TB + 0x51000000, 0x8000, 0},
        {TB + 0x28030000ULL, 0, TB + 0x28030000ULL, 0x4000, 0xa},
    };
    nub = "/arm-io/dcpext0/iop-dcpext0-nub";
    prop_set(nub, "segment-ranges", ext, sizeof(ext));
    prop_str(nub, "segment-names", "__DATA;__OS_LOG");
    prop_str(nub, "uuid", T50_UUID);
    prop_u32(nub, "running", 1);
    reg_add("/arm-io/dcpext0", T50_EXT, 0x6c000);
    region_add(T50_EXT, 0x4000);
    poke32(T50_EXT + 0x44, 0x10);
    t50_dart("/arm-io/dart-dcpext0", T50_EXT_DCP, 23, T50_L1 + 0x300000);
    t50_dart("/arm-io/dart-dispext0", T50_EXT_DISP, 16, T50_L1 + 0x400000);
    region_add(PMGR_BASE, 0x1000);
    poke32(T50_PS_EXT, 0xff);
}

/* ---- the device tree: the display block and dcpext0, as a kernel DT describes them ---- */

static int t50_mbox(void *dt, int soc, const char *name, u64 base, u32 ph)
{
    int n = OK(fdt_add_subnode(dt, soc, name));
    OK(fdt_setprop_string(dt, n, "compatible", "apple,asc-mailbox-v4"));
    reg2(dt, n, base, 0x4000);
    OK(fdt_setprop_string(dt, n, "status", "disabled"));
    OK(fdt_setprop_u32(dt, n, "phandle", ph));
    return n;
}

static void t50_pmc(void *dt, const char *bus, u64 size)
{
    int n = OK(fdt_add_subnode(dt, fdt_path_offset(dt, bus), T50_PMC_NAME));
    reg2(dt, n, T50_PMC, size);
    OK(fdt_setprop_u32(dt, n, "#apple,bw-scratch-cells", 3));
}

static void build_t6050(void *dt, const char *dcp_status, const char *disp_status)
{
    static const char root[] = "apple,j714c\0apple,t6050\0apple,arm-platform";
    static const char names[] = "coproc\0iop-vbar\0disp-0\0disp-1\0disp-2\0disp-3\0disp-4";

    OK(fdt_create_empty_tree(dt, DT_SIZE));
    OK(fdt_setprop(dt, 0, "compatible", root, sizeof(root)));
    cells(dt, 0);
    OK(fdt_add_subnode(dt, 0, "chosen"));
    int resv = OK(fdt_add_subnode(dt, 0, "reserved-memory"));
    cells(dt, resv);
    OK(fdt_setprop_empty(dt, resv, "ranges"));

    int soc = OK(fdt_add_subnode(dt, 0, "soc"));
    cells(dt, soc);
    OK(fdt_setprop_empty(dt, soc, "ranges"));
    dart(dt, soc, "iommu@512340000", T50_DART_DCP, T50_PH_DART_DCP);
    dart(dt, soc, "iommu@512300000", T50_DART_DISP, T50_PH_DART_DISP);
    dart(dt, soc, "iommu@522340000", T50_EXT_DCP, T50_PH_EXT_DCP);
    dart(dt, soc, "iommu@522300000", T50_EXT_DISP, T50_PH_EXT_DISP);
    t50_mbox(dt, soc, "mbox@512e08000", T50_COPROC + 0x8000, T50_PH_MBOX);
    t50_mbox(dt, soc, "mbox@522e08000", T50_EXT + 0x8000, T50_PH_EXT_MBOX);

    fdt64_t reg[14];
    for (int i = 0; i < 7; i++) {
        u64 base = i == 0 ? T50_COPROC : i == 1 ? T50_COPROC + 0x100000 : T50_DISP;
        base = i == 6 ? T50_PMC : i > 1 ? base + (i - 2) * 0x100000ULL : base;
        reg[2 * i] = cpu_to_fdt64(base);
        reg[2 * i + 1] = cpu_to_fdt64(i == 0 ? 0x6c000 : i == 6 ? T50_PMC_SIZE : 0x4000);
    }
    int dcp = OK(fdt_add_subnode(dt, soc, "dcp@512e00000"));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6050-dcp"));
    OK(fdt_setprop(dt, dcp, "reg", reg, sizeof(reg)));
    OK(fdt_setprop(dt, dcp, "reg-names", names, sizeof(names)));
    cells(dt, dcp);
    iommus(dt, dcp, T50_PH_DART_DCP, 23);
    OK(fdt_setprop_u32(dt, dcp, "mboxes", T50_PH_MBOX));
    OK(fdt_setprop_string(dt, dcp, "status", dcp_status));
    OK(fdt_add_subnode(dt, dcp, "panel"));
    int pio = OK(fdt_add_subnode(dt, dcp, "piodma"));
    iommus(dt, pio, T50_PH_DART_DISP, 16);

    int ext = OK(fdt_add_subnode(dt, soc, "dcp@522e00000"));
    OK(fdt_setprop_string(dt, ext, "compatible", "apple,t6050-dcpext"));
    reg2(dt, ext, T50_EXT, 0x6c000);
    iommus(dt, ext, T50_PH_EXT_DCP, 23);
    OK(fdt_setprop_u32(dt, ext, "mboxes", T50_PH_EXT_MBOX));
    OK(fdt_setprop_string(dt, ext, "apple,connector-type", "USB-C"));
    OK(fdt_setprop_string(dt, ext, "status", "disabled"));
    pio = OK(fdt_add_subnode(dt, ext, "piodma"));
    iommus(dt, pio, T50_PH_EXT_DISP, 16);

    int disp = OK(fdt_add_subnode(dt, 0, "display-subsystem"));
    OK(fdt_setprop_string(dt, disp, "compatible", "apple,display-subsystem"));
    iommus(dt, disp, T50_PH_DART_DISP, 0);
    OK(fdt_setprop_string(dt, disp, "status", disp_status));

    int aliases = OK(fdt_add_subnode(dt, 0, "aliases"));
    OK(fdt_setprop_string(dt, aliases, "dcp", T50_DCP_PATH));
    OK(fdt_setprop_string(dt, aliases, "disp0", "/display-subsystem"));
}

/* ---- checks ---- */

static const u32 t50_ver[3] = {27, 3, 0};

static const char *t50_status(const void *dt)
{
    return fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), T50_STATUS, NULL);
}

static int t50_pmc_count(const void *dt)
{
    int count = 0;
    for (int node = 0; node >= 0; node = fdt_next_node(dt, node, NULL))
        count += !strncmp(fdt_get_name(dt, node, NULL), "pmc-metadata", 12);
    return count;
}

/* The one PMC metadata node, at `path`: reg, cells and a phandle. Returns the phandle */
static u32 t50_pmc_check(const void *dt, const char *path)
{
    int node = fdt_path_offset(dt, path);
    u64 base = 0, size = 0;
    const u32 three = 3;
    u32 ph = node < 0 ? 0 : fdt_get_phandle(dt, node);
    check(node >= 0 && t50_pmc_count(dt) == 1, "one PMC metadata node, at %s", path);
    check(node >= 0 && reg_of(dt, node, &base, &size) && base == T50_PMC && size == T50_PMC_SIZE &&
              u32s_are(dt, node, "#apple,bw-scratch-cells", &three, 1) && ph,
          "it has reg <pmc size>, #apple,bw-scratch-cells = <3> and a phandle");
    return ph;
}

/* dcpext0 handed off against the PMC node `ph`, or left exactly as the device tree had it */
static void t50_ext_check(const void *dt, bool want, u32 ph)
{
    int ext = fdt_path_offset(dt, T50_EXT_PATH);
    if (!want) {
        check(ext >= 0 && status_is(dt, ext, "disabled") &&
                  !fdt_getprop(dt, ext, "apple,firmware-version", NULL) &&
                  !fdt_getprop(dt, ext, "apple,bw-scratch", NULL) &&
                  status_is(dt, fdt_node_offset_by_phandle(dt, T50_PH_EXT_DCP), "disabled"),
              "dcpext0 is not handed off");
        return;
    }
    const u32 scratch[4] = {ph, 0, 5, T50_BW_EXT};
    const u32 scanout[4] = {T50_PH_DART_DISP, 0, T50_PH_EXT_DISP, 0};
    const char *uuid = ext < 0 ? NULL : fdt_getprop(dt, ext, "apple,firmware-uuid", NULL);
    check(ext >= 0 && status_is(dt, ext, "okay") &&
              status_is(dt, fdt_node_offset_by_phandle(dt, T50_PH_EXT_MBOX), "okay") &&
              status_is(dt, fdt_node_offset_by_phandle(dt, T50_PH_EXT_DCP), "okay") &&
              status_is(dt, fdt_node_offset_by_phandle(dt, T50_PH_EXT_DISP), "okay"),
          "dcpext0, its mailbox and both of its DARTs are enabled");
    check(ext >= 0 && u32s_are(dt, ext, "apple,firmware-version", t50_ver, 3) &&
              u32s_are(dt, ext, "apple,firmware-compat", t50_ver, 3) && uuid &&
              !strcmp(uuid, T50_UUID),
          "dcpext0 apple,firmware-version and -compat are <27 3 0>, with the image id");
    check(ext >= 0 && ph && u32s_are(dt, ext, "apple,bw-scratch", scratch, 4),
          "dcpext0 apple,bw-scratch points at the PMC metadata node, agent 5");
    check(u32s_are(dt, alias_node(dt, "disp0"), "iommus", scanout, 4),
          "the display subsystem scans out through dart-dispext0 too");
}

/* Nothing but /chosen and the __OS_LOG reservations, which are their own transaction, differs */
static void t50_unchanged_check(const void *in, const void *dt)
{
    static u8 copy[DT_SIZE];
    const char *const extra[] = {"/reserved-memory/dcp-oslog@", NULL};
    memcpy(copy, dt, fdt_totalsize(dt));
    fdt_delprop(copy, fdt_path_offset(copy, "/chosen"), T50_STATUS);
    check(dt_diff(in, copy, extra) == 0,
          "the device tree is unchanged apart from /chosen and the __OS_LOG reservations");
    check(t50_pmc_count(dt) == t50_pmc_count(in), "no PMC metadata node published");
    t50_ext_check(dt, false, 0);
}

/* ---- scenarios ---- */

static const struct {
    const char *name, *what;
} t50_scenarios[] = {
    {"t6050-27.3", "the 27.3 beta image: the DCP nodes get 27.3.0, not the OS firmware numbers"},
    {"t6050-adopted", "dcp and disp0 enabled already: PMC metadata published, dcpext0 handed off"},
    {"t6050-adopted-existing-pmc", "a matching PMC metadata node is reused and gets a phandle"},
    {"t6050-adopted-pmc-other-bus", "a matching PMC metadata node on another bus is reused"},
    {"t6050-adopted-pmc-mismatch", "a PMC metadata node of another size: nothing handed off"},
    {"t6050-adopted-no-status",
     "dcp and disp0 without a status are enabled: handed off as adopted"},
    {"t6050-adopted-dcp-only", "dcp enabled, disp0 disabled: nothing handed off"},
    {"t6050-adopted-reserved", "dcp status \"reserved\": nothing handed off"},
    {"t6050-adopted-wrong-compat", "an enabled dcp of another SoC: nothing handed off"},
    {"t6050-adopted-dangling-alias", "the dcp alias names no node: nothing handed off"},
    {"t6050-adopted-mapped-bus", "the dcp's bus translates addresses: nothing handed off"},
};

int main(int argc, char **argv)
{
    if (argc == 2 && is(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(t50_scenarios); i++)
            printf("%s\n", t50_scenarios[i].name);
        return 0;
    }
    const char *s = NULL;
    for (size_t i = 0; argc == 2 && i < ARRAY_SIZE(t50_scenarios); i++)
        if (is(argv[1], t50_scenarios[i].name))
            s = t50_scenarios[i].name;
    if (!s) {
        fprintf(stderr, "usage: %s list | <scenario>\n", argv[0]);
        return 2;
    }

    static u8 dt[DT_SIZE], in[DT_SIZE];
    bool internal = is(s, "t6050-27.3");
    world_t6050();
    build_t6050(dt, internal ? "disabled" : "okay",
                internal || is(s, "t6050-adopted-dcp-only") ? "disabled" : "okay");
    if (is(s, "t6050-adopted-existing-pmc"))
        t50_pmc(dt, "/soc", T50_PMC_SIZE);
    if (is(s, "t6050-adopted-pmc-mismatch"))
        t50_pmc(dt, "/soc", T50_PMC_SIZE / 2);
    if (is(s, "t6050-adopted-pmc-other-bus")) {
        int bus = OK(fdt_add_subnode(dt, 0, "bus@200000000"));
        cells(dt, bus);
        OK(fdt_setprop_empty(dt, bus, "ranges"));
        t50_pmc(dt, "/bus@200000000", T50_PMC_SIZE);
    }
    if (is(s, "t6050-adopted-no-status")) {
        OK(fdt_delprop(dt, fdt_path_offset(dt, T50_DCP_PATH), "status"));
        OK(fdt_delprop(dt, fdt_path_offset(dt, "/display-subsystem"), "status"));
    }
    if (is(s, "t6050-adopted-reserved"))
        OK(fdt_setprop_string(dt, fdt_path_offset(dt, T50_DCP_PATH), "status", "reserved"));
    if (is(s, "t6050-adopted-wrong-compat"))
        OK(fdt_setprop_string(dt, fdt_path_offset(dt, T50_DCP_PATH), "compatible",
                              "apple,t6020-dcp"));
    if (is(s, "t6050-adopted-dangling-alias"))
        OK(fdt_setprop_string(dt, fdt_path_offset(dt, "/aliases"), "dcp", "/soc/dcp@0"));
    if (is(s, "t6050-adopted-mapped-bus")) {
        const fdt32_t ranges[6] = {0, 0, 0, cpu_to_fdt32(0x10), 0, cpu_to_fdt32(0x10000000)};
        OK(fdt_setprop(dt, fdt_path_offset(dt, "/soc"), "ranges", ranges, sizeof(ranges)));
    }
    OK(fdt_pack(dt));
    OK(fdt_open_into(dt, dt, DT_SIZE));

    memcpy(in, dt, fdt_totalsize(dt));
    mmio_reads = mmio_writes = 0;
    check(dt_set_display_handoff(dt) == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0 && mmio_writes == 0, "no MMIO fault and no MMIO write");

    const char *status = t50_status(dt);
    int dcp = fdt_path_offset(dt, T50_DCP_PATH);
    if (internal) {
        check(status && !strcmp(status, "published"), "/chosen/" T50_STATUS " = \"published\"");
        check(status_is(dt, dcp, "okay") && status_is(dt, alias_node(dt, "disp0"), "okay"),
              "dcp and disp0 are enabled");
        check(u32s_are(dt, dcp, "apple,firmware-version", t50_ver, 3) &&
                  u32s_are(dt, dcp, "apple,firmware-compat", t50_ver, 3),
              "dcp apple,firmware-version and -compat are exactly <27 3 0>");
        const u32 scratch[4] = {t50_pmc_check(dt, T50_PMC_PATH), 0, 5, T50_BW};
        check(u32s_are(dt, dcp, "apple,bw-scratch", scratch, 4),
              "dcp apple,bw-scratch points at the PMC metadata node, agent 2");
        t50_ext_check(dt, true, scratch[0]);
    } else if (is(s, "t6050-adopted") || is(s, "t6050-adopted-existing-pmc") ||
               is(s, "t6050-adopted-pmc-other-bus") || is(s, "t6050-adopted-no-status")) {
        check(status && !strcmp(status, "refused"), "/chosen/" T50_STATUS " = \"refused\"");
        check(!fdt_getprop(dt, dcp, "apple,firmware-version", NULL) &&
                  !fdt_getprop(dt, dcp, "apple,bw-scratch", NULL) &&
                  !fdt_getprop(dt, dcp, "memory-region", NULL),
              "the internal dcp gets nothing: the kernel adopts it");
        t50_ext_check(dt, true,
                      t50_pmc_check(dt, is(s, "t6050-adopted-pmc-other-bus")
                                            ? "/bus@200000000/" T50_PMC_NAME
                                            : T50_PMC_PATH));
    } else {
        check(status && !strcmp(status, "refused"), "/chosen/" T50_STATUS " = \"refused\"");
        t50_unchanged_check(in, dt);
    }
    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
