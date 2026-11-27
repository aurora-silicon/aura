# SPDX-License-Identifier: MIT
"""Select the host compiler consistently with the CI compiler environment."""

import os
from pathlib import Path
import shlex


def host_cc():
    return shlex.split(os.environ.get("CC") or
                       ("clang" if os.environ.get("USE_CLANG") == "1" else "cc"))


def display_handoff_sources(root=None):
    """Single source list for host tests that link src/kboot_display_handoff.c."""
    base = Path(root) if root is not None else Path(__file__).resolve().parents[2]
    lines = (base / "tests/display-handoff-sources.txt").read_text().splitlines()
    return tuple(line.strip() for line in lines if line.strip() and not line.strip().startswith("#"))
