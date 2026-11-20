# SPDX-License-Identifier: MIT
"""T8132 Mac mini (J773g) display handoff on macOS 27.2 (26B123): external processors only.

Failure model, recorded before src/kboot_display_handoff.c learnt the J773g:
- On 27.2, macOS drives the J773g's HDMI port from dcpext0. iBoot leaves dcp, dcpext0 and
  dcpext1 running the same DCP image, B3BA603C-3CA7-3E21-B393-44EB20BD2AEF. The dcpext nubs have
  four segments (__TEXT, __DATA, __OS_LOG, heap), and __TEXT is one buffer shared by all three
  processors. /arm-io/dispextN clock-ids index /arm-io clock-frequencies from 256, like disp0's
  (dispext0: 369 = 533333328 Hz, 432 = 0 Hz).
- Like the J575c, the board is headless: the internal DCP is never handed over. Its node, DARTs
  and mailbox stay as the device tree has them, and it gets no marker, memory-region, clock or
  firmware property. No MMIO is written at all: dart-dcp is not locked.
- Each apple,t8132-dcpext node the device tree describes, at /dcp@286e00000 or /dcp@48ae00000,
  is handed over as the J575c's are. Requirements:
  - the node and its DART (iommus <&dart 23>, an apple,t8110-dart) are status "disabled";
  - it has three disabled apple,asc-mem placeholders named text, data and heap;
  - its nub has the pinned UUID, is pre-loaded and running, and has the four-segment layout;
  - data and heap lie outside the kernel's RAM;
  - its __TEXT is the first processor's.
  Then:
  - data and heap are filled and enabled with their DART alias;
  - text points at the shared dcp-text region, which carries one iommu-addresses tuple per
    processor;
  - apple,firmware-version and -compat <27 2 0> and apple,firmware-uuid are published;
  - the node and its DART stay disabled for the kernel's display gate.
- Unlike the J575c's, a J773g dcpext gets its own pipe's firmware clocks:
  - fixed-clock providers, clock-names firmware-0..N and apple,clock-ids = the /arm-io/dispextN
    clock-ids, where T6041's get empty apple,clock-ids. The firmware asks for them (D408), and
    macOS answers with these rates.
  - A /clock-dcp-firmware-<id> node already in the device tree (the 26.6.2 tree has static
    ones) is reused when it is a fixed-clock of the same rate; with another rate the processor
    is refused.
  - A single static `clocks` on the node is replaced. Existing clock-names or apple,clock-ids
    refuse it.
- Any failed check refuses that processor only: its placeholders stay disabled and it gets no
  firmware, UUID or clock property, and no clock node. The other processor is unaffected.
- Once a processor is published, the display subsystem (disp0 alias) and its DART are enabled,
  with iBoot's boot framebuffer as the display's "framebuffer" memory-region. If the device tree
  already enables them (the 26.6.2 J773g tree does), that step is refused and logged, and the
  display node is left as it is; the published processors stay published. With no processor
  published, the display is not touched.
- __OS_LOG of dcp, dcpext0 and dcpext1 is reserved, as on every supported boot.
- Nothing at all (no MMIO, the device tree byte for byte unchanged) unless:
  - the chip is T8132 and the root is apple,j773g (a J713 gets nothing from this path);
  - the OS firmware is 27.2 (26.6.2 and 27.0 get nothing);
  - the internal nub runs the pinned UUID (26B131's 27.2.1 image gets nothing);
  - /arm-io/disp0 has valid clock-ids;
  - the device tree has the dcp and disp0 aliases on apple,t8132-dcp and
    apple,t8132-display-subsystem nodes.
- A second call changes nothing. A device tree with no room refuses transactionally: no partial
  clock nodes, properties or regions.
- T6041 (J616c/J614c/J575c) behaviour is unchanged (test_t6041_*_display_handoff.py).
"""

import re

import pytest
from test_t6041_display_handoff import ROOT, build_host, run

