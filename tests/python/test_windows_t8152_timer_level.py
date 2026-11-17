# SPDX-License-Identifier: MIT
"""Host source rules for the T8152 (J804) OS-phase timer PPI, after M3 run 2.

M3 run 2 (2026-11-17): from about 1.5 s after ExitBootServices the boot CPU took ~8.4k
interrupts a second for 3+ minutes with CNTV_CTL = 7 (ENABLE | IMASK | ISTATUS) and
ELR_EL1 frozen.  The guest wrote CNTV_CTL = ENABLE | IMASK before programming CVAL; the
carrier had left the firmware's expired CVAL, and its OS-phase route asked
hv_timer_firing(), which ignores IMASK, so INTID 18 was pended on every exit and re-pended
after every EOI.  These rules pin the fix (the level logic itself is replayed in
tests/e2e/windows_gic_cpuif_host.c, cases timer_gate and timer_level_windows):

1. Guest entry hands CNTV over with CTL 0 and CVAL ~0.
2. On T8152 the OS-phase route samples the timer level through gicv3_timer_gate() and keeps
   the PPI pending only while it is asserted and enabled; T8142 keeps hv_timer_firing().
3. A trapped CNTP access in the OS phase does not run the Mu reflection handshake.
4. The T8152 redistributor enable word is set / clear, priorities are a byte array, and the
   shared paths used by the other chips are unchanged.
5. The 1 Hz report adds a T8152 line per vCPU and a global line, lock-free, from the tick.
Nothing here touches hardware.
"""

from pathlib import Path
import re

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    """The definition (not a forward declaration) that starts with `signature`."""
    start = 0
    while True:
        start = text.index(signature, start)
        head_end = text.index(")", start + len(signature) - 1)
        if not text[head_end + 1:].lstrip(" ").startswith(";"):
            break
        start += len(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def test_entry_hands_over_cntv_disabled_with_unreachable_cval():
    body = function_body(source("hv.c"), "static void hv_t8152_quiesce_guest_vtimer(bool primary)")
    assert "chip_id != T8152" in body
    assert "msr(SYS_CNTV_CTL_EL02, 0);" in body
    assert "msr(SYS_CNTV_CVAL_EL02, ~0UL);" in body


def test_os_phase_route_is_level_sensitive_on_t8152_only():
    exc = source("hv_exc.c")
    body = function_body(exc, "static void hv_update_fiq(struct exc_info *ctx)")
    branch = body[body.index("} else if (hv_gic_cpuif_active()) {"):
                  body.index("} else if (hv_native_aic_mu_timer_active()) {")]
    t8152 = branch[branch.index("if (chip_id == T8152) {"):branch.index("} else {")]
    assert "hv_t8152_gic_timer_update(true);" in t8152
    assert "hv_t8152_gic_timer_update(false);" in t8152
    assert "hv_timer_firing" not in t8152
    other = branch[branch.index("} else {"):]
    # T8142 keeps its measured route.
    assert "hv_timer_firing(mrs(CNTP_CTL_EL02))" in other
    assert "hv_gic_cpuif_timer_pend(false);" in other

    upd = function_body(exc, "static void hv_t8152_gic_timer_update(bool physical)")
    assert "gicv3_timer_gate(ctl, *held)" in upd
    assert "hv_t8152_gic_ppi_enabled(cpu, intid)" in upd
    # Low or disabled: not pending (level semantics); no vm_tmr_fiq_* (they ignore the hold).
    assert upd.rstrip().endswith("gicv3_cpuif_clear_pending(&t8152_cpuif[cpu], intid);")
    assert "vm_tmr_fiq" not in upd and "printf" in upd  # one-time disabled-PPI note only
    assert upd.count("printf") == 1

    gate = function_body(source("hv_gic_cpuif.c"),
                         "struct gicv3_timer_gate gicv3_timer_gate(u64 ctl, bool held)")
    assert "held = held && imask;" in gate
    assert "g.asserted = fired && (!imask || held);" in gate


def test_trapped_cntp_in_os_phase_skips_mu_handshake():
    exc = source("hv_exc.c")
    rearm = function_body(exc, "static void hv_timer_reflect_guest_rearm(bool physical, "
                               "bool control_write, u64 value,")
    hook = rearm.index("if (hv_t8152_gic_timer_guest_write(physical, control_write))")
    assert hook < rearm.index("if (!hv_native_aic_windows_active()) {")
    write = function_body(exc, "static bool hv_t8152_gic_timer_guest_write(bool physical, "
                               "bool control_write)")
    assert "hv_t8152_gic_owned()" in write and "vm_tmr_fiq" not in write
    assert re.search(r"regs\[rt\] = hv_t8152_gic_timer_guest_read\(physical, control, "
                     r"_mrs\(sr_tkn\(to\)\)\);", exc)
    start = function_body(exc, "void hv_t8152_gic_cpu_start(void)")
    assert "timer_held[0] = false" in start and "timer_held[1] = false" in start


def test_redistributor_enable_and_priority_model_is_t8152_gated():
    vgic = source("hv_vgic.c")
    helper = function_body(vgic, "static bool hv_vgic_t8152_redist_sgi_access(")
    assert "if (chip_id != T8152)\n        return false;" in helper
    assert "enabled |= (u32)*val;" in helper and "enabled &= ~(u32)*val;" in helper
    assert "isactiver" not in helper and "icactiver" not in helper
    assert "u32 nbytes = 1u << width;" in helper
    redist = function_body(vgic, "static bool handle_vgic_redist_access(")
    assert redist.index("hv_vgic_t8152_redist_sgi_access(cpu_num, relative_addr, val, write, "
                        "width)") < redist.index("case GIC_REDIST_CTLR:")
    # The shared paths the other chips use are untouched.
    assert "redistributors[cpu_num].sgi_region.gicr_isenabler0 = *val;" in redist
    assert "TODO: should we also write to gicr_isenabler0?" in redist


def test_report_lines_are_t8152_only_lock_free_and_on_the_tick():
    exc = source("hv_exc.c")
    rep = function_body(exc, "void hv_guest_diag_report(void)")
    assert "if (chip_id == T8152)\n            hv_t8152_gdiag_line(cpu, &cur, last, freq_ms);" in rep
    assert "if (chip_id == T8152)\n        hv_t8152_gdiag_global();" in rep
    for name in ("static void hv_t8152_gdiag_line(", "static void hv_t8152_gdiag_global(void)",
                 "static void hv_t8152_top4("):
        body = function_body(exc, name)
        assert "spin_lock" not in body and "iodev_lock" not in body
    snap = function_body(exc, "static void hv_guest_diag_snapshot(struct exc_info *ctx)")
    assert "d->cntv_cval = mrs(CNTV_CVAL_EL02);" in snap
    assert "d->cntvoff = mrs(CNTVOFF_EL2);" in snap
    pmu = function_body(exc, "static bool hv_handle_software_pmu(")
    assert "d->pmc_reads++;" in pmu and "d->pmc_last = regs[rt];" in pmu
    psci = function_body(source("hv_psci.c"), "bool hv_handle_psci_smc(struct exc_info *ctx)")
    assert "__atomic_fetch_add(&hv_psci_calls[slot], 1, __ATOMIC_RELAXED);" in psci
    # The ack/eoi traffic line is once a second on T8152 and says which chip it is.
    act = function_body(exc, "void hv_report_t8142_gic_activity(void)")
    assert "if (chip_id == T8152) {" in act and "t8152_next = now + mrs(CNTFRQ_EL0);" in act
    assert 'chip_id == T8152 ? "T8152" : "T8142"' in act
