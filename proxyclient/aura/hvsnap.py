# SPDX-License-Identifier: MIT
"""GPU firmware snapshots and the GPU mailbox log from the host (src/sptm_virt_snap.[ch],
src/sptm_virt_gpu.c): what run_guest_sptm.py sets in aura before entry, and how
tools/dump_snapshot.py reads the buffer back and lays it out as the GPU lane's fw272-b2dump.py does
(objects/<path>.bin, objects.txt, walk.txt, tables/shared-l2/ttbs/handoff.bin)."""

import hashlib
import struct

SNAP_MAGIC = 0x31504e5341525541
HDR = struct.Struct("<QIIQQIIII4QII4QQ")
REC = struct.Struct("<II7Q")
CHAIN = struct.Struct("<16sQII6Q")
WATCH = struct.Struct("<IIQ")
SNAP_CHAINS, SNAP_CHAIN_STEPS, STEP_ADD = 24, 6, 1 << 63
KIND = {1: "snap", 2: "region", 3: "walk", 4: "obj", 5: "end", 6: "chain"}
REGIONS = ("tables", "shared-l2", "ttbs", "handoff")
PAGE = 0x4000
W_FLAGS = {1: "TTBAT empty: walked the tables region's first page", 2: "walk truncated"}
E_FLAGS = {1: "buffer full", 2: "object limit reached", 4: "PAs outside guest RAM refused",
           8: "no firmware page tables"}
C_STATUS = {0: "ok", 1: "a pointer on the way didn't translate", 2: "shorter than asked"}
HDR_FIELDS = ("magic", "version", "hdr_bytes", "capacity", "used", "nrec", "nsnap", "flags",
              "skipped", "w0", "w1", "w2", "w3", "watch_ok", "pad", "a0", "a1", "a2", "a3",
              "reserved")

# The G16G firmware of macOS 27.2 (26B123), from the GPU lane's analysis (g16-272/MAP.md and the
# Linux port's fw/initdata.rs): from the InitData root, the main config at +0x18; in it HW data's
# pointer at +0x00, the 12 submit channels' descriptors at +0x20 + 0x20 i and device control's at
# +0x1a0 (state +0x0, ring +0x18; 256 records of 0x40), then {state, ring} for event +0x1c0, log
# +0x1d0, ktrace +0x1e0 and stats +0x1f0; FWCTL's state and ring in FirmwareData (root +0xb0) at
# +0x5168 and +0x5170. Ring state: read +0x0, completion +0x10, write +0x20.
PRESETS = {
    "g16g-272": (
        [("maincfg", (0x18,), 0x4b8), ("hwdata", (0x18, 0x0), 0x4000),
         ("devctl-state", (0x18, 0x1a0), 0x30), ("devctl-ring", (0x18, 0x1b8), 0x4000)]
        + [(f"chan{i}-state", (0x18, 0x20 + 0x20 * i), 0x30) for i in range(12)]
        + [("event-state", (0x18, 0x1c0), 0x30), ("log-state", (0x18, 0x1d0), 0x1b0),
           ("ktrace-state", (0x18, 0x1e0), 0x30), ("stats-state", (0x18, 0x1f0), 0x30),
           ("fwctl-state", (0xb0, 0x5168), 0x30), ("fwctl-ring", (0xb0, 0x5170), 0x1000)],
        [("hwdata", 0xfa0, 8), ("hwdata", 0xfa8, 8)]),
}


def parse_chains(spec):
    """'preset' or 'name=step/step/...:len;...', a step '0x18' reading a pointer at +0x18 and
    '+0x8' adding 8. Returns (chains, watches) as PRESETS has them (a preset brings its watches)."""
    if spec in PRESETS:
        return PRESETS[spec]
    chains = []
    for item in filter(None, (x.strip() for x in spec.split(";"))):
        name, rest = item.split("=", 1)
        path, length = rest.rsplit(":", 1)
        steps = tuple(STEP_ADD | int(s[1:], 0) if s.startswith("+") else int(s, 0)
                      for s in path.split("/") if s)
        chains.append((name, steps, int(length, 0)))
    return chains, []


def parse_watches(spec):
    """'name+0xfa0:8,name+0xfa8:4'"""
    out = []
    for item in filter(None, (x.strip() for x in spec.split(","))):
        where, n = item.rsplit(":", 1)
        name, off = where.split("+", 1)
        out.append((name, int(off, 0), int(n, 0)))
    return out


