# SPDX-License-Identifier: GPL-2.0-only
"""A post-M4 ANS started from off: stale messages in the receive mailbox are dropped before the run
bit, and a failed start says which step failed."""

from pathlib import Path
import os
import subprocess

import pytest

from host_compiler import host_cc

CASES = {
    # name: (stale messages, power on fails, SART fails, handshake fails, BOOT_STATUS fails, expected)
    "clean": (0, 0, 0, 0, 0, "ok drained=0 boots=1"),
    "stale-at-start": (3, 0, 0, 0, 0, "ok drained=3 boots=1"),
    "power-on-fails": (0, 1, 0, 0, 0, "fail drained=0 boots=0 why=power"),
    "sart-fails": (0, 0, 1, 0, 0, "fail drained=0 boots=0 why=sart"),
    "no-handshake": (0, 0, 0, 1, 0, "fail drained=0 boots=1 why=handshake"),
    "boot-status": (0, 0, 0, 0, 1, "fail drained=0 boots=1 why=boot_status"),
}


@pytest.mark.parametrize("case", sorted(CASES))
def test_ans_warm_start(tmp_path, case):
    stale, pwr, sart, hs, bs, expected = CASES[case]
    source = Path(os.environ.get("NVME_SOURCE", Path(__file__).resolve().parents[2] / "src/nvme.c")).read_text()
    start = source.index("/*\n * Discard what an earlier owner left in the receive mailbox")
    body = source[start:source.index("/* A write or flush, timed:")]
    harness = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
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
struct asc_message { u64 msg0; u32 msg1; };
static asc_dev_t asc;
static rtkit_dev_t rtkit;
static sart_dev_t sart;
static asc_dev_t *nvme_asc = &asc;
static rtkit_dev_t *nvme_rtkit = &rtkit;
static sart_dev_t *nvme_sart = &sart;
static struct sart_entry nvme_sart_saved[16];
static int nvme_sart_saved_cnt = 9;
static u64 nvme_base, nvme_boot_ticks;
static int stale = @STALE@, drained, boots;
static const char *why = "";
static u64 get_ticks(void) { return 1; }
static u32 read32(u64 a) { return 0; }
static u32 asc_cpu_status(asc_dev_t *a) { return 0x6a; }
static void asc_i2a_enable(asc_dev_t *a, bool on) {}
static bool asc_recv(asc_dev_t *a, struct asc_message *m)
{
    if (!stale)
        return false;
    stale--; drained++; m->msg0 = 0x0070000000000020ull; m->msg1 = 0;
    return true;
}
static int pcie_ans_power_on(void) { if (@PWR@) why = "power"; return @PWR@; }
static bool sart_set_entries(sart_dev_t *s, const struct sart_entry *e, int n)
{ if (@SART@) why = "sart"; return !@SART@; }
static bool rtkit_boot_warm(rtkit_dev_t *r, u32 mask)
{
    boots++;
    /* A queued message is read in place of HELLO. */
    if (stale || @HS@) { why = "handshake"; return false; }
    return true;
}
static int poll32(u64 a, u32 m, u32 v, unsigned t) { if (@BS@) why = "boot_status"; return @BS@ ? -1 : 0; }
''' + body + r'''
int main(void)
{
    bool ok = nvme_ans_power_on();
    printf("%s drained=%d boots=%d%s%s\n", ok ? "ok" : "fail", drained, boots, *why ? " why=" : "", why);
    return 0;
}
'''
    for k, v in (("@STALE@", stale), ("@PWR@", pwr), ("@SART@", sart), ("@HS@", hs), ("@BS@", bs)):
        harness = harness.replace(k, str(v))
    c = tmp_path / "t.c"
    c.write_text(harness)
    exe = tmp_path / "t"
    subprocess.run([*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable", "-Wno-format", "-o", str(exe), str(c)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert out.splitlines()[-1] == expected
