/*
 * Drives the production vector bracketing exactly as hv_asm.S does
 * (hv_el2_enter(default) -> handler -> hv_el2_exit) for a busy secondary, an
 * idle secondary and the boot CPU, then checks every reported number against
 * the time the model actually spent.
 */
static unsigned checks;
static void check(bool ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "FAIL %s\n", name);
        exit(1);
    }
    ++checks;
    fprintf(stdout, "PASS %s\n", name); /* the modeled console is captured separately */
}

static u64 truth_ticks[MAX_CPUS][HV_EL2_CAUSES], truth_count[MAX_CPUS][HV_EL2_CAUSES];
static u64 truth_max[MAX_CPUS];
static u64 seen_ticks[MAX_CPUS][HV_EL2_CAUSES], seen_count[MAX_CPUS][HV_EL2_CAUSES];
static u64 seen_held[MAX_CPUS], seen_locks[MAX_CPUS], last_report_time;
static bool have_report;
static size_t report_from;
static unsigned report_calls_from, reports_checked;
static const char *report_label = "";
static struct exc_info ctx;

static u64 us(u64 ticks) { return (ticks / frequency) * 1000000 + ((ticks % frequency) * 1000000) / frequency; }

/* One lower-EL exception, bracketed exactly like hv_asm.S. */
static void stay(unsigned c, u64 vector_cause, unsigned truth_cause, void (*body)(void))
{
    unsigned saved = cpu;
    cpu = c;
    u64 start = clock_now[c], waited = truth_lock_wait[c];
    hv_el2_enter(vector_cause);
    body();
    cpu = c;
    hv_el2_exit();
    u64 length = clock_now[c] - start, wait = truth_lock_wait[c] - waited;
    truth_ticks[c][HV_EL2_LOCK] += wait;
    truth_ticks[c][truth_cause] += length - wait;
    truth_count[c][truth_cause]++;
    if (length > truth_max[c])
        truth_max[c] = length;
    cpu = saved;
}

static void fiq_body(void) { hv_exc_fiq(&ctx); }
static void wfi_body(void) { assert(hv_handle_wfx(&ctx)); }
static u32 site_insn;
static void site_body(void) { hv_el2_site_cause(site_insn); clock_now[cpu] += 150; }
static void bad_cause_body(void) { hv_el2_cause(HV_EL2_CAUSES + 1); hv_el2_cause(HV_EL2_LOCK); clock_now[cpu] += 1000; }
static void plain_body(void) { clock_now[cpu] += 2000; }