def config_blobs(chains, watches):
    if len(chains) > SNAP_CHAINS:
        raise ValueError(f"{len(chains)} chains, at most {SNAP_CHAINS}")
    names = [c[0] for c in chains]
    blob = b""
    for name, steps, length in chains:
        if len(name.encode()) > 15 or len(steps) > SNAP_CHAIN_STEPS or not length:
            raise ValueError(f"chain {name}: a name of at most 15 bytes, at most "
                             f"{SNAP_CHAIN_STEPS} steps, a length")
        blob += CHAIN.pack(name.encode(), length, len(steps), 0,
                           *(list(steps) + [0] * (SNAP_CHAIN_STEPS - len(steps))))
    wblob = b""
    for name, off, n in watches[:4]:
        if name not in names or n not in (4, 8):
            raise ValueError(f"watch {name}+0x{off:x}:{n}: no such chain, or not 4 or 8 bytes")
        wblob += WATCH.pack(names.index(name) + 1, n, off)
    return blob, wblob


def configure(p, sym, env):
    """run_guest_sptm.py: the snapshot and mailbox-log switches (AURA_GPU_SNAP*, AURA_GPU_MBOX*,
    AURA_GPU_MSG_PARK) into aura before entry."""
    def num(name, default=None):
        v = env.get(name)
        return int(v, 0) if v else default

    for var, symbol in (("AURA_GPU_SNAP", "sptm_virt_gpu_snap_every"),
                        ("AURA_GPU_SNAP_MIB", "sptm_virt_gpu_snap_mib"),
                        ("AURA_GPU_SNAP_MAX", "sptm_virt_snap_max"),
                        ("AURA_GPU_SNAP_PARTS", "sptm_virt_snap_parts"),
                        ("AURA_GPU_MSG_PARK", "sptm_virt_gpu_msg_park"),
                        ("AURA_GPU_MBOX", "sptm_virt_gpu_mbox_max")):
        if num(var) is not None:
            p.write32(sym[symbol], num(var))
    roots = [int(x, 0) for x in env.get("AURA_GPU_SNAP_ROOTS", "").split(",") if x]
    for i, r in enumerate(roots[:4]):
        p.write64(sym["sptm_virt_snap_roots"] + 8 * i, r)
    chains, watches = parse_chains(env.get("AURA_GPU_SNAP_CHAINS", ""))
    if env.get("AURA_GPU_SNAP_WATCH"):
        watches = parse_watches(env["AURA_GPU_SNAP_WATCH"])
    blob, wblob = config_blobs(chains, watches)
    if blob:
        p.iface.writemem(sym["sptm_virt_snap_chains"], blob)
    if wblob:
        p.iface.writemem(sym["sptm_virt_snap_watch"], wblob)
    if env.get("AURA_GPU_MBOX_STOP"):
        v = [int(x, 0) for x in env["AURA_GPU_MBOX_STOP"].split(",")]
        p.write64(sym["sptm_virt_gpu_mbox_stop_msg"], v[0])
        p.write64(sym["sptm_virt_gpu_mbox_stop_mask"], v[1])
        p.write32(sym["sptm_virt_gpu_mbox_stop_ep"], v[2] + 1)
        p.write32(sym["sptm_virt_gpu_mbox_stop_after"], v[3] if len(v) > 3 else 0)
    if num("AURA_GPU_SNAP"):
        print(f"GPU snapshots: every {num('AURA_GPU_SNAP')}-th named write, "
              f"{num('AURA_GPU_SNAP_MAX', 1)} in all, {len(chains)} chains, "
              f"{len(watches)} watched words")


# ---- reading it back ----------------------------------------------------------------------

class Snapshot:
    def __init__(self, index):
        self.index = index
        self.kick = None
        self.regions = {}
        self.walk = None
        self.objects = []
        self.chains = []
        self.end = None


