# SPDX-License-Identifier: MIT
"""Exercise payload_profile_rank_fw() across firmware-specific and fallback DTs."""

from pathlib import Path
import subprocess


def test_payload_profile_host(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    libfdt = repo / "src/libfdt"
    binary = tmp_path / "payload_profile_host"
    sources = [
        repo / "tests/payload_profile_host.c",
        libfdt / "fdt.c",
        libfdt / "fdt_ro.c",
        libfdt / "fdt_rw.c",
        libfdt / "fdt_wip.c",
        libfdt / "fdt_sw.c",
        libfdt / "fdt_empty_tree.c",
        libfdt / "fdt_addresses.c",
    ]
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-I", str(repo / "src"),
         *[str(s) for s in sources], "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
