# SPDX-License-Identifier: MIT
"""Experimental T8122 profile. No J700 patch fallback; no Apple image reads on import."""
import json
from pathlib import Path
PROFILE = json.loads((Path(__file__).parent / 'profiles/j613-25G83.json').read_text())
# Export must bind image SHA256/UUID and provide checked sites and GL constants.
PATCH_EXPORT = None

def capability(mmfr2, gxf):
    return ((mmfr2 >> 24) & 15) >= 2 and gxf

def validate_resources(get_reg):
    for r in PROFILE['resources']:
        if 'index' in r and tuple(get_reg(r['path'], r['index'])) != (int(r['base'], 0), int(r['size'], 0)):
            raise ValueError('ADT drift: ' + str(r))

def patch_plan(image, entry_off):
    raise ValueError('J613 dirty-room patch/GL export absent; guest entry disabled')
