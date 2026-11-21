# SPDX-License-Identifier: MIT
"""src/sptm_virt_trace.c, the record-only block and IRQ traces (AURA_TRACE_BLKS, AURA_IRQ_TRACE),
with its cores as host threads and a fake device (tests/trace_host), read back and decoded by
proxyclient/aura/hvtrace.py, and the mutants that prove the harness notices each way it can fail.

Written before the code. What the trace must never do, each one checked below (a mutant of the
source must fail the harness for the ones marked M):

The hooks
  F1 (M) perform a guest access twice, not at all, at another width, change the value a write
     carries or return another value than the device's to a read;
  F2 (M) read the AIC event register twice for one guest read (that acknowledges an IRQ the guest
     never sees), record a read that returned 0 (no IRQ: counted only), miss a nonzero one, or not
     pass another register of the event page through, once;
  F3 (M) get a record's PA, width, direction, core, block, guest PC, GL flag (SPTM's GL1 trap) or
     value wrong, or lose a 128-bit access's second word.
The ring
  F4 (M) let two cores take one slot (a record overwritten) when they record at once;
  F5 (M) show a slot as complete while it is being written (its time is written last: 0 = not yet);
  F6 (M) overwrite an undrained record when it is full instead of counting the access as lost, or
     not count it (in all and for its block), or record half of a 128-bit access at the edge;
  F7 (M) drain records the host hasn't read: past head, past the first one still being written,
     or move the tail before the slots are zeroed (a stale record read again as a new one);
  F8 accept a drain of anything but its own two rings.
The block list (the host's, before entry; checked again by aura before it hooks anything)
  F9 (M) accept a range that isn't whole 16 KiB pages, one past 48 bits, two that overlap, more
     than 16 ranges, 8 blocks or 32 pages, a range in no block or a block with no range.
The decoder
  D1 decode a slot still being written (t = 0) or one past the reader's cut;
  D2 duplicate or drop records across reads and drains (the capture directory's index);
  D3 take a multi-range block's offsets from anything but its lowest page;
  D4 count the distinct (offset, width, R/W) set wrong, or a 128-bit access twice;
  D5 decode an AIC event wrong: type, die, number, the flat number (die * IRQs per die + number);
  D6 leave the merged timeline out of time order, or leave out a block access or an IRQ;
  D7 not report what was lost: the ring's lost count, and a gap between two reads.

What only the hardware shows (the run's console and summary tell each one):
  H1 a page on the host's debug link (its USB controller, DART, PHY, the bus of its port
     controller): aura refuses the whole list from its ADT ("SPTM-virt[trace]: refused"), and
     run_guest_sptm refuses it before anything is sent; no page is hooked;
  H2 a page another aura hook owns (the reset guard, the vuart, the GPU record, the sleep path):
     refused the same way, the page named;
  H3 the trap rate slows the guest: the "SPTM-virt[s2 traps]" lines every 10 s give it;
  H4 a register that misbehaves under the HV's emulation (an LDP/STP of two W registers is one
     64-bit access; a 32-byte one isn't supported and fails the access): an SError or an XNU
     panic at that PC;
  H5 the ring fills between two reads: one "FULL" line on the console, the access count goes on,
     and the reader's summary gives what was lost per block;
  H6 a park long enough to trip a guest timeout (ANS's 20 s, a watchdog): a panic after the
     resume; the reader caps what one park reads (--max-mib);
  H7 a register on the AIC event page with read side effects: every access there is still
     performed once, and shows up as an "other" record;
  H8 no heap for a ring: "no memory", and that trace hooks nothing.
"""

import concurrent.futures
import os
import struct
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
from aura import hvtrace  # noqa: E402

SRC = REPO / "src/sptm_virt_trace.c"
HARNESS = REPO / "tests/trace_host/trace_host.c"
CFLAGS = ["-O1", "-g", "-pthread", "-w", "-DHVTRACE_HOST", f"-I{REPO}/src",
          f"-I{REPO}/tests/trace_host"]

PERFORM = "    sptm_virt_mmio_rw(addr, val, write, width);\n    return true;\n}\n"
CLAIM = """    do {
        if (head + n - __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE) > h->cap)
            return false;
    } while (!__atomic_compare_exchange_n(&h->head, &head, head + n, true, __ATOMIC_ACQ_REL,
                                          __ATOMIC_RELAXED));
"""
T_LAST = """        r->meta = (meta + 8 * k) | (u64)k << 62;
        __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);
"""
ZERO_THEN_TAIL = """    for (u64 j = tail; j < upto; j++)
        memset(&trace_recs(h)[j % h->cap], 0, sizeof(struct hvtrace_rec));
    __atomic_store_n(&h->tail, upto, __ATOMIC_RELEASE);
"""
EVENT = "    bool event = !write && addr == h->irq_event;\n"

