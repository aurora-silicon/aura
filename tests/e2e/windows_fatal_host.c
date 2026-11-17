/* SPDX-License-Identifier: MIT */
/*
 * Host replay of the T8152 fatal-exception context, src/hv/windows/hv_fatal.c, compiled
 * unchanged (it is #included below) against stub headers that
 * tests/python/test_windows_t8152_serror.py writes: types.h, smp.h, soc.h, utils.h and an
 * hv_ahci.h that records the AHCI report call.  System registers are plain variables; a
 * DISR_EL1 read on a CPU without FEAT_RAS fails the run, as it would UNDEF on hardware.
 *
 * Failure modes this covers (windows-m6/D3-RESULTS.md section 4):
 *  R1 the report prints on another chip, or before a guest was entered;
 *  R2 a fault inside the report re-enters it (endless nested exceptions instead of a reboot);
 *  R3 the frame walk leaves the stack, follows a loop or a misaligned pointer;
 *  R4 a record copied while its owner rewrites it is printed as if it were stable;
 *  R5 DISR_EL1 is read without FEAT_RAS (UNDEF inside the SError handler);
 *  R6 a stage marker is written before its fence, so an SError the previous stage caused is
 *     blamed on the next one; the converter record is opened before its fence (an error
 *     pending from the idle path's trap handling blamed on the band) or closed before it;
 *  R7 HPFAR is recorded for exits it does not describe; the IPA is decoded wrongly;
 *  R8 the D3 run 1 syndrome (ESR 0xbe000000) is classified as containable.
 * The report text itself is printed on stdout between "report-begin"/"report-end" markers for
 * the Python side to check; every case ends with one JSON line.
 * Usage: windows_fatal_host
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hv_fatal.c"

/* ---- stubs ----------------------------------------------------------------------------- */

u32 chip_id = T8152;
int stub_cpu;
u64 stub_par, stub_hpfar, stub_cntpct = 1000000, stub_cntfrq = 24000000, stub_esr, stub_far;
u64 stub_isr, stub_pfr0, stub_disr, stub_elr12, stub_esr12, stub_far12, stub_spsr12;
char stub_base_storage[16];
int stub_ahci_reports;
u64 stub_crumbs[MAX_CPUS];
static int fences;
static int bad_disr_reads;
/* hv_fatal_stage() must fence before it writes: what the stage was at each fence. */
static u64 stage_at_fence[64];
static u64 fb_seq_at_fence[64];

void stub_sysop(const char *op)
{
    if (!strcmp(op, "dsb sy") && fences < 64) {
        stage_at_fence[fences] = hv_fatal_cpus[stub_cpu].stage;
        fb_seq_at_fence[fences] = hv_fatal_cpus[stub_cpu].fb.seq;
        fences++;
    }
}

u64 stub_read_disr(void)
{
    if (!((stub_pfr0 >> 28) & 0xf))
        bad_disr_reads++;
    return stub_disr;
}

void hv_ahci_fatal_report(u64 now, u64 freq)
{
    (void)now;
    (void)freq;
    stub_ahci_reports++;
    printf("HV: fatal: AHCI stub report\n");
}

/* D4: the USB part of the report (hv_usb.c); recorded values only, nothing to model here. */
void hv_usb_fatal_report(void)
{
    printf("HV: fatal: USB stub report\n");
}

u64 hv_wdt_breadcrumbs_read(int cpu)
{
    return stub_crumbs[cpu];
}

/* ---- harness --------------------------------------------------------------------------- */

static int failures;
static const char *current;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d [%s] CHECK(%s)\n", __FILE__, __LINE__, current, #cond);         \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

