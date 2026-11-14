# SPDX-License-Identifier: MIT
"""bootstrap_port() over a slow link, and stale replies at the NOP sync point.

A fake aura proxy on a PTY answers P_IODEV_WHOAMI late, the way a console
bridged through USB (KIS/DockChannel via kisd) sometimes does. A whoami reply
that arrives after the client stopped waiting used to be read by the following
NOP: "Reply command mismatch: Expected 0x00aa55ff, got 0x01aa55ff".
"""
import os
import select
import struct
import sys
import threading
import time

import pytest

from proxyclient.aura import proxyutils
from proxyclient.aura.proxy import AuraProxy, IODEV, UartCMDError, UartInterface

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="needs a POSIX PTY")

P_GET_CHIPID = 0x013
P_IODEV_WHOAMI = 0x905


def checksum(data):
    s = 0xDEADBEEF
    for c in data:
        s = ((s * 31337) + (c ^ 0x5A)) & 0xFFFFFFFF
    return (s ^ 0xADDEDBAD) & 0xFFFFFFFF


def packet(cmd, data, status=0):
    body = struct.pack("<Ii", cmd, status) + data
    return body + struct.pack("<I", checksum(body))


class FakeTarget:
    """Answers NOP and proxy requests on the master side of a PTY."""

    def __init__(self, whoami_delay):
        import tty

        self.whoami_delay = whoami_delay
        self.master, slave = os.openpty()
        tty.setraw(slave)
        self.path = os.ttyname(slave)
        self.slave = slave
        self.stop = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        buf = b""
        os.write(self.master, b"Stage 1: waiting 15 s for a host\r\n")
        while not self.stop.is_set():
            # Poll so close() never races a blocking read (closing an fd that
            # another thread is reading blocks on macOS).
            if not select.select([self.master], [], [], 0.02)[0]:
                continue
            d = os.read(self.master, 4096)
            if not d:
                return
            buf += d
            while True:
                i = buf.find(b"\xff\x55\xaa")
                if i < 0 or len(buf) < i + UartInterface.CMD_LEN + 8:
                    break
                pkt = buf[i:i + UartInterface.CMD_LEN + 8]
                buf = buf[i + UartInterface.CMD_LEN + 8:]
                cmd = struct.unpack("<I", pkt[:4])[0]
                if cmd == UartInterface.REQ_NOP:
                    data = struct.pack("<QQQ", 1, 0, 0)
                elif cmd == UartInterface.REQ_PROXY:
                    op = struct.unpack("<Q", pkt[4:12])[0]
                    ret = {P_IODEV_WHOAMI: IODEV.DOCKCHANNEL_UART, P_GET_CHIPID: 0x8112}.get(op, 0)
                    if op == P_IODEV_WHOAMI:
                        time.sleep(self.whoami_delay)
                    data = struct.pack("<QqQ", op, 0, ret)
                else:
                    continue
                os.write(self.master, packet(cmd, data))

    def close(self):
        self.stop.set()
        self.thread.join()
        os.close(self.master)
        os.close(self.slave)


@pytest.fixture
def no_custom_baud(monkeypatch):
    # PTYs have no line rate; macOS rejects the custom-baud ioctl pyserial uses for 1500000.
    import serial.serialposix

    monkeypatch.setattr(serial.serialposix.Serial, "_set_special_baudrate", lambda self, baud: None, raising=False)


def connect(target):
    iface = UartInterface(target.path)
    proxy = AuraProxy(iface)
    proxyutils.bootstrap_port(iface, proxy)
    return iface, proxy


@pytest.mark.parametrize("delay", [0.05, 0.3, 1.0])
def test_bootstrap_waits_for_a_slow_whoami(monkeypatch, no_custom_baud, delay):
    monkeypatch.delenv("AURA_BOOTSTRAP_TIMEOUT", raising=False)
    target = FakeTarget(delay)
    try:
        iface, proxy = connect(target)
        assert proxy.get_chipid() == 0x8112
        assert iface.dev.baudrate == UartInterface.DEFAULT_BAUD_RATE
    finally:
        target.close()


def test_whoami_later_than_the_timeout_is_dropped_at_the_nop(monkeypatch, no_custom_baud, capsys):
    monkeypatch.setenv("AURA_BOOTSTRAP_TIMEOUT", "0.4")
    target = FakeTarget(0.6)
    try:
        iface, proxy = connect(target)
        assert proxy.get_chipid() == 0x8112
    finally:
        target.close()
    assert "Dropped stale reply: expected 0x00aa55ff, got 0x01aa55ff" in capsys.readouterr().err


class BufferDev:
    def __init__(self, data):
        self.data = data
        self.timeout = 1

    def read(self, n):
        out, self.data = self.data[:n], self.data[n:]
        return out


def iface_with(data):
    iface = object.__new__(UartInterface)
    iface.dev = BufferDev(data)
    iface.debug = False
    iface.tty_enable = False
    iface.handlers = {}
    iface.evt_handlers = {}
    return iface


def test_reply_still_rejects_a_mismatch_outside_the_sync_point():
    stale = packet(UartInterface.REQ_PROXY, struct.pack("<QqQ", P_IODEV_WHOAMI, 0, 1))
    with pytest.raises(UartCMDError, match="Expected 0x00aa55ff, got 0x01aa55ff"):
        iface_with(stale).reply(UartInterface.REQ_NOP)


def test_drop_stale_reads_past_queued_replies():
    stale = packet(UartInterface.REQ_PROXY, struct.pack("<QqQ", P_IODEV_WHOAMI, 0, 1))
    good = packet(UartInterface.REQ_NOP, struct.pack("<QQQ", 1, 0, 0))
    iface = iface_with(stale + stale + good)
    assert iface.reply(UartInterface.REQ_NOP, drop_stale=True) == struct.pack("<QQQ", 1, 0, 0)
    assert iface.dev.data == b""
