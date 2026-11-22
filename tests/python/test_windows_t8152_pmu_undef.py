# SPDX-License-Identifier: MIT
"""Host source rules for the T8152 (J804) absent-PMUv3 recovery and the stray-CNTV fix.

M3 run 1 (2026-11-16): the Windows boot manager executed "mrs x9, PMCCNTR_EL0" (0xd53b9d09)
while Mu still owned VBAR_EL1.  It UNDEFed at EL1 (ESR 0x02000000) with MDCR_EL2 0x60 set,
exactly as on T8142.  Mu forwarded it over "hvc #0" with x0 = "NTSRG", but the resident took
that call only on T8142, answered it as a PSCI call, and Mu asserted.  These rules pin:

1. One predicate, hv_absent_pmuv3() (T8142 or T8152), opens the NTSRG service and every
   chip gate of the recovery (vector redirect, trampoline, wedge recovery, re-verify, module
   walk); hv_windows_configure() accepts the guest-compatibility bit on exactly those chips.
2. The kernel-build-specific diagnostics and the Mu SP_EL0 repair stay T8142-only.
3. On T8152 the recovery reads and writes guest memory only inside the guest RAM window.
4. Every PMU instruction encoding the boot manager uses (encodings from the Arm ARM) decodes,
   the way hv_t8142_sysreg_assist_impl() decodes it, to a register the software PMU answers.
5. T8152 hands the guest its virtual timer disabled on every CPU (the ~126k/s reflection
   loop at the Mu shell came from the carrier's own CNTV_CTL_EL02 = 7).
Nothing here touches hardware.
"""

