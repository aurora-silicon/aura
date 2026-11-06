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
static volatile int exc_guard;
static volatile u64 exc_last_esr;
/* Set per case to model each chip; the probe must not read it. */
static int chip_id __attribute__((unused));
static int boot_cpu_idx, alive_mask, probe_faults, call_fails, calls, printed;
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
#define printf(...) (printed++)
''' + "%s" + r'''
#undef printf
static int run(int chip, int alive, int faults) {
    /* ctxr_exec_confined() caches its answer per boot: model each case as a fresh boot. */
    chip_id = chip; alive_mask = alive; probe_faults = faults; calls = 0; printed = 0;
    call_fails = 0;
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
        assert(run(chips[i], 0x2, 0) == 0 && calls == 1 && printed == 0);
    assert(run(T8112, 0xe, 0) == 0 && calls == 1);
    /* A probe that never ran is not evidence of confinement: say so, do not refuse. */
    chip_id = T8112; alive_mask = 0x2; probe_faults = 0; calls = 0; printed = 0; call_fails = 1;
    assert(ctxr_exec_confined() == 0 && calls == 1 && printed == 1);
    /* No secondary to ask, so nothing to confine. */
    assert(run(T6040, 0x1, 1) == 0 && calls == 0);
    puts("ctxr exec probe OK");
    return 0;
}
'''


def build(tmp_path, cases_source):
    body = probe_source()
    # Each case is a fresh boot: drop the static cache so one binary can run them all.
    body = body.replace("static int confined = -1;", "int confined = -1;")
    function = body[body.index("bool ctxr_exec_confined(void)"):]
    assert "chip_id" not in function, "ctxr_exec_confined() must not filter by chip ID"
    compile_and_run(tmp_path, "ctxr_exec_probe", HARNESS % body)


def test_probe_decides_on_every_chip(tmp_path):
    build(tmp_path, None)


def test_answer_is_cached_per_boot():
    body = probe_source()
    assert "static int confined = -1;" in body
    assert body.index("if (confined >= 0)") < body.index("smp_call1(")


def test_reports_the_fault():
    body = probe_source()
    assert "exc_last_esr = 0;" in body and "ESR 0x%lx" in body


def test_callers_use_the_probe():
    source = (REPO / "src/ctxr_exec.c").read_text()
    assert "if (!ctxr_exec_confined() || ctxr_exec_problem())" in source
    assert "if (!ctxr_exec_confined() || !ctxr_exec_problem())" in source
    assert "if (ctxr_exec_confined()) {" in (REPO / "src/kboot.c").read_text()
