# SPDX-License-Identifier: MIT
"""Host tests of the T8152 (J804) software GICv3 CPU interface (blocker B1).

1. The model, src/hv/windows/hv_gic_cpuif.c, is compiled unchanged with the host clang
   (address and undefined-behaviour sanitizers) against a stand-in types.h and replayed by
   tests/e2e/windows_gic_cpuif_host.c: reset values, PMR and running-priority masking,
   acknowledge / priority drop / deactivation (EOImode 0 and 1), nested preemption, the
   binary point and CBPR, Group 0 and special INTIDs, SGI1R target decoding and concurrent
   pending updates.  Expected values follow Arm IHI 0069.
2. Source rules for the carrier glue: T8152 always uses the software interface, refuses the
   hardware-interface configuration, programs the measured trap-all ICH_HCR_EL2 value with
   HCR_EL2.IMO/FMO, delivers by HCR_EL2.VI without list registers, keeps the Mu phase as
   measured, and answers the whole TID3 space locally.
Nothing here touches hardware.
"""

import json
from pathlib import Path
import re
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"

TYPES_H = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t s64;
"""

CASES = ["reset_values", "group1_disabled", "pmr_mask", "ack_eoi", "active_and_pending",
         "preemption_nesting", "binary_point", "eoimode1", "special_and_group0",
         "ties_and_range", "power_on", "sgi1r_targets", "concurrent_pending", "windows_like",
         "timer_gate", "timer_level_windows"]


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


@pytest.fixture(scope="module")
def replay(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    out = tmp_path_factory.mktemp("gic_cpuif")
    (out / "types.h").write_text(TYPES_H)
    shutil.copy2(WIN / "hv_gic_cpuif.h", out / "hv_gic_cpuif.h")
    shutil.copy2(WIN / "hv_gic_cpuif.c", out / "hv_gic_cpuif.c")
    binary = out / "gic-cpuif-host"
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(out),
                    str(out / "hv_gic_cpuif.c"), str(REPO / "tests/e2e/windows_gic_cpuif_host.c"),
                    "-o", str(binary)], check=True, capture_output=True, text=True, timeout=120)
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=60)
    return result


def test_model_replay_passes(replay):
    assert replay.returncode == 0, replay.stdout + replay.stderr
    lines = [json.loads(line) for line in replay.stdout.splitlines()]
    assert [line["case"] for line in lines] == CASES
    assert all(line["result"] == "passed" for line in lines)


def test_model_is_pure():
    text = source("hv_gic_cpuif.c") + source("hv_gic_cpuif.h")
    # No system-register access, no MMIO, no printing, no aura state: only logic.
    for forbidden in ("mrs(", "msr(", "read32", "write32", "printf", "smp_id", "chip_id", "aic_"):
        assert forbidden not in text, forbidden
    includes = re.findall(r'#include "([^"]+)"', text)
    assert set(includes) == {"types.h", "hv_gic_cpuif.h"}


def test_t8152_always_uses_the_software_interface():
    hv_c = source("hv.c")
    body = function_body(hv_c, "bool hv_soft_gic_cpuif(void)")
    assert "return chip_id == T8142 || chip_id == T8152;" in body
    configure = function_body(hv_c, "int hv_windows_configure(u64 flags)")
    assert "chip_id == T8152 && !(flags & HV_WINDOWS_T8152_SOFT_GIC)" in configure
    # The T8142 model never runs on T8152, and T8152's handler runs before every other one.
    exc = source("hv_exc.c")
    t8142 = function_body(exc, "static bool hv_handle_t8142_gic_cpuif(")
    assert "chip_id == T8152" in t8142.split("\n")[3]
    unlocked = function_body(exc, "static bool hv_handle_msr_unlocked(")
    assert unlocked.index("hv_handle_t8152_gic_cpuif(") < unlocked.index(
        "hv_handle_t8142_gic_cpuif(")
    assert unlocked.index("hv_handle_t8152_gic_cpuif(") < unlocked.index(
        "hv_handle_native_aic_carrier_cpuif(")


def test_t8152_trap_all_configuration_matches_measurement_f():
    exc = source("hv_exc.c")
    # En | TC | TALL0 | TALL1 (IHI 0069 ICH_HCR_EL2 bits 0, 10, 11, 12), VMCR VENG1.
    assert re.search(r"#define HV_T8152_ICH_HCR_TRAPALL 0x1c01\b", exc)
    assert (1 << 0) | (1 << 10) | (1 << 11) | (1 << 12) == 0x1c01
    assert re.search(r"#define HV_T8152_ICH_VMCR_VENG1\s+BIT\(1\)", exc)
    route = function_body(exc, "static void hv_t8152_gic_route_cpu(void)")
    assert "msr(ICH_HCR_EL2, HV_T8152_ICH_HCR_TRAPALL)" in route
    assert "HCR_IMO | HCR_FMO" in route
    # Delivery is HCR_EL2.VI from the model; the T8152 paths never touch a list register.
    t8152 = exc[exc.index("static struct gicv3_cpuif t8152_cpuif"):
                exc.index("static struct hv_gic_cpuif *hv_gic_cpuif_this(void)")]
    for forbidden in ("ICH_LR", "hv_vgic3_inject_irq", "hv_vgic3_write_lr", "ICH_MISR", "ICH_EISR"):
        assert forbidden not in t8152, forbidden
    sync = function_body(exc, "static void hv_t8152_gic_sync_vi(void)")
    assert "gicv3_cpuif_deliverable" in sync and "HCR_VI" in sync
    # The physical IRQ path on T8152 marks the INTID pending before any ICH_MISR/EISR read.
    irq = function_body(exc, "void hv_exc_irq(struct exc_info *ctx)")
    assert irq.index("chip_id == T8152 && hv_t8152_gic_owned()") < irq.index("mrs(ICH_MISR_EL2)")


def test_mu_phase_unchanged_until_the_os_owns_the_gic():
    aic = source("hv_aic.c")
    enter = function_body(aic, "void hv_native_aic_enter_cpu(void)")
    # T8152 takes over only through hv_t8152_gic_enter_cpu, which returns false before the
    # OS owns the GIC; the Mu path below it (ICH_HCR_EL2 0, IMO clear) is untouched.
    assert enter.index("hv_t8152_gic_enter_cpu(startup_carrier)") < enter.index("if (startup_carrier)")
    assert "msr(ICH_HCR_EL2, 0);" in enter
    exc = source("hv_exc.c")
    enter_cpu = function_body(exc, "bool hv_t8152_gic_enter_cpu(bool os_phase)")
    assert "if (!hv_t8152_gic_owned())\n        return false;" in enter_cpu
    # Ownership: Mu's ExitBootServices, or a guest write to the emulated GIC; never a read.
    vgic = source("hv_vgic.c")
    assert vgic.count("if (write)\n        hv_t8152_gic_claim(") == 2
    # The native AIC handoff cannot take an owned T8152 GIC away.
    ready = function_body(aic, "bool hv_native_aic_windows_controller_ready(const char *source)")
    assert ready.index("hv_t8152_gic_owned()") < ready.index("windows_aic_enabled, true")
    # A CPU the guest starts gets a reset CPU interface before its routing is applied.
    hv_c = source("hv.c")
    assert hv_c.index("hv_t8152_gic_cpu_start();") < hv_c.index(
        "hv_native_aic_enter_cpu();", hv_c.index("hv_t8152_gic_cpu_start();"))


def test_tid3_space_answered_on_t8152():
    exc = source("hv_exc.c")
    unlocked = function_body(exc, "static bool hv_handle_msr_unlocked(")
    # GIC = 1 in ID_AA64PFR0_EL1 (bits [27:24]): the system-register interface is emulated.
    assert "regs[rt] = _mrs(sr_tkn(ID_AA64PFR0_EL1)) | (1 << 24);" in unlocked
    tail = unlocked[unlocked.rindex("chip_id == T8152 && is_read"):]
    assert "FIELD_GET(ESR_ISS_MSR_CRm, reg) >= 1 && FIELD_GET(ESR_ISS_MSR_CRm, reg) <= 7" in tail
    assert "regs[rt] = 0;" in tail
    hv_c = source("hv.c")
    assert "HCR_TID3 |" in hv_c


def test_capability_bit_15():
    proxy_c = (REPO / "src/proxy.c").read_text()
    assert "reply->retval = 0x4157494e0001ffdfULL;" in proxy_c
    import sys
    sys.path.insert(0, str(REPO / "proxyclient"))
    from aura import windows
    assert windows.T8152_GIC_TRAPALL == 0x8000 and 0x0001ffdf & windows.T8152_GIC_TRAPALL


def test_model_built_into_the_windows_resident():
    makefile = (REPO / "Makefile").read_text()
    assert re.search(r"hv_t8152\.o hv_gic_cpuif\.o", makefile)


def test_mu_phase_icc_trap_never_claims_the_gic():
    exc = source("hv_exc.c")
    handler = function_body(exc, "static bool hv_handle_t8152_gic_cpuif(")
    # Only the OS-phase hooks claim (ExitBootServices, guest distributor/redistributor write).
    # An SGI-register write that traps through HCR_EL2.FMO in the Mu phase is ignored instead.
    assert "hv_t8152_gic_claim(" not in handler
    unowned = handler.index("if (!hv_t8152_gic_owned()) {")
    model = handler.index("gicv3_cpuif_access(")
    assert unowned < model
    early = handler[unowned:model]
    assert "return true;" in early and "GIC_SPURIOUS_INTID" in early
    assert exc.count("hv_t8152_gic_claim(\"") == 1  # only enter_cpu's ExitBootServices claim


def test_only_icc_encodings_are_taken():
    handler = function_body(source("hv_exc.c"), "static bool hv_handle_t8152_gic_cpuif(")
    # ICC_* live at CRn 12, CRm 8..12 (plus PMR at C4_C6_0); VBAR/RMR/ISR/DISR_EL1 do not.
    assert "key == GICV3_ICC_PMR || (crn == 12 && crm >= 8 && crm <= 12)" in handler
    assert "crn != 12" not in handler


def test_lost_claim_waits_for_ownership():
    exc = source("hv_exc.c")
    wait = function_body(exc, "static void hv_t8152_gic_wait_owned(void)")
    assert "t8152_gic_claimed" in wait and "t8152_gic_owned" in wait and "wfe" not in wait
    claim = function_body(exc, "void hv_t8152_gic_claim(const char *why)")
    lost = claim[claim.index("__atomic_compare_exchange_n"):]
    assert lost.index("hv_t8152_gic_wait_owned();") < lost.index("return;")
    enter_cpu = function_body(exc, "bool hv_t8152_gic_enter_cpu(bool os_phase)")
    assert enter_cpu.index("hv_t8152_gic_wait_owned();") < enter_cpu.index(
        "if (!hv_t8152_gic_owned())")


def test_aarch32_id_block_raz_only_on_an_aarch64_only_core():
    exc = source("hv_exc.c")
    helper = function_body(exc, "static bool hv_t8152_aarch64_only(void)")
    # ID_AA64PFR0_EL1.EL0 [3:0] and .EL1 [7:4] == 0b0001: AArch64 only (Arm ARM).
    assert "(pfr0 & 0xf) == 1 && ((pfr0 >> 4) & 0xf) == 1" in helper
    unlocked = function_body(exc, "static bool hv_handle_msr_unlocked(")
    tail = unlocked[unlocked.rindex("chip_id == T8152 && is_read"):]
    assert tail.index("hv_t8152_aarch64_only()") < tail.index("regs[rt] = 0;")


def test_non_windows_hv_t8152_actlr_el12_write_only_if_different():
    hv_c = (REPO / "src/hv/hv.c").read_text()
    assert "SYS_IMP_APL_ACTLR_EL12, info->actlr_el1" in hv_c  # other chips unchanged
    assert "if (chip_id != T8152 || mrs(SYS_ACTLR_EL12) != info->actlr_el1)" in hv_c
