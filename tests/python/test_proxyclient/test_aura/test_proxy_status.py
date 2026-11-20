# SPDX-License-Identifier: GPL-2.0-only

import struct

import pytest

from proxyclient.aura.proxy import AuraProxy, ProxyRemoteError, UartInterface


def test_remote_operation_failure_is_distinct_from_unknown_opcode():
    assert AuraProxy.P_KBOOT_BOOT_STORAGE == 0x705
    class Interface:
        def proxyreq(self, request, **_kwargs):
            opcode = struct.unpack_from("<Q", request)[0]
            return struct.pack("<QqQ", opcode, -2, 0)

    proxy = object.__new__(AuraProxy)
    proxy.iface = Interface()
    proxy.debug = False
    with pytest.raises(ProxyRemoteError, match="Remote operation failed"):
        proxy._request(proxy.P_SMP_CALL, 99, 1)


def test_memory_reads_are_bounded_for_dockchannel():
    class Reader:
        def __init__(self):
            self.calls = []
            # DockChannel, not KIS: exercises readmem's plain chunked path.
            self.is_kis = False

        def _readmem_one(self, addr, size):
            self.calls.append((addr, size))
            return b"x" * size

    reader = Reader()
    data = UartInterface.readmem(reader, 0x1000, 40 * 1024)
    assert len(data) == 40 * 1024
    assert reader.calls == [
        (0x1000, 16 * 1024),
        (0x5000, 16 * 1024),
        (0x9000, 8 * 1024),
    ]
