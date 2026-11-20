#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Stop a virtualized-SPTM guest in aura's proxy, look at it, and let it go on (src/sptm_virt_park.c).

    export AURADEVICE=<the HV's proxy device (if00)>
    sptm_park.py --request                       # ask the running guest to park; wait for START
    sptm_park.py --elf build/aura-raw.elf --status
    dump_gpu_ring.py / dump_snapshot.py / ...   # anything else that reads the parked HV
    sptm_park.py --elf build/aura-raw.elf --continue --tail 60

The steps combine in that order in one call: --request --restore-nvram-write ... --continue.
Closing the port doesn't end a park: the HV waits for the next opener and sends START again.

--continue sends P_EXIT, which ends a park that resumes: the parked core releases the others and
returns into the trap it parked in. It refuses a one-way park (the reset guard's, SMP's, the sleep
path's), where P_EXIT would stop the proxy for good, unless --force.

--restore-nvram-write (off unless given) is the late NVRAM write enable: the guest booted with the
nvram_ro quirk (AppleARMNORNVRAM::write returns success without writing), and in a park that
resumes, with every guest core stopped, XNU's original words go back at those sites. The sites come
from run_guest_sptm.py's AURA_XNU_PATCH_LOG file (--patch-log) or its lines in hv.log (--hv-log).
It refuses, writing nothing, unless every guest core is stopped (waited for up to --confirm-secs)
and every site holds exactly its patch word; each word is then read back from DRAM.
"""

import argparse
import importlib.util
import os
import pathlib
import sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura import hvpark  # noqa: E402


def nvram_names():
    spec = importlib.util.spec_from_file_location(
        "rgs", pathlib.Path(__file__).resolve().parent / "run_guest_sptm.py")
    rgs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rgs)
    return {what for q in rgs.XNU_QUIRKS for what, *_ in q.get("nvram_ro", ())}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--elf", type=pathlib.Path, help="aura-raw.elf of the running image")
    ap.add_argument("--nm", default="llvm-nm")
    ap.add_argument("--request", action="store_true", help="ask the running guest to park")
    ap.add_argument("--timeout", type=float, default=30, help="seconds to wait for its START")
    ap.add_argument("--status", action="store_true", help="print the park's state")
    ap.add_argument("--restore-nvram-write", action="store_true",
                    help="put XNU's own words back at the nvram_ro sites")
    ap.add_argument("--patch-log", type=pathlib.Path, help="run_guest_sptm's AURA_XNU_PATCH_LOG")
    ap.add_argument("--hv-log", type=pathlib.Path, help="run_guest_sptm's output (hv.log)")
    ap.add_argument("--confirm-secs", type=float, default=10,
                    help="how long to wait for every guest core to stop")
    ap.add_argument("--continue", dest="cont", action="store_true", help="P_EXIT: the guest goes on")
    ap.add_argument("--force", action="store_true", help="--continue a one-way park too")
    ap.add_argument("--tail", type=float, default=0, help="then follow the console this long")
    ap.add_argument("--tail-idle", type=float, default=0, help="... or until silent this long")
    args = ap.parse_args()
    if not os.environ.get("AURADEVICE"):
        sys.exit("set AURADEVICE first")
    if (args.status or args.cont or args.restore_nvram_write) and not args.elf:
        sys.exit("--status, --continue and --restore-nvram-write need --elf")
    if args.restore_nvram_write and not (args.patch_log or args.hv_log):
        sys.exit("--restore-nvram-write needs --patch-log or --hv-log")
    sites = None
    if args.restore_nvram_write:
        sites = hvpark.nvram_sites(args.patch_log, args.hv_log,
                                   None if args.patch_log else nvram_names())

    from aura.proxy import START, AuraProxy, UartInterface
    from aura.proxyutils import bootstrap_port

    iface = UartInterface()
    for code in hvpark.CODES:  # a START that arrives after the open is the park's, not news
        iface.set_handler(START.HV, code, lambda *a: None)
    if args.request:
        to, iface.dev.timeout = iface.dev.timeout, 0.2
        hvpark.request(iface.dev)
        print(f"park requested; waiting up to {args.timeout:g} s for START", flush=True)
        tail = hvpark.ConsoleTail(iface.dev)
        res = tail.run(args.timeout)
        iface.dev.timeout = to
        if res[0] != "park":
            tail.flush()
            sys.exit(f"no park within {args.timeout:g} s ({res[0]}): the guest made no trap "
                     f"that looks at the pipe, or this isn't an image that does")
        print(f"parked: {res[1]}", flush=True)
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    if not args.elf:
        return
    sym = hvpark.Symbols(args.elf, p.get_base(), args.nm)
    hvpark.check_image(iface.readmem, sym)
    info = hvpark.park_info(iface.readmem, sym)
    state = hvpark.PARK_STATES.get(info["state"], str(info["state"]))
    if args.status or args.request:
        unstopped = p.call(sym["sptm_virt_park_unstopped"]) if info["state"] == 1 else None
        print(f"park: {state}; owner cpu{info['cpu']} ({hvpark.CODES.get(info['code'], hex(info['code']))}), "
              f"cores stopped at their entry 0x{info['waiting']:x}, left inside aura "
              f"0x{info['stragglers']:x}, owner's wait {info['wait_ticks'] / 24e3:.1f} ms; "
              f"{info['count']} parks resumed so far, {info['requests']} host requests, "
              f"{info['relinks']} reopens of the port in this park"
              + (f"; guest cores not stopped now 0x{unstopped:x}" if unstopped is not None else ""))
    if args.restore_nvram_write:
        if info["state"] != 1:
            sys.exit(f"refused: the park is {state}; the NVRAM write enable needs a park that "
                     f"resumes")
        hvpark.restore_nvram_write(p.call, iface.readmem, sym, sites, args.confirm_secs)
        print("NVRAM writes enabled: XNU's AppleARMNORNVRAM::write is its own again")
    if args.cont:
        if info["state"] != 1 and not args.force:
            sys.exit(f"refused: the park is {state}; P_EXIT would end its proxy for good "
                     f"(--force)")
        p.exit(0)
        print("P_EXIT sent: the guest goes on", flush=True)
        if args.tail:
            iface.dev.timeout = 0.2
            tail = hvpark.ConsoleTail(iface.dev)
            res = tail.run(args.tail, args.tail_idle)
            tail.flush()
            if res[0] == "park":
                print(f"--- the HV parked again: {res[1]} ---")


if __name__ == "__main__":
    main()