#define EQ(a, b)                                                                                   \
    do {                                                                                           \
        unsigned long long _a = (unsigned long long)(a), _b = (unsigned long long)(b);             \
        if (_a != _b) {                                                                            \
            fprintf(stderr, "%s:%d [%s] %s = 0x%llx, want 0x%llx\n", __FILE__, __LINE__, current,  \
                    #a, _a, _b);                                                                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

static void begin(const char *name)
{
    current = name;
    fflush(stdout);
}

static void end(int before)
{
    fflush(stdout);
    printf("{\"case\": \"%s\", \"result\": \"%s\"}\n", current,
           failures == before ? "passed" : "failed");
}

static void reset_state(void)
{
    memset(hv_fatal_cpus, 0, sizeof(hv_fatal_cpus));
    hv_fatal_armed = false;
    hv_fatal_reporting = 0;
    chip_id = T8152;
    stub_cpu = 0;
    fences = 0;
    stub_ahci_reports = 0;
}

/* ---- cases ----------------------------------------------------------------------------- */

/* R1: silent off T8152 and before entry; the writers record nothing off T8152. */
static void case_gating(void)
{
    int before = failures;
    begin("gating");
    reset_state();
    u64 regs[32] = {0};

    chip_id = 0x8142;
    hv_fatal_armed = true;
    hv_fatal_note_trap(HV_FATAL_TRAP_SYNC, 0x93800000, 0x1234, 0x5, 0);
    hv_fatal_stage(HV_FATAL_STAGE_FB);
    hv_fatal_fb_begin(0, 64);
    hv_fatal_fb_end();
    EQ(hv_fatal_cpus[0].trap.seq, 0);
    EQ(hv_fatal_cpus[0].stage, 0);
    EQ(hv_fatal_cpus[0].fb.seq, 0);
    EQ(fences, 0);
    hv_windows_fatal_report(regs, "SError at EL2", 0, "at entry");
    EQ(stub_ahci_reports, 0);
    EQ(hv_fatal_reporting, 0);

    chip_id = T8152;
    hv_fatal_armed = false;
    hv_windows_fatal_report(regs, "SError at EL2", 0, "at entry");
    EQ(stub_ahci_reports, 0);
    EQ(hv_fatal_reporting, 0);
    end(before);
}

/* R2: the second entry (a fault inside the report) prints one line and returns. */
static void case_reentry(void)
{
    int before = failures;
    begin("reentry");
    reset_state();
    hv_fatal_armed = true;
    printf("report-begin reentry\n");
    hv_windows_fatal_report(NULL, "SError at EL2", 0, "at entry");
    hv_windows_fatal_report(NULL, "EL2 synchronous exception", 0, "at entry");
    printf("report-end reentry\n");
    EQ(stub_ahci_reports, 1);
    end(before);
}

/* R3: the frame walk stays on the stack and stops on anything malformed. */
static void case_unwind(void)
{
    int before = failures;
    begin("unwind");
    static u64 stack[512] __attribute__((aligned(16)));
    u64 lo = (u64)stack, hi = (u64)(stack + 512);
    u64 out[16];

    /* A chain of four records, upward, then a terminating record (fp 0). */
    u64 *r0 = &stack[20], *r1 = &stack[60], *r2 = &stack[100], *r3 = &stack[140];
    r0[0] = (u64)r1, r0[1] = 0x1000;
    r1[0] = (u64)r2, r1[1] = 0x2000;
    r2[0] = (u64)r3, r2[1] = 0x3000;
    r3[0] = 0, r3[1] = 0x4000;
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 16), 4);
    EQ(out[0], 0x1000);
    EQ(out[3], 0x4000);
    /* The cap. */
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 2), 2);
    /* A loop back down stops after the record that points down. */
    r2[0] = (u64)r1;
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 16), 3);
    r2[0] = (u64)r3;
    /* Out of the window: below, above, the last 8 bytes, or a pointer off the stack. */
    EQ(hv_fatal_unwind(lo - 16, lo, hi, out, 16), 0);
    EQ(hv_fatal_unwind(hi, lo, hi, out, 16), 0);
    EQ(hv_fatal_unwind(hi - 8, lo, hi, out, 16), 0);
    r1[0] = 0xdead0000;
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 16), 2);
    /* Misaligned. */
    r1[0] = (u64)r2 + 4;
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 16), 2);
    EQ(hv_fatal_unwind((u64)r0 + 1, lo, hi, out, 16), 0);
    /* A zero return address ends the chain. */
    r1[0] = (u64)r2;
    r1[1] = 0;
    EQ(hv_fatal_unwind((u64)r0, lo, hi, out, 16), 1);
    end(before);
}

