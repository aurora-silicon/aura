# SPDX-License-Identifier: MIT
"""Clean-room SPTM exports are chosen by image identity, not by SoC, board or OS build.
No Apple image fixtures or target connection are used.
"""
import copy
import hashlib
import json
import os
import struct
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from aura import sptm_export  # noqa: E402
REAL_IMAGE = os.environ.get("SPTM_EXPORT_IMAGE")


def synthetic(entry_off=0x100):
    """A made-up image and export with one APSTS site: no Apple bytes."""
    image = bytearray(0x400)
    pattern = bytes(range(0x20, 0x40))
    image[0x200:0x220] = pattern
    struct.pack_into("<I", image, 0x210, 0xb4ffffc1)
    pattern = bytes(image[0x200:0x220])
    image = bytes(image)
    export = {
        "_name": "synthetic", "label": "synthetic",
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "entry_file_offset": hex(entry_off),
        "groups": {"APSTS poll": [{
            "purpose": "test site", "pattern_offset": "0x200", "pattern": pattern.hex(),
            "patch_offset": "0x210", "entry_offset": hex(0x210 - entry_off),
            "expected": "0xb4ffffc1", "replacement": "0xd503201f"}],
            "CTRR check": []},
        "reviewed": True,
    }
    return image, export


def test_shipped_exports_load_and_are_admissible():
    assert sptm_export.EXPORTS
    for export in sptm_export.EXPORTS:
        sptm_export.admit(export)
        assert not {"board", "soc", "build", "resources", "windows"} & export.keys()


def test_selection_is_by_image():
    image, export = synthetic()
    assert sptm_export.find(image, [export]) is export
    assert sptm_export.find(image + b"\0", [export]) is None
    plan = sptm_export.patch_plan(image, 0x100, export)
    assert plan["patches"] == [("APSTS poll", 0x110, 0xffffffff, 0xb4ffffc1, 0xd503201f)]


def test_refusals():
    image, export = synthetic()
    with pytest.raises(ValueError, match="entry offset"):
        sptm_export.patch_plan(image, 0x104, export)
    with pytest.raises(ValueError, match="identity"):
        sptm_export.patch_plan(image[:-1] + b"\1", 0x100, export)
    bad = copy.deepcopy(export)
    bad["groups"]["CTRR check"] = None
    with pytest.raises(ValueError, match="incomplete"):
        sptm_export.admit(bad)
    bad = copy.deepcopy(export)
    bad["groups"]["CTRR check"] = [{"purpose": "unapplied"}]
    with pytest.raises(ValueError, match="does not apply"):
        sptm_export.admit(bad)
    bad = copy.deepcopy(export)
    bad["reviewed"] = False
    with pytest.raises(ValueError, match="not reviewed"):
        sptm_export.admit(bad)
    drift = bytearray(image)
    struct.pack_into("<I", drift, 0x210, 0)
    drift = bytes(drift)
    bad = copy.deepcopy(export)
    bad["image_sha256"] = hashlib.sha256(drift).hexdigest()
    with pytest.raises(ValueError, match="pattern hits|drift"):
        sptm_export.patch_plan(drift, 0x100, bad)


def test_duplicate_or_incomplete_export_files(tmp_path):
    _, export = synthetic()
    del export["_name"]
    (tmp_path / "a.json").write_text(json.dumps(export))
    assert len(sptm_export.load_exports(tmp_path)) == 1
    (tmp_path / "b.json").write_text(json.dumps(export))
    with pytest.raises(ValueError, match="same image"):
        sptm_export.load_exports(tmp_path)
    (tmp_path / "b.json").write_text(json.dumps({"groups": {}}))
    with pytest.raises(ValueError, match="missing"):
        sptm_export.load_exports(tmp_path)


@pytest.mark.skipif(not REAL_IMAGE, reason="a private SPTM image with an export was not given")
def test_real_image_export_is_bound_and_admitted():
    image = Path(REAL_IMAGE).read_bytes()
    export = sptm_export.find(image)
    assert export
    plan = sptm_export.patch_plan(image, int(export["entry_file_offset"], 0), export)
    assert plan["patches"]


def test_runner_selects_by_image_not_chip():
    source = (REPO / "proxyclient/tools/run_guest_sptm.py").read_text()
    assert "export = sptm_export.find(args.sptm.read_bytes())" in source
    assert "sptm_export.patch_plan(images[\"sptm\"], entry_off, export) if export" in source
    main = source[source.index("def main():"):]
    assert "0x8122" not in main and "J613" not in main
