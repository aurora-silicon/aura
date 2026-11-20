# SPDX-License-Identifier: MIT
"""src/sptm_virt_snap.c against a fake GPU firmware in a fake guest RAM (tests/snap_host), read back
by tools/dump_snapshot.py, and compared with the GPU lane's fw272-b2dump.py algorithm run on the same
RAM (below, its walk and its object follow as they are, over a RAM image instead of the proxy).

What a snapshot must never do, each one a mutant below that must fail the comparison:
  - read a PA the platform refuses: a page table outside RAM, a page the firmware maps to MMIO or to
    a hole in RAM, a region page in that hole (the harness stops at such a read);
  - walk or follow differently from fw272-b2dump.py: three levels of objects, 200 where it takes 201,
    a skipped last word of the 0x3000-byte scan, pointers only at aligned offsets, objects past
    64 KiB, runs merged across a gap in the PAs, the raw doorbell message taken as the root,
    TTBR0 walked where TTBR1 is valid;
  - write past its buffer, or fill it without saying so;
  - read a chain's offset step as a pointer, start a kick's chains anywhere but at the InitData
    root, take a watched word from the wrong place;
  - take everything at a later kick, or more snapshots than the budget.
Where fw272-b2dump.py would read a refused PA (it reads whatever the proxy gives), the reference
here treats it as unmapped and a refused region page as zeros, which is what aura does instead.
The mailbox log's decoding (two halves of a read paired, EP 0x21 marked) is checked too.
"""

import concurrent.futures
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from host_compiler import host_cc  # noqa: E402

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
from aura import hvsnap  # noqa: E402

SRC = REPO / "src/sptm_virt_snap.c"
HARNESS = REPO / "tests/snap_host/snap_host.c"
CFLAGS = ["-O1", "-g", "-w", "-DSNAP_HOST", f"-I{REPO}/src", f"-I{REPO}/tests/snap_host"]
DUMP = REPO / "proxyclient/tools/dump_snapshot.py"
PAGE, PA_MASK, IAS = 0x4000, 0x3ffffffc000, (1 << 42) - 1
ADD = hvsnap.STEP_ADD


def chains_for_test():
    chains, watches = hvsnap.PRESETS["g16g-272"]
    chains = [(n, s, 0x12000 if n == "hwdata" else ln) for n, s, ln in chains]
    chains.append(("hw-fa0", (0x18, 0x0, ADD | 0xfa0), 0x10))  # an offset step
    return chains, watches


class Ram:
    def __init__(self, out):
        self.meta = {}
        for line in (out / "meta.txt").read_text().splitlines():
            k, *v = line.split()
            self.meta[k] = [int(x, 0) for x in v]
        self.base, self.hole = self.meta["ram"][0], self.meta["hole"][0]
        self.data = (out / "ram.bin").read_bytes()

    def ok(self, pa, n):
        return (self.base <= pa and pa + n <= self.base + len(self.data)
                and not (pa < self.hole + PAGE and pa + n > self.hole))

    def read(self, pa, n):
        assert self.ok(pa, n)
        return self.data[pa - self.base:pa - self.base + n]