# name: [(text in src/sptm_virt_trace.c, its replacement), ...], applied together
MUTATIONS = {
    "F1 a read performed twice": [(PERFORM, "    sptm_virt_mmio_rw(addr, val, write, width);\n"
                                  "    if (!write)\n        sptm_virt_mmio_rw(addr, val, write, width);\n"
                                  "    return true;\n}\n")],
    "F1 a write performed twice": [(PERFORM, "    sptm_virt_mmio_rw(addr, val, write, width);\n"
                                   "    if (write)\n        sptm_virt_mmio_rw(addr, val, write, width);\n"
                                   "    return true;\n}\n")],
    "F1 performed at another width": [(PERFORM, "    sptm_virt_mmio_rw(addr, val, write, width < 2 ? 2 : width);\n"
                                      "    return true;\n}\n")],
    "F1 a write's value changed": [(PERFORM, "    if (write)\n        val[0] &= 0xffff;\n"
                                   "    sptm_virt_mmio_rw(addr, val, write, width);\n    return true;\n}\n")],
    "F1 a pair split in two accesses": [("        trace_pair(addr, val, write);\n",
                                         "        sptm_virt_mmio_rw(addr, val, write, 3);\n"
                                         "        sptm_virt_mmio_rw(addr + 8, val + 1, write, 3);\n")],
    "F2 the event register read twice": [(EVENT, EVENT + "    if (event)\n"
                                          "        trace_perform(ctx, addr, val, write, width);\n")],
    "F2 a zero read recorded": [("    if (event && !(u32)val[0]) {\n", "    if (0) {\n")],
    "F2 another register taken for the event register": [(EVENT, "    bool event = !write;\n")],
    "F2 the page's other accesses not recorded": [(
        "    sptm_virt_trace_note(h, addr, val, write, width, event ? HVTRACE_IRQ_EVENT : HVTRACE_IRQ_OTHER,\n",
        "    if (event)\n    sptm_virt_trace_note(h, addr, val, write, width, event ? HVTRACE_IRQ_EVENT : HVTRACE_IRQ_OTHER,\n")],
    "F3 the direction lost": [("(u64) !!write << 51", "(u64)0 << 51")],
    "F3 the core lost": [("(u64)(cpu & 31) << 52", "(u64)(cpu & 7) << 52")],
    "F3 the PC lost": [("        r->pc = pc;\n", "        r->pc = 0;\n")],
    "F3 SPTM's accesses pass for XNU's": [("(u64) !!gl << 61", "(u64)0 << 61")],
    "F3 a pair's second word lost": [("        r->val = val[k];\n", "        r->val = val[0];\n")],
    "F3 a range's index taken for its block": [("            b = h->rng[i].blk;\n",
                                                "            b = i;\n")],
    "F3 the value read before the access": [(
        "    if (!trace_perform(ctx, addr, val, write, width))\n        return false;\n"
        "    u64 t = trace_now();\n    for (u32 i = 0;",
        "    u64 saved = val[0];\n    if (!trace_perform(ctx, addr, val, write, width))\n"
        "        return false;\n    u64 t = trace_now();\n    u64 got = val[0];\n"
        "    val[0] = saved;\n    sptm_virt_trace_note(h, addr, val, write, width, 0, ctx->elr, 0, 0, t);\n"
        "    h->head--;\n    val[0] = got;\n    for (u32 i = 0;")],
    "F4 slots claimed non-atomically": [(CLAIM, """    if (head + n - __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE) > h->cap)
        return false;
    for (volatile int spin = 0; spin < 50; spin++)
        ;
    __atomic_store_n(&h->head, head + n, __ATOMIC_RELEASE);
""")],
    "F5 a record's time written first": [(T_LAST, "        r->meta = (meta + 8 * k) | (u64)k << 62;\n"), (
        "        r->val = val[k];\n",
        "        __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);\n"
        "        for (volatile int spin = 0; spin < 100; spin++)\n            ;\n"
        "        r->val = val[k];\n")],
    "F6 a full ring overwrites": [(
        "        if (head + n - __atomic_load_n(&h->tail, __ATOMIC_ACQUIRE) > h->cap)\n"
        "            return false;\n", "")],
    "F6 a loss not counted": [("        __atomic_fetch_add(&h->lost, 1, __ATOMIC_RELAXED);\n", "")],
    "F6 a loss charged to no block": [(
        "            __atomic_fetch_add(&h->blk_lost[blk], 1, __ATOMIC_RELAXED);\n", "            ;\n")],
    "F6 half a pair recorded at the edge": [("        if (head + n - __atomic_load_n(",
                                             "        if (head + 1 - __atomic_load_n(")],
    "F7 a drain past head": [("    if (upto < tail || upto > __atomic_load_n(&h->head, __ATOMIC_ACQUIRE))\n",
                              "    if (upto < tail)\n")],
    "F7 a drain past a record being written": [(
        "        if (!__atomic_load_n(&trace_recs(h)[j % h->cap].t, __ATOMIC_ACQUIRE))\n"
        "            return -3;\n", "        ;\n")],
    "F7 the tail moved before the slots are zeroed": [(ZERO_THEN_TAIL, """    __atomic_store_n(&h->tail, upto, __ATOMIC_RELEASE);
    for (volatile int spin = 0; spin < 2000; spin++)
        ;
    for (u64 j = tail; j < upto; j++)
        memset(&trace_recs(h)[j % h->cap], 0, sizeof(struct hvtrace_rec));
""")],
    "F8 a drain of anything accepted": [(
        "    if (!h || (h != sptm_virt_trace_blk && h != sptm_virt_trace_irq))\n", "    if (!h)\n")],
    "F9 a misaligned range accepted": [("((r->pa | r->len) & 0x3fff)", "((r->pa | r->len) & 0xfff)")],
    "F9 a PA past 48 bits accepted": [(" || r->pa >> 48 ||", " ||")],
    "F9 overlapping ranges accepted": [("                return \"two ranges overlap\";\n", "                ;\n")],
    "F9 33 pages accepted": [("    if (pages > HVTRACE_PAGES)\n", "    if (pages > 2 * HVTRACE_PAGES)\n")],
    "F9 a block with no range accepted": [("    if (used != (1U << c->n_blk) - 1)\n", "    if (0)\n")],
    "F9 9 blocks accepted": [("c->n_blk > HVTRACE_BLKS)", "c->n_blk > HVTRACE_BLKS + 1)")],
}


