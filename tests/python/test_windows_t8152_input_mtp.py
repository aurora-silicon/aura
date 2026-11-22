# SPDX-License-Identifier: GPL-2.0-only
"""I1/I2: the M6's internal keyboard and trackpad over the T8152 MTP, carrier side (host tests).

The pure files are compiled for the host (clang, -std=gnu11 -Wall -Wextra -Werror, ASan and
UBSan) with C drivers in tests/e2e that hold the failure lists written before the code:

  windows_mtp_scm_host.c      S1-S11  rings, frames, checksum, INIT/READY/GPIO, acks, fuzz
  windows_mtp_dart_host.c     D1-D9   stream-4 entries, the DVA plan, refusals
  windows_input_kbd_host.c    H1-H6, K1-K7  descriptor walker, keyboard translation, Fn layer
  windows_input_tp_host.c     T1-T13  trackpad finger records to a relative mouse (I2)
  windows_mtp_bringup_host.c  B1-B14, P1-P8  bring-up and run-time service against a fake MTP
                              (keyboard; trackpad firmware, AFE reset order, geometry)
  windows_rtkit_host.c        (tools/windows_rtkit_e2e.py) premapped pool-only RTKit
  windows_input_glue_host.c   G12-G14 (test_windows_t8152_input.py) hv_input with the MTP

Synthetic frames, descriptors and firmware only: no keyboard or trackpad capture exists on this
Mac (the carrier's PROBE logs the real descriptors and frames on the M6), and the real trackpad
firmware never enters a test.  The rules the replays cannot see are checked in the sources
below: the MMIO allow-list, the single SMC write site (pcIO, the AFE reset only), no NVRAM, the
poll hooks, the build objects, and provenance (our own MIT files, no Apple data).
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
        "hid_desc.h", "input_kbd.c", "input_kbd.h", "input_tp.c", "input_tp.h", "mtp_bringup.c",
        "mtp_bringup.h"]
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
    "tp": (["input_tp.c"], "windows_input_tp_host.c", ["200000"],
           ["gain", "accel", "y_axis", "no_jump", "taps", "click", "scroll", "three", "malformed",
            "silence", "geometry", "palms", "fuzz"]),
    "bringup": (["mtp_bringup.c", "mtp_scm.c", "mtp_dart_t8152.c", "hid_desc.c", "input_kbd.c",
                 "input_tp.c"],
                "windows_mtp_bringup_host.c", [],
                ["happy", "run_set", "run_after_power", "ttbr_invalid", "ttbr_extra", "tcr",
                 "root", "foreign", "pmgr_stuck", "rtkit", "no_fe", "mbox_full", "no_init",
                 "no_kbd_desc", "kbd_desc_late", "layout", "no_ack", "ack_error", "no_ready",
                 "gpio", "csum_latch", "rtkit_latch", "ring_latch", "budget_wake", "mbox_busy",
                 "gpio_with_ack", "doorbell_owed", "tp_happy", "tp_params", "tp_no_init",
                 "tp_enable", "tp_afe_assert", "tp_fw_no_ack", "tp_fw_error", "tp_power_error",
                 "tp_afe_release", "tp_no_ready", "tp_no_geometry", "tp_latch", "tp_silence"]),
}


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def py_function(text, signature):
    start = text.index(signature)
    end = text.find("\ndef ", start + 1)
    return text[start:end if end > 0 else len(text)]


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


def test_trackpad_numbers(replays):
    # T2: acceleration is real (fast > 2x slow) and saturates at 1 + accel exactly; P1: the
    # firmware buffer is mapped after the RTKit pool and its guard page, with the rings' tables.
    _, cases, _ = replays["tp"]
    accel = cases["accel"]
    assert accel["fast"] > 2 * accel["slow"] and accel["saturated"] == 480
    assert cases["fuzz"]["iterations"] == 200000 and cases["fuzz"]["frames"] > 10000
    _, cases, _ = replays["bringup"]
    tp = cases["tp_happy"]
    assert tp["fw_dva"] == "0x10040514000" and tp["afe_calls"] == 2 and tp["tables"] == 2


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


def test_no_nvram_no_tlbi_and_one_smc_write():
    for name in NEW_FILES:
        text = re.sub(r"/\*.*?\*/", "", source(name), flags=re.S)
        for word in ("nvram", "NVRAM", "tlbi", "dart_init", "dart_map(", "dart_tlb"):
            assert not re.search(r"(?<!\w)" + re.escape(word), text), (name, word)
        if name != "hv_mtp_t8152.c":
            # The pure files reach the AFE reset only through ops->afe_reset.
            for word in ("smc_", "SMC_", "pcIO"):
                assert not re.search(r"(?<!\w)" + re.escape(word), text), (name, word)
    # The glue: one SMC write, of the pcIO key, with the ADT-checked AFE template only.
    glue = re.sub(r"/\*.*?\*/", "", source("hv_mtp_t8152.c"), flags=re.S)
    writes = re.findall(r"smc_write_\w+\(([^;]*)\);", glue)
    assert writes == ["smc, MTP_AFE_KEY, value"], writes
    assert re.search(r"#define MTP_AFE_KEY\s+0x7063494fu", glue)  # 'pcIO'
    assert re.search(r"#define MTP_AFE_LO\s+0x10000u", glue)
    assert re.search(r"#define MTP_AFE_HI\s+0x1c0000u", glue)
    afe = function_body(glue, "static bool op_afe_reset(void *ctx, bool assert)")
    assert "u64 value = (u64)MTP_AFE_HI << 32 | MTP_AFE_LO | (assert ? 1 : 0);" in afe
    # Every SMC wait is bounded: the session set-up (smc_init_timed) and each command.
    assert afe.index("if (!tp_wanted || hv_windows_guest_entered())") < \
        afe.index("smc_init_timed(MTP_SMC_INIT_US)")
    assert "smc_set_timeout(smc, MTP_SMC_TIMEOUT_US)" in afe
    assert re.search(r"#define MTP_SMC_INIT_US\s+2000000u", glue)
    attach = function_body(glue, "bool hv_mtp_t8152_attach(")
    # SMC is open only inside the bring-up; refused for good after it.
    assert attach.index("mtp_bringup_attach(") < attach.index("smc_close();") < \
        attach.rindex("tp_wanted = false;")
    assert glue.count("smc_init_timed(") == 1 and "smc_init()" not in glue
    assert glue.count("smc_shutdown(") == 1
    smc = (REPO / "src/smc.c").read_text()
    timed = function_body(smc, "smc_dev_t *smc_init_timed(u32 timeout_usec)")
    assert "rtkit_boot_or_adopt_timed(smc->rtkit, timeout_usec, &adopted)" in timed
    assert "(adopted || timeout_usec) && timeout_expired(timeout)" in timed
    rtkit = (REPO / "src/rtkit.c").read_text()
    adopt = function_body(rtkit, "bool rtkit_boot_or_adopt_timed(")
    assert "timeout_usec ? rtkit_boot_timed(rtk, timeout_usec) : rtkit_boot(rtk)" in adopt


def test_trackpad_bringup_order():
    # The order our Linux driver measured: enable, AFE asserted + 50 ms, firmware, power-on
    # announced, AFE released + 50 ms, power-on confirmed, READY; nothing after a failure.
    bring = source("mtp_bringup.c")
    tp = function_body(bring, "static int bring_up_trackpad(struct mtp_bringup *b)")
    order = ["u8 en[2] = {MTP_CMD_ENABLE, i};", "b->ops->dma_sync(",
             "b->ops->afe_reset(b->ops->ctx, true)", "settle(b, MTP_T_AFE_MS)",
             "MTP_CMD_FIRMWARE, 2, 0, i", "MTP_CMD_POWER, MTP_POWER_METHOD2, i, MTP_POWER_ON, 0",
             "b->ops->afe_reset(b->ops->ctx, false)", "pw[4] = 1;", "b->tp_live = true;",
             "wait_ready(b, i, MTP_T_READY_MS, MTP_E_TP_READY)", "TP_FEATURE_GEOMETRY"]
    at = [tp.index(x) for x in order]
    assert at == sorted(at), order
    assert tp.count("afe_reset(") == 2
    # Every step returns on failure before the next one.
    assert tp.count("if (rc)\n        return rc;") >= 6
    attach = function_body(bring, "int mtp_bringup_attach(")
    assert attach.index("mtp_params_parse(") < attach.index("map_rings(b)")
    assert attach.index("enable_keyboard(b)") < attach.index("bring_up_trackpad(b)")


def test_relative_descriptor_has_wheel_and_pan():
    # The trackpad's wheel and AC Pan go out as report ID 3; the Windows driver's descriptor
    # (m6input, INPUT-DESIGN 1.5) has both, so no driver change: the model's layout must match.
    model = source("input_model.h")
    assert "ID 3 relative   8 bytes: id, buttons 1-5, dX s16, dY s16 (-32767..32767), wheel, pan" \
        in model
    assert re.search(r"#define INPUT_LEN_RELATIVE\s+8u", model)


def test_run_rule_before_any_write():
    bring = source("mtp_bringup.c")
    attach = function_body(bring, "int mtp_bringup_attach(")
    assert "MTP_T8152_ASC_CPU_CTRL" not in attach
    assert attach.index("power_and_run_rule(b)") < attach.index("check_dart(b)") < \
        attach.index("map_rings(b)") < attach.index("ops->rtkit_start(") < \
        attach.index("start_scm(b)") < attach.index("wait_init(b)") < \
        attach.index("kbd_layout_from_desc(") < attach.index("enable_keyboard(b)")
    # ASC_CPU_CONTROL is read only with every MTP domain on: before any write when they all
    # were, else only after power_on() switched the off ones on.
    rule = function_body(bring, "static int power_and_run_rule(struct mtp_bringup *b)")
    assert rule.count("MTP_T8152_ASC_CPU_CTRL") == 2
    first, second = (m.start() for m in re.finditer("MTP_T8152_ASC_CPU_CTRL", rule))
    assert rule.index("b->ps_before[i] = b->ops->read32(") < rule.index("if (all_on) {") < \
        first < rule.index("int rc = power_on(b);") < rule.index("if (!all_on) {") < second
    assert "MTP_T8152_ASC_CPU_CTRL" not in function_body(bring, "static int power_on(")
    # Only the keyboard (and, later, multi-touch) is ever enabled.
    assert bring.count("MTP_CMD_ENABLE") == 2
    assert '"keyboard"' in bring and '"multi-touch"' in bring
    assert '"stm"' not in bring and '"actuator"' not in bring and '"tp-accel"' not in bring


def test_rings_noncacheable_and_area_denied():
    glue = source("hv_mtp_t8152.c")
    prep = function_body(glue, "static bool prepare_area(void)")
    assert prep.index("dc_civac_range((void *)rings, size)") < \
        prep.index("mmu_add_mapping(rings, rings, size, MAIR_IDX_NORMAL_NC, PERM_RW)")
    for alias in ("REGION_RWX_EL0", "REGION_RW_EL0", "REGION_RX_EL1"):
        assert f"mmu_add_mapping(rings | {alias}, rings, size, MAIR_IDX_NORMAL_NC" in prep
    # The carrier's own EL2&0 translations are invalidated (mmu_add_mapping's vmalle1is only
    # reaches EL1&0 with TGE clear), every ring page is checked to translate Normal-NC (0x44),
    # and only then are the rings cleaned again and zeroed.
    last_map = prep.index("mmu_add_mapping(rings | REGION_RX_EL1")
    assert last_map < prep.index("mmu_flush_own_tlb();") < prep.index("mmu_probe_attr(") < \
        prep.index("memset((void *)rings, 0, size)")
    assert "attr != MTP_ATTR_NORMAL_NC" in prep and "return false;" in prep
    assert re.search(r"#define MTP_ATTR_NORMAL_NC 0x44", glue)
    mem = (REPO / "src/memory.c").read_text()
    flush = function_body(mem, "void mmu_flush_own_tlb(void)")
    assert 'if (mrs(CurrentEL) == 8)\n        sysop("tlbi alle2is");' in flush
    probe = function_body(mem, "int mmu_probe_attr(u64 va)")
    assert "at s1e2r" in probe and "msr(PAR_EL1, save);" in probe and "return (int)(par >> 56);" in probe
    attach = function_body(glue, "bool hv_mtp_t8152_attach(")
    assert attach.index("identity()") < attach.index("hv_t8152_reserve_input_area(") < \
        attach.index("asc_init(") < attach.index("if (!prepare_area()) {") < \
        attach.index("mtp_bringup_attach(")
    refuse = attach[attach.index("if (!prepare_area()) {"):attach.index("attached = true;")]
    assert "mb.reason = MTP_E_AREA;" in refuse and "return false;" in refuse
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
    assert "mtp_scm.o mtp_dart_t8152.o hid_desc.o input_kbd.o input_tp.o mtp_bringup.o " \
        "hv_mtp_t8152.o) \\" in make


# ---- I2 launcher side: the area, the firmware container, the parameter page -----------------

def _windows():
    sys.path.insert(0, str(REPO / "proxyclient"))
    from aura import windows
    return windows


def test_input_area_plan():
    w = _windows()
    assert w.plan_t8152_input_area(0x9_0000_0000) == (0x8_FF80_0000, 0x8_FF80_0000)
    # An unaligned top: the area ends at the 2 MiB boundary below it.
    assert w.plan_t8152_input_area(0x9_0012_3456) == (0x8_FF80_0000, 0x8_FF80_0000)
    with pytest.raises(ValueError):
        w.plan_t8152_input_area(0x700000)
    assert (w.INPUT_AREA_SIZE, w.INPUT_AREA_FW, w.INPUT_AREA_FW_SIZE) == (0x800000, 0x700000,
                                                                          0x20000)
    hdr = source("mtp_bringup.h")
    assert re.search(r"#define MTP_AREA_SIZE\s+0x800000ull", hdr)
    assert re.search(r"#define MTP_AREA_FW\s+0x700000u", hdr)
    assert re.search(r"#define MTP_AREA_FW_SIZE\s+0x20000u", hdr)


def _container(payload, iface_off=0x10, magic=0x46444948, version=1, hdr_len=32, data_len=None,
               trailer=b""):
    import struct
    data_len = len(payload) if data_len is None else data_len
    head = struct.pack("<IIIII", magic, version, hdr_len, data_len, iface_off)
    return head + bytes(max(0, hdr_len - len(head))) + payload + trailer


def test_tp_firmware_container():
    # Synthetic containers only: the owner's real file never enters a test or a repo.
    w = _windows()
    payload = bytes((i * 13 + 5) & 0xFF for i in range(5000))
    assert w.parse_tp_firmware(_container(payload)) == (payload, 0x10)
    assert w.parse_tp_firmware(_container(payload, trailer=b"\0" * 8))[0] == payload
    for bad in (_container(payload, magic=0x46444947), _container(payload, version=2),
                _container(payload, hdr_len=16), _container(payload, data_len=6000),
                _container(payload, iface_off=5000), _container(b"\1" * 0x20001),
                _container(payload)[:19], b""):
        with pytest.raises(ValueError):
            w.parse_tp_firmware(bad)
    assert str(w.default_tp_firmware_path({"J804_PRIVATE_DATA": "/x"})) == \
        "/x/firmware/tpmtfw-j804.bin"
    assert w.default_tp_firmware_path({}) is None


def test_param_page_read_by_the_carrier(tmp_path):
    # The launcher's page, parsed by the carrier's own C parser.
    w = _windows()
    cc = clang()
    out = tmp_path / "params"
    out.mkdir()
    (out / "types.h").write_text(TYPES_H)
    for name in PURE:
        shutil.copy2(WIN / name, out / name)
    binary = out / "params-host"
    build = subprocess.run(
        [cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror", "-DMTP_HOST_TEST",
         "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-iquote", str(out),
         *[str(out / n) for n in ("mtp_bringup.c", "mtp_scm.c", "mtp_dart_t8152.c", "hid_desc.c",
                                  "input_kbd.c", "input_tp.c")],
         str(REPO / "tests/e2e/windows_mtp_params_host.c"), "-o", str(binary)],
        capture_output=True, text=True, timeout=300)
    assert build.returncode == 0, build.stderr[-3000:]
    payload = bytes((i * 7 + 1) & 0xFF for i in range(79928))
    (out / "payload.bin").write_bytes(payload)

    def parse(page):
        (out / "page.bin").write_bytes(page)
        run = subprocess.run([str(binary), str(out / "page.bin"), str(out / "payload.bin")],
                             capture_output=True, text=True, timeout=60)
        assert run.returncode == 0, run.stderr
        return json.loads(run.stdout)

    got = parse(w.input_params(payload, 0x24))
    assert got == {"result": "accepted", "fw_size": 79928, "fw_crc_ok": True, "iface_off": 0x24,
                   "gain_q8": 1536, "accel_q8": 768, "accel_lo": 15, "accel_hi": 150,
                   "scroll_um": 5000, "tap_ms": 180, "tap_travel_um": 1500,
                   "tap_key_quiet_ms": 300, "click_settle_ms": 40, "palm_major": 0,
                   "tap": True, "natural": False}
    opts = w.TpOptions(tap=False, natural=True, gain=4.5, accel=0, scroll_mm=8)
    got = parse(w.input_params(payload, 0, opts))
    assert (got["tap"], got["natural"], got["gain_q8"], got["accel_q8"], got["scroll_um"]) == \
        (False, True, 1152, 0, 8000)
    # Any flipped byte is refused (the page's CRC), and so is a wrong firmware offset.
    page = bytearray(w.input_params(payload, 0x24))
    for at in (0, 8, 16, 20, 36, 59, 63):
        bad = bytearray(page)
        bad[at] ^= 0x01
        assert parse(bytes(bad))["result"] == "refused", at
    for call in (lambda: w.input_params(payload, len(payload)),
                 lambda: w.input_params(b"", 0),
                 lambda: w.input_params(payload, 0, w.TpOptions(gain=0.01)),
                 lambda: w.input_params(payload, 0, w.TpOptions(scroll_mm=0.1))):
        with pytest.raises(ValueError):
            call()


def test_provenance_of_new_files():
    for name in NEW_FILES:
        text = source(name)
        assert text.startswith("/* SPDX-License-Identifier: GPL-2.0-only */"), name
        for foreign in ("Copyright", "AuroraSilicon", "x1n1", "Neo", "/Users/",
                        "j804-private-data", "HIDF"):
            assert foreign not in text, (name, foreign)
    for name in ("windows_mtp_scm_host.c", "windows_mtp_dart_host.c",
                 "windows_input_kbd_host.c", "windows_mtp_bringup_host.c",
                 "windows_input_tp_host.c", "windows_mtp_params_host.c"):
        text = (REPO / "tests/e2e" / name).read_text()
        assert text.startswith("/* SPDX-License-Identifier: GPL-2.0-only */")
        for foreign in ("Copyright", "/Users/", "j804-private-data", "HIDF"):
            assert foreign not in text, (name, foreign)


# ---- review fixes: the launch-time RUN check, the RTKit preset, the trackpad's silent lift ----

class _FakeMtpRegs:
    """PMGR states and ASC_CPU_CONTROL as the M6 would answer; an ASC read with any domain off
    is the fault the launcher must never cause."""

    def __init__(self, states, ctrl):
        self.states, self.ctrl, self.reads, self.unpowered = states, ctrl, [], 0

    def read32(self, addr):
        w = _windows()
        self.reads.append(addr)
        if addr == w.MTP_ASC_CPU_CONTROL:
            if any((v >> 4) & 0xF != 0xF for v in self.states):
                self.unpowered += 1
                return 0xFFFFFFFF
            return self.ctrl
        index, rem = divmod(addr - w.MTP_PMGR_PS, w.MTP_PMGR_STRIDE)
        assert rem == 0 and 0 <= index < w.MTP_PMGR_COUNT, hex(addr)
        return self.states[index]


def test_launch_mtp_run_check():
    w = _windows()
    hdr = source("mtp_bringup.h")
    assert int(re.search(r"#define MTP_T8152_PMGR_PS\s+(0x[0-9a-f]+)ull", hdr).group(1), 16) == \
        w.MTP_PMGR_PS
    assert w.MTP_ASC_CPU_CONTROL == 0x314600000 + 0x44 and w.MTP_RUN == 0x10
    fresh = [0x0 if i in (6, 10, 11) else 0xFF for i in range(13)]  # as iBoot leaves it
    regs = _FakeMtpRegs(fresh, 0x10)
    assert w.t8152_mtp_running(regs.read32)[0] is False
    assert w.MTP_ASC_CPU_CONTROL not in regs.reads and regs.unpowered == 0
    regs = _FakeMtpRegs([0xFF] * 13, 0x10)  # a session left it running
    running, detail = w.t8152_mtp_running(regs.read32)
    assert running and "0x10" in detail and regs.unpowered == 0
    regs = _FakeMtpRegs([0xFF] * 13, 0x0)  # powered by a failed attach, never started
    assert w.t8152_mtp_running(regs.read32)[0] is False
    # Every J804 launch and every chainload checks it, read-only, before anything changes.
    rg = (REPO / "proxyclient/tools/run_guest.py").read_text()
    check = py_function(rg, "def windows_t8152_mtp_idle_check(u):")
    assert "w.t8152_mtp_running(p.read32)" in check and "sys.exit(" in check
    assert "write" not in check
    run = py_function(rg, "def run_windows_t8152(caps):")
    assert run.index("chip_id != T8152_CHIP") < run.index("windows_t8152_mtp_idle_check(u)") < \
        run.index("windows_t8152_usb_attach(")
    cl = (REPO / "proxyclient/tools/chainload.py").read_text()
    assert cl.index("t8152_mtp_running(p.read32)") < cl.index("new_base = u.base")


def test_rtkit_premapped_preset_replaced():
    rtkit = (REPO / "src/rtkit.c").read_text()
    req = function_body(rtkit, "static bool rtkit_handle_buffer_request(")
    assert req.index("rtk->windows_mode && addr && rtk->pool_premapped") < \
        req.index("rtk->windows_mode && addr) {")
    oslog = function_body(rtkit, "static bool rtkit_handle_oslog_request(")
    assert "rtk->windows_mode && addr && rtk->pool_premapped" in oslog


def test_fn_usages_and_silent_tap_sources():
    kbd = source("input_kbd.h")
    for usage in ("0x00ff0003u", "0xff000003u", "0xff0c0003u"):
        assert usage in kbd
    tp = source("input_tp.c")
    idle = function_body(tp, "int tp_idle(")
    assert "!s->lifts && s->session" in idle and "tap_button(s, s->t_prev)" in idle
    assert re.search(r"#define MTP_FRAME_MAX\s+\(MTP_FRAME_HDR \+ 0xfffcu \+ MTP_FRAME_CSUM\)",
                     source("mtp_scm.h"))


def test_launcher_probe_step_a():
    # Step A (carrier only): PROBE in slices of at most 10 s after ATTACH; --windows-input-probe-only
    # closes the NVMe disk with its verdict and stops before hv.start().
    rg = (REPO / "proxyclient/tools/run_guest.py").read_text()
    probe = py_function(rg, "def windows_t8152_input_probe(sources):")
    assert "min(10000, left_ms)" in probe and "w.INPUT_OP_PROBE" in probe
    assert "return args.windows_input_probe_only" in probe
    area = py_function(rg, "def windows_t8152_input_area(top):")
    assert "1 <= args.windows_input_probe <= 120" in area
    assert "--windows-input-probe-only needs --windows-input-probe" in area
    run = py_function(rg, "def run_windows_t8152(caps):")
    assert run.index("windows_t8152_input_attach(hv, input_sources") < \
        run.index("probe_only = windows_t8152_input_probe(input_sources)") < \
        run.index("if probe_only:") < run.index("windows_t8152_ahci_nvme_close(-1, ahci, hv)") < \
        run.index("result = hv.start()")
    hdr = source("hv_input.h")
    assert "HV_INPUT_OP_PROBE = 4" in hdr
