# SPDX-License-Identifier: MIT
"""cL4 region derivation for an ExclaveCore bundle (proxyclient/aura/exclave_bundle.py)."""

import importlib.util
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("exclave_bundle",
                                              REPO / "proxyclient/aura/exclave_bundle.py")
eb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(eb)

PAGE = eb.PAGE


def comp(text, data, linkedit, pdata=(0, 0)):
    return eb.Component(name="c", text=text, data=data, linkedit=linkedit, pdata=pdata)


def test_regions_tile_the_payload():
    comps = [comp((0, 0x8000), (0x10000, 0x4000), (0x18000, 0x4000)),
             comp((0x8000, 0x8000), (0x14000, 0x4000), (0x1C000, 0x2000), (0x1E000, 0x2000))]
    regions = eb.derive_regions(comps, 0x22000)
    assert regions == {
        "CL4-rx": (0, 0x10000),
        "CL4-rw": (0x10000, 0x8000),
        "CL4-le": (0x18000, 0x8000),
        "CL4-ro": (0x20000, 0x4000),
    }


def test_misordered_bundle_is_refused():
    with pytest.raises(eb.BundleError):
        eb.derive_regions([comp((0, 0x8000), (0x10000, 0x4000), (0x8000, 0x4000))], 0x20000)


def test_unaligned_group_is_refused():
    with pytest.raises(eb.BundleError):
        eb.derive_regions([comp((0, 0x8000), (0x9000, 0x4000), (0x10000, 0x4000))], 0x18000)


def test_truncated_im4p_is_refused():
    with pytest.raises(eb.BundleError):
        eb.im4p_payload(bytes([0x30, 0x84, 0x00]))