def _build_run(src_text, out_dir, timeout=60):
    out_dir.mkdir(parents=True, exist_ok=True)
    src = out_dir / "sptm_virt_trace.c"
    src.write_text(src_text)
    exe = out_dir / "trace_host"
    cc = subprocess.run(host_cc() + CFLAGS + [str(src), str(HARNESS), "-o", str(exe)],
                        capture_output=True, text=True)
    if cc.returncode:
        return SimpleNamespace(returncode=-1000, stdout=cc.stderr)
    try:
        return subprocess.run([str(exe), str(out_dir)], capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired:
        return SimpleNamespace(returncode=-999, stdout="timeout")


@pytest.fixture(scope="module")
def rings(tmp_path_factory):
    out = tmp_path_factory.mktemp("trace_host")
    r = _build_run(SRC.read_text(), out)
    assert r.returncode == 0, r.stdout[-4000:]
    assert r.stdout.rstrip().endswith("OK"), r.stdout[-2000:]
    return out


def test_harness_passes(rings):
    assert (rings / "blk.bin").is_file() and (rings / "irq.bin").is_file()


def _mutant(name, edits, base):
    text = SRC.read_text()
    for old, new in edits:
        assert text.count(old) == 1, f"{name}: the mutation's text isn't in the source once: {old!r}"
        text = text.replace(old, new)
    r = _build_run(text, base / name.replace(" ", "_").replace("'", ""), timeout=90)
    return name, r.returncode, r.stdout[-300:]


def test_every_failure_mode_is_noticed_when_broken(tmp_path):
    workers = max(2, min(8, (os.cpu_count() or 2) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        res = list(pool.map(lambda kv: _mutant(kv[0], kv[1], tmp_path), MUTATIONS.items()))
    unnoticed = [name for name, rc, out in res if rc == 0]
    broken = [(name, out) for name, rc, out in res if rc == -1000]
    assert not broken, f"mutants that don't compile: {broken}"
    assert not unnoticed, f"the harness didn't notice: {unnoticed}"


# ---- the decoder (D1-D7) ---------------------------------------------------------------------

PTD, ATC2, ATC2_B, AIC_EV = 0x3803c0000, 0x413000000, 0x413014000, 0x381040000
FIRST_RUN = ("0x3803c0000:0x4000:ptd,0x413000000:0x4000:atc2,0x413014000:0x4000:atc2,"
             "0x413020000:0x4000:atc2,0x413044000:0x4000:atc2,0x413050000:0x4000:atc2,"
             "0x412a84000:0x4000:atc2pipe,0x412280000:0x4000:atc2dwc3,"
             "0x410000000:0x4000:atc2axi2af,0x410020000:0x4000:atc2lioa,0x39a000000:0x4000:gpio")


def test_ring_dump_decodes_what_the_harness_did(rings):
    h, recs = hvtrace.ring_dump((rings / "blk.bin").read_bytes())
    assert h["name"][:2] == ["ptd", "atc2"] and h["blk_base"][1] == ATC2
    acc = hvtrace.accesses(recs)
    assert len(acc) == 10  # the harness's ten accesses; its two 128-bit ones are one each
    d = SimpleNamespace(blk=(h, recs, []))
    offs = [hvtrace.Decoded.off(d, a) for a in acc]
    # D3: the second page of atc2 is +0x16008 from its lowest, not from its own range
    assert ("atc2", 0x16008) in offs and ("atc2", 0x17090) in offs and ("ptd", 0x1d10) in offs
    pair = next(a for a in acc if a.pa == ATC2_B + 0x3090)
    assert (pair.val, pair.hi, pair.width, pair.write) == (0x1111222233334444, 0x5555666677778888,
                                                           4, True)
    sptm = next(a for a in acc if a.pa == PTD + 0x1f30)
    assert sptm.gl and hvtrace.who(sptm) == "SPTM" and sptm.width == 0
    assert [a.cpu for a in acc][:5] == [3, 1, 2, 0, 9]


def test_d1_a_slot_being_written_ends_the_records(rings):
    raw = bytearray((rings / "blk.bin").read_bytes())
    h = hvtrace.parse_hdr(raw)
    struct.pack_into("<Q", raw, hvtrace.HDR_BYTES + 32 * 3, 0)  # slot 3: its time not yet written
    _, recs = hvtrace.ring_dump(bytes(raw))
    assert [r.index for r in recs] == [0, 1, 2] and h["head"] > 3


def test_d5_aic_events(rings):
    h, recs = hvtrace.ring_dump((rings / "irq.bin").read_bytes())
    ev = [hvtrace.aic_event(r.val, h["irq_max"]) for r in recs if r.blk == hvtrace.IRQ_EVENT]
    assert ev == [(1, 0, 892, 892), (1, 0, 894, 894), (1, 0, 895, 895), (1, 1, 5, 0x405)]
    assert h["irq_zero"] == 2 and h["irq_event"] == AIC_EV
    other = [r for r in recs if r.blk == hvtrace.IRQ_OTHER]
    assert [(r.pa - AIC_EV, r.write) for r in other] == [(0x100, False), (0x104, True), (0, True)]


class FakeHV:
    """aura's two rings in host memory, read and drained as the proxy does (sptm_virt_trace.c's
    rules, in Python): enough for the capture directory's bookkeeping."""

    BLK, IRQ, SYM_BLK, SYM_IRQ, DRAIN = 0x10000, 0x40000, 0x100, 0x108, 0x200

    def __init__(self, cap=16):
        self.mem = {}
        self.cap = cap
        for base, kind in ((self.BLK, 1), (self.IRQ, 2)):
            hdr = bytearray(hvtrace.HDR_BYTES)
            struct.pack_into("<QIIII Q", hdr, 0, hvtrace.MAGIC, 1, 1024, 32, kind, cap)
            struct.pack_into("<QQ", hdr, 64, 24000000, 1000)  # cntfrq, t_setup
            struct.pack_into("<IIII", hdr, 96, 0x400, 1, 1, 0)  # irq_max n_blk n_rng
            struct.pack_into("<QII", hdr, 312, PTD if kind == 1 else AIC_EV, 0x4000, 0)
            struct.pack_into("<Q", hdr, 248, PTD)  # blk_base[0]
            hdr[568:568 + 16] = b"ptd".ljust(16, b"\0") if kind == 1 else b"aic-event".ljust(16, b"\0")
            self.mem[base] = hdr + bytearray(32 * cap)
        self.sym = {"sptm_virt_trace_blk": self.SYM_BLK, "sptm_virt_trace_irq": self.SYM_IRQ,
                    "sptm_virt_trace_drain": self.DRAIN}
        self.t = 2000
        self.n = {self.BLK: 0, self.IRQ: 0}

    def field(self, base, off, val=None):
        if val is None:
            return struct.unpack_from("<Q", self.mem[base], off)[0]
        struct.pack_into("<Q", self.mem[base], off, val)

    def readmem(self, addr, size):
        if addr == self.SYM_BLK:
            return struct.pack("<Q", self.BLK)
        if addr == self.SYM_IRQ:
            return struct.pack("<Q", self.IRQ)
        for base, buf in self.mem.items():
            if base <= addr and addr + size <= base + len(buf):
                return bytes(buf[addr - base:addr - base + size])
        raise AssertionError(f"read outside the rings: 0x{addr:x}+{size}")

    def record(self, base, n=1, pending=False):
        """n accesses by the guest; the last one's time not yet written with pending."""
        for k in range(n):
            head, tail = self.field(base, 32), self.field(base, 40)
            if head - tail >= self.cap:
                self.field(base, 48, self.field(base, 48) + 1)
                continue
            self.t += 10
            v = self.n[base]
            self.n[base] += 1
            pa = PTD + 4 * (v % 16) if base == self.BLK else AIC_EV
            val = v if base == self.BLK else (0x10000 | (892 + v % 4))
            meta = pa | 2 << 48 | (v % 10) << 52
            off = 1024 + 32 * (head % self.cap)
            t = 0 if pending and k == n - 1 else self.t
            struct.pack_into("<4Q", self.mem[base], off, t, val, 0xfffffe0000000000 | v, meta)
            self.field(base, 32, head + 1)

    def complete(self, base):
        for i in range(self.cap):
            off = 1024 + 32 * i
            if not struct.unpack_from("<Q", self.mem[base], off)[0] and \
                    struct.unpack_from("<Q", self.mem[base], off + 24)[0]:
                self.t += 10
                struct.pack_into("<Q", self.mem[base], off, self.t)

    def call(self, addr, ptr, upto):
        assert addr == self.DRAIN
        tail, head = self.field(ptr, 40), self.field(ptr, 32)
        if upto < tail or upto > head:
            return -2 & 0xffffffff
        for j in range(tail, upto):
            if not struct.unpack_from("<Q", self.mem[ptr], 1024 + 32 * (j % self.cap))[0]:
                return -3 & 0xffffffff
        for j in range(tail, upto):
            off = 1024 + 32 * (j % self.cap)
            self.mem[ptr][off:off + 32] = bytes(32)
        self.field(ptr, 40, upto)
        self.field(ptr, 56, self.field(ptr, 56) + 1)
        return 0


def test_d2_d7_reads_and_drains_keep_every_record_once(tmp_path):
    hv = FakeHV(cap=16)
    out = tmp_path / "cap"
    quiet = lambda *a: None  # noqa: E731
    hv.record(hv.BLK, 10)
    hv.record(hv.IRQ, 3)
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, max_bytes=32 * 6, log=quiet)  # a partial read
    hv.record(hv.BLK, 4, pending=True)  # the last one still being written at the next read
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, max_bytes=32 * 100, log=quiet)
    hv.complete(hv.BLK)
    hv.record(hv.BLK, 30)  # fills the ring: 16 - 1 undrained left room for 15 (well, cap)
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, drain=False, log=quiet)
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, log=quiet)  # nothing new, nothing twice
    assert hv.field(hv.BLK, 40) == hv.field(hv.BLK, 32), "what the read without a drain saved stays"
    hv.record(hv.BLK, 3)
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, log=quiet)
    data = hvtrace.load(out)
    h, recs, gaps = data["blk"]
    vals = [r.val for r in recs]
    assert vals == list(range(len(vals))), "a record twice, out of order, or dropped"
    lost = hv.field(hv.BLK, 48)
    assert len(vals) + lost == 10 + 4 + 30 + 3 and lost > 0 and h["lost"] == lost
    assert not gaps
    assert [r.val & 0xffff for r in data["irq"][1]] == [892, 893, 894]
    # D7: records drained behind the capture's back are a gap, and the summary says so
    hv.record(hv.BLK, 5)
    tail = hv.field(hv.BLK, 40)
    assert hv.call(hv.DRAIN, hv.BLK, tail + 2) == 0
    hvtrace.capture(hv.readmem, hv.call, hv.sym, out, log=quiet)
    _, recs2, gaps = hvtrace.load(out)["blk"]
    assert gaps == [(recs[-1].index + 1, 2)]
    text = hvtrace.report(out)
    assert "GAP: 2 records" in text and f"LOST {lost}" in text
    # Another boot's ring (its setup time differs) never goes into this directory.
    hv2 = FakeHV(cap=16)
    hv2.field(hv2.BLK, 72, 5000)
    hv2.record(hv2.BLK, 2)
    with pytest.raises(SystemExit):
        hvtrace.capture(hv2.readmem, hv2.call, hv2.sym, out, log=quiet)


