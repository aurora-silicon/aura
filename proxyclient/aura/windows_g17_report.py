# SPDX-License-Identifier: MIT
"""Diagnostic-only G17 description; no hardware access or routing authority.

Fixed profile facts: public Linux e50ba38b24140fc0751b811ea093c8f843bcec76,
arch/arm64/boot/dts/apple/t8142-j813-gpu.dtsi:63-127 and asahi/m5/live_boot.rs.
These are source-derived descriptions, not live register observations.
"""
import struct
import zlib

PROPERTY = "aura,windows-g17-report-v1"
POLICY = "j813-resource-report-v1"
SIZE = 688

MMIO = ((1, 0, 0, 0x580000000, 0x4000000),
        (2, 1, 1, 0x582600000, 0x88000),
        (3, 1, 1, 0x582050000, 8),
        (4, 2, 1, 0x582e00000, 0x88000),
        (5, 2, 1, 0x582850000, 8),
        (6, 0, 1, 0x580e80000, 0x30000))
IRQS = ((1, 1, 1123), (1, 2, 1126), (2, 1, 1127), (2, 2, 1130))


def encode(preservation):
    """Wrap an already admitted preservation record in a zero-authority report."""
    from .windows_g17 import require, SIZE as PRESERVE_SIZE
    require(isinstance(preservation, bytes) and len(preservation) == PRESERVE_SIZE,
            "report requires the complete admitted preservation record")
    record = bytearray(struct.pack("<4sHHIIIIII", b"G17R", 1, 32, SIZE, 0,
                                  PRESERVE_SIZE, len(MMIO), len(IRQS), 0))
    record.extend(preservation)
    for tag, role, parent, base, size in MMIO:
        record.extend(struct.pack("<HHHHQQII", tag, role, parent, 0, base, size, 0, 0))
    for role, event, physical in IRQS:
        record.extend(struct.pack("<HHIIIII", role, event, physical, 0, 0, 1, 0))
    record.extend(struct.pack("<HHHHIIQII", 42, 42, 14, 2, 3, 4, 0x40000, 0, 0))
    record.extend(struct.pack("<IIIIIIII", 0, 6, 0, 0x110, 0x10000, 1, 0, 0))
    require(len(record) == SIZE, "internal report size mismatch")
    struct.pack_into("<I", record, 28, zlib.crc32(record))
    return bytes(record)
