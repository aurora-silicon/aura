/* SPDX-License-Identifier: MIT */
/*
 * Host integration test for populating panel apple,max-brightness from ADT
 * user-accessible-max-nits on the shared dcp6 display handoff path.
 *
 * Reuses the production src/kboot_display_handoff.c + libfdt harness and synthetic
 * DART/PMGR world from tests/t6041_display_host.c without touching hardware.
 */
#define main desktop26_main
#include "t6041_display_host.c"
#undef main

#define UUID27 "964EDE5B-8F78-38C2-A727-519CC9F0AF27"

static void add_panel(void *dt, bool has_max, u32 existing_max)
{
    int dcp = alias_node(dt, "dcp");
    int panel = OK(fdt_add_subnode(dt, dcp, "panel"));
    OK(fdt_setprop_string(dt, panel, "compatible", "apple,panel-j616"));
    OK(fdt_setprop_string(dt, panel, "status", "disabled"));
    if (has_max)
        OK(fdt_setprop_u32(dt, panel, "apple,max-brightness", existing_max));
}

static void add_v27_clocks(void)
{
    u32 ids[] = {361, 458};
    u32 hz[300] = {0};
    hz[361 - 256] = 810000000;
    hz[458 - 256] = 402280000;
    prop_set("/arm-io/disp0", "clock-ids", ids, sizeof(ids));
    prop_set("/arm-io", "clock-frequencies", hz, sizeof(hz));
}

static bool panel_has_max(const void *dt, u32 *out)
{
    int dcp = alias_node(dt, "dcp");
    int panel = dcp < 0 ? -1 : fdt_subnode_offset(dt, dcp, "panel");
    if (panel < 0)
        return false;
    int len = 0;
    const fdt32_t *v = fdt_getprop(dt, panel, "apple,max-brightness", &len);
    if (!v || len != 4)
        return false;
    if (out)
        *out = fdt32_ld(v);
    return true;
}

struct bl_case {
    const char *name;
    const char *what;
};

static const struct bl_case bl_cases[] = {
    {"j616c-backlight-600", "J616C 26.6.2 reads /backlight 600 nits into panel"},
    {"j616c-27-2-backlight-600", "J616C 27.2 reads /backlight 600 nits into panel"},
    {"j616s-backlight-600", "J616s (T6040) reads /backlight 600 nits into panel"},
    {"override-precedence", "/backlight-override wins over /backlight when both are valid"},
    {"override-node-no-prop", "/backlight-override without property falls back to /backlight"},
    {"override-invalid-no-fallback",
     "invalid /backlight-override does not fall back to /backlight"},
    {"override-bad-len-no-fallback",
     "truncated /backlight-override does not fall back to /backlight"},
    {"preexisting-dt-preserved", "pre-existing panel apple,max-brightness is kept untouched"},
    {"absent-adt-prop", "absent /backlight leaves panel without apple,max-brightness"},
    {"zero-nits", "0 nits (0x00000000) is ignored without failing handoff"},
    {"sub-nit-fractional", "fractional-only 0x0000ffff (0 integer nits) is ignored"},
    {"above-range", "10001 nits is ignored without failing handoff"},
    {"bad-len-short", "2-byte user-accessible-max-nits is ignored"},
    {"bad-len-long", "8-byte user-accessible-max-nits is ignored"},
    {"boundary-min-1", "1 nit (0x00010000) is accepted"},
    {"boundary-max-10000", "10000 nits (10000 << 16) is accepted"},
    {"fractional-truncated", "600.5 nits (0x02588000) truncates to 600 nits"},
    {"no-panel-child", "dcp without panel child succeeds without adding panel"},
    {"dt-full-rollback", "FDT out of space during publish leaves DT untouched"},
};

