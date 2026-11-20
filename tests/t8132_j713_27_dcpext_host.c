/* SPDX-License-Identifier: MIT */
/*
 * Host test of the T8132 MacBook Air (J713) external display processors on macOS 27.2 (26B123):
 * kboot.c's M4 handoff has handed the internal panel's DCP over, and dt_set_j713_dcpext_handoff()
 * hands dcpext0 and dcpext1 over beside it, each with its own pipe's firmware clocks.
 * tests/python/test_t8132_j713_27_dcpext_handoff.py has the failure model.
 *
 * Reuses the J773g 27.2 host (t8132_j773g_27_display_host.c): its fake ADT (same image, same
 * nub layouts, same dcpext addresses and stream IDs) and device tree builders. Physical
 * addresses of firmware segments are synthetic; nothing here is Apple firmware data.
 *
 *   t8132-j713-27-dcpext-host list
 *   t8132-j713-27-dcpext-host <scenario>
 */
#define J773G_HOST_NO_MAIN
#include "t8132_j773g_27_display_host.c"

int dt_set_j713_dcpext_handoff(void *dt);

#define DCPEXT_READY "apple,t8132-dcpext-firmware-ready"

/* What kboot.c's M4 handoff leaves: the dcp node with the image's UUID, __TEXT reserved for it */
static void m4_internal_handoff(void *dt)
{
    int dcp = alias_node(dt, "dcp");
    OK(fdt_setprop_string(dt, dcp, "apple,firmware-uuid", J773G_UUID));
    OK(fdt_setprop_string(dt, dcp, "status", "okay"));
    char name[48];
    snprintf(name, sizeof(name), "asc-firmware@%llx", (unsigned long long)EXT_TEXT_PHYS);
    int text = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
    OK(fdt_setprop_string(dt, text, "compatible", "apple,asc-mem"));
    reg2(dt, text, EXT_TEXT_PHYS, ALIGN_UP((u64)EXT_TEXT_SIZE, SZ_16K));
    OK(fdt_setprop_empty(dt, text, "no-map"));
    OK(fdt_setprop_u32(dt, text, "phandle", 90));
    fdt32_t tuple[5] = {cpu_to_fdt32(PH_J_DART_DCP + 200)};
    fdt64_st(&tuple[1], EXT_TEXT_REMAP);
    fdt64_st(&tuple[3], ALIGN_UP((u64)EXT_TEXT_SIZE, SZ_16K));
    OK(fdt_setprop_u32(dt, alias_node(dt, "dcp"), "phandle", PH_J_DART_DCP + 200));
    OK(fdt_setprop(dt, fdt_path_offset(dt, "/reserved-memory/asc-firmware@10280000000"),
                   "iommu-addresses", tuple, sizeof(tuple)));
}

/* The J713 tree: the external chains enabled for the 26.6.2 path */
static void enable_ext(void *dt, int i)
{
    char path[96];
    ext_path(path, sizeof(path), i);
    OK(fdt_setprop_string(dt, fdt_path_offset(dt, path), "status", "okay"));
    OK(fdt_setprop_string(dt, fdt_node_offset_by_phandle(dt, 100 + i), "status", "okay"));
}

