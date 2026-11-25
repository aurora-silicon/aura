# SPDX-License-Identifier: MIT
"""Host tests for the iBoot boot-record helpers (src/pmu_counter.c).

pmu_counter.c finds the primary SPMI PMU in the ADT and clears or reads iBoot's
failed-boot record in its legacy scratchpad. It is compiled unchanged with
aura's own ADT reader (rust/src/adt.rs, built as a host staticlib) against
synthetic ADTs, with a modelled SPMI bus that records every access.
"""
from pathlib import Path
import re
import shutil
import struct
import subprocess

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def prop(name, value):
    data = name.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value
    return data + b"\0" * (-len(data) % 4)


def node(name, props=(), children=()):
    props = [("name", name.encode() + b"\0"), *props]
    out = struct.pack("<II", len(props), len(children))
    out += b"".join(prop(k, v) for k, v in props)
    return out + b"".join(children)


def u32(*v):
    return struct.pack(f"<{len(v)}I", *v)


def pmu(name, primary=1, scrpad=0x9f00, reg=(0x0f, 0x0, 0x0), compatible=b"pmu,spmi\0"):
    props = [("compatible", compatible), ("reg", u32(*reg))]
    if primary is not None:
        props.append(("is-primary", u32(primary)))
    if scrpad is not None:
        props.append(("info-leg_scrpad", u32(scrpad)))
    return node(name, props)


def adt(*buses):
    return node("device-tree", [], [node("arm-io", [], list(buses))])


# The usual layout: a secondary PMU and a non-PMU device before the primary.
DEFAULT = adt(
    node("i2c0", [], [pmu("pmu-on-i2c", scrpad=0x1000)]),
    node("nub-spmi0", [], [
        node("hpm0", [("compatible", b"usbc,cd3217\0"), ("reg", u32(0x38, 0, 0))]),
        pmu("pmu-aux", primary=0, scrpad=0x2000, reg=(0x0e, 0, 0)),
        pmu("pmu-main"),
    ]),
)

SCENARIOS = {
    "default": DEFAULT,
    # a bus called plain "spmiN" is searched too; the first primary wins
    "spmi-bus": adt(node("spmi1", [], [pmu("pmu", scrpad=0x40, reg=(0x1a2b,))]),
                    node("nub-spmi0", [], [pmu("pmu2", scrpad=0x80)])),
    "no-primary": adt(node("nub-spmi0", [], [pmu("pmu", primary=0), pmu("pmu1", primary=None)])),
    "not-pmu": adt(node("nub-spmi0", [], [pmu("pmu", compatible=b"pmu,i2c\0")])),
    "no-scrpad": adt(node("nub-spmi0", [], [pmu("pmu", scrpad=None)])),
    "short-reg": adt(node("nub-spmi0", [], [pmu("pmu", reg=())])),
    "top-scrpad": adt(node("nub-spmi0", [], [pmu("pmu", scrpad=0xfffc)])),
    "high-scrpad": adt(node("nub-spmi0", [], [pmu("pmu", scrpad=0xfffd)])),
    "no-arm-io": node("device-tree", [], [node("nub-spmi0", [], [pmu("pmu")])]),
}

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmu_counter.h"
#include "spmi.h"

const void *adt;
static u32 adt_size;
u32 adt_get_size(void) { return adt_size; }

int debug_printf(const char *fmt, ...) { (void)fmt; return 0; }

/* The modelled bus: one PMU register file, every access logged. */
static u8 regs[0x10000];
static char opened[64];
static int inits, shutdowns, fail_init, fail_read, fail_write;
static char log_buf[4096];

struct spmi_dev { int unused; };
static struct spmi_dev dev;

spmi_dev_t *spmi_init(const char *node)
{
    if (fail_init)
        return NULL;
    snprintf(opened, sizeof(opened), "%s", node);
    inits++;
    return &dev;
}

void spmi_shutdown(spmi_dev_t *d)
{
    assert(d == &dev);
    shutdowns++;
}

int spmi_ext_read_long(spmi_dev_t *d, u8 addr, u16 reg, u8 *bfr, size_t len)
{
    assert(d == &dev && inits > shutdowns);
    snprintf(log_buf + strlen(log_buf), sizeof(log_buf) - strlen(log_buf), "R%02x@%04x/%zu;",
             addr, reg, len);
    if (fail_read)
        return -1;
    assert(reg + len <= sizeof(regs));
    memcpy(bfr, regs + reg, len);
    return 0;
}

int spmi_ext_write_long(spmi_dev_t *d, u8 addr, u16 reg, const u8 *bfr, size_t len)
{
    assert(d == &dev && inits > shutdowns);
    snprintf(log_buf + strlen(log_buf), sizeof(log_buf) - strlen(log_buf), "W%02x@%04x/%zu=%02x;",
             addr, reg, len, bfr[0]);
    if (fail_write)
        return -1;
    memcpy(regs + reg, bfr, len);
    return 0;
}

