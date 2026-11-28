# SPDX-License-Identifier: MIT
"""sptm_patch.plan on synthetic SPTM images: the CTRR/CTXR/AMCC family is patched only in an SPTM
that carries its own region checks (ctXr_check_all_regions, M4 and later). One that locks CTRR
itself (M2's) has none of those windows and gets the APSTS patch alone, with no CTRR stash."""
import struct
import sys

import pytest

from test_smp_timeouts import REPO

sys.path.insert(0, str(REPO / "proxyclient"))
from aura import sptm_patch  # noqa: E402

FILL = 0xD503201F  # NOP


def image(*windows, marker=b""):
    words = []
    for w in windows:
        words += [FILL] * 16 + [x if isinstance(x, int) else x[0] for x in w]
    words += [FILL] * 16
    return struct.pack(f"<{len(words)}I", *words) + marker


def apsts_poll(reg_a, reg_b):
    """Architectural 5-instruction APSTS readiness poll for register pair (xA, xB)."""
    return (
        0xD53EFC80 | reg_a,
        0x92400000 | (reg_a << 5) | reg_b,
        0xB4FFFFC0 | reg_b,
        0xD53CF080 | reg_a,
        0xB2400000 | (reg_a << 5) | reg_a,
    )


def test_no_ctxr_checks_patches_apsts_only():
    p = sptm_patch.plan(image(sptm_patch.APSTS), 0)
    assert [x[0] for x in p["patches"]] == ["APSTS poll"]
    assert p["ctrr_stash_off"] == 0


def test_two_apsts_polls_with_distinct_registers_both_patched():
    p = sptm_patch.plan(image(apsts_poll(0, 1), apsts_poll(10, 8)), 0)
    assert [x[0] for x in p["patches"]] == ["APSTS poll", "APSTS poll"]
    assert [x[3] for x in p["patches"]] == [0xB4FFFFC1, 0xB4FFFFC8]
    assert [x[4] for x in p["patches"]] == [sptm_patch.NOP, sptm_patch.NOP]
    assert p["ctrr_stash_off"] == 0


def test_zero_or_too_many_apsts_polls_refused():
    with pytest.raises(sptm_patch.SptmPatchError):
        sptm_patch.plan(image((FILL, FILL)), 0)
    with pytest.raises(sptm_patch.SptmPatchError):
        sptm_patch.plan(image(apsts_poll(0, 1), apsts_poll(10, 8), apsts_poll(2, 3)), 0)


def test_mismatched_apsts_registers_ignored():
    bad = list(apsts_poll(10, 8))
    bad[4] = 0xB2400000 | (9 << 5) | 9  # wrong register in ORR
    with pytest.raises(sptm_patch.SptmPatchError):
        sptm_patch.plan(image(tuple(bad)), 0)


def test_ctxr_marker_without_the_windows_is_refused():
    with pytest.raises(sptm_patch.SptmPatchError):
        sptm_patch.plan(image(sptm_patch.APSTS, marker=sptm_patch.CTXR_MARKER), 0)