HOST = "t8132_j773g_27_display_host.c"
SCENARIOS = re.findall(r'^    \{"([a-z0-9-]+)", ', (ROOT / "tests" / HOST).read_text(), re.M)


PUBLISHED = "dcpext0 firmware memory published"
BOTH = "dcpext1 firmware memory published"
DISPLAY = "display subsystem and its DART enabled for the external processors"
NOTHING = "nothing done"
# The log line of the check each scenario is meant to reach, so that none passes for another reason
REASONS = {
    "publish": [PUBLISHED, BOTH, DISPLAY],
    "dcpext0-only": [PUBLISHED, DISPLAY],
    "repeat": ["3 __OS_LOG reservation(s) already present", "dcpext0 already has firmware clocks"],
    "static-clock-nodes": [PUBLISHED, BOTH, DISPLAY],
    "display-preenabled": [PUBLISHED, BOTH, 'display subsystem is not status "disabled"'],
    "ext-uuid-mismatch": [PUBLISHED, "dcpext1: ADT image uuid is not B3BA603C-"],
    "ext-not-running": [BOTH, "dcpext0: iBoot did not leave its four-segment firmware loaded"],
    "ext-enabled": [BOTH, 'dcpext0 is not status "disabled"'],
    "ext-dart-enabled": [BOTH, 'dcpext0 DART is not status "disabled"'],
    "ext-wrong-compat": [BOTH, "dcpext0 is not apple,t8132-dcpext"],
    "ext-no-placeholders": [BOTH, "dcpext0: no text, data and heap memory-region placeholders"],
    "ext-text-not-shared": [PUBLISHED, "dcpext1: __TEXT 0x10284000000 is not the shared one"],
    "ext-data-in-ram": [BOTH, "dcpext0: segment 1 0x10010000000+0x8000 at 0x10002490000 is kernel RAM"],
    "ext-clock-missing": [BOTH, "27.2 /arm-io/dispext0 clock metadata is missing or invalid"],
    "ext-clock-zero": [BOTH, "27.2 /arm-io/dispext0 primary clock frequency is zero"],
    "ext-clock-names": [BOTH, "dcpext0 already has firmware clocks"],
    "ext-clock-conflict": [BOTH, "/clock-dcp-firmware-369 exists with another rate"],
    "ext-all-refused": ["dcpext0: iBoot did not leave", "dcpext1: iBoot did not leave"],
    "wrong-chip": ["not a J773g (chip 0x6041): " + NOTHING],
    "j713": ["not a J773g (chip 0x8132): " + NOTHING],
    "os-26-6-2": ["no qualified OS firmware for apple,t8132-dcp: " + NOTHING],
    "os-27-0": ["no qualified OS firmware for apple,t8132-dcp: " + NOTHING],
    "wrong-uuid": ["27.2 DCP image mismatch: " + NOTHING],
    "disp0-clocks-missing": ["27.2 /arm-io/disp0 clock metadata is missing or invalid"],
    "no-display-dt": ["the device tree does not describe the display block"],
    "dt-full": ["could not add the __OS_LOG reservations", "dcpext0 firmware memory refused"],
}


def test_every_scenario_has_its_reason():
    assert sorted(REASONS) == sorted(SCENARIOS)


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("t8132-27"), host_source=HOST)


def test_scenarios_cover_the_failure_model():
    for required in ("publish", "repeat", "display-preenabled", "static-clock-nodes",
                     "ext-uuid-mismatch", "ext-text-not-shared", "ext-clock-missing",
                     "ext-clock-conflict", "ext-all-refused", "j713", "os-26-6-2",
                     "wrong-uuid", "dt-full"):
        assert required in SCENARIOS


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_t8132_j773g_27_2_display_handoff(host, scenario):
    result = run(host, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
    for reason in REASONS[scenario]:
        assert reason in result.stdout, (reason, result.stdout)
