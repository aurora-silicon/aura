# SPDX-License-Identifier: MIT
"""j804-windows-nvme-serror: a forwarded guest SError on a D5 (NVMe-backed AHCI) carrier.

2026-11-17, carrier 353b3cf5 (D5, read-write): the guest took an SError, the carrier forwarded
it ({'reason': 2, 'code': 3, ...}), the launcher answered EXIT_GUEST and then waited with no
link timeout; the carrier printed nothing more for ~13 min, until ^C sent the proxy break, after
which every CPU exited and the NVMe disk closed CLEAN.  Every value below is synthetic.  The launcher must now:

  - keep the carrier's SError report (its "HV: fatal:" lines) and repeat what matters,
  - never wait unbounded after EXIT_GUEST: a silent carrier gets the break ^C sends, a bounded
    number of times, then the launcher gives up (WindowsGuestExitTimeout),
  - then report the NVMe disk: CLEAN or FAILED as the resident says, or UNKNOWN (with the
    carrier's last console verdict) when it does not answer.

These tests drive the real UartInterface, AuraProxy and HV exception handling against a fake
carrier on the wire (no device is opened), and run run_guest's close and summary functions
unchanged (extracted from its source).
"""
import ast
import io
import struct
import sys
import types
from contextlib import redirect_stdout
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))

from aura.hv import HV, WindowsGuestAborted, WindowsGuestExitTimeout  # noqa: E402
from aura.proxy import (EXC, START, AuraProxy, ExcInfo, ProxyError, ProxyRemoteError,  # noqa: E402
                        UartError, UartInterface)
from aura import windows as w  # noqa: E402

INFO = 0x10008402fa0
FRAME = struct.pack("<32Q", *range(32)) + struct.pack(
    "<14Q", 0x60000005, 0xfffff80000101004, 0xbe000000, 0, 0, 0x7ff0, 0xfffff80000400000,
    0x10008402fa0, 6, 0x80010100, 0, 0, 0, 0)

REPORT = [
    "[100.000200] HV: fatal: guest SError on cpu 6: ESR_EL2 0xbe000000 (SError, ISS 0); "
    "guest PC 0xfffff80000101004 EL1h SPSR 0x600003c5; FAR_EL2 0x0 HPFAR_EL2 0x0 (both stale)",
    "[100.000201] HV: fatal: cpu 6 previous exit S 4 us before: ESR 0x5e000000",
    "[100.000202] HV: fatal: cpu 6 SError vs previous exit: verdict 2: taken at that exit's "
    "return point; not pending when the guest exited, pending when EL2 returned",
    "[100.000203] HV: fatal: AHCI NVMe backend RUNNING, partition LBAs [100, 200], "
    "read-write, wrote 1, controller lock owner -1",
    "[100.000204] HV: fatal: AHCI NVMe last command #17: write block 150 (partition +50), "
    "8 block(s) on cpu 3, issued 900 us ago, completed in 120 us, ok",
    "[100.000205] HV: fatal: attribution: no guest address near the SError reaches a "
    "passthrough or identity region",
    "[100.000206] HV: fatal: guest SError: the host decides what follows",
]
EXIT_LINES = [
    "[160.000100] HV: Exiting from CPU 9",
    "[160.000200] HV: All CPUs exited",
    "[160.750000] HV: T8152: AHCI NVMe: guest session failed, aborted or refused: CLEAN "
    "(this session wrote; controller shutdown confirmed)",
]


