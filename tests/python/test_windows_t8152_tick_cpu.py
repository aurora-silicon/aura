# SPDX-License-Identifier: MIT
"""SError A/B: the host tick on another CPU (run_guest --windows-tick-cpu), host only.

All four T8152 SErrors so far (D3 run 1 at EL2; i0 session 2, D5c and G5 S9 forwarded from the
guest) were on the boot CPU, the only CPU that runs hv_tick().  P_HV_WINDOWS_TICK_CPU moves the
tick to another CPU while that CPU is in the guest, so the next SErrors say whether they follow
the tick.  The A/B is only valid if nothing else changes, so the failure modes are:
  K1 the request is reset by hv_start() (as hv_pinned_cpu is), so it never takes effect;
  K2 the tick goes to a CPU that is not in the guest (before the guest OS starts it, after a
     PSCI CPU_OFF), so nobody pets the watchdog, polls the proxy link or flushes the vUART;
  K3 the request restricts which CPU may enter the proxy (hv_pin_cpu()'s spin in
     hv_exc_proxy()), so an SError on the boot CPU would park it forever;
  K4 it can be changed while the guest runs;
  K5 the default (no request, or the boot CPU) is not the old behaviour;
  K6 the log does not show which CPU ticked, or the SError report does not say;
  K7 run_guest calls it after the HV is initialised, or ignores a refusal;
plus the two host fixes that ride along: a ^C (reason 3) reported as a guest exception (d5g),
and --windows-serror continue accepted on a read-write NVMe disk or with the GPU admitted.
Nothing here touches hardware.
"""

import ast
from pathlib import Path
import re
import shutil
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"


def source(rel):
    return (REPO / rel).read_text()


def c_function(text, name):
    start = re.search(r"^[a-z].*\b" + re.escape(name) + r"\(.*\)\n\{", text, re.M).start()
    depth, i = 0, text.index("{", start)
    while True:
        c = text[i]
        depth += c == "{"
        depth -= c == "}"
        i += 1
        if depth == 0:
            return text[start:i]


def test_k1_hv_start_does_not_reset_the_request():
    hv = source("src/hv/windows/hv.c")
    session = c_function(hv, "hv_start_session")
    assert "hv_pinned_cpu = -1;" in session
    assert "hv_tick_cpu_req =" not in session
    assert "static int hv_tick_cpu_req = -1;" in hv


def test_k4_set_refuses_once_the_guest_has_started():
    fn = c_function(source("src/hv/windows/hv.c"), "hv_windows_set_tick_cpu")
    assert fn.index("if (hv_windows_entered)") < fn.index("hv_tick_cpu_req = (int)cpu;")
    assert "smp_is_alive(cpu)" in fn and "cpu >= MAX_CPUS" in fn


def test_k3_proxy_entry_is_not_pinned():
    exc = source("src/hv/windows/hv_exc.c")
    proxy = c_function(exc, "hv_exc_proxy")
    assert "hv_tick" not in proxy
    assert "hv_tick_cpu_req" not in exc


def test_k5_k2_only_the_fiq_owner_changes():
    exc = source("src/hv/windows/hv_exc.c")
    fiq = c_function(exc, "hv_exc_fiq")
    assert "interruptible_cpu = hv_tick_owner();" in fiq
    assert "interruptible_cpu = boot_cpu_idx;" not in fiq
    # The fast path and the tick both key on the same owner.
    assert "smp_id() != interruptible_cpu" in fiq and "smp_id() == interruptible_cpu" in fiq
    assert exc.count("hv_tick_owner()") == 2  # the FIQ owner and the SError report line


