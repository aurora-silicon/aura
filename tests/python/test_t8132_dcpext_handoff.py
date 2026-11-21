# SPDX-License-Identifier: MIT
"""External display processors handed over from the ADT (dt_set_dcpext_handoff()).

Builds tests/t8132_dcpext_host.c (on t6041_display_host.c's fake ADT, PMGR and MMIO) with the
tree's src/kboot_display_handoff.c and runs each scenario. The trees have the T8132 27.2 shape:
- the J713 (MacBook Air): beside a complete internal handoff (kboot.c's M4 handoff published the
  image's uuid on dcp and reserved its __TEXT as asc-firmware@), dcpext0 and dcpext1 are matched
  to their ADT nodes by address; their DART stream (23), DART address and window come from the
  ADT mapper and DART and must match the device tree; their pipe is the ADT ext-display-subsystem
  on their piodma stream (16) and gives their firmware clocks; their image must be the internal
  DCP's and share its __TEXT. Each processor is its own transaction; a refusal is one log line.
  Nothing external is handed over after an incomplete internal handoff (a failed SID0/SID16
  walk), and an image with no known external interface is not this code's at all (0 returned,
  the tree byte for byte unchanged: kboot.c keeps its firmware-ready path for it).
- the J773g (Mac mini, ADT /arm-io/disp0 external): no panel, the same code; the processors must
  be disabled, share the internal nub's __TEXT (reserved once, as dcp-text@), and once one is
  handed over the display subsystem and its DART are enabled on the boot framebuffer.
- a later board and SoC whose processor lists apple,t8132-dcpext as a fallback compatible.
No register is read or written in any scenario.
"""
import re

import pytest
from test_t6041_display_handoff import ROOT, build_host, run

HOST = "t8132_dcpext_host.c"
SCENARIOS = re.findall(r'^    \{"([a-z0-9-]+)", "', (ROOT / "tests" / HOST).read_text(), re.M)

H0 = "dcpext: dcpext0 handed over (image B3BA603C-3CA7-3E21-B393-44EB20BD2AEF, stream 23, 2 pipe clocks)"
H1 = "dcpext: dcpext1 handed over (image B3BA603C-3CA7-3E21-B393-44EB20BD2AEF, stream 23, 2 pipe clocks)"
REASONS = {
    "j713-both": [H0, H1],
    "j713-one": [H0, "dcpext1: iBoot did not leave its four-segment firmware loaded and running"],
    "j713-refused-uuid": [H0, "dcpext1: does not run the internal DCP's image B3BA603C-"],
    "j713-internal-walk-failed": ["internal DCP handoff not complete: external processors not handed over"],
    "j713-unknown-image": ["no external processor interface known for image 81BAF476-"],
    "j713-dcp-uuid-missing": ["dcp was not handed over with image B3BA603C-"],
    "j713-sid-mismatch": [H1, "dcpext0: iommus is not <&dart 22>, the ADT's stream"],
    "j713-dart-reg-mismatch": [H0, "dcpext1: its DART is not the ADT's apple,t8110-dart at 0x48a440000"],
    "j713-dma-range-mismatch": [H1, "dcpext0: DART apple,dma-range is not the ADT window <0x10000000000 0x800000000>"],
    "j713-no-mapper": [H0, "dcpext1: the ADT does not describe its DART stream"],
    "j713-no-pipe": [H0, "dcpext1: 0 ADT display pipes use its piodma stream 16 at 0x48a300000"],
    "j713-two-pipes": [H1, "dcpext0: 2 ADT display pipes use its piodma stream 16 at 0x286300000"],
    "j713-no-adt-node": [H0, "/soc/dcp@48ae00000: 0 ADT processors at 0x48ae00000"],
    "j713-text-not-shared": [H0, "dcpext1: __TEXT 0x10284000000+0x10000 is not the internal DCP's"],
    "j713-bad-layout": [H1, "dcpext0: iBoot did not leave its four-segment firmware loaded and running"],
    "j713-has-clocks": [H1, "dcpext0: already has clocks"],
    "j713-repeat": [H0, H1, "dcpext0: the text placeholder is not an empty, disabled apple,asc-mem"],
    "j713-dt-full": ["dcpext0: no room in the device tree for its handoff",
                     "dcpext1: no room in the device tree for its handoff"],
    "j773g-headless": [H0, "dcpext1: no text, data and heap memory-region placeholders",
                       "no panel: display subsystem and its DART enabled for the external processors"],
    "j773g-both": [H0, H1, "no panel: display subsystem and its DART enabled"],
    "j773g-enabled": ["dcpext0 is not status \"disabled\"", "dcpext1 is not status \"disabled\""],
    "later-soc": [H0, H1],
}
# Scenarios in which exactly one processor is refused: its refusal is one log line.
ONE_REFUSAL = {"j713-one", "j713-refused-uuid", "j713-sid-mismatch", "j713-dart-reg-mismatch",
               "j713-dma-range-mismatch", "j713-no-mapper", "j713-no-pipe", "j713-two-pipes",
               "j713-no-adt-node", "j713-text-not-shared", "j713-bad-layout", "j713-has-clocks"}


def test_every_scenario_has_its_reason():
    assert sorted(REASONS) == sorted(SCENARIOS)
    for required in ("j713-both", "j713-one", "j713-refused-uuid", "j713-internal-walk-failed",
                     "j713-unknown-image", "j773g-headless"):
        assert required in SCENARIOS


def test_no_board_chip_or_release_test_in_the_adt_path():
    code = (ROOT / "src/kboot_display_handoff.c").read_text()
    start = code.index("/* ---- external display processors the ADT describes ---- */")
    end = code.index("\n}\n", code.index("int dt_set_dcpext_handoff(void *dt"))
    path = re.sub(r"/\*.*?\*/", "", code[start:end], flags=re.S)  # the code, not its comments
    for banned in ("chip_id", "apple,j", "V27_", "V26_", "V_UNKNOWN +", "os_firmware",
                   "board", "0x286", "0x48a", "0x30a", "dcpext0", "dcpext1", "dispext", "23"):
        assert banned not in path, banned


@pytest.fixture(scope="module")
def host(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("t8132-dcpext"), host_source=HOST)


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_t8132_dcpext_handoff(host, scenario):
    result = run(host, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
    for reason in REASONS[scenario]:
        assert reason in result.stdout, (reason, result.stdout)
    if scenario in ONE_REFUSAL:
        refusals = [line for line in result.stdout.splitlines()
                    if line.startswith("DCP: dcpext: ") and "handed over" not in line
                    and "__OS_LOG" not in line]
        assert len(refusals) == 1, refusals
