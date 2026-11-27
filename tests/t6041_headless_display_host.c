/* SPDX-License-Identifier: MIT */
/*
 * Host integration test for headless T6041 (J575c) internal DCP sleep-host handoff.
 *
 * Reuses the production kboot_display_handoff.c + libfdt harness and synthetic DART/PMGR world
 * from t6041_display_host.c without touching hardware or embedding Apple firmware bytes.
 */
#define main desktop26_main
#include "t6041_display_host.c"
#undef main

static const char J575C[] = "apple,j575c\0apple,t6041\0apple,arm-platform";

#define DUMMY_VRAM      (TB + 0x2c0000000ULL)
#define DUMMY_VRAM_SIZE 0x20000ULL

static void world_headless(bool with_disp_heap, bool bad_disp_pa, bool zero_vram_size)
{
    struct opts opts = {0};
    world_j616s(&opts);
    chip_id = T6041;

    /* Headless internal DCP has no /vram segment in iop-dcp-nub. */
    const char *nub = "/arm-io/dcp/iop-dcp-nub";
    prop_set(nub, "segment-ranges", segs, (NSEGS - 1) * sizeof(segs[0]));
    prop_str(nub, "segment-names", "__TEXT;__DATA;__OS_LOG;unknown;unknown;unknown");

    /* /vram is iBoot's dummy boot framebuffer outside the firmware segments. */
    node_drop("/vram");
    reg_add("/vram", DUMMY_VRAM, zero_vram_size ? 0 : DUMMY_VRAM_SIZE);
    fb_base = DUMMY_VRAM;
    cur_boot_args.video.stride = 4096;
    cur_boot_args.video.height = 16;

    /* Rebuild DART page tables without SEG_VRAM; disp0 maps dummy /vram pages. */
    struct pt pd = {TB + 0x2f0000000ULL, TB + 0x2f0004000ULL};
    struct pt p0 = {TB + 0x2f0100000ULL, TB + 0x2f0104000ULL};
    struct pt pp = {TB + 0x2f0200000ULL, TB + 0x2f0204000ULL};
    memset(mem(pd.lo, 0x20000), 0, 0x20000);
    memset(mem(p0.lo, 0x10000), 0, 0x10000);
    memset(mem(pp.lo, 0x10000), 0, 0x10000);
    for (int i = 0; i < NSEGS - 1; i++) {
        if (i != SEG_OSLOG)
            pt_map(&pd, segs[i].remap - TB, segs[i].phys, seg_size(i));
        if (with_disp_heap && i == SEG_HEAP4)
            pt_map(&p0, segs[i].remap - TB, segs[i].phys, seg_size(i));
        if (PIODMA_SEGS & (1u << i))
            pt_map(&pp, segs[i].remap - TB, segs[i].phys, seg_size(i));
    }
    pt_map(&p0, 0x1a54000, DUMMY_VRAM, 0x8000);
    if (bad_disp_pa)
        pt_map(&p0, 0x5000000, TB + 0x10000000, 0x4000);

    poke32(DART_DCP + 0x1400 + 4 * SID_DCP, ttbr_of(pd.lo));
    poke32(DART_DISP0 + 0x1400 + 4 * SID_DISP, ttbr_of(p0.lo));
    poke32(DART_DISP0 + 0x1400 + 4 * SID_PIODMA, ttbr_of(pp.lo));
}

static void check_ext_published(const void *dt, int which)
{
    char path[40], ph_name[24];
    snprintf(path, sizeof(path), "/dcp@%llx", (unsigned long long)(which ? EXT1 : EXT0));
    int ext = fdt_path_offset(dt, path);
    const fdt32_t *mr = ext < 0 ? NULL : fdt_getprop(dt, ext, "memory-region", NULL);
    check(ext >= 0 && mr && status_is(dt, ext, "disabled"),
          "dcpext%d stays disabled for the display gate", which);
    if (ext < 0 || !mr)
        return;
    int text_reg = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[0]));
    const char *text_name = text_reg < 0 ? NULL : fdt_get_name(dt, text_reg, NULL);
    check(text_name && !strncmp(text_name, "dcp-text@", 9),
          "dcpext%d text points to shared dcp-text@", which);
    snprintf(ph_name, sizeof(ph_name), "dcpext%d-text", which);
    check(fdt_subnode_offset(dt, fdt_path_offset(dt, "/reserved-memory"), ph_name) < 0,
          "dcpext%d text placeholder removed", which);
    for (int j = 1; j < 3; j++) {
        int region = fdt_node_offset_by_phandle(dt, fdt32_ld(&mr[j]));
        check(region >= 0 && status_is(dt, region, "okay"),
              "dcpext%d segment %d enabled", which, j);
    }
}

