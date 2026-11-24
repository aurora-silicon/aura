# SPDX-License-Identifier: MIT
"""T6050 behaviour selected by tree content: the internal display branch and the combo PHY ports.

The functions under test are extracted from src/ and built against the in-tree libfdt; the
device trees are built here. Nothing touches hardware.
"""
from fdt_harness import PRELUDE, REPO, definition, run


def test_t6050_display_selected_by_tree_shape(tmp_path):
    source = (REPO / "src/kboot_display_handoff.c").read_text()
    selector = definition(source, "int dt_set_display_handoff(void *dt)")
    # The tree shape alone selects the prewired branch: no chip or board test beside it.
    assert "else if (t6050_prewired(dt))" in selector and "apple,j716c" not in source
    code = PRELUDE + r'''
#define T6050_TEXT
#define ADT_DCP 0x48ae00000UL
static bool adt_has_dcp = true;
static int t6050_adt_reg(const char *path, int idx, u64 *addr, u64 *size)
{
    assert(!strcmp(path, "/arm-io/dcp") && !idx && !size);
    if (!adt_has_dcp)
        return -1;
    *addr = ADT_DCP;
    return 0;
}
''' + definition(source, "static T6050_TEXT int t6050_node_at(") + \
        definition(source, "static T6050_TEXT bool t6050_prewired(") + r'''
enum { CLOCKS = 1, IDS = 2, SCRATCH = 4, ALIASES = 8 };
/* A T6050 tree: its dcp node at @at with the properties @props names */
static int tree_with(u64 at, int props)
{
    fresh();
    node_at("/", "soc");
    char name[32];
    snprintf(name, sizeof(name), "dcp@%lx", (unsigned long)at);
    int dcp = node_at("/soc", name);
    static const char compat[] = "apple,t6050-dcp\0apple,dcp";
    assert(!fdt_setprop(tree, dcp, "compatible", compat, sizeof(compat)));
    reg(dcp, at, 0x4000);
    assert(!fdt_setprop_string(tree, dcp, "status", "disabled"));
    if (props & CLOCKS)
        assert(!fdt_setprop_u32(tree, dcp, "clocks", 1));
    if (props & IDS)
        assert(!fdt_setprop_u32(tree, dcp, "apple,clock-ids", 361));
    if (props & SCRATCH)
        assert(!fdt_setprop_u32(tree, dcp, "apple,bw-scratch", 2));
    if (props & ALIASES) {
        int aliases = node_at("/", "aliases");
        assert(!fdt_setprop_string(tree, aliases, "dcp", "/soc/dcp@48ae00000"));
        assert(!fdt_setprop_string(tree, aliases, "disp0", "/soc/display"));
    }
    return fdt_path_offset(tree, "/soc/dcp@48ae00000") >= 0 && at == ADT_DCP
               ? fdt_path_offset(tree, "/soc/dcp@48ae00000")
               : -1;
}
int main(void)
{
    int cases = 0;
    /* J716c: described by address, clocks, clock ids and scratch prewired. */
    int dcp = tree_with(ADT_DCP, CLOCKS | IDS | SCRATCH);
    assert(dcp >= 0 && t6050_prewired(tree)); cases++;
    /* The same with the aliases too: still that shape, but the aliased path is chosen first. */
    dcp = tree_with(ADT_DCP, CLOCKS | IDS | SCRATCH | ALIASES);
    assert(dcp >= 0 && t6050_prewired(tree)); cases++;
    /* J714s: the aliased tree, nothing prewired. */
    tree_with(ADT_DCP, ALIASES);
    assert(!t6050_prewired(tree)); cases++;
    /* K116s: aliased, with clock ids but no clocks or scratch. */
    tree_with(ADT_DCP, ALIASES | IDS);
    assert(!t6050_prewired(tree)); cases++;
    /* J714c: no aliases, nothing prewired. */
    tree_with(ADT_DCP, 0);
    assert(!t6050_prewired(tree)); cases++;
    /* Any one of the three missing. */
    tree_with(ADT_DCP, IDS | SCRATCH);
    assert(!t6050_prewired(tree)); cases++;
    tree_with(ADT_DCP, CLOCKS | SCRATCH);
    assert(!t6050_prewired(tree)); cases++;
    tree_with(ADT_DCP, CLOCKS | IDS);
    assert(!t6050_prewired(tree)); cases++;
    /* Prewired, but not at the ADT's dcp. */
    tree_with(ADT_DCP + 0x100000, CLOCKS | IDS | SCRATCH);
    assert(!t6050_prewired(tree)); cases++;
    /* No dcp in the ADT (the Studio). */
    tree_with(ADT_DCP, CLOCKS | IDS | SCRATCH);
    adt_has_dcp = false;
    assert(!t6050_prewired(tree)); cases++;
    printf("PASS display shape %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "display-shape", code, "PASS display shape 10 cases")


def test_t6050_combo_phy_ports_selected_by_tree(tmp_path):
    source = (REPO / "src/kboot_atc.c").read_text()
    setup = definition(source, "int kboot_setup_atc(void *dt)")
    gate = setup[:setup.index("t6050_setup_atc(dt);")]
    # The combo PHY node alone selects the T6050 path: no chip or board test before it.
    assert 'fdt_node_offset_by_compatible(dt, -1, "apple,t6050-combo-phy") >= 0' in gate
    assert "chip_id" not in gate and "apple,j716c" not in source
    code = PRELUDE + r'''
#define T6050_ATC_INIT
static u32 exported[8];
static int nexported;
static void t6050_clear_atc(void *t, int node) { (void)t; (void)node; }
static int t6050_export_atc(void *t, int node, u32 port)
{
    (void)t; (void)node;
    exported[nexported++] = port;
    return 0;
}
''' + definition(source, "static T6050_ATC_INIT void t6050_setup_atc(void *dt)") + r'''
enum { RESERVED = 1, NO_INDEX = 2 };
static void phy(const char *name, u32 port, int flags)
{
    int n = node_at("/", name);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,t6050-combo-phy"));
    if (!(flags & NO_INDEX))
        assert(!fdt_setprop_u32(tree, n, "apple,port-index", port));
    if (flags & RESERVED)
        assert(!fdt_setprop_empty(tree, n, "apple,guest-port-reserved"));
}
static void expect(const u32 *ports, int n)
{
    nexported = 0;
    t6050_setup_atc(tree);
    assert(nexported == n);
    /* In any order: each expected port once */
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < n; j++)
            seen += exported[j] == ports[i];
        assert(seen == 1);
    }
}
int main(void)
{
    /* J714s and the other T6050 trees: ports 1 to 3. */
    fresh();
    phy("phy@2ab000000", 1, RESERVED);
    phy("phy@383000000", 2, RESERVED);
    phy("phy@38b000000", 3, RESERVED);
    expect((const u32[]){1, 2, 3}, 3);
    /* J716c: its tree also hands port 0 to the OS. */
    phy("phy@2a3000000", 0, RESERVED);
    expect((const u32[]){1, 2, 3, 0}, 4);
    /* Port 0 the tree does not hand over keeps aura's debug link. */
    fresh();
    phy("phy@2a3000000", 0, 0);
    phy("phy@2ab000000", 1, RESERVED);
    expect((const u32[]){1}, 1);
    /* No port index, or no such port: left alone. */
    fresh();
    phy("phy@1", 0, RESERVED | NO_INDEX);
    phy("phy@2", 6, RESERVED);
    expect(NULL, 0);
    puts("PASS combo phy ports 4 cases");
    return 0;
}
'''
    run(tmp_path, "combo-ports", code, "PASS combo phy ports 4 cases")


def test_t6050_register_windows_map_one_to_one(tmp_path):
    source = (REPO / "src/kboot_display_handoff.c").read_text()
    code = PRELUDE + r'''
