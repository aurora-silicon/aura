# SPDX-License-Identifier: GPL-2.0-only
"""Run the production Rust ADT accessors against a well-formed host tree."""

from pathlib import Path
import subprocess

import pytest


# The reg bounds check behaved differently with and without overflow checks,
# so run the harness both ways (aura itself is built with optimisation).
@pytest.mark.parametrize("opt", [[], ["-O"]], ids=["debug", "release"])
def test_adt_semantics(tmp_path, opt):
    repo = Path(__file__).resolve().parents[2]
    binary = tmp_path / "adt-semantics"
    subprocess.run(
        ["rustc", "--edition=2021", *opt, str(repo / "tests/rust/adt_semantics.rs"),
         "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
