/*
 * Drives the production shadow-framebuffer path, its start-up self-test included, against the
 * memory and CPU model in host.h. argv[1] picks the variant (see `variants` below).
 */
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
static void require(bool ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
}

/* Modeled secondaries: each call runs at once on that CPU's own cache view. */
static u64 call_ret[MAX_CPUS];
static bool call_pending[MAX_CPUS];
static int hang_cpu = -1, fault_cpu = -1;
static bool hung_now;
static unsigned remote_calls, calls_after_hang, calls_to[MAX_CPUS], steps_on[MAX_CPUS];

static int smp_call4(int target, void *func, u64 a0, u64 a1, u64 a2, u64 a3)
{
    (void)a3;
    require(cpu == 0, "smp calls come from the boot CPU");
    if (target <= 0 || target >= MAX_CPUS || !alive[target] || call_pending[target])
        return -1;
    remote_calls++;
    calls_to[target]++;
    calls_after_hang += hung_now;
    bool step = func == (void *)hv_fb_selftest_step;
    if (target == hang_cpu && step && a1 == HV_FB_ST_WRITE) {
        hung_now = true;
        call_pending[target] = true; /* acknowledged, never completes: it may write any time later */
        return 0;
    }
    unsigned caller = cpu;
    view_keep(caller);
    cpu = target;
    view_load(target);
    u64 ret = 0;
    if (step) {
        require(mmu_on[target], "a secondary reads the alias only with its MMU on");
        require(exc_guard == (GUARD_MARK | GUARD_SILENT), "secondary steps run under the exception guard");
        steps_on[target]++;
        if (target == fault_cpu) {
            fault_cpu = -1;
            exc_count++; /* a guarded abort: skipped and counted */
        }
        ret = hv_fb_selftest_step(a0, a1, a2);
        if (a1 != HV_FB_ST_WRITE)
            view_keep(target);
    } else if (func == (void *)hv_fb_selftest_mmu_on) {
        ret = hv_fb_selftest_mmu_on();
    } else if (func == (void *)mmu_disable) {
        ret = mmu_disable();
    } else {
        require(false, "only the self-test's functions run on secondaries");
    }
    cpu = caller;
    view_load(caller);
    call_ret[target] = ret;
    call_pending[target] = true;
    return 0;
}

