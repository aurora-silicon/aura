# SPDX-License-Identifier: GPL-2.0-only
"""D3 run 1: the T8152 EL2 SError on the host tick, host only.

D3 run 1 (2026-11-17) died 388 s after entry with "Exception: SError / taken from EL2h" on
cpu6 at rel 0x8bb30, the instruction after "at s12e0r" in hv_translate().  The disassembly
and the register dump put that AT in hv_recover_t8142_sysreg_undef() on the host tick,
walking the guest's interrupted EL0 PC; FAR was stale from an earlier AHCI PxCI abort and
x12-x16 were the framebuffer converter's leftovers.  The cause is not isolated: SErrors are
asynchronous, and an error pending from the converter's scanout stores earlier in the tick is
taken at that AT too, so T1 below is hygiene and the fences (T3, T4) are what tell the stages
apart next time.  windows-m6/D3-RESULTS.md has the run.  Register values in the replay are
synthetic (PROVENANCE.md), not copied from the run log.

Failure modes, written before the code (D3-RESULTS.md section 4; T7 and T8 added with the
review fixes):
  T1 the tick still walks a user PC through TTBR0 when the guest was interrupted in EL0;
  T2 kernel-VA reads made from the tick use the EL0 regime and silently fail;
  T3 an SError pending from earlier EL2 work is taken after the next AT and blamed on it;
  T4 an SError from a tick stage is taken in a later stage (no fence between stages), or one
     pending from the idle path's trap handling is taken inside the next converter band;
  T5 a stage-1 walk into the AIC Device mapping becomes a real AIC read (HCR_EL2.PTW clear);
  T6 the fatal report runs too late (after the reboot) or on another chip, or PAR_EL1 is
     overwritten before the report reads it, or is labelled "at entry" when it is not;
  T7 an exit vector of the built carrier (ENABLE_VGIC_MODULE) records nothing;
  T8 the records are printed after a register read that can trap and end the report;
  plus R1-R8 of the report itself, replayed by tests/e2e/windows_fatal_host.c (hv_fatal.c
  compiled unchanged with the host clang under ASan/UBSan).
The AHCI trace (last access, last command and its spans) is replayed by the store harness in
test_windows_t8152_ahci.py; HCR_EL2.PTW by the policy harness in test_windows_t8152_policy.py.
Nothing here touches hardware.  The artifact is the replay transcript.
"""

import json
import os
from pathlib import Path
import re
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"

CASES = ["gating", "reentry", "unwind", "seqlock", "report_d3", "report_ras",
         "report_guest", "report_return", "report_icc"]

STUBS = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
''',
    "smp.h": r'''
