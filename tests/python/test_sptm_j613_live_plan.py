# SPDX-License-Identifier: MIT
"""Offline replay of rubra's retained live ADT and iBoot-copy geometry.

This is placement evidence only.  The TrustCache bytes are deliberately absent;
the fixture supplies a same-size structural placeholder and must never be used
as a boot payload.
"""

import hashlib
import json
import os
import struct
from pathlib import Path

import pytest

from aura.sptm_virt_plan import plan_boot


INPUTS = {
    "sptm": os.environ.get("J613_SPTM_IMAGE"),
    "txm": os.environ.get("J613_TXM_IMAGE"),
    "xnu": os.environ.get("J613_XNU_IMAGE"),
    "adt": os.environ.get("J613_LIVE_ADT"),
    "report": os.environ.get("J613_BOOT_COPY_REPORT"),
}

pytestmark = pytest.mark.skipif(not all(INPUTS.values()), reason="retained private J613 inputs not given")


def test_retained_live_plan_geometry_only():
    blobs = {name: Path(path).read_bytes() for name, path in INPUTS.items() if name != "report"}
    report = json.loads(Path(INPUTS["report"]).read_text())
    copies = [(row["name"], row["address"], row["size"])
              for row in report["boot_copies"]["entries"]]
    assert hashlib.sha256(blobs["adt"]).hexdigest() == \
        "0237d97a05589cbee9a0b167d38165e513f8ca95755220c52d7cec555d59f6e4"
    assert copies == [
        ("RTBuddySeg", 0x1000385C000, 0x60000),
        ("SEPFW", 0x100038BC000, 0x5AC000),
        ("preoslog", 0x10003E68000, 0x40000),
    ]

    # Same 0x60000 extent and container header as the live entry, zero payload.
    # This proves placement only, never authentication or bootability.
    trustcache_placeholder = struct.pack("<II", 1, 8) + bytes(0x60000 - 8)
    plan = plan_boot(
        blobs["sptm"], blobs["txm"], blobs["xnu"], blobs["adt"],
        dram_base=0x10000000000,
        phys_base=0x1001E000000,
        mem_size=0x103DE7A4000 - 0x1001E000000,
        mem_size_actual=0x400000000,
        blob_base=0x1001E000000,
        trustcache=trustcache_placeholder,
        iboot_copies=copies,
    )
    assert len(plan.loads) == 20
    assert len(plan.adt) == 0x70000
    assert not plan.adt_grew
    assert (plan.blob_start, plan.blob_end) == (0x10020000000, 0x10027960000)
    assert plan.sptm_entry_pa == 0x100251A4388
    assert plan.boot_args_pa == 0x10027310000
    assert plan.virt_base == 0xFFFFFE001E000000
    assert [(load.what, load.fileoff, load.size) for load in plan.loads if load.image == "copy"] == [
        ("RTBuddySeg", 0x1000385C000, 0x60000),
        ("SEPFW", 0x100038BC000, 0x5AC000),
        ("preoslog", 0x10003E68000, 0x40000),
    ]
    assert any("/arm-io/dcp/iop-dcp-nub __OS_LOG" in note for note in plan.notes)
    assert any("/arm-io/dcpext/iop-dcpext-nub __OS_LOG" in note for note in plan.notes)