/* R4: a reader racing a writer never accepts a mixed copy as stable. */
struct rec {
    u64 seq;
    u64 a, b, c, d;
};
static struct rec shared;
static volatile int stop_writer;

static void *writer(void *arg)
{
    (void)arg;
    for (u64 v = 1; !stop_writer; v++) {
        hv_fatal_seq_begin(&shared.seq);
        shared.a = v;
        shared.b = v;
        shared.c = v;
        shared.d = v;
        hv_fatal_seq_end(&shared.seq);
    }
    return NULL;
}

static void case_seqlock(void)
{
    int before = failures;
    begin("seqlock");
    struct rec copy;

    /* Quiet record: stable and even. */
    memset(&shared, 0, sizeof(shared));
    hv_fatal_seq_begin(&shared.seq);
    shared.a = shared.b = shared.c = shared.d = 7;
    hv_fatal_seq_end(&shared.seq);
    EQ(hv_fatal_read(&shared.seq, &shared, &copy, sizeof(copy)), 2);
    EQ(copy.d, 7);
    /* An owner stopped inside its record (the CPU that took the SError): odd, reported. */
    hv_fatal_seq_begin(&shared.seq);
    EQ(hv_fatal_read(&shared.seq, &shared, &copy, sizeof(copy)), 3);
    hv_fatal_seq_end(&shared.seq);

    pthread_t t;
    stop_writer = 0;
    pthread_create(&t, NULL, writer, NULL);
    u64 stable = 0, mixed = 0, unstable = 0;
    for (int i = 0; i < 2000000; i++) {
        u64 s = hv_fatal_read(&shared.seq, &shared, &copy, sizeof(copy));
        if (s == ~0UL || (s & 1)) {
            unstable++;
            continue;
        }
        stable++;
        if (copy.a != copy.b || copy.b != copy.c || copy.c != copy.d)
            mixed++;
    }
    stop_writer = 1;
    pthread_join(t, NULL);
    EQ(mixed, 0);
    CHECK(stable > 0);
    printf("seqlock: %llu stable, %llu busy or changing, %llu mixed\n", (unsigned long long)stable,
           (unsigned long long)unstable, (unsigned long long)mixed);
    end(before);
}

/* R5, R7, R8 and the full report on a state shaped like D3 run 1's (synthetic values: a
 * PAR inside the synthetic guest window 0x10100000000:0x10561600000, made-up kernel and user
 * VAs, a FAR that keeps the PxCI offset 0x138; nothing here was read from the run). */
