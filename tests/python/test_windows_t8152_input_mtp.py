# SPDX-License-Identifier: MIT
"""I1: the M6's internal keyboard over the T8152 MTP, carrier side (host tests only).

The pure files are compiled for the host (clang, -std=gnu11 -Wall -Wextra -Werror, ASan and
UBSan) with C drivers in tests/e2e that hold the failure lists written before the code:

  windows_mtp_scm_host.c      S1-S11  rings, frames, checksum, INIT/READY/GPIO, acks, fuzz
  windows_mtp_dart_host.c     D1-D9   stream-4 entries, the DVA plan, refusals
  windows_input_kbd_host.c    H1-H6, K1-K7  descriptor walker, keyboard translation, Fn layer
  windows_mtp_bringup_host.c  B1-B14  bring-up and run-time service against a fake MTP
  windows_rtkit_host.c        (tools/windows_rtkit_e2e.py) premapped pool-only RTKit
  windows_input_glue_host.c   G12-G14 (test_windows_t8152_input.py) hv_input with the MTP

Synthetic frames and descriptors only: no keyboard capture exists on this Mac (the carrier's
PROBE logs the real descriptor on the M6).  The rules the replays cannot see are checked in the
sources below: the MMIO allow-list, no SMC or NVRAM access, the poll hooks, the build objects,
and provenance (our own MIT files, no Apple data).
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"

TYPES_H = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef long s64;
#define BIT(x) (1UL << (x))
"""

PURE = ["mtp_scm.c", "mtp_scm.h", "mtp_dart_t8152.c", "mtp_dart_t8152.h", "hid_desc.c",
        "hid_desc.h", "input_kbd.c", "input_kbd.h", "mtp_bringup.c", "mtp_bringup.h"]
NEW_FILES = PURE + ["hv_mtp_t8152.c", "hv_mtp_t8152.h"]

DRIVERS = {
    "scm": (["mtp_scm.c"], "windows_mtp_scm_host.c", ["1000000"],
            ["checksum", "ring_rx", "ring_rx_refusals", "oversize", "ring_tx", "parse", "build",
             "init", "events", "ack", "fuzz"]),
    "dart": (["mtp_dart_t8152.c"], "windows_mtp_dart_host.c", [],
             ["pte_format", "dva_plan", "walk", "foreign", "mapped", "tables", "args",
              "l3_boundary"]),
    "kbd": (["hid_desc.c", "input_kbd.c"], "windows_input_kbd_host.c", [],
            ["items", "state", "layouts", "keys", "fn_layer", "sticky", "media_overlap",
             "errors"]),
    "bringup": (["mtp_bringup.c", "mtp_scm.c", "mtp_dart_t8152.c", "hid_desc.c", "input_kbd.c"],
                "windows_mtp_bringup_host.c", [],
                ["happy", "run_set", "ttbr_invalid", "ttbr_extra", "tcr", "root", "foreign",
                 "pmgr_stuck", "rtkit", "no_fe", "mbox_full", "no_init", "no_kbd_desc", "layout",
                 "no_ack", "ack_error", "no_ready", "gpio", "csum_latch", "rtkit_latch",
                 "ring_latch", "budget_wake", "mbox_busy"]),
}


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def clang():
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    return cc


