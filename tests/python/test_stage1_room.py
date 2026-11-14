# SPDX-License-Identifier: MIT
"""tools/stage1_room.py: the room a Stage 1 link has left, with and without a 64 KiB step
between .init and .text."""

import importlib.util
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("stage1_room", ROOT / "tools/stage1_room.py")
stage1_room = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage1_room)

STEP = 0x10000


def syms(**kw):
    base = {
        "_init_content_end": 0x1DC68,
        "_text_content_end": 0xAD178,
        "_rodata_content_end": 0xE9000,
        "_data_start": 0x100000,
        "_file_vital_end": 0x11B538,
    }
    base.update(kw)
    return base


def test_text_follows_init():
    # .text right after .init: one slot, so .init's distance to its step does not count.
    r, headroom = stage1_room.room(syms(_text_content_start=0x1E000))
    assert "init" not in r
    assert r["text"] == 0xB0000 - 0xAD178
    assert headroom == min(r["text"], r["rodata"], r["data"]) == 0x2E88


def test_text_on_its_own_step():
    # An older link: .text starts on the step after .init, and .init's room counts.
    r, headroom = stage1_room.room(syms(_init_content_end=0x1FFA0, _text_content_end=0xAF000))
    assert r["init"] == 0x60
    assert headroom == 0x60


def test_init_ending_on_a_step_is_not_full():
    # With no step between them, .init ending exactly on 64 KiB is not zero room.
    r, headroom = stage1_room.room(syms(_init_content_end=0x20000, _text_content_start=0x20000))
    assert "init" not in r and headroom > 0


def test_data_room_of_a_step_or_more_wins():
    r, headroom = stage1_room.room(syms(_text_content_start=0x1E000, _file_vital_end=0x100000))
    assert headroom == r["data"] == 0x20000


def test_built_stage1():
    elf = ROOT / "build/aura-raw.elf"
    nm = next((n for n in ("llvm-nm", "aarch64-linux-gnu-nm", "nm") if shutil.which(n)), None)
    if not elf.exists() or nm is None:
        pytest.skip("no Stage 1 build")
    out = subprocess.run([nm, str(elf)], capture_output=True, text=True, check=True).stdout
    s = {f[-1]: int(f[0], 16) for f in (l.split() for l in out.splitlines()) if len(f) == 3}
    if "_text_content_start" not in s:
        pytest.skip("not an aura-raw.ld link")
    # .text starts within one page of the end of .init, not on the next 64 KiB step.
    assert s["_text_content_start"] - s["_init_content_end"] < 0x4000
