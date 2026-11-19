#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the GPU trace ring (src/sptm_virt_gpu.c) out of a parked SPTM-virt guest, every record, oldest first.

The ring holds AURA_GPU_TRACE's and AURA_TRACE_NODES's records (32 bytes: t | meta = pa[47:0] width[50:48]
write[51] cpu[55:52] | val | guest pc); aura prints only a budgeted part of it. Park the guest first, then:

    python3 proxyclient/tools/dump_gpu_ring.py --elf build/aura-raw.elf --out DIR

Writes DIR/gpu.bin (raw) and DIR/gpu.txt (decoded, with the ADT node each address belongs to); dump_guest_mem.py
--ring DIR/gpu.txt takes a powered-off DART's TCR/TTBR from it. Read-only: the proxy reads memory, nothing else.
The S2R ring has its own reader, tools/dump_s2r_ring.py.
"""
import argparse, bisect, os, pathlib, struct, subprocess, sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

ap = argparse.ArgumentParser()
ap.add_argument("--elf", type=pathlib.Path, required=True, help="aura-raw.elf of the running SPTM_VIRT build")
ap.add_argument("--nm", default="nm")
ap.add_argument("--out", type=pathlib.Path, required=True)
ap.add_argument("--max-mib", type=int, default=72, help="cap per ring (default: whole ring)")
args = ap.parse_args()
if not os.environ.get("AURADEVICE"):
    sys.exit("set AURADEVICE first")

from aura.proxy import AuraProxy, UartInterface  # noqa: E402
from aura.proxyutils import ProxyUtils, bootstrap_port  # noqa: E402

iface = UartInterface()
p = AuraProxy(iface, debug=False)
bootstrap_port(iface, p)
u = ProxyUtils(p)
args.out.mkdir(parents=True, exist_ok=True)

syms = {}
for line in subprocess.check_output([args.nm, str(args.elf)], text=True).splitlines():
    f = line.split()
    if len(f) == 3:
        syms[f[2]] = int(f[0], 16)
base = p.get_base()
at = lambda n: base + syms[n] - syms["_base"]
rd64 = lambda a: struct.unpack("<Q", iface.readmem(a, 8))[0]

# ADT ranges -> names, so every record says which block it touched.
ranges = []
def walk(n):
    try:
        for i in range(len(n.reg)):
            a, s = n.get_reg(i)
            ranges.append((a, a + s, f"{n._path}[{i}]"))
    except Exception:
        pass
    for c in n:
        walk(c)
walk(u.adt["/arm-io"])
ranges.sort()
starts = [r[0] for r in ranges]
def where(pa):
    i = bisect.bisect_right(starts, pa) - 1
    while i >= 0 and ranges[i][0] <= pa:
        if pa < ranges[i][1]:
            return f"{ranges[i][2]}+{pa - ranges[i][0]:#x}"
        i -= 1
    return "?"

RINGS = {
    "gpu": ("sptm_virt_gpu_ring", "sptm_virt_gpu_nrec", "sptm_virt_gpu_head"),
}
for name, (rs, ns, hs) in RINGS.items():
    if rs not in syms:
        print(f"{name}: not in this build"); continue
    ring, nrec, head = rd64(at(rs)), rd64(at(ns)), rd64(at(hs))
    if not ring or not nrec:
        print(f"{name}: ring not allocated (switch off in this session)"); continue
    have = min(head, nrec, (args.max_mib << 20) // 32)
    first = head - have  # oldest kept record
    print(f"{name}: ring {ring:#x}, {head} records written, capacity {nrec}, reading {have}")
    raw = bytearray()
    idx = first % nrec
    left = have
    while left:
        n = min(left, nrec - idx, 8192)
        raw += iface.readmem(ring + idx * 32, n * 32)
        idx = (idx + n) % nrec
        left -= n
    (args.out / f"{name}.bin").write_bytes(raw)
    with open(args.out / f"{name}.txt", "w") as f:
        f.write(f"# {name} ring: records {first}..{head - 1} (oldest first); t = CNTPCT\n")
        for k in range(have):
            t, meta, val, pc = struct.unpack_from("<4Q", raw, k * 32)
            pa = meta & ((1 << 48) - 1)
            width, wr, cpu = 8 << ((meta >> 48) & 7), (meta >> 51) & 1, (meta >> 52) & 15
            f.write(f"{first + k} t={t:#x} c{cpu} {'W' if wr else 'R'}{width} {pa:#x} = {val:#x} pc={pc:#x}  "
                    f"{where(pa)}\n")
    print(f"  -> {args.out}/{name}.txt")
