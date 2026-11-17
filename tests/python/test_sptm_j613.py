# SPDX-License-Identifier: MIT
"""Admission failures: absent capability, unreviewed image, and ADT drift.
No Apple image fixtures or target connection are used.
"""
import os
from pathlib import Path

import pytest
from aura.sptm_j613 import (PROFILE, capability, inspect_patch_export, patch_plan,
                            unresolved_groups, validate_resources)

REAL_IMAGE = os.environ.get('J613_SPTM_IMAGE')

def test_capability_and_incomplete_export():
    assert not capability(1 << 24, True)
    assert not capability(2 << 24, False)
    assert capability(2 << 24, True)
    with pytest.raises(ValueError, match='dirty-room'):
        patch_plan(None, None)
    assert unresolved_groups() == ['AMCC CTRR match']
    assert inspect_patch_export(None, None)['unresolved'] == unresolved_groups()

@pytest.mark.skipif(not REAL_IMAGE, reason='exact private J613 SPTM image not given')
def test_exact_image_partial_export_is_bound_but_not_admitted():
    image = Path(REAL_IMAGE).read_bytes()
    partial = inspect_patch_export(image, int(PROFILE['patch_export']['entry_file_offset'], 0))
    assert len(partial['patches']) == 2
    assert {p[1] for p in partial['patches']} == {0x3438, -0xc2c4}
    assert not partial['reviewed']
    with pytest.raises(ValueError, match='AMCC CTRR match'):
        patch_plan(image, int(PROFILE['patch_export']['entry_file_offset'], 0))

def test_adt_validation_before_hooks():
    regs = {(r['path'], r['index']): (int(r['base'], 0), int(r['size'], 0))
            for r in PROFILE['resources'] if 'index' in r}
    validate_resources(lambda p, i: regs[p, i])
    key = next(iter(regs))
    regs[key] = (0, 0)
    with pytest.raises(ValueError, match='ADT drift'):
        validate_resources(lambda p, i: regs[p, i])
