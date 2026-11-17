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

def test_capability_and_reviewed_export():
    assert not capability(1 << 24, True)
    assert not capability(2 << 24, False)
    assert capability(2 << 24, True)
    assert patch_plan(None, None)['reviewed']
    assert unresolved_groups() == []
    assert inspect_patch_export(None, None)['unresolved'] == unresolved_groups()

@pytest.mark.skipif(not REAL_IMAGE, reason='exact private J613 SPTM image not given')
def test_exact_image_export_is_bound_and_admitted():
    image = Path(REAL_IMAGE).read_bytes()
    partial = inspect_patch_export(image, int(PROFILE['patch_export']['entry_file_offset'], 0))
    assert len(partial['patches']) == 2
    assert {p[1] for p in partial['patches']} == {0x3438, -0xc2c4}
    assert partial['reviewed']
    assert patch_plan(image, int(PROFILE['patch_export']['entry_file_offset'], 0)) == partial

def test_adt_validation_before_hooks():
    regs = {(r['path'], r['index']): (int(r['base'], 0), int(r['size'], 0))
            for r in PROFILE['resources'] if 'index' in r}
    validate_resources(lambda p, i: regs[p, i])
    key = next(iter(regs))
    regs[key] = (0, 0)
    with pytest.raises(ValueError, match='ADT drift'):
        validate_resources(lambda p, i: regs[p, i])

def test_runner_uses_the_exact_j613_plan():
    source = (Path(__file__).resolve().parents[2] /
              'proxyclient/tools/run_guest_sptm.py').read_text()
    assert 'sptm_j613.patch_plan(images["sptm"], entry_off) if chip == 0x8122' in source

def test_j613_entry_has_a_bounded_recovery_watchdog():
    source = (Path(__file__).resolve().parents[2] / 'src/sptm_virt.c').read_text()
    enter = source[source.index('void sptm_virt_enter(void)'):]
    xnu = source[source.index('void sptm_virt_cpu_enters_xnu(int cpu)'):
                 source.index('void sptm_virt_enter(void)')]
    assert 'chip_id == T8122 && wdt_boot_watchdog_armed()' in enter
    assert 'wdt_disable();' in xnu
