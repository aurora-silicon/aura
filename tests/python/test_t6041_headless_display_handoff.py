# SPDX-License-Identifier: MIT
"""Headless T6041 (J575c) internal DCP sleep-host handoff.

Verifies:
- A headless J575c DT without `apple,sleep-host-stop` keeps the existing external-only path
  (no internal handoff, display subsystem and disp0 DART enabled by aura) so older kernels
  continue to boot unchanged.
- A headless J575c DT with `apple,sleep-host-stop` hands off the internal DCP without a panel
  or /vram segment, skips iBoot's dummy boot framebuffer pages on disp0 SID 0, leaves the
  display subsystem and disp0 DART disabled for the kernel gate, adds the boot framebuffer
  memory-region to the display subsystem (preserving any DART-mapped heap segment), and shares
  `dcp-text@` with the external processors.
- A second call is a byte-for-byte no-op.
- Out-of-bounds disp0 pages (outside both firmware segments and /vram) or a zero-sized /vram
  refuse the internal handoff cleanly and fall back to external-only display enablement.
- Panel T6041 Macs (J616c) still refuse an ADT whose DCP segments omit /vram.
"""

import pytest
from test_t6041_display_handoff import build_host, run

REASONS = {
    "j575c-external-only": "no panel: the internal DCP is not handed over",
    "j575c-sleep-host": "no panel: display subsystem framebuffer added",
    "j575c-sleep-host-heap": "no panel: display subsystem framebuffer added",
    "j575c-sleep-host-repeat": "handoff already published",
    "j575c-sleep-host-bad-disp-pa": "outside the firmware segments",
    "j575c-sleep-host-zero-vram": "outside the firmware segments",
    "j616c-no-vram": "no dcp segment is /vram",
}


@pytest.fixture(scope="module")
def host_headless(tmp_path_factory):
    return build_host(
        tmp_path_factory.mktemp("t6041-headless"),
        host_source="t6041_headless_display_host.c",
    )


@pytest.mark.parametrize("scenario", list(REASONS))
def test_t6041_headless_display_handoff(host_headless, scenario):
    result = run(host_headless, scenario)
    assert result.returncode == 0, result.stdout + result.stderr
    assert REASONS[scenario] in result.stdout, result.stdout
    assert result.stdout.rstrip().endswith(f"{scenario}: pass"), result.stdout
    assert "FAIL" not in result.stdout, result.stdout