class FakeCarrier:
    """The proxy end of the link.  `mode`:
      exit_then_silent: answers EXIT, then nothing until the break (the 2026-11-17 hang);
      exit_unanswered:  the EXIT itself is answered only after the break;
      dead:             nothing after the forwarded exception, breaks included;
      link_drop:        the USB link goes away after the EXIT (a carrier reset);
      normal:           no exception: hv_start returns SYSTEM_OFF."""

    def __init__(self, mode, nvme_state=3, answers_status=True):
        self.mode = mode
        self.timeout = None
        self.out = bytearray()
        self.inbuf = bytearray()
        self.breaks = 0
        self.exits = 0
        self.pending_exit = False
        self.dropped = False
        self.nvme_state = nvme_state
        self.answers_status = answers_status
        self.waited_unbounded = False
        self.checksum = UartInterface.checksum.__get__(self)

    # pyserial surface used by UartInterface
    def flushOutput(self):
        pass

    def flushInput(self):
        pass

    def read(self, n):
        if self.dropped:
            raise OSError("device disconnected")
        if not self.out:
            if self.timeout is None:
                self.waited_unbounded = True
                raise AssertionError("the launcher waits with no timeout on a silent carrier: "
                                     "it would hang")
            return b""
        data = bytes(self.out[:n])
        del self.out[:n]
        return data

    def write(self, data):
        self.inbuf += data
        while self.inbuf:
            if self.inbuf[:1] == b"!":
                del self.inbuf[:1]
                self.on_break()
                continue
            if len(self.inbuf) < 64:
                break
            pkt = bytes(self.inbuf[:64])
            del self.inbuf[:64]
            self.on_command(pkt)
        return len(data)

    # carrier behaviour
    def console(self, lines):
        for line in lines:
            self.out += (line + "\n").encode()

    def reply(self, typ, data, status=0):
        body = struct.pack("<Ii", typ, status) + data.ljust(24, b"\0")
        self.out += body + struct.pack("<I", self.checksum(body))

    def proxy_reply(self, opcode, retval=0, status=AuraProxy.S_OK):
        self.reply(UartInterface.REQ_PROXY, struct.pack("<QqQ", opcode, status, retval))

    def on_command(self, pkt):
        typ = struct.unpack("<I", pkt[:4])[0]
        body = pkt[4:60]
        assert struct.unpack("<I", pkt[60:])[0] == self.checksum(pkt[:60])
        if typ == UartInterface.REQ_MEMREAD:
            addr, size = struct.unpack("<QQ", body[:16])
            data = FRAME[:size] if addr == INFO else bytes(size)
            self.reply(UartInterface.REQ_MEMREAD, struct.pack("<I", self.checksum(data)))
            self.out += data
            return
        assert typ == UartInterface.REQ_PROXY
        opcode, *args = struct.unpack("<7Q", body)
        if opcode == AuraProxy.P_HV_START:
            if self.mode == "normal":
                self.console(["[10.0] HV: Windows requested SYSTEM_OFF; draining guest CPUs"])
                self.proxy_reply(opcode, 1)
                return
            self.console(REPORT)
            start = struct.pack("<IIQ", int(START.EXCEPTION_LOWER), int(EXC.SERROR), INFO)
            self.reply(UartInterface.REQ_BOOT, start)
        elif opcode == AuraProxy.P_EXIT:
            self.exits += 1
            if self.mode == "exit_unanswered":
                self.pending_exit = True
            else:
                self.proxy_reply(opcode)
            if self.mode == "link_drop":
                self.dropped = True
        elif opcode == AuraProxy.P_HV_WINDOWS_AHCI:
            if self.mode == "dead" or not self.answers_status:
                return
            op, index = args[0], args[1]
            if op == w.AHCI_OP_NVME_STATUS:
                value = self.nvme_state if index == w.NVME_STATUS["state"] else \
                    (1 if index == w.NVME_STATUS["wrote"] else 7)
                self.proxy_reply(opcode, value)
            else:
                self.proxy_reply(opcode, 0)
        else:
            self.proxy_reply(opcode, 0)

    def on_break(self):
        self.breaks += 1
        if self.mode == "dead":
            return
        if self.pending_exit:
            self.pending_exit = False
            self.proxy_reply(AuraProxy.P_EXIT)
        if self.mode in ("exit_then_silent", "exit_unanswered") and self.breaks == 1:
            self.console(EXIT_LINES)
            # hv_start() returned -1: S_ERROR, as proxy.c answers it.
            self.proxy_reply(AuraProxy.P_HV_START, 0, AuraProxy.S_ERROR)


