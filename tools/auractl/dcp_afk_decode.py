#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Decode an auractl ring dump taken with AURA_DCP_AFK_TRACE (src/sptm_virt_dcp_afk.c), and
AURA_SEP_TRACE (src/sptm_virt_sep.c) records in the same dump, into one timeline.

usage: dcp_afk_decode.py <dump.trc> <outdir> [--sep-mbox <page>]
Writes <outdir>/timeline.txt and <outdir>/dcp/<seq>-dcp<n>-ep<xx>-<what>.bin (each copy, cut to
its byte count). --sep-mbox takes only untagged records on that mailbox page as SEP messages
(without it: any untagged record at page offset 0x800/0x830, as sep_decode.py does).

The AFK/EPIC layouts are the public ones of the Linux DCP driver (drivers/gpu/drm/apple/afk.c,
afk.h, afkv2-codec.h): a 16-byte queue entry header (magic, size, channel, type); with channel
and type 0 the AFKv2/compact frame (seq, flags, interface, length, token, type, category,
interface flags, reserved: 24 bytes), and for commands and responses an 8-byte command word
(bit 0 OOB, id, value) with, if OOB, rx/tx DVA and sizes; otherwise the classic EPIC header and
sub-header (16 + 24 bytes).
"""
import os
import struct
import sys

HDR = struct.Struct("<8sII5Q48s")
REC = struct.Struct("<4Q")
SEPH, SEPD = 0x48504553, 0x44504553
TAGS = {int.from_bytes(k.encode(), "little"): k
        for k in ("DAFM", "DAFR", "DAFH", "DAFD", "DAFX", "DAFN")}
RBEP = {0x80: "INIT", 0xA0: "INIT_ACK", 0x89: "GETBUF", 0xA1: "GETBUF_ACK", 0x8A: "INIT_TX",
        0x8B: "INIT_RX", 0xA3: "START", 0x86: "START_ACK", 0xA2: "SEND", 0x85: "RECV",
        0xA7: "SPACE_AVAILABLE", 0xA8: "WAIT_FOR_SPACE", 0xC0: "SHUTDOWN", 0xC1: "SHUTDOWN_ACK"}
V2CAT = {0: "REPORT", 1: "COMMAND", 2: "RESPONSE"}
PROBLEM = {1: "DVA not mapped", 2: "ring pointer out of range", 3: "bad entry magic",
           4: "entry budget used up", 5: "bad ring header",
           6: "DART table outside guest RAM and pt-regions (val = PA)",
           7: "buffer page outside guest RAM (val = PA)"}
KIND = {0: "qe", 1: "OOB-rx", 2: "OOB-tx"}
QE_MAGIC = 0x20504F49
HEXMAX = 4096  # payload bytes printed per line


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def msg_text(m0, i2a):
    ty = m0 >> 48
    name = RBEP.get(ty, f"type 0x{ty:x}")
    if ty == 0x89:
        extra = f" size 0x{((m0 >> 16) & 0xFFFF) << 6:x} tag 0x{m0 & 0xFFFF:x}"
    elif ty == 0xA1:
        extra = f" dva 0x{m0 & 0xFFFFFFFFFFFF:x}"
    elif ty in (0x8A, 0x8B):
        extra = (f" off 0x{((m0 >> 32) & 0xFFFF) << 6:x} size 0x{((m0 >> 16) & 0xFFFF) << 6:x}"
                 f" tag 0x{m0 & 0xFFFF:x}")
    elif ty == 0xA2:
        extra = f" wptr 0x{m0 & 0xFFFFFFFF:x}"
    elif ty == 0x85:
        extra = f" (low 48: 0x{m0 & 0xFFFFFFFFFFFF:x})"
    else:
        extra = ""
    return f"{'I2A' if i2a else 'A2I'} {name}{extra}"


class Decoder:
    def __init__(self, out, sep_mbox=None):
        self.out = out
        self.sep_mbox = sep_mbox
        self.tl = open(os.path.join(out, "timeline.txt"), "w")
        self.sep_names = {}
        self.intf = {}  # (dcp, ep, intf) -> service name
        self.frag = {}  # (dcp, ep, ring, intf) -> [total, bytes]
        self.open = {}  # seq -> an AFK copy waiting for its words
        self.sepcur = None
        self.names = {}  # dcp index -> ADT name (DAFN)
        self.lines = []  # (time, n, text): sorted by time at the end

    def w(self, s, ts=None):
        self.lines.append((ts if ts is not None else float("inf"), len(self.lines), s))

    def dname(self, n):
        return self.names.get(n, f"dcp{n}")

    # ---- the EPIC side of one queue entry
    def epic(self, key, body, ring):
        dcp, ep = key
        if len(body) < 16:
            return "short"
        magic, size, ch, ety = struct.unpack_from("<4I", body, 0)
        if magic != QE_MAGIC:
            return f"bad magic 0x{magic:x}"
        d = body[16:]
        head = f"size {size}"
        if ch or ety:
            if len(d) < 40:
                return head + f" classic ch {ch} type {ety} (short)"
            ver, seq = d[0], struct.unpack_from("<H", d, 1)[0]
            ln, sver, cat, sty = struct.unpack_from("<IBBH", d, 16)
            tag, unk, inl = struct.unpack_from("<HHI", d, 32)
            pl = d[40:]
            extra = ""
            if sty == 0xC0 and len(pl) >= 16 and u32(pl, 12) == 0x69706378:
                grp, cmd, dl = struct.unpack_from("<HII", pl, 2)
                extra = f"service call group 0x{grp:x} command 0x{cmd:x} data_len {dl} | "
                pl = pl[64:]
            elif cat in (0x20, 0x30) and len(pl) >= 30:
                rc, rxb, txb, rxl, txl = struct.unpack_from("<IQQII", pl, 0)
                extra = (f"cmd retcode 0x{rc:x} rx 0x{rxb:x}/0x{rxl:x} tx 0x{txb:x}/0x{txl:x} | ")
                pl = pl[30:]
            return (head + f" classic ch {ch} type {ety} v{ver} seq {seq} | len {ln} v{sver} "
                    f"cat 0x{cat:02x} type 0x{sty:x} tag 0x{tag:x} unk 0x{unk:x} inline {inl} | "
                    + extra + self.payload(pl))
        if len(d) < 8:
            return head + " v2 (short)"
        seq, fl, intf, length = struct.unpack_from("<BBHI", d, 0)
        fk = (dcp, ep, ring, intf)
        if fk in self.frag:
            st = self.frag[fk]
            st[1] += d[8:]
            done = len(st[1]) >= st[0]
            s = head + f" v2 fragment intf {intf} seq {seq} +{len(d) - 8} ({len(st[1])} of {st[0]})"
            if done:
                del self.frag[fk]
                s += " | assembled: " + self.v2(dcp, ep, seq, fl, intf, st[0], st[1][:st[0]])
            return s
        if length > len(d) - 8 and len(d) == size:
            self.frag[fk] = [length, bytearray(d[8:])]
            return head + f" v2 first fragment intf {intf} seq {seq} {len(d) - 8} of {length}"
        return head + " " + self.v2(dcp, ep, seq, fl, intf, length, d[8:8 + length])

    def v2(self, dcp, ep, seq, fl, intf, length, m):
        # m: token(8) type cat iflags(2) reserved(4), then the payload
        if len(m) < 16:
            return f"v2 seq {seq} intf {intf} len {length} (short)"
        token, ty, cat, ifl, res = struct.unpack_from("<QBBHI", m, 0)
        pl = m[16:]
        if cat == 0 and ty == 0x11 and len(pl) >= 32:
            self.intf[(dcp, ep, intf)] = pl[:32].split(b"\0")[0].decode(errors="replace")
        name = self.intf.get((dcp, ep, intf), "")
        catn = V2CAT.get(cat, f"cat {cat}")
        s = (f"v2 seq {seq} intf {intf} {name} flags 0x{fl:x} len {length} {catn} type 0x{ty:x} "
             f"token 0x{token:x} iflags 0x{ifl:x} res 0x{res:x}")
        if cat in (1, 2) and len(pl) >= 8 and not pl[0] & ~1 and not pl[2] and not pl[3]:
            oob, cid, val = pl[0] & 1, pl[1], u32(pl, 4)
            if oob and len(pl) >= 32:
                rx, tx, rxs, txs = struct.unpack_from("<QQII", pl, 8)
                s += (f" | oob id 0x{cid:x} value 0x{val:x} rx 0x{rx:x}/0x{rxs:x} "
                      f"tx 0x{tx:x}/0x{txs:x}")
                pl = pl[32:]
            else:
                s += f" | inline id 0x{cid:x} value 0x{val:x}"
                pl = pl[8:]
        return s + " | " + self.payload(pl)

    @staticmethod
    def payload(pl):
        h = bytes(pl[:HEXMAX]).hex()
        return f"payload {len(pl)}: {h}" + (" ..." if len(pl) > HEXMAX else "")

    # ---- a finished copy
    def flush(self, c):
        raw = b"".join(c["words"])
        data = raw[c["skip"]:c["skip"] + c["bytes"]]
        fn = (f"{c['seq']:06d}-dcp{c['dcp']}-ep{c['ep']:02x}-{'a2d' if c['ring'] == 0 else 'd2a'}-"
              f"{KIND.get(c['kind'], 'x')}.bin")
        open(os.path.join(self.out, "dcp", fn), "wb").write(data)
        pre = (f"{c['ts']:12.6f} c{c['cpu']} {self.dname(c['dcp'])} ep 0x{c['ep']:02x} "
               f"{c['dir']}")
        notes = ""
        if c["full"] > c["bytes"]:
            notes += f" TRUNCATED {c['bytes']} of {c['full']}"
        if len(data) < c["bytes"]:
            notes += f" ({len(data)} of {c['bytes']} bytes: words missing from the ring)"
        if c["kind"] == 0:
            txt = self.epic((c["dcp"], c["ep"]), data, c["ring"]) if len(data) == c["bytes"] \
                else "(incomplete)"
            self.w(f"{pre} qe@0x{c['pa']:x} {txt}{notes} -> {fn}", c["ts"])
        else:
            self.w(f"{pre} {KIND.get(c['kind'], '?')} dva 0x{c['pa']:x} {len(data)} bytes{notes}: "
                   f"{data[:HEXMAX].hex()} -> {fn}", c["ts"])

    def sep_flush(self):
        if self.sepcur:
            seq, fn, chunks = self.sepcur
            open(os.path.join(self.out, "dcp", fn), "wb").write(b"".join(chunks))
            self.sepcur = None

    def record(self, ts, cpu, meta, val, pc):
        pa = meta & 0xFFFFFFFFFFFF
        lo, hi = pc & 0xFFFFFFFF, pc >> 32
        tag = TAGS.get(lo)
        if tag == "DAFD":
            c = self.open.get(hi)
            if c and pa == 8 * len(c["words"]):
                c["words"].append(struct.pack("<Q", val))
                if len(c["words"]) >= c["nwords"]:
                    self.flush(self.open.pop(hi))
            return
        if lo == SEPD:
            if self.sepcur and hi == self.sepcur[0]:
                self.sepcur[2].append(struct.pack("<Q", val))
            return
        if lo != SEPD:
            self.sep_flush()
        if (meta >> 55) & 1:
            name = struct.pack("<Q", pc).rstrip(b"\0").decode(errors="replace")
            self.w(f"{ts:12.6f} c{cpu} MARK {name} 0x{val:x}", ts)
            return
        if tag == "DAFH":
            dcp, ring, kind, ep = val & 15, (val >> 4) & 1, (val >> 5) & 7, (val >> 8) & 0xFF
            nb, skip = (val >> 16) & 0xFFFFFF, pa & 7 if kind else 0
            old = self.open.pop(hi, None)  # a copy whose words never all came: show it anyway
            if old:
                self.flush(old)
            c = dict(ts=ts, cpu=cpu, seq=hi, dcp=dcp, ring=ring, kind=kind, ep=ep, bytes=nb,
                     full=val >> 40, pa=pa, skip=skip, words=[], nwords=(skip + nb + 7) // 8,
                     dir="AP->DCP" if ring == 0 else "DCP->AP")
            if c["nwords"]:
                self.open[hi] = c
            else:
                self.flush(c)
            return
        if tag:
            dcp, b4, ep = hi & 15, (hi >> 4) & 15, (hi >> 8) & 0xFF
            if tag == "DAFN":
                self.names[dcp] = struct.pack("<Q", val).rstrip(b"\0").decode(errors="replace")
                self.w(f"{ts:12.6f} c{cpu} DCP {dcp} = {self.names[dcp]} mailbox 0x{pa:x}", ts)
                return
            pre = f"{ts:12.6f} c{cpu} {self.dname(dcp)} ep 0x{ep:02x}"
            if tag == "DAFM":
                self.w(f"{pre} {msg_text(val, b4 & 1)}  (msg 0x{val:016x} pc 0x{pa:x})", ts)
            elif tag == "DAFR":
                self.w(f"{pre} RING {'rx' if b4 & 1 else 'tx'} bufsz 0x{val & 0xFFFFFFFF:x} "
                       f"stride 0x{val >> 32:x} dva 0x{pa:x}", ts)
            elif tag == "DAFX":
                self.w(f"{pre} PROBLEM {b4} {PROBLEM.get(b4, '')} (0x{val:x})", ts)
            return
        if lo == SEPH:
            ep, buf, reply, words = val & 0xFF, (val >> 8) & 1, (val >> 9) & 1, val >> 16
            fn = f"sep-{hi:06d}-ep{ep:02x}-{'out' if buf else 'in'}-{'reply' if reply else 'send'}.bin"
            self.w(f"{ts:12.6f} c{cpu} SEP BUF seq {hi} ep 0x{ep:02x} "
                   f"{self.sep_names.get(ep, '')} {'out' if buf else 'in'} "
                   f"@{'reply' if reply else 'send'} iova 0x{pa:x} {words * 8} bytes -> {fn}", ts)
            self.sepcur = (hi, fn, [])
            return
        off = pa & 0xFFF
        if off not in (0x800, 0x830):
            return
        if self.sep_mbox is not None and (pa & ~0x3FFF) != self.sep_mbox:
            return
        ep, stag, ty, param, d = val & 0xFF, (val >> 8) & 0xFF, (val >> 16) & 0xFF, \
            (val >> 24) & 0xFF, val >> 32
        if ep == 0xFD and ty == 0:
            self.sep_names[param] = struct.pack("<I", d).decode(errors="replace")[::-1]
        self.w(f"{ts:12.6f} c{cpu} SEP {'A2I' if off == 0x800 else 'I2A'} ep 0x{ep:02x} "
               f"{self.sep_names.get(ep, ''):4} tag 0x{stag:02x} type 0x{ty:02x} "
               f"param 0x{param:02x} data 0x{d:08x}  (msg 0x{val:016x})", ts)

    def finish(self):
        for seq in sorted(self.open):
            self.flush(self.open[seq])
        self.open = {}
        self.sep_flush()
        for (dcp, ep, ring, intf), st in self.frag.items():
            self.w(f"# dcp{dcp} ep 0x{ep:02x} ring {ring} intf {intf}: fragmented message "
                   f"left at {len(st[1])} of {st[0]} bytes")
        self.w("# interfaces: " + ", ".join(f"dcp{d} ep 0x{e:02x} intf {i}={n}"
                                            for (d, e, i), n in sorted(self.intf.items())))
        self.w("# SEP endpoints: " + ", ".join(f"0x{k:02x}={v}"
                                              for k, v in sorted(self.sep_names.items())))
        head = [ln for ln in self.lines if ln[2].startswith("# ") and ln[0] == float("-inf")]
        rest = sorted(ln for ln in self.lines if ln not in head)
        for _, _, text in head + rest:
            self.tl.write(text + "\n")
        self.tl.close()


def main():
    args = sys.argv[1:]
    sep_mbox = None
    if "--sep-mbox" in args:
        i = args.index("--sep-mbox")
        sep_mbox = int(args[i + 1], 0) & ~0x3FFF
        del args[i:i + 2]
    src, out = args
    os.makedirs(os.path.join(out, "dcp"), exist_ok=True)
    data = open(src, "rb").read()
    magic, ver, rsz, frq, first, count, head, lost, build = HDR.unpack_from(data, 0)
    assert magic == b"AURATRC1" and rsz == 32, (magic, rsz)
    count = min(count, (len(data) - HDR.size) // 32)
    dec = Decoder(out, sep_mbox)
    dec.w(f"# {src}: {count} records from seq {first}, lost {lost}, cntfrq {frq}, "
          f"build {build.rstrip(bytes(1)).decode(errors='replace')}", float("-inf"))
    dec.w("# columns: time(s) cpu dcp ep dir | AFK message, or queue entry: AFKv2 frame (seq, "
          "intf, flags, len, category, type, token, iflags; command id/value, OOB rx/tx) or classic "
          "EPIC (channel, type, ver, seq | sub-header len, cat, type, tag, inline; service call "
          "group/command) | payload <len>: hex", float("-inf"))
    t0 = None
    for i in range(count):
        t, meta, val, pc = REC.unpack_from(data, HDR.size + i * 32)
        t0 = t if t0 is None else t0
        cpu = ((meta >> 51) & 15) | (((meta >> 56) & 3) << 4)
        dec.record((t - t0) / frq, cpu, meta, val, pc)
    dec.finish()


if __name__ == "__main__":
    main()
