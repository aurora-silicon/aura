# SPDX-License-Identifier: MIT
"""src/sptm_virt_gfxrec.c, the GPU/PMP mailbox record of a guest that runs past its InitData
doorbell, with its cores as host threads (tests/gfxrec_host), read back by proxyclient/aura/
hvgfxrec.py, and the mutants that prove the harness notices each way the record can fail.

What the record must never do, each one a mutant below that must fail the harness:
  - record anything before the doorbell that starts it, or let a second doorbell restart it (its
    window would then move and the first one's record be mixed with a later one);
  - record past the window's end, end a tick early or late, or never end;
  - write past its buffer, or fill it without saying so (the state, the slots claimed past the
    capacity, when it stopped);
  - lose or duplicate a record when cores record at once, or leave one half-written once every
    core has finished (a record's time is written last: 0 marks a slot still being written);
  - take the wrong accesses: an ASC's other registers (its CPU control, the mailbox controls), a
    read of the A2I words or a write of the I2A ones, an sgx read, an sgx write outside
    +0xd60000..+0xd60067, +0x1000104 and +0x1000108, a PMGR read, a write to another device's PS
    word; or miss one: the second word of a mailbox written as two 64-bit halves, a 32-bit store
    pair over a PS word, the doorbell itself;
  - put a gfx-asc access (inside sgx's window) under sgx, or a PMP one under the GPU;
  - get a record's direction, width, core, value or second word wrong;
  - change what the guest wrote or read (the record is pass-through: it only reads the value);
  - let the MMIO ring run past the window's end (the host reads the lead-in from it at the
    desktop; a ring still running then has overwritten it).
The MMIO_PROTECTED block (sgx +0xe00000, 0x4000 bytes: the one register-map record the GPU firmware
rewrites at InitData) has a record of its own, which must never:
  - wait for the doorbell (it records from the first access the stage 2 traps, at XNU's entry),
    or end anywhere but at the mailbox record's window end (or when full);
  - miss a read or a write, take an access just outside the block, or lose a pair's second word;
  - lose who made the access (SPTM's GL1 traps against XNU's), its PC, core, width or value;
  - share its buffer with the mailbox record: a block XNU polls must not starve the mailboxes.
The host's decoding is checked against the same buffers: halves of a split message paired per core,
time order, the trigger marked, endpoints and message types named, the PMP's 0x0037000000000008 and
0x0020f50100000000 called out, the per-endpoint counts, and drops reported.

Needs a host compiler with pthreads; runs in a few seconds, the mutants in parallel.
"""

import concurrent.futures
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
from aura import hvgfxrec  # noqa: E402

SRC = REPO / "src/sptm_virt_gfxrec.c"
HARNESS = REPO / "tests/gfxrec_host/gfxrec_host.c"
CFLAGS = ["-O1", "-g", "-pthread", "-w", "-DGFXREC_HOST", f"-I{REPO}/src",
          f"-I{REPO}/tests/gfxrec_host"]

STATE_CHECK = "    if (!h || __atomic_load_n(&h->state, __ATOMIC_ACQUIRE) != GFXREC_ON)\n        return;\n"
END_CHECK = "    if (t >= h->t_end) {\n        u32 on"
CLAIM = "    u64 i = __atomic_fetch_add(&h->claimed, 1, __ATOMIC_RELAXED);\n"
FULL_CHECK = "    if (i >= h->cap) {\n"
ONCE = "    if (__atomic_exchange_n(&gfxrec_triggered, 1, __ATOMIC_SEQ_CST))\n        return false;\n"
T_LAST = ("((u64)(cpu & 15) << 56);\n    __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);\n}\n")
BLK_STATE = "    if (!b || __atomic_load_n(&b->state, __ATOMIC_ACQUIRE) != GFXREC_ON)\n"
BLK_END = "    if (end && t >= end) {\n"

