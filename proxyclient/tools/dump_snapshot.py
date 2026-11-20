#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the GPU firmware snapshots and the GPU mailbox log out of a parked SPTM-virt HV
(src/sptm_virt_snap.c, src/sptm_virt_gpu.c), and lay them out as the GPU lane's fw272-b2dump.py does.

    export AURADEVICE=<the parked HV's proxy (if00)>
    dump_snapshot.py --elf build/aura-raw.elf --out DIR
    dump_snapshot.py --from-bin DIR/snapshot.bin [--mbox-bin DIR/mbox.bin] --out DIR2   # offline

DIR gets snapshot.bin and mbox.bin (raw), summary.txt (each snapshot: the doorbell, the watched
words such as HW data +0xfa0/+0xfa8, flags), mbox.txt (every GPU mailbox message, EP 0x21 marked
">>"), and the first snapshot (the B2 one) in fw272-b2dump.py's layout: tables.bin, shared-l2.bin,
ttbs.bin, handoff.bin, walk.txt, objects/<path>.bin, objects.txt, plus chains/<name>.bin and
chains.txt (the host's pointer chains, copied whole). Later snapshots go in kick-<n>/ the same way.
Read-only: the proxy reads memory, nothing else.
"""
import argparse
import os
import pathlib
import struct
import sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura import hvsnap  # noqa: E402


def read_hv(args):
    from aura import hvpark
    from aura.proxy import START, AuraProxy, UartInterface
    from aura.proxyutils import bootstrap_port

    iface = UartInterface()
    for code in hvpark.CODES:
        iface.set_handler(START.HV, code, lambda *a: None)
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    sym = hvpark.Symbols(args.elf, p.get_base(), args.nm)
    hvpark.check_image(iface.readmem, sym, ("sptm_virt_snap_take", "sptm_virt_park_proxy"))
    rd64 = lambda a: struct.unpack("<Q", iface.readmem(a, 8))[0]  # noqa: E731
    rd32 = lambda a: struct.unpack("<I", iface.readmem(a, 4))[0]  # noqa: E731

    def bulk(addr, size):
        return b"".join(iface.readmem(addr + o, min(1 << 20, size - o))
                        for o in range(0, size, 1 << 20))

    snap = b""
    buf = rd64(sym["sptm_virt_snap_buf"])
    if buf:
        used = hvsnap.HDR.unpack(iface.readmem(buf, hvsnap.HDR.size))[4]
        snap = bulk(buf, min(used, rd64(sym["sptm_virt_snap_bytes"])))
    else:
        print("no snapshot buffer in this boot (AURA_GPU_SNAP unset)")
    mbox = b""
    mb = rd64(sym["sptm_virt_gpu_mbox"])
    if mb:
        n = min(rd32(sym["sptm_virt_gpu_mbox_n"]), rd32(sym["sptm_virt_gpu_mbox_cap"]))
        mbox = bulk(mb, n * hvsnap.MBOX.size) if n else b""
    else:
        print("no mailbox log in this boot (AURA_GPU_MBOX unset)")
    return snap, mbox


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", type=pathlib.Path, help="aura-raw.elf of the running image")
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--from-bin", type=pathlib.Path, help="a saved snapshot.bin, no device")
    ap.add_argument("--mbox-bin", type=pathlib.Path, help="with --from-bin: a saved mbox.bin")
    ap.add_argument("--out", type=pathlib.Path, required=True)
    args = ap.parse_args()
    if args.from_bin:
        snap = args.from_bin.read_bytes()
        mbox = args.mbox_bin.read_bytes() if args.mbox_bin else b""
    else:
        if not args.elf or not os.environ.get("AURADEVICE"):
            sys.exit("reading the HV needs --elf and AURADEVICE (or --from-bin)")
        snap, mbox = read_hv(args)
    out = args.out
    out.mkdir(parents=True, exist_ok=True)
    text = ""
    if snap:
        if not args.from_bin:
            (out / "snapshot.bin").write_bytes(snap)
        h, snaps = hvsnap.parse(snap)
        for s in snaps:
            hvsnap.write_layout(s, out if s.index == 0 else out / f"kick-{s.index}")
        text = hvsnap.summary(h, snaps)
    if mbox:
        if not args.mbox_bin:
            (out / "mbox.bin").write_bytes(mbox)
        lines = hvsnap.mbox_lines(mbox)
        (out / "mbox.txt").write_text("\n".join(lines) + "\n")
        n21 = sum(line.startswith(">>") for line in lines)
        text += f"mailbox log: {len(lines)} messages, {n21} on EP 0x21 -> mbox.txt\n"
    (out / "summary.txt").write_text(text)
    print(text, end="")
    print(f"-> {out}")


if __name__ == "__main__":
    main()
