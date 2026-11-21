# SPDX-License-Identifier: MIT
"""The GPU/PMP record from the host (src/sptm_virt_gfxrec.[ch]): what run_guest_sptm.py sets before
entry, how a park reads it back with the MMIO ring (sptm_virt_gpu.c), and its decoding: mailbox
messages (a message read or written as two 64-bit halves paired per core), RTKit management, the
GPU firmware's endpoints 0x20/0x21, the PMP's endpoint 0x20 (its 0x0037000000000008 and
0x0020f50100000000 called out), and the register writes. The MMIO_PROTECTED block's record is
apart: every access, who made it (SPTM or XNU), offsets and values. Read-only on the device."""

import collections
import struct

MAGIC = 0x3152504741525541  # "AURAGPR1"
HDR_BYTES, REC_BYTES = 512, 32
HDR = struct.Struct("<QIIIIQQQQQQQQQQII4Q4Q8I8Q128sII13Q")
REC = struct.Struct("<4Q")
OFF, ARMED, ON, DONE_TIME, DONE_FULL = range(5)
STATES = {OFF: "OFF", ARMED: "ARMED (no doorbell yet)", ON: "ON (recording)",
          DONE_TIME: "DONE_TIME (ended by time)", DONE_FULL: "DONE_FULL (the buffer filled)"}
SRC_NAMES = ("gfx", "gfx1", "pmp", "sgx", "pmgr")
SRC_GFX, SRC_GFX1, SRC_PMP, SRC_SGX, SRC_PMGR = range(5)
A2I, I2A = 0x8800, 0x8830
PMP_CONFIRM_8 = 0x0037000000000008
PMP_AGX_VOTE = 0x0020f50100000000
PATTERNS = {PMP_CONFIRM_8: "PMP CONFIRM_IOREG 8",
            PMP_AGX_VOTE: "AGX power vote: devpower 0x0f dev 5 on"}
assert HDR.size == HDR_BYTES

# ---- the buffer ---------------------------------------------------------------------------------

Rec = collections.namedtuple("Rec", "index t val hi pa src write width cpu")


class Message:
    """A mailbox message (kind "mbox") or a register write (kind "reg")."""

    def __init__(self, kind, t, cpu, src, write, pa, msg=None, hi=None, width=3, halves=2,
                 index=0):
        self.kind, self.t, self.cpu, self.src_i, self.write, self.pa = kind, t, cpu, src, write, pa
        self.msg, self.hi, self.width, self.halves, self.index = msg, hi, width, halves, index
        self.trigger = False

    @property
    def src(self):
        return SRC_NAMES[self.src_i]

    @property
    def dir(self):
        return "A2I" if self.write else "I2A"

    @property
    def ep(self):
        return None if self.hi is None else self.hi & 0xff


def parse_hdr(raw):
    v = HDR.unpack_from(raw, 0)
    h = dict(zip(("magic", "version", "hdr_bytes", "rec_bytes", "state", "cap", "claimed",
                  "t_start", "t_end", "t_done", "cntfrq", "window_ms", "trig_val", "trig_hi",
                  "trig_pc", "trig_cpu", "n_ps"), v[:17]))
    h["base"], h["size"] = list(v[17:21]), list(v[21:25])
    w = v[25:33]
    h["sgx_w"] = [(w[2 * i], w[2 * i + 1]) for i in range(4) if w[2 * i + 1]]
    h["ps"] = list(v[33:33 + h["n_ps"]])
    names = v[41]
    h["ps_name"] = [names[16 * i:16 * i + 16].split(b"\0")[0].decode("latin1")
                    for i in range(h["n_ps"])]
    h["ps_trapped"] = v[42]
    if h["magic"] != MAGIC or h["hdr_bytes"] != HDR_BYTES or h["rec_bytes"] != REC_BYTES:
        raise ValueError("not a GPU/PMP record (magic, header or record size)")
    return h


class Record:
    def __init__(self, hdr, recs, incomplete):
        self.hdr, self.recs, self.incomplete = hdr, recs, incomplete
        self._msgs = None

    def messages(self):
        if self._msgs is None:
            self._msgs = mark_trigger(pair(self.recs, self.hdr), self.hdr)
        return self._msgs


