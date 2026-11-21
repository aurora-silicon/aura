#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Read the block and IRQ traces (src/sptm_virt_trace.c: AURA_TRACE_BLKS, AURA_IRQ_TRACE) out of a
virtualized-SPTM guest that keeps running, and decode them.

    export AURADEVICE=<the HV's proxy device (if00)>
    # one read: ask for a park, read both rings into DIR, drain them, let the guest go on
    sptm_trace.py --elf IMG/aura-raw.elf --out DIR --request --continue --tail 5
    # a park already held (run_guest_sptm's AURA_PARK_AFTER_SECS, sptm_park.py --request):
    sptm_trace.py --elf IMG/aura-raw.elf --out DIR [--continue]
    # every 60 s for 15 minutes: park, read, drain, resume; the console in DIR/console.txt
    sptm_trace.py --elf IMG/aura-raw.elf --out DIR --every 60 --for 900
    # offline (also done after every read): DIR/summary.txt and DIR/timeline.txt
    sptm_trace.py --decode DIR [--adt adt.bin] [--irq 892,894,895] [--blocks ptd,gpio]

A read appends the records it read to DIR (aura/hvtrace.py's capture directory), then drains them
in aura, so the next park reads only what came after; --no-drain leaves them in aura (the next
read starts after them all the same). A park reads at most --max-mib of each ring and leaves the
rest for the next one: a park stops every guest core until P_EXIT. --continue (and the loop)
refuse a one-way park, where P_EXIT would end the proxy for good.
"""

import argparse
import os
import pathlib
import sys
import time

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura import hvpark, hvtrace  # noqa: E402


def decode(args, out):
    names = {}
    if args.adt:
        from aura.adt import load_adt
        names = hvtrace.irq_names(load_adt(args.adt.read_bytes()))
    want = [int(x, 0) for x in args.irq.split(",") if x] if args.irq else []
    blks = set(args.blocks.split(",")) if args.blocks else None
    text = hvtrace.report(out, names, want, blks)
    print(text, end="")
    print(f"-> {out}/summary.txt, {out}/timeline.txt")


class Link:
    """The proxy device, its console and the parked HV's proxy."""

    def __init__(self, args):
        from aura.proxy import START, UartInterface

        self.args = args
        self.iface = UartInterface()
        for code in hvpark.CODES:  # a START that arrives after the open is the park's
            self.iface.set_handler(START.HV, code, lambda *a: None)
        self.log = open(args.out / "console.txt", "a") if args.out else None
        self.sym = None

    def emit(self, line):
        print("TTY> " + line, flush=True)
        if self.log:
            self.log.write(line + "\n")
            self.log.flush()

    def tail(self, secs, request=False):
        """The console for secs, or until a park; request: ask for one first."""
        to, self.iface.dev.timeout = self.iface.dev.timeout, 0.2
        try:
            if request:
                hvpark.request(self.iface.dev)
                print(f"park requested; waiting up to {secs:g} s for START", flush=True)
            tail = hvpark.ConsoleTail(self.iface.dev, self.emit)
            res = tail.run(secs)
            if res[0] != "park":
                tail.flush()
            return res
        finally:
            self.iface.dev.timeout = to

    def proxy(self):
        from aura.proxy import AuraProxy
        from aura.proxyutils import bootstrap_port

        p = AuraProxy(self.iface, debug=False)
        bootstrap_port(self.iface, p)
        if self.sym is None:
            self.sym = hvpark.Symbols(self.args.elf, p.get_base(), self.args.nm)
            if "sptm_virt_trace_drain" not in self.sym:
                raise SystemExit(f"{self.args.elf} has no block/IRQ trace (sptm_virt_trace_drain)")
            hvpark.check_image(self.iface.readmem, self.sym,
                               ("sptm_virt_trace_drain", "sptm_virt_park_proxy"))
        return p

    def read(self, p, cont):
        info = hvpark.park_info(self.iface.readmem, self.sym)
        state = hvpark.PARK_STATES.get(info["state"], str(info["state"]))
        print(f"park: {state}; owner cpu{info['cpu']} "
              f"({hvpark.CODES.get(info['code'], hex(info['code']))}), cores stopped "
              f"0x{info['waiting']:x}, still inside aura 0x{info['stragglers']:x}", flush=True)
        t0 = time.time()
        got = hvtrace.capture(self.iface.readmem, p.call, self.sym, self.args.out,
                              drain=not self.args.no_drain, max_bytes=self.args.max_mib << 20)
        if not got:
            print("no trace ring in this boot (AURA_TRACE_BLKS/AURA_IRQ_TRACE unset, or refused: "
                  "see the 'SPTM-virt[trace]' lines in the console)")
        print(f"read in {time.time() - t0:.1f} s", flush=True)
        if cont:
            if info["state"] != 1:
                raise SystemExit(f"refused: the park is {state}; P_EXIT would end its proxy for "
                                 f"good")
            p.exit(0)
            print("P_EXIT sent: the guest goes on", flush=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", type=pathlib.Path, help="aura-raw.elf of the running image")
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--out", type=pathlib.Path, help="the capture directory (appended to)")
    ap.add_argument("--request", action="store_true", help="ask the running guest to park first")
    ap.add_argument("--timeout", type=float, default=30, help="seconds to wait for its START")
    ap.add_argument("--continue", dest="cont", action="store_true", help="P_EXIT after the read")
    ap.add_argument("--tail", type=float, default=0, help="then follow the console this long")
    ap.add_argument("--every", type=float, help="loop: a park, a read and a resume every N s")
    ap.add_argument("--for", dest="duration", type=float, default=600, help="... for this long")
    ap.add_argument("--no-drain", action="store_true", help="leave the records in aura")
    ap.add_argument("--max-mib", type=int, default=8, help="per ring per park (default 8)")
    ap.add_argument("--decode", type=pathlib.Path, help="decode a capture directory: no device")
    ap.add_argument("--adt", type=pathlib.Path, help="an ADT blob: name the IRQs by device")
    ap.add_argument("--irq", help="IRQs to mark and to keep in the timeline, e.g. 892,894,895")
    ap.add_argument("--blocks", help="blocks to keep in the timeline, e.g. ptd,gpio")
    args = ap.parse_args()

    if args.decode:
        decode(args, args.decode)
        return
    if not os.environ.get("AURADEVICE"):
        sys.exit("set AURADEVICE first (or --decode)")
    if not args.elf or not args.out:
        sys.exit("reading the HV needs --elf (its aura-raw.elf) and --out")
    args.out.mkdir(parents=True, exist_ok=True)
    link = Link(args)
    if args.every:
        end = time.time() + args.duration
        n = 0
        while time.time() < end:
            res = link.tail(args.timeout, request=True)
            if res[0] != "park":
                print(f"--- no park within {args.timeout:g} s ({res[0]}); trying again ---",
                      flush=True)
                continue
            n += 1
            print(f"--- read {n}: {res[1]} ---", flush=True)
            link.read(link.proxy(), True)
            res = link.tail(max(0.0, min(args.every, end - time.time())))
            if res[0] == "park":
                print(f"--- the HV parked on its own: {res[1]}; stopping the loop ---")
                break
        decode(args, args.out)
        return
    if args.request:
        res = link.tail(args.timeout, request=True)
        if res[0] != "park":
            sys.exit(f"no park within {args.timeout:g} s ({res[0]})")
        print(f"parked: {res[1]}", flush=True)
    link.read(link.proxy(), args.cont)
    decode(args, args.out)
    if args.cont and args.tail:
        res = link.tail(args.tail)
        if res[0] == "park":
            print(f"--- the HV parked again: {res[1]} ---")


if __name__ == "__main__":
    main()