# name: [(text in src/sptm_virt_gfxrec.c, its replacement), ...], applied together
MUTATIONS = {
    "records before the doorbell": [("    h->state = GFXREC_ARMED;\n",
                                     "    h->state = GFXREC_ON;\n    h->t_end = ~0UL;\n")],
    "an armed record ends at its first access": [(STATE_CHECK, (
        "    if (!h || __atomic_load_n(&h->state, __ATOMIC_ACQUIRE) < GFXREC_ARMED)\n"
        "        return;\n")), (
        "    if (t >= h->t_end) {\n        u32 on = GFXREC_ON;\n",
        "    if (t >= h->t_end) {\n        u32 on = h->state;\n")],
    "a second doorbell restarts the window": [(ONCE, "")],
    "the window never ends": [(END_CHECK, "    if (0) {\n        u32 on")],
    "the window ends a tick late": [(END_CHECK, "    if (t > h->t_end) {\n        u32 on")],
    "the window ends a tick early": [(END_CHECK, "    if (t + 1 >= h->t_end) {\n        u32 on")],
    "a full buffer is written past": [(FULL_CHECK, "    if (0) {\n")],
    "a full buffer isn't said": [(
        "        if (__atomic_compare_exchange_n(&h->state, &on, GFXREC_DONE_FULL, false,",
        "        if (__atomic_compare_exchange_n(&h->state, &on, GFXREC_ON, false,")],
    "slots are claimed non-atomically": [(CLAIM, (
        "    u64 i = __atomic_load_n(&h->claimed, __ATOMIC_RELAXED);\n"
        "    for (volatile int spin = 0; spin < 50; spin++)\n        ;\n"
        "    __atomic_store_n(&h->claimed, i + 1, __ATOMIC_RELAXED);\n"))],
    "a record's time is written first": [(T_LAST, "((u64)(cpu & 15) << 56);\n}\n"), (
        "    struct gfxrec_rec *r = &gfxrec_recs(h)[i];\n",
        "    struct gfxrec_rec *r = &gfxrec_recs(h)[i];\n"
        "    __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);\n"
        "    for (volatile int spin = 0; spin < 200; spin++)\n        ;\n")],
    "A2I reads are taken": [(
        "        return write ? off >= GFXREC_A2I && off < GFXREC_A2I + 16\n",
        "        return off >= GFXREC_A2I && off < GFXREC_A2I + 16 ? true\n")],
    "the second half of a split write is lost": [(
        "        return write ? off >= GFXREC_A2I && off < GFXREC_A2I + 16\n",
        "        return write ? off >= GFXREC_A2I && off < GFXREC_A2I + 8\n")],
    "an ASC's other registers are taken": [(
        "        return write ? off >= GFXREC_A2I && off < GFXREC_A2I + 16\n"
        "                     : off >= GFXREC_I2A && off < GFXREC_I2A + 16;\n",
        "        return true;\n")],
    "sgx reads are taken": [("    if (src == GFXREC_SRC_SGX && write) {\n",
                             "    if (src == GFXREC_SRC_SGX) {\n")],
    "the host block's last word is missed": [("    {0xd60000, 0x68},\n", "    {0xd60000, 0x60},\n")],
    "the gfx-asc counts as sgx": [(
        "    for (u32 s = 0; s < GFXREC_SRCS; s++) {\n",
        "    for (u32 s = GFXREC_SRC_SGX; s < GFXREC_SRCS; s++) {\n")],
    "a store pair over a PS word is missed": [(
        "        if (addr < h->ps[k] + 4 && addr + bytes > h->ps[k])\n",
        "        if (addr == h->ps[k])\n")],
    "another device's PS word is taken": [(
        "        if (addr < h->ps[k] + 4 && addr + bytes > h->ps[k])\n",
        "        if (addr < h->ps[k] + 16 && addr + bytes > h->ps[k])\n")],
    "the direction is lost": [("((u64)src << 48) | ((u64) !!write << 52)",
                               "((u64)src << 48) | ((u64)1 << 52)")],
    "a pair's second word is lost": [("    r->hi = width == 4 ? val[1] : 0;\n", "    r->hi = 0;\n")],
    "the block waits for the doorbell": [(BLK_STATE, (
        "    if (!b || __atomic_load_n(&b->state, __ATOMIC_ACQUIRE) != GFXREC_ON ||\n"
        "        !sptm_virt_gfxrec_buf || sptm_virt_gfxrec_buf->state != GFXREC_ON)\n"))],
    "the block doesn't end with the window": [(BLK_END, "    if (0) {\n")],
    "the block ends a tick late": [(BLK_END, "    if (end && t > end) {\n")],
    "an access just past the block is taken": [(
        "    if (addr - b->base >= b->size || width < 0 || width > 4)\n",
        "    if (addr - b->base > b->size || width < 0 || width > 4)\n")],
    "the block's reads are lost": [(
        "    if (addr - b->base >= b->size || width < 0 || width > 4)\n",
        "    if (addr - b->base >= b->size || width < 0 || width > 4 || !write)\n")],
    "SPTM's accesses pass for XNU's": [("((u64) !!gl << 60)", "((u64)0 << 60)")],
    "the block's pair loses its second word": [(
        "        r->val = val[k];\n", "        r->val = val[0];\n")],
    "the block's PC is lost": [("        r->pc = pc;\n", "        r->pc = 0;\n")],
    "the block shares the mailbox record's buffer": [(
        "    u64 i = __atomic_fetch_add(&b->claimed, n, __ATOMIC_RELAXED);\n",
        "    u64 i = __atomic_fetch_add(&b->claimed, n, __ATOMIC_RELAXED);\n"
        "    if (sptm_virt_gfxrec_buf)\n"
        "        __atomic_fetch_add(&sptm_virt_gfxrec_buf->claimed, n, __ATOMIC_RELAXED);\n")],
    "a full block isn't said": [(
        "        if (__atomic_compare_exchange_n(&b->state, &on, GFXREC_DONE_FULL, false,",
        "        if (__atomic_compare_exchange_n(&b->state, &on, GFXREC_ON, false,")],
    "the ring runs past the window": [(
        "    __atomic_store_n(&sptm_virt_gfxrec_ring_stop, t_end, __ATOMIC_RELAXED);\n", "")],
}


