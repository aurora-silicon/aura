# SPDX-License-Identifier: MIT
"""Clean-room SPTM patch exports, selected by the SPTM image they were reviewed against.

Each sptm_exports/*.json names one exact SPTM image (image_sha256, entry_file_offset) and its
patch groups. An image with no export goes to the pattern patcher (sptm_patch.plan), which refuses
images it cannot fully plan; an image with an export is patched only from it, and only when the
export is complete and reviewed. No SoC, board or OS build is matched here, and no Apple image is
read on import.
"""
import hashlib
import json
import struct
from pathlib import Path

EXPORT_DIR = Path(__file__).parent / "sptm_exports"
REQUIRED = ("image_sha256", "entry_file_offset", "groups", "reviewed")


def load_exports(directory=EXPORT_DIR):
    exports = []
    for path in sorted(directory.glob("*.json")):
        export = json.loads(path.read_text())
        missing = [k for k in REQUIRED if k not in export]
        if missing:
            raise ValueError(f"{path.name}: missing {', '.join(missing)}")
        export["_name"] = path.stem
        exports.append(export)
    digests = [e["image_sha256"] for e in exports]
    if len(set(digests)) != len(digests):
        raise ValueError("two SPTM exports name the same image")
    return exports


EXPORTS = load_exports()


def find(image, exports=None):
    """The export for this exact SPTM image, or None."""
    digest = hashlib.sha256(image).hexdigest()
    return next((e for e in (EXPORTS if exports is None else exports)
                 if e["image_sha256"] == digest), None)


def unresolved_groups(export):
    return [name for name, rows in export["groups"].items() if rows is None]


def inspect(image, entry_off, export):
    """Validate the export against its image and return the patches it describes."""
    name = export.get("label", export["_name"])
    if hashlib.sha256(image).hexdigest() != export["image_sha256"]:
        raise ValueError(f"SPTM image identity does not match the {name} export")
    expected_entry = int(export["entry_file_offset"], 0)
    if entry_off != expected_entry:
        raise ValueError(f"SPTM entry offset {entry_off:#x}, expected {expected_entry:#x}")
    patches = []
    for site in export["groups"].get("APSTS poll") or ():
        pattern = bytes.fromhex(site["pattern"])
        hits = [i for i in range(len(image) - len(pattern) + 1)
                if image[i:i + len(pattern)] == pattern]
        start = int(site["pattern_offset"], 0)
        if hits != [start]:
            raise ValueError(f"{site['purpose']}: pattern hits {hits}, expected [{start}]")
        patch_off = int(site["patch_offset"], 0)
        expected = int(site["expected"], 0)
        if struct.unpack_from("<I", image, patch_off)[0] != expected:
            raise ValueError(f"{site['purpose']}: patch word drift")
        off = int(site["entry_offset"], 0)
        if patch_off - entry_off != off:
            raise ValueError(f"{site['purpose']}: entry-relative offset drift")
        patches.append(("APSTS poll", off, 0xffffffff, expected, int(site["replacement"], 0)))
    return {"patches": patches, "ctrr_stash_off": None, "mte": None,
            "reviewed": bool(export["reviewed"]), "unresolved": unresolved_groups(export)}


def admit(export):
    """Refuse an incomplete or unreviewed export before anything is placed on the target."""
    name = export.get("label", export["_name"])
    missing = unresolved_groups(export)
    if missing:
        raise ValueError(f"{name}: clean-room export incomplete ({', '.join(missing)}); "
                         "guest entry disabled")
    extra = [g for g, rows in export["groups"].items() if rows and g != "APSTS poll"]
    if extra:
        raise ValueError(f"{name}: export rows this loader does not apply ({', '.join(extra)})")
    if not export["reviewed"]:
        raise ValueError(f"{name}: clean-room export is not reviewed; guest entry disabled")


def patch_plan(image, entry_off, export):
    admit(export)
    return inspect(image, entry_off, export)
