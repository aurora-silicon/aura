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
- the DARTs each processor uses (its firmware DART and its pipe's) and, with no panel, the display
  subsystem's DART: an unlocked one must be idle; a locked one's streams must translate through
  two-level tables in DRAM, whose pages in usable RAM are reserved (no-map). The PMGR power
  states follow both parents of each device.
- every ADT external processor that is not handed over on firmware given its clocks (refused for
  any reason, an incomplete internal handoff, no uuid on the internal nub, no device tree node)
  keeps its memory: its firmware segments and the table pages of its DARTs in usable RAM are
  reserved no-map and nothing is published for it (no uuid, clocks or 26.6.2 marker), unless its
  clock-gates show it is not running (power-gated, not under auto power).
Registers are only read (the fake MMIO counts writes: none in any scenario).
"""
import re

import pytest
from test_t6041_display_handoff import ROOT, build_host, run

HOST = "t8132_dcpext_host.c"
SCENARIOS = re.findall(r'^    \{"([a-z0-9-]+)", "', (ROOT / "tests" / HOST).read_text(), re.M)

H0 = "dcpext: dcpext0 handed over (image B3BA603C-3CA7-3E21-B393-44EB20BD2AEF, stream 23, 2 pipe clocks)"
H1 = "dcpext: dcpext1 handed over (image B3BA603C-3CA7-3E21-B393-44EB20BD2AEF, stream 23, 2 pipe clocks)"


def K(n, firmware, tables):
    return (f"dcpext{n} memory: not handed over, may run: {firmware} firmware and {tables} DART table "
            "page(s)")


REASONS = {
    "j713-both": [H0, H1],
    "j713-adt272-layout": [H0, H1],
    "j713-one": [H0, "dcpext1: iBoot did not leave its four-segment firmware loaded and running"],
    "j713-refused-uuid": [H0, "dcpext1: does not run the internal DCP's image B3BA603C-"],
    "j713-internal-walk-failed": ["internal DCP handoff not complete: external processors not handed over"],
    "j713-26-6-2": ["image 81BAF476-679D-3CC0-8C6A-E6B6FA659D5E is not given its clocks by the OS"],
    "j713-other-image": ["dcpext0 handed over (image 0BC97BDD-", "dcpext1 handed over (image 0BC97BDD-"],
    "j713-dcp-no-clocks": ["dcp was handed over without firmware clocks"],
    "j713-pipe-no-clocks": [H0, "dcpext1: dispext1 clock metadata is missing or invalid"],
    "j713-dcp-uuid-missing": ["dcp was not handed over with image B3BA603C-"],
    "j713-sid-mismatch": [H1, "dcpext0: iommus is not <&dart 22>, the ADT's stream"],
    "j713-dart-reg-mismatch": [H0, "dcpext1: its DART is not the ADT's apple,t8110-dart at 0x48a440000"],
    "j713-dma-range-mismatch": [H1, "dcpext0: DART apple,dma-range is not the ADT window <0x10000000000 0x800000000>"],
    "j713-window-too-small": [H1, "dcpext0: segment 0 0x10280000000+0x10000 at 0x10040000000 is kernel RAM or outside the DVA window"],
    "j713-ext-unpowered": [H0, "dcpext1: DISPEXT1_FE is not on (0xf)"],
    "j713-cpu-not-requested": [H1, "dcpext0: DISPEXT0_CPU is not requested (0x0)"],
    "j713-no-clock-gates": [H0, "dcpext1: no ADT clock-gates to check its power by"],
    "j713-dart-unlocked": [H1, "dcpext0: DART 0x286340000 is not locked (PROTECT 0x1e0)"],
    "j713-stream-no-root": [H0, "dcpext1: DART 0x48a340000 stream 23 is not translating through a root (TCR 0x1, TTBR 0x40000)"],
    "j713-stream-bypass": [H1, "dcpext0: DART 0x286340000 stream 23 is not translating through a root (TCR 0x3, TTBR "],
    "j713-dart-read-faults": [H0, "dcpext1: reading DART 0x48a340000 faulted"],
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
    "j773g-disp-dart-unlocked": ["the display subsystem: DART 0x30a300000 is not locked (PROTECT 0x1e0)",
                                 "no panel, and the display subsystem's DART is not as iBoot leaves it"],
    "j773g-disp-unpowered": ["dcp: DISP0_CPU is not requested (0x0)",
                             "no panel, and the display subsystem's DART is not as iBoot leaves it"],
    "j773g-enabled": ["dcpext0 is not status \"disabled\"", "dcpext1 is not status \"disabled\""],
    "later-soc": [H0, H1],
    "j713-tables-in-ram": [H0, H1, "dcpext0: DART 0x286340000 locked, 1 translating stream(s), 2 table page(s)",
                           "dcpext1: DART 0x48a300000 locked, 2 translating stream(s), 4 table page(s)"],
    "j713-pipe-dart-idle": [H0, H1, "dcpext1: DART 0x48a300000 unlocked and idle, 0 translating stream(s), 0 table page(s)"],
    "j713-pipe-dart-live": [H0, "dcpext1: DART 0x48a300000 is not locked (PROTECT 0x1e0) and stream 16 is live (TCR 0x1)"],
    "j713-four-level": [H1, "dcpext0: DART 0x286340000 stream 23 is not a two-level translation (TCR 0x9, "],
    "j713-l2-outside-dram": [H0, "dcpext1: DART 0x48a300000 stream 0 L2 table 0x7f0000000 is not in DRAM"],
    "j713-pipe-dart-faults": [H1, "dcpext0: reading DART 0x286300000 or its tables faulted"],
    "j713-second-parent-off": [H0, "dcpext1: DISPEXT1_AUX is not on (0x0)"],
    "j773g-disp-tables-in-ram": [H0, H1, "the display subsystem: DART 0x30a300000 locked, 2 translating stream(s), 4 table page(s)",
                                 "no panel: display subsystem and its DART enabled"],
    "j713-stream-word1": [H0, H1, "dcpext1: DART 0x48a300000 locked, 3 translating stream(s), 6 table page(s)"],
    "j713-disabled-live": [H0, H1, "dcpext0: DART 0x286340000 locked, 1 translating stream(s), 2 table page(s)"],
    "j713-zero-streams": [H0, "dcpext1: DART 0x48a340000 reports 0 streams"],
    "j713-too-many-tables": [H1, "dcpext0: DART 0x286340000 has more than 256 table pages to keep"],
    "j713-shared-table": [H0, H1, "dcpext1: DART 0x48a300000 locked, 2 translating stream(s), 2 table page(s)"],
    "j713-table-in-reservation": [H0, H1],
    "j713-table-partial-reservation": [H1, "dcpext0: DART table page 0x10100000000 overlaps /reserved-memory/fw-test@10100002000"],
    "j713-table-small-reservation": [H1, "dcpext0: DART table page 0x10100000000 overlaps /reserved-memory/fw-test@10100000000"],
    "j713-table-small-memreserve": [H1, "dcpext0: DART table page 0x10100000000 straddles /memreserve/ 0x10100000000+0x2000"],
    "j713-tables-in-ram-refused": [H1, "dcpext0: already has clocks"],
    "j713-table-in-carveout": [H1, "dcpext0: DART 0x286340000 table page 0x103f0000000 is in memory carveout 0x103f0000000+0x4000"],
    "j713-extra-child-dart": [H0, "dcpext1: DART 0x48a380000 is not locked (PROTECT 0x1e0) and stream 3 is live (TCR 0x1)"],
    "j713-iommu-cells-2": [H1, "dcpext0: piodma iommus[0] is not a one-cell apple,t8110-dart"],
    "j713-self-parent": [H0, H1],
    "j773g-tables-display-fails": [H0, H1, "display subsystem not enabled; the external processors stay disabled"],
    # A processor that is not handed over keeps its memory: K(n, firmware pages, table pages)
    "j713-refused-segs-kept": [H0, "dcpext1: segment 1 0x10121000000+0x8000 at 0x10051100000 is kernel RAM",
                               K(1, 6, 0)],
    "j713-refused-pipe-kept": [H0, "dcpext1: 0 ADT display pipes use its piodma stream 16", K(1, 0, 4)],
    "j713-refused-power-kept": [H0, "dcpext1: DISPEXT1_FE is not on (0xf)", "dcpext1 memory: DISPEXT1_FE is not on",
                                "dcpext1 memory: not fully powered: its 2 DART(s) not read", K(1, 6, 0)],
    "j713-refused-image-kept": [H0, "dcpext1: does not run the internal DCP's image B3BA603C-", K(1, 6, 4)],
    "j713-refused-unlocked-kept": [H1, "dcpext0: DART 0x286340000 is not locked (PROTECT 0x1e0)", K(0, 0, 4)],
    "j713-off-nothing-kept": [H0, "dcpext1: DISPEXT1_CPU is not requested (0x0)",
                              "dcpext1 memory: not running (its clock-gates are off): nothing to keep"],
    "j713-auto-off-kept": [H0, "dcpext1: DISPEXT1_CPU is not requested (0x10000000)", K(1, 6, 0)],
    "j713-internal-incomplete-kept": ["internal DCP handoff not complete: external processors not handed over",
                                      K(0, 6, 4), K(1, 6, 4)],
    "j713-fw27-no-uuid": ["no image uuid on /arm-io/dcp/iop-dcp-nub: external processors not handed over",
                          K(0, 6, 4), K(1, 6, 4)],
    "j713-26-6-2-no-uuid": ["image (none) is not given its clocks by the OS: external processors left to the "
                            "firmware-ready path"],
    "j713-no-dt-ext-kept": ["the device tree describes no external processor", K(0, 7, 2), K(1, 7, 2)],
    "j713-no-adt-node-kept": [H0, "/soc/dcp@48ae00000: 0 ADT processors at 0x48ae00000", K(1, 7, 0)],
    "j773g-refused-kept": ["no panel, and the display subsystem's DART is not as iBoot leaves it",
                           K(0, 6, 4), K(1, 6, 4)],
    "j773g-disp-piodma-bypass": ["the display subsystem: DART 0x30a300000 stream 16 is not a two-level translation (TCR 0x3, ",
                                 "no panel, and the display subsystem's DART is not as iBoot leaves it"],
}
# Scenarios in which exactly one processor is refused: its refusal is one log line.
ONE_REFUSAL = {"j713-one", "j713-refused-uuid", "j713-sid-mismatch", "j713-dart-reg-mismatch",
               "j713-dma-range-mismatch", "j713-window-too-small", "j713-no-mapper", "j713-no-pipe", "j713-two-pipes",
               "j713-no-adt-node", "j713-text-not-shared", "j713-bad-layout", "j713-has-clocks",
               "j713-pipe-no-clocks", "j713-ext-unpowered", "j713-cpu-not-requested",
               "j713-no-clock-gates", "j713-dart-unlocked", "j713-stream-no-root",
               "j713-stream-bypass", "j713-dart-read-faults", "j713-pipe-dart-live",
               "j713-four-level", "j713-l2-outside-dram", "j713-pipe-dart-faults",
               "j713-second-parent-off", "j713-zero-streams", "j713-too-many-tables",
               "j713-table-partial-reservation", "j713-table-small-reservation",
               "j713-table-small-memreserve", "j713-tables-in-ram-refused", "j713-table-in-carveout",
               "j713-extra-child-dart", "j713-iommu-cells-2", "j713-refused-segs-kept",
               "j713-refused-pipe-kept", "j713-refused-power-kept", "j713-refused-image-kept",
               "j713-refused-unlocked-kept", "j713-off-nothing-kept", "j713-auto-off-kept",
               "j713-no-adt-node-kept"}
# Processors that are not handed over keep their memory, with their own lines (" memory: ")


def test_every_scenario_has_its_reason():
    assert sorted(REASONS) == sorted(SCENARIOS)
    for required in ("j713-both", "j713-one", "j713-refused-uuid", "j713-internal-walk-failed",
                     "j713-26-6-2", "j773g-headless", "j713-fw27-no-uuid", "j713-off-nothing-kept"):
        assert required in SCENARIOS


def test_no_board_chip_or_release_test_in_the_adt_path():
    code = (ROOT / "src/kboot_display_handoff.c").read_text()
    start = code.index("/* ---- external display processors the ADT describes ---- */")
    end = code.index("\n}\n", code.index("int dt_set_dcpext_handoff(void *dt"))
    path = re.sub(r"/\*.*?\*/", "", code[start:end], flags=re.S)  # the code, not its comments
    for banned in ("chip_id", "apple,j", "V27_", "V26_", "V_UNKNOWN +", "os_firmware",
                   "board", "0x286", "0x48a", "0x30a", "dcpext0", "dcpext1", "dispext", "23"):
        assert banned not in path, banned


def function_body(code, signature):
    start = code.index(signature)
    return code[start:code.index("\n}\n", start)]


def test_no_silent_early_return():
    """Every way out of the handoff before a handoff says why, in one line."""
    code = (ROOT / "src/kboot_display_handoff.c").read_text()
    for signature in ("static void dcpext_hand_over(void *dt", "int dt_set_dcpext_handoff(void *dt"):
        body = function_body(code, signature).splitlines()
        for n, line in enumerate(body):
            if line.strip() not in ("return;", "return 0;", "return 1;"):
                continue
            before = "\n".join(body[max(0, n - 6):n])
            assert "dlog(" in before or "dcp6_scratch(dt)" in before or "display" in before or \
                "dcpext_keep_rest(dt);" in before, (signature, n, before)


def test_every_firmware_27_outcome_keeps_memory():
    """Once the firmware is given its clocks, the handoff always ends in dcpext_keep_rest() (every
    processor not handed over keeps its memory) and returns 1: the only 0 (kboot.c's 26.6.2
    firmware-ready path) is decided from fw_clocks before anything else."""
    code = re.sub(r"/\*.*?\*/", "", (ROOT / "src/kboot_display_handoff.c").read_text(), flags=re.S)
    body = function_body(code, "int dt_set_dcpext_handoff(void *dt")
    head, tail = body.split("if (!fw_clocks) {", 1)
    assert "return" not in head and "adt_getprop" not in head.split("dcp6.soc")[1]
    first, rest = tail.split("return 0;\n    }", 1)
    assert "return" not in first
    assert [line.strip() for line in rest.strip().splitlines()] == \
        ["dcpext_hand_over(dt, internal);", "dcpext_keep_rest(dt);", "return 1;"]


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
                    and "__OS_LOG" not in line and " translating stream(s), " not in line
                    and " memory: " not in line]
        assert len(refusals) == 1, refusals
