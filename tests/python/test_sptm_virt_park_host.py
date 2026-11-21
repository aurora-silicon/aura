# SPDX-License-Identifier: MIT
"""src/sptm_virt_park.c, the virtualized SPTM's world stop, with its cores as host threads
(tests/park_host/park_host.c), and the mutants that prove the harness notices each way it can fail.

What a park must never do, each one a mutant below that must fail the harness:
  - let a waiting core go before the owner releases it (two cores in aura, one in the proxy);
  - let a core into aura between the owner's election and its wait (the entry's count published
    after its check, or the owner reading the counts before it is elected);
  - not wait for a core inside aura (the old blind delay), or wait for it without bound, or not
    name it;
  - leave the released cores waiting for an event that never comes (the guest never resumes);
  - elect two owners at once, or let the loser of an election run its park inside the winner's;
  - count the loser of an election, or a waiting core, as inside aura (every park then waits its
    bound and names a straggler that isn't one);
  - make a nested entry (a fault inside one of aura's handlers) wait for the park: it and the
    owner then wait for each other;
  - end the park when the host closes the port (uartproxy_run fails once the CDC DTR drops), or
    lose the proxy there;
  - return into the trap with the return state the proxy session rewrote;
  - miss the host's request split across two looks at the pipe, park on other bytes, park twice
    for one request, look at the pipe more often than every sptm_virt_park_poll_ms, or park in a
    GL trap within the grace while EL2 traps come;
  - let a core out of reset (XNU started it, or it woke from idle) into the guest during a park;
  - leave a core's count raised after it leaves aura.

Needs a host compiler with pthreads; runs in about 2 s, the mutants in parallel.
"""

import concurrent.futures
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src/sptm_virt_park.c"
HARNESS = REPO / "tests/park_host/park_host.c"
CFLAGS = ["-O1", "-g", "-pthread", "-w", "-DPARK_HOST", f"-I{REPO}/tests/park_host"]

INNER_WAIT = ("        while ((p = __atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_ACQUIRE)) >= 0 "
              "&& p != me)\n            sysop(\"wfe\");\n")
ENTRY = ("    if (__atomic_fetch_add(&park_busy[me], 1, __ATOMIC_SEQ_CST))\n        return me;\n"
         "    park_wait(me, 1);\n")
ELECTION = ("        if (__atomic_compare_exchange_n(&sptm_virt_parking_cpu, &none, me, false, "
            "__ATOMIC_SEQ_CST,\n                                        __ATOMIC_SEQ_CST) ||\n"
            "            none == me)\n            break;\n")
LOSER_WAIT = "        park_wait(me, __atomic_load_n(&park_busy[me], __ATOMIC_RELAXED));\n"
OWNER_WAIT = ("    while ((inside = park_inside(me)) && mrs(CNTPCT_EL0) - t0 < lim)\n        ;\n")
CLAIM = ("    if (!__atomic_exchange_n(&park_req_t, 0, __ATOMIC_SEQ_CST))\n        return;\n")

