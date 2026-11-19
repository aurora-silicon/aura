# SPDX-License-Identifier: MIT
"""G5b C-2: the once-a-minute "HV: GPU SUM" line (G5B-BUILD 3.1).

Failure modes written down before the code:
  S-1 the line's fields drift from what g5watch parses (names, order, one line per call);
  S-2 a counter is read under g2_lock or amap_lock, or through MMIO, from the tick (lock order
      bhl -> g2_lock against the forwarding CPU's g2_lock -> printf -> console);
  S-3 the line is printed while no G1/G2 session forwards (G0, GPU off, after teardown);
  S-4 the pacing is per CPU, so a tick handover (tick 6 or 7) doubles or drops minutes, or a long
      stall (the Mac asleep) replays every missed minute as a burst;
  S-5 another CPU than the tick owner prints it;
  S-6 a counter goes backwards between two lines (printed as-is; the watch must flag it).
"""
import pathlib
import re
import shutil
import subprocess

import pytest

from test_windows_t8152_gpu import glue_exe  # noqa: F401 (the probe host fixture)

REPO = pathlib.Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"

SUM_RE = re.compile(
    r"^HV: GPU SUM t=(?P<t>[0-9]+) st=0x(?P<st>[0-9a-f]+) g2=(?P<g2>[01]) fwd (?P<fwd>[0-9]+) "
    r"denied (?P<denied>[0-9]+) runref (?P<runref>[0-9]+) faults (?P<faults>[0-9]+) "
    r"db (?P<db>[0-9]+) aud (?P<aud>[0-9]+) fresh_max (?P<fresh_max>[0-9]+) "
    r"capped (?P<capped>[01]) tbl (?P<tbl>[0-9]+)/1024 guards (?P<guards>[0-9]+) "
    r"init 0x(?P<init>[0-9a-f]+) msgs tx/rx (?P<gtx>[0-9]+)/(?P<grx>[0-9]+) "
    r"(?P<mtx>[0-9]+)/(?P<mrx>[0-9]+) drop (?P<drop>[0-9]+)$")


def parse_sum(line):
    m = SUM_RE.match(line)
    return {k: int(v, 16) if k in ("st", "init") else int(v) for k, v in m.groupdict().items()} \
        if m else None


def backwards(prev, cur):
    """S-6: counters that only ever grow; a drop between two lines is a carrier bug (or a reload)."""
    return [k for k in ("fwd", "denied", "runref", "faults", "db", "aud", "fresh_max", "guards",
                        "gtx", "grx", "mtx", "mrx", "drop", "t") if cur[k] < prev[k]]


def test_sum_format_from_the_g2_session(glue_exe, tmp_path):
    """S-1, S-3: the probe host's G2 guard session prints one SUM line with its own counters; it
    is silent before PROBE and after STOP (asserted inside the host, "G2 GUARD OK")."""
    import test_windows_t8152_gpu as g
    out = _run_probe(g, glue_exe, tmp_path)
    block = out.split("G2 SUM BEGIN\n", 1)[1].split("G2 SUM END", 1)[0].splitlines()
    assert len(block) == 1, block
    s = parse_sum(block[0])
    assert s, block[0]
    assert s["t"] == 120 and s["g2"] == 1 and s["drop"] == 7
    assert s["db"] == 1 and s["faults"] == 0 and s["capped"] == 0 and s["init"] == 0x3
    assert s["guards"] == 2 + 3 + 8 + 3 + 3          # the guard counts the fatal report shows
    assert s["aud"] >= 1 and s["fresh_max"] >= 1 and 0 < s["tbl"] <= 1024
    assert s["mtx"] == 2 and s["gtx"] >= 4 and s["grx"] >= 1
    assert "G2 GUARD OK" in out
    assert len(block[0]) < 260