@pytest.fixture(scope="module")
def replays(tmp_path_factory):
    cc = clang()
    out = tmp_path_factory.mktemp("mtp")
    (out / "types.h").write_text(TYPES_H)
    for name in PURE:
        shutil.copy2(WIN / name, out / name)
    results = {}
    transcript = []
    for key, (sources, driver, args, _) in DRIVERS.items():
        binary = out / f"{key}-host"
        build = subprocess.run(
            [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-DMTP_HOST_TEST",
             "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-iquote", str(out),
             *[str(out / s) for s in sources], str(REPO / "tests/e2e" / driver), "-o",
             str(binary)],
            capture_output=True, text=True, timeout=300)
        assert build.returncode == 0, build.stderr[-4000:]
        run = subprocess.run([str(binary), *args], capture_output=True, text=True, timeout=900)
        lines = [json.loads(l) for l in run.stdout.splitlines() if l.startswith("{")]
        results[key] = (run, {l["case"]: l for l in lines}, [l["case"] for l in lines])
        transcript.append(run.stdout)
    artifact = os.environ.get("AURA_MTP_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text("".join(transcript))
    return results


@pytest.mark.parametrize("key", list(DRIVERS))
def test_replay_passes(replays, key):
    run, cases, order = replays[key]
    assert run.returncode == 0, run.stdout[-4000:] + run.stderr[-4000:]
    assert order == DRIVERS[key][3]
    assert all(cases[name]["result"] == "passed" for name in order)


def test_scm_fuzz_ran(replays):
    _, cases, _ = replays["scm"]
    assert cases["fuzz"]["iterations"] == 1000000 and cases["fuzz"]["frames"] > 100000


def test_dva_plan_is_the_measured_one(replays):
    # D2: the rings at the DVAs the MTP has seen on every Linux boot, the RTKit pool after the
    # wake ring's guard page, two tables in all.
    _, cases, _ = replays["dart"]
    plan = cases["dva_plan"]
    assert (plan["tx"], plan["rx"], plan["wake"], plan["rtkit_pool"]) == \
        ("0x10040000000", "0x10040204000", "0x10040408000", "0x10040410000")
    assert plan["tables"] == 2
    _, cases, _ = replays["bringup"]
    happy = cases["happy"]
    assert (happy["tx"], happy["rx"], happy["wake"], happy["pool"]) == \
        ("0x10040000000", "0x10040204000", "0x10040408000", "0x10040410000")
    assert happy["pmgr_writes"] == 3 and happy["tables"] == 2


def test_rtkit_premapped_pool(tmp_path):
    # The production RTKit, replayed with the premapped pool-only scenario added for I1.
    if not (REPO / "build/build_cfg.h").exists():
        pytest.skip("build/build_cfg.h missing: run `gmake build-cfg WINDOWS=1` first")
    clang()
    out = tmp_path / "rtkit"
    run = subprocess.run([sys.executable, str(REPO / "tools/windows_rtkit_e2e.py"), "--output",
                          str(out)], capture_output=True, text=True, timeout=600)
    report = json.loads((out / "result.json").read_text())
    assert report["result"] == "passed", report.get("error")
    assert report["observations"]["premapped_pool_only"] is True


# ---- carrier sources: rules the replays cannot see -----------------------------------------

def test_mmio_allow_list():
    glue = source("hv_mtp_t8152.c")
    read = function_body(glue, "static bool read_allowed(u64 addr)")
    assert "MTP_T8152_ASC_CPU_CTRL" in read and "MTP_T8152_DART_TTBR" in read
    assert "MTP_T8152_DART_TCR" in read and "pmgr_reg(addr)" in read
    write = function_body(glue, "static void op_write32(void *ctx, u64 addr, u32 value)")
    assert write.index("if (!pmgr_reg(addr))") < write.index("write32(addr, value)")
    # Every direct MMIO in the glue goes through those two functions, under the guard.
    for call in ("read32(", "write32(", "set32(", "clear32(", "mask32(", "write64(", "read64("):
        hits = [m.start() for m in re.finditer(r"(?<![\w>])" + re.escape(call), glue)]
        allowed = {"read32(": 1, "write32(": 1}.get(call, 0)
        assert len(hits) == allowed, (call, len(hits))
    hdr = source("mtp_bringup.h")
    assert re.search(r"#define MTP_T8152_ASC\s+0x314600000ull", hdr)
    assert re.search(r"#define MTP_T8152_DART\s+0x318100000ull", hdr)
    assert re.search(r"#define MTP_T8152_PMGR_PS\s+0x30828c000ull", hdr)
    assert re.search(r"#define MTP_T8152_PMGR_COUNT\s+13u", hdr)
    assert re.search(r"#define MTP_T8152_DVA_BASE\s+0x10040000000ull", hdr)
    assert re.search(r"#define MTP_T8152_TCR\s+9u", hdr)


def test_no_smc_no_nvram_no_tlbi():
    for name in NEW_FILES:
        text = re.sub(r"/\*.*?\*/", "", source(name), flags=re.S)
        for word in ("smc_", "SMC_", "pcIO", "nvram", "NVRAM", "tlbi", "dart_init", "dart_map(",
                     "dart_tlb"):
            assert not re.search(r"(?<!\w)" + re.escape(word), text), (name, word)


def test_run_rule_before_any_write():
    bring = source("mtp_bringup.c")
    attach = function_body(bring, "int mtp_bringup_attach(")
    run = attach.index("MTP_T8152_ASC_CPU_CTRL")
    assert attach.index("if (b->asc_ctrl & MTP_T8152_RUN)") < attach.index("check_dart(b)") < \
        attach.index("map_rings(b)") < attach.index("power_on(b)") < \
        attach.index("ops->rtkit_start(") < attach.index("start_scm(b)") < \
        attach.index("wait_init(b)") < attach.index("kbd_layout_from_desc(") < \
        attach.index("enable_keyboard(b)")
    assert run < attach.index("check_dart(b)")
    # Only the keyboard (and, later, multi-touch) is ever enabled.
    assert bring.count("MTP_CMD_ENABLE") == 2
    assert '"keyboard"' in bring and '"stm"' not in bring and '"actuator"' not in bring


def test_rings_noncacheable_and_area_denied():
    glue = source("hv_mtp_t8152.c")
    prep = function_body(glue, "static void prepare_area(void)")
    assert prep.index("dc_civac_range((void *)rings, size)") < \
        prep.index("mmu_add_mapping(rings, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW)")
    for alias in ("REGION_RWX_EL0", "REGION_RW_EL0", "REGION_RX_EL1"):
        assert f"mmu_add_mapping(rings | {alias}, rings, size, MAIR_IDX_NORMAL_NC" in prep
    attach = function_body(glue, "bool hv_mtp_t8152_attach(")
    assert attach.index("identity()") < attach.index("hv_t8152_reserve_input_area(") < \
        attach.index("asc_init(") < attach.index("prepare_area()") < \
        attach.index("mtp_bringup_attach(")
    policy = (WIN / "hv_t8152.c").read_text()
    area = function_body(policy, "int hv_t8152_reserve_input_area(u64 start, u64 end)")
    assert "start < ram_end" in area and "ram_conflicts(start, end, ram_host_top)" in area
    assert area.index("deny_add(start, end - start,") > area.index("ram_conflicts(")


def test_poll_hooks_and_single_servicer():
    glue = source("hv_mtp_t8152.c")
    poll = function_body(glue, "void hv_mtp_t8152_poll(void)")
    assert "__atomic_test_and_set(&servicer, __ATOMIC_ACQUIRE)" in poll
    assert "MTP_POLL_US * freq_mhz" in poll
    hv_input = source("hv_input.c")
    tick = function_body(hv_input, "void hv_input_poll(void)")
    assert tick.index("hv_mtp_t8152_poll()") < tick.index("spin_lock(&input_lock)")
    idle = function_body(hv_input, "void hv_input_idle_poll(void)")
    assert "sync_line" not in idle and "spin_lock" not in idle
    exc = source("hv_exc.c")
    assert exc.index("hv_windows_mtp_poll();\n    /* I1:") < exc.index("hv_input_idle_poll();")
    proxy = function_body(hv_input, "u64 hv_input_proxy(u64 op, u64 a1, u64 a2, u64 a3)")
    attach_case = proxy[proxy.index("case HV_INPUT_OP_ATTACH:"):proxy.index("case HV_INPUT_OP_PROBE:")]
    # The MTP is brought up outside input_lock (its keyboard updates take it).
    assert attach_case.index("spin_unlock(&input_lock);") < \
        attach_case.index("hv_mtp_t8152_attach(")
    make = (REPO / "Makefile").read_text()
    assert "mtp_scm.o mtp_dart_t8152.o hid_desc.o input_kbd.o mtp_bringup.o hv_mtp_t8152.o) \\" \
        in make


def test_provenance_of_new_files():
    for name in NEW_FILES:
        text = source(name)
        assert text.startswith("/* SPDX-License-Identifier: MIT */"), name
        for foreign in ("Copyright", "GPL", "AuroraSilicon", "x1n1", "Neo", "/Users/",
                        "j804-private-data", "HIDF"):
            assert foreign not in text, (name, foreign)
    for name in ("windows_mtp_scm_host.c", "windows_mtp_dart_host.c",
                 "windows_input_kbd_host.c", "windows_mtp_bringup_host.c"):
        text = (REPO / "tests/e2e" / name).read_text()
        assert text.startswith("/* SPDX-License-Identifier: MIT */")
        for foreign in ("Copyright", "/Users/", "j804-private-data", "HIDF"):
            assert foreign not in text, (name, foreign)
