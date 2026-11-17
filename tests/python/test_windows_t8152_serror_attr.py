# SPDX-License-Identifier: MIT
"""i0 session 2: the guest SError attribution and the launcher's SError policy, host only.

i0 session 2 (2026-11-17) ended ~500 s after entry on a lower-EL SError (ESR ISS 0) on the
boot CPU with nothing printed.  The carrier now reports it (test_windows_t8152_serror.py, R9 and
R10); this file covers the part that asks whether a guest access to a passthrough or identity
region could have raised it (src/hv/windows/hv_serror.c), and run_guest --windows-serror.

Failure modes, written before the code:
  A1-A5 the software stage-1 walk, the load/store decode, the IPA classification and the report
        (tests/e2e/windows_serror_host.c, hv_serror.c compiled unchanged under ASan/UBSan);
  A6 the attribution uses an AT instruction (D3 run 1 took an EL2 SError at one) or reads a
     guest descriptor outside guest RAM;
  A7 the attribution runs before the records and registers are out (it is the riskiest part);
  P1 the default policy changes (anything but abort), or "continue" resumes something that is
     not a guest SError;
  P2 "continue" is unbounded, or does not log the frame first;
  P3 run_guest does not pass the choice to the HV object.
Register values and addresses are synthetic (PROVENANCE.md).  Nothing here touches hardware.
"""

import json
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import types

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"

CASES = ["walk", "decode", "classify", "report_device", "report_none"]

STUBS = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned long u64;
typedef long s64;
typedef uint32_t u32;
typedef int32_t s32;
typedef uint16_t u16;
typedef uint8_t u8;
''',
    "soc.h": '#pragma once\n#include "types.h"\n#define T8152 0x8152\nextern u32 chip_id;\n',
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
''',
}


