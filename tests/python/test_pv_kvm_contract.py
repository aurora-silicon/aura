from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def test_pv_el1_step_and_run_fail_closed_contract():
    header = (ROOT / "src/pv_kvm.h").read_text()
    host = (ROOT / "src/pv_kvm.c").read_text()
    exception = (ROOT / "src/exception.c").read_text()

    assert "#define AURA_PV_FUNC_EL1_STEP 0xa011" in header
    assert "#define AURA_PV_FEATURE_EL1_STEP    BIT(2)" in header
    assert "#define AURA_PV_STATUS_HOST     (-5)" in header
    assert "AURA_PV_FEATURE_EL1_STEP" in host
    assert "regs[19] != AURA_PV_SENTINEL_X19" in host
    assert "regs[20] != AURA_PV_SENTINEL_X20" in host
    assert "AURA_PV_STATUS_HOST" in host
    assert "case AURA_PV_FUNC_EL1_STEP" in host
    assert exception.count("msr(ELR_EL2, elr);") >= 2