#pragma once
#include "types.h"
#define MAX_CPUS 16
extern int stub_cpu;
static inline int smp_id(void) { return stub_cpu; }
''',
    "soc.h": '#pragma once\n#include "types.h"\n#define T8152 0x8152\nextern u32 chip_id;\n',
    "hv_ahci.h": '#pragma once\n#include "types.h"\nvoid hv_ahci_fatal_report(u64 now, u64 freq);\n',
    "hv_usb.h": '#pragma once\nvoid hv_usb_fatal_report(void);\n',
    "hv_gpu.h": '#pragma once\nvoid hv_gpu_fatal_report(void);\n',
    "hv_input.h": '#pragma once\nvoid hv_input_fatal_report(void);\n',
    "hv_scanout.h": '#pragma once\nvoid hv_scanout_fatal_report(void);\n',
    "hv_serror.h": '#pragma once\n#include "types.h"\n'
                   'void hv_serror_attribute(const u64 *regs, const u64 *sp, u64 esr, u64 elr, '
                   'u64 spsr);\n',
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
extern u64 stub_par, stub_hpfar, stub_cntpct, stub_cntfrq, stub_esr, stub_far, stub_isr;
extern u64 stub_pfr0, stub_disr, stub_elr12, stub_esr12, stub_far12, stub_spsr12;
extern u64 stub_elr2, stub_spsr2;
u64 stub_read_disr(void);
void stub_sysop(const char *op);
#define mrs(r) stub_mrs_##r
#define stub_mrs_PAR_EL1 stub_par
#define stub_mrs_HPFAR_EL2 stub_hpfar
#define stub_mrs_CNTPCT_EL0 stub_cntpct
#define stub_mrs_CNTFRQ_EL0 stub_cntfrq
#define stub_mrs_ESR_EL2 stub_esr
#define stub_mrs_FAR_EL2 stub_far
#define stub_mrs_ISR_EL1 stub_isr
#define stub_mrs_ID_AA64PFR0_EL1 stub_pfr0
#define stub_mrs_DISR_EL1 stub_read_disr()
#define stub_mrs_ELR_EL12 stub_elr12
#define stub_mrs_ESR_EL12 stub_esr12
#define stub_mrs_FAR_EL12 stub_far12
#define stub_mrs_SPSR_EL12 stub_spsr12
#define stub_mrs_ELR_EL2 stub_elr2
#define stub_mrs_SPSR_EL2 stub_spsr2
#define sysop(op) stub_sysop(op)
extern char stub_base_storage[];
#define _base stub_base_storage
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
    out = tmp_path_factory.mktemp("fatal")
    for name, text in STUBS.items():
        (out / name).write_text(text)
    for name in ("hv_fatal.c", "hv_fatal.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "fatal-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-iquote", str(out), str(REPO / "tests/e2e/windows_fatal_host.c"),
                            "-lpthread", "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)
    artifact = os.environ.get("AURA_FATAL_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text(result.stdout)
    return result


def report(result, name):
    text = result.stdout
    start = text.index(f"report-begin {name}\n")
    return text[start:text.index(f"report-end {name}\n", start)]


def test_replay_passes(replay):
    assert replay.returncode == 0, replay.stdout[-4000:] + replay.stderr[-4000:]
    cases = [json.loads(line) for line in replay.stdout.splitlines() if line.startswith("{")]
    assert [c["case"] for c in cases] == CASES
    assert all(c["result"] == "passed" for c in cases)


def test_report_on_the_d3_state(replay):
    text = report(replay, "report_d3")
    assert "HV: fatal: SError at EL2 on cpu 6; T8152 carrier context follows" in text
    # PAR as read on entry (F=0: a successful walk; synthetic PA inside the synthetic window).
    assert "PAR_EL1 0xff00010123456b80 at entry: F=0 PA 0x10123456000 attr 0xff" in text
    assert "ESR_EL2 0xbe000000: uncategorized (DFSC 0): no architectural classification" in text
    assert "DISR_EL1 absent (no FEAT_RAS)" in text
    assert "FAR_EL2 0xffffa50012400138 HPFAR_EL2 0xf400000 (IPA 0xf40000138)" in text
    assert "EL2 lr rel 0x40124; frames (rel): 0x40a58\n" in text
    # The interrupted CPU, marked, with the EL0 PC, the stage and the finished band.
    assert "cpu 6* last trap F 100 us ago: ESR 0x93c08046 PC 0x7ff600001230 EL0t\n" in text
    assert re.search(r"cpu 6\* EL2 stage R \d+ us ago; fb rows 1800\.\.1864 done \d+ us ago; "
                     r"crumbs XxFX\n", text)
    assert "cpu 3  EL2 crumbs \n" in text  # nothing recorded beyond the trap
    # A data abort carries its IPA; an HVC and a FIQ do not print a FAR.
    assert re.search(r"cpu 3  last trap S \d+ us ago: ESR 0x93c08046 PC 0xfffff80000101230 EL1h "
                     r"FAR 0xffffa50012400138 IPA 0xf40000138\n", text)
    assert re.search(r"cpu 7  last trap f \d+ us ago: ESR 0x5a000000 PC 0x7ff600000000 EL0t\n",
                     text)
    assert "fb rows 64..224 IN PROGRESS" in text
    assert "HV: fatal: AHCI stub report" in text
    assert text.rstrip().endswith("not contained: the carrier does not resume after this "
                                  "(SError at EL2)")
    # Only CPUs that recorded something are listed.
    assert "cpu 0" not in text and "cpu 1 " not in text
    # The records come out before the reads most likely to trap (DISR_EL1, the _EL12 set).
    assert text.index("cpu 6* EL2") < text.index("AHCI stub report") < text.index("EL2 lr rel") \
        < text.index("DISR_EL1") < text.index("guest EL1 ELR_EL12")


def test_report_on_a_guest_serror(replay):
    """i0 session 2: the guest SError is set against the previous exit of its CPU (kept from
    before the SError's own note), then every CPU's records, then the guest registers."""
    text = report(replay, "report_guest")
    assert ("HV: fatal: guest SError on cpu 6: ESR_EL2 0xbe000000 (uncategorized (DFSC 0): no "
            "architectural classification); guest PC 0xfffff80000101004 EL1h SPSR 0x60000005; "
            "FAR_EL2 0xffffa50012400138 HPFAR_EL2 0xf400000 (both stale: an SError writes "
            "neither)") in text
    assert "AFSR1_EL2 0x5a5a (IMPLEMENTATION DEFINED, raw); ISR_EL1 now 0x0 (A=0: nothing " \
           "else pending)" in text
    # The WFI exit of the replay never returned through _hv_return (no return record).
    assert "cpu 6 that exit has no return record" in text
    assert re.search(r"cpu 6 previous exit S 7 us before: ESR 0x4000001 PC 0xfffff80000101000 "
                     r"EL1h; ISR_EL1 at it 0x100\n", text)
    assert "cpu 6 SError vs previous exit: verdict 1: taken at that exit's return point and " \
           "already pending when it exited" in text
    assert "HV: fatal: guest SError on cpu 6; T8152 carrier context follows" in text
    assert "PAR_EL1 0x0 at the guest SError" in text
    # The all-CPU part shows the SError's own record on cpu 6 and the other CPU.
    assert re.search(r"cpu 6\* last trap E \d+ us ago: ESR 0xbe000000 PC 0xfffff80000101004 "
                     r"EL1h\n", text)
    assert re.search(r"cpu 0  last trap I \d+ us ago: ESR 0x0 PC 0xfffff80000300000 EL1h\n", text)
    assert re.search(r"cpu 6\* EL2 fb rows 400\.\.464 done \d+ us ago; SError pending after "
                     r"that band; crumbs", text)
    assert "HV: fatal: guest x0-x3: 0000000000000000 1111111111111111 2222222222222222 " \
           "3333333333333333\n" in text
    assert "HV: fatal: guest x28-x30: cccccccccccccccc dddddddddddddddd eeeeeeeeeeeeeeee\n" in text
    assert "HV: fatal: guest SP_EL0 0000000000007ff0 SP_EL1 fffff80000400000\n" in text
    # The carrier goes on after a guest SError: the "not contained" line is for EL2 errors.
    assert "not contained" not in text
    assert "HV: fatal: guest SError: the host decides what follows (run_guest --windows-serror: " \
           "abort exits the guest, continue resumes it)" in text
    assert text.index("previous exit") < text.index("carrier context follows") < \
        text.index("AHCI stub report") < text.index("USB stub report") < \
        text.index("GPU stub report") < text.index("input stub report") < \
        text.index("guest x0-x3") < text.index("attribution stub (sp 7ff0 fffff80000400000)")


def test_report_with_the_return_record(replay):
    """R10: the return fence turned an error of the WFI exit's EL2 work into ISR.A at the
    return; the SError was taken at that return point, so the verdict is EL2 work (code 2)."""
    text = report(replay, "report_return")
    assert re.search(r"cpu 6 that exit returned to PC 0xfffff80000101004 EL1h \(SPSR 0x60000005, "
                     r"A unmasked\) \d+ us before the SError; ISR_EL1 after the return fence "
                     r"0x100\n", text)
    assert "cpu 6 SError vs previous exit: verdict 2: taken at that exit's return point; not " \
           "pending when the guest exited, pending when EL2 returned: raised by the EL2 work " \
           "of that exit" in text


def test_every_return_to_the_guest_is_recorded_after_a_fence():
    asm = source("src/hv/windows/hv_asm.S")
    ret = asm[asm.index("_hv_return:"):asm.index("eret", asm.index("_hv_return:"))]
    assert ret.index("bl hv_fatal_note_return") < ret.index("ldp x0, x1, [sp], #16")
    # Every vector returns through _hv_return.
    for vec in ("_v_hv_sync", "_v_hv_irq", "_v_hv_fiq", "_v_hv_serr"):
        body = asm[asm.index(f"{vec}:"):]
        body = body[:body.index("b _hv_return")]
        assert "eret" not in body, vec
    note = code(function_body(source("src/hv/windows/hv_fatal.c"),
                              "void hv_fatal_note_return(void)"))
    assert note.index("if (chip_id != T8152)") < note.index('sysop("dsb sy");') < \
        note.index("t->ret_isr = mrs(ISR_EL1);")


def test_a_pending_serror_is_flagged_on_the_exit_record(replay):
    text = report(replay, "report_d3")
    assert "SError pending at that exit" not in text  # stub ISR 0 at every D3 exit
    assert "SError pending after that band" not in text


def test_guest_serror_vector_keeps_the_previous_exit_and_reports_before_the_proxy():
    body = code(function_body(source("src/hv/windows/hv_exc.c"),
                              "void hv_exc_serr(struct exc_info *ctx)"))
    snap = body.index("hv_fatal_trap_snapshot(&prev);")
    assert snap < body.index("hv_get_context(ctx);") < body.index("hv_fatal_note_trap(")
    rep = body.index("hv_windows_guest_serror_report(ctx->regs, ctx->sp, ctx->esr, ctx->elr, "
                     "ctx->spsr, ctx->far,")
    assert body.index("hv_exc_entry();") < rep < \
        body.index("hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);")
    note = code(function_body(source("src/hv/windows/hv_fatal.c"),
                              "void hv_fatal_note_trap(u64 kind, u64 esr, u64 elr, u64 spsr, "
                              "u64 far)"))
    assert note.index("t->isr = mrs(ISR_EL1);") < note.index("hv_fatal_seq_end(&t->seq);")


def test_host_logs_the_guest_exception_frame():
    import struct as st
    import sys
    import types
    sys.path.insert(0, str(REPO / "proxyclient"))
    from aura.hv import HV
    from aura.proxy import ExcInfo

    regs = [0x100 + i for i in range(32)]
    frame = st.pack("<32Q", *regs) + st.pack(
        "<14Q", 0x60000005, 0xfffff80000101004, 0xbe000000, 0xffffa50012400138, 0, 0x7ff0,
        0xfffff80000400000, 0x10008402fa0, 6, 0x80010100, 0x10100101004, 0, 0x10100400000, 0)
    assert len(frame) == ExcInfo.sizeof()
    code_words = st.pack("<5I", 1, 2, 3, 4, 5)
    reads = []

    def readmem(addr, size):
        reads.append((addr, size))
        return frame if addr == 0x10008402fa0 else code_words

    lines = []
    fake = types.SimpleNamespace(iface=types.SimpleNamespace(readmem=readmem), log=lines.append)
    HV._log_windows_exc_info(fake, 0x10008402fa0)
    text = "\n".join(lines)
    assert "cpu 6 MPIDR 0x80010100 ESR_EL2 0xbe000000 (EC 0x2f ISS 0x0) ELR 0xfffff80000101004 " \
           "(PA 0x10100101004) SPSR 0x60000005 (M 0x5)" in text
    assert "x28-x30: 000000000000011c 000000000000011d 000000000000011e" in text
    assert "code at PA 0x10100100ffc: 00000001 00000002 [00000003] 00000004 00000005" in text
    assert reads[1] == (0x10100100ffc, 20)

    # A failed read is logged, never raised: the guest exit must still go ahead.
    lines.clear()

    def broken(addr, size):
        raise OSError("link down")

    fake.iface.readmem = broken
    HV._log_windows_exc_info(fake, 0x10008402fa0)
    assert "could not read the frame at 0x10008402fa0" in lines[0]


def test_report_with_ras_and_a_walk_fault(replay):
    text = report(replay, "report_ras")
    assert "DISR_EL1 0x80000000" in text
    assert ("PAR_EL1 0x2b at entry: F=1 FST 0x15 (synchronous external abort on the table "
            "walk, level 1), stage 1") in text


def test_report_reentry_is_one_line(replay):
    text = report(replay, "reentry")
    assert text.count("carrier context follows") == 1
    assert "EL2 synchronous exception inside the fatal report; skipping it" in text


def test_seqlock_transcript(replay):
    m = re.search(r"seqlock: (\d+) stable, (\d+) busy or changing, (\d+) mixed", replay.stdout)
    assert m and int(m.group(1)) > 0 and m.group(3) == "0"


# ---- source rules -------------------------------------------------------------------------


def test_t1_tick_never_walks_a_user_pc_on_t8152():
    body = code(function_body(source("src/hv/windows/hv_exc.c"),
                              "bool hv_recover_t8142_sysreg_undef(struct exc_info *ctx)"))
    skip = body.index("if (chip_id == T8152 && !(FIELD_GET(SPSR_M, ctx->spsr) >> 2))\n"
                      "        return false;")
    assert skip < body.index("hv_absent_pmu_translate(ctx->elr, false, 4)")
    assert body.index("if (!hv_absent_pmuv3())") < skip


def test_t2_kernel_vas_use_the_el1_regime_on_t8152():
    body = code(function_body(source("src/hv/windows/hv_exc.c"),
                              "static u64 hv_absent_pmu_translate(u64 va, bool w, u64 len)"))
    assert "if (chip_id == T8152 && (va & BIT(55)))\n" \
           "        pa = hv_translate_el(va, false, w, 1, NULL);\n" \
           "    else\n" \
           "        pa = hv_translate(va, false, w, NULL);" in body
    vm = code(source("src/hv/hv_vm.c"))
    tr = function_body(vm, "u64 hv_translate(u64 addr, bool s1, bool w, u64 *par_out)")
    assert "return hv_translate_el(addr, s1, w, FIELD_GET(SPSR_M, hv_get_spsr()) >> 2, " \
           "par_out);" in tr
    el = function_body(vm, "u64 hv_translate_el(u64 addr, bool s1, bool w, u64 el, u64 *par_out)")
    assert "hv_get_spsr" not in el and el.count("if (el == 0)") == 4


def test_t3_translate_fences_before_the_at_on_t8152():
    el = code(function_body(source("src/hv/hv_vm.c"),
                            "u64 hv_translate_el(u64 addr, bool s1, bool w, u64 el, "
                            "u64 *par_out)"))
    fence = el.index('if (chip_id == T8152) {\n        sysop("dsb sy");\n        sysop("isb");')
    assert el.index("u64 save = mrs(PAR_EL1);") < fence < el.index('asm("at ')
    assert el.index('sysop("isb");', el.rindex('asm("at ')) < el.index("u64 par = mrs(PAR_EL1);")
    assert "#ifdef AURA_WINDOWS" in el  # other builds keep the old instruction stream


TICK_ORDER = [
    ("HV_FATAL_STAGE_FB", "hv_fb_convert_slice(HV_FB_SLICE_TICK);"),
    ("HV_FATAL_STAGE_REPORT", "hv_fb_report();"),
    ("HV_FATAL_STAGE_TRACK", "hv_track_t8142_undef_vector(ctx);"),
    ("HV_FATAL_STAGE_VERIFY", "hv_verify_t8142_undef_vectors();"),
    ("HV_FATAL_STAGE_MODULES", "hv_scan_t8142_guest_modules();"),
    ("HV_FATAL_STAGE_PROCESS", "hv_report_t8142_process(ctx);"),
    ("HV_FATAL_STAGE_MTP", "hv_windows_mtp_poll();"),
    ("HV_FATAL_STAGE_BUGCHECK", "hv_check_t8142_bugcheck();"),
    ("HV_FATAL_STAGE_RECOVER", "hv_recover_t8142_sysreg_undef(ctx);"),
    ("HV_FATAL_STAGE_IO", "iodev_handle_events(uartproxy_iodev);"),
]


def test_t4_every_tick_stage_is_fenced_and_named():
    hv = source("src/hv/windows/hv.c")
    io = function_body(hv, "static void hv_tick_host_io(struct exc_info *ctx)")
    tick = code(function_body(hv, "void hv_tick(struct exc_info *ctx)").replace(
        "hv_tick_host_io(ctx);", io))
    pos = 0
    for stage, call in TICK_ORDER:
        mark = tick.index(f"hv_fatal_stage({stage});", pos)
        work = tick.index(call, mark)
        pos = work
    # After DONE only the T8142 tick-deadline bookkeeping (j813-windows) may follow.
    done = tick.rindex("hv_fatal_stage(HV_FATAL_STAGE_DONE);")
    assert tick[done:].replace("hv_fatal_stage(HV_FATAL_STAGE_DONE);", "").split() in (
        [], "if (chip_id == T8142) hv_tick_deadline_complete(state, hv_host_counter());".split())
    stage = code(function_body(source("src/hv/windows/hv_fatal.c"), "void hv_fatal_stage(u64 stage)"))
    assert stage.index('sysop("dsb sy");') < stage.index("c->stage = stage;")
    assert "if (chip_id != T8152)\n        return;" in stage
    conv = code(function_body(source("src/hv/windows/hv.c"), "void hv_fb_convert_slice(u32 lines)"))
    assert conv.index("hv_fatal_fb_begin(start, end);") < conv.index("for (u32 y = start;") < \
        conv.index("hv_fatal_fb_end();")
    fb_end = code(function_body(source("src/hv/windows/hv_fatal.c"), "void hv_fatal_fb_end(void)"))
    assert fb_end.index('sysop("dsb sy");') < fb_end.index("hv_fatal_seq_end(")
    # The idle path reaches the converter with no fence of its own: _begin fences first.
    fb_begin = code(function_body(source("src/hv/windows/hv_fatal.c"),
                                  "void hv_fatal_fb_begin(u64 start, u64 end)"))
    assert fb_begin.index('sysop("dsb sy");') < fb_begin.index('sysop("isb");') < \
        fb_begin.index("hv_fatal_seq_begin(")


def test_t5_ptw_is_set_and_verified():
    t8152 = code(source("src/hv/windows/hv_t8152.c"))
    flt = function_body(t8152, "u64 hv_t8152_hcr_filter(u64 hcr)")
    assert "return (hcr | HCR_TWI | HCR_TWE | HCR_PTW) & ~HCR_FWB;" in flt
    ver = function_body(t8152, "int hv_t8152_verify(")
    assert "if (!(hcr & HCR_PTW) || !(secondary_hcr & HCR_PTW)) {" in ver


def test_t6_report_runs_before_every_el2_reboot_with_par_from_entry():
    exc = source("src/exception.c")
    serr = function_body(exc, "void exc_serr(u64 *regs)")
    body = code(serr)
    first = re.sub(r"\n\s*\n", "\n", body[body.index("{") + 1:]).lstrip()
    assert first.startswith("#ifdef AURA_WINDOWS\n    u64 par_at_entry = mrs(PAR_EL1);")
    assert body.index('hv_windows_fatal_report(regs, "SError at EL2", par_at_entry, '
                      '"at entry");') < \
        body.index('printf("Unhandled exception, rebooting...\\n");')
    sync = code(function_body(exc, "void exc_sync(u64 *regs)"))
    assert sync.index("u64 par_at_entry = mrs(PAR_EL1);") < sync.index("u32 insn;")
    assert sync.index("hv_windows_fatal_report(regs, \"EL2 synchronous exception\", "
                      "par_at_entry, \"at entry\");") < \
        sync.index('printf("Unhandled exception, rebooting')
    # The guest path: PAR is read before the proxy's own translations overwrite it, and the
    # report says so instead of "at entry".
    proxy = code(function_body(source("src/hv/windows/hv_exc.c"),
                               "static void _hv_exc_proxy(struct exc_info *ctx, "))
    par = proxy.index("u64 par_before = mrs(PAR_EL1);")
    assert par < proxy.index("ctx->elr_phys = hv_translate(") < proxy.index("uartproxy_run(")
    guest = proxy.index('hv_windows_fatal_report(NULL, "unhandled guest exception", par_before,\n'
                        '                                    "before the proxy\'s translations");')
    assert guest < proxy.index('hv_safe_reset("unhandled guest exception");', guest)
    assert proxy.count("mrs(PAR_EL1)") == 1
    report = code(function_body(source("src/hv/windows/hv_fatal.c"),
                                "void hv_windows_fatal_report(u64 *regs, const char *what, "
                                "u64 par, const char *par_when)"))
    assert report.index("if (chip_id != T8152 || !hv_fatal_armed)") < \
        report.index("__atomic_exchange_n(&hv_fatal_reporting, 1")
    start = function_body(source("src/hv/windows/hv.c"),
                          "static int hv_start_session(void *entry, u64 regs[4])\n{")
    assert start.index("hv_fatal_armed = chip_id == T8152;") < \
        start.index('printf("HV: Entering guest at %p')
    assert start.index("hv_fatal_armed = false;") < start.index("hv_ahci_guest_exited(status);")


def pp_value(expr, defined):
    """Evaluate a #if expression made of defined(X), !, && and || (all hv_exc.c uses)."""
    expr = re.sub(r"defined\s*\(\s*(\w+)\s*\)", lambda m: str(m.group(1) in defined), expr)
    expr = expr.replace("&&", " and ").replace("||", " or ")
    expr = re.sub(r"!(?!=)", " not ", expr)
    assert re.fullmatch(r"[\sA-Za-z()]*", expr) and set(re.findall(r"\w+", expr)) <= \
        {"True", "False", "and", "or", "not"}, expr
    return eval(expr)  # noqa: S307 - only True/False/and/or/not, checked above


def active_branches(text, defined):
    """The lines a preprocessor with exactly `defined` set keeps (#ifdef/#ifndef/#if/#elif/
    #else/#endif; every other name is undefined, as in the build)."""
    out, stack = [], []  # stack entries: [this branch taken, some branch already taken]
    for line in text.splitlines(keepends=True):
        t = line.strip()
        word = t.split()[0] if t.startswith("#") else ""
        if word in ("#ifdef", "#ifndef", "#if"):
            arg = t.split(None, 1)[1]
            val = (arg.split()[0] in defined) if word == "#ifdef" else \
                (arg.split()[0] not in defined) if word == "#ifndef" else pp_value(arg, defined)
            stack.append([val, val])
        elif word == "#elif":
            val = not stack[-1][1] and pp_value(t.split(None, 1)[1], defined)
            stack[-1] = [val, stack[-1][1] or val]
        elif word == "#else":
            stack[-1] = [not stack[-1][1], True]
        elif word == "#endif":
            stack.pop()
        elif all(e[0] for e in stack):
            out.append(line)
    assert not stack
    return "".join(out)


def test_every_guest_exit_is_recorded():
    # The Windows resident's config: config.h defines both under AURA_WINDOWS.
    config = source("config.h")
    win = config[config.index("#ifdef AURA_WINDOWS"):]
    win = win[:win.index("#endif")]
    defined = {"AURA_WINDOWS"} | set(re.findall(r"#define (ENABLE_\w+)", win))
    assert {"ENABLE_VGIC_MODULE", "ENABLE_NATIVE_AIC_PASSTHROUGH"} <= defined
    exc = code(active_branches(source("src/hv/windows/hv_exc.c"), defined))
    # Each exit vector of the built carrier records its exit.
    for vector, kinds in (("void hv_exc_sync(struct exc_info *ctx)", ["SYNC"]),
                          ("void hv_exc_irq(struct exc_info *ctx)", ["IRQ"]),
                          ("void hv_exc_fiq(struct exc_info *ctx)", ["FIQ_FAST", "FIQ"]),
                          ("void hv_exc_serr(struct exc_info *ctx)", ["SERROR"])):
        body = function_body(exc, vector)
        for kind in kinds:
            assert f"hv_fatal_note_trap(HV_FATAL_TRAP_{kind}, " in body, (vector, kind)
    # Where the context was read, the note is the very next statement and uses it.
    contexts = [m.start() for m in re.finditer(r"hv_get_context\(ctx\);", exc)]
    notes = [m.start() for m in re.finditer(r"hv_fatal_note_trap\(HV_FATAL_TRAP_\w+, ctx->esr, "
                                            r"ctx->elr, ctx->spsr, ctx->far\);", exc)]
    assert len(contexts) == len(notes) == 4
    for c, n in zip(contexts, notes):
        assert c < n and exc[c:n].count("\n") == 1
    # The IRQ vector never reads the context in this build: it records ELR/SPSR directly,
    # before any branch.
    irq = function_body(exc, "void hv_exc_irq(struct exc_info *ctx)")
    assert "hv_get_context(ctx);" not in irq
    note = irq.index("hv_fatal_note_trap(HV_FATAL_TRAP_IRQ, 0, mrs(ELR_EL2), mrs(SPSR_EL2), 0);")
    assert note < irq.index("if (hv_rendezvous_requested())")
    # And the non-VGIC branch does not record it a second time.
    raw = function_body(code(source("src/hv/windows/hv_exc.c")),
                        "void hv_exc_irq(struct exc_info *ctx)")
    assert raw.count("hv_fatal_note_trap(") == 1


def test_makefile_links_the_report():
    make = source("Makefile")
    assert "ahci_model.o hv_ahci.o hv_vdev_spi.o hv_fatal.o" in make
    assert "mtp_handoff.o hv_serror.o" in make


def test_r5_an_icc_exit_is_named_as_the_window_not_the_source(replay):
    """G6 r5 (run 20261120-053521): verdict 2 on a trapped ICC_IAR1_EL1 read.  The software GIC
    model touches no device, so the report must not leave "raised by the EL2 work of that exit"
    standing as the cause; a WFI exit (its idle path polls devices) gets no such line."""
    text = report(replay, "report_icc")
    assert "cpu 6 SError vs previous exit: verdict 2: taken at that exit's return point; not " \
           "pending when the guest exited, pending when EL2 returned" in text
    assert "HV: fatal: cpu 6 that exit (ICC_IAR1_EL1 read) runs the software GIC model only " \
           "(past its one-time log line on each CPU): carrier memory and system registers, no " \
           "device access; the verdict names the window, not the source" in text
    assert text.index("verdict 2") < text.index("ICC_IAR1_EL1 read") < \
        text.index("carrier context follows")
    wfi = report(replay, "report_icc_wfi")
    assert "verdict 2" in wfi
    assert "software GIC model" not in wfi


def test_r5_apple_error_registers_come_last_read_only(replay):
    """The latched-error registers are read after everything else, each guarded; a trapping
    one is reported as UNDEF and the report goes on to its last line."""
    text = report(replay, "report_icc")
    assert "HV: fatal: Apple error registers (read only, guarded): L2C_ERR_STS 0x82 (access " \
           "fault, recursive) L2C_ERR_ADR 0x10012345678 L2C_ERR_INF 0x42 LSU_ERR_STS UNDEF " \
           "FED_ERR_STS 0x0 MMU_ERR_STS 0x0\n" in text
    assert text.index("guest EL1 ELR_EL12") < text.index("Apple error registers") < \
        text.index("the host decides what follows")
    # G6 r6: a nonzero L2C_ERR_ADR that read is handed to the carve-out decoder once, after the
    # register line; a report whose ADR read zero never calls it.
    assert "MMU_ERR_STS 0x0\nHV: fatal: L2C_ERR_ADR decoder stub (0x10012345678)\n" in text
    assert text.count("L2C_ERR_ADR decoder stub") == 1
    assert "L2C_ERR_ADR decoder stub" not in report(replay, "report_d3")
    d3 = report(replay, "report_d3")
    assert d3.index("Apple error registers") < d3.index("not contained")
    assert d3.rstrip().endswith("not contained: the carrier does not resume after this "
                                "(SError at EL2)")


def test_r5_the_apple_error_reads_never_write_and_are_guarded():
    src = code(source("src/hv/windows/hv_fatal_apl.c"))
    assert "msr(" not in src and "_msr(" not in src and "reg_set" not in src \
        and "reg_clr" not in src
    on = function_body(src, "static void guard_on(void)")
    assert "exc_guard = GUARD_MARK | GUARD_SILENT;" in on
    assert "exc_guard = GUARD_OFF;" in function_body(src, "static void guard_off(void)")
    body = function_body(src, "bool hv_fatal_apl_err_read(")
    # Only the MRS of the chosen register runs inside the guard; the trap count decides.
    assert body.index("int before = exc_count;") < body.index("guard_on();") \
        < body.index("switch (which)") < body.index("guard_off();") \
        < body.index("if (exc_count != before)")
    inside = body[body.index("guard_on();"):body.index("guard_off();")]
    assert inside.count("= mrs(SYS_IMP_APL_") == 9
    assert "printf" not in inside
    assert "hv_fatal_apl.o" in source("Makefile")
