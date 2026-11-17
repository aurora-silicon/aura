"""Retained J613 entry record parsing is strict and value-preserving."""
import importlib.util
from pathlib import Path

import pytest


PATH = Path(__file__).resolve().parents[2] / "proxyclient/tools/read_j613_entry_crumbs.py"
SPEC = importlib.util.spec_from_file_location("read_j613_entry_crumbs", PATH)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def test_decode_record():
    words = [MODULE.MAGIC, 9, 0x69, 123456, 4,
             MODULE.MAGIC ^ 9 ^ 0x69, 0, 0]
    record = MODULE.decode_record(words)
    assert record == dict(magic="J613ENTR", sequence=9, stage=0x69,
                          ticks=123456, cpu=4, reserved=[0, 0])


@pytest.mark.parametrize("words", (
    [0] * 8,
    [MODULE.MAGIC, 1, 2, 3, 4, 0, 0, 0],
    [MODULE.MAGIC] * 7,
))
def test_decode_record_rejects_torn_or_wrong_data(words):
    with pytest.raises(ValueError):
        MODULE.decode_record(words)
