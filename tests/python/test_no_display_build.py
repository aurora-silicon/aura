# SPDX-License-Identifier: GPL-2.0-only
"""The proxy-only Stage 1 can select Aura's existing headless display path."""

from pathlib import Path
import subprocess


ROOT = Path(__file__).resolve().parents[2]


def build_cfg(no_display: str) -> str:
    subprocess.run(
        ["gmake", "-s", "build-cfg", f"NO_DISPLAY={no_display}"],
        cwd=ROOT,
        check=True,
    )
    return (ROOT / "build" / "build_cfg.h").read_text()


def test_no_display_is_explicit_and_defaults_off():
    assert "#define NO_DISPLAY" not in build_cfg("0")
    assert "#define NO_DISPLAY" in build_cfg("1")
