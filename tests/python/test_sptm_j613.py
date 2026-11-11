# SPDX-License-Identifier: MIT
"""Admission failures: absent capability, unreviewed image, and ADT drift.
No Apple image fixtures or target connection are used.
"""
import pytest
from aura.sptm_j613 import capability, validate_resources, patch_plan, PROFILE

def test_capability_and_incomplete_export():
    assert not capability(1 << 24, True)
    assert not capability(2 << 24, False)
    assert capability(2 << 24, True)
    with pytest.raises(ValueError, match='dirty-room'):
        patch_plan(b'', 0)

def test_adt_validation_before_hooks():
    regs = {(r['path'], r['index']): (int(r['base'], 0), int(r['size'], 0))
            for r in PROFILE['resources'] if 'index' in r}
    validate_resources(lambda p, i: regs[p, i])
    key = next(iter(regs))
    regs[key] = (0, 0)
    with pytest.raises(ValueError, match='ADT drift'):
        validate_resources(lambda p, i: regs[p, i])
