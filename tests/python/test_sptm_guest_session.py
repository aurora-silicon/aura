# SPDX-License-Identifier: MIT
"""Unit tests for the SPTM-virt host session helper and vuart tailer."""

import importlib.util
import io
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
VUART_TOOL = REPO / "proxyclient/tools/sptm_vuart_tail.py"
SESSION_SH = REPO / "proxyclient/tools/sptm_guest_session.sh"


def _load_vuart():
    spec = importlib.util.spec_from_file_location("sptm_vuart_tail", VUART_TOOL)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_vuart_tail_help():
    r = subprocess.run([sys.executable, str(VUART_TOOL), "--help"],
                       capture_output=True, text=True, timeout=30)
    assert r.returncode == 0, r.stderr
    assert "--stop-file" in r.stdout


def test_vuart_tail_sets_dtr_and_honors_stop_file(tmp_path):
    mod = _load_vuart()
    stop_file = tmp_path / "run.stop"
    out = io.BytesIO()
    events = []

    class FakeSerial:
        def __init__(self, path, baud, timeout=0.5, dsrdtr=False, rtscts=False):
            events.append(("open", path, baud))
            self.is_open = True
            self.dtr = False
            self.rts = False
            self._chunks = [b"Darwin Kernel Version 27.2.0\n", b"boot ok\n"]

        @property
        def in_waiting(self):
            return len(self._chunks[0]) if self._chunks else 0

        def read(self, size=1):
            assert size >= 1
            if not self._chunks:
                stop_file.write_text("stop\n")
                return b""
            chunk = self._chunks.pop(0)
            assert size <= len(chunk)
            return chunk[:size]

        def close(self):
            self.is_open = False
            events.append(("close",))

    mod.tail_vuart("/dev/ttyACM1", out, stop_file=str(stop_file),
                   serial_cls=FakeSerial, sleep_fn=lambda _: None)
    text = out.getvalue().decode()
    assert "DTR=1" in text
    assert "Darwin Kernel Version 27.2.0\n" in text
    assert "boot ok\n" in text
    assert "vuart stop file seen" in text
    assert events[0] == ("open", "/dev/ttyACM1", 115200)
    assert events[-1] == ("close",)


def test_guest_session_script_syntax_and_help():
    r_chk = subprocess.run(["bash", "-n", str(SESSION_SH)],
                           capture_output=True, text=True, timeout=30)
    assert r_chk.returncode == 0, r_chk.stderr
    r_help = subprocess.run(["bash", str(SESSION_SH), "--help"],
                            capture_output=True, text=True, timeout=30)
    assert r_help.returncode == 0, r_help.stderr
    assert "--img-dir" in r_help.stdout
    assert "--out" in r_help.stdout