def make_hv(mode, **kw):
    dev = FakeCarrier(mode, **kw)
    iface = UartInterface(device=dev)
    p = AuraProxy(iface)
    hv = HV.__new__(HV)
    hv.iface, hv.p = iface, p
    hv.windows = True
    hv.windows_serror = "abort"
    hv.windows_serrors_continued = 0
    hv.windows_abort_reason = None
    hv.windows_fatal_lines = []
    hv.windows_exit_breaks = 0
    hv.windows_nvme_console = None
    hv.entry, hv.guest_base, hv.bootargs_off = 0x10000800, 0x10000000, 0x100
    hv.WINDOWS_EXIT_QUIET_S = 0.01  # the fake answers a finite wait at once
    lines = []
    hv.log = lines.append
    iface.set_handler(START.EXCEPTION_LOWER, EXC.SERROR, hv.handle_exception)
    # As HV.start() does around hv_start for a Windows guest.
    iface.tty_line_hook = hv._windows_tty_line
    dev.timeout = None
    return hv, dev, lines


def run_guest_functions(p, iface):
    """run_guest.py's close helpers, unchanged, with this test's proxy."""
    tree = ast.parse((REPO / "proxyclient/tools/run_guest.py").read_text())
    want = {"bounded_timeout", "windows_t8152_ahci_nvme_close", "windows_t8152_fatal_summary"}
    body = [n for n in tree.body
            if (isinstance(n, ast.FunctionDef) and n.name in want) or
            (isinstance(n, ast.Assign) and
             any(getattr(t, "id", None) == "NVME_DETACH_TIMEOUT_S" for t in n.targets))]
    assert {n.name for n in body if isinstance(n, ast.FunctionDef)} == want
    ns = {"p": p, "iface": iface, "ProxyRemoteError": ProxyRemoteError,
          "ProxyError": ProxyError, "UartError": UartError}
    exec(compile(ast.Module(body=body, type_ignores=[]), "run_guest.py", "exec"), ns)
    return ns


def after_start(hv, dev, timeout=3):
    """run_guest after hv.start() raised: HV.start()'s finally restored the link timeout."""
    dev.timeout = timeout
    hv.iface.tty_line_hook = None
    rg = run_guest_functions(hv.p, hv.iface)
    out = io.StringIO()
    with redirect_stdout(out):
        rg["windows_t8152_fatal_summary"](hv)
        verdict = rg["windows_t8152_ahci_nvme_close"](-1, {"flags": 1}, hv)
    return verdict, out.getvalue()


@pytest.mark.parametrize("mode", ["exit_then_silent", "exit_unanswered"])
def test_silent_carrier_gets_the_break_and_the_disk_verdict_follows(mode):
    hv, dev, lines = make_hv(mode)
    with redirect_stdout(io.StringIO()):
        with pytest.raises(ProxyRemoteError, match="Remote operation failed"):
            hv._windows_hv_start()
    assert not dev.waited_unbounded
    assert dev.exits == 1 and dev.breaks == 1 and hv.windows_exit_breaks == 1
    assert hv.windows_abort_reason == {"reason": 2, "code": 3, "info": hex(INFO)}
    assert any("Guest exception context: cpu 6" in l for l in lines)  # the frame was read
    assert any("sending the proxy break ('!', as ^C does) 1/2" in l for l in lines)
    # The carrier's report was kept and the NVMe close line seen on its console.
    assert len(hv.windows_fatal_lines) == len(REPORT)
    assert "CLEAN (this session wrote; controller shutdown confirmed)" in hv.windows_nvme_console
    verdict, out = after_start(hv, dev)
    assert verdict is True
    summary = out.index("the carrier's fatal report")
    assert out.index("aborted on a forwarded exception: {'reason': 2, 'code': 3") < summary
    for key in ("guest SError on cpu 6", "verdict 2:", "AHCI NVMe backend RUNNING",
                "AHCI NVMe last command #17", "no guest address near"):
        assert summary < out.index(key) < out.index("T8152 AHCI NVMe: CLEAN")


