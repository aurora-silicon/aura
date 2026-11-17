# SPDX-License-Identifier: MIT
"""tools/dump_guest_mem.py and tools/guest_mem.py against the simulated machine in guest_mem_fake.py.

What it guards: a powered-off DART or one whose power is unknown is never touched (on a J815 such a register read
makes the proxy fault in a loop), an exclave stream is never walked, MMIO leaves are listed and not read, a faulting
DRAM page costs only itself, large mappings keep only their head, a powered-off DART is walked from the ring's last
TCR/TTBR writes, and the reader resolves IOVAs through the dumped tables (DART 3/4-level, GPU kernel roots).
"""
import subprocess
import sys
from pathlib import Path


def test_dump_guest_mem_on_a_simulated_machine():
    r = subprocess.run([sys.executable, str(Path(__file__).with_name("guest_mem_fake.py"))],
                       capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