def parse(raw):
    h = parse_hdr(raw)
    n = min(h["claimed"], h["cap"], (len(raw) - HDR_BYTES) // REC_BYTES)
    recs, incomplete = [], 0
    for i in range(n):
        t, val, hi, meta = REC.unpack_from(raw, HDR_BYTES + REC_BYTES * i)
        if not t:
            incomplete += 1
            continue
        recs.append(Rec(i, t, val, hi, meta & 0xffffffffffff, (meta >> 48) & 15,
                        bool((meta >> 52) & 1), (meta >> 53) & 7, (meta >> 56) & 15))
    return Record(h, recs, incomplete)


# ---- the record's rules, for the MMIO ring (as src/sptm_virt_gfxrec.c) ---------------------------

def classify(h, pa, write, width):
    """The record's source for an access, or None if the record doesn't take it."""
    for s in range(4):
        if h["size"][s] and h["base"][s] <= pa < h["base"][s] + h["size"][s]:
            off = pa - h["base"][s]
            if s <= SRC_PMP:
                box = A2I if write else I2A
                return s if box <= off < box + 16 else None
            if write and any(o <= off < o + n for o, n in h["sgx_w"]):
                return s
            return None
    if write and any(pa < p + 4 and pa + (1 << width) > p for p in h["ps"]):
        return SRC_PMGR
    return None


def ring_messages(raw, h):
    """The MMIO ring's records (struct gpu_rec: t, pa [47:0] | width [50:48] | write [51] |
    cpu [55:52], value, pc), oldest first, that the record's rules take, as messages."""
    recs = []
    for i in range(len(raw) // 32):
        t, meta, val, _pc = struct.unpack_from("<4Q", raw, 32 * i)
        if not t:
            continue
        pa, width, write, cpu = meta & 0xffffffffffff, (meta >> 48) & 7, bool((meta >> 51) & 1), \
            (meta >> 52) & 15
        src = classify(h, pa, write, width)
        if src is not None:
            recs.append(Rec(i, t, val, 0, pa, src, write, width, cpu))
    return pair(recs, h)


# ---- messages -----------------------------------------------------------------------------------

def pair(recs, h):
    """Records -> messages in time order: a mailbox access of both words is one message; two
    64-bit halves (+0 then +8) on one core are one, timed at the second; a half on its own stays
    one, its other word unknown."""
    out, pend = [], {}
    for r in sorted(recs, key=lambda r: (r.t, r.index)):
        if r.src > SRC_PMP:
            out.append(Message("reg", r.t, r.cpu, r.src, r.write, r.pa, msg=r.val, width=r.width,
                               halves=0, index=r.index))
            continue
        off = (r.pa - h["base"][r.src]) & 0xf
        if r.width == 4 and off == 0:
            out.append(Message("mbox", r.t, r.cpu, r.src, r.write, r.pa, r.val, r.hi, 4, 2,
                               r.index))
            continue
        key = (r.src, r.write, r.cpu)
        if r.width == 3 and off == 0:
            if key in pend:
                out.append(pend.pop(key))
            pend[key] = Message("mbox", r.t, r.cpu, r.src, r.write, r.pa, r.val, None, 3, 1,
                                r.index)
            continue
        if r.width == 3 and off == 8:
            first = pend.pop(key, None)
            if first:
                first.hi, first.t, first.halves = r.val, r.t, 2
                out.append(first)
            else:
                out.append(Message("mbox", r.t, r.cpu, r.src, r.write, r.pa - 8, None, r.val, 3,
                                   1, r.index))
            continue
        # A narrower access to a mailbox word: shown as it was.
        out.append(Message("mbox", r.t, r.cpu, r.src, r.write, r.pa, r.val, None, r.width, 1,
                           r.index))
    out.extend(pend.values())
    out.sort(key=lambda m: (m.t, m.index))
    return out


def mark_trigger(msgs, h):
    for m in msgs:
        if (m.kind == "mbox" and m.write and m.src_i == SRC_GFX and m.msg == h["trig_val"]
                and m.hi == h["trig_hi"] and m.cpu == h["trig_cpu"]):
            m.trigger = True
            break
    return msgs


# ---- decoding -----------------------------------------------------------------------------------

PWR = {0x0: "OFF", 0x1: "SLEEP", 0x201: "IDLE", 0x10: "QUIESCED", 0x20: "ON", 0x220: "INIT"}
SYS_EPS = {1: "crashlog", 2: "syslog", 3: "debug", 4: "ioreport", 7: "coreanalytics/aux",
           8: "oslog", 0xa: "tracekit"}
PMP_OPS = {0x10: "GET_IOVA_TABLE", 0x12: "MALLOC", 0x14: "FREE", 0x30: "SET_BUF",
           0x32: "REGISTER_IOREG", 0x34: "SET_IOREG", 0x36: "UPDATE_IOREG", 0x37: "CONFIRM_IOREG"}
GFX_OPS = {0x81: "InitData", 0x42: "RX doorbell (firmware notification)", 0x83: "TX doorbell",
           0x84: "FWCTL doorbell", 0x85: "halt"}


def _pwr(v):
    return PWR.get(v, f"0x{v:x}")


def mgmt(msg, write):
    typ = (msg >> 52) & 0xff
    if typ == 1:
        return f"HELLO ver {msg & 0xffff}..{(msg >> 16) & 0xffff}"
    if typ == 2:
        return f"HELLO_ACK ver {msg & 0xffff}..{(msg >> 16) & 0xffff}"
    if typ == 5:
        return f"STARTEP ep 0x{(msg >> 32) & 0xff:02x}" + (" flag" if msg & 2 else "")
    if typ == 6:
        return f"IOP_PWR_STATE {_pwr(msg & 0xffff)}"
    if typ == 7:
        return f"IOP_PWR_STATE_ACK {_pwr(msg & 0xffff)}"
    if typ == 8:
        base = (msg >> 32) & 7
        if write:
            return f"EPMAP_REPLY base {base} " + ("last" if msg >> 51 & 1 else "more"
                                                 if msg & 1 else "")
        return f"EPMAP base {base} bitmap 0x{msg & 0xffffffff:08x}" + (" last" if msg >> 51 & 1
                                                                       else "")
    if typ == 0xb:
        return f"AP_PWR_STATE{' ' if write else '_ACK '}{_pwr(msg & 0xffff)}"
    if typ == 0xc:
        return "IDLE_NOTIFY"
    return f"mgmt type 0x{typ:x}"


def gfx_ep(msg, ep):
    op = msg >> 48
    name = GFX_OPS.get(op, f"type 0x{op:x}")
    if op == 0x81:
        va = msg & ((1 << 44) - 1)
        return f"{name} root fw VA 0x{va | 0xfffff00000000000:x}"
    if op == 0x83:
        low = msg & 0xffffffffffff
        what = {0x10: "KICKFW", 0x11: "DEVCTRL"}.get(low, f"pipe {low & 3} index {low >> 2}")
        return f"{name} {what}"
    return name if op in GFX_OPS else f"{name} 0x{msg & 0xffffffffffff:x}"


def pmp_ep(msg, write):
    cmd = (msg >> 44) & 0xff
    if msg & (1 << 53) and cmd in (0x0e, 0x0f, 0x10):
        if write:
            return (f"devpower cmd 0x{cmd:02x} dev {(msg >> 40) & 0xf} "
                    f"{'on' if (msg >> 32) & 0xff else 'off'}")
        return f"devpower reply 0x{cmd:02x}"
    op = (msg >> 48) & 0xff
    name = PMP_OPS.get(op) or (PMP_OPS.get(op & ~1, f"op 0x{op & ~1:02x}") + " ack" if op & 1
                               else f"op 0x{op:02x}")
    return f"{name} 0x{msg & 0xffffffffffff:x}"


def mtype(m):
    """A short name of a message's kind, for the per-endpoint counts."""
    if m.kind == "reg":
        return "write"
    if m.msg is None:
        return "second word only"
    ep = m.ep
    if ep is None:
        return "first word only"
    if ep == 0:
        return mgmt(m.msg, m.write).split(" ")[0]
    if ep in SYS_EPS:
        return f"type 0x{(m.msg >> 52) & 0xff:x}"
    if m.src_i in (SRC_GFX, SRC_GFX1):
        return GFX_OPS.get(m.msg >> 48, f"type 0x{m.msg >> 48:x}")
    if m.src_i == SRC_PMP:
        return " ".join(pmp_ep(m.msg, m.write).split(" ")[:-1]) or "?"
    return "?"


def reg_writes(m, h):
    """A register write as [(register, the value its word got)]: a PMGR store wider than the PS
    word (a 32-bit store pair) gives the word's part."""
    if m.src_i == SRC_SGX:
        return [(f"sgx+0x{m.pa - h['base'][SRC_SGX]:x}", m.msg)]
    out = []
    for p, name in zip(h["ps"], h["ps_name"]):
        if m.pa < p + 4 and m.pa + (1 << m.width) > p:
            word = (m.msg >> (8 * (p - m.pa))) & 0xffffffff if p >= m.pa else m.msg
            out.append((f"pmgr {name} ps 0x{p:x}", word))
    return out or [(f"0x{m.pa:x}", m.msg)]


def is_power(m):
    """A power-state event: RTKit's IOP/AP power states and idle, the PMP's device power, a PS
    word written."""
    if m.kind == "reg":
        return m.src_i == SRC_PMGR
    if m.msg is None or m.ep is None:
        return False
    if m.ep == 0:
        return (m.msg >> 52) & 0xff in (6, 7, 0xb, 0xc)
    return m.src_i == SRC_PMP and m.ep == 0x20 and pmp_ep(m.msg, m.write).startswith("devpower")


def decode(m, h):
    if m.kind == "reg":
        out = []
        for where, word in reg_writes(m, h):
            what = f"W{8 << m.width} {where} = 0x{word:x}"
            if where.startswith("pmgr"):
                what += (f" (target 0x{word & 0xf:x}" + (", auto" if word >> 28 & 1 else "")
                         + (", dev-disable" if word >> 10 & 1 else "")
                         + (", reset" if word >> 31 & 1 else "") + ")")
            out.append(what)
        return "; ".join(out)
    if m.msg is None:
        return "(second word only)"
    if m.ep is None:
        return "(first word only)"
    ep = m.ep
    if ep == 0:
        what = mgmt(m.msg, m.write)
    elif ep in SYS_EPS:
        typ = (m.msg >> 52) & 0xff
        what = f"{SYS_EPS[ep]} type 0x{typ:x}"
        if typ == 1:
            what += (f" buffer {(m.msg >> 44) & 0xff} pages at 0x{m.msg & ((1 << 44) - 1):x}")
    elif m.src_i in (SRC_GFX, SRC_GFX1):
        what = gfx_ep(m.msg, ep)
    elif m.src_i == SRC_PMP:
        what = pmp_ep(m.msg, m.write)
    else:
        what = ""
    return what


def _ms(t, h):
    return (t - h["t_start"]) * 1000.0 / h["cntfrq"] if h["cntfrq"] else 0.0


def message_line(m, h):
    ms = f"{_ms(m.t, h):+12.6f}"
    msg = "?" * 18 if m.msg is None else f"0x{m.msg:016x}"
    hi = "?" * 18 if m.hi is None else f"0x{m.hi:016x}"
    ep = "??" if m.ep is None else f"{m.ep:02x}"
    mark = ""
    if m.msg in PATTERNS:
        mark += f"  <<< 0x{m.msg:016x} ({PATTERNS[m.msg]})"
    if m.trigger:
        mark += "  [TRIGGER]"
    if is_power(m):
        mark += "  [PWR]"
    halves = "" if m.halves == 2 else "  (one half)"
    if m.kind == "reg":
        return f"{ms} 0x{m.t:012x} c{m.cpu:<2d} {m.src:4s} {decode(m, h)}{mark}"
    return (f"{ms} 0x{m.t:012x} c{m.cpu:<2d} {m.src:4s} {m.dir} ep 0x{ep} {msg} {hi}  "
            f"{decode(m, h)}{halves}{mark}")


def trace_lines(rec, msgs=None, title="the record"):
    h = rec.hdr
    msgs = rec.messages() if msgs is None else msgs
    out = [f"# {title}: {len(msgs)} messages/writes, time-ordered; ms from the InitData doorbell "
           f"(tick 0x{h['t_start']:x}, {h['cntfrq']} Hz)",
           "# columns: ms tick cpu src dir ep msg0 msg1 decode | ms tick cpu src register write"]
    out += [message_line(m, h) for m in msgs]
    return out


def after_bursts(msgs, h, gap_ms=2.0, follow=8, most=16):
    """What the AP sends after each of the GPU firmware's 0x42 (RX doorbell) bursts: the GPU's
    0x42 messages on EP 0x20 less than gap_ms apart make one burst; after its last, the AP's first
    follow messages to either coprocessor, until the next burst."""
    def is42(m):
        return (m.kind == "mbox" and m.src_i in (SRC_GFX, SRC_GFX1) and not m.write
                and m.ep == 0x20 and m.msg is not None and m.msg >> 48 == 0x42)

    gap = gap_ms * h["cntfrq"] / 1000
    bursts = []
    for m in msgs:
        if is42(m):
            if bursts and m.t - bursts[-1][-1].t < gap:
                bursts[-1].append(m)
            else:
                bursts.append([m])
    out = [f"what the AP sends after each GPU firmware 0x42 (RX doorbell) burst ({len(bursts)}; "
           f"a burst: 0x42s < {gap_ms:g} ms apart; ms after its last 0x42):"]
    for n, b in enumerate(bursts[:most]):
        end = b[-1].t
        nxt = bursts[n + 1][0].t if n + 1 < len(bursts) else None
        out.append(f"  burst {n + 1}: {len(b)} GPU 0x42 messages, {_ms(b[0].t, h):+.3f} .. "
                   f"{_ms(end, h):+.3f} ms")
        after = [m for m in msgs if m.kind == "mbox" and m.write and m.t > end
                 and (nxt is None or m.t < nxt)]
        for m in after[:follow]:
            ep = "??" if m.ep is None else f"{m.ep:02x}"
            msg = "?" * 18 if m.msg is None else f"0x{m.msg:016x}"
            out.append(f"    {(m.t - end) * 1000 / h['cntfrq']:+.3f} ms {m.src} {m.dir} ep 0x{ep} "
                       f"{msg}  {decode(m, h)}")
        gpu = next((m for m in after if m.src_i in (SRC_GFX, SRC_GFX1)), None)
        out.append("    the AP's first message to the GPU: "
                   + (f"{(gpu.t - end) * 1000 / h['cntfrq']:+.3f} ms ({decode(gpu, h)})" if gpu
                      else "none before the next burst or the record's end"))
    if len(bursts) > most:
        out.append(f"  ... {len(bursts) - most} more bursts in trace.txt")
    return out


def summary(rec, ring=None):
    """The header, then counts by source, direction and endpoint, the PMP patterns, the register
    writes; ring: (messages, records read, head, capacity) of the MMIO ring if it was read."""
    h, msgs = rec.hdr, rec.messages()
    n = min(h["claimed"], h["cap"])
    dropped = max(0, h["claimed"] - h["cap"])
    out = [f"state {STATES.get(h['state'], h['state'])}",
           f"window {h['window_ms']} ms from the doorbell 0x{h['trig_val']:016x} "
           f"(ep 0x{h['trig_hi'] & 0xff:02x}, cpu {h['trig_cpu']}, guest pc 0x{h['trig_pc']:x}) "
           f"at tick 0x{h['t_start']:x}; end 0x{h['t_end']:x}"]
    if h["state"] == DONE_TIME:
        out.append(f"ended by time at +{_ms(h['t_done'], h):.3f} ms: lossless")
    elif h["state"] == DONE_FULL:
        out.append(f"the buffer was FULL at +{_ms(h['t_done'], h):.3f} ms: everything after it "
                   f"is lost ({dropped} dropped that raced the end)")
    elif h["state"] == ON:
        out.append("still ON: no access the record takes came after the window's end yet")
    out.append(f"records {n} of {h['cap']} ({dropped} dropped), {rec.incomplete} incomplete "
               f"(claimed, not yet written)")
    src = ", ".join(f"{SRC_NAMES[s]} 0x{h['base'][s]:x}+0x{h['size'][s]:x}" for s in range(4)
                    if h["size"][s])
    ps = ", ".join(f"{nm} 0x{p:x} ({'trapped' if h['ps_trapped'] >> i & 1 else 'NOT trapped'})"
                   for i, (p, nm) in enumerate(zip(h["ps"], h["ps_name"])))
    out.append(f"windows: {src}; PMGR PS words: {ps or 'none'}")
    out.append("")
    out.append("per endpoint (count, first and last ms, message kinds):")
    groups = collections.OrderedDict()
    for m in msgs:
        if m.kind == "mbox":
            ep = "??" if m.ep is None else f"0x{m.ep:02x}"
            groups.setdefault(f"{m.src} {m.dir} ep {ep}", []).append(m)
    for key in sorted(groups):
        g = groups[key]
        kinds = collections.Counter(mtype(m) for m in g)
        out.append(f"  {key:18s} {len(g):7d}  {_ms(g[0].t, h):+11.3f} {_ms(g[-1].t, h):+11.3f}  "
                   + ", ".join(f"{k} x{c}" for k, c in kinds.most_common(8)))
    out.append("")
    out.append("PMP patterns:")
    for pat, name in PATTERNS.items():
        hits = [m for m in msgs if m.kind == "mbox" and m.src_i == SRC_PMP and m.msg == pat]
        by = collections.Counter(m.dir for m in hits)
        first = f", first {_ms(hits[0].t, h):+.3f} ms, last {_ms(hits[-1].t, h):+.3f} ms" \
            if hits else ""
        out.append(f"  0x{pat:016x} ({name}): A2I {by['A2I']}, I2A {by['I2A']}{first}")
    out.append("")
    out += after_bursts(msgs, h)
    out.append("")
    out.append("register writes:")
    regs = collections.OrderedDict()
    for m in msgs:
        if m.kind == "reg":
            for where, word in reg_writes(m, h):
                regs.setdefault(where, []).append((m.t, word))
    for key, g in regs.items():
        vals = " ".join(f"0x{w:x}" for _, w in g[:6]) + (" ..." if len(g) > 6 else "")
        out.append(f"  {key:30s} {len(g):5d}  last at {_ms(g[-1][0], h):+.3f} ms: {vals}")
    if not regs:
        out.append("  none")
    power = [m for m in msgs if is_power(m)]
    out.append("")
    out.append(f"power events ({len(power)}; [PWR] in trace.txt), the first 48:")
    out += ["  " + message_line(m, h) for m in power[:48]]
    if ring is not None:
        rmsgs, nread, head, nrec = ring
        before = [m for m in rmsgs if m.t < h["t_start"]]
        inwin = [m for m in rmsgs if h["t_start"] <= m.t < h["t_end"]]
        mine = [m for m in msgs if m.kind == "mbox"]
        out.append("")
        out.append(f"MMIO ring: {nread} records read of {head} written (capacity {nrec}"
                   + (", WRAPPED: its oldest are gone" if head > nrec else ", never wrapped")
                   + f"); {len(before)} messages/writes before the doorbell (ring-trace.txt)")
        out.append(f"  cross-check in the window: ring {sum(m.kind == 'mbox' for m in inwin)} "
                   f"mailbox messages, record {len(mine)}")
    return "\n".join(out) + "\n"


# ---- the MMIO_PROTECTED block -------------------------------------------------------------------

BLK_MAGIC = 0x314b4c4241525541  # "AURABLK1"
BLK_HDR = struct.Struct("<QIIIIQQQQQQQ48s")
assert BLK_HDR.size == 128
BlkRec = collections.namedtuple("BlkRec", "index t val pc pa write width cpu gl pair")


class Block:
    def __init__(self, hdr, recs, incomplete):
        self.hdr, self.recs, self.incomplete = hdr, recs, incomplete


def parse_blk_hdr(raw):
    v = BLK_HDR.unpack_from(raw, 0)
    h = dict(zip(("magic", "version", "hdr_bytes", "rec_bytes", "state", "cap", "claimed", "base",
                  "size", "t_first", "t_done", "cntfrq"), v[:12]))
    h["node"] = v[12].split(b"\0")[0].decode("latin1")
    if h["magic"] != BLK_MAGIC or h["hdr_bytes"] != BLK_HDR.size or h["rec_bytes"] != REC_BYTES:
        raise ValueError("not an MMIO_PROTECTED block record (magic, header or record size)")
    return h


def parse_blk(raw):
    h = parse_blk_hdr(raw)
    n = min(h["claimed"], h["cap"], (len(raw) - BLK_HDR.size) // REC_BYTES)
    recs, incomplete = [], 0
    for i in range(n):
        t, val, pc, meta = REC.unpack_from(raw, BLK_HDR.size + REC_BYTES * i)
        if not t:
            incomplete += 1
            continue
        recs.append(BlkRec(i, t, val, pc, meta & 0xffffffffffff, bool(meta >> 52 & 1),
                           meta >> 53 & 7, meta >> 56 & 15, bool(meta >> 60 & 1),
                           bool(meta >> 61 & 1)))
    recs.sort(key=lambda r: (r.t, r.index))
    return Block(h, recs, incomplete)


def blk_who(r):
    """SPTM: a GL1 trap (SPTM's own code, its VAs 0xfffffff0...); XNU: the kernel's EL trap."""
    return "SPTM" if r.gl else "XNU"


def blk_lines(blk, main_hdr):
    b = blk.hdr
    out = [f"# MMIO_PROTECTED block 0x{b['base']:x}+0x{b['size']:x} ({b['node'] or 'no ADT node'}"
           f"): every access, time-ordered; ms from the InitData doorbell (negative: before it)",
           "# from the first access the stage 2 traps (each core's XNU entry; SPTM's own bring-up "
           "before it runs without stage 2 and isn't seen) to the window's end",
           "# columns: ms tick cpu who pc R/W<bits> +offset value  (who: SPTM = a GL1 trap, "
           "XNU = an EL trap)",
           "# only the AP's accesses trap: the GPU firmware's own (the J713's G16 27.2 maps the "
           "block at fw VA 0xfffffc11c0000000 + offset) don't, nor do its RAM writes"]
    for r in blk.recs:
        pair = " (128-bit pair)" if r.pair else ""
        out.append(f"{_ms(r.t, main_hdr):+13.6f} 0x{r.t:012x} c{r.cpu:<2d} {blk_who(r):4s} "
                   f"pc 0x{r.pc:016x} {'W' if r.write else 'R'}{8 << r.width:<3d} "
                   f"+0x{r.pa - b['base']:04x} = 0x{r.val:x}{pair}")
    return out


def blk_summary(blk, main_hdr):
    b, recs = blk.hdr, blk.recs
    n = min(b["claimed"], b["cap"])
    end = {DONE_TIME: "ended with the window (lossless)",
           DONE_FULL: f"FULL at {_ms(b['t_done'], main_hdr):+.3f} ms: what came after is lost",
           ON: "still ON (no doorbell, or no access after the window's end yet)"}
    who = collections.Counter(blk_who(r) for r in recs)
    out = [f"MMIO_PROTECTED block 0x{b['base']:x}+0x{b['size']:x}: ADT {b['node'] or '?'}",
           f"  {end.get(b['state'], b['state'])}; {n} records of {b['cap']} "
           f"({max(0, b['claimed'] - b['cap'])} dropped), {blk.incomplete} incomplete; "
           f"by who: SPTM {who['SPTM']}, XNU {who['XNU']}"]
    if recs:
        out.append(f"  first {_ms(recs[0].t, main_hdr):+.3f} ms, last "
                   f"{_ms(recs[-1].t, main_hdr):+.3f} ms (from the doorbell)")
    regs = collections.OrderedDict()
    for r in recs:
        regs.setdefault((r.pa - b["base"], r.write, blk_who(r)), []).append(r)
    out.append("  per offset (who, R/W, count, values in order, first 6):")
    for (off, w, wh), g in sorted(regs.items()):
        vals = " ".join(f"0x{r.val:x}" for r in g[:6]) + (" ..." if len(g) > 6 else "")
        out.append(f"    +0x{off:04x} {wh:4s} {'W' if w else 'R'} {len(g):6d}  {vals}")
    return "\n".join(out) + "\n"


def read_blk(readmem, sym, chunk=1 << 20):
    """The block's record bytes (header + records) from a parked HV; b"" without one."""
    if "sptm_virt_gfxrec_blk" not in sym:
        return b""
    buf = struct.unpack("<Q", readmem(sym["sptm_virt_gfxrec_blk"], 8))[0]
    if not buf:
        return b""
    hdr = readmem(buf, BLK_HDR.size)
    h = parse_blk_hdr(hdr)
    size = min(h["claimed"], h["cap"]) * REC_BYTES
    return hdr + b"".join(readmem(buf + BLK_HDR.size + o, min(chunk, size - o))
                          for o in range(0, size, chunk))


# ---- the device ---------------------------------------------------------------------------------

def configure(p, sym, env):
    """run_guest_sptm.py: AURA_GFXPMP=<ms> and AURA_GFXPMP_MAX=<records> into aura before entry."""
    if env.get("AURA_GFXPMP"):
        p.write32(sym["sptm_virt_gfxrec_ms"], int(env["AURA_GFXPMP"], 0))
    if env.get("AURA_GFXPMP_MAX"):
        p.write32(sym["sptm_virt_gfxrec_max"], int(env["AURA_GFXPMP_MAX"], 0))
    if env.get("AURA_GFXPMP_BLK"):
        pa, length = (int(x, 0) for x in env["AURA_GFXPMP_BLK"].split(","))
        p.write64(sym["sptm_virt_gfxrec_blk_pa"], pa)
        p.write64(sym["sptm_virt_gfxrec_blk_len"], length)
    if env.get("AURA_GFXPMP_BLK_MAX"):
        p.write32(sym["sptm_virt_gfxrec_blk_max"], int(env["AURA_GFXPMP_BLK_MAX"], 0))


def check_env(env):
    """Refuse a configuration that would park at the doorbell or never start: '' or the reason."""
    if not env.get("AURA_GFXPMP"):
        return ""
    if int(env["AURA_GFXPMP"], 0) <= 0:
        return "AURA_GFXPMP is the window in ms (> 0)"
    if env.get("AURA_GFXPMP_MAX") and not 1 <= int(env["AURA_GFXPMP_MAX"], 0) <= 1 << 20:
        return "AURA_GFXPMP_MAX is 1..1048576 records"
    if env.get("AURA_GFXPMP_BLK_MAX") and not 2 <= int(env["AURA_GFXPMP_BLK_MAX"], 0) <= 1 << 20:
        return "AURA_GFXPMP_BLK_MAX is 2..1048576 records"
    if env.get("AURA_GFXPMP_BLK"):
        try:
            pa, length = (int(x, 0) for x in env["AURA_GFXPMP_BLK"].split(","))
        except ValueError:
            return "AURA_GFXPMP_BLK is <pa>,<length>"
        if not length or (pa | length) & 3:
            return "AURA_GFXPMP_BLK: a 4-byte-aligned PA and a length"
    if env.get("AURA_GPU_TRACE") != "1":
        return "AURA_GFXPMP needs AURA_GPU_TRACE (its hooks feed the record)"
    for var in ("AURA_GPU_STOP_MSG", "AURA_GPU_STOP_MSG_MASK", "AURA_GPU_STOP_EP"):
        if not env.get(var):
            return f"AURA_GFXPMP starts at the write AURA_GPU_STOP_MSG names: set {var}"
    for var in ("AURA_GPU_STOP_SECS", "AURA_GPU_STOP_BOOT_SECS"):
        if env.get(var) and env[var] != "0":
            return f"AURA_GFXPMP: {var} would park the guest; the record needs it running"
    if env.get("AURA_GPU_MSG_PARK", "0") != "0":
        return "AURA_GFXPMP: the doorbell mustn't park (AURA_GPU_MSG_PARK=0, the record's default)"
    if env.get("AURA_GPU_SNAP"):
        return "AURA_GFXPMP: AURA_GPU_SNAP holds the doorbell's core while it copies"
    return ""


def read_hv(readmem, sym, ring=True, chunk=1 << 20):
    """(record bytes, ring bytes oldest first, ring head, ring capacity) from a parked HV."""
    def rd64(a):
        return struct.unpack("<Q", readmem(a, 8))[0]

    def bulk(addr, size):
        return b"".join(readmem(addr + o, min(chunk, size - o)) for o in range(0, size, chunk))

    buf = rd64(sym["sptm_virt_gfxrec_buf"])
    raw = b""
    if buf:
        hdr = readmem(buf, HDR_BYTES)
        h = parse_hdr(hdr)
        raw = hdr + bulk(buf + HDR_BYTES, min(h["claimed"], h["cap"]) * REC_BYTES)
    rraw, head, nrec = b"", 0, 0
    if ring:
        rp, nrec, head = (rd64(sym[s]) for s in ("sptm_virt_gpu_ring", "sptm_virt_gpu_nrec",
                                                 "sptm_virt_gpu_head"))
        if rp and nrec:
            have = min(head, nrec)
            first = (head - have) % nrec
            n1 = min(have, nrec - first)
            rraw = bulk(rp + 32 * first, 32 * n1) + bulk(rp, 32 * (have - n1))
    return raw, rraw, head, nrec
