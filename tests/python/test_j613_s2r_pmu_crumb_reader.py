"""The retained J613 sleep record is decoded without losing raw bytes."""
import importlib.util
from pathlib import Path

import pytest


PATH = Path(__file__).resolve().parents[2] / "proxyclient/tools/read_j613_s2r_pmu_crumbs.py"
SPEC = importlib.util.spec_from_file_location("read_j613_s2r_pmu_crumbs", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_decode_record():
    assert MODULE.decode([0x30, 0x81, 0x02, 0x48, 3]) == {
        "step": 0x30,
        "wfi_mask": 0x281,
        "wake_step": 0x48,
        "commit_count": 3,
        "raw": [0x30, 0x81, 0x02, 0x48, 3],
    }


@pytest.mark.parametrize("values", ([0] * 4, [0] * 6, [0, 0, 0, 0, 256]))
def test_decode_record_rejects_non_bytes(values):
    with pytest.raises(ValueError, match="five bytes"):
        MODULE.decode(values)
