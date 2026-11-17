# SPDX-License-Identifier: MIT
"""Host source rules for the T8152 (J804) Mu timer route and the 1 Hz guest report.

First M2 run (2026-11-16): Mu's serial stopped mid line after TimerDxe armed CNTP (10 ms
period; an EL2 FIQ storm is the inferred cause, not observed), with "Mu AIC2 configured; waiting for timer handler" and never "Mu timer handler
ready".  The carrier left EL1 CNTP untrapped on T8152 (CNTHCTL_EL2 0xc00), so TimerDxe's
final ENABLE write never reached hv_timer_reflect_guest_rearm(), and hv_update_fiq() had
no branch for an asserted guest timer before readiness.  These rules pin the fix:

1. T8152 traps EL0/EL1 CNTP (CNTHCTL_EL2.EL1PTEN clear under E2H) and keeps the direct
   counter (EL1PCTEN set).
2. A guest timer that no route owns is masked, never unmasked, in hv_update_fiq().
3. The timer software-IRQ block uses Mu's AIC_TIMER_REFLECT_CPU_SLOTS (24 per bank), not
   MAX_CPUS, and every set/clear is bounded to that block.  With AURA_MU_TREE pointing at a
   Mu checkout, the Mu side is read from its j804-windows branch as well.
4. The guest report is lock-free, printed from the 1 Hz host tick whether or not the BGRA
   shadow exists, and the vUART only records TX-full edges (no printf in the trap path).
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


def test_t8152_traps_el1_cntp_but_not_the_counter():
    hv = source("hv.c")
    branch = hv[hv.index("} else if (chip_id == T8152) {"):]
    branch = branch[:branch.index("} else {")]
    writes = re.findall(r"msr\(CNTHCTL_EL2, ([^;]+)\);", branch)
    assert writes == ["CNTHCTL_EL1PCTEN"]
    assert "trapping EL1 CNTP timer accesses for reflection" in branch
    # The trapped accesses must land in the reflecting handler.
    exc = source("hv_exc.c")
    assert re.search(r"SYSREG_MAP_REFLECT\(SYS_CNTP_CTL_EL0, SYS_CNTP_CTL_EL02, true, true\)", exc)


def test_unrouted_guest_timer_is_masked_never_unmasked():
    exc = source("hv_exc.c")
    body = function_body(exc, "static void hv_update_fiq(struct exc_info *ctx)")
    tail = body[body.index("} else if (hv_native_aic_mu_timer_active()) {"):body.index("#else")]
    fallback = tail[tail.index("    } else {"):]
    assert "vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_P)" in fallback
    assert "vm_tmr_fiq_clr(VM_TMR_FIQ_ENA_ENA_V)" in fallback
    assert "vm_tmr_fiq_set" not in fallback
    assert "aic_set_sw" not in fallback and "hv_timer_swirq_set" not in fallback


def test_reflect_block_matches_mu_abi_and_is_bounded():
    exc = source("hv_exc.c")
    assert re.search(r"#define HV_TIMER_REFLECT_SLOTS\s+24\b", exc)
    base = r"#define HV_TIMER_SWIRQ_BASE\s+\(aic->nr_irq - \(2 \* HV_TIMER_REFLECT_SLOTS\)\)"
    assert re.search(base, exc)
    assert "2 * MAX_CPUS" not in exc
    # Only the bounded helper names a reflection line.
    uses = re.findall(r"aic_set_sw\(([^;]*HV_TIMER_[PV]_SWIRQ[^;]*)\);", exc)
    assert len(uses) == 1
    helper = function_body(exc, "static void hv_timer_swirq_set(bool physical, int cpu, bool on)")
    assert "cpu >= HV_TIMER_REFLECT_SLOTS" in helper


def test_mu_side_uses_the_same_slot_count():
    mu = os.environ.get("AURA_MU_TREE")
    if not mu:
        pytest.skip("AURA_MU_TREE not set")
    header = "Silicon/Apple/AppleSiliconPkg/Include/Library/AppleAicLib.h"
    out = subprocess.run(["git", "-C", mu, "show", f"j804-windows:{header}"],
                         capture_output=True, text=True, check=True).stdout
    m = re.search(r"#define AIC_TIMER_REFLECT_CPU_SLOTS\s+(\d+)", out)
    assert m and int(m.group(1)) == 24


def test_guest_report_is_lock_free_and_on_the_tick():
    hdr = source("guest_diag.h")
    assert "lock" not in re.sub(r"/\*.*?\*/", "", hdr, flags=re.S)
    hv = source("hv.c")
    report = function_body(hv, "void hv_fb_report(void)")
    # Printed every second even without the BGRA shadow, after the shadow block.
    assert report.index("hv_guest_diag_report();") > report.index("if (hv_fb_shadow) {")
    assert "return;" in report and report.count("return;") == 1  # only the 1 Hz gate
    exc = source("hv_exc.c")
    rep = function_body(exc, "void hv_guest_diag_report(void)")
    assert "spin_lock" not in rep and "iodev_lock" not in rep
    # Skips CPUs that took no exit since the last report.
    assert "cur.exits == last->exits" in rep
    ctx = function_body(exc, "static void hv_get_context(struct exc_info *ctx)")
    assert "hv_guest_diag_snapshot(ctx);" in ctx


def test_vuart_records_tx_full_edges_without_printing():
    vu = (REPO / "src/hv/hv_vuart.c").read_text()
    update = function_body(vu, "static void update_irq(void)")
    assert "hv_vuart_diag.full_edges++" in update
    assert "hv_vuart_diag.recover_edges++" in update
    edge = update[update.index("hv_vuart_diag"):update.index("if (tx_ready)\n        utrstat")]
    assert "printf" not in edge
    handler = function_body(vu, "static bool handle_vuart(")
    for counter in ("vuart_rd", "vuart_wr", "vuart_tx", "vuart_txfull"):
        assert f"HV_GUEST_DIAG_INC({counter})" in handler