def source(path):
    return (REPO / path).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def code(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


@pytest.fixture(scope="module")
def replay(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    out = tmp_path_factory.mktemp("serror")
    for name, text in STUBS.items():
        (out / name).write_text(text)
    for name in ("hv_serror.c", "hv_serror.h", "hv_t8152.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "serror-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-iquote", str(out), str(REPO / "tests/e2e/windows_serror_host.c"),
                            "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    return subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)


def report(result, name):
    text = result.stdout
    start = text.index(f"report-begin {name}\n")
    return text[start:text.index(f"report-end {name}\n", start)]


def test_replay_passes(replay):
    assert replay.returncode == 0, replay.stdout[-4000:] + replay.stderr[-4000:]
    cases = [json.loads(line) for line in replay.stdout.splitlines() if line.startswith("{")]
    assert [c["case"] for c in cases] == CASES
    assert all(c["result"] == "passed" for c in cases)


def test_report_finds_the_xhci_load_and_the_registers(replay):
    text = report(replay, "report_device")
    assert "attribution: architectural signal: none (ESR ISS 0: no class, no address; " \
           "FAR/HPFAR are not written)" in text
    assert "guest PC 0xfffff80000101004 -> IPA 0x80100004: guest RAM" in text
    # The load just before the PC, through x3, into the xHCI passthrough.
    assert ("attribution: PC-0x4 insn b9402060 load via x3 VA 0xffffd00000030020 -> IPA "
            "0x40a430020: MMIO passthrough (Device-nGnRnE, reaches hardware) "
            "(usb-drd1 xHCI (D4))") in text
    assert "CANDIDATE PC-0x4 insn b9402060 load via x3 -> IPA 0x40a430020 (usb-drd1 xHCI (D4))" \
        in text
    # The instruction on the previous page is walked too (RAM: printed, not a candidate).
    assert "PC-0x1c insn f9400420 load via x1 VA 0xfffff80000400108 -> IPA 0x80400108: " \
           "guest RAM" in text
    assert "x3 VA 0xffffd00000030000 -> IPA 0x40a430000: MMIO passthrough" in text
    assert "x5 0x301000000 is itself the address of aic (native AIC passthrough) " \
           "(MMIO passthrough)" in text
    assert "x7 VA 0xffffd00000040000 -> IPA 0xf40000000: emulated by a carrier hook (traps) " \
           "(carrier hook)" in text
    assert "x8 VA 0xffffd00000050000 -> IPA 0x10a0000000: registered identity exception, not " \
           "mapped (would trap) (gpu fw data)" in text
    assert "CANDIDATE x7" not in text and "CANDIDATE x8" not in text
    assert "attribution: x1 " not in text and "SP_EL1" not in text  # RAM: not listed
    assert text.rstrip().endswith("attribution: 2 candidate(s) reach a passthrough or identity "
                                  "region; a guest device access is possible (not proven: the "
                                  "SError has no address)")


def test_report_without_a_candidate(replay):
    text = report(replay, "report_none")
    assert "CANDIDATE" not in text
    assert text.rstrip().endswith("no guest address near the SError reaches a passthrough or "
                                  "identity region (weighs against a guest device access)")
    assert report(replay, "off") == "report-begin off\n"


# ---- source rules -------------------------------------------------------------------------


def test_a6_no_at_and_reads_only_guest_ram():
    src = code(source("src/hv/windows/hv_serror.c"))
    assert not re.search(r'asm[^;]*"\s*at\s', src)
    assert "hv_translate" not in src
    rd = function_body(src, "static bool rd_guest_ram(void *ctx, u64 ipa, u64 *out)")
    assert rd.index("ipa < rd_guest_ram_base || ipa >= rd_guest_ram_top") < rd.index("read64(")
    insn = function_body(src, "static bool read_insn(u64 ipa, u32 *out)")
    assert insn.index("ipa < rd_guest_ram_base || ipa >= rd_guest_ram_top") < insn.index("read32(")
    # Stage-2 lookups stay inside the table's range (hv_pt_walk asserts otherwise).
    assert "return ipa < BIT(vaddr_bits) ? hv_pt_walk(ipa) : 0;" in src
    assert "hv_serror.o" in source("Makefile")


def test_a7_attribution_comes_last_in_the_guest_report():
    body = code(function_body(source("src/hv/windows/hv_fatal.c"),
                              "void hv_windows_guest_serror_report("))
    assert body.index("hv_windows_fatal_report(NULL, \"guest SError\"") < \
        body.index('printf("HV: fatal: guest x%d-x%d:"') < \
        body.index("hv_serror_attribute(regs, sp, esr, elr, spsr);") < \
        body.index("__atomic_store_n(&hv_fatal_reporting, 0, __ATOMIC_RELEASE);")


def test_policy_lookup_reads_the_lists_only():
    body = code(function_body(source("src/hv/windows/hv_t8152.c"),
                              "int hv_t8152_describe_ipa(u64 ipa, const char **name)"))
    assert "read" not in body and "hv_pt_walk" not in body and "printf" not in body
    assert body.index("if (!armed)") < body.index("ram_start")


# ---- the launcher policy -----------------------------------------------------------------


def _hv(policy):
    sys.path.insert(0, str(REPO / "proxyclient"))
    from aura.hv import HV
    from aura.proxy import EXC, EXC_RET, START, ExcInfo

    hv = HV.__new__(HV)
    hv.windows = True
    hv.windows_serror = policy
    hv.windows_serrors_continued = 0
    hv.windows_abort_reason = None
    frame = struct.pack("<32Q", *range(32)) + struct.pack(
        "<14Q", 0x60000005, 0xfffff80000101004, 0xbe000000, 0, 0, 0x7ff0, 0xfffff80000400000,
        0x10008402fa0, 6, 0x80010100, 0, 0, 0, 0)
    assert len(frame) == ExcInfo.sizeof()
    exits, lines = [], []
    hv.iface = types.SimpleNamespace(readmem=lambda addr, size: frame[:size])
    hv.p = types.SimpleNamespace(exit=exits.append)
    hv.log = lines.append
    return hv, exits, lines, (EXC, EXC_RET, START)


def test_p1_default_is_abort():
    hv, exits, lines, (EXC, EXC_RET, START) = _hv("abort")
    hv.handle_exception(START.EXCEPTION_LOWER, EXC.SERROR, 0x10008402fa0)
    assert exits == [EXC_RET.EXIT_GUEST]
    assert hv.windows_abort_reason == {"reason": int(START.EXCEPTION_LOWER),
                                       "code": int(EXC.SERROR), "info": "0x10008402fa0"}
    assert any("Guest exception context: cpu 6" in line for line in lines)
    run_guest = source("proxyclient/tools/run_guest.py")
    assert "parser.add_argument('--windows-serror', choices=('abort', 'continue'), " \
           "default='abort'," in run_guest
    assert re.search(r'self\.windows_serror = "abort"\n', source("proxyclient/aura/hv/__init__.py"))


def test_p1_continue_resumes_only_guest_serrors():
    hv, exits, lines, (EXC, EXC_RET, START) = _hv("continue")
    hv.handle_exception(START.EXCEPTION_LOWER, EXC.SERROR, 0x10008402fa0)
    assert exits == [EXC_RET.HANDLED]
    assert hv.windows_abort_reason is None
    assert lines[0].startswith("Guest SError 1/8 at 0x10008402fa0: --windows-serror continue")
    assert any("Guest exception context: cpu 6 MPIDR 0x80010100 ESR_EL2 0xbe000000" in line
               for line in lines)  # P2: the frame is logged before resuming
    # A synchronous guest exception, or an EL2 one, still aborts.
    hv.handle_exception(START.EXCEPTION_LOWER, EXC.SYNC, 0x10008402fa0)
    assert exits[-1] == EXC_RET.EXIT_GUEST
    hv2, exits2, _, _ = _hv("continue")
    hv2.handle_exception(START.EXCEPTION, EXC.SERROR, 0x10008402fa0)
    assert exits2 == [EXC_RET.EXIT_GUEST]


def test_p2_continue_is_bounded():
    hv, exits, lines, (EXC, EXC_RET, START) = _hv("continue")
    for _ in range(hv.WINDOWS_SERROR_CONTINUE_MAX):
        hv.handle_exception(START.EXCEPTION_LOWER, EXC.SERROR, 0x10008402fa0)
    assert exits == [EXC_RET.HANDLED] * 8
    hv.handle_exception(START.EXCEPTION_LOWER, EXC.SERROR, 0x10008402fa0)
    assert exits[-1] == EXC_RET.EXIT_GUEST
    assert any("8 already continued this session (limit 8); aborting" in line for line in lines)


def test_p3_run_guest_passes_the_choice():
    run_guest = source("proxyclient/tools/run_guest.py")
    body = run_guest[run_guest.index("def run_windows_t8152(caps):"):]
    assert body.index("hv = HV(iface, p, u, windows=True)") < \
        body.index("hv.windows_serror = args.windows_serror") < body.index("hv.init()")
