# SPDX-License-Identifier: GPL-2.0-only
"""A post-M4 ANS started from off: reset first when its CPU isn't in its reset state, and once
more with a fresh RTKit instance when the firmware doesn't answer the run bit."""

from pathlib import Path
import os
import subprocess

import pytest

from host_compiler import host_cc

CASES = {
    # name: (CPU status before the run bit, warm starts that fail, buffers owned, expected)
    "reset-state": (0x6A, 0, False, "ok resets=0 inits=0 boots=1"),
    "stopped-no-reset": (0x62, 0, False, "ok resets=1 inits=0 boots=1"),
    "no-answer-retry": (0x6A, 1, False, "ok resets=1 inits=1 boots=2"),
    "both-fixes": (0x62, 1, False, "ok resets=2 inits=1 boots=2"),
    "no-answer-twice": (0x6A, 2, False, "fail resets=1 inits=1 boots=2"),
    "owned-buffers-no-retry": (0x6A, 1, True, "fail resets=0 inits=0 boots=1"),
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_ans_start_from_off(tmp_path, case):
    status, fails, owned, expected = CASES[case]
    source = Path(os.environ.get("NVME_SOURCE", Path(__file__).resolve().parents[2] / "src/nvme.c")).read_text()
    start = source.index("/* ASC CPU status bits: stopped")
    power_on = source[start:source.index("/* A write or flush, timed:")]
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1U << (n))
#define USEC_PER_SEC 1000000
#define NVME_BOOT_STATUS 0x1300
#define NVME_BOOT_STATUS_OK 0xde71ce55
typedef int asc_dev_t;
typedef int rtkit_dev_t;
typedef int sart_dev_t;
struct sart_entry { u64 a; };
static asc_dev_t asc;
static rtkit_dev_t rtkit_a, rtkit_b;
static sart_dev_t sart;
static asc_dev_t *nvme_asc = &asc;
static rtkit_dev_t *nvme_rtkit = &rtkit_a;
static sart_dev_t *nvme_sart = &sart;
static struct sart_entry nvme_sart_saved[16];
static int nvme_sart_saved_cnt = 9;
static u8 nvme_die;
static u64 nvme_base, nvme_boot_ticks;
static u32 status = @STATUS@, fails = @FAILS@;
static int resets, inits, boots, stopped;
static bool owned = @OWNED@;
static u64 get_ticks(void) { return 1; }
static void udelay(u32 us) {}
static int pcie_ans_power_on(void) { return 0; }
static u32 asc_cpu_status(asc_dev_t *a) { return status | (stopped ? BIT(1) : 0); }
static void asc_cpu_stop(asc_dev_t *a) { stopped = 1; }
/* The reset leaves the CPU stopped in its reset state, and the SART empty. */
static int pmgr_reset(u8 die, const char *name) { resets++; status = 0x6a; return 0; }
static bool sart_set_entries(sart_dev_t *s, const struct sart_entry *e, int n) { return n == 9; }
static bool rtkit_boot_warm(rtkit_dev_t *r, u32 mask)
{
    boots++;
    /* A CPU stopped without a reset never answers the run bit. */
    if (!(status & BIT(3)))
        return false;
    status = 0x40;
    return !(fails && fails--);
}
static int poll32(u64 a, u32 m, u32 v, unsigned t) { return 0; }
static u32 read32(u64 a) { return NVME_BOOT_STATUS_OK; }
static bool rtkit_has_owned_buffers(const rtkit_dev_t *r) { return owned; }
static void rtkit_free(rtkit_dev_t *r) {}
static rtkit_dev_t *rtkit_init(const char *n, asc_dev_t *a, void *d, void *i, sart_dev_t *s,
                               bool sram) { inits++; return &rtkit_b; }
''' + power_on + r'''
int main(void)
{
    bool ok = nvme_ans_power_on();
    printf("%s resets=%d inits=%d boots=%d\n", ok ? "ok" : "fail", resets, inits, boots);
    return 0;
}
'''
    harness = (harness.replace("@STATUS@", hex(status)).replace("@FAILS@", str(fails))
               .replace("@OWNED@", "true" if owned else "false"))
    c = tmp_path / "t.c"
    c.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-Wno-unused-function",
                    "-Wno-unused-variable", "-o", str(exe), str(c)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1] == expected