static int smp_wait_timed(int target, u64 *retval, u32 timeout_ms)
{
    require(timeout_ms <= 2000, "waits on a secondary are bounded");
    if (target <= 0 || target >= MAX_CPUS || !retval || !call_pending[target])
        return -1;
    if (hung_now && target == hang_cpu)
        return SMP_TIMEOUT;
    call_pending[target] = false;
    *retval = call_ret[target];
    return 0;
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

/* The boot CPU holds every line of the alias with `value`: cached long before the guest drew. */
static void stale_boot_lines(u32 value)
{
    for (size_t i = 0; i < WORDS; i++) {
        if (!cache_valid[0][i])
            cache_held[0]++;
        cache_valid[0][i] = true;
        cache_val[0][i] = cview[i] = value;
    }
    holders |= 1;
}

static const struct variant {
    const char *name;
    enum maint_model maint;
    const char *fail; /* expected failure reason; NULL: PASS */
} variants[] = {
    {"normal", MAINT_ALL, NULL},
    {"maintenance-noop", MAINT_NONE, "same core"},
    {"maintenance-not-broadcast", MAINT_LOCAL, "cross-cluster, held there"},
    {"maintenance-cluster-only", MAINT_CLUSTER, "cross-cluster, held there"},
    {"lines-32-bytes", MAINT_LINE32, "same core"},
    {"too-few-cpus", MAINT_ALL, "fewer than two free non-boot CPUs in each of two clusters"},
    {"guest-owned-cpus", MAINT_ALL, NULL},
    {"secondary-mmu-on", MAINT_ALL, NULL},
    {"secondary-fault", MAINT_ALL, "exception"},
    {"secondary-hangs-writing", MAINT_ALL, "secondary call"},
};

static void check_selftest_line(const struct variant *v)
{
    const char *line = strstr(console, "HV: fb cached-read self-test: ");
    check(line && !strstr(line + 1, "HV: fb cached-read self-test: "), "exactly one self-test line");
    const char *end = strchr(line, '\n');
    check(end != NULL, "self-test line is complete");
    fprintf(stdout, "CONSOLE %.*s\n", (int)(end - line), line);
    if (v->fail) {
        char want[160];
        snprintf(want, sizeof(want), "HV: fb cached-read self-test: FAIL (%s) in ", v->fail);
        check(!strncmp(line, want, strlen(want)), "self-test line names the failing case");
        const char *nc = " -- reading the shadow non-cacheable";
        check(end - line > (long)strlen(nc) && !strncmp(end - strlen(nc), nc, strlen(nc)),
              "a failed self-test says the read is NC");
        return;
    }
    char same[8], lab[3][4], cp[3][8], hp[3][8];
    unsigned long us, sb, cb[3], hb[3];
    unsigned kib;
    int n = sscanf(line,
                   "HV: fb cached-read self-test: PASS in %lu us, %u KiB (same %7[a-z]/%lu; "
                   "%3[a-z] checker %7[a-z]/%lu holder %7[a-z]/%lu; %3[a-z] checker %7[a-z]/%lu holder %7[a-z]/%lu; "
                   "%3[a-z] checker %7[a-z]/%lu holder %7[a-z]/%lu)",
                   &us, &kib, same, &sb, lab[0], cp[0], &cb[0], hp[0], &hb[0], lab[1], cp[1], &cb[1], hp[1], &hb[1],
                   lab[2], cp[2], &cb[2], hp[2], &hb[2]);
    check(n == 19 && end[-1] == ')', "self-test PASS line format");
    check(kib == 1024 && us > 0, "self-test covers 1 MiB and reports its time");
    check(!strcmp(same, "stale") && sb == 0, "same core: a stale line was there, the maintained read was exact");
    check(!strcmp(lab[0], "pe") && !strcmp(lab[1], "ep") && !strcmp(lab[2], "ee"),
          "cases: held on P checked on E, held on E checked on P, same cluster");
    check(!cb[0] && !cb[1] && !cb[2] && !hb[0] && !hb[1] && !hb[2], "every cross-core maintained read was exact");
    check(!strcmp(cp[0], "stale") && !strcmp(cp[1], "stale") && !strcmp(cp[2], "stale"),
          "every checker had a stale line to catch");
    check(!strcmp(hp[0], "fresh") && !strcmp(hp[1], "fresh") && !strcmp(hp[2], "fresh"),
          "the checker's maintenance reached the holder's lines");
    unsigned e = 0, p = 0;
    for (unsigned c = 1; c < MAX_CPUS; c++)
        if (steps_on[c] && (model_mpidr[c] & MPIDR_CORE_TYPE_P))
            p++;
        else if (steps_on[c])
            e++;
    check(e == 2 && p == 2, "two E and two P secondaries took part");
}

static void check_left_clean(const bool *mmu_before)
{
    bool zero = true, cached = true, mmu = true;
    for (size_t i = 0; i < WORDS; i++)
        zero &= mem[i] == 0;
    for (size_t i = 0; i < TEST_WORDS; i++) { /* 0xdeadbeef: the boot CPU's lines from before */
        cached &= cview[i] == mem[i] || cview[i] == 0xdeadbeef;
        for (unsigned k = 0; k < MAX_CPUS; k++)
            cached &= !cache_valid[k][i] || cache_val[k][i] == mem[i] || cache_val[k][i] == 0xdeadbeef;
    }
    for (unsigned k = 1; k < MAX_CPUS; k++)
        mmu &= mmu_on[k] == mmu_before[k] && !call_pending[k];
    check(zero, "the shadow is zero in DRAM after the self-test");
    check(cached, "no CPU holds a test pattern of the shadow in its caches");
    check(exc_guard == GUARD_OFF, "exception guard restored");
    check(mmu, "every secondary's MMU is as it was, no call left pending");
}

int main(int argc, char **argv)
{
    const struct variant *v = NULL;
    for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++)
        if (argc > 1 && !strcmp(argv[1], variants[i].name))
            v = &variants[i];
    if (!v) {
        fprintf(stderr, "FAIL unknown variant\n");
        return 2;
    }
    maint_model = v->maint;
    for (unsigned c = 0; c < MAX_CPUS; c++) { /* six E cores (boot CPU 0 among them), then four P */
        model_mpidr[c] = c < 6 ? c : MPIDR_CORE_TYPE_P | (1UL << 8) | (c - 6);
        alive[c] = true;
    }
    if (!strcmp(v->name, "too-few-cpus"))
        for (unsigned c = 2; c < MAX_CPUS; c++)
            alive[c] = c == 6;
    if (!strcmp(v->name, "guest-owned-cpus"))
        hv_started_cpus[1] = hv_setup_pending[6] = true;
    if (!strcmp(v->name, "secondary-mmu-on"))
        mmu_on[2] = true;
    if (!strcmp(v->name, "secondary-fault"))
        fault_cpu = 7;
    if (!strcmp(v->name, "secondary-hangs-writing"))
        hang_cpu = 6;
    bool mmu_before[MAX_CPUS];
    memcpy(mmu_before, mmu_on, sizeof(mmu_before));

    struct boot_args gba = {.video = {.base = (u64)scanout, .stride = STRIDE_PX * 4, .width = W, .height = H, .depth = 30}};
    cur_boot_args = gba;
    stale_boot_lines(0xdeadbeef);

    bool entered = hv_fb_init(&gba);
    check_selftest_line(v);
    if (hang_cpu >= 0) {
        check(!entered && !hv_fb_shadow && gba.video.base == (u64)scanout, "a hung secondary refuses guest entry");
        check(strstr(console, "guest entry refused") != NULL, "the refusal is printed");
        check(hung_now && calls_after_hang == 0, "no call is made after a secondary stops answering");
        check(exc_guard == GUARD_OFF, "exception guard restored");
        fprintf(stdout, "CHECKS %u\n", checks);
        return 0;
    }
    check(entered, "hv_fb_init accepts the panel");
    check_left_clean(mmu_before);
    if (!strcmp(v->name, "too-few-cpus"))
        check(remote_calls == 0, "without enough CPUs no secondary is called");
    if (!strcmp(v->name, "guest-owned-cpus"))
        check(!calls_to[1] && !calls_to[6], "CPUs handed to the guest are never called");
    if (v->fail) {
        check(hv_fb_source == mem && !hv_fb_source_cached && strstr(console, "read non-cacheable"),
              "failed self-test: the converter reads the NC mapping");
        check(setway_flushes == 1 || !strcmp(v->name, "too-few-cpus"), "failed self-test flushes the boot CPU by set/way");
        unsigned calls = civac_calls;
        guest_nc_write(5, 0x00abcdef);
        hv_fb_cursor = 5;
        hv_fb_convert_slice(1);
        check(panel_shows(5, 0x00abcdef) && civac_calls == calls, "NC read converts without maintenance");
        fprintf(stdout, "CHECKS %u\n", checks);
        return 0;
    }
    check(hv_fb_source == cview && hv_fb_source_cached && strstr(console, "read write-back alias"),
          "converter reads the write-back alias");
    check(mapping_nc_calls == 1 && hw_map_calls == 1, "identity NC mapping and guest mapping as before");
    check(gba.video.base == (u64)mem && (gba.video.depth & 0xff) == 32, "guest is handed the BGRA shadow");
    if (strcmp(v->name, "normal")) {
        fprintf(stdout, "CHECKS %u\n", checks);
        return 0;
    }

    stale_boot_lines(0xdeadbeef); /* lines cached again before the guest draws */

    /* Identical output to the previous converter, over every row and padding column. */
    for (u32 i = 0; i < WORDS; i++)
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

    /* No alias: the converter falls back to the non-cacheable reads, still correct, no self-test. */
    alias_available = false;
    hv_fb_shadow = NULL;
    gba.video.base = (u64)scanout;
    gba.video.depth = 30;
    console_len = 0;
    console[0] = 0;
    check(hv_fb_init(&gba) && hv_fb_source == mem && !hv_fb_source_cached, "fallback reads the NC mapping");
    check(!strstr(console, "self-test"), "no alias, no self-test");
    unsigned calls = civac_calls;
    guest_nc_write(5, 0x00abcdef);
    hv_fb_cursor = 5;
    hv_fb_convert_slice(1);
    check(panel_shows(5, 0x00abcdef) && civac_calls == calls, "fallback converts without maintenance");

    fprintf(stdout, "CHECKS %u\n", checks);
    return 0;
}