def parse(buf):
    """The buffer -> (header dict, [Snapshot])."""
    h = dict(zip(HDR_FIELDS, HDR.unpack_from(buf, 0)))
    if h["magic"] != SNAP_MAGIC or h["version"] != 1 or h["hdr_bytes"] != HDR.size:
        raise ValueError("not a version-1 snapshot buffer")
    snaps, cur, off = [], None, HDR.size
    for _ in range(h["nrec"]):
        if off + REC.size > min(len(buf), h["used"]):
            raise ValueError(f"record at 0x{off:x} runs past the buffer's end")
        kind, length, va, pa, attrs, a, b, c, d = REC.unpack_from(buf, off)
        data = buf[off + REC.size:off + REC.size + length]
        if len(data) != length:
            raise ValueError(f"record at 0x{off:x}: {length} bytes, the buffer ends first")
        off += REC.size + (length + 15) // 16 * 16
        k = KIND.get(kind)
        if k == "snap":
            cur = Snapshot(len(snaps))
            snaps.append(cur)
            cur.kick = dict(value=va, value_hi=pa, pc=attrs, t=a, cpu=b & 0xff, ep=(b >> 8) & 0xff,
                            match=b >> 32, ring_head=c, root=d)
            continue
        if cur is None:
            raise ValueError("a record before the first snapshot")
        if k == "region":
            cur.regions[REGIONS[va]] = dict(pa=pa, size=a, refused=c, data=data)
        elif k == "walk":
            runs = [struct.unpack_from("<4Q", data, i) for i in range(0, len(data), 32)]
            cur.walk = dict(ttbr1=va, leaves=pa, flags=attrs & 0xffffffff, refused=attrs >> 32,
                            ttbat=(a, b, c, d), runs=runs)
        elif k == "obj":
            cur.objects.append(dict(va=va, pa=pa, attrs=attrs, parent=a, off=b, depth=c,
                                    index=d & 0xffffffff, slot=d >> 32, data=data))
        elif k == "chain":
            name = struct.pack("<QQ", c, d).split(b"\0")[0].decode("latin1")
            cur.chains.append(dict(name=name, index=a & 0xffffffff, status=a >> 32, step=b,
                                   va=va, pa=pa, attrs=attrs, data=data))
        elif k == "end":
            cur.end = dict(objects=a, ticks=b, flags=c, refused=d)
        else:
            raise ValueError(f"record kind {kind} at 0x{off:x}")
    return h, snaps


def object_paths(objects):
    slots = {o["slot"] for o in objects if o["parent"] == (1 << 64) - 1}
    paths = []
    for o in objects:
        if o["parent"] == (1 << 64) - 1:
            paths.append("root" if len(slots) <= 1 else f"root{o['slot']}")
        else:
            paths.append(f"{paths[o['parent']]}+{o['off']:#x}")
    return paths


def flags_text(flags, table):
    return ", ".join(t for bit, t in table.items() if flags & bit) or "none"


def write_layout(snap, out):
    """One snapshot in fw272-b2dump.py's layout under out."""
    out.mkdir(parents=True, exist_ok=True)
    (out / "objects").mkdir(exist_ok=True)
    for name, r in snap.regions.items():
        (out / f"{name}.bin").write_bytes(r["data"])
    if snap.walk:
        w = snap.walk
        with open(out / "walk.txt", "w") as f:
            f.write(f"ttbat[0..4] {[hex(x) for x in w['ttbat']]} ttbr1 {w['ttbr1']:#x}; "
                    f"{w['leaves']} leaves\n")
            for va, va_end, pa, at in w["runs"]:
                f.write(f"{va:#x}..{va_end:#x} pa {pa:#x} attrs {at:#018x}\n")
    rows = []
    for path, o in zip(object_paths(snap.objects), snap.objects):
        (out / "objects" / f"{path}.bin").write_bytes(o["data"])
        rows.append(f"{path} va {o['va']:#x} pa {o['pa']:#x} attrs {o['attrs']:#x} bytes "
                    f"{len(o['data']):#x} sha {hashlib.sha256(o['data']).hexdigest()[:12]}")
    (out / "objects.txt").write_text("\n".join(rows) + "\n")
    if snap.chains:
        (out / "chains").mkdir(exist_ok=True)
        lines = []
        for c in snap.chains:
            (out / "chains" / f"{c['name']}.bin").write_bytes(c["data"])
            lines.append(f"{c['name']} va {c['va']:#x} pa {c['pa']:#x} attrs {c['attrs']:#x} "
                         f"bytes {len(c['data']):#x} {C_STATUS.get(c['status'], c['status'])}"
                         + (f" (step {c['step']})" if c["status"] == 1 else ""))
        (out / "chains.txt").write_text("\n".join(lines) + "\n")


