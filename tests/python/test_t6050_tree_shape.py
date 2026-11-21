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
    code = PRELUDE + r'''
#define T6050_TEXT
#define T6050_INIT
#define DART_DCP 0x48a340000UL
#define DART_DISP0 0x48a300000UL
#define EXT0 0x2b2e00000UL
#define DART_DISPEXT0 0x2b2300000UL
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
''' + definition(source, "static T6050_TEXT int t6050_node_at(") + \
        definition(source, "static T6050_TEXT bool t6050_available(") + \
        definition(source, "static T6050_INIT bool t6050_identity_reg(") + \
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
    /* status as whole strings: "okay"/"ok" with their terminator, nothing longer or shorter */
    fresh();
    int n = node_at("/", "n");
    assert(t6050_available(tree, n)); cases++;
    assert(!fdt_setprop_string(tree, n, "status", "ok") && t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "okay", 4) && !t6050_available(tree, n)); cases++;
    assert(!fdt_setprop(tree, n, "status", "okay\0x", 6) && !t6050_available(tree, n)); cases++;
    assert(!fdt_setprop_string(tree, n, "status", "fail") && !t6050_available(tree, n)); cases++;
    printf("PASS external scanout darts %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "ext-darts", code, "PASS external scanout darts 15 cases")


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
    code = PRELUDE + r'''
#include <stdlib.h>
#define T6050_TEXT
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
static struct fw_version_info os_firmware;
static struct { const char *name; } soc = {"t6050"};
static struct { const typeof(soc) *soc; } dcp6 = {&soc};
#define dlog(fmt, ...) printf("DCP: %s: " fmt, dcp6.soc->name, ##__VA_ARGS__)
''' + definition(firmware, "int firmware_set_fdt(") + \
        definition(source, "static void *dcp6_scratch(") + \
        definition(source, "static int dcp6_commit(") + \
        definition(source, "static T6050_TEXT bool t6050_available(") + \
        definition(source, "static T6050_TEXT void t6050_ext_version(") + r'''
static const u32 tree_version[3] = {27, 0, 0};
/* A dcpext node with apple,firmware-compat, and apple,firmware-version when @version */
static int engine(const char *name, const char *status, bool version)
{
    int n = node_at("/", name);
    fdt32_t v[3];
    for (int i = 0; i < 3; i++)
        v[i] = cpu_to_fdt32(tree_version[i]);
    assert(!fdt_setprop_string(tree, n, "compatible", "apple,t6050-dcpext"));
    assert(!fdt_setprop(tree, n, "apple,firmware-compat", v, sizeof(v)));
    if (version)
        assert(!fdt_setprop(tree, n, "apple,firmware-version", v, sizeof(v)));
    if (status)
        assert(!fdt_setprop_string(tree, n, "status", status));
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
static void world(void)
{
    fresh();
    engine("dcpext0", NULL, true);
    engine("dcpext1", "okay", false);
    engine("dcpext2", "disabled", true);
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
    assert(is("/dcpext2", "apple,firmware-compat", &old)); cases++;
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
    printf("PASS dcpext version %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "ext-version", code, "PASS dcpext version 5 cases")