#define T6050_INIT
''' + definition(source, "static T6050_INIT bool t6050_identity_reg(") + r'''
static void cells(int node, u32 a, u32 s)
{
    assert(!fdt_setprop_u32(tree, node, "#address-cells", a));
    assert(!fdt_setprop_u32(tree, node, "#size-cells", s));
}
static void ranges(int node, const u64 *e, int n)
{
    fdt32_t r[24];
    for (int i = 0; i < 3 * n; i++) {
        r[2 * i] = cpu_to_fdt32(e[i] >> 32);
        r[2 * i + 1] = cpu_to_fdt32((u32)e[i]);
    }
    assert(!fdt_setprop(tree, node, "ranges", r, 8 * 3 * n));
}
/* /soc (with @n ranges entries) / dcp, reg 0x48ae00000+0x4000 */
static int build(const u64 *e, int n)
{
    fresh();
    cells(0, 2, 2);
    int soc = node_at("/", "soc");
    cells(soc, 2, 2);
    if (n < 0)
        assert(!fdt_setprop_empty(tree, soc, "ranges"));
    else
        ranges(soc, e, n);
    int dcp = node_at("/soc", "dcp");
    reg(dcp, 0x48ae00000UL, 0x4000);
    return fdt_path_offset(tree, "/soc/dcp");
}
int main(void)
{
    int cases = 0;
    /* Empty ranges, or one identity entry holding the window (the J716c's soc). */
    assert(t6050_identity_reg(tree, build(NULL, -1))); cases++;
    const u64 j716c[] = {0x200000000UL, 0x200000000UL, 0x400000000UL};
    assert(t6050_identity_reg(tree, build(j716c, 1))); cases++;
    /* An identity entry that ends inside the window, or does not reach it. */
    const u64 tail[] = {0x200000000UL, 0x200000000UL, 0x28ae02000UL};
    assert(!t6050_identity_reg(tree, build(tail, 1))); cases++;
    const u64 short_[] = {0x200000000UL, 0x200000000UL, 0x200000000UL};
    assert(!t6050_identity_reg(tree, build(short_, 1))); cases++;
    /* A translating entry first: it is the one used, even with an identity one after it. */
    const u64 first[] = {0x480000000UL, 0x500000000UL, 0x40000000UL,
                         0x200000000UL, 0x200000000UL, 0x400000000UL};
    assert(!t6050_identity_reg(tree, build(first, 2))); cases++;
    /* The identity entry first, an unrelated one after: accepted. */
    const u64 after[] = {0x200000000UL, 0x200000000UL, 0x400000000UL,
                         0x700000000UL, 0x800000000UL, 0x100000000UL};
    assert(t6050_identity_reg(tree, build(after, 2))); cases++;
    /* No ranges at all on the bus: not mapped. */
    int dcp = build(NULL, -1);
    assert(!fdt_delprop(tree, fdt_parent_offset(tree, dcp), "ranges"));
    assert(!t6050_identity_reg(tree, fdt_path_offset(tree, "/soc/dcp"))); cases++;
    /* Directly under the root: the root's own cells are checked. */
    fresh();
    cells(0, 2, 1);
    reg(node_at("/", "pmc"), 0x286700000UL, 0x114000);
    assert(!t6050_identity_reg(tree, fdt_path_offset(tree, "/pmc"))); cases++;
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    assert(t6050_identity_reg(tree, fdt_path_offset(tree, "/pmc"))); cases++;
    /* An empty ranges under a one-cell root: the 64-bit window would be cut. */
    dcp = build(NULL, -1);
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 1));
    assert(!t6050_identity_reg(tree, fdt_path_offset(tree, "/soc/dcp"))); cases++;
    /* A wrapping entry, even one listed after the identity entry. */
    const u64 wrap[] = {0x200000000UL, 0x200000000UL, 0x400000000UL,
                        0xfffffffff0000000UL, 0xfffffffff0000000UL, 0x20000000UL};
    assert(!t6050_identity_reg(tree, build(wrap, 2))); cases++;
    printf("PASS identity reg %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "identity-reg", code, "PASS identity reg 11 cases")


def test_t6050_external_scanout_darts_resolved(tmp_path):
    source = (REPO / "src/kboot_display_handoff.c").read_text()
    header = (REPO / "src/adt.h").read_text()
    layout_start = header.index("struct adt_segment_ranges {")
    layout = header[layout_start:header.index("} PACKED;", layout_start) + len("} PACKED;")]
    code = PRELUDE + r'''
#define T6050_TEXT
#define T6050_INIT
#define T6050_ENGINES 8
#define PACKED __attribute__((packed))
#define SEG_FLAGS_OSLOG 0xa
#define SZ_16K 0x4000UL
#define DART_PAGE 0x4000UL
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define ALIGN_UP(x, a) ALIGN_DOWN((x) + (a) - 1, a)
#define dlog(fmt, ...) printf(fmt, ##__VA_ARGS__)
''' + layout + r'''
static struct { u64 phys_base, mem_size; } cur_boot_args = {0x10000000000UL, 0x200000000UL};
static struct { const char *uuid; u32 ext_aura; } t6050;
static void *adt;
#define DART_DCP 0x48a340000UL
#define DART_DISP0 0x48a300000UL
#define EXT0 0x2b2e00000UL
#define DART_DISPEXT0 0x2b2300000UL
#define UUID_A "C51FB0C8-C45D-39B0-85BC-520E600FC950"
#define UUID_B "64863924-B56B-3E32-87FE-038677F52709"
static struct adt_segment_ranges ext0_segs[2];
static u32 ext0_nsegs;
static u32 ext0_running;
static const char *ext0_uuid;
static u64 pt23[2], l1_23[2], pt16[2], l1_16[2];
static void reset_adt(void)
{
    t6050.uuid = UUID_A;
    ext0_uuid = UUID_A;
    ext0_running = 1;
    ext0_nsegs = 2;
    ext0_segs[0] = (struct adt_segment_ranges){0x10200000000UL, 0, 0x10200000000UL, 0x4000, 0};
    ext0_segs[1] = (struct adt_segment_ranges){0x10004500000UL, 0, 0x10004500000UL, 0x4000, SEG_FLAGS_OSLOG};
    pt23[0] = 0x10201000000UL; pt23[1] = 0x10201040000UL;
    l1_23[0] = 0x10201000000UL; l1_23[1] = 1;
    pt16[0] = 0x10201100000UL; pt16[1] = 0x10201140000UL;
    l1_16[0] = 0x10201100000UL; l1_16[1] = 1;
}
enum { ADT_NUB = 1, ADT_DDCP, ADT_DDISP };
static int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/dcpext0/iop-dcpext0-nub")) return ADT_NUB;
    if (!strcmp(path, "/arm-io/dart-dcpext0")) return ADT_DDCP;
    if (!strcmp(path, "/arm-io/dart-dispext0")) return ADT_DDISP;
    return -1;
}
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (node == ADT_NUB) {
        if (!strcmp(name, "uuid") && ext0_uuid) {
            if (len) *len = strlen(ext0_uuid) + 1;
            return ext0_uuid;
        }
        if (!strcmp(name, "running")) {
            if (len) *len = sizeof(ext0_running);
            return &ext0_running;
        }
        if (!strcmp(name, "segment-ranges") && ext0_nsegs) {
            if (len) *len = ext0_nsegs * sizeof(ext0_segs[0]);
            return ext0_segs;
        }
    } else if (node == ADT_DDCP) {
        if (!strcmp(name, "l1-tt-23") && l1_23[0]) {
            if (len) *len = sizeof(l1_23);
            return l1_23;
        }
        if (!strcmp(name, "pt-region-23")) {
            if (len) *len = sizeof(pt23);
            return pt23;
        }
    } else if (node == ADT_DDISP) {
        if (!strcmp(name, "l1-tt-16") && l1_16[0]) {
            if (len) *len = sizeof(l1_16);
            return l1_16;
        }
        if (!strcmp(name, "pt-region-16")) {
            if (len) *len = sizeof(pt16);
            return pt16;
        }
    }
    return NULL;
}
/* The ADT: the internal DARTs, dcpext0 and its display DART; nothing else */
static int t6050_adt_reg(const char *path, int idx, u64 *addr, u64 *size)
{
    (void)idx; (void)size;
    if (!strcmp(path, "/arm-io/dart-dcp")) *addr = DART_DCP;
    else if (!strcmp(path, "/arm-io/dart-disp0")) *addr = DART_DISP0;
    else if (!strcmp(path, "/arm-io/dcpext0")) *addr = EXT0;
    else if (!strcmp(path, "/arm-io/dart-dispext0")) *addr = DART_DISPEXT0;
    else return -1;
    return 0;
}
''' + definition(source, "static bool dcp6_overlaps(") + \
        definition(source, "static int dcp6_adt_u32(") + \
        definition(source, "static int dcp6_adt_u64_pair(") + \
        definition(source, "static bool dcp6_is_uuid(") + \
        definition(source, "static T6050_TEXT int t6050_node_at(") + \
        definition(source, "static T6050_TEXT bool t6050_available(") + \
        definition(source, "static T6050_INIT bool t6050_identity_reg(") + \
        definition(source, "static T6050_TEXT int t6050_check_placement(") + \
        definition(source, "static T6050_TEXT int t6050_check_ext_adt(") + \
        definition(source, "static T6050_TEXT bool t6050_ext_dart_engine(") + \
        definition(source, "static T6050_TEXT bool t6050_ext_display_dart(") + r'''
enum { PH_EXT = 1, PH_DCP, PH_DISP0, PH_OTHER };
static void dart(const char *name, u64 base, u32 ph, u32 cells, const char *status)
{
    int n = node_at("/", name);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,t8110-dart"));
    reg(n, base, 0x20000);
    assert(!fdt_setprop_u32(tree, n, "#iommu-cells", cells));
    if (status)
        assert(!fdt_setprop_string(tree, n, "status", status));
    assert(!fdt_setprop_u32(tree, n, "phandle", ph));
}
/* dcpext0 whose piodma maps through <ph sid>; `ext_status`, `cells`, `dstatus` vary */
static void world(u32 ph, u32 sid, const char *ext_status, u32 cells, const char *dstatus)
{
    fresh();
    reset_adt();
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    dart("iommu@2b2300000", DART_DISPEXT0, PH_EXT, cells, dstatus);
    dart("iommu@48a340000", DART_DCP, PH_DCP, 1, NULL);
    dart("iommu@48a300000", DART_DISP0, PH_DISP0, 1, NULL);
    dart("iommu@500000000", 0x500000000UL, PH_OTHER, 1, NULL);
    int e = node_at("/", "dcp@2b2e00000");
    assert(!fdt_setprop_string(tree, e, "compatible", "apple,t6050-dcpext"));
    reg(e, EXT0, 0x4000);
    if (ext_status)
        assert(!fdt_setprop_string(tree, e, "status", ext_status));
    int pio = node_at("/dcp@2b2e00000", "piodma");
    fdt32_t io[2] = {cpu_to_fdt32(ph), cpu_to_fdt32(sid)};
    assert(!fdt_setprop(tree, pio, "iommus", io, sizeof(io)));
}
int main(void)
{
    int cases = 0;
    /* The J716c's shape: an enabled engine's piodma on its display DART, SID 16. */
    world(PH_EXT, 16, "okay", 1, "okay");
    assert(t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, NULL, 1, NULL);
    assert(t6050_ext_display_dart(tree, PH_EXT)); cases++;
    /* The engine disabled, its DART disabled, a two-cell DART, or the piodma on SID 15. */
    world(PH_EXT, 16, "disabled", 1, NULL);
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "disabled");
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 2, NULL);
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 15, "okay", 1, NULL);
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    /* A dangling phandle, even when the engine's piodma names it too. */
    world(0x77, 16, "okay", 1, NULL);
    assert(!t6050_ext_display_dart(tree, 0x77)); cases++;
    /* Either internal DART, or a DART that is no engine's dart-dispextN, even if piodma names it. */
    world(PH_DCP, 16, "okay", 1, NULL);
    assert(!t6050_ext_display_dart(tree, PH_DCP)); cases++;
    world(PH_DISP0, 16, "okay", 1, NULL);
    assert(!t6050_ext_display_dart(tree, PH_DISP0)); cases++;
    world(PH_OTHER, 16, "okay", 1, NULL);
    assert(!t6050_ext_display_dart(tree, PH_OTHER)); cases++;
    /* Placement and ADT image checks: segment in OS RAM, pt-region in RAM, running/uuid. */
    world(PH_EXT, 16, "okay", 1, "okay");
    ext0_segs[0].phys = cur_boot_args.phys_base + 0x100000;
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "okay");
    pt23[0] = cur_boot_args.phys_base + 0x200000;
    pt23[1] = cur_boot_args.phys_base + 0x240000;
    l1_23[0] = pt23[0];
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "okay");
    pt16[0] = cur_boot_args.phys_base + 0x300000;
    pt16[1] = cur_boot_args.phys_base + 0x340000;
    l1_16[0] = pt16[0];
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "okay");
    l1_16[0] = 0;
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "okay");
    ext0_running = 0;
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    world(PH_EXT, 16, "okay", 1, "okay");
    ext0_uuid = UUID_B;
    assert(!t6050_ext_display_dart(tree, PH_EXT)); cases++;
    /* status as Linux reads it: absent, or first string "okay"/"ok", regardless of trailing bytes */
    fresh();
    int n = node_at("/", "n");
    assert(t6050_available(tree, n)); cases++;
    assert(!fdt_setprop_string(tree, n, "status", "ok") && t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "okay", 4) && !t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "", 0) && !t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "okay\0x", 6) && t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "okay\0disabled", 14) && t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "ok\0extra", 9) && t6050_available(tree, n)); cases++;
    assert(!fdt_setprop_string(tree, n, "status", "fail") && !t6050_available(tree, n)); cases++;
    assert(!t6050_available(tree, -1)); cases++;
    printf("PASS external scanout darts %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "ext-darts", code, "PASS external scanout darts 25 cases")


def test_t6050_rx_clock_groups_pass_whole(tmp_path):
    source = (REPO / "src/kboot_atc.c").read_text()
    layout = source[source.index("struct atc_tunable {"):source.index("struct atc_fuse_info {")]
    table = source[source.index("static const struct t6050_atc_tunable t6050_atc_tunables[]"):]
    table = table[:table.index("};")]
    # Every group is copied whole, in ADT order: the driver applies the records in that order.
    # The RX clock groups go to the driver's lane RX clock banks (core + 0xd000 and + 0x1d000).
    assert '"apple,tunable-lane0-rx-clock", "core", 20, 0xd000}' in table
    assert '"apple,tunable-lane1-rx-clock", "core", 29, 0x1d000}' in table
    append = definition(source, "static int dt_append_atc_tunable(")
    assert "n_tunables = tunables_len / sizeof(*tunable_adt);" in append
    assert append.count("n_tunables =") == 1
    code = PRELUDE + r'''
#define PACKED __attribute__((packed))
#define static_assert _Static_assert
static void *adt;
static const void *records;
static u32 records_len;
static const void *adt_getprop(void *a, int node, const char *name, u32 *len)
{
    (void)a; (void)node; (void)name;
    *len = records_len;
    return records;
}
''' + layout + definition(source, "static int dt_append_atc_tunable(") + r'''
/*
 * A synthetic RX clock group of four records on four registers of the lane's RX clock bank, the
 * shape a T6050 ADT gives each combo PHY lane. The offsets, masks and values are made up.
 */
#define R(off, mask, value) {0x200 + (off), 32, mask, value}
static const struct atc_tunable four[4] = {
    R(0, 0x3, 0x1), R(8, 0xf0, 0x50), R(0x20, 0x100, 0x0), R(0x2c, 0x7000, 0x3000),
};
/* That group as the OS gets it: <offset mask value> per record, in this order */
static const u32 four_cells[12] = {
    0x200, 0x3, 0x1, 0x208, 0xf0, 0x50, 0x220, 0x100, 0x0, 0x22c, 0x7000, 0x3000,
};
/* Copies @n records through the T6050 RX clock path; checks each one; returns how many */
static int copied(const struct atc_tunable *t, u32 n)
{
    fresh();
    int node = node_at("/", "phy");
    /* As t6050_export_atc() builds it: offsets within the bank, whose size bounds them */
    struct adt_tunable_info info = {"tunable_LN0_RX_CFG_TX_OF_RXCLK",
                                    "apple,tunable-lane0-rx-clock", 0, 0x4000, true};
    records = t;
    records_len = n * sizeof(*t);
    assert(!dt_append_atc_tunable(tree, 0, node, &info));
    int len;
    const fdt32_t *v = fdt_getprop(tree, node, "apple,tunable-lane0-rx-clock", &len);
    assert(v && len % 12 == 0);
    for (int i = 0; i < len / 12; i++)
        assert(fdt32_ld(&v[3 * i]) == t[i].offset && fdt32_ld(&v[3 * i + 1]) == t[i].mask &&
               fdt32_ld(&v[3 * i + 2]) == t[i].value);
    return len / 12;
}
int main(void)
{
    int cases = 0, len;
    struct atc_tunable g[8];
    /* The four records reach the OS unchanged and in order. */
    assert(copied(four, 4) == 4);
    const fdt32_t *v =
        fdt_getprop(tree, fdt_path_offset(tree, "/phy"), "apple,tunable-lane0-rx-clock", &len);
    assert(len == sizeof(four_cells));
    for (int i = 0; i < 12; i++)
        assert(fdt32_ld(&v[i]) == four_cells[i]);
    cases++;
    /* A group that revisits the same registers and fields later passes whole, in order: the
     * later records can be the final values, so none is dropped. */
    memcpy(g, four, sizeof(four));
    for (int i = 0; i < 4; i++)
        g[4 + i] = (struct atc_tunable)R(four[i].offset - 0x200, four[i].mask, 0);
    assert(copied(g, 8) == 8); cases++;
    for (int i = 0; i < 4; i++)
        g[7 - i] = (struct atc_tunable)R(four[i].offset - 0x200, four[i].mask, 0);
    assert(copied(g, 8) == 8); cases++;
    memcpy(g, four, sizeof(four));
    memcpy(g + 4, four, sizeof(four));
    assert(copied(g, 8) == 8); cases++;
    /* An odd group passes whole. */
    assert(copied(four, 3) == 3); cases++;
    /* A record outside the bank fails the group. */
    g[0] = (struct atc_tunable)R(0x4000, 0x1, 0x1);
    fresh();
    int node = node_at("/", "phy");
    struct adt_tunable_info info = {"tunable_LN0_RX_CFG_TX_OF_RXCLK",
                                    "apple,tunable-lane0-rx-clock", 0, 0x4000, true};
    records = g;
    records_len = sizeof(*g);
    assert(dt_append_atc_tunable(tree, 0, node, &info)); cases++;
    printf("PASS rx clock %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "rx-clock", code, "PASS rx clock 6 cases")


def test_t6050_prewired_display_subsystem_is_the_only_one(tmp_path):
    source = (REPO / "src/kboot_display_handoff.c").read_text()
    check = definition(source, "static T6050_INIT int t6050_check_dt(void *dt)")
    assert "int disp = t6050_disp_node(dt);" in check
    code = PRELUDE + r'''
#define T6050_INIT
static struct { bool prewired; } t6050;
static struct { const char *name, *disp_compat; } soc = {"t6050", "apple,display-subsystem"};
static struct { const typeof(soc) *soc; } dcp6 = {&soc};
#define dlog(fmt, ...) printf("DCP: %s: " fmt, dcp6.soc->name, ##__VA_ARGS__)
#undef bail
#define bail(...) do { dlog(__VA_ARGS__); return -1; } while (0)
static int kboot_fdt_path_exact(void *t, const char *alias)
{
    const char *path = fdt_get_alias(t, alias);
    return path ? fdt_path_offset(t, path) : -FDT_ERR_NOTFOUND;
}
''' + definition(source, "static T6050_INIT int t6050_disp_node(") + r'''
static int subsystem(const char *name)
{
    int n = node_at("/", name);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,display-subsystem"));
    return n;
}
int main(void)
{
    int cases = 0;
    /* Prewired (the J716c): its one display subsystem. */
    fresh();
    subsystem("display");
    t6050.prewired = true;
    assert(t6050_disp_node(tree) == fdt_path_offset(tree, "/display")); cases++;
    /* A second one, enabled or not: refused. */
    int second = subsystem("display-die1");
    assert(!fdt_setprop_string(tree, second, "status", "disabled"));
    assert(t6050_disp_node(tree) < 0); cases++;
    /* None. */
    fresh();
    assert(t6050_disp_node(tree) < 0); cases++;
    /* An aliased tree names its own by disp0, however many there are. */
    fresh();
    subsystem("display");
    subsystem("display-die1");
    assert(!fdt_setprop_string(tree, node_at("/", "aliases"), "disp0", "/display-die1"));
    t6050.prewired = false;
    assert(t6050_disp_node(tree) == fdt_path_offset(tree, "/display-die1")); cases++;
    printf("PASS display subsystem %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "disp-node", code, "PASS display subsystem 4 cases")


def test_t6050_external_engines_get_the_stub_version(tmp_path):
    source = (REPO / "src/kboot_display_handoff.c").read_text()
    firmware = (REPO / "src/firmware.c").read_text()
    header = (REPO / "src/adt.h").read_text()
    layout_start = header.index("struct adt_segment_ranges {")
    layout = header[layout_start:header.index("} PACKED;", layout_start) + len("} PACKED;")]
    code = PRELUDE + r'''
#include <stdlib.h>
#define T6050_TEXT
#define T6050_INIT
#define T6050_PATH 64
#define T6050_ENGINES 8
#define PACKED __attribute__((packed))
#define SEG_FLAGS_OSLOG 0xa
#define SZ_16K 0x4000UL
#define DART_PAGE 0x4000UL
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define ALIGN_UP(x, a) ALIGN_DOWN((x) + (a) - 1, a)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define OS_VER_COMP 4
enum fw_version { V_UNKNOWN = 0, V27_0, V27_2, V27_3 };
struct fw_version_info {
    enum fw_version version;
    const char *string;
    u32 num[OS_VER_COMP];
    size_t num_length;
    const char *iboot;
};
''' + layout + r'''
static struct fw_version_info os_firmware;
static struct { u64 phys_base, mem_size; } cur_boot_args = {0x10000000000UL, 0x200000000UL};
static struct { const char *uuid; u32 ext_aura; } t6050;
static struct { const char *name; } soc = {"t6050"};
static struct { const typeof(soc) *soc; } dcp6 = {&soc};
static void *adt;
#define dlog(fmt, ...) printf("DCP: %s: " fmt, dcp6.soc->name, ##__VA_ARGS__)
#define UUID_A "C51FB0C8-C45D-39B0-85BC-520E600FC950"
#define UUID_B "64863924-B56B-3E32-87FE-038677F52709"
#define DART_DISP0 0x48a300000UL
struct fake_ext {
    u64 coproc;
    u64 dart_disp;
    u32 running;
    const char *uuid;
    struct adt_segment_ranges seg[2];
    u32 nseg;
    u64 pt23[2], l1_23[2], pt16[2], l1_16[2];
};
static struct fake_ext exts[3];
static void reset_adt(void)
{
    t6050.uuid = UUID_A;
    for (int i = 0; i < 3; i++) {
        exts[i] = (struct fake_ext){
            .coproc = 0x2b2e00000UL + (u64)i * 0x1000000UL,
            .dart_disp = 0x2b2300000UL + (u64)i * 0x1000000UL,
            .running = 1,
            .uuid = UUID_A,
            .seg = {
                {0x10200000000UL + (u64)i * 0x1000000UL, 0,
                 0x10200000000UL + (u64)i * 0x1000000UL, 0x4000, 0},
                {0x10004500000UL + (u64)i * 0x4000UL, 0,
                 0x10004500000UL + (u64)i * 0x4000UL, 0x4000, SEG_FLAGS_OSLOG},
            },
            .nseg = 2,
            .pt23 = {0x10201000000UL, 0x10201040000UL},
            .l1_23 = {0x10201000000UL, 1},
            .pt16 = {0x10201100000UL, 0x10201140000UL},
            .l1_16 = {0x10201100000UL, 1},
        };
    }
}
static int kboot_fdt_path_exact(void *t, const char *path)
{
    return fdt_path_offset(t, path);
}
static int t6050_adt_reg(const char *path, int idx, u64 *addr, u64 *size)
{
    (void)idx; (void)size;
    if (!strcmp(path, "/arm-io/dart-disp0")) {
        *addr = DART_DISP0;
        return 0;
    }
    for (int i = 0; i < 3; i++) {
        char a[32];
        snprintf(a, sizeof(a), "/arm-io/dcpext%d", i);
        if (!strcmp(path, a)) {
            *addr = exts[i].coproc;
            return 0;
        }
        snprintf(a, sizeof(a), "/arm-io/dart-dispext%d", i);
        if (!strcmp(path, a)) {
            *addr = exts[i].dart_disp;
            return 0;
        }
    }
    return -1;
}
static int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    for (int i = 0; i < 3; i++) {
        char b[64];
        snprintf(b, sizeof(b), "/arm-io/dcpext%d/iop-dcpext%d-nub", i, i);
        if (!strcmp(path, b)) return i * 3 + 1;
        snprintf(b, sizeof(b), "/arm-io/dart-dcpext%d", i);
        if (!strcmp(path, b)) return i * 3 + 2;
        snprintf(b, sizeof(b), "/arm-io/dart-dispext%d", i);
        if (!strcmp(path, b)) return i * 3 + 3;
    }
    return -1;
}
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (node <= 0)
        return NULL;
    int i = (node - 1) / 3, kind = (node - 1) % 3;
    if (kind == 0) {
        if (!strcmp(name, "uuid") && exts[i].uuid) {
            if (len) *len = strlen(exts[i].uuid) + 1;
            return exts[i].uuid;
        }
        if (!strcmp(name, "running")) {
            if (len) *len = sizeof(exts[i].running);
            return &exts[i].running;
        }
        if (!strcmp(name, "segment-ranges") && exts[i].nseg) {
            if (len) *len = exts[i].nseg * sizeof(exts[i].seg[0]);
            return exts[i].seg;
        }
    } else if (kind == 1) {
        if (!strcmp(name, "l1-tt-23") && exts[i].l1_23[0]) {
            if (len) *len = sizeof(exts[i].l1_23);
            return exts[i].l1_23;
        }
        if (!strcmp(name, "pt-region-23")) {
            if (len) *len = sizeof(exts[i].pt23);
            return exts[i].pt23;
        }
    } else {
        if (!strcmp(name, "l1-tt-16") && exts[i].l1_16[0]) {
            if (len) *len = sizeof(exts[i].l1_16);
            return exts[i].l1_16;
        }
        if (!strcmp(name, "pt-region-16")) {
            if (len) *len = sizeof(exts[i].pt16);
            return exts[i].pt16;
        }
    }
    return NULL;
}
''' + definition(firmware, "int firmware_set_fdt(") + \
        definition(source, "static bool dcp6_overlaps(") + \
        definition(source, "static int dcp6_adt_u32(") + \
        definition(source, "static int dcp6_adt_u64_pair(") + \
        definition(source, "static bool dcp6_is_uuid(") + r'''
static bool fail_scratch;
static void *test_malloc(size_t n)
{
    return fail_scratch ? NULL : malloc(n);
}
#define malloc test_malloc
''' + definition(source, "static void *dcp6_scratch(") + r'''
#undef malloc
''' + definition(source, "static int dcp6_commit(") + \
        definition(source, "static T6050_TEXT bool t6050_available(") + \
        definition(source, "static T6050_INIT bool t6050_identity_reg(") + \
        definition(source, "static T6050_TEXT int t6050_check_placement(") + \
        definition(source, "static T6050_TEXT int t6050_check_ext_adt(") + \
        definition(source, "static T6050_TEXT int t6050_ext_index(") + \
        definition(source, "static T6050_TEXT bool t6050_dart_at(") + \
        definition(source, "static T6050_TEXT int t6050_drop_ext_scanout(") + \
        definition(source, "static T6050_TEXT int t6050_ext_walk(") + \
        definition(source, "static T6050_TEXT void t6050_ext_reject(") + \
        definition(source, "static T6050_TEXT void t6050_ext_version(") + r'''
static const u32 tree_version[3] = {27, 0, 0};
enum { PH_DISP0 = 10, PH_DART0 = 11, PH_DART1 = 12, PH_DART2 = 13 };
static void dart_node(const char *name, u64 base, u32 ph)
{
    int n = node_at("/", name);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,t8110-dart"));
    reg(n, base, 0x20000);
    assert(!fdt_setprop_u32(tree, n, "#iommu-cells", 1));
    assert(!fdt_setprop_u32(tree, n, "phandle", ph));
}
/* A dcpext node with reg, piodma, apple,firmware-compat, and apple,firmware-version when @version */
static int engine_under(const char *parent, const char *name, u64 coproc, const char *status,
                        bool version, u32 dart_ph)
{
    int n = node_at(parent, name);
    fdt32_t v[3];
    for (int i = 0; i < 3; i++)
        v[i] = cpu_to_fdt32(tree_version[i]);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,t6050-dcpext"));
    reg(n, coproc, 0x4000);
    assert(!fdt_setprop(tree, n, "apple,firmware-compat", v, sizeof(v)));
    if (version)
        assert(!fdt_setprop(tree, n, "apple,firmware-version", v, sizeof(v)));
    if (status)
        assert(!fdt_setprop_string(tree, n, "status", status));
    char ep[160];
    int elen = snprintf(ep, sizeof(ep), "%s%s%s", strcmp(parent, "/") ? parent : "", "/", name);
    assert(elen > 0 && (size_t)elen < sizeof(ep));
    int pio = node_at(ep, "piodma");
    fdt32_t io[2] = {cpu_to_fdt32(dart_ph), cpu_to_fdt32(16)};
    assert(!fdt_setprop(tree, pio, "iommus", io, sizeof(io)));
    return n;
}
/* Whether @prop of node @name is @want's cells */
static bool is(const char *name, const char *prop, const struct fw_version_info *want)
{
    int len;
    const fdt32_t *v = fdt_getprop(tree, fdt_path_offset(tree, name), prop, &len);
    if (!v || len != (int)(want->num_length * 4))
        return false;
    for (size_t i = 0; i < want->num_length; i++)
        if (fdt32_ld(&v[i]) != want->num[i])
            return false;
    return true;
}
static bool is_disabled(const char *path)
{
    int n = fdt_path_offset(tree, path);
    const char *s = n < 0 ? NULL : fdt_getprop(tree, n, "status", NULL);
    return s && !strcmp(s, "disabled");
}
static bool has_scanout_dart(u32 ph)
{
    int disp = fdt_path_offset(tree, "/display");
    int len = 0;
    const fdt32_t *io = disp < 0 ? NULL : fdt_getprop(tree, disp, "iommus", &len);
    for (int i = 0; io && i + 2 <= len / 4; i += 2)
        if (fdt32_ld(&io[i]) == ph)
            return true;
    return false;
}
static void world(void)
{
    fresh();
    reset_adt();
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    dart_node("iommu@48a300000", DART_DISP0, PH_DISP0);
    dart_node("iommu@2b2300000", exts[0].dart_disp, PH_DART0);
    dart_node("iommu@2b3300000", exts[1].dart_disp, PH_DART1);
    dart_node("iommu@2b4300000", exts[2].dart_disp, PH_DART2);
    int disp = node_at("/", "display");
    assert(!fdt_setprop_string(tree, disp, "compatible", "apple,display-subsystem"));
    fdt32_t io[8] = {cpu_to_fdt32(PH_DISP0), 0, cpu_to_fdt32(PH_DART0), 0,
                     cpu_to_fdt32(PH_DART1), 0, cpu_to_fdt32(PH_DART2), 0};
    assert(!fdt_setprop(tree, disp, "iommus", io, sizeof(io)));
    engine_under("/", "dcpext0", exts[0].coproc, NULL, true, PH_DART0);
    engine_under("/", "dcpext1", exts[1].coproc, "okay", false, PH_DART1);
    engine_under("/", "dcpext2", exts[2].coproc, "disabled", true, PH_DART2);
}
static void assert_rejected(const char *path, u32 dart_ph, const struct fw_version_info *old)
{
    assert(is_disabled(path));
    assert(is(path, "apple,firmware-compat", old));
    assert(has_scanout_dart(PH_DISP0));
    assert(has_scanout_dart(PH_DART0));
    assert(!has_scanout_dart(dart_ph));
}
int main(void)
{
    static const struct fw_version_info old = {V27_0, "27.0", {27, 0, 0}, 3, NULL};
    static const struct fw_version_info v272 = {V27_2, "27.2", {27, 2, 0}, 3, NULL};
    static const struct fw_version_info v273 = {V27_3, "27.3.1", {27, 3, 1, 4}, 4, NULL};
    static unsigned char before[sizeof(tree)];
    int cases = 0;
    /* A three-cell stub: the engines the tree enables take it, a disabled one keeps the tree's. */
    world();
    os_firmware = v272;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert(is("/dcpext0", "apple,firmware-version", &v272));
    assert(is("/dcpext1", "apple,firmware-compat", &v272));
    assert(!fdt_getprop(tree, fdt_path_offset(tree, "/dcpext1"), "apple,firmware-version", NULL));
    assert(is("/dcpext2", "apple,firmware-compat", &old));
    assert(has_scanout_dart(PH_DART0) && has_scanout_dart(PH_DART1)); cases++;
    /* An enabled engine the tree gave no apple,firmware-compat gets one: the driver has no
     * method table without it. apple,firmware-version is not added where it was absent. */
    world();
    assert(!fdt_delprop(tree, fdt_path_offset(tree, "/dcpext1"), "apple,firmware-compat"));
    os_firmware = v272;
    t6050_ext_version(tree);
    assert(is("/dcpext1", "apple,firmware-compat", &v272));
    assert(!fdt_getprop(tree, fdt_path_offset(tree, "/dcpext1"), "apple,firmware-version", NULL));
    assert(is("/dcpext0", "apple,firmware-compat", &v272)); cases++;
    /* A four-cell stub fits too: the properties grow. */
    world();
    os_firmware = v273;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v273));
    assert(is("/dcpext0", "apple,firmware-version", &v273));
    assert(is("/dcpext1", "apple,firmware-compat", &v273));
    assert(is("/dcpext2", "apple,firmware-version", &old)); cases++;
    /* No room in the tree for the four cells: nothing changes. */
    world();
    assert(!fdt_pack(tree));
    memcpy(before, tree, fdt_totalsize(tree));
    t6050_ext_version(tree);
    assert(!memcmp(before, tree, fdt_totalsize(tree))); cases++;
    /* An unknown firmware: nothing changes. */
    world();
    os_firmware = (struct fw_version_info){V_UNKNOWN, NULL, {0}, 0, NULL};
    memcpy(before, tree, sizeof(tree));
    t6050_ext_version(tree);
    assert(!memcmp(before, tree, sizeof(tree))); cases++;
    /* One engine fails placement (segment in OS RAM): disabled and dropped from scanout. */
    world();
    os_firmware = v272;
    exts[1].seg[0].phys = cur_boot_args.phys_base + 0x100000;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* One engine fails pt-region placement: disabled and dropped from scanout. */
    world();
    exts[1].pt16[0] = cur_boot_args.phys_base + 0x200000;
    exts[1].pt16[1] = cur_boot_args.phys_base + 0x240000;
    exts[1].l1_16[0] = exts[1].pt16[0];
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* One engine fails running or uuid check: disabled and dropped from scanout. */
    world();
    exts[1].running = 0;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    world();
    exts[1].uuid = UUID_B;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* An engine with no matching ADT dcpextN: disabled and dropped from scanout. */
    world();
    reg(fdt_path_offset(tree, "/dcpext1"), 0x2b9e00000UL, 0x4000);
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* An engine behind a translated bus (non-1:1 ranges) whose raw reg equals dcpext1 is
     * rejected rather than matched to ADT dcpext1. */
    fresh();
    reset_adt();
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    dart_node("iommu@48a300000", DART_DISP0, PH_DISP0);
    dart_node("iommu@2b2300000", exts[0].dart_disp, PH_DART0);
    dart_node("iommu@2b3300000", exts[1].dart_disp, PH_DART1);
    int disp = node_at("/", "display");
    assert(!fdt_setprop_string(tree, disp, "compatible", "apple,display-subsystem"));
    fdt32_t io2[6] = {cpu_to_fdt32(PH_DISP0), 0, cpu_to_fdt32(PH_DART0), 0,
                      cpu_to_fdt32(PH_DART1), 0};
    assert(!fdt_setprop(tree, disp, "iommus", io2, sizeof(io2)));
    engine_under("/", "dcpext0", exts[0].coproc, "okay", true, PH_DART0);
    int bus = node_at("/", "bus");
    assert(!fdt_setprop_u32(tree, bus, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, bus, "#size-cells", 2));
    fdt32_t tr[6] = {cpu_to_fdt32((u32)(exts[1].coproc >> 32)), cpu_to_fdt32((u32)exts[1].coproc),
                     cpu_to_fdt32(5), 0, 0, cpu_to_fdt32(0x10000)};
    assert(!fdt_setprop(tree, bus, "ranges", tr, sizeof(tr)));
    engine_under("/bus", "dcpext1", exts[1].coproc, "okay", false, PH_DART1);
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/bus/dcpext1", PH_DART1, &old); cases++;
    /* Even when every available engine is rejected (zero version updates), the rejection still
     * commits. */
    fresh();
    reset_adt();
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    dart_node("iommu@48a300000", DART_DISP0, PH_DISP0);
    dart_node("iommu@2b3300000", exts[1].dart_disp, PH_DART1);
    disp = node_at("/", "display");
    assert(!fdt_setprop_string(tree, disp, "compatible", "apple,display-subsystem"));
    fdt32_t io3[4] = {cpu_to_fdt32(PH_DISP0), 0, cpu_to_fdt32(PH_DART1), 0};
    assert(!fdt_setprop(tree, disp, "iommus", io3, sizeof(io3)));
    engine_under("/", "dcpext1", exts[1].coproc, "okay", false, PH_DART1);
    exts[1].running = 0;
    t6050_ext_version(tree);
    assert(is_disabled("/dcpext1") && has_scanout_dart(PH_DISP0) && !has_scanout_dart(PH_DART1));
    cases++;
    /* Unknown stub: healthy engines keep the tree's version, rejected ones are still disabled and
     * stripped from scanout. */
    world();
    os_firmware = (struct fw_version_info){V_UNKNOWN, NULL, {0}, 0, NULL};
    exts[1].running = 0;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &old) && !is_disabled("/dcpext0"));
    assert(has_scanout_dart(PH_DISP0) && has_scanout_dart(PH_DART0));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* Packed DT where the four-cell version write fails: rejection is NOT rolled back. */
    world();
    os_firmware = v273;
    exts[1].running = 0;
    assert(!fdt_pack(tree));
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &old) && !is_disabled("/dcpext0"));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* Packed DT where status = "disabled" cannot fit (absent status on dcpext0): the rejected
     * node is deleted instead and its scanout DART is stripped. */
    world();
    os_firmware = v273;
    exts[0].running = 0;
    assert(!fdt_pack(tree));
    t6050_ext_version(tree);
    assert(fdt_path_offset(tree, "/dcpext0") < 0 && !has_scanout_dart(PH_DART0));
    assert(has_scanout_dart(PH_DISP0) && has_scanout_dart(PH_DART1)); cases++;
    /* Stopped engine behind two identity-mapped buses with path > 64 bytes: still disabled and
     * stripped from scanout rather than aborting on T6050_PATH. */
    fresh();
    reset_adt();
    os_firmware = v272;
    assert(!fdt_setprop_u32(tree, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, 0, "#size-cells", 2));
    dart_node("iommu@48a300000", DART_DISP0, PH_DISP0);
    dart_node("iommu@2b2300000", exts[0].dart_disp, PH_DART0);
    dart_node("iommu@2b3300000", exts[1].dart_disp, PH_DART1);
    disp = node_at("/", "display");
    assert(!fdt_setprop_string(tree, disp, "compatible", "apple,display-subsystem"));
    assert(!fdt_setprop(tree, disp, "iommus", io2, sizeof(io2)));
    engine_under("/", "dcpext0", exts[0].coproc, "okay", true, PH_DART0);
    int b1 = node_at("/", "bus-level-one-display-fabric");
    assert(!fdt_setprop_u32(tree, b1, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, b1, "#size-cells", 2));
    fdt32_t idr[6] = {cpu_to_fdt32(2), 0, cpu_to_fdt32(2), 0, cpu_to_fdt32(4), 0};
    assert(!fdt_setprop(tree, b1, "ranges", idr, sizeof(idr)));
    int b2 = node_at("/bus-level-one-display-fabric", "bus-level-two-external-engines");
    assert(!fdt_setprop_u32(tree, b2, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, b2, "#size-cells", 2));
    assert(!fdt_setprop_empty(tree, b2, "ranges"));
    static const char nested_ep[] =
        "/bus-level-one-display-fabric/bus-level-two-external-engines/dcpext@2b3e00000";
    _Static_assert(sizeof(nested_ep) > T6050_PATH, "test path must exceed T6050_PATH");
    engine_under("/bus-level-one-display-fabric/bus-level-two-external-engines",
                 "dcpext@2b3e00000", exts[1].coproc, "okay", false, PH_DART1);
    exts[1].running = 0;
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected(nested_ep, PH_DART1, &old); cases++;
    /* Scratch allocation failure during rejection: falls back to in-place editing so the
     * unsafe engine is still disabled (or deleted on a packed DT) and stripped from scanout. */
    world();
    os_firmware = v272;
    exts[1].running = 0;
    fail_scratch = true;
    t6050_ext_version(tree);
    fail_scratch = false;
    assert(is("/dcpext0", "apple,firmware-compat", &old));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    world();
    os_firmware = v272;
    exts[0].running = 0;
    assert(!fdt_pack(tree));
    fail_scratch = true;
    t6050_ext_version(tree);
    fail_scratch = false;
    assert(fdt_path_offset(tree, "/dcpext0") < 0 && !has_scanout_dart(PH_DART0));
    assert(has_scanout_dart(PH_DISP0) && has_scanout_dart(PH_DART1)); cases++;
    /* Scanout iommus > 128 bytes on an enabled display-subsystem: compacts iommus in place
     * without allocating, strips PH_DART1 while keeping other phandles intact, disables the
     * rejected engine, and updates healthy engines. */
    for (int inplace = 0; inplace < 2; inplace++) {
        world();
        os_firmware = v272;
        exts[1].running = 0;
        int d = fdt_path_offset(tree, "/display");
        assert(!fdt_setprop_string(tree, d, "status", "okay"));
        fdt32_t big_io[36];
        big_io[0] = cpu_to_fdt32(PH_DISP0); big_io[1] = 0;
        big_io[2] = cpu_to_fdt32(PH_DART0); big_io[3] = 0;
        big_io[4] = cpu_to_fdt32(PH_DART1); big_io[5] = 0;
        for (int i = 3; i < 18; i++) {
            big_io[2 * i] = cpu_to_fdt32(100 + i);
            big_io[2 * i + 1] = 0;
        }
        assert(!fdt_setprop(tree, d, "iommus", big_io, sizeof(big_io)));
        if (inplace) {
            assert(!fdt_pack(tree));
            fail_scratch = true;
        }
        t6050_ext_version(tree);
        fail_scratch = false;
        assert(is_disabled("/dcpext1") && is("/dcpext1", "apple,firmware-compat", &old));
        if (!inplace)
            assert(is("/dcpext0", "apple,firmware-compat", &v272));
        else
            assert(is("/dcpext0", "apple,firmware-compat", &old));
        int len = 0;
        const fdt32_t *got = fdt_getprop(tree, fdt_path_offset(tree, "/display"), "iommus", &len);
        assert(got && len == 34 * (int)sizeof(fdt32_t));
        assert(fdt32_ld(&got[0]) == PH_DISP0 && !fdt32_ld(&got[1]));
        assert(fdt32_ld(&got[2]) == PH_DART0 && !fdt32_ld(&got[3]));
        for (int i = 3; i < 18; i++) {
            int j = (i - 1) * 2;
            assert(fdt32_ld(&got[j]) == (u32)(100 + i) && !fdt32_ld(&got[j + 1]));
        }
        assert(!has_scanout_dart(PH_DART1));
        cases++;
    }
    /* status = "okay\0..." with trailing bytes: Linux probes the first string, so rejection
     * still disables the engine and strips its scanout DART. */
    world();
    os_firmware = v272;
    exts[1].running = 0;
    assert(!fdt_setprop(tree, fdt_path_offset(tree, "/dcpext1"), "status", "okay\0disabled", 14));
    t6050_ext_version(tree);
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert_rejected("/dcpext1", PH_DART1, &old); cases++;
    /* An engine Aura just enabled itself (ext_aura bit) is left untouched by t6050_ext_version(),
     * even if its ADT lacks pt-region. */
    world();
    os_firmware = v272;
    exts[1].pt16[0] = exts[1].pt16[1] = 0;
    t6050.ext_aura = 1u << 1;
    t6050_ext_version(tree);
    t6050.ext_aura = 0;
    assert(is("/dcpext0", "apple,firmware-compat", &v272));
    assert(!is_disabled("/dcpext1") && is("/dcpext1", "apple,firmware-compat", &old));
    assert(has_scanout_dart(PH_DART1)); cases++;
    printf("PASS dcpext version %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "ext-version", code, "PASS dcpext version 22 cases")