def _run_probe(g, exes, tmp_path):
    (tmp_path / "adt.bin").write_bytes(g.machine().encode())
    for r in range(2):
        (tmp_path / f"text{r}.bin").write_bytes(g.make_text(r))
    hole = g.SGX + 0xe8_0000
    args = [str(exes["probe"]), str(tmp_path / "adt.bin"), f"{g.TEXT[0][0]:#x}",
            str(tmp_path / "text0.bin"), f"{g.TEXT[1][0]:#x}", str(tmp_path / "text1.bin"),
            f"{g.SGX:#x}", f"{g.SGX_SIZE:#x}", f"{g.MBOX:#x}", f"{g.POOL[0]:#x}",
            f"{g.POOL[1]:#x}", f"{g.RAM_BASE:#x}", f"{g.RAM_TOP:#x}", f"{g.WRAP[0]:#x}",
            f"{g.WRAP[1]:#x}", f"{g.VBAR[0]:#x}", f"{hole:#x}"]
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def test_sum_reads_no_lock_and_no_mmio():
    """S-2."""
    src = (WIN / "hv_gpu.c").read_text()
    body = src.split("bool hv_gpu_sum(u64 guest_s, u64 console_dropped)\n{", 1)[1].split("\n}\n", 1)[0]
    for banned in ("g2_acquire", "amap_acquire", "spin_lock", "hw_rw", "read32", "write32",
                   "malloc", "uat_check", "marker_update", "hv_pa_rw"):
        assert banned not in body, banned
    assert body.count("printf(") == 1          # one line, one console write
    assert "if (!probing || torn_down)\n        return false;" in body


PACE_HOST = r"""
#include <assert.h>
#include <stdio.h>
#include "hv_status.h"
static unsigned long calls[64]; static int ncalls;
bool hv_gpu_sum(u64 guest_s, u64 dropped) { (void)dropped; printf("SUM %lu\n", guest_s); return true; }
static int tick(u64 now, u64 f)
{
    u64 s;
    if (!hv_status_due(now, f, &s))
        return 0;
    calls[ncalls++] = s;
    return 1;
}
int main(void)
{
    const u64 f = 24000000;
    u64 t0 = 5000 * f;
    assert(!tick(t0, f));                 /* before entry: nothing */
    hv_status_guest_entered(t0, f);
    for (u64 s = 0; s < 59; s++)
        assert(!tick(t0 + s * f, f));
    assert(tick(t0 + 60 * f, f) && calls[0] == 60);
    assert(!tick(t0 + 60 * f + 1, f));    /* a second CPU's tick (handover): no repeat */
    assert(!tick(t0 + 119 * f, f));
    assert(tick(t0 + 120 * f + f / 2, f) && calls[1] == 120);
    /* A 705 s stall: one block when the tick comes back, then a minute from there. */
    assert(tick(t0 + 825 * f, f) && calls[2] == 825);
    assert(!tick(t0 + 884 * f, f));
    assert(tick(t0 + 885 * f, f) && calls[3] == 885);
    assert(ncalls == 4);
    /* A new guest entry restarts the minutes. */
    hv_status_guest_entered(t0 + 1000 * f, f);
    assert(!tick(t0 + 1059 * f, f));
    assert(tick(t0 + 1060 * f, f) && calls[4] == 60);
    /* The block: live CPUs only, one printf per line, GPU SUM last with the console drops. */
    struct hv_status_snapshot s = {.guest_s = 1860, .soc_s = 2891, .tick_cpu = 7, .boot_cpu = 6,
                                   .ahci_commands = 66734, .input_ok = 12, .input_bad = 0,
                                   .con_dropped = 4096, .con_stalls = 2,
                                   .con_longest_stall_us = 705000000, .con_stalled_now_us = 0,
                                   .con_longest_call_us = 37, .vuart_overruns = 3};
    for (int c = 0; c < HV_STATUS_CPUS; c++)
        s.exits[c] = c < 10 ? (unsigned long)(100 * c) : ~0UL;
    hv_status_print(&s);
    puts("STATUS PACE OK");
    return 0;
}
"""