static void case_report(int ras)
{
    int before = failures;
    begin(ras ? "report_ras" : "report_d3");
    reset_state();
    hv_fatal_armed = true;
    bad_disr_reads = 0;
    stub_pfr0 = ras ? (1UL << 28) : 0;
    stub_disr = 0x80000000;

    /* cpu 3: an AHCI PxCI store, a stage-2 data abort with HPFAR. */
    stub_cpu = 3;
    stub_cntpct = 1000000;
    stub_hpfar = (0xf40000000UL >> 12) << 4;
    hv_fatal_note_trap(HV_FATAL_TRAP_SYNC, 0x93c08046UL, 0xfffff80000101230UL, 0x60000005,
                       0xffffa50012400138UL);
    EQ(hv_fatal_cpus[3].trap.hpfar, stub_hpfar);
    /* cpu 7: an HVC (not an abort): HPFAR is not recorded. */
    stub_cpu = 7;
    hv_fatal_note_trap(HV_FATAL_TRAP_SYNC, 0x5a000000UL, 0xfffff80000204560UL, 0x60000005, 0x1234);
    EQ(hv_fatal_cpus[7].trap.hpfar, 0);
    hv_fatal_note_trap(HV_FATAL_TRAP_FIQ_FAST, 0x5a000000UL, 0x7ff600000000UL, 0x0, 0x1234);
    EQ(hv_fatal_cpus[7].trap.hpfar, 0);

    /* cpu 6: the tick, interrupting EL0; converter band 1800..1864 done; stage R. */
    stub_cpu = 6;
    stub_cntpct = 1000000 + 24000; /* 1 ms later */
    hv_fatal_note_trap(HV_FATAL_TRAP_FIQ, 0x93c08046UL, 0x7ff600001230UL, 0x20000040,
                       0xffffa50012400138UL);
    fences = 0;
    hv_fatal_stage(HV_FATAL_STAGE_FB);
    hv_fatal_fb_begin(1800, 1864);
    EQ(hv_fatal_cpus[6].fb.seq & 1, 1);
    hv_fatal_fb_end();
    EQ(hv_fatal_cpus[6].fb.seq & 1, 0);
    hv_fatal_stage(HV_FATAL_STAGE_REPORT);
    hv_fatal_stage(HV_FATAL_STAGE_RECOVER);
    /* R6: each stage's fence ran while the previous stage was still recorded; the converter
     * fenced once before opening its record (still closed) and once before closing it (still
     * open). */
    EQ(fences, 5);
    EQ(stage_at_fence[0], 0);
    EQ(stage_at_fence[1], HV_FATAL_STAGE_FB);
    EQ(fb_seq_at_fence[1] & 1, 0);
    EQ(fb_seq_at_fence[2] & 1, 1);
    EQ(stage_at_fence[2], HV_FATAL_STAGE_FB);
    EQ(stage_at_fence[3], HV_FATAL_STAGE_FB);
    EQ(stage_at_fence[4], HV_FATAL_STAGE_REPORT);
    EQ(hv_fatal_cpus[6].stage, HV_FATAL_STAGE_RECOVER);
    stub_crumbs[6] = 0x58784658UL; /* "XxFX" */

    /* cpu 9: a band still in progress on the idle path; its begin fenced before opening. */
    stub_cpu = 9;
    int f9 = fences;
    hv_fatal_fb_begin(64, 224);
    EQ(fences, f9 + 1);
    EQ(fb_seq_at_fence[f9] & 1, 0);

    /* The SError on cpu 6: the D3 run 1 syndrome (ESR 0xbe000000), synthetic addresses. */
    stub_cpu = 6;
    stub_cntpct += 2400; /* 100 us */
    stub_esr = 0xbe000000UL;
    stub_far = 0xffffa50012400138UL;
    stub_hpfar = (0xf40000000UL >> 12) << 4;
    stub_isr = 0;
    stub_elr12 = 0xfffff80000204560UL;
    stub_spsr12 = 0x3c5;
    u64 par = ras ? 0x2b : 0xff00010123456b80UL; /* F=1 FST 0x15 / F=0, a PA in the synthetic window */

    static u64 stack[256] __attribute__((aligned(16)));
    u64 *regs = stack;
    u64 *frame = &stack[100];
    frame[0] = 0;
    frame[1] = (u64)stub_base_storage + 0x40a58;
    regs[29] = (u64)frame;
    regs[30] = (u64)stub_base_storage + 0x40124;

    printf("report-begin %s\n", current);
    hv_windows_fatal_report(regs, "SError at EL2", par, "at entry");
    printf("report-end %s\n", current);
    EQ(bad_disr_reads, 0);
    EQ(stub_ahci_reports, 1);

    /* R8 and the decoders. */
    CHECK(strstr(hv_fatal_serror_class(0xbe000000UL), "no architectural classification"));
    CHECK(strstr(hv_fatal_serror_class(0xbe000000UL | BIT(24)), "IDS=1"));
    CHECK(strstr(hv_fatal_serror_class(0xbe000011UL | (6UL << 10)), "corrected"));
    CHECK(strstr(hv_fatal_serror_class(0xbe000011UL), "uncontainable"));
    CHECK(strstr(hv_fatal_serror_class(0x96000000UL), "not an SError"));
    CHECK(!strcmp(hv_fatal_fst_name(0x15), "synchronous external abort on the table walk"));
    CHECK(!strcmp(hv_fatal_fst_name(0x07), "translation fault"));
    CHECK(!strcmp(hv_fatal_fst_name(0x0d), "permission fault"));
    CHECK(!strcmp(hv_fatal_fst_name(0x10), "synchronous external abort"));
    end(before);
}