# name: [(text in src/sptm_virt_park.c, its replacement), ...], applied together
MUTATIONS = {
    "a waiter leaves before the release": [(INNER_WAIT, "")],
    "the entry counts itself after its check": [(ENTRY, (
        "    park_wait(me, 0);\n    if (__atomic_fetch_add(&park_busy[me], 1, __ATOMIC_SEQ_CST))\n"
        "        return me;\n"))],
    "the owner waits for the others before it is elected": [(
        "    int me = smp_id();\n\n    while (1) {\n        int none = -1;",
        "    int me = smp_id();\n    while (park_inside(me))\n        ;\n\n"
        "    while (1) {\n        int none = -1;"), (OWNER_WAIT, "    inside = 0;\n")],
    "the owner doesn't wait (the blind delay)": [(OWNER_WAIT, "    inside = 0;\n")],
    "the owner waits without bound": [(OWNER_WAIT, (
        "    while ((inside = park_inside(me)))\n        ;\n"))],
    "a straggler isn't named": [(
        "    if (inside)\n        printf(\"SPTM-virt[park]: cpu%d: cores", "    if (0)\n"
        "        printf(\"SPTM-virt[park]: cpu%d: cores")],
    "no event at the release": [("    sysop(\"dsb sy\");\n    sysop(\"sev\");\n",
                                 "    sysop(\"dsb sy\");\n")],
    "the election isn't atomic": [(ELECTION, (
        "        int cur = __atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_SEQ_CST);\n"
        "        if (cur < 0 || cur == me) {\n"
        "            __atomic_store_n(&sptm_virt_parking_cpu, me, __ATOMIC_SEQ_CST);\n"
        "            break;\n        }\n"))],
    "the loser runs its park inside the winner's": [(
        "__ATOMIC_SEQ_CST) ||\n            none == me)", "__ATOMIC_SEQ_CST) ||\n            1)")],
    "the loser is counted as inside aura": [(LOSER_WAIT, (
        "        while (__atomic_load_n(&sptm_virt_parking_cpu, __ATOMIC_ACQUIRE) >= 0)\n"
        "            sysop(\"wfe\");\n"))],
    "a waiting core is counted as inside aura": [(
        "        __atomic_store_n(&park_busy[me], 0, __ATOMIC_SEQ_CST);\n        while ((p", (
            "        while ((p"))],
    "a nested entry waits for the park": [(ENTRY, (
        "    __atomic_fetch_add(&park_busy[me], 1, __ATOMIC_SEQ_CST);\n"
        "    park_wait(me, __atomic_load_n(&park_busy[me], __ATOMIC_RELAXED));\n"))],
    "the host closing the port resumes the guest": [(
        "        if (ret >= 0)\n            return ret;\n", "        return ret;\n")],
    "the return state isn't restored": [("    park_restore(&r);\n", "")],
    "a request split across looks is missed": [(
        "    iodev_handle_events(id);\n    for (int left", "    park_sr = 0;\n"
        "    iodev_handle_events(id);\n    for (int left")],
    "other bytes park": [("            if (park_sr == PARK_MAGIC) {",
                          "            if (park_sr == PARK_MAGIC || b[i] == 'A') {")],
    "one request parks twice": [(CLAIM, (
        "    if (!__atomic_load_n(&park_req_t, __ATOMIC_SEQ_CST))\n        return;\n"
        "    __atomic_store_n(&park_req_t, 0, __ATOMIC_SEQ_CST);\n"))],
    "the pipe isn't rate-limited": [(
        "        if (now < park_poll_next || __atomic_exchange_n", "        if (__atomic_exchange_n"),
        ("        if (now < park_poll_next) {\n            __atomic_store_n",
         "        if (0) {\n            __atomic_store_n")],
    "a GL trap parks within the grace": [(
        "    return !gl || (now > req && now - req >= park_ms(sptm_virt_park_gl_grace_ms));",
        "    return true;")],
    "a core out of reset doesn't wait for the park": [(
        "    park_wait(me, 1);\n    __atomic_store_n(&park_busy[me], 0, __ATOMIC_RELEASE);",
        "    __atomic_store_n(&park_busy[me], 0, __ATOMIC_RELEASE);")],
    "a core's count stays raised": [(
        "        __atomic_fetch_sub(&park_busy[me], 1, __ATOMIC_RELEASE);", "        ;")],
}


def _build_run(source, out, timeout=60):
    subprocess.check_call(host_cc() + CFLAGS + ["-o", str(out), str(HARNESS), str(source)])
    try:
        return subprocess.run([str(out)], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(e.cmd, 124, e.stdout or "", "timed out")


def test_park_on_host_threads():
    r = _build_run(SRC, Path(tempfile.mkdtemp()) / "park")
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]


def _mutant(args):
    name, edits, d = args
    m = SRC.read_text()
    for a, b in edits:
        assert m.count(a) == 1, f"{name}: the checked code changed"
        m = m.replace(a, b, 1)
    src = d / f"m{abs(hash(name))}.c"
    src.write_text(m)
    return name, _build_run(src, src.with_suffix(""), timeout=30).returncode


def test_every_guarantee_is_noticed_when_broken():
    d = Path(tempfile.mkdtemp())
    workers = max(2, min(8, (os.cpu_count() or 2) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        results = list(pool.map(_mutant, [(n, e, d) for n, e in MUTATIONS.items()]))
    missed = [n for n, rc in results if rc == 0]
    assert not missed, f"not noticed: {missed}"


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL {name}: {type(e).__name__}: {str(e)[:2000]}")
            else:
                print(f"ok   {name}")
    sys.exit(1 if failed else 0)