static void check_j713_ext(const void *in, const void *dt, int i, bool want)
{
    char path[96], dart_path[64];
    ext_path(path, sizeof(path), i);
    snprintf(dart_path, sizeof(dart_path), "/soc/iommu@%llx", (unsigned long long)ext_dart[i]);
    int ext = fdt_path_offset(dt, path), len = 0;
    const fdt32_t *mr = ext < 0 ? NULL : fdt_getprop(dt, ext, "memory-region", &len);

    check(ext >= 0 && !fdt_getprop(dt, ext, DCPEXT_READY, NULL),
          "dcpext%d: no 26.6.2 firmware-ready marker", i);
    if (!want) {
        check(props_unchanged(in, dt, path) && props_unchanged(in, dt, dart_path),
              "dcpext%d refused: its node and DART are unchanged", i);
        for (int j = 0; mr && len == 12 && j < 3; j++) {
            int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
            check(region >= 0 && status_is(dt, region, "disabled") &&
                      !fdt_getprop(dt, region, "iommu-addresses", NULL),
                  "dcpext%d refused: placeholder %d stays disabled", i, j);
        }
        return;
    }

    int in_ext = fdt_path_offset(in, path);
    const char *in_status = fdt_getprop(in, in_ext, "status", NULL);
    const char *status = fdt_getprop(dt, ext, "status", NULL);
    check(in_status && status && !strcmp(in_status, status) && props_unchanged(in, dt, dart_path),
          "dcpext%d and its DART keep the device tree's status (%s)", i, status ? status : "?");
    if (!mr || len != 12)
        return;
    const u32 ver[3] = {27, 2, 0};
    const char *uuid = fdt_getprop(dt, ext, "apple,firmware-uuid", NULL);
    check(u32s_are(dt, ext, "apple,firmware-version", ver, 3) &&
              u32s_are(dt, ext, "apple,firmware-compat", ver, 3) && uuid &&
              !strcmp(uuid, J773G_UUID),
          "dcpext%d: firmware 27.2.0 and the pinned UUID", i);
    check(u32s_are(dt, ext, "apple,clock-ids", ext_clk[i], 2), "dcpext%d: dispext%d's clock-ids", i,
          i);
    check_clock(dt, ext, 0, ext_clk[i][0], EXT_HZ);
    check_clock(dt, ext, 1, ext_clk[i][1], 0);

    int text = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[0]));
    const char *tname = text < 0 ? NULL : fdt_get_name(dt, text, NULL);
    char want_text[48];
    snprintf(want_text, sizeof(want_text), "asc-firmware@%llx", (unsigned long long)EXT_TEXT_PHYS);
    check(tname && !strcmp(tname, want_text) &&
              tuple_for(dt, text, ext, EXT_TEXT_REMAP, ALIGN_UP((u64)EXT_TEXT_SIZE, SZ_16K)) &&
              tuple_for(dt, text, alias_node(dt, "dcp"), EXT_TEXT_REMAP,
                        ALIGN_UP((u64)EXT_TEXT_SIZE, SZ_16K)),
          "dcpext%d: text is the internal handoff's __TEXT, mapped for both processors", i);
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

enum j713_kind { K_PUBLISH, K_NOTHING };

struct j713_scenario {
    const char *name;
    enum j713_kind kind;
    bool ext0, ext1;
    const char *what;
};

static const struct j713_scenario j713_scenarios[] = {
    {"publish", K_PUBLISH, true, true, "both external processors beside the internal handoff"},
    {"dcpext0-only", K_PUBLISH, true, false, "a tree with only dcpext0 described"},
    {"repeat", K_PUBLISH, true, true, "a second call changes nothing"},
    {"ext-disabled", K_PUBLISH, true, true, "a tree that disables the external chains"},
    {"ext-uuid-mismatch", K_PUBLISH, true, false, "dcpext1 runs another image"},
    {"ext-not-running", K_PUBLISH, false, true, "dcpext0 is not running"},
    {"ext-text-not-shared", K_PUBLISH, true, false, "dcpext1's __TEXT is its own"},
    {"no-internal-text", K_PUBLISH, false, false, "the internal __TEXT reservation is missing"},
    {"no-internal-handoff", K_NOTHING, false, false, "the internal DCP was not handed over"},
    {"j773g", K_NOTHING, false, false, "the J773g is not this board"},
    {"os-26-6-2", K_NOTHING, false, false, "26.6.2 firmware"},
    {"os-27-0", K_NOTHING, false, false, "27.0 firmware"},
    {"dt-full", K_PUBLISH, false, false, "no room: refused without partial edits"},
};

