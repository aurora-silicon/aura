# SPDX-License-Identifier: MIT
"""The block and IRQ traces from the host (src/sptm_virt_trace.[ch]): what run_guest_sptm.py
checks and sets before entry (AURA_TRACE_BLKS, AURA_IRQ_TRACE), how a park reads both rings and
drains them while the guest is stopped, and the decoding: per block the distinct (offset, width,
R/W) set with counts and values, the IRQs by number (named from the ADT), and a timeline of both.

A capture directory holds everything one boot's parks read, in order:
  blk.bin, irq.bin      the records, 32 bytes each, in ring order, appended at each read
  hdr-blk.bin, ...      each ring's header at the last read
  reads.jsonl           one line per ring per read: the indices read, head, tail, lost
Read-only on the device, apart from the drain (sptm_virt_trace_drain), which frees only records
already saved here."""

import collections
import json
import os
import pathlib
import re
import struct
import time

MAGIC = 0x3143525441525541  # "AURATRC1"
HDR_BYTES, REC_BYTES = 1024, 32
KIND_BLK, KIND_IRQ = 1, 2
IRQ_EVENT, IRQ_OTHER = 0, 1
BLKS, RNGS, PAGES = 8, 16, 32
PAGE = 0x4000
RINGS = (("blk", "sptm_virt_trace_blk"), ("irq", "sptm_virt_trace_irq"))
HDR = struct.Struct("<QIIIIQQQQQQQQQIIIIII8Q8Q8Q" + "QII" * RNGS + "128s41Q")
CFG = struct.Struct("<6I" + "QII" * RNGS + "128s")
REC = struct.Struct("<4Q")
assert HDR.size == HDR_BYTES and CFG.size == 408
AIC_TYPES = {1: "hw", 4: "ipi"}
DEFAULT_BLK, DEFAULT_IRQ, MAX_RECS = 1 << 19, 1 << 18, 1 << 21
NAME = re.compile(r"[A-Za-z0-9_.-]{1,15}$")

Rec = collections.namedtuple("Rec", "index t val pc pa width write cpu blk gl hi")
Access = collections.namedtuple("Access", "index t cpu pa width write val hi pc gl blk")


# ---- the block list ------------------------------------------------------------------------------

def parse_blks(spec):
    """AURA_TRACE_BLKS: <pa>:<len>[:name],... -> ([(pa, len, block index)], [names]). Entries
    with one name are one block (its offsets from its lowest page); an entry without a name is a
    block of its own. Raises ValueError with what is wrong; the rules are aura's
    (sptm_virt_trace_check)."""
    rngs, names = [], []
    for i, ent in enumerate(x for x in spec.split(",") if x.strip()):
        f = ent.strip().split(":")
        if len(f) not in (2, 3):
            raise ValueError(f"{ent!r}: <pa>:<len>[:name]")
        try:
            pa, length = int(f[0], 0), int(f[1], 0)
        except ValueError:
            raise ValueError(f"{ent!r}: <pa>:<len>[:name]") from None
        name = f[2] if len(f) == 3 and f[2] else f"blk{i}"
        if not NAME.match(name):
            raise ValueError(f"{ent!r}: a name is 1-15 of A-Z a-z 0-9 _ . -")
        if name not in names:
            names.append(name)
        rngs.append((pa, length, names.index(name)))
    check_rngs(rngs, len(names))
    return rngs, names


def check_rngs(rngs, n_blk):
    if not 1 <= len(rngs) <= RNGS or not 1 <= n_blk <= BLKS:
        raise ValueError(f"1-{RNGS} ranges in 1-{BLKS} blocks ({len(rngs)} in {n_blk})")
    pages = set()
    for i, (pa, length, blk) in enumerate(rngs):
        if not length or (pa | length) % 4 or pa >> 48 or length >> 32 or blk >= n_blk:
            raise ValueError(f"0x{pa:x}:0x{length:x}: not 4-byte aligned below 2^48")
        for pa2, len2, _ in rngs[:i]:
            if pa < pa2 + len2 and pa2 < pa + length:
                raise ValueError(f"0x{pa:x}:0x{length:x} overlaps 0x{pa2:x}:0x{len2:x}")
        # A sub-page range traps its whole page and records only its own bytes.
        pages |= set(range(pa & ~(PAGE - 1), pa + length, PAGE))
    if len(pages) > PAGES:
        raise ValueError(f"{len(pages)} distinct pages: at most {PAGES}")
    if {b for *_, b in rngs} != set(range(n_blk)):
        raise ValueError("a block with no range")


