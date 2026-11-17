/* Drives the production shadow-framebuffer path against the memory model in host.h. */
static unsigned checks;
static void check(bool ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
    checks++;
    fprintf(stdout, "PASS %s\n", name);
}

static u32 expect(u32 p) { return ((p & 0xff) << 2) | (((p >> 8) & 0xff) << 12) | (((p >> 16) & 0xff) << 22); }
static u64 rng = 0x9e3779b97f4a7c15UL;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)rng; }
static void guest_nc_write(u32 row, u32 value) { for (u32 x = 0; x < STRIDE_PX; x++) mem[row * STRIDE_PX + x] = value + x; }
static void guest_wb_write(u32 row, u32 value)
{
    for (u32 x = 0; x < STRIDE_PX; x++) {
        gdirty[row * STRIDE_PX + x] = value + x;
        gdirty_valid[row * STRIDE_PX + x] = true;
    }
}
static bool panel_shows(u32 row, u32 value)
{
    for (u32 x = 0; x < STRIDE_PX; x++)
        if (scanout[row * STRIDE_PX + x] != expect(value + x))
            return false;
    return true;
}
static void full_pass(void) { for (u32 i = 0; i < H; i++) hv_fb_convert_slice(1); }

/* Idle conversion by several cores for `seconds`, each held WFI `gap` apart; returns passes. */
static u64 idle_for(double seconds, u64 gap)
{
    u64 before = hv_fb_cursor, end = clock_now + (u64)(seconds * 1e9);
    while (clock_now < end) {
        for (cpu = 0; cpu < 6; cpu++)
            hv_fb_convert_idle();
        clock_now += gap;
    }
    cpu = 0;
    return (hv_fb_cursor - before) / H;
}

int main(void)
{
    struct boot_args gba = {.video = {.base = (u64)scanout, .stride = STRIDE_PX * 4, .width = W, .height = H, .depth = 30}};
    cur_boot_args = gba;
    for (u32 i = 0; i < STRIDE_PX * H; i++)
        cview[i] = 0xdeadbeef; /* lines cached long before the guest drew anything */

    check(hv_fb_init(&gba), "hv_fb_init accepts the panel");
    check(hv_fb_source == cview && hv_fb_source_cached, "converter reads the write-back alias");
    check(mapping_nc_calls == 1 && hw_map_calls == 1, "identity NC mapping and guest mapping as before");
    check(gba.video.base == (u64)mem && (gba.video.depth & 0xff) == 32, "guest is handed the BGRA shadow");

    /* Identical output to the previous converter, over every row and padding column. */
    for (u32 i = 0; i < STRIDE_PX * H; i++)
        mem[i] = rnd();
    full_pass();
    for (u32 i = 0; i < H; i++)
        old_hv_fb_convert_slice(1);
    check(memcmp(scanout, scanout_old, sizeof(scanout)) == 0, "panel output identical to c470c540's converter");

    /* The alias now holds the pass's lines; the guest draws again, non-cacheable. */
    for (u32 row = 0; row < H; row += 3)
        guest_nc_write(row, 0x00102030 + row);
    full_pass();
    bool fresh = true;
    for (u32 row = 0; row < H; row += 3)
        fresh &= panel_shows(row, 0x00102030 + row);
    check(fresh, "no stale pixel after non-cacheable guest writes");

    /* A guest that maps the buffer write-back: its dirty lines must be cleaned, not dropped. */
    for (u32 row = 1; row < H; row += 4)
        guest_wb_write(row, 0x00405060 + row);
    full_pass();
    bool kept = true;
    for (u32 row = 1; row < H; row += 4)
        kept &= panel_shows(row, 0x00405060 + row);
    check(kept, "write-back guest updates reach the panel (clean, not invalidate)");
    check(civac_calls >= 3 * H + 1, "every converted band is cleaned and invalidated first");

    /* Pacing: six idle cores, a held WFI every 200 us each, for three seconds. */
    idle_for(1.0, 200000); /* reach the schedule first */
    u64 passes = idle_for(3.0, 200000);
    check(passes >= 3 * 60 - 2 && passes <= 3 * 60 + 2, "idle conversion paced at 60 passes a second");
    u64 paced_before = 0;
    for (int c = 0; c < MAX_CPUS; c++)
        paced_before += hv_fb_cost[c].paced;
    check(paced_before > 0, "declined steps are counted");

    /* A five-second busy stretch with no idle core, then one idle second: no catch-up burst. */
    clock_now += 5000000000UL;
    passes = idle_for(1.0, 200000);
    check(passes >= 58 && passes <= 62, "no burst of passes after a busy stretch");

    /* A pending interrupt still ends the idle conversion within one step. */
    clock_now += 1000000000UL; /* the next pass is due */
    hv_fb_cursor = (hv_fb_cursor / H + 1) * H; /* start of a pass, so only the interrupt can stop it early */
    u64 before = hv_fb_cursor;
    isr_pending_at = clock_now + 3 * STRIDE_PX * 4 * HV_FB_SLICE_STEP * maint_cost_per_byte / 2;
    hv_fb_convert_idle();
    u64 lines = hv_fb_cursor - before;
    check(lines == 2 * HV_FB_SLICE_STEP, "an interrupt arriving in the second step stops the idle slice after it");
    isr_pending_at = ~0UL;

    /* The tick's floor is not paced. */
    before = hv_fb_cursor;
    hv_fb_convert_slice(HV_FB_SLICE_TICK);
    check(hv_fb_cursor - before == HV_FB_SLICE_TICK, "tick slice converts unpaced");

    /* The 1 Hz report: unchanged fps line, plus where a line's time went. */
    console_len = 0;
    hv_fb_report_next = 0;
    hv_fb_report();
    unsigned long fps_lines, l, m, c, ns, p;
    char read[8];
    const char *cost = strstr(console, "HV: fb cost: ");
    check(sscanf(console, "HV: fb: %lu lines/s", &fps_lines) == 1, "fps line keeps its format");
    check(cost && sscanf(cost, "HV: fb cost: read=%2s lines=%lu maint_us=%lu copy_us=%lu ns_per_line=%lu paced=%lu",
                         read, &l, &m, &c, &ns, &p) == 6, "cost line format");
    check(!strcmp(read, "wb") && l == fps_lines && ns == STRIDE_PX * 4 * maint_cost_per_byte,
          "cost line: lines match, ns per line is the modeled maintenance");

    /* No alias: the converter falls back to the non-cacheable reads, still correct. */
    alias_available = false;
    hv_fb_shadow = NULL;
    gba.video.base = (u64)scanout;
    gba.video.depth = 30;
    check(hv_fb_init(&gba) && hv_fb_source == mem && !hv_fb_source_cached, "fallback reads the NC mapping");
    unsigned calls = civac_calls;
    guest_nc_write(5, 0x00abcdef);
    hv_fb_cursor = 5;
    hv_fb_convert_slice(1);
    check(panel_shows(5, 0x00abcdef) && civac_calls == calls, "fallback converts without maintenance");

    fprintf(stdout, "CHECKS %u\n", checks);
    return 0;
}