@pytest.mark.skipif(shutil.which("cc") is None, reason="no host C compiler")
def test_k2_k5_owner_falls_back_to_the_boot_cpu(tmp_path):
    hv = source("src/hv/windows/hv.c")
    owner = c_function(hv, "hv_tick_owner")
    prog = tmp_path / "owner.c"
    prog.write_text(r'''
#include <stdbool.h>
#include <stdio.h>
#define MAX_CPUS 12
static int boot_cpu_idx = 6;
static bool hv_started_cpus[MAX_CPUS];
static int hv_tick_cpu_req = -1;
''' + owner + r'''
int main(void)
{
    int bad = 0;
#define CHECK(want) do { int got = hv_tick_owner(); if (got != (want)) { \
        printf("req %d started7 %d: owner %d, want %d\n", hv_tick_cpu_req, \
               hv_started_cpus[7], got, want); bad = 1; } } while (0)
    CHECK(6);                       /* default */
    hv_tick_cpu_req = 6; CHECK(6);  /* the boot CPU requested */
    hv_tick_cpu_req = 7; CHECK(6);  /* not yet started by the guest OS */
    hv_started_cpus[7] = true; CHECK(7);
    hv_started_cpus[7] = false; CHECK(6); /* PSCI CPU_OFF */
    hv_tick_cpu_req = 12; hv_started_cpus[7] = true; CHECK(6);
    hv_tick_cpu_req = -1; CHECK(6);
    return bad;
}
''')
    exe = tmp_path / "owner"
    subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-o", str(exe), str(prog)], check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True)
    assert out.returncode == 0, out.stdout


def test_k6_the_log_names_the_tick_cpu():
    hv = source("src/hv/windows/hv.c")
    tick = c_function(hv, "hv_tick")
    assert "HV: host tick now on CPU %d" in tick
    assert tick.index("hv_wdt_pet();") < tick.index("host tick now on CPU")
    session = c_function(hv, "hv_start_session")
    assert session.index("hv_tick_cpu_last = -1;") < session.index("HV: Entering guest at")
    assert "HV: host tick: the boot CPU %d" in session
    serr = c_function(source("src/hv/windows/hv_exc.c"), "hv_exc_serr")
    assert serr.index("host tick on cpu %d") < serr.index("hv_windows_guest_serror_report(")
    assert serr.index("hv_exc_entry();") < serr.index("host tick on cpu %d")


def test_k7_run_guest_sets_it_before_init_and_stops_on_a_refusal():
    rg = source("proxyclient/tools/run_guest.py")
    body = rg[rg.index("def run_windows_t8152(caps):"):]
    call = body.index("p.hv_windows_tick_cpu(args.windows_tick_cpu)")
    assert call < body.index("hv = HV(iface, p, u, windows=True)") < body.index("hv.init()")
    tail = body[call:body.index("hv = HV(iface, p, u, windows=True)")]
    assert "if ret != 0:" in tail and "sys.exit(" in tail
    assert "P_HV_WINDOWS_TICK_CPU = 0xc8a" in source("proxyclient/aura/proxy.py")
    assert "P_HV_WINDOWS_TICK_CPU = 0xc8a," in source("src/proxy.h")
    assert "hv_windows_set_tick_cpu(request->args[0])" in source("src/proxy.c")


def _summary():
    rg = source("proxyclient/tools/run_guest.py")
    tree = ast.parse(rg)
    fn = next(n for n in tree.body
              if isinstance(n, ast.FunctionDef) and n.name == "windows_t8152_fatal_summary")
    ns = {}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "run_guest.py", "exec"), ns)
    return ns["windows_t8152_fatal_summary"]


class FakeHV:
    def __init__(self, event, lines=()):
        self.windows_abort_reason = event
        self.windows_fatal_lines = list(lines)

    def windows_fatal_summary(self):
        return self.windows_fatal_lines


def test_reason_3_is_not_reported_as_a_guest_exception(capsys):
    summary = _summary()
    summary(FakeHV({"reason": 3, "code": 0, "info": "0x0"}))
    out = capsys.readouterr().out
    assert "ended from the host" in out and "guest exception: " not in out
    assert "printed no fatal report" not in out
    summary(FakeHV({"reason": 2, "code": 3, "info": "0x100093ab9e0"}))
    out = capsys.readouterr().out
    assert "aborted on a forwarded exception" in out and "printed no fatal report" in out


def test_serror_continue_is_refused_on_persistent_disks_and_with_the_gpu():
    rg = source("proxyclient/tools/run_guest.py")
    body = rg[rg.index("def run_windows_t8152(caps):"):]
    guard = body[:body.index("if not args.windows_guest_ram")]
    assert "args.windows_serror == 'continue'" in guard
    assert "args.windows_ahci_nvme_access == 'rw'" in guard
    assert "args.windows_gpu not in ('off', 'disabled')" in guard