int main(int argc, char **argv)
{
    assert(argc == 6);
    FILE *f = fopen(argv[1], "rb");
    static u8 blob[1 << 16];
    adt_size = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    adt = blob;

    const char *op = argv[2], *fail = argv[3];
    unsigned scrpad = strtoul(argv[4], NULL, 0);
    regs[(scrpad + 2) & 0xffff] = strtoul(argv[5], NULL, 0);
    regs[(scrpad + 3) & 0xffff] = 0x5c;
    fail_init = !strcmp(fail, "init");
    fail_read = !strcmp(fail, "read");
    fail_write = !strcmp(fail, "write");

    int ret;
    u8 counts = 0xee, stage = 0xee;
    if (!strcmp(op, "clear")) {
        ret = pmu_clear_panic_counter();
    } else {
        ret = pmu_read_boot_record(&counts, &stage);
        if (ret)
            assert(counts == 0xee && stage == 0xee);
    }
    /* every bus that was opened is closed again */
    assert(inits == shutdowns);
    fprintf(stdout, "ret=%d path=%s counts=%02x stage=%02x after=%02x log=%s\n", ret, opened, counts, stage,
           regs[(scrpad + 2) & 0xffff], log_buf);
    return 0;
}
'''

UTILS = r'''
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
int debug_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define printf(...) debug_printf(__VA_ARGS__)
'''


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("pmu-counter")
    rs = tmp / "rs"
    rs.mkdir()
    shutil.copy(ROOT / "rust/src/adt.rs", rs / "adt.rs")
    (rs / "lib.rs").write_text(
        "#![allow(non_camel_case_types, dead_code, unused_imports)]\n"
        "type c_size_t = usize;\nmod adt;\n")
    libs = subprocess.run(
        ["rustc", "--edition", "2021", "--crate-type", "staticlib", "-O",
         "--print", "native-static-libs", "-o", str(rs / "libadt_host.a"), str(rs / "lib.rs")],
        check=True, capture_output=True, text=True)
    native = re.search(r"native-static-libs: (.*)", libs.stderr)
    native = native.group(1).split() if native else []

    src = tmp / "src"
    src.mkdir()
    for name in ("pmu_counter.c", "pmu_counter.h", "adt.h", "spmi.h"):
        shutil.copy(ROOT / "src" / name, src / name)
    shutil.copy(ROOT / "tests/kboot_audio_host/types.h", src / "types.h")
    (src / "utils.h").write_text(UTILS)
    (src / "harness.c").write_text(HARNESS)
    binary = tmp / "pmu-counter-host"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wextra",
         "-Wno-unused-parameter", "-Wno-sign-compare", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-fno-sanitize-recover=undefined",
         "-I", str(src), "-I", str(ROOT / "src"), "-I", str(ROOT),
         str(src / "harness.c"), str(src / "pmu_counter.c"), str(rs / "libadt_host.a"),
         *native, "-o", str(binary)],
        check=True)
    for name, blob in SCENARIOS.items():
        (tmp / f"{name}.adt").write_bytes(blob)
    return tmp, binary


def run(host, scenario, op, fail="none", scrpad=0x9f00, counts=0x13):
    tmp, binary = host
    out = subprocess.run(
        [str(binary), str(tmp / f"{scenario}.adt"), op, fail, hex(scrpad), hex(counts)],
        capture_output=True, text=True, env={"ASAN_OPTIONS": "detect_leaks=0"})
    assert out.returncode == 0, out.stdout + out.stderr
    return dict(kv.split("=", 1) for kv in out.stdout.split())


def test_clear_finds_the_primary_pmu(host):
    r = run(host, "default", "clear")
    assert r["ret"] == "0"
    assert r["path"] == "/arm-io/nub-spmi0"
    # read the counts byte at scratchpad + 2 on the primary's SPMI address, then zero it
    assert r["log"] == "R0f@9f02/1;W0f@9f02/1=00;"
    assert r["after"] == "00"


def test_clear_leaves_a_zero_count_alone(host):
    r = run(host, "default", "clear", counts=0)
    assert r["ret"] == "0"
    assert r["log"] == "R0f@9f02/1;"


def test_first_primary_on_any_spmi_bus(host):
    r = run(host, "spmi-bus", "clear", scrpad=0x40)
    assert r["ret"] == "0"
    assert r["path"] == "/arm-io/spmi1"
    # only the low byte of reg[0] is the SPMI address
    assert r["log"] == "R2b@0042/1;W2b@0042/1=00;"


@pytest.mark.parametrize("scenario", ["no-primary", "not-pmu", "no-scrpad", "short-reg",
                                      "high-scrpad", "no-arm-io"])
@pytest.mark.parametrize("op", ["clear", "read"])
def test_refused_without_touching_the_bus(host, scenario, op):
    r = run(host, scenario, op)
    assert r["ret"] == "-1"
    assert r["path"] == ""
    assert r["log"] == ""


def test_highest_scratchpad_still_fits(host):
    r = run(host, "top-scrpad", "clear", scrpad=0xfffc)
    assert r["ret"] == "0"
    assert r["log"] == "R0f@fffe/1;W0f@fffe/1=00;"


@pytest.mark.parametrize("op", ["clear", "read"])
def test_bus_failures(host, op):
    assert run(host, "default", op, fail="init")["ret"] == "-1"
    r = run(host, "default", op, fail="read")
    assert r["ret"] == "-1"
    assert r["after"] == "13"
    assert "W" not in r["log"]


def test_failed_clear_is_reported(host):
    r = run(host, "default", "clear", fail="write")
    assert r["ret"] == "-1"
    assert r["log"] == "R0f@9f02/1;W0f@9f02/1=00;"


def test_read_boot_record(host):
    r = run(host, "default", "read", counts=0x21)
    assert r["ret"] == "0"
    # counts byte and the failed boot's stage, in one two-byte read; nothing written
    assert (r["counts"], r["stage"]) == ("21", "5c")
    assert r["log"] == "R0f@9f02/2;"
    assert r["after"] == "21"