def test_dead_carrier_ends_in_unknown_never_a_hang():
    hv, dev, lines = make_hv("dead")
    with redirect_stdout(io.StringIO()):
        with pytest.raises(WindowsGuestExitTimeout, match="did not finish the guest exit"):
            hv._windows_hv_start()
    assert not dev.waited_unbounded
    assert dev.breaks == hv.WINDOWS_EXIT_BREAKS == 2
    verdict, out = after_start(hv, dev)
    assert verdict is False
    assert "guest SError on cpu 6" in out
    assert "T8152 AHCI NVMe: UNKNOWN" in out
    assert "the carrier's console last said" not in out  # it printed no close line
    assert "T8152 AHCI NVMe: CLEAN" not in out


def test_unknown_names_the_carriers_last_console_verdict():
    hv, dev, _ = make_hv("exit_then_silent", answers_status=False)
    with redirect_stdout(io.StringIO()):
        with pytest.raises(ProxyRemoteError):
            hv._windows_hv_start()
    verdict, out = after_start(hv, dev)
    assert verdict is False
    assert "UNKNOWN" in out
    assert "the carrier's console last said: 'HV: T8152: AHCI NVMe: guest session failed, " \
           "aborted or refused: CLEAN" in out


def test_failed_close_is_reported_failed():
    hv, dev, _ = make_hv("exit_then_silent", nvme_state=4)
    with redirect_stdout(io.StringIO()):
        with pytest.raises(ProxyRemoteError):
            hv._windows_hv_start()
    verdict, out = after_start(hv, dev)
    assert verdict is False and "FAILED: the controller did NOT confirm its shutdown" in out


def test_link_drop_after_the_exit_is_an_exit_timeout():
    hv, dev, _ = make_hv("link_drop")
    with redirect_stdout(io.StringIO()):
        with pytest.raises(WindowsGuestExitTimeout, match="link to the carrier failed"):
            hv._windows_hv_start()


def test_without_the_bound_the_launcher_would_hang():
    # The detector works: with the old unbounded wait the fake carrier sees a wait with no
    # timeout on a silent link (2026-11-17's 13 minutes).
    hv, dev, _ = make_hv("exit_then_silent")
    hv._windows_bound_exit_wait = lambda: None
    with redirect_stdout(io.StringIO()):
        with pytest.raises(AssertionError, match="would hang"):
            hv._windows_hv_start()
    assert dev.waited_unbounded and dev.breaks == 0


def test_a_normal_exit_keeps_the_unbounded_wait():
    hv, dev, _ = make_hv("normal")
    with redirect_stdout(io.StringIO()):
        assert hv._windows_hv_start() == 1
    assert dev.timeout is None and dev.breaks == 0 and hv.windows_abort_reason is None


def test_continue_policy_does_not_bound_the_session():
    hv, dev, _ = make_hv("exit_then_silent")
    hv.windows_serror = "continue"
    hv.p = types.SimpleNamespace(exit=lambda ret: None)
    hv.iface.readmem = lambda addr, size: FRAME[:size]
    hv.handle_exception(START.EXCEPTION_LOWER, EXC.SERROR, INFO)
    assert dev.timeout is None and hv.windows_abort_reason is None


