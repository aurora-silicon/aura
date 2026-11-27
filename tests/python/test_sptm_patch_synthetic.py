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


def test_no_ctxr_checks_patches_apsts_only():
    p = sptm_patch.plan(image(sptm_patch.APSTS), 0)
    assert [x[0] for x in p["patches"]] == ["APSTS poll"]
    assert p["ctrr_stash_off"] == 0


def test_ctxr_marker_without_the_windows_is_refused():
    with pytest.raises(sptm_patch.SptmPatchError):
        sptm_patch.plan(image(sptm_patch.APSTS, marker=sptm_patch.CTXR_MARKER), 0)
