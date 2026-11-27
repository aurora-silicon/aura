# SPDX-License-Identifier: MIT
"""T6041 (M4 Max, J616c/J614c) internal DCP and dcpext handoff on macOS 27.2 (26B123).

Failure model recorded before changing src/kboot_display_handoff.c:
- T6041 on 27.2 (V27_2) runs DCP image 964EDE5B-8F78-38C2-A727-519CC9F0AF27 on dcp and
  dcpext, with /arm-io/disp0 clock-ids indexing /arm-io clock-frequencies from
  DCP_CLOCK_ID_BASE (256), requiring a non-zero primary clock and allowing an explicit
  0 Hz secondary clock.
- Both J616c and J614c select the 27.2 row on dcp_soc_t6041_max, and clock-frequencies
  is indexed dynamically from the IDs read out of /arm-io/disp0 clock-ids.
- Wrong chip (T6040), non-T6041 boards (J616s), unsupported OS firmware (e.g. 27.0),
  wrong or missing iop-dcp-nub UUID (including 27.2.1 71F140DC-...), missing DT display
  aliases, or missing/empty/excess/below-base/duplicate/out-of-range/zero-primary clock
  metadata must refuse before reserving __OS_LOG or performing any MMIO, leaving the FDT
  byte-for-byte unchanged.
- Upstream Linux t6041-j614c-j616c.dtsi places a single static disp0 clock phandle on
  &dcp (`clocks = <&clk_disp0>` with no `clock-names` or `apple,clock-ids`), while
  synthetic DTs may omit `clocks`. Both must be accepted, whereas pre-existing
  `apple,clock-ids`, `clock-names`, or multi-entry `clocks` must be refused.
- A valid T6041 27.2 boot publishes the retained segment/DART contract, framebuffer
  memory-region, `apple,firmware-version` and `apple,firmware-compat` <27 2 0>, and
  `fixed-clock` providers (`firmware-0`, `firmware-1`, ...) with `apple,clock-ids` on
  &dcp, publishes firmware memory, `apple,firmware-uuid`, and empty `apple,clock-ids` on
  external `dcpext` nubs whose UUID matches the 27.2 row, refuses any external nub with
  a missing or mismatched UUID without failing internal or valid external handoffs, and
  leaves display/DART/mailbox nodes disabled.
- Full FDT scratch space refuses transactionally without partial clocks or markers.
- Desktop 26.6.2 on T6041 remains unchanged.
"""

import pytest
from test_t6041_display_handoff import build_host, run

SCENARIOS = [
    "publish",
    "j614c-publish",
    "alternate-clock-ids",
    "publish-static-clock",
    "external-handoff",
    "external-uuid-mismatch",
    "repeat",
    "desktop26-preserved",
    "wrong-chip",
    "wrong-board-j616s",
    "t6040-j616s-27",
    "wrong-os",
    "wrong-uuid",
    "wrong-uuid-26b131",
    "missing-nub",
    "no-display-dt",
    "missing-clocks",
    "empty-clocks",
    "too-many-clocks",
    "clock-id-below-base",
    "duplicate-clock-ids",
    "bad-clock-order",
    "short-frequencies",
    "zero-first-clock",
    "preexisting-clock-ids",
    "preexisting-clock-names",
    "preexisting-two-clocks",
    "dt-full",
]


@pytest.fixture(scope="module")
def host27(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("t6041-27"), host_source="t6041_27_display_host.c")


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_t6041_27_2_display_handoff(host27, scenario):
    result = run(host27, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