/* Parse and verify the lines one report printed against the truth at that instant. */
static void verify_report(void)
{
    static const char *const gate[] = {"guest WFIs held at EL2:", "emulated IRQ traffic (ack/eoi):",
                                       "redirected sync vectors:", "User interrupt", "SYSTEM_OFF",
                                       "All CPUs exited", "now running:", "guest bugcheck"};
    u64 now = clock_now[boot_cpu_idx];
    const char *p = console + report_from;
    unsigned lines = 0;

    check(printf_calls - report_calls_from == MAX_CPUS, "one printf per CPU line, boot CPU included");
    for (int c = 0; c < MAX_CPUS; c++, lines++) {
        const char *end = strchr(p, '\n');
        check(end != NULL && end - p + 1 + 20 < 512, "line ends and fits the 512-byte printf buffer");
        char line[512];
        memcpy(line, p, end - p + 1);
        line[end - p + 1] = 0;
        for (unsigned g = 0; g < sizeof(gate) / sizeof(gate[0]); g++)
            check(!strstr(line, gate[g]), "line carries no substring the health gate parses");
        int id;
        unsigned long win, el2, max, n[HV_EL2_CAUSES], t[HV_EL2_CAUSES], hold;
        int fields = sscanf(line,
                            "HV: el2 cpu%d win=%lu el2=%lu max=%lu tick=%lu:%lu irq=%lu:%lu abs=%lu:%lu "
                            "vec=%lu:%lu sync=%lu:%lu wfi=%lu:%lu lock=%lu:%lu hold=%lu\n",
                            &id, &win, &el2, &max, &n[0], &t[0], &n[1], &t[1], &n[2], &t[2], &n[3],
                            &t[3], &n[4], &t[4], &n[5], &t[5], &n[6], &t[6], &hold);
        check(fields == 19 && id == c, "stable line format, CPUs in order");
        check(win == us(now - last_report_time), "window is the time since the previous report");
        check(win > 999000 && win < 1001000, "a one-second window reads as ~1000000 us at this counter rate");
        u64 total = 0;
        for (int k = 0; k < HV_EL2_CAUSES; k++) {
            u64 dt = truth_ticks[c][k] - seen_ticks[c][k];
            u64 dn = k == HV_EL2_LOCK ? truth_locks[c] - seen_locks[c] : truth_count[c][k] - seen_count[c][k];
            total += dt;
            if (t[k] != us(dt) || n[k] != dn) {
                fprintf(stderr, "%s cpu%d cause %d: reported %lu:%lu, modeled %lu:%lu\n%s", report_label,
                        c, k, n[k], t[k], (unsigned long)dn, (unsigned long)us(dt), line);
                check(false, "every bucket matches modeled time and count");
            }
            seen_ticks[c][k] = truth_ticks[c][k];
            seen_count[c][k] = truth_count[c][k];
        }
        check(el2 == us(total), "el2 is the sum of the exclusive buckets");
        check(max == us(truth_max[c]), "max is the longest stay that ended in the window");
        check(hold == us(truth_held[c] - seen_held[c]), "hold is the time the CPU held bhl");
        seen_held[c] = truth_held[c];
        seen_locks[c] = truth_locks[c];
        truth_max[c] = 0;
        p = end + 1;
    }
    check(*p == 0 && lines == MAX_CPUS, "nothing else printed");
    last_report_time = now;
    reports_checked++;
}

static void report_hook(void)
{
    if (!have_report) {
        check(printf_calls == report_calls_from, "first report only takes the baseline");
        have_report = true;
        last_report_time = clock_now[boot_cpu_idx];
        for (int c = 0; c < MAX_CPUS; c++) {
            memcpy(seen_ticks[c], truth_ticks[c], sizeof(seen_ticks[c]));
            memcpy(seen_count[c], truth_count[c], sizeof(seen_count[c]));
            seen_held[c] = truth_held[c];
            seen_locks[c] = truth_locks[c];
            truth_max[c] = 0;
        }
        return;
    }
    verify_report();
}

/* The boot CPU's 1 Hz housekeeping FIQ: slow path, bhl, hv_tick -> report. */
static void boot_housekeeping(u64 at, const char *label)
{
    report_label = label;
    clock_now[boot_cpu_idx] = at;
    report_from = console_len;
    report_calls_from = printf_calls;
    after_report = report_hook;
    stay(boot_cpu_idx, HV_EL2_IRQ, HV_EL2_TICK, fiq_body);
    after_report = NULL;
}

/* A busy secondary: guest never idles; only host-timer FIQs, every tenth on the bhl slow path. */
static void busy_secondary(unsigned c, u64 until, u64 contention)
{
    unsigned i = 0;
    cpu = c;
    while (expires[c] + 100000 <= until) {
        clock_now[c] = expires[c];
        if (++i % 10 == 0) {
            registers[c][ISR_EL1] = 0x40; /* a FIQ still pending: hv_exc_fiq takes the slow path */
            lock_contention = contention;
        }
        stay(c, HV_EL2_IRQ, HV_EL2_TICK, fiq_body);
        registers[c][ISR_EL1] = 0;
        if (i % 100 == 0) {
            static const struct { u32 insn; unsigned cause; } sites[] = {
                {0xd53b9d08, HV_EL2_ABS}, /* mrs x8, pmccntr_el0 (boot 143's hottest site) */
                {0xd538cc09, HV_EL2_IRQ}, /* mrs x9, icc_iar1_el1 */
                {0xd518cc28, HV_EL2_IRQ}, /* msr icc_eoir1_el1, x8 */
            };
            for (unsigned s = 0; s < 3; s++) {
                clock_now[c] += 1000;
                site_insn = sites[s].insn;
                stay(c, HV_EL2_SYNC, sites[s].cause, site_body);
            }
        }
    }
}

