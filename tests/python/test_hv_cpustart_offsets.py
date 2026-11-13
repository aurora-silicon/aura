# SPDX-License-Identifier: MIT
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proxyclient"))

from aura.hv import CPU_START_OFFSETS

SMP_C = Path(__file__).resolve().parents[2] / "src" / "smp.c"


def smp_c_offsets():
    """Chip ID -> CPU-start offset, as smp_init() in smp.c chooses it, for the Mac SoCs (T6xxx, T8103 and up)."""
    src = SMP_C.read_text()
    values = {m[1]: int(m[2], 16) for m in re.finditer(r"#define (CPU_START_OFF_\w+)\s+(0x[0-9a-f]+)", src)}
    body = src[src.index("switch (chip_id) {", src.index("int smp_init(void)")):]
    out = {}
    for m in re.finditer(r"((?:\s*case \w+:\n)+)\s*cpu_start_off = (CPU_START_OFF_\w+);", body):
        for chip in re.findall(r"case T([0-9A-F]{4}):", m[1]):
            chip_id = int(chip, 16)
            if 0x6000 <= chip_id < 0x7000 or chip_id >= 0x8100:
                out[chip_id] = values[m[2]]
    return out


def test_hv_table_matches_firmware():
    fw = smp_c_offsets()
    assert 0x6050 in fw and 0x8103 in fw, "parser lost the T6050 or T8103 case"
    assert CPU_START_OFFSETS == fw


def test_t6050_uses_the_t6031_block():
    # m1n1 starts all 17 secondaries of a T6050 through PMGR0 + 0x88000.
    assert CPU_START_OFFSETS[0x6050] == 0x88000
