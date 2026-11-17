"""J613 entry must carry an independent reset and RAM evidence past console loss."""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_j613_entry_arm_precedes_terminal_proxy_request():
    runner = (ROOT / "proxyclient/tools/run_guest_sptm.py").read_text()
    arm = runner.index('sym["sptm_virt_j613_entry_recovery_arm"]')
    enter = runner.index('p.request(p.P_CALL, sym["sptm_virt_enter"], no_reply=True)')
    assert arm < enter
    assert "J613 retained entry crumbs" in runner[arm:enter]


def test_j613_entry_crumbs_are_cache_cleaned_at_risky_boundaries():
    source = (ROOT / "src/sptm_virt.c").read_text()
    marker = source[source.index("void sptm_virt_j613_entry_mark("):]
    marker = marker[:marker.index("\n}") + 2]
    assert "dc_cvac_range" in marker
    assert 'sysop("dsb sy")' in marker

    enter = source[source.index("void sptm_virt_enter("):]
    enter = enter[:enter.index("\n}") + 2]
    for stage in ("0x20", "0x21", "0x22", "0x30", "0x40", "0x50"):
        assert f"sptm_virt_j613_entry_mark({stage})" in enter

    xnu = source[source.index("void sptm_virt_cpu_enters_xnu("):]
    xnu = xnu[:xnu.index("\n}") + 2]
    assert "j613_entry_recovery_armed" in xnu
    assert "wdt_disable()" in xnu

    nv2 = (ROOT / "src/nv2.c").read_text()
    entry = nv2[nv2.index("void nv2_enter_os("):]
    entry = entry[:entry.index("\n}") + 2]
    for stage in ("0x60", "0x61", "0x62", "0x63", "0x68", "0x69"):
        assert f"sptm_virt_j613_entry_mark({stage})" in entry


def test_dispatch_marks_ram_before_console_output():
    source = (ROOT / "src/uartproxy.c").read_text()
    block = source[source.index("if (guest_entry)"):]
    block = block[:block.index("#endif")]
    assert block.index("sptm_virt_j613_entry_mark(0x10)") < block.index("printf(")
