/* SPDX-License-Identifier: MIT */
/*
 * Host integration test for the T6041 (J616c/J614c) macOS 27.2 (26B123) internal and external
 * display handoff.
 *
 * Reuses the production kboot_display_handoff.c + libfdt harness and synthetic DART/PMGR world
 * from t6041_display_host.c without touching hardware or embedding Apple firmware bytes.
 */
#define main desktop26_main
#include "t6041_display_host.c"
#undef main

#define UUID27            "964EDE5B-8F78-38C2-A727-519CC9F0AF27"
#define WRONG_UUID_26B131 "71F140DC-F18D-31B3-9772-537A5CC8400D"
#define EXT2              0x606e00000ULL
#define EXT_DART2         0x606340000ULL

static u64 ext27_base(int which)
{
    return which == 0 ? EXT0 : which == 1 ? EXT1 : EXT2;
}

static u64 ext27_dart_base(int which)
{
    return which == 0 ? EXT_DART0 : which == 1 ? EXT_DART1 : EXT_DART2;
}

static void build_ext27(void *dt, int which)
{
    if (which < 2) {
        build_ext(dt, which);
        return;
    }
    const u64 base = ext27_base(which);
    const u64 dart_base = ext27_dart_base(which);
    const u32 dart_ph = 100 + which;
    char name[64], path[96];
    snprintf(name, sizeof(name), "iommu@%llx", (unsigned long long)dart_base);
    dart(dt, 0, name, dart_base, dart_ph);
    snprintf(name, sizeof(name), "dcp@%llx", (unsigned long long)base);
    int dcp = OK(fdt_add_subnode(dt, 0, name));
    OK(fdt_setprop_string(dt, dcp, "compatible", "apple,t6041-dcpext"));
    reg2(dt, dcp, base, 0x4000);
    iommus(dt, dcp, dart_ph, 23);
    OK(fdt_setprop_string(dt, dcp, "status", "disabled"));
    fdt32_t mr[3];
    static const char *const parts[] = {"text", "data", "heap"};
    for (int i = 0; i < 3; i++) {
        snprintf(name, sizeof(name), "dcpext%d-%s", which, parts[i]);
        int region = OK(fdt_add_subnode(dt, fdt_path_offset(dt, "/reserved-memory"), name));
        OK(fdt_setprop_string(dt, region, "compatible", "apple,asc-mem"));
        reg2(dt, region, 0, 0x4000);
        OK(fdt_setprop_empty(dt, region, "no-map"));
        OK(fdt_setprop_string(dt, region, "status", "disabled"));
        OK(fdt_setprop_u32(dt, region, "phandle", 110 + which * 3 + i));
        mr[i] = cpu_to_fdt32(110 + which * 3 + i);
    }
    snprintf(path, sizeof(path), "/dcp@%llx", (unsigned long long)base);
    dcp = fdt_path_offset(dt, path);
    OK(fdt_setprop(dt, dcp, "memory-region", mr, sizeof(mr)));
    static const char names[] = "text\0data\0heap";
    OK(fdt_setprop(dt, dcp, "memory-region-names", names, sizeof(names)));
}

