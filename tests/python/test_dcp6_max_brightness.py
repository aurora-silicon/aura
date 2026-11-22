# SPDX-License-Identifier: MIT
"""Panel apple,max-brightness population from ADT user-accessible-max-nits on dcp6.

Failure model recorded before changing src/kboot_display_handoff.c:
- ADT /backlight user-accessible-max-nits (16.16 fixed point, e.g. 600 nits on J616C/J616s)
  populates apple,max-brightness on dcp/panel for both T6041 (26.6.2 and 27.2) and T6040
  without any chip-ID gate in the shared dcp6 publish path.
- ADT /backlight-override takes precedence over /backlight when both carry
  user-accessible-max-nits; a /backlight-override node without that property falls back to
  /backlight, whereas an invalid or malformed /backlight-override value does not silently
  fall back to /backlight.
- Pre-existing DT apple,max-brightness on dcp/panel is preserved untouched, matching T6050.
- Absent ADT backlight properties, out-of-range values (0, fractional-only <1 nit, >10000
  nits), and wrong property sizes (2 B, 8 B) leave apple,max-brightness unset without
  failing the display handoff.
- Boundary valid values (1 nit, 10000 nits, fractional 600.5 -> 600 nits) are accepted.
- A dcp node without a panel child still succeeds and does not synthesize a panel node.
- An FDT-full write failure rolls back transactionally, leaving the DT untouched.
"""

import pytest
from test_t6041_display_handoff import build_host, run

SCENARIOS = [
    "j616c-backlight-600",
    "j616c-27-2-backlight-600",
    "j616s-backlight-600",
    "override-precedence",
    "override-node-no-prop",
    "override-invalid-no-fallback",
    "override-bad-len-no-fallback",
    "preexisting-dt-preserved",
    "absent-adt-prop",
    "zero-nits",
    "sub-nit-fractional",
    "above-range",
    "bad-len-short",
    "bad-len-long",
    "boundary-min-1",
    "boundary-max-10000",
    "fractional-truncated",
    "no-panel-child",
    "dt-full-rollback",
]


@pytest.fixture(scope="module")
def host_bl(tmp_path_factory):
    return build_host(tmp_path_factory.mktemp("dcp6-bl"),
                      host_source="dcp6_max_brightness_host.c")


def test_scenario_list_matches(host_bl):
    import subprocess
    out = subprocess.run([str(host_bl), "list"], check=True, capture_output=True, text=True)
    assert out.stdout.split() == SCENARIOS


@pytest.mark.parametrize("scenario", SCENARIOS)
def test_dcp6_max_brightness(host_bl, scenario):
    result = run(host_bl, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