def test_start_and_run_guest_wiring():
    hvsrc = (REPO / "proxyclient/aura/hv/__init__.py").read_text()
    start = hvsrc[hvsrc.index("    def start(self):"):]
    assert start.index("self.iface.tty_line_hook = self._windows_tty_line") < \
        start.index("result = self._windows_hv_start()") < \
        start.index("raise WindowsGuestAborted(") < start.index("self.iface.tty_line_hook = None")
    abort = hvsrc[hvsrc.index("    def _abort_windows_guest("):]
    abort = abort[:abort.index("\n    def ", 10)]
    assert abort.index("self._windows_bound_exit_wait()") < \
        abort.index("self._log_windows_exc_info(info)") < abort.index("self.p.exit(EXC_RET.EXIT_GUEST)")
    rg = (REPO / "proxyclient/tools/run_guest.py").read_text()
    t8152 = rg[rg.index("def run_windows_t8152(caps):"):]
    fail = t8152[t8152.index("except (ProxyRemoteError, WindowsGuestAborted, "
                             "WindowsGuestExitTimeout) as exc:"):]
    fail = fail[:fail.index('sys.exit(f"Hypervisor refused or failed')]
    assert fail.index("windows_t8152_fatal_summary(hv)") < \
        fail.index("windows_t8152_ahci_nvme_close(-1, ahci, hv)")
    close = rg[rg.index("def windows_t8152_ahci_nvme_close("):rg.index("def windows_t8152_fatal_summary(")]
    # The NVMe close report never waits without a bound.
    assert "no_timeout(" not in close
    assert "bounded_timeout(NVME_DETACH_TIMEOUT_S, p.hv_windows_ahci, w.AHCI_OP_DETACH)" in close
    # The --windows-serror option and the D5 NVMe options coexist.
    for opt in ("'--windows-serror'", "'--windows-ahci-backend'", "'--windows-ahci-partition'",
                "'--windows-ahci-nvme-access'"):
        assert f"parser.add_argument({opt}" in rg


def _body(src, head):
    start = src.index(head)
    depth, i = 0, src.index("{", start)
    while True:
        depth += {"{": 1, "}": -1}.get(src[i], 0)
        if depth == 0:
            return src[start:i + 1]
        i += 1


def test_carrier_serror_report_carries_the_nvme_record_and_keeps_the_close():
    win = REPO / "src/hv/windows"
    fatal = (win / "hv_fatal.c").read_text()
    report = _body(fatal, "void hv_windows_fatal_report(")
    # NVMe/AHCI next to USB, GPU and input in every fatal report (the guest SError's included).
    assert report.index("hv_ahci_fatal_report(now, freq);") < report.index("hv_usb_fatal_report();") \
        < report.index("hv_gpu_fatal_report();") < report.index("hv_input_fatal_report();")
    guest = _body(fatal, "void hv_windows_guest_serror_report(")
    assert 'hv_windows_fatal_report(NULL, "guest SError"' in guest
    ahci = _body((win / "hv_ahci.c").read_text(), "void hv_ahci_fatal_report(u64 now, u64 freq)")
    assert "hv_ahci_nvme_fatal_report(now, freq);" in ahci
    glue = (win / "hv_ahci_nvme.c").read_text()
    rec = _body(glue, "void hv_ahci_nvme_fatal_report(u64 now, u64 freq)")
    for key in ("state_name(state)", "nvme_lock_owner()", "IN FLIGHT", "hv_fatal_read(&act.seq",
                "closing", "nvme_fence_active()"):
        assert key in rec
    assert "printf" not in _body(glue, "static void act_begin(") + _body(glue, "static void act_end(")
    for fn in ("static bool dev_read(", "static bool dev_write(", "static bool dev_flush("):
        b = _body(glue, fn)
        assert b.index("nvme_lock_acquire(") < b.index("act_begin(") < b.index("act_end(") < \
            b.index("nvme_lock_release();")
    # The SError's own path: EXIT_GUEST returns from hv_start (whose return closes the disk);
    # anything else resets through flush_and_reboot, which closes NVMe first or parks.
    exc = (win / "hv_exc.c").read_text()
    serr = _body(exc, "void hv_exc_serr(struct exc_info *ctx)")
    assert serr.index("hv_windows_guest_serror_report(") < \
        serr.index("hv_exc_proxy(ctx, START_EXCEPTION_LOWER, EXC_SERROR, NULL);")
    proxy = _body(exc, "static void _hv_exc_proxy(")
    exit_case = proxy[proxy.index("case EXC_EXIT_GUEST:"):proxy.index("default:")]
    assert "hv_exit_guest();" in exit_case
    assert "hv_safe_reset(" in proxy[proxy.index("default:"):]
    utils = _body((REPO / "src/utils.c").read_text(), "void flush_and_reboot(void)")
    assert utils.index("hv_ahci_nvme_reset_close()") < utils.index("reboot();")