def test_d4_d6_summary_and_timeline(tmp_path, rings):
    out = tmp_path / "cap"
    cap = hvtrace.Capture(out)
    for ring in ("blk", "irq"):
        raw = (rings / f"{ring}.bin").read_bytes()
        h, recs = hvtrace.ring_dump(raw)
        body = b"".join(raw[1024 + 32 * (r.index % h["cap"]):][:32] for r in recs)
        cap.save(ring, raw[:1024], recs[0].index, body, h)
    text = hvtrace.report(out, {892: ["/arm-io/pmgr/clpc"]}, [892, 999])
    lines = text.splitlines()
    # D4: one line per distinct (offset, width, R/W); the 128-bit write once, with both words
    row = next(x for x in lines if x.strip().startswith("+0x17090"))
    assert row.split()[1:4] == ["128", "W", "1"] and "0x55556666777788881111222233334444" in row
    assert next(x for x in lines if x.strip().startswith("+0x01d10")).split()[1:4] == ["32", "R", "1"]
    assert any("892" in x and "/arm-io/pmgr/clpc" in x and "<<<" in x for x in lines)
    assert any("999  not taken" in x for x in lines)
    tl = (out / "timeline.txt").read_text().splitlines()[1:]
    secs = [float(x.split()[0]) for x in tl]
    assert secs == sorted(secs), "D6: the timeline isn't in time order"
    assert sum(" IRQ hw " in x for x in tl) == 1  # only 892 kept with --irq 892,999
    assert sum(" atc2 " in x for x in tl) == 5 and sum(" ptd " in x for x in tl) == 5