def fw272(ram, out, root_arg, regions=True, walk_txt=True):
    """fw272-b2dump.py over ram (refused PAs unmapped). Returns translate() for the chains."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "objects").mkdir(exist_ok=True)
    reg = {n: tuple(ram.meta[n]) for n in ("tables", "shared-l2", "ttbs", "handoff")}

    def readreg(base, size):
        return b"".join(ram.read(base + o, PAGE) if ram.ok(base + o, PAGE) else bytes(PAGE)
                        for o in range(0, size, PAGE))
    if regions:
        for name, (base, size) in reg.items():
            (out / f"{name}.bin").write_bytes(readreg(base, size))
    ttbs = readreg(*reg["ttbs"])
    q = lambda b, o: struct.unpack_from("<Q", b, o)[0]  # noqa: E731
    ttbat = [q(ttbs, i * 8) for i in range(128)]
    ttbr1 = next((w & PA_MASK for w in (ttbat[1], ttbat[0]) if w & 1), reg["tables"][0])

    def table(pa):
        return ram.read(pa, PAGE) if ram.ok(pa, PAGE) else None

    def translate(va):
        t, shifts = ttbr1, (36, 25, 14)
        for lvl, sh in enumerate(shifts):
            tb = table(t)
            if tb is None:
                return None
            idx = ((va & IAS) >> sh) & (0x3f if lvl == 0 else 0x7ff)
            pte = q(tb, idx * 8)
            if not pte & 1:
                return None
            if lvl == 2:
                return (pte & PA_MASK) | (va & (PAGE - 1)), pte & ~PA_MASK
            t = pte & PA_MASK
    leaves = []

    def walk(t, lvl, va):
        tb = table(t)
        if tb is None:
            return
        for idx in range(64 if lvl == 0 else 2048):
            pte = q(tb, idx * 8)
            if not pte & 1:
                continue
            v = va | (idx << (36, 25, 14)[lvl])
            if lvl == 2:
                leaves.append((v, pte & PA_MASK, pte & ~PA_MASK))
            else:
                walk(pte & PA_MASK, lvl + 1, v)
    if walk_txt:
        walk(ttbr1, 0, 0xfffffc0000000000)
        with open(out / "walk.txt", "w") as f:
            f.write(f"ttbat[0..4] {[hex(w) for w in ttbat[:4]]} ttbr1 {ttbr1:#x}; "
                    f"{len(leaves)} leaves\n")
            run = None
            for v, pa, at in leaves + [(None, None, None)]:
                if run and v is not None and at == run[2] and v == run[1] and pa == run[4]:
                    run[1] += PAGE
                    run[4] += PAGE
                    continue
                if run:
                    f.write(f"{run[0]:#x}..{run[1]:#x} pa {run[3]:#x} attrs {run[2]:#018x}\n")
                run = None if v is None else [v, v + PAGE, at, pa, pa + PAGE]
    root = root_arg & 0xfffffffffff if root_arg >> 48 == 0x81 else root_arg
    if root < (1 << 63):
        root |= 0xfffff00000000000

    def readva(va, n):
        data = b""
        while len(data) < n:
            t = translate(va + len(data))
            if not t:
                break
            pa = t[0]
            chunk = min(n - len(data), PAGE - (pa & (PAGE - 1)))
            if not ram.ok(pa, chunk):
                break
            data += ram.read(pa, chunk)
        return data
    seen, rows = {}, []

    def dump(path, va, depth, cap=0x10000):
        if va in seen or len(seen) > 200:
            return
        data = readva(va, cap)
        if not data:
            return
        seen[va] = path
        t = translate(va)
        (out / "objects" / f"{path}.bin").write_bytes(data)
        rows.append(f"{path} va {va:#x} pa {t[0]:#x} attrs {t[1]:#x} bytes {len(data):#x} sha "
                    f"{hashlib.sha256(data).hexdigest()[:12]}")
        if depth == 0:
            return
        for off in range(0, min(len(data), 0x3000) - 7):
            w = q(data, off)
            if w >> 40 == 0xfffffc and translate(w) and w not in seen:
                dump(f"{path}+{off:#x}", w, depth - 1, cap)
    dump("root", root, 2)
    (out / "objects.txt").write_text("\n".join(rows) + "\n")
    return readva


def chain_data(readva, root, steps, length):
    va = root
    for s in steps:
        if s & ADD:
            va += s & ~ADD
        else:
            va = struct.unpack("<Q", readva(va + s, 8))[0]
    return readva(va, length)


def same_tree(a, b):
    for f in sorted(p.relative_to(b) for p in b.rglob("*") if p.is_file()):
        assert (a / f).exists(), f"{f}: missing"
        assert (a / f).read_bytes() == (b / f).read_bytes(), f"{f}: differs"


def run(source, d):
    """Build and run the harness on source; compare everything. Raises on any difference."""
    chains, watches = chains_for_test()
    blob, wblob = hvsnap.config_blobs(chains, watches)
    (d / "chains.bin").write_bytes(blob)
    (d / "watch.bin").write_bytes(wblob)
    exe = d / "snap_host"
    subprocess.check_call(host_cc() + CFLAGS + ["-o", str(exe), str(HARNESS), str(source)])
    subprocess.run([str(exe), str(d), str(d / "chains.bin"), str(d / "watch.bin")], check=True,
                   capture_output=True, timeout=120)
    env = dict(os.environ, PYTHONPATH=str(REPO / "proxyclient"))
    for name, out in (("snapshot.bin", "got"), ("small.bin", "small")):
        subprocess.run([sys.executable, str(DUMP), "--from-bin", str(d / name), "--out",
                        str(d / out)], check=True, capture_output=True, env=env, timeout=120)
    ram = Ram(d)
    root = ram.meta["root"][0]
    readva = fw272(ram, d / "ref", 0x0081000000000000 | (root & 0xfffffffffff))
    got = d / "got"
    same_tree(got, d / "ref")
    for name, steps, length in chains:
        want = chain_data(readva, root, steps, length)
        assert (got / "chains" / f"{name}.bin").read_bytes() == want, f"chain {name}"
    summary = (got / "summary.txt").read_text()
    hw = chain_data(readva, root, (0x18, 0x0), 0x12000)
    for i, off in enumerate((0xfa0, 0xfa8)):
        v = struct.unpack_from("<Q", hw, off)[0]
        assert f"watch {i}: hwdata +{off:#x} (8 bytes) = {v:#x}" in summary, summary
    # The later kick: objects from its root, chains from the InitData root, nothing else.
    fw272(ram, d / "ref-kick", ram.meta["kick_root"][0], regions=False, walk_txt=False)
    kick = got / "kick-1"
    assert not (kick / "walk.txt").exists() and not (kick / "tables.bin").exists()
    assert (kick / "objects.txt").read_text() == (d / "ref-kick" / "objects.txt").read_text()
    st0 = (got / "chains" / "devctl-state.bin").read_bytes()
    st1 = (kick / "chains" / "devctl-state.bin").read_bytes()
    assert struct.unpack_from("<I", st0, 0x20)[0] == 7 and struct.unpack_from("<I", st1, 0x20)[0] == 8
    assert "2 snapshots" in summary and "1 not taken" in summary
    small = (d / "small" / "summary.txt").read_text()
    assert "FULL" in small and "buffer full" in small, small
    return summary


def test_snapshot_is_fw272s_and_never_reads_a_refused_pa():
    d = Path(tempfile.mkdtemp())
    try:
        summary = run(SRC, d)
        assert "reads refused" in summary and "1 tables refused" in summary
    finally:
        shutil.rmtree(d, ignore_errors=True)


MUTATIONS = {
    "a table's PA isn't checked": [("    return snap_ok(pa, PAGE) ? sptm_virt_snap_pa(pa) : NULL;",
                                    "    return sptm_virt_snap_pa(pa);")],
    "a page's PA isn't checked": [("        if (!snap_ok(pa, chunk))\n            break;\n", "")],
    "a region page isn't checked": [("            if (snap_ok(pa + o, n)) {", "            if (1) {")],
    "three levels of objects": [("    if (!depth)\n        return;\n    u64 lim", "    u64 lim")],
    "199 objects": [("snap_seen(va) || sn.nseen > MAX_SEEN", "snap_seen(va) || sn.nseen >= MAX_SEEN")],
    "the scan's last word skipped": [("o + 8 <= lim; o++", "o + 8 < lim; o++")],
    "pointers only at aligned offsets": [("o + 8 <= lim; o++", "o + 8 <= lim; o += 8")],
    "objects past 64 KiB": [("    u64 cap = min(OBJ_CAP, snap_room());", "    u64 cap = snap_room();")],
    "runs merged across a PA gap": [(" && run[2] + (run[1] - run[0]) == pa", "")],
    "the root as the raw message": [("    return r < BIT(63) ? r | 0xfffff00000000000UL : r;",
                                     "    return v;")],
    "TTBR0 where TTBR1 is valid": [("    if (ttbat[1] & 1)\n", "    if (0)\n")],
    "a record past the buffer": [("    if (sn.cap < h->used + keep) {", "    if (0) {")],
    "an offset step read as a pointer": [("            if (st & SNAP_STEP_ADD) {", "            if (0) {")],
    "a kick's chains from its own root": [("        snap_chains(sn.initdata);", "        snap_chains(root);")],
    "a watched word from the wrong place": [(
        "memcpy(&v, sn.chain_data[ci] + w->off, n);", "memcpy(&v, sn.chain_data[ci], n);")],
    "a later kick takes everything": [("    u32 parts = first ? ~0U : sptm_virt_snap_parts;",
                                       "    u32 parts = ~0U;")],
    "more than the budget": [("h->nsnap >= sptm_virt_snap_max", "h->nsnap > sptm_virt_snap_max")],
}


def _mutant(args):
    name, edits = args
    m = SRC.read_text()
    for a, b in edits:
        assert m.count(a) == 1, f"{name}: the checked code changed"
        m = m.replace(a, b, 1)
    d = Path(tempfile.mkdtemp())
    try:
        (d / "m.c").write_text(m)
        try:
            run(d / "m.c", d)
        except (AssertionError, subprocess.CalledProcessError, subprocess.TimeoutExpired,
                FileNotFoundError, ValueError, struct.error):
            return name, True
        return name, False
    finally:
        shutil.rmtree(d, ignore_errors=True)


def test_every_rule_is_noticed_when_broken():
    # Each run holds a 32 MiB RAM image a few times over: four at once stay well inside the
    # suite's memory scope.
    with concurrent.futures.ThreadPoolExecutor(4) as pool:
        results = list(pool.map(_mutant, MUTATIONS.items()))
    missed = [n for n, caught in results if not caught]
    assert not missed, f"not noticed: {missed}"


def test_mailbox_log_decoding():
    rec = hvsnap.MBOX.pack
    raw = (rec(10, 0x00810c2000100000, 0x20, 0 | 0 << 8, 1)            # A2I, both words, EP 0x20
           + rec(11, 0x0083000000000011, 0x21, 0 | 3 << 8, 1)          # A2I kick on EP 0x21
           + rec(12, 0x0042000000000001, 0, 1 | 1 << 2 | 5 << 8, 1)    # I2A first half, cpu5
           + rec(13, 0x0042000000000002, 0, 1 | 1 << 2 | 6 << 8, 1)    # I2A first half, cpu6
           + rec(14, 0, 0x20, 1 | 2 << 2 | 5 << 8, 1)                  # cpu5's second half
           + rec(15, 0, 0x21, 1 | 2 << 2 | 6 << 8, 1))                 # cpu6's second half
    lines = hvsnap.mbox_lines(raw)
    assert len(lines) == 4, lines
    assert lines[0].startswith("   0 ") and "A2I asc0 ep 0x20 0x00810c2000100000" in lines[0]
    assert lines[1].startswith(">> 1 ") and "ep 0x21 0x0083000000000011 kick" in lines[1]
    assert "c5 I2A asc0 ep 0x20 0x0042000000000001" in lines[2] and lines[2].startswith("  ")
    assert "c6 I2A asc0 ep 0x21 0x0042000000000002" in lines[3] and lines[3].startswith(">>")
    # A first half whose second never came (the log ended between them, or the core read again)
    # is still a message: it isn't dropped, and a later first half doesn't overwrite it.
    raw2 = (rec(20, 0x0042000000000003, 0, 1 | 1 << 2 | 5 << 8, 1)
            + rec(21, 0x0042000000000004, 0, 1 | 1 << 2 | 5 << 8, 1))
    lines2 = hvsnap.mbox_lines(raw2)
    assert len(lines2) == 2 and "0x0042000000000003" in lines2[0], lines2
    assert "0x0042000000000004" in lines2[1] and "first word only" in lines2[1], lines2
