#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read a tools/dump_guest_mem.py dump offline (no device needed).

    guest_mem.py DUMP summary                        targets, streams, mapped/read sizes, faults
    guest_mem.py DUMP maps [TARGET] [--all]          IOVA runs (contiguous) per target/stream, with how much was read
    guest_mem.py DUMP x TARGET[:STREAM] IOVA [LEN]   hexdump through that target's page tables (LEN default 0x100)
    guest_mem.py DUMP pa PA [LEN]                    hexdump by physical address
    guest_mem.py DUMP find HEX [--width 8]           where a value (pointer, magic) occurs in the read memory, as
                                                 target IOVAs; e.g. an IOVA from the MMIO trace ring
    guest_mem.py DUMP save TARGET[:STREAM] IOVA LEN FILE

TARGET is the first column of maps.tsv (dart-ane/DARTLLT, gpu/0,kernel-gfx, ...) or a unique prefix of it.
As a module: d = Dump(path); d.read_iova("dart-ane", iova, n, stream=0); d.read_pa(pa, n); d.runs().
"""
import bisect, collections, json, mmap, pathlib, struct, sys

PAGE = 0x4000


class Dump:
    def __init__(self, path):
        self.dir = pathlib.Path(path)
        self.maps = []
        for l in open(self.dir / "maps.tsv"):
            if l.startswith("#"):
                continue
            t, inst, st, iova, pa, size, flags, kind = l.rstrip("\n").split("\t")
            self.maps.append((t, inst, st, int(iova, 16), int(pa, 16), int(size, 16), int(flags, 16), kind))
        self.idx = []
        if (self.dir / "mem.idx").exists():
            for l in open(self.dir / "mem.idx"):
                if l.startswith("#"):
                    continue
                pa, off, n, st = l.rstrip("\n").split("\t")
                self.idx.append((int(pa, 16), int(off, 16), int(n, 16), st))
        self.idx.sort()
        self._starts = [x[0] for x in self.idx]
        mb = self.dir / "mem.bin"
        self._f = open(mb, "rb") if mb.exists() and mb.stat().st_size else None
        self.mem = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ) if self._f else b""
        self.by_target = collections.defaultdict(dict)  # (target, stream) -> {iova page: pa}
        for t, _, st, iova, pa, size, _, kind in self.maps:
            self.by_target[(t, st)][iova] = pa
        self.summary = json.loads((self.dir / "summary.json").read_text()) if (self.dir / "summary.json").exists() else {}

    def targets(self):
        return sorted({t for t, _ in self.by_target})

    def resolve(self, spec):
        """'dart-ane' or 'dart-ane/DARTLLT:0' -> (target, stream)."""
        name, _, st = spec.partition(":")
        exact = [k for k in self.by_target if k[0] == name and (not st or k[1] == st)]
        cands = exact or [k for k in self.by_target if k[0].startswith(name) and (not st or k[1] == st)]
        names = {k[0] for k in cands}
        if len(names) != 1:
            raise SystemExit(f"{spec}: {'no' if not names else 'ambiguous'} target ({', '.join(sorted(names)) or '-'})")
        if len(cands) > 1 and not st:
            # one target, several streams: the one with the most mappings
            cands.sort(key=lambda k: -len(self.by_target[k]))
        return cands[0]

    def read_pa(self, pa, n):
        """Bytes at pa, or None where not read (whole result None if any page is missing)."""
        out = bytearray()
        while n > 0:
            i = bisect.bisect_right(self._starts, pa) - 1
            if i < 0:
                return None
            base, off, size, st = self.idx[i]
            if not base <= pa < base + size or st != "ok":
                return None
            k = min(n, base + size - pa)
            out += self.mem[off + pa - base: off + pa - base + k]
            pa += k
            n -= k
        return bytes(out)

    def translate(self, target, stream, iova):
        page = self.by_target[(target, stream)].get(iova & ~(PAGE - 1))
        return None if page is None else page + (iova & (PAGE - 1))

    def read_iova(self, target, iova, n, stream=None):
        key = self.resolve(target if stream is None else f"{target}:{stream}")
        out = bytearray()
        while n > 0:
            pa = self.translate(*key, iova)
            k = min(n, PAGE - (iova & (PAGE - 1)))
            d = self.read_pa(pa, k) if pa is not None else None
            if d is None:
                return None if not out else bytes(out)
            out += d
            iova += k
            n -= k
        return bytes(out)

    def runs(self, target=None):
        """IOVA-contiguous runs: (target, stream, iova, size, bytes read, kinds)."""
        res = []
        cur = None
        for t, _, st, iova, pa, size, _, kind in sorted(self.maps, key=lambda m: (m[0], m[2], m[3])):
            if target and not t.startswith(target):
                continue
            got = size if kind == "ram" and self.read_pa(pa, 1) is not None else 0
            if cur and cur[0] == t and cur[1] == st and cur[2] + cur[3] == iova:
                cur[3] += size
                cur[4] += got
                cur[5].add(kind)
            else:
                cur = [t, st, iova, size, got, {kind}]
                res.append(cur)
        return res

    def iova_of_pa(self, pa):
        hits = []
        page = pa & ~(PAGE - 1)
        for (t, st), m in self.by_target.items():
            for iova, p in m.items():
                if p == page:
                    hits.append((t, st, iova + (pa - page)))
        return hits


def hexdump(data, base):
    for i in range(0, len(data), 16):
        c = data[i:i + 16]
        words = " ".join(c[j:j + 4][::-1].hex() for j in range(0, len(c), 4))
        txt = "".join(chr(b) if 32 <= b < 127 else "." for b in c)
        print(f"{base + i:016x}  {words:<35}  {txt}")


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    d = Dump(argv[0])
    cmd, rest = argv[1], argv[2:]
    if cmd == "summary":
        s = d.summary
        print(f"read {s.get('read_bytes', 0) >> 20} MiB of {s.get('mapped_pages', 0) * PAGE >> 20} MiB mapped, "
              f"{s.get('faulted', 0)} faulted pages, {s.get('over_budget', 0)} pages over budget, {s.get('secs')} s")
        for t, v in sorted(s.get("per_target", {}).items()):
            print(f"  {t:40s} {v['pages'] * PAGE >> 10:>9} KiB mapped  mmio pages {v['mmio']:>5}  listed-only {v['listed']}")
        for p in s.get("problems", []):
            print("  problem:", p)
    elif cmd == "maps":
        for t, st, iova, size, got, kinds in d.runs(rest[0] if rest and not rest[0].startswith("-") else None):
            if size < PAGE * 2 and "--all" not in rest and not got:
                continue
            print(f"{t:36s} {st:>8} {iova:#018x} {size:#10x} read {got:#10x} {','.join(sorted(kinds))}")
    elif cmd == "x":
        key = d.resolve(rest[0])
        iova, n = int(rest[1], 16), int(rest[2], 16) if len(rest) > 2 else 0x100
        data = d.read_iova(key[0], iova, n, stream=key[1])
        if data is None:
            sys.exit(f"{iova:#x}: not mapped or not read in {key[0]}:{key[1]}")
        hexdump(data, iova)
    elif cmd == "pa":
        pa, n = int(rest[0], 16), int(rest[1], 16) if len(rest) > 1 else 0x100
        data = d.read_pa(pa, n)
        if data is None:
            sys.exit(f"{pa:#x}: not read")
        hexdump(data, pa)
    elif cmd == "find":
        width = int(rest[rest.index("--width") + 1]) if "--width" in rest else 8
        val = int(rest[0], 16)
        needle = val.to_bytes(width, "little")
        pos = d.mem.find(needle) if d.mem else -1
        shown = 0
        while pos >= 0 and shown < 50:
            for pa, off, n, st in d.idx:
                if off <= pos < off + n:
                    hit = pa + pos - off
                    where = ", ".join(f"{t}:{st_} {iova:#x}" for t, st_, iova in d.iova_of_pa(hit)[:3])
                    print(f"pa {hit:#x}  {where}")
                    break
            shown += 1
            pos = d.mem.find(needle, pos + 1)
    elif cmd == "save":
        key = d.resolve(rest[0])
        data = d.read_iova(key[0], int(rest[1], 16), int(rest[2], 16), stream=key[1])
        if data is None:
            sys.exit("not mapped or not read")
        pathlib.Path(rest[3]).write_bytes(data)
        print(f"{len(data)} bytes -> {rest[3]}")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