# ---- the block list on the host -----------------------------------------------------------

def test_the_first_runs_list_parses():
    rngs, names = hvtrace.parse_blks(FIRST_RUN)
    assert names == ["ptd", "atc2", "atc2pipe", "atc2dwc3", "atc2axi2af", "atc2lioa", "gpio"]
    assert len(rngs) == 11 and sum(r[1] for r in rngs) == 11 * 0x4000
    cfg = hvtrace.config({"AURA_TRACE_BLKS": FIRST_RUN, "AURA_IRQ_TRACE": "1"})
    assert struct.unpack_from("<6I", cfg) == (11, 7, 0, 0, 1, 0)
    assert struct.unpack_from("<QII", cfg, 24 + 16 * 2) == (0x413014000, 0x4000, 1)


@pytest.mark.parametrize("spec", [
    "0x3803c1000:0x4000", "0x3803c0000:0x2000", "0x3803c0000:0", "0x1000000000000:0x4000",
    "0x413000000:0x8000:a,0x413004000:0x4000:b", "0x3803c0000", "x:0x4000", "0x0:0x4000:bad name",
    ",".join(f"0x{0x413000000 + 0x8000 * i:x}:0x4000" for i in range(17)),
    ",".join(f"0x{0x413000000 + 0x8000 * i:x}:0x4000:b{i}" for i in range(9)),
    "0x413000000:0x84000",
])
def test_f9_the_host_refuses_what_aura_would(spec):
    assert hvtrace.check_env({"AURA_TRACE_BLKS": spec})