static void world_ext27(int which)
{
    if (which < 2) {
        world_ext(which);
        return;
    }
    const u64 base = ext27_base(which);
    const u64 dart_base = ext27_dart_base(which);
    struct seg ext[4] = {
        {segs[SEG_TEXT].phys, 0, segs[SEG_TEXT].remap, segs[SEG_TEXT].size, 1},
        {TB + 0x281200000ULL + which * 0x200000, 0, TB + 0x51000000 + which * 0x100000, 0x8000, 0},
        {TB + 0x28030000ULL + which * 0x40000, 0, TB + 0x28030000ULL + which * 0x40000, 0x4000,
         0xa},
        {TB + 0x282200000ULL + which * 0x200000, ~0ULL, TB + 0x52000000 + which * 0x100000, 0x10000,
         2},
    };
    char path[96];
    snprintf(path, sizeof(path), "/arm-io/dcpext%d", which);
    reg_add(path, base, 0x4000);
    snprintf(path, sizeof(path), "/arm-io/dart-dcpext%d", which);
    reg_add(path, dart_base, 0x4000);
    snprintf(path, sizeof(path), "/arm-io/dcpext%d/iop-dcpext%d-nub", which, which);
    prop_set(path, "segment-ranges", ext, sizeof(ext));
    prop_str(path, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown");
    prop_u32(path, "pre-loaded", 1);
    prop_u32(path, "running", 1);
}

static void clock_props(u32 id0, u32 hz0, u32 id1, u32 hz1)
{
    u32 ids[] = {id0, id1};
    u32 hz[300] = {0};
    hz[id0 - 256] = hz0;
    hz[id1 - 256] = hz1;
    prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    prop_set("/arm-io", "clock-frequencies", hz, sizeof(hz));
}

static void add_static_disp0_clock(void *dt)
{
    int clk = OK(fdt_add_subnode(dt, 0, "clk-disp0"));
    OK(fdt_setprop_string(dt, clk, "compatible", "fixed-clock"));
    OK(fdt_setprop_u32(dt, clk, "#clock-cells", 0));
    OK(fdt_setprop_u32(dt, clk, "clock-frequency", 700000000));
    OK(fdt_setprop_u32(dt, clk, "phandle", 50));
    OK(fdt_setprop_u32(dt, alias_node(dt, "dcp"), "clocks", 50));
}

static void check_clocks_27(const void *dt, u32 want_id0, u32 want_hz0, u32 want_id1, u32 want_hz1)
{
    int dcp = alias_node(dt, "dcp"), len = 0;
    const fdt32_t *ids = fdt_getprop(dt, dcp, "apple,clock-ids", &len);
    check(ids && len == 8 && fdt32_ld(ids) == want_id0 && fdt32_ld(ids + 1) == want_id1,
          "published: apple,clock-ids matches /arm-io/disp0 clock-ids");
    check(fdt_stringlist_count(dt, dcp, "clock-names") == 2 &&
              fdt_stringlist_search(dt, dcp, "clock-names", "firmware-0") == 0 &&
              fdt_stringlist_search(dt, dcp, "clock-names", "firmware-1") == 1,
          "published: clock-names = firmware-0, firmware-1");
    const fdt32_t *clocks = fdt_getprop(dt, dcp, "clocks", &len);
    bool valid = clocks && len == 8;
    for (int i = 0; valid && i < 2; i++) {
        int node = fdt_node_offset_by_phandle(dt, fdt32_ld(clocks + i));
        const fdt32_t *hz = node < 0 ? NULL : fdt_getprop(dt, node, "clock-frequency", &len);
        const fdt32_t *cells = node < 0 ? NULL : fdt_getprop(dt, node, "#clock-cells", NULL);
        valid &= node >= 0 && !fdt_node_check_compatible(dt, node, "fixed-clock") && cells &&
                 fdt32_ld(cells) == 0 && hz && len == 4 &&
                 fdt32_ld(hz) == (i == 0 ? want_hz0 : want_hz1);
    }
    check(valid, "published: fixed-clock providers match indexed clock-frequencies");
    const char *uuid = fdt_getprop(dt, dcp, "apple,firmware-uuid", NULL);
    check(uuid && !strcmp(uuid, UUID27), "published: exact 26B123 firmware UUID");
    int fb = fdt_stringlist_search(dt, dcp, "memory-region-names", "framebuffer");
    check(fb >= 0 && region_at(dt, dcp, fb) >= 0, "published: DCP framebuffer memory-region");
}

static void check_ext_27(const void *dt, int which, bool want)
{
    char path[40], ph_name[24];
    int len = 0;
    snprintf(path, sizeof(path), "/dcp@%llx", (unsigned long long)ext27_base(which));
    int ext = fdt_path_offset(dt, path);
    const fdt32_t *mr = ext < 0 ? NULL : fdt_getprop(dt, ext, "memory-region", NULL);
    check(ext >= 0 && mr && status_is(dt, ext, "disabled"),
          "dcpext%d stays disabled for display-gate admission", which);
    if (ext < 0 || !mr)
        return;

    const char *euuid = fdt_getprop(dt, ext, "apple,firmware-uuid", NULL);
    const void *cids = fdt_getprop(dt, ext, "apple,clock-ids", &len);
    const u32 want_ver[3] = {27, 2, 0};
    if (!want) {
        check(!fdt_getprop(dt, ext, "apple,firmware-version", NULL) &&
                  !fdt_getprop(dt, ext, "apple,firmware-compat", NULL) && !euuid && !cids,
              "dcpext%d refused cleanly without firmware or clock properties", which);
        for (int j = 0; j < 3; j++) {
            int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
            check(region >= 0 && status_is(dt, region, "disabled"),
                  "dcpext%d placeholder %d stays disabled on refusal", which, j);
        }
        return;
    }

    check(u32s_are(dt, ext, "apple,firmware-version", want_ver, 3) &&
              u32s_are(dt, ext, "apple,firmware-compat", want_ver, 3) && euuid &&
              !strcmp(euuid, UUID27) && cids && len == 0,
          "dcpext%d handed off on 27.2 with 26B123 UUID and empty apple,clock-ids", which);
    snprintf(ph_name, sizeof(ph_name), "dcpext%d-text", which);
    check(fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), ph_name) < 0,
          "dcpext%d text placeholder replaced by shared panel text", which);
    for (int j = 1; j < 3; j++) {
        int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
        check(region >= 0 && status_is(dt, region, "okay"),
              "dcpext%d segment %d memory-region enabled", which, j);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *s = argv[1];
    struct opts opts = {0};
    world_j616s(&opts);
    chip_id = T6041;
    os_firmware = (struct fw_version_info){V27_2, "27.2", {27, 2, 0}, 3, "mBoot-20457.41.3"};
    prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", UUID27);
    u32 want_id0 = is(s, "alternate-clock-ids") ? 390 : 376;
    u32 want_hz0 = is(s, "alternate-clock-ids") ? 650000000u : 700000000u;
    u32 want_id1 = is(s, "alternate-clock-ids") ? 488 : 460;
    u32 want_hz1 = is(s, "alternate-clock-ids") ? 120000000u : 0u;
    clock_props(want_id0, want_hz0, want_id1, want_hz1);

    static u8 dt[DT_SIZE], in[DT_SIZE];
    const char *board = (is(s, "j614c-publish") || is(s, "alternate-clock-ids"))  ? J614C
                        : (is(s, "wrong-board-j616s") || is(s, "t6040-j616s-27")) ? J616S
                                                                                  : J616C;
    build_dt(dt, board, sizeof(J616C));

    if (is(s, "publish-static-clock") || is(s, "desktop26-preserved"))
        add_static_disp0_clock(dt);
    if (is(s, "external-handoff") || is(s, "external-uuid-mismatch") ||
        is(s, "wrong-uuid-26b131") || is(s, "j614c-publish")) {
        for (int i = 0; i < 3; i++) {
            char nub[96];
            build_ext27(dt, i);
            world_ext27(i);
            snprintf(nub, sizeof(nub), "/arm-io/dcpext%d/iop-dcpext%d-nub", i, i);
            if (is(s, "external-uuid-mismatch") && i == 1)
                prop_str(nub, "uuid", WRONG_UUID_26B131);
            else if (!(is(s, "external-uuid-mismatch") && i == 2))
                prop_str(nub, "uuid", UUID27);
        }
    }
    if (is(s, "wrong-chip") || is(s, "t6040-j616s-27"))
        chip_id = T6040;
    if (is(s, "wrong-os"))
        os_firmware = (struct fw_version_info){V27_0, "27.0", {27, 0, 0}, 3, "mBoot-20457.1.29"};
    if (is(s, "desktop26-preserved")) {
        os_firmware =
            (struct fw_version_info){V26_6_2, "26.6.2", {26, 6, 2}, 3, "mBoot-18000.161.10"};
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", DCP_UUID);
    }
    if (is(s, "wrong-uuid"))
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", DCP_UUID);
    if (is(s, "wrong-uuid-26b131"))
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", WRONG_UUID_26B131);
    if (is(s, "missing-nub"))
        node_drop("/arm-io/dcp/iop-dcp-nub");
    if (is(s, "no-display-dt"))
        OK(fdt_delprop(dt, fdt_path_offset(dt, "/aliases"), "dcp"));
    if (is(s, "missing-clocks"))
        prop_drop("/arm-io/disp0", "clock-ids");
    if (is(s, "empty-clocks"))
        prop_set("/arm-io/disp0", "clock-ids", "", 0);
    if (is(s, "too-many-clocks")) {
        u32 ids[] = {300, 301, 302, 303, 304};
        prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    }
    if (is(s, "clock-id-below-base")) {
        u32 ids[] = {255, 460};
        prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    }
    if (is(s, "duplicate-clock-ids")) {
        u32 ids[] = {376, 376};
        prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    }
    if (is(s, "bad-clock-order")) {
        u32 ids[] = {460, 376};
        prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    }
    if (is(s, "short-frequencies")) {
        u32 hz = 700000000;
        prop_set("/arm-io", "clock-frequencies", &hz, sizeof(hz));
    }
    if (is(s, "zero-first-clock")) {
        u32 hz[300] = {0};
        prop_set("/arm-io", "clock-frequencies", hz, sizeof(hz));
    }

    bool refuse = is(s, "preexisting-clock-ids") || is(s, "preexisting-clock-names") ||
                  is(s, "preexisting-two-clocks") || is(s, "dt-full");
    if (is(s, "preexisting-clock-ids"))
        OK(fdt_setprop_empty(dt, alias_node(dt, "dcp"), "apple,clock-ids"));
    if (is(s, "preexisting-clock-names"))
        OK(fdt_setprop_string(dt, alias_node(dt, "dcp"), "clock-names", "firmware-0"));
    if (is(s, "preexisting-two-clocks")) {
        fdt32_t two[2] = {cpu_to_fdt32(50), cpu_to_fdt32(51)};
        OK(fdt_setprop(dt, alias_node(dt, "dcp"), "clocks", two, sizeof(two)));
    }
    if (is(s, "dt-full")) {
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, fdt_totalsize(dt) + 600));
    } else {
        OK(fdt_pack(dt));
        OK(fdt_open_into(dt, dt, DT_SIZE));
    }

    memcpy(in, dt, fdt_totalsize(dt));
    mmio_reads = mmio_writes = 0;
    check(dt_set_display_handoff(dt) == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO fault");

    if (is(s, "publish") || is(s, "j614c-publish") || is(s, "alternate-clock-ids") ||
        is(s, "publish-static-clock") || is(s, "external-handoff") ||
        is(s, "external-uuid-mismatch") || is(s, "repeat")) {
        contract_check(dt);
        oslog_check(dt, true);
        check_clocks_27(dt, want_id0, want_hz0, want_id1, want_hz1);
        check(mmio_writes == 1 && last_write == DART_DCP + 0x200,
              "the only MMIO write is the dart-dcp lock");
        if (is(s, "external-handoff") || is(s, "j614c-publish")) {
            for (int i = 0; i < 3; i++)
                check_ext_27(dt, i, true);
        }
        if (is(s, "external-uuid-mismatch")) {
            check_ext_27(dt, 0, true);
            check_ext_27(dt, 1, false);
            check_ext_27(dt, 2, false);
        }
        if (is(s, "repeat")) {
            static u8 first[DT_SIZE];
            memcpy(first, dt, fdt_totalsize(dt));
            int writes = mmio_writes;
            check(dt_set_display_handoff(dt) == 0 && !memcmp(first, dt, fdt_totalsize(dt)) &&
                      mmio_writes == writes,
                  "second call changes nothing and writes no MMIO");
        }
    } else if (is(s, "desktop26-preserved")) {
        contract_check(dt);
        oslog_check(dt, true);
        check(!fdt_getprop(dt, alias_node(dt, "dcp"), "apple,clock-ids", NULL),
              "26.6.2 keeps static dcp clock without 27.2 apple,clock-ids");
    } else if (refuse) {
        refusal_check(in, dt, true);
    } else {
        check(fdt_totalsize(dt) == fdt_totalsize(in) && !memcmp(in, dt, fdt_totalsize(in)),
              "refused: FDT byte for byte unchanged");
        check(mmio_reads == 0 && mmio_writes == 0, "refused: zero MMIO");
    }
    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
