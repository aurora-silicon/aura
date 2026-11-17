# SPDX-License-Identifier: MIT
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "proxyclient"))

from aura.hv import pmgr_hook_devs
from aura.proxy import IODEV


def test_usb_iodev_keeps_its_atc_alive():
    assert pmgr_hook_devs(IODEV.USB0) == ["UART0", "ATC0_USB", "ATC0_USB_AON"]
    assert pmgr_hook_devs(IODEV.USB3) == ["UART0", "ATC3_USB", "ATC3_USB_AON"]


@pytest.mark.parametrize("iodev", [IODEV.UART, IODEV.DOCKCHANNEL_UART, IODEV.FB, IODEV.USB_VUART])
def test_non_usb_iodev_has_no_atc(iodev):
    # DOCKCHANNEL_UART (1) used to build the name "ATC-3_USB", which is not in any ADT.
    assert pmgr_hook_devs(iodev) == ["UART0"]
