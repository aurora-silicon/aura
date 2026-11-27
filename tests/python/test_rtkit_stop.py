# SPDX-License-Identifier: MIT
"""rtkit_stop() drains a stopped IOP only once its CPU has halted, or after the whole window."""
from pathlib import Path
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


def define(source, name):
    start = source.index("#define " + name)
    return source[start:source.index("\n", start) + 1]


@pytest.fixture(scope="module")
def stop_binary(tmp_path_factory):
    asc = (ROOT / "src/asc.c").read_text()
    rtkit = (ROOT / "src/rtkit.c").read_text()
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long u64;
#define BIT(n) (1UL << (n))
typedef struct { u64 cpu_base; } asc_dev_t;
struct asc_message { u64 msg0; u32 msg1; };
typedef struct { const char *name; asc_dev_t *asc; bool crashed; } rtkit_dev_t;
#define rtkit_printf(...) ((void)0)
#define ASC_CPU_CONTROL 0x44
#define ASC_CPU_CONTROL_START 0x10
''' + define(asc, "ASC_CPU_STATUS ") + define(asc, "ASC_CPU_STATUS_STOPPED") + \
        define(asc, "ASC_CPU_STATUS_IDLE") + r'''
/* Fake time: every CPU_STATUS read takes 10 us. */
static u64 now_us;
static u32 status_before, status_after; /* CPU_STATUS before and after `halt_at` us */
static u64 halt_at, late_at;            /* late_at: when the late acknowledgement arrives */
static bool late_pending, run;
static unsigned dropped_late, warnings;
static u32 read32(u64 addr)
{
    if (addr == ASC_CPU_CONTROL)
        return run ? ASC_CPU_CONTROL_START : 0;
    assert(addr == ASC_CPU_STATUS);
    now_us += 10;
    return now_us >= halt_at ? status_after : status_before;
}
static void asc_cpu_stop(asc_dev_t *asc) { run = false; }
static bool asc_recv(asc_dev_t *asc, struct asc_message *msg)
{
    if (late_pending && now_us >= late_at) {
        late_pending = false;
        msg->msg0 = 0x60000000000000UL;
        msg->msg1 = 0;
        dropped_late++;
        return true;
    }
    return false;
}
static u64 timeout_calculate(u32 usec) { return now_us + usec; }
static bool timeout_expired(u64 timeout) { return now_us >= timeout; }
static int counted_printf(const char *fmt, ...) { warnings++; return 0; }
#define printf counted_printf
''' + function(asc, "bool asc_cpu_halted(") + function(asc, "bool asc_cpu_idle(") + \
        function(asc, "u32 asc_cpu_status(") + function(rtkit, "static void rtkit_drain(") + \
        define(rtkit, "RTKIT_STOP_TIMEOUT_US") + function(rtkit, "void rtkit_stop(") + r'''
int main(int argc, char **argv)
{
    asc_dev_t asc = {0};
    rtkit_dev_t rtk = {.name = "ans", .asc = &asc};
    status_before = strtoul(argv[1], NULL, 0);
    halt_at = strtoul(argv[2], NULL, 0);
    status_after = strtoul(argv[3], NULL, 0);
    late_at = strtoul(argv[4], NULL, 0);
    late_pending = true;
    run = true;
    rtkit_stop(&rtk);
    fprintf(stdout, "end=%lu late=%u pending=%d warnings=%u crashed=%d run=%d\n", now_us,
            dropped_late, late_pending, warnings, rtk.crashed, run);
    return 0;
}
'''
    out = tmp_path_factory.mktemp("rtkit_stop")
    source = out / "rtkit_stop.c"
    binary = out / "rtkit_stop"
    source.write_text(harness)
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-unused-function",
                    "-o", str(binary), str(source)], check=True)
    return binary


def run(binary, before, halt_at, after, late_at):
    result = subprocess.run([str(binary), hex(before), str(halt_at), hex(after), str(late_at)],
                            check=True, capture_output=True, text=True)
    return dict(item.split("=") for item in result.stdout.split())


def test_idle_iop_keeps_the_whole_window(stop_binary):
    # STOPPED|IDLE (0x22) on the first read: the IOP is in its idle loop, RUN was set, and its
    # acknowledgement lands 400 us later. STOPPED alone would end the wait at once and miss it.
    r = run(stop_binary, 0x22, 1 << 30, 0x22, 400)
    assert r["late"] == "1" and r["pending"] == "0"
    assert int(r["end"]) >= 1000
    assert r["warnings"] == "0"  # still idle at the end: no "not halted" line
    assert r["crashed"] == "1" and r["run"] == "0"


def test_idle_then_halted_ends_at_the_halt(stop_binary):
    # Idle, then halted (STOPPED, IDLE clear) at 300 us: the wait ends there and the last drain
    # takes what was sent before the halt.
    r = run(stop_binary, 0x22, 300, 0x02, 200)
    assert r["late"] == "1"
    assert 300 <= int(r["end"]) < 1000
    assert r["warnings"] == "0"


def test_halted_at_once_ends_at_once(stop_binary):
    r = run(stop_binary, 0x02, 0, 0x02, 0)
    assert r["late"] == "1"
    assert int(r["end"]) < 100
    assert r["warnings"] == "0"


def test_running_cpu_is_reported_after_the_window(stop_binary):
    r = run(stop_binary, 0x00, 1 << 30, 0x00, 600)
    assert r["late"] == "1"
    assert int(r["end"]) >= 1000
    assert r["warnings"] == "1"