static void check_headless_sleep_host(const void *dt, bool with_disp_heap)
{
    int dcp = alias_node(dt, "dcp");
    int disp = alias_node(dt, "disp0");
    int pio = alias_node(dt, "disp0-piodma");
    int dart_disp = fdt_path_offset(dt, "/soc/iommu@402300000");

    check(marker_set(dt, dcp) && marker_set(dt, disp) && marker_set(dt, pio),
          "sleep-host: " MARKER " set on dcp, display and piodma");
    check(status_is(dt, dcp, "disabled") && status_is(dt, disp, "disabled") &&
              status_is(dt, dart_disp, "disabled"),
          "sleep-host: dcp, display and disp0 DART stay disabled for kernel gate");
    check(linked_segments(dt, dcp) == (1u << (NSEGS - 1)) - 1 && region_count(dt, dcp) == NSEGS - 1,
          "sleep-host: all 6 headless iop-dcp-nub segments published on dcp");
    check(linked_segments(dt, pio) == PIODMA_SEGS && dvas_match(dt, pio, PIODMA_SEGS),
          "sleep-host: piodma segment published");

    int fb_idx = fdt_stringlist_search(dt, disp, "memory-region-names", "framebuffer");
    int fb = fb_idx >= 0 ? region_at(dt, disp, fb_idx) : -1;
    u64 fb_phys = 0, fb_sz = 0;
    check(fb >= 0 && !fdt_node_check_compatible(dt, fb, "framebuffer") &&
              reg_of(dt, fb, &fb_phys, &fb_sz) && fb_phys == DUMMY_VRAM && fb_sz == 0x10000,
          "sleep-host: display has dummy boot framebuffer memory-region");
    int want_disp_regions = with_disp_heap ? 2 : 1;
    check(region_count(dt, disp) == want_disp_regions &&
              fdt_stringlist_count(dt, disp, "memory-region-names") == want_disp_regions,
          "sleep-host: display memory-region count matches (%d)", want_disp_regions);
    if (with_disp_heap)
        check(segment_region(dt, disp, SEG_HEAP4) >= 0,
              "sleep-host: display kept its DART-mapped heap segment alongside framebuffer");
    check(mmio_writes == 1 && last_write == DART_DCP + 0x200 && (peek32(DART_DCP + 0x200) & 1),
          "sleep-host: dart-dcp locked after validation");
    oslog_check(dt, true);
    check_ext_published(dt, 0);
    check_ext_published(dt, 1);
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    const char *s = argv[1];
    static u8 dt[DT_SIZE], in[DT_SIZE];

    bool with_disp_heap = is(s, "j575c-sleep-host-heap");
    bool bad_disp_pa = is(s, "j575c-sleep-host-bad-disp-pa");
    bool zero_vram = is(s, "j575c-sleep-host-zero-vram");
    world_headless(with_disp_heap, bad_disp_pa, zero_vram);

    if (is(s, "j616c-no-vram")) {
        build_dt(dt, J616C, sizeof(J616C));
    } else {
        build_dt(dt, J575C, sizeof(J575C));
        build_ext(dt, 0);
        build_ext(dt, 1);
        world_ext(0);
        world_ext(1);
        if (!is(s, "j575c-external-only"))
            OK(fdt_setprop_empty(dt, alias_node(dt, "dcp"), "apple,sleep-host-stop"));
    }

    OK(fdt_pack(dt));
    OK(fdt_open_into(dt, dt, DT_SIZE));
    memcpy(in, dt, fdt_totalsize(dt));

    mmio_reads = mmio_writes = 0;
    check(dt_set_display_handoff(dt) == 0, "dt_set_display_handoff() returns 0");
    check(exc_count == 0, "no MMIO fault");

    if (is(s, "j575c-sleep-host") || is(s, "j575c-sleep-host-heap") ||
        is(s, "j575c-sleep-host-repeat")) {
        check_headless_sleep_host(dt, with_disp_heap);
        if (is(s, "j575c-sleep-host-repeat")) {
            static u8 first[DT_SIZE];
            memcpy(first, dt, fdt_totalsize(dt));
            int writes = mmio_writes;
            check(dt_set_display_handoff(dt) == 0 &&
                      fdt_totalsize(dt) == fdt_totalsize(first) &&
                      !memcmp(first, dt, fdt_totalsize(first)) && mmio_writes == writes,
                  "repeat call leaves FDT identical and writes no MMIO");
        }
    } else if (is(s, "j575c-external-only") || is(s, "j575c-sleep-host-bad-disp-pa") ||
               is(s, "j575c-sleep-host-zero-vram")) {
        int dcp = alias_node(dt, "dcp");
        int disp = alias_node(dt, "disp0");
        int dart_disp = fdt_path_offset(dt, "/soc/iommu@402300000");
        check(!marker_set(dt, dcp) && !marker_set(dt, disp),
              "external-only/fallback: no internal handoff marker set");
        check(mmio_writes == 0 && !(peek32(DART_DCP + 0x200) & 1),
              "external-only/fallback: dart-dcp left unlocked");
        check(status_is(dt, disp, "okay") && status_is(dt, dart_disp, "okay"),
              "external-only/fallback: display subsystem and disp0 DART enabled for externals");
        int fb_idx = fdt_stringlist_search(dt, disp, "memory-region-names", "framebuffer");
        check(fb_idx >= 0 && region_at(dt, disp, fb_idx) >= 0,
              "external-only/fallback: display subsystem has framebuffer memory-region");
        check_ext_published(dt, 0);
        check_ext_published(dt, 1);
    } else if (is(s, "j616c-no-vram")) {
        refusal_check(in, dt, true);
    } else {
        printf("FAIL unknown scenario %s\n", s);
        return 2;
    }

    printf("%s: %s\n", s, failures ? "FAIL" : "pass");
    return failures ? 1 : 0;
}