import os
from pathlib import Path
import re
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def code(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def test_one_predicate_names_the_absent_pmuv3_chips():
    hv = source("hv.c")
    body = function_body(hv, "bool hv_absent_pmuv3(void)")
    assert "return chip_id == T8142 || chip_id == T8152;" in body
    conf = function_body(hv, "int hv_windows_configure(u64 flags)")
    assert "if ((flags & HV_WINDOWS_GUEST_COMPAT) && !hv_absent_pmuv3())" in conf
    # MTP stays a J813 feature.
    assert ("if ((flags & (HV_WINDOWS_MTP | HV_WINDOWS_CPUFREQ)) && (chip_id != T8142 || "
            "board_id != 0x24))") in conf


@pytest.mark.parametrize("signature", [
    "static bool hv_handle_t8142_undef_trampoline(struct exc_info *ctx)",
    "bool hv_recover_t8142_sysreg_undef(struct exc_info *ctx)",
    "void hv_verify_t8142_undef_vectors(void)",
    "void hv_track_t8142_undef_vector(struct exc_info *ctx)",
    "static void hv_scan_guest_modules(void)",
])
def test_recovery_gates_take_t8152(signature):
    body = function_body(source("hv_exc.c"), signature)
    assert "chip_id != T8142" not in body and "chip_id == T8142" not in body
    assert "!hv_absent_pmuv3()" in body
    assert "if (!hv_windows_guest_compat_enabled())" in body  # still an explicit opt-in


def test_ntsrg_is_served_on_both_chips_without_the_opt_in():
    body = function_body(source("hv_exc.c"), "static bool hv_handle_smc(struct exc_info *ctx)")
    first = body[:body.index("#ifdef ENABLE_NATIVE_AIC_PASSTHROUGH")]
    assert "ctx->regs[0] == HV_SYSREG_ASSIST_CALL_MAGIC && hv_absent_pmuv3()" in first
    assert "hv_windows_guest_compat_enabled" not in first
    assert "ctx->regs[0] = handled;" in first


def test_build_specific_diagnostics_and_sp_el0_repair_stay_t8142():
    exc = source("hv_exc.c")
    assert "if (chip_id != T8142 || !t8142_kernel_base)" in function_body(
        exc, "static void hv_report_t8142_current_process(struct exc_info *ctx)")
    assert "if (chip_id != T8142 || reported || !t8142_kernel_base)" in function_body(
        exc, "void hv_check_t8142_bugcheck(void)")
    # The stuck-PC reporter in the wedge recovery reads the same build-specific RVA and
    # EPROCESS offsets; the recovery itself runs on T8152, so the reporter needs its own gate.
    assert "if (chip_id != T8142 || reported || !t8142_kernel_base)" in function_body(
        exc, "static void hv_report_t8142_bugcheck(struct exc_info *ctx)")
    assert "hv_windows_guest_compat_enabled() && chip_id == T8142 && " \
           "hv_native_aic_mu_timer_active()" in exc


def test_t8152_recovery_stays_inside_the_guest_ram_window():
    exc = source("hv_exc.c")
    helper = function_body(exc, "static u64 hv_absent_pmu_translate(u64 va, bool w, u64 len)")
    assert "chip_id == T8152" in helper and "hv_t8152_guest_ram(&base, &size)" in helper
    assert "pa - base > size - len" in helper and "len > size" in helper
    for signature in [
        "static bool hv_emulate_t8142_pending_undef(",
        "static bool hv_patch_t8142_undef_vector(u64 slot)",
        "void hv_verify_t8142_undef_vectors(void)",
        "static bool hv_unpatch_t8142_undef_vector(u64 va)",
        "static bool hv_read_guest32(u64 va, u32 *out)",
        "static void hv_report_t8142_dead_process(u64 code, u64 param1)",
        "static void hv_report_t8142_bugcheck(struct exc_info *ctx)",
        "bool hv_recover_t8142_sysreg_undef(struct exc_info *ctx)",
    ]:
        body = code(function_body(exc, signature))
        assert "hv_absent_pmu_translate(" in body, signature
        assert "hv_translate(" not in body, signature
    # The image scanner: both passes translate page by page through the guard.
    scan = exc[exc.index("static bool hv_patch_t8142_pmu_site("):
               exc.index("static void hv_scan_guest_image_absent_regs(u64 va_in_image)")]
    scan += function_body(exc, "static void hv_scan_guest_image_absent_regs(u64 va_in_image)")
    assert "hv_translate(" not in code(scan)


def test_recovery_console_lines_name_the_chip():
    exc = source("hv_exc.c")
    assert 'return chip_id == T8152 ? "T8152" : "T8142";' in function_body(
        exc, "static const char *hv_absent_pmu_chip(void)")
    region = exc[exc.index("static u64 hv_absent_pmu_translate("):
                 exc.index("void hv_report_t8142_gic_activity(void)")]
    assert 'printf("HV: T8142: ' not in region
    smc = function_body(exc, "static bool hv_handle_smc(struct exc_info *ctx)")
    assert 'printf("HV: %s: handled forwarded EL1 sysreg instruction 0x%08x at EL2\\n",' in smc


def sysreg_defs():
    defs = {}
    for name in ("windows_regs.h",):
        for m in re.finditer(r"#define (SYS_PM\w+)\s+sys_reg\((\d+), (\d+), (\d+), (\d+), (\d+)\)",
                             source(name)):
            defs[m.group(1)] = tuple(int(v) for v in m.groups()[1:])
    return defs


def decode(insn):
    """A64 MRS/MSR (register) fields, as hv_t8142_sysreg_assist_impl() extracts them."""
    assert insn & 0xffd00000 == 0xd5100000, hex(insn)
    return ((insn >> 19) & 3, (insn >> 16) & 7, (insn >> 12) & 0xf, (insn >> 8) & 0xf,
            (insn >> 5) & 7), bool(insn & (1 << 21))


# Encodings only (Arm ARM): the PMU accesses the M3 run-1 boot manager executes, from its
# PMU init block plus the faulting cycle-counter read.
BOOTMGR_PMU = {
    0xd53b9d09: ("SYS_PMCCNTR_EL0", True),    # mrs x9, PMCCNTR_EL0 (the run-1 fault)
    0xd53b9d08: ("SYS_PMCCNTR_EL0", True),    # mrs x8, PMCCNTR_EL0
    0xd53b9c08: ("SYS_PMCR_EL0", True),
    0xd51b9c08: ("SYS_PMCR_EL0", False),
    0xd51b9c48: ("SYS_PMCNTENCLR_EL0", False),
    0xd51b9c68: ("SYS_PMOVSCLR_EL0", False),
    0xd5189e48: ("SYS_PMINTENCLR_EL1", False),
    0xd51befe8: ("SYS_PMCCFILTR_EL0", False),
    0xd51b9c28: ("SYS_PMCNTENSET_EL0", False),
    0xd51b9e08: ("SYS_PMUSERENR_EL0", False),
}


@pytest.mark.parametrize("insn", sorted(BOOTMGR_PMU))
def test_bootmgr_pmu_encodings_reach_the_software_pmu(insn):
    name, is_read = BOOTMGR_PMU[insn]
    fields, read = decode(insn)
    assert read == is_read
    assert sysreg_defs()[name] == fields
    pmu = function_body(source("hv_exc.c"),
                        "static bool hv_handle_software_pmu(u64 reg, bool is_read, u64 rt, "
                        "u64 regs[32])")
    assert f"case SYSREG_ISS({name}):" in pmu
    # The software PMU is live only where the Apple bank is locked; the M6 profile is.
    assert "if (cpu_features->apple_sysregs_unlocked)\n        return false;" in pmu


def test_m6_profile_keeps_the_software_pmu():
    chickens = (REPO / "src/chickens.c").read_text()
    m6 = chickens[chickens.index("features_m6"):]
    m6 = re.sub(r"/\*.*?\*/", "", m6[:m6.index("};")], flags=re.S)
    assert "apple_sysregs_unlocked" not in m6


def test_t8152_guest_vtimer_starts_disabled_on_every_cpu():
    hv = source("hv.c")
    body = function_body(hv, "static void hv_t8152_quiesce_guest_vtimer(bool primary)")
    assert "if (chip_id != T8152)\n        return;" in body
    assert "msr(SYS_CNTV_CTL_EL02, 0);" in body
    start = function_body(hv, "static int hv_start_session(void *entry, u64 regs[4])\n{")
    entry = start.index('printf("HV: Entering guest at %p')
    assert start.rindex("hv_t8152_quiesce_guest_vtimer(true);", 0, entry) > \
        start.index("hv_t8152_verify(")
    secondary = function_body(hv, "static void hv_init_secondary(struct hv_secondary_info_t *info)")
    assert "hv_t8152_quiesce_guest_vtimer(false);" in secondary
    # The carrier's own startup state is what armed the loop; it must stay ENABLE|IMASK there
    # (aura's own FIQ sources) and be cleared only at guest entry.
    assert "msr(CNTV_CTL_EL02, 7L);" in (REPO / "src/exception.c").read_text()


def test_mu_forwards_with_the_same_magic():
    mu = os.environ.get("AURA_MU_TREE")
    if not mu:
        pytest.skip("AURA_MU_TREE not set")
    path = "Silicon/Apple/AppleSiliconPkg/Library/AuraArmExceptionLib/AuraExceptionForward.c"
    out = subprocess.run(["git", "-C", mu, "show", f"j804-windows:{path}"],
                         capture_output=True, text=True, check=True).stdout
    m = re.search(r"#define AURA_SYSREG_REQUEST\s+(0x[0-9a-fA-F]+)ULL", out)
    assert m and int(m.group(1), 16) == 0x4e54535247
    assert re.search(r"#define HV_SYSREG_ASSIST_CALL_MAGIC\s+0x4e54535247ULL",
                     source("hv_exc.c"))
