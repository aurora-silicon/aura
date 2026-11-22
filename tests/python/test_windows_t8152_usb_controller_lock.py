# SPDX-License-Identifier: GPL-2.0-only
"""G6 r3: the proxy console and the Windows vUART share one DWC3 controller and one lock.

G6 session 230552 ended about 440 s after guest entry on an EL2 SError (ESR 0xbe000000) on
cpu 9. cpu 9 was emulating a guest byte write to the vUART (UTXH, IPA 0x331200020): the EL2 PC
was the entry of usb_vuart_write(), right after update_irq() had polled the DWC3 (event loop,
endpoint commands). At the same time other CPUs were printing "site emulated" lines from the PMU
site fast path, which does not hold the hypervisor lock. Those console writes start bulk-in
transfers on the same controller under console_lock and the USB iodev's lock. The vUART path
held only the vUART iodev's lock, so two CPUs could drive the controller at once.

Failure modes:
  L-1 a vUART accessor (lock, can_read, can_write, read, write) reaches the controller without
      console_lock while a printf on another CPU is inside it (threaded host run, real iodev.c);
  L-2 the pre-r3 wiring (no console_controller on the vUART iodev) does not show the overlap,
      so L-1 would prove nothing;
  L-3 the Windows vUART iodev stops declaring that it drives the console's controller;
  L-4 the lock order changes: console_lock must come before the device lock, and the console
      must be marked busy (in_iodev) so a nested printf cannot re-enter the controller.
Nothing here touches hardware.
"""
import json
import pathlib
import re
import shutil
import subprocess

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
HOST = REPO / "tests/e2e/windows_usb_controller_lock_host.c"


def _build_and_run(tmp_path, *defines):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    binary = tmp_path / ("lock-host" + "".join(d.lower() for d in defines))
    subprocess.run(
        [cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-function", "-Wno-missing-field-initializers", "-pthread",
         "-fsanitize=address,undefined", "-I", str(REPO / "src"), "-idirafter",
         str(REPO / "sysinc"), *("-D" + d for d in defines), str(HOST), "-o", str(binary)],
        check=True, capture_output=True, text=True, timeout=120,
    )
    run = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=120)
    return json.loads(run.stdout)


def test_l1_vuart_and_console_never_overlap_in_the_controller(tmp_path):
    result = _build_and_run(tmp_path)
    assert result["console_controller"] is True
    assert result["entries"] > 0
    assert result["overlaps"] == 0
    assert result["most_inside"] == 1


def test_l2_the_pre_r3_wiring_shows_the_race(tmp_path):
    result = _build_and_run(tmp_path, "UNFIXED")
    assert result["console_controller"] is False
    assert result["overlaps"] > 0
    assert result["most_inside"] >= 2


def test_l3_the_windows_vuart_iodev_drives_the_console_controller():
    usb = (REPO / "src/usb.c").read_text()
    block = usb[usb.index("struct iodev iodev_usb_vuart = {"):]
    block = block[: block.index("};")]
    assert re.search(r"#ifdef AURA_WINDOWS\s*(/\*.*?\*/\s*)?\.console_controller = true,\s*#endif",
                     block, re.S)
    header = (REPO / "src/iodev.h").read_text()
    assert "bool console_controller;" in header


def test_l4_every_accessor_takes_console_lock_first():
    src = (REPO / "src/iodev.c").read_text()
    take = src[src.index("static void iodev_take("):]
    take = take[: take.index("\n}\n")]
    assert take.index("spin_lock(&console_lock)") < take.index("in_iodev++") \
        < take.index("spin_lock(&iodevs[id]->lock)")
    drop = src[src.index("static void iodev_drop("):]
    drop = drop[: drop.index("\n}\n")]
    assert drop.index("spin_unlock(&iodevs[id]->lock)") < drop.index("in_iodev--") \
        < drop.index("spin_unlock(&console_lock)")
    for name in ["iodev_can_read", "iodev_can_write", "iodev_read", "iodev_write",
                 "iodev_write_nonblocking", "iodev_write_atomic", "iodev_try_write_atomic",
                 "iodev_queue", "iodev_flush", "iodev_lock", "iodev_unlock"]:
        m = re.search(r"\n(?:ssize_t|bool|void) " + name + r"\(iodev_id_t id[^)]*\)\n\{(.*?)\n\}\n",
                      src, re.S)
        assert m, name
        body = m.group(1)
        assert "spin_lock(&iodevs[id]->lock)" not in body, name
        assert "spin_unlock(&iodevs[id]->lock)" not in body, name
        if name != "iodev_unlock":
            assert "iodev_take(id);" in body or name == "iodev_unlock", name
        if name != "iodev_lock":
            assert "iodev_drop(id);" in body, name