def _build_run(source, out, outdir, timeout=120):
    subprocess.check_call(host_cc() + CFLAGS + ["-o", str(out), str(HARNESS), str(source)])
    try:
        return subprocess.run([str(out), str(outdir)], capture_output=True, text=True,
                              timeout=timeout)
    except subprocess.TimeoutExpired as e:
        return subprocess.CompletedProcess(e.cmd, 124, e.stdout or "", "timed out")


_RUN = {}


def harness_out():
    """The unmutated harness's buffers (built and run once)."""
    if "dir" not in _RUN:
        d = Path(tempfile.mkdtemp())
        r = _build_run(SRC, d / "gfxrec", d)
        assert r.returncode == 0, r.stdout[-4000:] + r.stderr[-4000:]
        _RUN["dir"] = d
    return _RUN["dir"]


def test_record_on_host_threads():
    harness_out()


def _mutant(args):
    name, edits, d = args
    m = SRC.read_text()
    for a, b in edits:
        assert m.count(a) == 1, f"{name}: the checked code changed"
        m = m.replace(a, b, 1)
    sub = d / f"m{abs(hash(name))}"
    sub.mkdir()
    src = sub / "gfxrec.c"
    src.write_text(m)
    return name, _build_run(src, sub / "gfxrec", sub, timeout=60).returncode