def _int(env, name, lo, hi):
    v = env.get(name)
    if not v:
        return 0
    try:
        n = int(v, 0)
    except ValueError:
        raise ValueError(f"{name}={v}: a number") from None
    if not lo <= n <= hi:
        raise ValueError(f"{name}={v}: {lo}..{hi}")
    return n


def config(env):
    """The struct hvtrace_cfg bytes for env, or None if neither trace is asked for. Raises
    ValueError."""
    irq = env.get("AURA_IRQ_TRACE", "0")
    if irq not in ("0", "1"):
        raise ValueError(f"AURA_IRQ_TRACE={irq}: 0 or 1")
    spec = env.get("AURA_TRACE_BLKS", "")
    rngs, names = parse_blks(spec) if spec.strip() else ([], [])
    blk_max = _int(env, "AURA_TRACE_MAX", 2, MAX_RECS)
    irq_max = _int(env, "AURA_IRQ_TRACE_MAX", 2, MAX_RECS)
    print_max = _int(env, "AURA_TRACE_PRINT", 0, 1 << 20)
    if not rngs and irq == "0":
        for var in ("AURA_TRACE_MAX", "AURA_TRACE_PRINT", "AURA_IRQ_TRACE_MAX"):
            if env.get(var):
                raise ValueError(f"{var} without AURA_TRACE_BLKS or AURA_IRQ_TRACE=1")
        return None
    flat = []
    for i in range(RNGS):
        flat += list(rngs[i]) if i < len(rngs) else [0, 0, 0]
    blob = b"".join(n.encode().ljust(16, b"\0") for n in names).ljust(128, b"\0")
    return CFG.pack(len(rngs), len(names), blk_max, irq_max, int(irq), print_max, *flat, blob)


def check_env(env):
    """'' or why run_guest_sptm must refuse the traces' settings, before anything is sent."""
    try:
        config(env)
    except ValueError as e:
        return f"AURA_TRACE_BLKS/AURA_IRQ_TRACE: {e}"
    return ""


def _regs(node):
    out = []
    for i in range(len(getattr(node, "reg", []) or [])):
        try:
            out.append(node.get_reg(i))
        except Exception:  # a bus child (SPMI, I2C): no MMIO reg
            pass
    return out


def check_link(tree, port_nodes, env):
    """'' or why a range is refused: it overlaps a reg of the debug link's nodes (port_nodes,
    run_guest_sptm's debug_port: what XNU never sees), or of the /arm-io bus a link node sits on
    (its port controller's). tree: aura.adt's parsed ADT."""
    spec = env.get("AURA_TRACE_BLKS", "")
    if not spec.strip():
        return ""
    rngs, _ = parse_blks(spec)
    devs = set()
    for path in port_nodes:
        parts = path.strip("/").split("/")
        devs.add(path)
        if len(parts) > 2:
            devs.add("/" + "/".join(parts[:2]))
    for path in sorted(devs):
        try:
            node = tree[path]
        except Exception:
            continue
        for a, sz in _regs(node):
            for pa, length, _ in rngs:
                if pa < a + sz and a < pa + length:
                    return (f"AURA_TRACE_BLKS: 0x{pa:x}:0x{length:x} overlaps {path} "
                            f"(0x{a:x}+0x{sz:x}), the debug link's: never hooked")
    return ""


def configure(p, sym, env):
    """run_guest_sptm.py: the traces' configuration into aura before entry (nothing if unset)."""
    cfg = config(env)
    if cfg is None:
        return False
    p.iface.writemem(sym["sptm_virt_trace_cfg"], cfg)
    n_rng, n_blk, blk_max, irq_max, irq, pmax = struct.unpack_from("<6I", cfg)
    if n_rng:
        rngs, names = parse_blks(env["AURA_TRACE_BLKS"])
        for pa, length, b in rngs:
            print(f"trace: block {names[b]}: 0x{pa:x}+0x{length:x}")
        print(f"trace: {n_rng} ranges in {n_blk} blocks, {blk_max or DEFAULT_BLK} records"
              + (f", the first {pmax} of each printed" if pmax else ""))
    if irq:
        print(f"trace: IRQs through the AIC's event register, {irq_max or DEFAULT_IRQ} records")
    return True