/*
 * R9 (i0 session 2): a guest SError.  The previous exit of the CPU is kept (the SError's own
 * note overwrites it), set against the SError's PC, and every CPU's records follow; the report
 * latch is released afterwards, so a later EL2 fatal still reports.  Synthetic values.
 */
static void case_guest(void)
{
    int before = failures;
    begin("report_guest");
    reset_state();
    hv_fatal_armed = true;
    stub_pfr0 = 0;
    struct hv_fatal_trap prev;

    /* Relation decoder: return points of each exit kind. */
    struct hv_fatal_trap t = {.kind = HV_FATAL_TRAP_SYNC, .esr = 0x04000001UL /* WFI */,
                              .elr = 0x1000, .cntpct = 1};
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1004, 5), "taken at that exit's return point: "
                                                          "pending when EL2 returned"));
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1000, 5), "the guest ran on"));
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1004, 20000), "not attributable"));
    t.esr = 0x5a000000UL; /* HVC: ELR_EL2 is already past it */
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1000, 5), "return point"));
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1004, 5), "the guest ran on"));
    t.kind = HV_FATAL_TRAP_IRQ;
    t.esr = 0;
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1000, 5), "return point"));
    t.isr = BIT(8);
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1000, 5), "already pending when it exited"));
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x2000, 5), "already pending at it"));
    CHECK(strstr(hv_fatal_serror_relation(NULL, 0x1000, 5), "no earlier exit"));
    t.cntpct = 0;
    CHECK(strstr(hv_fatal_serror_relation(&t, 0x1000, 5), "no earlier exit"));

    /* cpu 0: some other exit, so the all-CPU part has a second CPU. */
    stub_cpu = 0;
    stub_cntpct = 1000000;
    stub_isr = 0;
    hv_fatal_note_trap(HV_FATAL_TRAP_IRQ, 0, 0xfffff80000300000UL, 0x60000005, 0);

    /* cpu 6: a WFI exit (the idle path) with an SError already pending, then the SError. */
    stub_cpu = 6;
    stub_isr = BIT(8);
    hv_fatal_note_trap(HV_FATAL_TRAP_SYNC, 0x04000001UL, 0xfffff80000101000UL, 0x60000005, 0);
    /* ...and an idle-path band after it that ended with the SError pending. */
    hv_fatal_fb_begin(400, 464);
    hv_fatal_fb_end();
    EQ(hv_fatal_cpus[6].fb.isr, BIT(8));
    stub_isr = 0;
    hv_fatal_trap_snapshot(&prev);
    EQ(prev.kind, HV_FATAL_TRAP_SYNC);
    EQ(prev.elr, 0xfffff80000101000UL);
    EQ(prev.isr, BIT(8));
    stub_cntpct += 24 * 7; /* 7 us */
    hv_fatal_note_trap(HV_FATAL_TRAP_SERROR, 0xbe000000UL, 0xfffff80000101004UL, 0x60000005,
                       0xffffa50012400138UL);
    stub_esr = 0xbe000000UL;
    u64 regs[32];
    for (int i = 0; i < 32; i++)
        regs[i] = 0x1111111111111111UL * (u64)(i % 16);

    printf("report-begin %s\n", current);
    hv_windows_guest_serror_report(regs, 0xbe000000UL, 0xfffff80000101004UL, 0x60000005,
                                   0xffffa50012400138UL, &prev, 0);
    printf("report-end %s\n", current);
    EQ(stub_ahci_reports, 1);
    EQ(hv_fatal_reporting, 0);

    /* Off T8152, or before entry: nothing; the snapshot is empty off T8152. */
    chip_id = 0x8142;
    hv_fatal_trap_snapshot(&prev);
    EQ(prev.cntpct, 0);
    hv_windows_guest_serror_report(regs, 0, 0, 0, 0, &prev, 0);
    chip_id = T8152;
    hv_fatal_armed = false;
    hv_windows_guest_serror_report(regs, 0, 0, 0, 0, &prev, 0);
    EQ(stub_ahci_reports, 1);
    end(before);
}

int main(void)
{
    case_gating();
    case_reentry();
    case_unwind();
    case_seqlock();
    case_report(0);
    case_report(1);
    case_guest();
    return failures ? 1 : 0;
}
