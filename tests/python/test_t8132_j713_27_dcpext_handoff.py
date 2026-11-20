# SPDX-License-Identifier: MIT
"""T8132 MacBook Air (J713) external display processors on macOS 27.2 (26B123).

Failure model, recorded with the J713's 27.2 ADT structure (no Apple data in this file):
- iBoot leaves dcp, dcpext0 and dcpext1 running the image the J773g runs on 27.2
  (B3BA603C-3CA7-3E21-B393-44EB20BD2AEF): the same four-segment dcpext layout, one __TEXT shared
  by all three processors, dispextN clock-ids, SIDs 23 (firmware DART) and 0/16 (display DART).
- kboot.c's M4 handoff hands the internal panel's DCP over first: it publishes the dcp node's
  apple,firmware-uuid and reserves every internal segment as /reserved-memory/asc-firmware@<phys>,
  __TEXT included, with an iommu-addresses tuple for the dcp.
- Then dt_set_j713_dcpext_handoff() hands each apple,t8132-dcpext over beside it, through the
  J773g's external-processor path (dcp6_ext_fill()), with these differences:
  - the node and its DART may be enabled (the J713 tree enables them for the 26.6.2 path) or
    disabled; their status is left as it is: the kernel's display gate decides;
  - its text entry points at the internal handoff's asc-firmware@<__TEXT> region, which gets a
    second tuple for this processor; a missing region refuses the processor;
  - no apple,t8132-dcpext-firmware-ready marker (that selects the kernel's 26.6.2 path);
  - the display subsystem is not touched (the internal handoff owns it), and no MMIO is done.
  As on the J773g: data and heap filled and enabled, firmware 27.2.0 + pinned UUID, the pipe's
  firmware clocks, __OS_LOG of both dcpexts reserved, one transaction per processor.
- Nothing at all unless the root is apple,j713, the OS firmware is 27.2 and the internal DCP was
  handed over with the pinned image. A second call changes nothing.
"""

import re

import pytest
from test_t6041_display_handoff import ROOT, build_host, run

HOST = "t8132_j713_27_dcpext_host.c"
SCENARIOS = re.findall(r'^    \{"([a-z0-9-]+)", ', (ROOT / "tests" / HOST).read_text(), re.M)

P0 = "dcpext0 firmware memory and clocks published for 27.2"
P1 = "dcpext1 firmware memory and clocks published for 27.2"
NOT_HANDED = "external processors not handed over"
REASONS = {
    "publish": [P0, P1],
    "dcpext0-only": [P0],
    "repeat": ["2 __OS_LOG reservation(s) already present", "dcpext0 already has firmware clocks"],
    "ext-disabled": [P0, P1],
    "ext-uuid-mismatch": [P0, "dcpext1: ADT image uuid is not B3BA603C-"],
    "ext-not-running": [P1, "dcpext0: iBoot did not leave its four-segment firmware loaded"],
    "ext-text-not-shared": [P0, "dcpext1: __TEXT 0x10284000000 is not the shared one"],
    "no-internal-text": ["the internal handoff's /reserved-memory/asc-firmware@10280000000 is missing"],
    "no-internal-handoff": ["internal DCP not handed over with image B3BA603C-"],
    "j773g": [],
    "os-26-6-2": ["not macOS 27.2: " + NOT_HANDED],
    "os-27-0": ["not macOS 27.2: " + NOT_HANDED],
    "dt-full": ["dcpext0 firmware memory refused", "dcpext1 firmware memory refused"],
}


def test_every_scenario_has_its_reason():
    assert sorted(REASONS) == sorted(SCENARIOS)


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("t8132-j713-27"), host_source=HOST,
                      includes=("t8132_j773g_27_display_host.c",))


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_t8132_j713_27_2_dcpext_handoff(host, scenario):
    result = run(host, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
    for reason in REASONS[scenario]:
        assert reason in result.stdout, (reason, result.stdout)