# ---- the rings -----------------------------------------------------------------------------------

def parse_hdr(raw):
    v = HDR.unpack_from(raw, 0)
    keys = ("magic", "version", "hdr_bytes", "rec_bytes", "kind", "cap", "head", "tail", "lost",
            "drains", "cntfrq", "t_setup", "irq_event", "irq_zero", "irq_max", "n_blk", "n_rng",
            "print_max", "full")
    h = dict(zip(keys, v[:19]))
    if h["magic"] != MAGIC or h["hdr_bytes"] != HDR_BYTES or h["rec_bytes"] != REC_BYTES:
        raise ValueError("not a trace ring (magic, header or record size)")
    h["blk_seen"] = list(v[20:28])
    h["blk_lost"] = list(v[28:36])
    h["blk_base"] = list(v[36:44])
    r = v[44:44 + 3 * RNGS]
    h["rng"] = [(r[3 * i], r[3 * i + 1], r[3 * i + 2]) for i in range(h["n_rng"])]
    names = v[44 + 3 * RNGS]
    h["name"] = [names[16 * i:16 * i + 16].split(b"\0")[0].decode("latin1")
                 for i in range(h["n_blk"])]
    return h


def unpack(raw, first):
    """Records from consecutive ring indices first.. (a slot without a time ends them)."""
    out = []
    for k in range(len(raw) // REC_BYTES):
        t, val, pc, meta = REC.unpack_from(raw, REC_BYTES * k)
        if not t:
            break
        out.append(Rec(first + k, t, val, pc, meta & 0xffffffffffff, meta >> 48 & 7,
                       bool(meta >> 51 & 1), meta >> 52 & 31, meta >> 57 & 15,
                       bool(meta >> 61 & 1), bool(meta >> 62 & 1)))
    return out


def ring_dump(raw):
    """A whole ring as it sits in memory (the header, then cap slots): (header, its undrained
    records [tail, head) up to the first one still being written)."""
    h = parse_hdr(raw)
    out = []
    for i in range(h["tail"], h["head"]):
        off = HDR_BYTES + REC_BYTES * (i % h["cap"])
        rec = unpack(raw[off:off + REC_BYTES], i)
        if not rec:
            break
        out += rec
    return h, out


def read_ring(readmem, ptr, start, max_recs, chunk=1 << 20):
    """At a park: (header, the first index read, the complete records' bytes) of [max(start,
    tail), head), at most max_recs, up to the first slot still being written."""
    h = parse_hdr(readmem(ptr, HDR_BYTES))
    first = max(start, h["tail"])
    n = min(max(0, h["head"] - first), max_recs)
    cap = h["cap"]
    raw = bytearray()
    i = first
    while i < first + n:
        slot = i % cap
        k = min(first + n - i, cap - slot, chunk // REC_BYTES)
        raw += readmem(ptr + HDR_BYTES + REC_BYTES * slot, REC_BYTES * k)
        i += k
    good = len(unpack(bytes(raw), first))
    return h, first, bytes(raw[:REC_BYTES * good])


def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v


class Capture:
    """A capture directory (see the module's docstring)."""

    def __init__(self, out):
        self.out = pathlib.Path(out)
        self.out.mkdir(parents=True, exist_ok=True)

    def reads(self):
        p = self.out / "reads.jsonl"
        if not p.is_file():
            return []
        return [json.loads(line) for line in p.read_text().splitlines() if line.strip()]

    def next_index(self, ring):
        done = [r for r in self.reads() if r["ring"] == ring]
        return done[-1]["upto"] if done else 0

    def save(self, ring, hdr_raw, first, raw, h):
        with open(self.out / f"{ring}.bin", "ab") as f:
            f.write(raw)
            f.flush()
            os.fsync(f.fileno())
        (self.out / f"hdr-{ring}.bin").write_bytes(hdr_raw)
        n = len(raw) // REC_BYTES
        line = {"ring": ring, "time": time.time(), "from": first, "upto": first + n,
                "expected": self.next_index(ring), "head": h["head"], "tail": h["tail"],
                "lost": h["lost"], "irq_zero": h["irq_zero"], "drained": False}
        with open(self.out / "reads.jsonl", "a") as f:
            f.write(json.dumps(line) + "\n")
            f.flush()
            os.fsync(f.fileno())
        return line

    def mark_drained(self, ring):
        lines = self.reads()
        for line in reversed(lines):
            if line["ring"] == ring:
                line["drained"] = True
                break
        (self.out / "reads.jsonl").write_text("".join(json.dumps(x) + "\n" for x in lines))


def capture(readmem, call, sym, out, drain=True, max_bytes=8 << 20, log=print):
    """At a park: read each ring's new records into the capture directory out, then (drain)
    free them in aura. Returns {ring: (records read, header)}."""
    cap = Capture(out)
    got = {}
    for ring, symbol in RINGS:
        if symbol not in sym:
            continue
        ptr = struct.unpack("<Q", readmem(sym[symbol], 8))[0]
        if not ptr:
            continue
        hdr_raw = readmem(ptr, HDR_BYTES)
        now, start = parse_hdr(hdr_raw), cap.next_index(ring)
        saved = cap.out / f"hdr-{ring}.bin"
        if saved.is_file() and parse_hdr(saved.read_bytes())["t_setup"] != now["t_setup"]:
            raise SystemExit(f"{cap.out} holds another boot's {ring} records: a new --out")
        if start > now["head"]:
            raise SystemExit(f"{cap.out} has {ring} records past this ring's head: a new --out")
        h, first, raw = read_ring(readmem, ptr, start, max_bytes // REC_BYTES)
        line = cap.save(ring, hdr_raw, first, raw, h)
        n = len(raw) // REC_BYTES
        left = h["head"] - first - n
        log(f"{ring}: read [{first}, {first + n}) of head {h['head']}"
            + (f" ({left} left for the next read)" if left else "")
            + f"; lost so far {h['lost']}"
            + (f"; GAP: {first - line['expected']} records drained before this read"
               if first > line["expected"] else ""))
        # Everything below first + n is saved (an earlier read without a drain saved what is
        # below first).
        if drain and first + n > h["tail"]:
            rc = s32(call(sym["sptm_virt_trace_drain"], ptr, first + n))
            if rc:
                raise SystemExit(f"{ring}: the drain of [{first}, {first + n}) was refused ({rc}); "
                                 f"the records are saved")
            cap.mark_drained(ring)
        got[ring] = (n, h)
    return got


# ---- decoding ------------------------------------------------------------------------------------

def load(out):
    """A capture directory: {ring: (header, records in ring order, gaps [(after index, lost)])}."""
    out = pathlib.Path(out)
    res = {}
    reads = Capture(out).reads()
    for ring, _ in RINGS:
        if not (out / f"hdr-{ring}.bin").is_file():
            continue
        h = parse_hdr((out / f"hdr-{ring}.bin").read_bytes())
        raw = (out / f"{ring}.bin").read_bytes() if (out / f"{ring}.bin").is_file() else b""
        recs, gaps, off = [], [], 0
        for line in (r for r in reads if r["ring"] == ring):
            n = line["upto"] - line["from"]
            chunk = raw[off:off + REC_BYTES * n]
            off += REC_BYTES * n
            if line["from"] > line["expected"]:
                gaps.append((line["expected"], line["from"] - line["expected"]))
            recs += unpack(chunk, line["from"])
        res[ring] = (h, recs, gaps)
    return res


def accesses(recs):
    """Records -> accesses: a 128-bit access's two records (consecutive indices) become one."""
    out, at = [], {}
    for r in recs:
        if r.hi:
            k = at.get(r.index - 1)
            if k is not None and out[k].cpu == r.cpu and out[k].hi is None:
                out[k] = out[k]._replace(hi=r.val)
                continue
            out.append(Access(r.index, r.t, r.cpu, r.pa - 8, r.width, r.write, None, r.val, r.pc,
                              r.gl, r.blk))
            continue
        at[r.index] = len(out)
        out.append(Access(r.index, r.t, r.cpu, r.pa, r.width, r.write, r.val, None, r.pc, r.gl,
                          r.blk))
    return out


def who(a):
    return "SPTM" if a.gl else "XNU"


def bits(width):
    return 8 << width


def aic_event(val, irq_max):
    """An AIC event value: (type, die, number, flat number)."""
    die, typ, num = val >> 24 & 0xff, val >> 16 & 0xff, val & 0xffff
    return typ, die, num, die * irq_max + num


def irq_names(tree):
    """{flat AIC IRQ: [ADT node paths]} from the nodes' interrupts with the AIC as their parent."""
    try:
        aic = tree["/arm-io/aic"]
    except Exception:
        return {}
    ph = getattr(aic, "AAPL,phandle", None)
    out = collections.defaultdict(list)

    def u32s(v):
        if isinstance(v, int):
            return [v]
        if isinstance(v, (bytes, bytearray)):
            return list(struct.unpack(f"<{len(v) // 4}I", bytes(v[:len(v) // 4 * 4])))
        try:
            return [int(x) for x in v]
        except TypeError:
            return []

    def walk(n, path):
        here = f"{path}/{n.name}"
        props = getattr(n, "_properties", {})
        if "interrupts" in props:
            irqs = u32s(getattr(n, "interrupts"))
            par = u32s(getattr(n, "interrupt-parent")) if "interrupt-parent" in props else []
            for i, irq in enumerate(irqs):
                p = par[i] if i < len(par) else (par[0] if len(par) == 1 else ph)
                if p == ph:
                    out[irq].append(here)
        for c in n:
            walk(c, here)

    for c in tree:
        walk(c, "")
    return out


class Decoded:
    def __init__(self, data):
        self.blk = data.get("blk")
        self.irq = data.get("irq")
        self.t0 = min((x[0]["t_setup"] for x in data.values()), default=0)
        self.frq = next((x[0]["cntfrq"] for x in data.values()), 24000000) or 24000000
        self.blk_acc = accesses(self.blk[1]) if self.blk else []
        self.irq_acc = accesses(self.irq[1]) if self.irq else []

    def secs(self, t):
        return (t - self.t0) / self.frq

    def off(self, a):
        """A block access's block name and offset from its lowest page."""
        h = self.blk[0]
        b = a.blk
        if b < h["n_blk"]:
            return h["name"][b], a.pa - h["blk_base"][b]
        return "?", a.pa


def _vals(vs, most=6):
    seen = list(dict.fromkeys(vs))
    s = " ".join(f"0x{v:x}" for v in seen[:most])
    return s + (f" ... ({len(seen)} distinct)" if len(seen) > most else "")


def blocks_summary(d):
    if not d.blk:
        return ["blocks: no block trace in this capture"]
    h, recs, gaps = d.blk
    out = [f"blocks: {len(d.blk_acc)} accesses recorded ({len(recs)} records); the ring: cap "
           f"{h['cap']}, head {h['head']}, tail {h['tail']}, LOST {h['lost']}, "
           f"{h['drains']} drains"]
    for at, n in gaps:
        out.append(f"  GAP: {n} records from index {at} were drained but aren't in this capture")
    for b in range(h["n_blk"]):
        rng = ", ".join(f"0x{pa:x}+0x{ln:x}" for pa, ln, bb in h["rng"] if bb == b)
        acc = [a for a in d.blk_acc if a.blk == b]
        out.append("")
        out.append(f"{h['name'][b]} (base 0x{h['blk_base'][b]:x}: {rng}): {h['blk_seen'][b]} "
                   f"accesses seen, {h['blk_lost'][b]} lost, {len(acc)} here")
        if acc:
            out.append(f"  first {d.secs(acc[0].t):.6f} s, last {d.secs(acc[-1].t):.6f} s")
        sets = collections.OrderedDict()
        for a in sorted(acc, key=lambda a: (d.off(a)[1], a.width, a.write)):
            sets.setdefault((d.off(a)[1], a.width, a.write), []).append(a)
        out.append("  offset   width R/W  count   XNU  SPTM  values (in order, distinct)")
        for (off, width, write), g in sets.items():
            g.sort(key=lambda a: (a.t, a.index))
            whos = collections.Counter(who(a) for a in g)
            vals = [a.val if a.hi is None else (a.hi << 64 | (a.val or 0)) for a in g]
            out.append(f"  +0x{off:05x} {bits(width):5d}  {'W' if write else 'R'}  {len(g):6d} "
                       f"{whos['XNU']:5d} {whos['SPTM']:5d}  {_vals(vals)}")
    return out


def irq_summary(d, names=None, want=()):
    if not d.irq:
        return ["IRQs: no IRQ trace in this capture"]
    h, recs, gaps = d.irq
    ev = [a for a in d.irq_acc if a.blk == IRQ_EVENT]
    other = [a for a in d.irq_acc if a.blk != IRQ_EVENT]
    out = [f"IRQs: {len(ev)} taken through the AIC event register 0x{h['irq_event']:x}; "
           f"{h['irq_zero']} of its reads returned 0 (none pending); {len(other)} other accesses "
           f"on its page; the ring: cap {h['cap']}, head {h['head']}, LOST {h['lost']}"]
    for at, n in gaps:
        out.append(f"  GAP: {n} records from index {at} were drained but aren't in this capture")
    by = collections.OrderedDict()
    for a in ev:
        typ, die, num, flat = aic_event(a.val, h["irq_max"])
        by.setdefault((typ, flat), []).append((a, die, num))
    out.append("  type  irq   die:num  count  first s       last s        cpus  ADT")
    for (typ, flat), g in sorted(by.items()):
        cpus = "".join(sorted({f"{x[0].cpu:x}" for x in g}))
        nm = " ".join((names or {}).get(flat, [])) if typ == 1 else ""
        mark = "  <<<" if flat in want else ""
        out.append(f"  {AIC_TYPES.get(typ, f'0x{typ:x}'):4s} {flat:5d}  {g[0][1]}:{g[0][2]:<5d} "
                   f"{len(g):6d}  {d.secs(g[0][0].t):12.6f}  {d.secs(g[-1][0].t):12.6f}  "
                   f"{cpus:5s} {nm}{mark}")
    for w in want:
        if (1, w) not in by:
            out.append(f"  hw   {w:5d}  not taken in this capture  <<<")
    if other:
        sets = collections.Counter((a.pa - h["rng"][0][0], a.width, a.write) for a in other)
        out.append("  the event page's other accesses (offset, width, R/W: count):")
        for (off, width, write), n in sorted(sets.items()):
            out.append(f"    +0x{off:04x} {bits(width)} {'W' if write else 'R'}: {n}")
    return out


def timeline(d, want_irqs=None, want_blks=None, names=None):
    """Both traces merged in time: block accesses and IRQs (optionally only some of each)."""
    rows = []
    for a in d.blk_acc:
        name, off = d.off(a)
        if want_blks and name not in want_blks:
            continue
        v = f"0x{a.val:x}" if a.hi is None else f"0x{a.hi:016x}{(a.val or 0):016x}"
        rows.append((a.t, 0, a.index, f"{d.secs(a.t):12.6f} c{a.cpu:<2d} {name:10s} "
                     f"{'W' if a.write else 'R'}{bits(a.width):<3d} +0x{off:05x} = {v}  "
                     f"pc 0x{a.pc:x} {who(a)}"))
    if d.irq:
        h = d.irq[0]
        for a in d.irq_acc:
            if a.blk == IRQ_EVENT:
                typ, die, num, flat = aic_event(a.val, h["irq_max"])
                if want_irqs and flat not in want_irqs:
                    continue
                nm = " ".join((names or {}).get(flat, [])) if typ == 1 else ""
                kind = AIC_TYPES.get(typ, hex(typ))
                rows.append((a.t, 1, a.index, f"{d.secs(a.t):12.6f} c{a.cpu:<2d} IRQ {kind} "
                             f"{flat} (die {die} num {num}, event 0x{a.val:x})  pc 0x{a.pc:x}"
                             + (f"  {nm}" if nm else "")))
            elif not want_irqs:
                rows.append((a.t, 1, a.index, f"{d.secs(a.t):12.6f} c{a.cpu:<2d} aic-page   "
                             f"{'W' if a.write else 'R'}{bits(a.width):<3d} "
                             f"+0x{a.pa - h['rng'][0][0]:05x} = 0x{a.val:x}  pc 0x{a.pc:x}"))
    rows.sort()
    return ["# seconds from aura's setup, core, block or IRQ, access/event, guest PC"] + \
        [r[3] for r in rows]


def report(out, names=None, want_irqs=(), want_blks=None):
    """Decode a capture directory into summary.txt and timeline.txt; returns the summary."""
    out = pathlib.Path(out)
    d = Decoded(load(out))
    text = "\n".join(blocks_summary(d) + [""] + irq_summary(d, names, want_irqs)) + "\n"
    (out / "summary.txt").write_text(text)
    (out / "timeline.txt").write_text(
        "\n".join(timeline(d, set(want_irqs) or None, want_blks, names)) + "\n")
    return text