def summary(h, snaps, chain_names=None):
    lines = [f"snapshot buffer: {h['nsnap']} snapshots, {h['nrec']} records, {h['used']:#x} of "
             f"{h['capacity']:#x} bytes{', FULL' if h['flags'] & 1 else ''}; {h['skipped']} not "
             f"taken"]
    for i in range(4):
        if h["watch_ok"] & (1 << i):
            at = h[f"a{i}"]
            ci, n, off = at & 0xffff, (at >> 16) & 0xffff, at >> 32
            name = None
            for s in snaps:
                name = name or next((c["name"] for c in s.chains if c["index"] == ci), None)
            lines.append(f"watch {i}: {name or f'chain {ci}'} +{off:#x} ({n} bytes) = "
                         f"{h[f'w{i}']:#x}")
    for s in snaps:
        k, e = s.kick, s.end or {}
        lines.append(f"#{s.index}: match {k['match']} on cpu{k['cpu']} ep 0x{k['ep']:02x} value "
                     f"{k['value']:#018x} pc {k['pc']:#x} root {k['root']:#x} t {k['t']:#x} ring "
                     f"head {k['ring_head']}: {len(s.objects)} objects, {len(s.chains)} chains, "
                     f"{e.get('ticks', 0) / 24:.0f} us; "
                     f"{flags_text(e.get('flags', 0), E_FLAGS)}"
                     + (f" ({e['refused']} reads refused)" if e.get("refused") else ""))
        if s.walk:
            lines.append(f"   walk: ttbr1 {s.walk['ttbr1']:#x}, {s.walk['leaves']} leaves in "
                         f"{len(s.walk['runs'])} runs; {flags_text(s.walk['flags'], W_FLAGS)}"
                         + (f"; {s.walk['refused']} tables refused" if s.walk["refused"] else ""))
    return "\n".join(lines) + "\n"


# ---- the mailbox log ----------------------------------------------------------------------

MBOX = struct.Struct("<QQQII")
# (msg >> 48) & 0x3f of a message on EP 0x21: 0x83 is the TX doorbell (a channel's kick, or
# DEVCTRL 0x11 / KICKFW 0x10), 0x84 FWCTL's (asahi gpu.rs MSG_TX_DOORBELL, MSG_FWCTL).
EP21_TYPES = {3: "kick", 4: "fwctl", 6: "generation"}


def mbox_lines(raw):
    """The log, oldest first; a mailbox read as two 64-bit halves is one message. EP 0x21, where
    the GPU's work goes, is marked."""
    recs = [MBOX.unpack_from(raw, i) for i in range(0, len(raw) - MBOX.size + 1, MBOX.size)]
    out, pend = [], {}

    def lone(n, t, msg, recv, asc, cpu):
        return (f"{n} t={t:#x} c{cpu} {'I2A' if recv else 'A2I'} asc{asc} {msg:#018x} (first "
                f"word only)")

    for n, (t, msg, hi, meta, match) in enumerate(recs):
        recv, asc, words, cpu = meta & 1, (meta >> 1) & 1, (meta >> 2) & 3, (meta >> 8) & 15
        key = (recv, asc, cpu)
        if words == 1:
            if key in pend:  # the core started another before this one's second half
                out.append("   " + lone(*pend.pop(key), *key))
            pend[key] = (n, t, msg)
            continue
        if words == 2:
            _n, t, msg = pend.pop(key, (n, t, None))
            if msg is None:
                out.append(f"{n} t={t:#x} c{cpu} {'I2A' if recv else 'A2I'} asc{asc} (second "
                           f"word only) {hi:#018x}")
                continue
        ep = hi & 0xff
        mark = ">>" if ep == 0x21 else "  "
        typ = (msg >> 48) & 0x3f
        what = f" {EP21_TYPES.get(typ, f'type {typ}')}" if ep == 0x21 else ""
        out.append(f"{mark} {n} t={t:#x} c{cpu} {'I2A' if recv else 'A2I'} asc{asc} ep 0x{ep:02x} "
                   f"{msg:#018x}{what} (after {match} named writes)")
    # First halves the log ended before the second of.
    out += ["   " + lone(n, t, msg, *key) for key, (n, t, msg) in pend.items()]
    return out
