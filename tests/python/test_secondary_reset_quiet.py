# SPDX-License-Identifier: GPL-2.0-only
"""Secondaries never print from the MMU-off reset path; the boot CPU reports them."""

from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def test_reset_path_quiet_on_every_chip():
    source = (ROOT / "src/startup.c").read_text()
    body = source[source.index("void _cpu_reset_c("):]
    quiet = re.search(r"bool quiet = ([^;]*);", body).group(1)
    assert "chip_id" not in quiet
    assert "!is_boot_cpu()" in quiet
    assert "RVBAR entry on secondary CPU" not in source


def test_secondary_entry_does_not_print_by_chip():
    source = (ROOT / "src/smp.c").read_text()
    entry = source[source.index("void smp_secondary_entry(void)"):]
    entry = entry[:entry.index("\n}\n")]
    assert "printf(" not in entry
    start = source[source.index("Starting CPU %d"):]
    assert "Started, MPIDR 0x%lx" in start[:start.index("return i >= 100")]
