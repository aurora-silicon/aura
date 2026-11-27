# SPDX-License-Identifier: GPL-2.0-only
"""usb_init() on a SoC whose USB PHY needs the deferred CDC carrier transition (the T8140 branch)
schedules that transition for a stage that is about to run the proxy. usb_cdc_schedule() arms the
CDC watchdog only when the schedule is accepted, and usb_cdc_state_schedule() accepts 1 to 15 s:
a refused schedule leaves the proxy with no carrier and no watchdog, so a Mac with no host stays
there until someone holds the power button. Run the real state machine on the arguments usb.c
passes."""

import ctypes
import re
from pathlib import Path

from test_cdc_state import State, state_lib

ROOT = Path(__file__).resolve().parents[2]


def _usb_init_schedule_args():
    usb = (ROOT / "src/usb.c").read_text()
    body = usb[usb.index("void usb_init(void)"):]
    body = body[:body.index("\n}\n")]
    branch = body[body.index("if (chip_id == T8140) {"):]
    m = re.search(r"usb_cdc_schedule\(\s*(\d+)\s*,\s*(\d+)\s*,\s*BIT\(1\)\s*\|\s*BIT\(2\)\s*\)", branch)
    assert m, "usb_init() no longer schedules the CDC transition the way this test reads it"
    return int(m.group(1)), int(m.group(2)), (1 << 1) | (1 << 2)


def test_usb_init_schedule_is_accepted(tmp_path):
    lib = state_lib(tmp_path)
    delay, reserved, flags = _usb_init_schedule_args()
    state = State()
    assert lib.usb_cdc_state_schedule(ctypes.byref(state), delay, reserved, flags, 10) == 0
    # It runs once its deadline has passed, as the Stage 1 window's schedule does.
    assert not lib.usb_cdc_state_due(ctypes.byref(state), 10 + delay - 1)
    assert lib.usb_cdc_state_due(ctypes.byref(state), 10 + delay)


def test_control_delay_zero_is_refused(tmp_path):
    """The call as it was: no transition and, in usb_cdc_schedule(), no watchdog."""
    lib = state_lib(tmp_path)
    state = State()
    assert lib.usb_cdc_state_schedule(ctypes.byref(state), 0, 0, 6, 10) == -1


def test_watchdog_follows_an_accepted_schedule_only():
    cdc = (ROOT / "src/usb_cdc.c").read_text()
    sched = cdc[cdc.index("int usb_cdc_schedule("):]
    sched = sched[:sched.index("\n}\n")]
    assert re.search(r"if \(!ret && usb_cdc_arm_watchdog\(\)\)", sched)