def _status_host(tmp_path):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    (tmp_path / "types.h").write_text(
        "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n"
        "typedef unsigned long u64; typedef uint32_t u32;\n")
    (tmp_path / "utils.h").write_text("#pragma once\n#include <stdio.h>\n")
    (tmp_path / "hv_gpu.h").write_text(
        "#pragma once\n#include \"types.h\"\nbool hv_gpu_sum(u64 guest_s, u64 console_dropped);\n")
    (tmp_path / "pace.c").write_text(PACE_HOST)
    exe = tmp_path / "pace"
    subprocess.run([cc, "-std=gnu11", "-Wall", "-Werror", "-fsanitize=address,undefined",
                    "-iquote", str(tmp_path), "-I", str(WIN), str(WIN / "hv_status.c"),
                    str(tmp_path / "pace.c"), "-o", str(exe)], check=True)
    return subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout


def test_sum_pacing(tmp_path):
    """S-4: guest-run minutes from entry, one cadence across tick CPUs, no burst after a stall."""
    assert "STATUS PACE OK" in _status_host(tmp_path)


def test_status_block_format(tmp_path):
    """C-3: the 60 s block's lines, and GPU SUM last with the console's dropped bytes."""
    out = _status_host(tmp_path).splitlines()
    i = out.index("HV: STATUS t=1860 soc=2891 tick cpu 7 boot cpu 6")
    assert out[i + 1] == "HV: STATUS exits/min: c0=0 c1=100 c2=200 c3=300 c4=400 c5=500 c6=600 " \
                         "c7=700 c8=800 c9=900"
    assert out[i + 2] == ("HV: STATUS ahci 66734 input 12/0; console dropped 4096 stalls 2 longest "
                          "705000000 us now 0 us write max 37 us; vuart overruns 3")
    assert out[i + 3] == "SUM 1860"


def test_sum_only_on_the_tick_owner():
    """S-5: one call site, inside hv_tick(), which only the tick owner runs."""
    hv = (WIN / "hv.c").read_text()
    tick = hv.split("void hv_tick(struct exc_info *ctx)\n{", 1)[1].split("\n}\n", 1)[0]
    assert "hv_status_tick();" in tick
    calls = [p.name for p in WIN.glob("*.c") for _ in re.finditer(r"\bhv_status_due\(", p.read_text())
             if p.name != "hv_status.c"]
    assert calls == ["hv.c"]
    body = hv.split("static void hv_status_tick(void)\n{", 1)[1].split("\n}\n", 1)[0]
    assert "hv_status_due(now, freq, &guest_s)" in body and ".tick_cpu = smp_id()," in body
    assert hv.count("hv_status_tick();") == 1
    exc = (WIN / "hv_exc.c").read_text()
    assert "interruptible_cpu = hv_tick_owner();" in exc
    assert "hv_status_guest_entered(mrs(CNTPCT_EL0), mrs(CNTFRQ_EL0));" in hv
    assert hv.index("hv_status_guest_entered(") < hv.index("hv_enter_guest(regs[0]")


def test_sum_backwards_counter_is_flagged():
    """S-6: the parser the watch shares flags a counter that went down."""
    a = parse_sum("HV: GPU SUM t=60 st=0xf5 g2=1 fwd 100 denied 0 runref 0 faults 0 db 10 aud 11 "
                  "fresh_max 0 capped 0 tbl 7/1024 guards 0 init 0x3 msgs tx/rx 12/3 2/1 drop 0")
    b = parse_sum("HV: GPU SUM t=120 st=0xf5 g2=1 fwd 90 denied 0 runref 0 faults 0 db 12 aud 13 "
                  "fresh_max 0 capped 0 tbl 7/1024 guards 0 init 0x3 msgs tx/rx 14/3 2/1 drop 0")
    assert a and b and backwards(a, b) == ["fwd"] and backwards(a, a) == []
