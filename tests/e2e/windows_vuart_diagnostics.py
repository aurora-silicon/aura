#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Replay the production smoke capture helper at its transport boundary."""

import ast
import json
from pathlib import Path
import struct
import sys
import time
from types import SimpleNamespace

source, output = map(Path, sys.argv[1:])
output.mkdir()
text = source.read_text()
node = next(
    n
    for n in ast.parse(text).body
    if isinstance(n, ast.ClassDef) and n.name == "FailureDiagnostics"
)
body = "\n".join(text.splitlines()[node.lineno - 1 : node.end_lineno])
(output / "production-helper.py").write_text(body + "\n")
namespace = {"time": time, "struct": struct, "json": json, "SIZE": 256}
exec(compile(body, str(source), "exec"), namespace)
helper = namespace["FailureDiagnostics"]


def setup(name, failed_read=False):
    out = output / name
    out.mkdir()
    clock, trace, report = [0.0], [], {}
    result = bytes(range(256))
    ctx = SimpleNamespace(
        cpu_id=6,
        elr=0x300123,
        far=0x456,
        esr=0x92000046,
        spsr=0x3C5,
        regs=list(range(31)),
        sp=[0, 1, 2],
    )

    def readmem(address, size):
        trace.append("read-context" if address == 0x1000 else "read-result")
        if failed_read:
            raise OSError("injected read failure")
        assert (address, size) in [(0x1000, 16), (0x2000, 256)]
        return bytes(16) if address == 0x1000 else result

    def abort(reason, code, info):
        trace.append("abort")
        if session.hv.windows_abort_reason is None:
            session.hv.windows_abort_reason = (reason, code, info)

    session = SimpleNamespace(
        iface=SimpleNamespace(
            readmem=readmem,
            proxyreq=lambda request, *a, **kw: struct.pack("<Qqq", 0xC1, -2, 0),
        ),
        proxy=SimpleNamespace(P_HV_START=0xC1),
        hv=SimpleNamespace(
            _abort_windows_guest=abort,
            windows_abort_reason=None,
            interrupt=lambda: trace.append("interrupt"),
        ),
    )
    parser = SimpleNamespace(sizeof=lambda: 16, parse=lambda raw: ctx)
    diag = helper(
        session, out, report, 0x2000, 0x300000, parser, 250, now=lambda: clock[0]
    )
    diag.install()
    return diag, session, clock, trace, report, out


diag, session, clock, trace, report, out = setup("deadline")
assert diag.begin() == 230
for now in [60, 220, 229]:
    clock[0] = now
    assert diag.alarm() == 230 - now and trace == []
clock[0] = 230
assert diag.alarm() == 20 and trace == ["interrupt"]
clock[0] = 240
assert diag.alarm() == 10 and trace == ["interrupt"]
session.hv._abort_windows_guest(3, 3, 0x1000)
assert trace == ["interrupt", "read-context", "read-result", "abort"]
assert report["diagnostic_events"][0]["kind"] == "deadline-interrupt"
assert (out / "exception-00.bin").read_bytes() == bytes(16)
assert (out / "result-at-exception-00.bin").read_bytes() == bytes(range(256))
session.iface.proxyreq(struct.pack("<Q", 0xC1))
assert diag.returned and report["hv_start_replies"] == [
    {"opcode": 0xC1, "status": -2, "value": 0}
]
clock[0] = 250
try:
    diag.alarm()
except TimeoutError:
    pass
else:
    raise AssertionError("hard deadline did not fire")

diag, session, clock, trace, report, out = setup("ordinary-fault")
diag.begin()
clock[0] = 5
session.hv._abort_windows_guest(2, 0, 0x1000)
session.hv._abort_windows_guest(3, 3, 0x1000)
assert session.hv.windows_abort_reason == (2, 0, 0x1000)
assert all(event["kind"] == "guest-exception" for event in report["diagnostic_events"])
clock[0] = 230
assert diag.alarm() == 20 and "interrupt" not in trace

diag, session, clock, trace, report, out = setup("capture-failure", True)
diag.begin()
session.hv._abort_windows_guest(2, 0, 0x1000)
assert trace[-1] == "abort" and "capture_error" in report["diagnostic_events"][0]
assert not diag.returned

diag, session, clock, trace, report, out = setup("late-entry")
clock[0] = 30
assert (
    diag.begin() == 220
)  # Hard overall deadline comes before the soft guest deadline.
clock[0] = 250
try:
    diag.alarm()
except TimeoutError:
    pass
else:
    raise AssertionError("late preparation extended the hard deadline")
assert "interrupt" not in trace

diag, session, clock, trace, report, out = setup("hard-during-capture")
diag.begin()


def blocked_read(address, size):
    clock[0] = 250
    diag.alarm()


session.iface.readmem = blocked_read
try:
    session.hv._abort_windows_guest(2, 0, 0x1000)
except TimeoutError:
    pass
else:
    raise AssertionError("capture swallowed the hard deadline")
assert diag.hard_expired and "abort" not in trace

print(
    json.dumps(
        {
            "passed": True,
            "cases": [
                "deadline-capture-before-abort",
                "ordinary-first-fault",
                "capture-failure-still-aborts",
                "overall-hard-limit",
                "hard-deadline-interrupts-capture",
            ],
        }
    )
)