def test_unset_changes_nothing():
    p = SimpleNamespace(iface=None)  # any write would fail
    assert hvtrace.config({}) is None and not hvtrace.configure(p, {}, {})
    assert hvtrace.check_env({}) == "" and hvtrace.check_link(None, ("/arm-io/usb-drd1",), {}) == ""
    assert hvtrace.check_env({"AURA_TRACE_PRINT": "4"})  # nothing to print
    assert hvtrace.check_env({"AURA_IRQ_TRACE": "2"})


class FakeNode:
    def __init__(self, name, regs):
        self.name, self._regs, self.reg = name, regs, regs

    def get_reg(self, i):
        if self._regs[i] is None:
            raise ValueError("not MMIO")
        return self._regs[i]


def test_h1_the_debug_links_pages_are_refused_on_the_host():
    tree = {"/arm-io/usb-drd1": FakeNode("usb-drd1", [(0x40a280000, 0x11800)]),
            "/arm-io/atc-phy1": FakeNode("atc-phy1", [(0x408000000, 0x1000000)]),
            "/arm-io/nub-spmi-a0/hpm1": FakeNode("hpm1", [None]),
            "/arm-io/nub-spmi-a0": FakeNode("nub-spmi-a0", [(0x3880d0000, 0x4000)])}
    nodes = ("/arm-io/atc-phy1", "/arm-io/nub-spmi-a0/hpm1", "/arm-io/usb-drd1")
    for spec in ("0x40a280000:0x4000", "0x408020000:0x4000", "0x3880d0000:0x4000",
                 "0x3803c0000:0x4000:ptd,0x40a28c000:0x4000:x"):
        why = hvtrace.check_link(tree, nodes, {"AURA_TRACE_BLKS": spec})
        assert "debug link" in why, spec
    assert hvtrace.check_link(tree, nodes, {"AURA_TRACE_BLKS": FIRST_RUN}) == ""