def test_every_rule_is_noticed_when_broken():
    d = Path(tempfile.mkdtemp())
    workers = max(2, min(8, (os.cpu_count() or 2) // 2))
    with concurrent.futures.ThreadPoolExecutor(workers) as pool:
        results = list(pool.map(_mutant, [(n, e, d) for n, e in MUTATIONS.items()]))
    missed = [n for n, rc in results if rc == 0]
    assert not missed, f"not noticed: {missed}"


# ---- the host's side ------------------------------------------------------------------------

def _load(name):
    raw = (harness_out() / name).read_bytes()
    return hvgfxrec.parse(raw)


def test_decode_the_window():
    rec = _load("window.bin")
    h = rec.hdr
    assert h["magic"] == hvgfxrec.MAGIC and h["state"] == hvgfxrec.DONE_TIME
    assert h["window_ms"] == 2000 and h["t_end"] - h["t_start"] == 2000 * h["cntfrq"] // 1000
    assert len(rec.recs) == 18 and rec.incomplete == 0
    # The host's copy of the rules (for the MMIO ring) takes exactly what aura took.
    assert all(hvgfxrec.classify(h, r.pa, r.write, r.width) == r.src for r in rec.recs)
    msgs = rec.messages()
    # Time order, the trigger among them.
    ts = [m.t for m in msgs]
    assert ts == sorted(ts)
    trig = [m for m in msgs if m.trigger]
    assert len(trig) == 1 and trig[0].ep == 0x20 and trig[0].msg == 0x00810c2000100000
    assert trig[0].src == "gfx" and trig[0].dir == "A2I"
    # The split PMP write is one message, and so is the split read; nothing pairs across cores.
    split = [m for m in msgs if m.src == "pmp" and m.msg == hvgfxrec.PMP_CONFIRM_8]
    assert len(split) == 1 and split[0].ep == 0x20 and split[0].dir == "A2I" and split[0].halves == 2
    vote = [m for m in msgs if m.msg == hvgfxrec.PMP_AGX_VOTE]
    assert len(vote) == 1 and vote[0].dir == "A2I" and vote[0].src == "pmp"
    reply = [m for m in msgs if m.src == "pmp" and m.dir == "I2A" and m.halves == 2]
    assert len(reply) == 1 and reply[0].msg == 0x0021000000000000
    lone = [m for m in msgs if m.halves == 1]
    assert len(lone) == 1 and lone[0].cpu == 5, "a half from another core was paired"
    lines = hvgfxrec.trace_lines(rec)
    text = "\n".join(lines)
    assert "<<< 0x0037000000000008" in text and "<<< 0x0020f50100000000" in text
    assert "[TRIGGER]" in text and "InitData" in text
    assert "devpower cmd 0x0f dev 5 on" in text
    assert "HELLO" in text and "AP_PWR_STATE" in text and "ON" in text
    assert "sgx+0xd60060" in text and "sgx+0x1000108" in text
    assert "pmgr GFX" in text
    # Per endpoint.
    s = hvgfxrec.summary(rec)
    assert "gfx A2I ep 0x20" in s and "pmp A2I ep 0x20" in s and "pmp I2A ep 0x20" in s
    assert "0x0037000000000008" in s and "0x0020f50100000000" in s
    assert "ended by time" in s and "0 dropped" in s


def test_decode_a_full_buffer():
    rec = _load("full.bin")
    assert rec.hdr["state"] == hvgfxrec.DONE_FULL
    blk = hvgfxrec.parse_blk((harness_out() / "blk-full.bin").read_bytes())
    assert blk.hdr["state"] == hvgfxrec.DONE_FULL and len(blk.recs) == 2
    assert "FULL" in hvgfxrec.blk_summary(blk, rec.hdr)
    s = hvgfxrec.summary(rec)
    assert "FULL" in s and "dropped" in s and "0 dropped" not in s


def _ring_rec(t, pa, val, write, cpu, width=3):
    """One record of sptm_virt_gpu.c's MMIO ring (struct gpu_rec)."""
    meta = pa | width << 48 | int(write) << 51 | cpu << 52
    return struct.pack("<4Q", t, meta, val, 0xfffffe0037390000)


def test_decode_the_ring():
    """The MMIO ring, which holds the lead-in: a pair is two records there, its halves paired per
    core, and only the record's own windows and rules apply."""
    rec = _load("window.bin")
    gfx, pmp = rec.hdr["base"][0], rec.hdr["base"][2]
    raw = b"".join([
        _ring_rec(10, 0x222000460, 0, False, 6, 2),               # pmp-ptd: none of the record's
        _ring_rec(11, gfx + 0x8114, 0x20001, False, 9, 2),        # a mailbox control
        _ring_rec(12, gfx + 0x8830, 0x00100000000c000c, False, 0),
        _ring_rec(13, pmp + 0x8800, 0x0013000000000000, True, 6),  # another core in between
        _ring_rec(14, gfx + 0x8838, 0x0, False, 0),
        _ring_rec(15, gfx + 0x8800, 0x00200000000c000c, True, 6),  # core 6: its pmp half is lone
        _ring_rec(16, gfx + 0x8808, 0x0, True, 6),
        bytes(32),                                                  # a slot never written
    ])
    msgs = hvgfxrec.ring_messages(raw, rec.hdr)
    # A split message is timed at its second half, when it is complete.
    assert [(m.t, m.src, m.dir, m.msg, m.halves) for m in msgs] == [
        (13, "pmp", "A2I", 0x0013000000000000, 1), (14, "gfx", "I2A", 0x00100000000c000c, 2),
        (16, "gfx", "A2I", 0x00200000000c000c, 2)]


def test_decode_the_block():
    """The MMIO_PROTECTED block's own record: from before the doorbell, who made each access,
    offsets in the block, values, a pair as its two words."""
    main = _load("window.bin")
    blk = hvgfxrec.parse_blk((harness_out() / "blk.bin").read_bytes())
    b = blk.hdr
    assert b["base"] == 0x300e00000 and b["size"] == 0x4000 and b["state"] == hvgfxrec.DONE_TIME
    assert b["node"] == "/arm-io/sgx reg[1] +0x100000"
    lines = hvgfxrec.blk_lines(blk, main.hdr)
    body = [x for x in lines if not x.startswith("#")]
    assert len(body) == len(blk.recs) == 8
    assert any("/arm-io/sgx reg[1] +0x100000" in x for x in lines if x.startswith("#"))
    ms = [float(x.split()[0]) for x in body]
    assert ms == sorted(ms) and ms[0] < 0 < ms[-1], "the block's record starts before the doorbell"
    assert "SPTM" in body[0] and "pc 0xfffffff027100000" in body[0] and "+0x1500" in body[0]
    assert "XNU " in body[1] and "R32" in body[1] and "+0x1400" in body[1] and "0x10000" in body[1]
    assert "W32" in body[2] and "+0x3ffc" in body[2]
    assert "+0x0010" in body[3] and "+0x0018" in body[4] and "0x2222" in body[4]
    s = hvgfxrec.blk_summary(blk, main.hdr)
    assert "SPTM 3" in s and "XNU 5" in s and "ended with the window" in s
    assert "/arm-io/sgx reg[1] +0x100000" in s


def _adt_host(tmp):
    """src/sptm_virt_gfxrec_adt.c with pmgr.c and aura's ADT reader (rust/src/adt.rs) for the
    host, as tests/python/test_sptm_virt_generic.py builds them."""
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
    for f in ("tests/t6031_host/utils.h", "tests/t6031_host/memory.h", "src/pmgr.c",
              "src/sptm_virt_gfxrec_adt.c"):
        (src / Path(f).name).write_text((REPO / f).read_text())
    out = tmp / "adt-host"
    subprocess.run(["cc", "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall",
                    "-Wno-unused-function", "-Wno-address-of-packed-member", "-Wno-format",
                    "-include", str(src / "types.h"), f"-I{src}", f"-I{REPO / 'src'}",
                    str(src / "sptm_virt_gfxrec_adt.c"), str(src / "pmgr.c"),
                    str(REPO / "tests/gfxrec_host/gfxrec_adt_host.c"),
                    str(rs / "libadt_host.a"), "-o", str(out)], check=True)
    return out


@pytest.mark.skipif(not os.environ.get("GFXREC_ADT"),
                    reason="no J773g 27.2 ADT given (GFXREC_ADT=<a live adt.bin>)")
def test_the_j773g_adt(tmp_path):
    """On the J773g's live 27.2 ADT: the reg aura names for the MMIO_PROTECTED block, and the one
    GFX/AGX/SGX device with a PS word (GFX-SGX, GFX-BUSY and GFX-ASC have none)."""
    host = _adt_host(tmp_path)
    out = subprocess.run([str(host), os.environ["GFXREC_ADT"], "0x300e00000", "0x4000"],
                         check=True, capture_output=True, text=True).stdout.splitlines()
    assert out[0] == "cover /arm-io/sgx reg[1] +0x100000", out
    assert out[1:] == ["ps GFX 0x380700250"], out


def test_what_follows_each_firmware_burst():
    """The question the record is for: what macOS sends right after the firmware's 0x42 (RX
    doorbell) bursts. A burst is the GPU's 0x42 messages (not the PMP's) less than 2 ms apart; the
    answer is the AP's first messages after its last one, to either coprocessor, never one
    from before it."""
    h = {"t_start": 0, "cntfrq": 24000000, "trig_val": 0, "trig_hi": 0, "trig_cpu": 0}
    ms = 24000

    def m(t_ms, src, write, msg, ep):
        return hvgfxrec.Message("mbox", int(t_ms * ms), 0, src, write, 0, msg, ep, 4, 2)

    G, P = hvgfxrec.SRC_GFX, hvgfxrec.SRC_PMP
    msgs = [m(0.0, G, True, 0x0083000000000011, 0x21),        # before: never an answer
            m(1.0, G, False, 0x0042000000000000, 0x20), m(1.5, G, False, 0x0042000000000000, 0x20),
            m(2.9, G, False, 0x0042000000000000, 0x20),        # 1.4 ms on: the same burst
            m(3.0, P, False, 0x0042000000000000, 0x20),        # the PMP's: not the GPU's burst
            m(3.2, P, True, 0x0037000000000008, 0x20),         # the first answers, PMP's and
            m(3.3, G, True, 0x0083000000000010, 0x21),         # the GPU's
            m(9.0, G, False, 0x0042000000000000, 0x20),        # a second burst, 6 ms later
            m(9.1, G, False, 0x00b0000000000020, 0x00),
            m(12.0, G, True, 0x00b0000000000020, 0x00)]
    lines = hvgfxrec.after_bursts(msgs, h)
    text = "\n".join(lines)
    assert sum(x.startswith("  burst ") for x in lines) == 2, text
    assert "burst 1: 3 GPU 0x42 messages, +1.000 .. +2.900 ms" in text
    assert "burst 2: 1 GPU 0x42 messages, +9.000 .. +9.000 ms" in text
    first = text.split("burst 2")[0]
    assert "+0.300 ms" in first and "pmp A2I" in first and "0x0037000000000008" in first
    assert "+0.400 ms" in first and "TX doorbell KICKFW" in first
    assert "DEVCTRL" not in first, "a message from before the burst"
    assert "the AP's first message to the GPU: +0.400 ms" in first
    second = text.split("burst 2")[1]
    assert "the AP's first message to the GPU: +3.000 ms" in second and "AP_PWR_STATE ON" in second


class FakePark:
    """A parked HV's memory as the proxy reads it: each read must fall inside what was put."""

    def __init__(self):
        self.mem, self.reads = {}, []

    def put(self, addr, data):
        self.mem[addr] = bytes(data)

    def readmem(self, addr, size):
        self.reads.append((addr, size))
        for base, data in self.mem.items():
            if base <= addr and addr + size <= base + len(data):
                return data[addr - base:addr - base + size]
        raise AssertionError(f"read of 0x{addr:x}+0x{size:x} outside what aura holds")


def _park(win, ring_slots, head, nrec):
    park, sym = FakePark(), {}
    for i, name in enumerate(("sptm_virt_gfxrec_buf", "sptm_virt_gpu_ring", "sptm_virt_gpu_nrec",
                              "sptm_virt_gpu_head")):
        sym[name] = 0x1000 + 8 * i
    park.put(0x1000, struct.pack("<4Q", 0x100000, 0x200000, nrec, head))
    # The record's buffer is larger than what was claimed: nothing past it is read.
    park.put(0x100000, win[:hvgfxrec.HDR_BYTES] + win[hvgfxrec.HDR_BYTES:])
    park.put(0x200000, b"".join(ring_slots))
    return park, sym


def test_read_from_a_park():
    win = (harness_out() / "window.bin").read_bytes()
    slots = [struct.pack("<4Q", 100 + i, i, i, i) for i in range(8)]
    # A wrapped ring comes back oldest first; only the claimed records of the buffer are read.
    park, sym = _park(win, slots, head=11, nrec=8)
    raw, rraw, head, nrec = hvgfxrec.read_hv(park.readmem, sym, chunk=64)
    assert raw == win and (head, nrec) == (11, 8)
    assert rraw == b"".join(slots[3:] + slots[:3])
    # One that never wrapped: its first head slots.
    park, sym = _park(win, slots, head=5, nrec=8)
    _raw, rraw, _h, _n = hvgfxrec.read_hv(park.readmem, sym)
    assert rraw == b"".join(slots[:5])
    # The block's record: only what was claimed; none in this boot reads as b"".
    braw = (harness_out() / "blk.bin").read_bytes()
    park.put(0x300000, braw + bytes(4096))
    park.put(0x2000, struct.pack("<Q", 0x300000))
    assert hvgfxrec.read_blk(park.readmem, {**sym, "sptm_virt_gfxrec_blk": 0x2000}, chunk=64) == braw
    park.put(0x2000, struct.pack("<Q", 0))
    assert hvgfxrec.read_blk(park.readmem, {**sym, "sptm_virt_gfxrec_blk": 0x2000}) == b""
    assert hvgfxrec.read_blk(park.readmem, sym) == b""
    # No record in this boot: nothing but the pointer.
    park, sym = _park(win, slots, head=5, nrec=8)
    park.put(0x1000, struct.pack("<4Q", 0, 0x200000, 8, 5))
    raw, _r, _h, _n = hvgfxrec.read_hv(park.readmem, sym, ring=False)
    assert raw == b"" and park.reads == [(0x1000, 8)]


def test_env_refuses_what_parks():
    ok = {"AURA_GFXPMP": "2000", "AURA_GPU_TRACE": "1", "AURA_GPU_STOP_MSG": "0x0081000000000000",
          "AURA_GPU_STOP_MSG_MASK": "0xffff000000000000", "AURA_GPU_STOP_EP": "0x20",
          "AURA_GPU_PRINT_MAX": "0", "AURA_GPU_NARROW": "1"}
    assert hvgfxrec.check_env(ok) == "" and hvgfxrec.check_env({}) == ""
    # hv22: the whole sgx window and the PMP dashboard trapped, the GPU never answered InitData.
    # A record takes only the narrow hooks.
    for extra in ({"AURA_GPU_MSG_PARK": "1"}, {"AURA_GPU_STOP_BOOT_SECS": "240"},
                  {"AURA_GPU_STOP_SECS": "5"}, {"AURA_GPU_SNAP": "1"}, {"AURA_GPU_TRACE": "0"},
                  {"AURA_GFXPMP": "0"}, {"AURA_GFXPMP_MAX": "0"}, {"AURA_GPU_NARROW": "0"}):
        assert hvgfxrec.check_env({**ok, **extra}), extra
    for missing in ("AURA_GPU_STOP_MSG", "AURA_GPU_STOP_MSG_MASK", "AURA_GPU_STOP_EP",
                    "AURA_GPU_NARROW"):
        assert hvgfxrec.check_env({k: v for k, v in ok.items() if k != missing}), missing


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