int main(int argc, char **argv)
{
    if (argc == 2 && is(argv[1], "list")) {
        for (size_t i = 0; i < ARRAY_SIZE(bl_cases); i++)
            printf("%s\n", bl_cases[i].name);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: %s list | <scenario>\n", argv[0]);
        return 2;
    }
    const struct bl_case *sc = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(bl_cases); i++)
        if (is(argv[1], bl_cases[i].name))
            sc = &bl_cases[i];
    if (!sc) {
        fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 2;
    }
    const char *s = sc->name;

    struct opts o = {0};
    world_j616s(&o);

    static u8 dt[DT_SIZE], in[DT_SIZE];
    bool use_j616s = is(s, "j616s-backlight-600");
    bool use_27_2 = is(s, "j616c-27-2-backlight-600");

    build_dt(dt, use_j616s ? J616S : J616C, sizeof(J616S));
    if (!use_j616s)
        chip_id = T6041;
    if (use_27_2) {
        os_firmware = fw_versions[V27_2];
        prop_str("/arm-io/dcp/iop-dcp-nub", "uuid", UUID27);
        add_v27_clocks();
    }

    if (!is(s, "no-panel-child"))
        add_panel(dt, is(s, "preexisting-dt-preserved"), 1000);

    bool want_prop = false;
    u32 want_nits = 0;
    bool expect_refuse = is(s, "dt-full-rollback");

    if (is(s, "j616c-backlight-600") || is(s, "j616c-27-2-backlight-600") ||
        is(s, "j616s-backlight-600")) {
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        want_prop = true;
        want_nits = 600;
    } else if (is(s, "override-precedence")) {
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        prop_u32("/backlight-override", "user-accessible-max-nits", 1000u << 16);
        want_prop = true;
        want_nits = 1000;
    } else if (is(s, "override-node-no-prop")) {
        prop_u32("/backlight-override", "other-prop", 1);
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        want_prop = true;
        want_nits = 600;
    } else if (is(s, "override-invalid-no-fallback")) {
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        prop_u32("/backlight-override", "user-accessible-max-nits", 10001u << 16);
    } else if (is(s, "override-bad-len-no-fallback")) {
        u16 short_val = 600;
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        prop_set("/backlight-override", "user-accessible-max-nits", &short_val, sizeof(short_val));
    } else if (is(s, "preexisting-dt-preserved")) {
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
        want_prop = true;
        want_nits = 1000;
    } else if (is(s, "zero-nits")) {
        prop_u32("/backlight", "user-accessible-max-nits", 0);
    } else if (is(s, "sub-nit-fractional")) {
        prop_u32("/backlight", "user-accessible-max-nits", 0x0000ffffu);
    } else if (is(s, "above-range")) {
        prop_u32("/backlight", "user-accessible-max-nits", 10001u << 16);
    } else if (is(s, "bad-len-short")) {
        u16 short_val = 600;
        prop_set("/backlight", "user-accessible-max-nits", &short_val, sizeof(short_val));
    } else if (is(s, "bad-len-long")) {
        u64 long_val = (u64)(600u << 16);
        prop_set("/backlight", "user-accessible-max-nits", &long_val, sizeof(long_val));
    } else if (is(s, "boundary-min-1")) {
        prop_u32("/backlight", "user-accessible-max-nits", 1u << 16);
        want_prop = true;
        want_nits = 1;
    } else if (is(s, "boundary-max-10000")) {
        prop_u32("/backlight", "user-accessible-max-nits", 10000u << 16);
        want_prop = true;
        want_nits = 10000;
    } else if (is(s, "fractional-truncated")) {
        prop_u32("/backlight", "user-accessible-max-nits", (600u << 16) | 0x8000u);
        want_prop = true;
        want_nits = 600;
    } else if (is(s, "no-panel-child") || is(s, "dt-full-rollback")) {
        prop_u32("/backlight", "user-accessible-max-nits", 600u << 16);
    }

    OK(fdt_pack(dt));
    if (expect_refuse)
        OK(fdt_open_into(dt, dt, fdt_totalsize(dt) + 512));
    else
        OK(fdt_open_into(dt, dt, DT_SIZE));
    memcpy(in, dt, fdt_totalsize(dt));

    printf("== %s: %s\n", s, sc->what);
    fflush(stdout);
    int ret = dt_set_display_handoff(dt);
    fflush(stdout);
    check(ret == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO access faulted");

    if (expect_refuse) {
        refusal_check(in, dt, true);
        check(!panel_has_max(dt, NULL), "refused: panel has no apple,max-brightness");
    } else {
        contract_check(dt);
        oslog_check(dt, true);
        check(mmio_writes == 1 && last_write == DART_DCP + 0x200,
              "the only MMIO write is the dart-dcp lock");

        int dcp = alias_node(dt, "dcp");
        int panel = dcp < 0 ? -1 : fdt_subnode_offset(dt, dcp, "panel");
        if (is(s, "no-panel-child")) {
            check(panel < 0, "no panel child created when DT omits panel");
        } else {
            u32 got = 0;
            bool has = panel_has_max(dt, &got);
            check(panel >= 0, "panel child node remains present");
            check(has == want_prop, "panel apple,max-brightness presence matches expected (%d)",
                  want_prop);
            if (want_prop)
                check(got == want_nits, "panel apple,max-brightness = %u (expected %u)", got,
                      want_nits);
        }
    }

    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