def test_h1_on_the_j773g_adt():
    """With the live 27.2 ADT (TRACE_ADT=<adt.bin>): the first run's pages are none of the debug
    link's (run_guest_sptm's debug_port() of atc-phy1), and the link's dwc3 page is refused."""
    path = os.environ.get("TRACE_ADT")
    if not path:
        pytest.skip("TRACE_ADT not set")
    import importlib.util

    from aura.adt import load_adt
    spec = importlib.util.spec_from_file_location("rgs", REPO / "proxyclient/tools/run_guest_sptm.py")
    rgs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rgs)
    raw = Path(path).read_bytes()
    tree = load_adt(raw)
    phy = getattr(tree["/arm-io/atc-phy1"], "AAPL,phandle")
    _, nodes = rgs.debug_port(raw, phy)
    assert "/arm-io/usb-drd1" in nodes and "/arm-io/atc-phy1" in nodes
    assert hvtrace.check_link(tree, nodes, {"AURA_TRACE_BLKS": FIRST_RUN}) == ""
    assert hvtrace.check_link(tree, nodes, {"AURA_TRACE_BLKS": "0x40a280000:0x4000"})
    names = hvtrace.irq_names(tree)
    assert "/arm-io/pmgr/clpc" in names[892] and "/arm-io/pmgr/ppm" in names[895]


def _adt_host(tmp):
    """src/sptm_virt_trace_adt.c with sptm_virt_s2r_soc.c, sptm_virt_gfxrec_adt.c, pmgr.c and
    aura's ADT reader (rust/src/adt.rs) for the host, as test_sptm_virt_gfxrec_host.py builds
    them."""
    import re
    rs, src = tmp / "rs", tmp / "src"
    rs.mkdir()
    src.mkdir()
    (tmp / "build").mkdir()
    (tmp / "build" / "build_cfg.h").write_text("")
    adt_rs = (REPO / "rust/src/adt.rs").read_text().replace(
        "CStr::from_bytes_until_nul(&self.name)",
        "CStr::from_bytes_until_nul(unsafe { &*(&self.name as *const [c_char; 32] "
        "as *const [u8; 32]) })")
    (rs / "adt.rs").write_text(adt_rs)
    (rs / "lib.rs").write_text("#![allow(non_camel_case_types, dead_code, unused_imports)]\n"
                               "type c_size_t = usize;\nmod adt;\n")
    subprocess.run(["rustc", "--edition", "2021", "--crate-type", "staticlib", "-O", "-o",
                    str(rs / "libadt_host.a"), str(rs / "lib.rs")], check=True)
    types = "".join(x for x in (REPO / "src/types.h").read_text().splitlines(keepends=True)
                    if not re.match(r"typedef (u64 uintptr_t|s64 ptrdiff_t|s64 ssize_t);", x))
    (src / "types.h").write_text(types)
    files = ("src/pmgr.c", "src/sptm_virt_gfxrec_adt.c", "src/sptm_virt_trace_adt.c",
             "src/sptm_virt_s2r_soc.c")
    for f in ("tests/t6031_host/utils.h", "tests/t6031_host/memory.h") + files:
        (src / Path(f).name).write_text((REPO / f).read_text())
    out = tmp / "trace-adt-host"
    subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall",
                    "-Wno-unused-function", "-Wno-address-of-packed-member", "-Wno-format",
                    "-include", str(src / "types.h"), f"-I{src}", f"-I{REPO / 'src'}"]
                   + [str(src / Path(f).name) for f in files]
                   + [str(REPO / "tests/trace_host/trace_adt_host.c"), str(rs / "libadt_host.a"),
                      "-o", str(out)], check=True)
    return out


