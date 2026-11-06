# SPDX-License-Identifier: MIT
"""ctxr_exec_confined() decides by its probe alone, on every chip."""

from test_smp_timeouts import REPO, compile_and_run


def probe_source():
    source = (REPO / "src/ctxr_exec.c").read_text()
    start = source.index("static const u32 probe_insns[]")
    end = source.index("\n}\n", source.index("bool ctxr_exec_confined(void)")) + 3
    return source[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t u64; typedef uint32_t u32;
#define MAX_CPUS 4
#define SZ_16K 16384
#define T6040 0x6040
#define T6041 0x6041
#define T8112 0x8112
#define T8140 0x8140
#define T8142 0x8142
enum exc_guard_t { GUARD_OFF = 0, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_TYPE_MASK = 0xff,
                   GUARD_SILENT = 0x100 };
static volatile int exc_guard, exc_count;
static volatile u64 exc_last_esr;
/* Set per case to model each chip; the probe must not read it. */
static int chip_id __attribute__((unused));
static int boot_cpu_idx, alive_mask, probe_faults, call_fails, calls, printed, reads, reads_fault;
static int at_el1;
static bool in_el2(void) { return !at_el1; }
static const char *last_format;
#define SZ_4K 4096
static u64 SYS_IMP_APL_CTXR_D_LWR_EL1, SYS_IMP_APL_CTXR_D_UPR_EL1;
static u64 read_register(u64 value)
{
    reads++;
    assert(exc_guard == (GUARD_MARK | GUARD_SILENT));
    if (reads_fault) { exc_count++; return 0xacce5515abad1dea; }
    return value;
}
#define mrs(reg) read_register(reg)
static void sysop_stub(void) {}
#define sysop(op) sysop_stub()
static bool smp_is_alive(int cpu) { return alive_mask & (1 << cpu); }
static void *memalign(size_t a, size_t s) { return aligned_alloc(a, s); }
static void dc_cvau_range(void *p, size_t s) {}
static void ic_ivau_range(void *p, size_t s) {}
static u64 result;
static int smp_call1(int cpu, void *fn, u64 arg) {
    calls++;
    assert(cpu != boot_cpu_idx && smp_is_alive(cpu));
    assert(exc_guard == (GUARD_RETURN | GUARD_SILENT));
    /* The probe is "mov x0, #1; ret": a secondary that may execute the heap returns 1. A
     * confined one takes an instruction abort, which GUARD_RETURN turns into a return of 0. */
    const u32 *insn = fn;
    assert(insn[0] == 0xd2800020 && insn[1] == 0xd65f03c0);
    if (call_fails)
        return -1;
    if (probe_faults) { exc_last_esr = 0x8600000f; result = 0; } else { result = 1; }
    return 0;
}
static int smp_wait(int cpu, u64 *ret) { *ret = result; return 0; }
#define printf(format, ...) (printed++, last_format = (format))
''' + "%s"

MAIN = r'''
#undef printf
static int run(int chip, int alive, int faults) {
    /* ctxr_exec_confined() caches its answer per boot: model each case as a fresh boot. */
    chip_id = chip; alive_mask = alive; probe_faults = faults; calls = 0; printed = 0;
    call_fails = 0; reads = 0; unprobed_warned = false;
    return ctxr_exec_confined();
}
int main(void) {
    /* M4 Pro / M4 Max / A18 Pro / M5 with the windows armed. */
    int chips[] = { T6040, T6041, T8140, T8142, 0x1234 };
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        assert(run(chips[i], 0x2, 1) == 1 && calls == 1 && printed == 1);
    }
    /* M1 to M3, or any raw boot object: the probe runs and nothing is confined. */
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++)
        assert(run(chips[i], 0x2, 0) == 0 && calls == 1 && printed == 0 && !reads);
    assert(run(T8112, 0xe, 0) == 0 && calls == 1 && !reads);
    /* A probe that never ran is not evidence of confinement: say so, do not refuse. */
    chip_id = T8112; alive_mask = 0x2; probe_faults = 0; calls = 0; printed = 0; call_fails = 1;
    assert(ctxr_exec_confined() == 0 && calls == 1 && printed == 1);
    /* No secondary to ask: not confined, and the log says what CTXR_D reads. */
    SYS_IMP_APL_CTXR_D_LWR_EL1 = 0; SYS_IMP_APL_CTXR_D_UPR_EL1 = 0;
    assert(run(T8112, 0x1, 0) == 0 && calls == 0 && printed == 1 && reads == 2);
    assert(strstr(last_format, "not armed"));
    /* An M1 to M3 has no such registers: the read faults under the guard. */
    reads_fault = 1;
    assert(run(T8112, 0x1, 0) == 0 && calls == 0 && printed == 1);
    assert(strstr(last_format, "not armed"));
    reads_fault = 0;
    /* T6040 in Mach-O mode, or secondaries that failed to start. */
    SYS_IMP_APL_CTXR_D_LWR_EL1 = 0x10000000000; SYS_IMP_APL_CTXR_D_UPR_EL1 = 0x10010000000;
    assert(run(T6040, 0x1, 1) == 0 && calls == 0 && printed == 1);
    assert(strstr(last_format, "WARNING") && strstr(last_format, "armed"));
    assert(exc_guard == GUARD_OFF);
    /* A guest under the hypervisor does not touch them, and has nothing armed to report. */
    at_el1 = 1;
    assert(run(T6040, 0x1, 1) == 0 && calls == 0 && printed == 1 && !reads);
    assert(strstr(last_format, "not armed"));
    at_el1 = 0;
    puts("ctxr exec probe OK");
    return 0;
}
'''

CACHE_MAIN = r'''
#undef printf
int main(void) {
    /* Nothing to ask: warn once, keep no answer. */
    alive_mask = 0x1;
    assert(!ctxr_exec_confined() && !ctxr_exec_confined());
    assert(calls == 0 && printed == 1);
    /* Once a secondary runs, the question gets its answer, and that answer stays. */
    alive_mask = 0x3; probe_faults = 1;
    assert(ctxr_exec_confined() && ctxr_exec_confined());
    assert(calls == 1 && printed == 2);
    alive_mask = 0x1; probe_faults = 0;
    assert(ctxr_exec_confined() && calls == 1);
    puts("ctxr exec probe cache OK");
    return 0;
}
'''

STUCK_MAIN = r'''
#undef printf
int main(void) {
    /* A probe that did not run is not repeated: it could wait out the timeout each time. */
    alive_mask = 0x3; call_fails = 1;
    assert(!ctxr_exec_confined() && !ctxr_exec_confined());
    assert(calls == 1 && printed == 1);
    puts("ctxr exec probe stuck OK");
    return 0;
}
'''


def build(tmp_path, cases_source):
    body = probe_source()
    # Each case is a fresh boot: drop the static cache so one binary can run them all.
    body = body.replace("static int confined = -1;", "int confined = -1;")
    function = body[body.index("bool ctxr_exec_confined(void)"):]
    assert "chip_id" not in function, "ctxr_exec_confined() must not filter by chip ID"
    compile_and_run(tmp_path, "ctxr_exec_probe", HARNESS % body + MAIN)


def test_probe_decides_on_every_chip(tmp_path):
    build(tmp_path, None)


def test_only_a_real_answer_is_cached(tmp_path):
    compile_and_run(tmp_path, "ctxr_exec_probe_cache", HARNESS % probe_source() + CACHE_MAIN)


def test_a_stuck_probe_is_not_repeated(tmp_path):
    compile_and_run(tmp_path, "ctxr_exec_probe_stuck", HARNESS % probe_source() + STUCK_MAIN)


def test_reports_the_fault():
    body = probe_source()
    assert "exc_last_esr = 0;" in body and "ESR 0x%lx" in body


def test_callers_use_the_probe():
    source = (REPO / "src/ctxr_exec.c").read_text()
    assert "if (!ctxr_exec_confined() || ctxr_exec_problem())" in source
    assert "if (!ctxr_exec_confined() || !ctxr_exec_problem())" in source
    assert "if (ctxr_exec_confined()) {" in (REPO / "src/kboot.c").read_text()
