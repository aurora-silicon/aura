# SPDX-License-Identifier: MIT
"""Experimental T8122 profile. No J700 patch fallback; no Apple image reads on import."""
import hashlib
import json
import struct
from pathlib import Path
PROFILE = json.loads((Path(__file__).parent / 'profiles/j613-25G83.json').read_text())
PATCH_EXPORT = PROFILE['patch_export']

def capability(mmfr2, gxf):
    return ((mmfr2 >> 24) & 15) >= 2 and gxf

def validate_resources(get_reg):
    for r in PROFILE['resources']:
        if 'index' in r and tuple(get_reg(r['path'], r['index'])) != (int(r['base'], 0), int(r['size'], 0)):
            raise ValueError('ADT drift: ' + str(r))

def inspect_patch_export(image, entry_off):
    """Validate and return the clean-room facts that are complete without admitting entry."""
    if image is None:
        return {'reviewed': False, 'unresolved': unresolved_groups()}
    if hashlib.sha256(image).hexdigest() != PATCH_EXPORT['image_sha256']:
        raise ValueError('J613 SPTM image identity does not match the 25G83 export')
    expected_entry = int(PATCH_EXPORT['entry_file_offset'], 0)
    if entry_off != expected_entry:
        raise ValueError(f'J613 SPTM entry offset {entry_off:#x}, expected {expected_entry:#x}')
    patches = []
    for site in PATCH_EXPORT['groups']['APSTS poll']:
        pattern = bytes.fromhex(site['pattern'])
        hits = [i for i in range(len(image) - len(pattern) + 1)
                if image[i:i + len(pattern)] == pattern]
        start = int(site['pattern_offset'], 0)
        if hits != [start]:
            raise ValueError(f"J613 {site['purpose']}: pattern hits {hits}, expected [{start}]")
        patch_off = int(site['patch_offset'], 0)
        expected = int(site['expected'], 0)
        if struct.unpack_from('<I', image, patch_off)[0] != expected:
            raise ValueError(f"J613 {site['purpose']}: patch word drift")
        off = int(site['entry_offset'], 0)
        if patch_off - entry_off != off:
            raise ValueError(f"J613 {site['purpose']}: entry-relative offset drift")
        patches.append(('APSTS poll', off, 0xffffffff, expected,
                        int(site['replacement'], 0)))
    return {'patches': patches, 'ctrr_stash_off': None, 'mte': None,
            'reviewed': False, 'unresolved': unresolved_groups()}

def unresolved_groups():
    return [name for name, rows in PATCH_EXPORT['groups'].items() if rows is None]

def patch_plan(image, entry_off):
    partial = inspect_patch_export(image, entry_off)
    missing = ', '.join(partial['unresolved'])
    raise ValueError(f'J613 dirty-room patch/GL export incomplete ({missing}); guest entry disabled')