int main(int argc, char **argv)
{
    if (argc == 2 && is(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(j713_scenarios); i++)
            printf("%s\n", j713_scenarios[i].name);
        return 0;
    }
    if (argc != 2)
        return 2;
    const struct j713_scenario *sc = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(j713_scenarios); i++)
        if (is(argv[1], j713_scenarios[i].name))
            sc = &j713_scenarios[i];
    if (!sc)
        return 2;
    const char *s = sc->name;

    world_j773g();
    static u8 dt[DT_SIZE], in[DT_SIZE];
    if (is(s, "j773g"))
        build_dt_j773g(dt, J773G, sizeof(J773G));
    else
        build_dt_j773g(dt, J713, sizeof(J713));
    int next = is(s, "dcpext0-only") ? 1 : 2;
    for (int i = 0; i < next; i++) {
        build_ext_j773g(dt, i);
        world_ext_j773g(i);
        if (!is(s, "ext-disabled"))
            enable_ext(dt, i);
    }
    if (is(s, "dcpext0-only"))
        world_ext_j773g(1);
    if (!is(s, "no-internal-handoff"))
        m4_internal_handoff(dt);

    char path[96];
    if (is(s, "os-26-6-2"))
        os_firmware =
            (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
    if (is(s, "os-27-0"))
        os_firmware = (struct fw_version_info){V27_0, "27.0", {27, 0, 0}, 3, "mBoot-20457.1.29"};
    if (is(s, "ext-uuid-mismatch"))
        prop_str("/arm-io/dcpext1/iop-dcpext1-nub", "uuid", OTHER_UUID);
    if (is(s, "ext-not-running"))
        prop_u32("/arm-io/dcpext0/iop-dcpext0-nub", "running", 0);
    if (is(s, "ext-text-not-shared")) {
        struct seg ext[4] = {
            {TB + 0x284000000ULL, 0, EXT_TEXT_REMAP, EXT_TEXT_SIZE, 1},
            {ext_data(1), 0x10000, EXT_DATA_REMAP, EXT_DATA_SIZE, 0},
            {ext_oslog(1), 0x18000, ext_oslog(1), 0x4000, 0xa},
            {ext_heap(1), ~0ULL, EXT_HEAP_REMAP, EXT_HEAP_SIZE, 2},
        };
        prop_set("/arm-io/dcpext1/iop-dcpext1-nub", "segment-ranges", ext, sizeof(ext));
    }
    if (is(s, "no-internal-text"))
        OK(fdt_del_node(dt, fdt_path_offset(dt, "/reserved-memory/asc-firmware@10280000000")));
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
    int ret = dt_set_j713_dcpext_handoff(dt);
    fflush(stdout);
    check(ret == 0, "dt_set_j713_dcpext_handoff() returns 0");
    check(exc_count == 0, "no MMIO access faulted");
    check(mmio_reads == 0 && mmio_writes == 0, "no MMIO access at all");

    if (sc->kind == K_NOTHING) {
        check(fdt_totalsize(dt) == fdt_totalsize(in) && !memcmp(in, dt, fdt_totalsize(in)),
              "nothing done: the device tree is byte for byte unchanged");
    } else {
        check(props_unchanged(in, dt, "/soc/dcp@30ae00000") &&
                  props_unchanged(in, dt, "/display-subsystem") &&
                  props_unchanged(in, dt, "/soc/iommu@30a300000"),
              "internal DCP and the display subsystem as the internal handoff left them");
        if (!is(s, "dt-full") && !is(s, "dcpext0-only"))
            check(resv_at(dt, "dcp-oslog", ext_oslog(0), 0x4000) &&
                      resv_at(dt, "dcp-oslog", ext_oslog(1), 0x4000),
                  "dcpext0 and dcpext1 __OS_LOG reserved");
        check_j713_ext(in, dt, 0, sc->ext0);
        if (!is(s, "dcpext0-only"))
            check_j713_ext(in, dt, 1, sc->ext1);
        if (is(s, "ext-uuid-mismatch") || is(s, "ext-text-not-shared")) {
            ext_path(path, sizeof(path), 1);
            u64 sz = 0;
            int text = fdt_path_offset(dt, "/reserved-memory/asc-firmware@10280000000");
            check(text >= 0 && iommu_dva(dt, text, fdt_path_offset(dt, path), &sz) == ~0ULL,
                  "the refused dcpext1 has no tuple on the shared text");
        }
        if (is(s, "repeat")) {
            static u8 first[DT_SIZE];
            memcpy(first, dt, fdt_totalsize(dt));
            fflush(stdout);
            ret = dt_set_j713_dcpext_handoff(dt);
            fflush(stdout);
            check(ret == 0 && fdt_totalsize(first) == fdt_totalsize(dt) &&
                      !memcmp(first, dt, fdt_totalsize(first)),
                  "second call returns 0 and changes nothing in the device tree");
        }
    }

    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