@pytest.mark.skipif(not os.environ.get("TRACE_ADT"),
                    reason="no J773g 27.2 ADT given (TRACE_ADT=<a live adt.bin>)")
def test_h1_aura_refuses_the_debug_link_on_the_j773g_adt(tmp_path):
    """aura's own decision, on the live 27.2 ADT with the host link's PHY (atc-phy1) as the link:
    every page of the first run is fine and covered; the link's dwc3, pipehandler, its ATC
    (ATCAXI2AF and LIOA of ATC1), its DART, its USB4 side, the bus of its port controller (hpm1
    on nub-spmi-a0) are refused. With no link PHY at all, every USB-C port's pages are."""
    from aura.adt import load_adt
    host = _adt_host(tmp_path)
    tree = load_adt(Path(os.environ["TRACE_ADT"]).read_bytes())
    phy = getattr(tree["/arm-io/atc-phy1"], "AAPL,phandle")
    rngs, _ = hvtrace.parse_blks(FIRST_RUN)
    first = [f"0x{pa:x}:0x{ln:x}" for pa, ln, _ in rngs]
    spmi = tree["/arm-io/nub-spmi-a0"].get_reg(0)[0]
    link = ["0x40a280000:0x4000", "0x40aa84000:0x4000", "0x408000000:0x4000", "0x408020000:0x4000",
            "0x40af00000:0x4000", "0x409f00000:0x4000", f"0x{spmi & ~0x3fff:x}:0x4000"]
    out = subprocess.run([str(host), os.environ["TRACE_ADT"], str(phy)] + first + link,
                         capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
    lines = out.stdout.splitlines()
    assert lines[0] == "link 1", lines
    for line in lines[1:1 + len(first)]:
        assert line.startswith("ok ") and "(no cover)" not in line, line
    refused = lines[1 + len(first):]
    assert all(x.startswith("refused ") for x in refused), refused
    # the first link device the walk finds over each page (ATC1's window holds apciec1's too)
    assert [x.split()[2] for x in refused] == ["usb-drd1", "usb-drd1", "atc-phy1", "apciec1",
                                              "dart-usb1", "acio1", "nub-spmi-a0"], refused
    out = subprocess.run([str(host), os.environ["TRACE_ADT"], "-"] + first[1:3] + first[-1:],
                         capture_output=True, text=True, check=True).stdout.splitlines()
    assert out[0] == "link 0" and out[1].startswith("refused ") and out[2].startswith("refused ")
    assert out[3].startswith("ok "), out  # the GPIO page isn't a USB-C port's


@pytest.mark.parametrize("env", [{"AURA_TRACE_BLKS": "0x3803c1000:0x4000"},
                                 {"AURA_IRQ_TRACE": "yes"},
                                 {"AURA_TRACE_BLKS": FIRST_RUN + ",0x420000000:0x58000:big"}])
def test_run_guest_sptm_refuses_before_it_connects(env, tmp_path):
    """A list aura would refuse never reaches a device: run_guest_sptm stops before it opens one
    (no AURADEVICE here: it would fail on the open otherwise)."""
    x = tmp_path / "x"
    x.write_bytes(b"")
    e = {k: v for k, v in os.environ.items() if not k.startswith(("AURA", "XNU_"))}
    e.update(env, PYTHONPATH=str(REPO / "proxyclient"))
    r = subprocess.run([sys.executable, str(REPO / "proxyclient/tools/run_guest_sptm.py"),
                        "--sptm", str(x), "--txm", str(x), "--xnu", str(x)],
                       capture_output=True, text=True, env=e, timeout=60)
    assert r.returncode != 0 and "refused before anything was sent" in r.stderr, r.stderr[-500:]