/* An idle secondary: guest WFI held at EL2, woken by CNTHP, which then arrives as a tick FIQ. */
static void idle_secondary(unsigned c, u64 until)
{
    cpu = c;
    while (clock_now[c] + frequency / 20 <= until) { /* leaves room for the straddling WFI below */
        clock_now[c] += 50000;
        stay(c, HV_EL2_SYNC, HV_EL2_WFI, wfi_body);
        stay(c, HV_EL2_IRQ, HV_EL2_TICK, fiq_body);
    }
}

static u64 straddle_report_at;
static void report_during_wfi(void)
{
    unsigned c = cpu;
    during_wfi = NULL;
    check(clock_now[c] < straddle_report_at && expires[c] > straddle_report_at,
          "idle CPU is inside a WFI stay when the boot CPU reports");
    boot_housekeeping(straddle_report_at, "report during WFI");
    cpu = c;
}

static int run(u64 freq)
{
    frequency = freq;
    cost_fb_line = freq * 13 / 100000; /* boot 144: ~130 us per scanline from the NC shadow */
    u64 second = freq, t0 = 5 * freq;
    hv_tick_interval = second;
    hv_secondary_tick_interval = second / 5000; /* hv_init's T8142 secondary value before the fix */
    hv_wfi_wake_interval = second / 1000;
    for (int c = 0; c < MAX_CPUS; c++)
        clock_now[c] = t0;

    cpu = boot_cpu_idx;
    hv_arm_tick(false);
    report_from = console_len;
    report_calls_from = printf_calls;
    hv_report_t8142_el2_acct(); /* baseline, outside any stay */
    report_hook();
    check(printf_calls == 0, "baseline report printed nothing");

    for (int c = 0; c < MAX_CPUS; c++)
        if (c != boot_cpu_idx) {
            cpu = c;
            hv_arm_tick(true);
        }

    /* Window 1: busy cpu7 with bhl contention, idle cpu3 ending in a WFI across the report. */
    u64 w1 = t0 + second;
    busy_secondary(7, w1, freq / 50000); /* 20 us wait on each slow-path acquisition */
    idle_secondary(3, w1);
    cpu = 3;
    check(clock_now[3] <= w1 - HV_FB_SLICE_IDLE * cost_fb_line - freq / 100000, "straddle starts after the idle stream");
    clock_now[3] = w1 - HV_FB_SLICE_IDLE * cost_fb_line - freq / 100000;
    hv_periodic_deadlines[3].next = w1 + freq / 6667; /* liveness deadline ~150 us after the report */
    straddle_report_at = w1;
    during_wfi = report_during_wfi;
    stay(3, HV_EL2_SYNC, HV_EL2_WFI, wfi_body);
    check(during_wfi == NULL && reports_checked == 1, "window-1 report ran inside cpu3's WFI");
    stay(3, HV_EL2_IRQ, HV_EL2_TICK, fiq_body);

    /* Window 2: the straddling WFI and the boot CPU's own report stay land here. */
    u64 w2 = w1 + second;
    busy_secondary(7, w2, 0);
    idle_secondary(3, w2);
    boot_housekeeping(w2, "window 2");

    /* A second report inside the same half-second must print nothing. */
    unsigned calls = printf_calls;
    cpu = boot_cpu_idx;
    clock_now[boot_cpu_idx] += second / 10;
    hv_report_t8142_el2_acct();
    check(printf_calls == calls, "flood guard: no second report within half a second");

    /* Window 3: classification edges. */
    u64 w3 = w2 + second;
    stay(1, HV_EL2_SYNC, HV_EL2_SYNC, bad_cause_body);
    stay(1, HV_EL2_CAUSES + 2, HV_EL2_SYNC, plain_body);
    stay(1, HV_EL2_LOCK, HV_EL2_SYNC, plain_body);
    static const struct { u32 insn; unsigned cause; } table[] = {
        {0xd53b9d0b, HV_EL2_ABS}, /* mrs x11, pmccntr_el0 */
        {0xd53be040, HV_EL2_ABS}, /* mrs x0, cntvct_el0 */
        {0xd538c000, HV_EL2_ABS}, /* mrs x0, vbar_el1: CRn=12 but not the CPU interface */
        {0xd538c100, HV_EL2_ABS}, /* mrs x0, isr_el1 */
        {0xd518cbaa, HV_EL2_IRQ}, /* msr icc_sgi1r_el1, x10 */
        {0xd538cca0, HV_EL2_IRQ}, /* mrs x0, icc_sre_el1 */
        {0xd518cce0, HV_EL2_IRQ}, /* msr icc_igrpen1_el1, x0 */
        {0xd538c800, HV_EL2_IRQ}, /* mrs x0, icc_iar0_el1 */
        {0xd538c900, HV_EL2_IRQ}, /* mrs x0, icc_ap1r0_el1 */
        {0xd5184600, HV_EL2_IRQ}, /* msr icc_pmr_el1, x0 */
        {0xd5384600, HV_EL2_IRQ}, /* mrs x0, icc_pmr_el1 */
        {0xd5384620, HV_EL2_ABS}, /* S3_0_C4_C6_1: next to PMR, not PMR */
    };
    for (unsigned s = 0; s < sizeof(table) / sizeof(table[0]); s++) {
        site_insn = table[s].insn;
        stay(2, HV_EL2_SYNC, table[s].cause, site_body);
    }
    /* Window 3b: an interrupt arrives mid-slice; the held WFI must end within one step. */
    u64 before = fb_lines_converted;
    clock_now[4] += freq / 1000;
    u64 arrive = clock_now[4] + 7 * cost_fb_line + cost_fb_line / 3;
    pending_at[4] = arrive;
    stay(4, HV_EL2_SYNC, HV_EL2_WFI, wfi_body);
    u64 lines = fb_lines_converted - before;
    check(clock_now[4] >= arrive && clock_now[4] - arrive <= HV_FB_SLICE_STEP * cost_fb_line,
          "an interrupt pending mid-slice ends the held WFI within one conversion step");
    check(lines > 0 && lines < HV_FB_SLICE_IDLE && lines % HV_FB_SLICE_STEP == 0,
          "conversion stops early, on a step boundary, with lines counted");
    pending_at[4] = ~0UL;
    before = fb_lines_converted;
    stay(4, HV_EL2_SYNC, HV_EL2_WFI, wfi_body);
    check(fb_lines_converted - before == HV_FB_SLICE_IDLE, "with nothing pending the whole idle slice is converted");
    pending_at[4] = clock_now[4];
    before = fb_lines_converted;
    u64 at = clock_now[4];
    stay(4, HV_EL2_SYNC, HV_EL2_WFI, wfi_body);
    check(fb_lines_converted == before && clock_now[4] == at, "an interrupt pending before the WFI: no conversion, no wait");
    pending_at[4] = ~0UL;
    boot_housekeeping(w3, "window 3");

    check(reports_checked == 3, "three full reports verified");
    fputs("CONSOLE\n", stdout);
    fwrite(console, 1, console_len, stdout);
    fprintf(stdout, "CHECKS %u\n", checks);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "24mhz"))
        return run(24000000);
    return run(1000000000);
}
