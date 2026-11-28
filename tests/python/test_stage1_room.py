# SPDX-License-Identifier: MIT
"""tools/stage1_room.py: the room a Stage 1 link has left, with and without a 64 KiB step
between .init and .text, and the symbols a link must or must not have."""

import importlib.util
import shutil
import subprocess
import sys
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


def test_symbol_problems():
    names = {"_file_vital_end", "dt_set_dcpext_handoff"}
    assert stage1_room.symbol_problems(names) == []
    assert stage1_room.symbol_problems(names, forbid=["dt_set_dcpext_handoff"]) == \
        ["links dt_set_dcpext_handoff, which it must not"]
    assert stage1_room.symbol_problems(names, require=["dt_set_dcpext_handoff"]) == []
    assert stage1_room.symbol_problems(set(), require=["dt_set_dcpext_handoff"]) == \
        ["does not link dt_set_dcpext_handoff"]


def fake_nm(tmp_path, lines, status=0):
    nm = tmp_path / "nm"
    out = tmp_path / "nm.out"
    out.write_text("".join(f"{line}\n" for line in lines))
    nm.write_text(f"#!/bin/sh\ncat '{out}'\nexit {status}\n")
    nm.chmod(0o755)
    return nm


LAYOUT = ["000000000001e000 T _text_content_start", "000000000001dc68 T _init_content_end",
          "00000000000ad178 T _text_content_end", "00000000000e9000 T _rodata_content_end",
          "0000000000100000 T _data_start", "000000000011b538 T _file_vital_end"]


def room_cli(tmp_path, nm, *args):
    return subprocess.run([sys.executable, str(ROOT / "tools/stage1_room.py"), "unused.elf",
                           "--nm", str(nm), *args], capture_output=True, text=True)


def test_cli_symbol_checks(tmp_path):
    nm = fake_nm(tmp_path, LAYOUT + ["0000000000041458 T dt_set_dcpext_handoff"])
    assert room_cli(tmp_path, nm, "--require", "dt_set_dcpext_handoff").returncode == 0
    r = room_cli(tmp_path, nm, "--forbid", "dt_set_dcpext_handoff")
    assert r.returncode == 1 and "links dt_set_dcpext_handoff" in r.stderr
    nm = fake_nm(tmp_path, LAYOUT)
    assert room_cli(tmp_path, nm, "--forbid", "dt_set_dcpext_handoff").returncode == 0
    r = room_cli(tmp_path, nm, "--require", "dt_set_dcpext_handoff")
    assert r.returncode == 1 and "does not link dt_set_dcpext_handoff" in r.stderr


def test_cli_nm_failure_fails(tmp_path):
    # A broken nm must not let a --forbid check pass silently.
    nm = fake_nm(tmp_path, [], status=1)
    r = room_cli(tmp_path, nm, "--forbid", "dt_set_dcpext_handoff")
    assert r.returncode != 0
