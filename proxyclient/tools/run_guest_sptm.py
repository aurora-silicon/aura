#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Boot macOS's stock SPTM, TXM, secure kernel and XNU as a FEAT_NV2 guest under aura (SPTM_VIRT).

The host plays iBoot's SPTM loader, which a kcbz=0 boot object skips: it lays the images into
SPTM's managed window in the order SPTM expects, edits the device tree, builds the boot_args
SPTM's __start takes in x0, and asks aura to enter SPTM at virtual EL2 with its MMU off. The
layout is aura/sptm_virt_plan.py; see docs/sptm-nv2-virt.md.

Needs aura built with SPTM_VIRT=1 CHAINLOADING=1 and chainloaded, with its proxy up.

Environment (all optional; 1 enables a switch):
  AURA_KEEP_EXCLAVES       keep the exclave nodes; with AURA_EXCLAVE_FW, run cL4 and ExclaveOS
  AURA_EXCLAVE_FW=<dir>    extracted exclave firmware (exclavecore_bundle.t8140.RELEASE and
                           <build>__MacOS/Firmware)
  AURA_XNU_SERIAL=<node>   XNU's console device under /arm-io, e.g. uart0 (aura's vuart)
  AURA_HIDE_DOCKCHANNEL    hide the DockChannel that carries aura's console and proxy
  AURA_KEEP_ERRH           keep the platform error handler visible to XNU
  AURA_AMCC_CLEAR          clear the AMCC error latches stage 1 left, before entry
  AURA_S2R_TRACE           sleep path: hooks on the power blocks and the trace ring
  AURA_S2R_GO              let the sleep proceed (otherwise stop in the proxy before it)
  AURA_S2R_FULLTRACE       also hook the high-volume blocks
  AURA_S2R_COPROC          also hook the coprocessor and exclave mailboxes
  AURA_S2R_DROP_USB        tear down aura's USB gadget at the sleep commit
  AURA_S2R_QUIESCE=<mask>  AMCC error-enable writes at the commit: 8 DSID_AGE_OUT,
                           16 UNEXP_RT_HIT_DIR
  AURA_XNU_BT              sample XNU backtraces every few seconds per core
  AURA_TAIL_SECS, AURA_TAIL_IDLE   keep reading the console after entry, until idle
  XNU_BOOTARGS             XNU's command line

Example:
  export PYTHONPATH=proxyclient
  python3 proxyclient/tools/run_guest_sptm.py --sptm sptm.bin --txm txm.bin \\
      --xnu kernelcache.decompressed --trustcache iboot.tc --dry-run
"""

import argparse
import os
import pathlib
import struct
import subprocess
import sys
import time

# wdt=-1 keeps XNU from arming the SoC watchdog. -vm_compressor_wk_sw avoids the SVE WKdm
# compressor (an exception to EL2 clears PSTATE.SM). sprr_tpro*=0 drop the TPRO policy.
# -noprogress: no framebuffer is handed over, and the boot-progress layout loops on a zero
# scale. disable-uart-irq=1 keeps AppleSerialShim off uart0's interrupt; the polled console
# needs none.
DEFAULT_BOOTARGS = ("serial=3 disable-uart-irq=1 -v -nobsdmgroot wdt=-1 sprr_tpro=0 "
                    "sprr_tpro_pagers=0 msgbuf=1048576 -vm_compressor_wk_sw CAEnableMedina=0 "
                    "-noprogress")

# Configuration flags, sptm_virt.h SPTM_VIRT_*
CONFIG_FLAGS = {
    "AURA_S2R_TRACE": 1 << 0,
    "AURA_S2R_GO": 1 << 1,
    "AURA_S2R_FULLTRACE": 1 << 2,
    "AURA_S2R_COPROC": 1 << 3,
    "AURA_S2R_DROP_USB": 1 << 4,
    "AURA_XNU_BT": 1 << 5,
}
KNOWN_ENV = {
    "AURA_KEEP_EXCLAVES",
    "AURA_EXCLAVE_FW",
    "AURA_XNU_SERIAL",
    "AURA_HIDE_DOCKCHANNEL",
    "AURA_KEEP_ERRH",
    "AURA_AMCC_CLEAR",
    "AURA_S2R_QUIESCE",
    "AURA_TAIL_SECS",
    "AURA_TAIL_IDLE",
} | set(CONFIG_FLAGS)

# The debug link: DockChannel plus the ATC0 port's PD controller, USB controller and PHY path.
# XNU must never drive them (it resets the port about 3 s into IOKit and the link drops).
KIS_PORT_NODES = (
    "/arm-io/nub-spmi-a0/hpm0",
    "/arm-io/usb-drd", "/arm-io/dart-usb", "/arm-io/atc-phy0",
    "/arm-io/atc0-dpxbar", "/arm-io/atc0-dpphy",
    "/arm-io/i2c2/usb-repeater-i2c", "/arm-io/i2c2/usb-hub-i2c",
)
# Nothing drives the ANE yet, and the platform error handler panics on an AMCC DSID timeout.
STRIP_NODES = ("/arm-io/ane", "/arm-io/dart-ane")
ERROR_HANDLER = "/arm-io/error-handler"
# Without cL4 the exclaves stay NOT_SUPPORTED, so their nodes and the RTBuddy routes that resolve
# through them would wait forever in IOKit. With cL4 they must stay: hidden, DCP never completes
# its sleep power state.
EXCLAVE_NODES = ("/arm-io/exdisplaypipe", "/arm-io/exdisplaypipe-s-proxy",
                 "/arm-io/dcp-exclave-ioreporting", "/arm-io/dcp-exclave-mailbox",
                 "/arm-io/aop-exclave-mailbox", "/arm-io/aop2-exclave-mailbox")
ROUTES_PROPS = (("/arm-io/dcp/iop-dcp-nub", "routes"), ("/arm-io/aop/iop-aop-nub", "routes"),
                ("/arm-io/aop2/iop-aop2-nub", "routes"))
# iBoot's data regions copied to the blob tail, in iBoot's order (the native layout).
IBOOT_COPIES = ("RTBuddySeg", "SEPFW", "SEPPatches", "uStuff", "preoslog")

NOP = 0xd503201f

# Patches to the loaded SPTM text, relative to its entry point (25G83). Each is checked against
# the instruction it expects and refuses to boot a different build.
# (what, offset, mask, expected, replacement)
SPTM_PATCHES = (
    # __start polls APSTS_EL1 bit 0 (PAC ready) after it programs TTBR/MAIR. The bit is banked
    # and the guest's copy reads 0 under aura, though PAC is up: drop the backward cbz.
    ("APSTS poll", 0x3258, 0xff000000, 0xb4000000, NOP),
    # ctrr_ctxr_check_region compares the locked hardware CTRR/CTXR bounds with the ones SPTM
    # stashed for its own text. Stage 1 locked them for aura, so they never match: branch to the
    # check's success tail, on the boot path (b.ne) and on the reset path (cbz w6).
    ("CTRR check", 0x129ec, 0xffffffff, 0x54000ac1, 0x14000056),
    ("CTRR reset-path check", 0x128c0, 0xffffffff, 0x34000986, 0x140000a1),
    # ctXr_check_all_regions requires the per-core CTRR-A/B CTL to read locked; these do not
    # trap to aura and cannot be set from EL2.
    ("ctXr lock check", 0x12b1c, 0xff00001f, 0x54000001, NOP),
    ("ctXr lock check", 0x12b28, 0xff00001f, 0x54000001, NOP),
    ("ctXr lock check", 0x12b34, 0xff00001f, 0x54000001, NOP),
    ("ctXr lock check", 0x12b40, 0xff00001f, 0x54000001, NOP),
    # init_pre_late_io_reservation compares the AMCC CTRR-C/D range (stage 1's) with the ACC one
    # (SPTM's), only to log a mismatch.
    ("AMCC CTRR match", 0x39164, 0xffffffff, 0x54003e41, NOP),
    ("AMCC CTRR match", 0x39170, 0xffffffff, 0x54003de2, NOP),
    ("AMCC CTRR match", 0x391ac, 0xffffffff, 0x54004a01, NOP),
    ("AMCC CTRR match", 0x391b8, 0xffffffff, 0x540049a2, NOP),
)

# AMCC error status words: four planes, seven words each.
AMCC_STATUS = 0x220025004
AMCC_PLANE_STRIDE = 0x40000


def env_on(name):
    return os.environ.get(name) == "1"


def env_int(name, default=0):
    value = os.environ.get(name)
    return int(value, 0) if value else default


def parse_args():
    ap = argparse.ArgumentParser(description="Boot the stock SPTM as an NV2 guest under aura")
    ap.add_argument("--sptm", type=pathlib.Path, required=True, help="sptm.bin (flat Mach-O)")
    ap.add_argument("--txm", type=pathlib.Path, required=True, help="txm.bin (flat Mach-O)")
    ap.add_argument("--xnu", type=pathlib.Path, required=True, help="decompressed kernelcache")
    ap.add_argument("--trustcache", type=pathlib.Path, default=None,
                    help="iBoot's trust cache container, or one raw trust cache (default: empty)")
    ap.add_argument("--bootargs", default=os.environ.get("XNU_BOOTARGS", DEFAULT_BOOTARGS),
                    help="XNU's command line (env XNU_BOOTARGS)")
    ap.add_argument("--elf", type=pathlib.Path, default=pathlib.Path("build/aura-raw.elf"),
                    help="aura's raw ELF, for symbols")
    ap.add_argument("--nm", default="llvm-nm", help="an nm that reads the ELF")
    ap.add_argument("--dry-run", action="store_true", help="plan and print only; write nothing")
    ap.add_argument("--no-enter", action="store_true", help="load and record; do not enter")
    # Older launchers pass it; SPTM's window always stays mapped in aura.
    ap.add_argument("--no-unmap", action="store_true", help=argparse.SUPPRESS)
    return ap.parse_args()


class Symbols:
    """aura symbols at their run addresses: run base + (nm(sym) - nm(_base))."""

    def __init__(self, nm, elf, run_base):
        self.syms = {}
        for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
            parts = line.split()
            if len(parts) == 3:
                self.syms[parts[2]] = int(parts[0], 16)
        self.run_base = run_base
        self.link_base = self.syms["_base"]

    def __getitem__(self, name):
        return self.run_base + (self.syms[name] - self.link_base)


def iboot_copies(adt):
    mm = adt["/chosen/memory-map"]._properties
    copies = []
    for name in IBOOT_COPIES:
        try:
            pa, size = int(mm[name][0]), int(mm[name][1])
        except (KeyError, IndexError, TypeError, ValueError):
            continue
        if size and pa != 0xFFFFFFFFFFFFFFFF and not pa & 0x3fff:
            copies.append((name, pa, size))
            print(f"iBoot {name} 0x{pa:x}+0x{size:x} -> copied to the blob tail")
    return copies


def check_oslog(adt, aura_base, aura_end):
    """aura runs at stage 1's base, the only place a secondary core may execute, and the display
    coprocessors log into __OS_LOG buffers iBoot placed right after stage 1. aura's .bss and stacks
    must end below them, or the DCP overwrites aura's state."""
    overlap = False
    for owner in ("dcp", "dcpext", "dcpext0", "dcpext1"):
        try:
            raw = adt[f"/arm-io/{owner}/iop-{owner}-nub"]._properties.get("segment-ranges")
        except KeyError:
            continue
        for i in range(len(raw or b"") // 32):
            phys, _iova, _remap, size, flags = struct.unpack_from("<QQQII", raw, 32 * i)
            if flags != 0xA:  # __OS_LOG
                continue
            ov = phys < aura_end and phys + size > aura_base
            overlap |= ov
            print(f"{owner} __OS_LOG phys 0x{phys:x} +0x{size:x} (aura +0x{phys - aura_base:x})"
                  + (f"  OVERLAPS aura [0x{aura_base:x}, 0x{aura_end:x})" if ov else ""))
    if overlap:
        raise SystemExit("aura's .bss/stacks overlap a DCP __OS_LOG buffer: shrink .bss")


def exclave_firmware(fw_dir, plan_mod):
    from aura.exclave_bundle import parse as parse_bundle

    bdir = fw_dir / "exclavecore_bundle.t8140.RELEASE"
    bundle = parse_bundle((bdir / "exclavecore_bundle.t8140.RELEASE").read_bytes())
    kernel = plan_mod.macho_info((bdir / "SYSTEM" / "kernel").read_bytes())
    fw = next(fw_dir.glob("*__MacOS")) / "Firmware"
    integrity = (fw / "exclave.integrity_catalog.bin").read_bytes()
    trustcache = (fw / "exclave.trustcache.bin").read_bytes()
    print(f"exclaves: cL4 bundle 0x{len(bundle.payload):x} entry 0x{kernel.entry:x} virt "
          f"0x{kernel.virtlo:x}; exic 0x{len(integrity):x} extc 0x{len(trustcache):x}; regions "
          + " ".join(f"{k}=0x{v[1]:x}" for k, v in bundle.regions.items()))
    return dict(cl4_regions=bundle.regions, cl4_entry=kernel.entry, cl4_virt=kernel.virtlo,
                exclave_integrity=integrity, exclave_trustcache=trustcache), bundle.payload


def write_plan(p, u, iface, sym, plan, images):
    # Device-side copies first (iBoot's data regions), refusing any whose source a load overwrites.
    copies = [ld for ld in plan.loads if ld.image == "copy"]
    for ld in copies:
        for other in plan.loads:
            if other is not ld and other.pa < ld.fileoff + ld.size and ld.fileoff < other.pa + other.total:
                raise SystemExit(f"{other.what} at 0x{other.pa:x} overwrites the source of {ld.what}")
    for ld in copies:
        print(f"copying {ld.what:22s} 0x{ld.fileoff:x} -> 0x{ld.pa:x} (0x{ld.size:x})")
        if not (ld.pa | ld.fileoff | ld.size) & 7:
            p.memcpy64(ld.pa, ld.fileoff, ld.size)
        else:
            p.memcpy8(ld.pa, ld.fileoff, ld.size)
        if ld.total > ld.size:
            iface.writemem(ld.pa + ld.size, bytes(ld.total - ld.size))
        p.call(sym["sptm_virt_flush"], ld.pa, ld.total)
    for ld in plan.loads:
        if ld.image == "copy":
            continue
        data = ld.payload if ld.image == "bytes" else images[ld.image][ld.fileoff:ld.fileoff + ld.size]
        data = data.ljust(ld.total, b"\0")
        print(f"writing {ld.what:22s} -> 0x{ld.pa:x} (0x{len(data):x})")
        u.compressed_writemem(ld.pa, data, True)
        p.call(sym["sptm_virt_flush"], ld.pa, len(data))

    print(f"writing device tree -> 0x{plan.adt_pa:x}")
    iface.writemem(plan.adt_pa, plan.adt)
    p.call(sym["sptm_virt_flush"], plan.adt_pa, len(plan.adt))
    print(f"writing boot_args   -> 0x{plan.boot_args_pa:x}")
    iface.writemem(plan.boot_args_pa, plan.boot_args)
    p.call(sym["sptm_virt_flush"], plan.boot_args_pa, len(plan.boot_args))


def patch_sptm(p, sym, entry):
    for what, off, mask, expected, new in SPTM_PATCHES:
        pa = entry + off
        before = p.read32(pa)
        if before & mask != expected:
            raise SystemExit(f"{what}: [0x{pa:x}] = 0x{before:08x}, expected 0x{expected:08x} "
                             f"under 0x{mask:08x}: not the SPTM these patches are for")
        p.write32(pa, new)
        p.call(sym["sptm_virt_flush"], pa & ~0x3f, 0x80)
        print(f"{what}: [0x{pa:x}] 0x{before:08x} -> 0x{p.read32(pa):08x}")


def report_amcc(p, clear):
    """The AMCC error latches stage 1 left: XNU's error handler panics on them at its start."""
    for plane in range(4):
        for i in range(7):
            addr = AMCC_STATUS + plane * AMCC_PLANE_STRIDE + i * 8
            v = p.read32(addr)
            if v:
                print("AMCC plane %d INTSTS%d 0x%x = 0x%08x" % (plane, i, addr, v))
                if clear:
                    p.write32(addr, v)
                    print("  cleared, now 0x%08x" % p.read32(addr))


def configure(p, sym):
    flags = sum(bit for name, bit in CONFIG_FLAGS.items() if env_on(name))
    p.call(sym["sptm_virt_config"], flags)
    if os.environ.get("AURA_S2R_QUIESCE"):
        p.write32(sym["sptm_virt_s2r_quiesce"], env_int("AURA_S2R_QUIESCE"))
        print("AMCC quiesce writes at the commit:", os.environ["AURA_S2R_QUIESCE"])
    if env_on("AURA_KEEP_ERRH") or env_on("AURA_AMCC_CLEAR"):
        report_amcc(p, env_on("AURA_AMCC_CLEAR"))


def tail_console(iface, secs, idle):
    """Keep reading the console after the entry call: a serial read error reopens the device,
    and the tail stops after secs, or after idle seconds of silence."""
    import serial

    end = time.time() + secs
    last = time.time()
    dev = iface.dev
    buf = b""
    print(f"--- tailing console for up to {secs}s (stop after {idle:g}s silent) ---", flush=True)
    while time.time() < end:
        if idle and time.time() - last > idle:
            print(f"--- tail: console silent for {idle:g}s ---", flush=True)
            break
        try:
            chunk = dev.read(4096)
        except (serial.serialutil.SerialException, OSError) as e:
            print(f"--- tail: read error ({e}); reopening ---", flush=True)
            try:
                dev.close()
            except Exception:
                pass
            for _ in range(100):
                try:
                    dev.open()
                    print("--- tail: reopened ---", flush=True)
                    break
                except (serial.serialutil.SerialException, OSError):
                    time.sleep(0.2)
            else:
                print("--- tail: device gone ---", flush=True)
                break
            continue
        if not chunk:
            continue
        last = time.time()
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print("TTY> " + line.decode("latin1", "replace").rstrip("\r"), flush=True)
    print("--- tail done ---", flush=True)


def main():
    args = parse_args()
    unknown = sorted(k for k in os.environ if k.startswith("AURA_") and k not in KNOWN_ENV)
    if unknown:
        print("not run_guest_sptm.py switches, ignored:", " ".join(unknown))

    from aura import sptm_virt_plan as plan_mod
    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024)
    sym = Symbols(args.nm, args.elf, p.get_base())

    # SPTM's managed window, as aura computed it: above aura and everything iBoot left low,
    # below aura's carve-out.
    scratch = u.malloc(0x40)
    if p.call(sym["sptm_virt_place"], scratch) != 0:
        raise SystemExit("sptm_virt_place failed: not an SPTM_VIRT build on a T8140, or no valid "
                         "layout (see aura's console)")
    managed_base, managed_end = (p.read64(scratch + 8 * i) for i in range(2))
    print(f"managed window from aura [0x{managed_base:x}, 0x{managed_end:x})")

    # The native layout: physBase is the window's base, the images follow a 32 MiB phys slide
    # (which XNU frees to its VM, so nothing live may be in it), and iBoot's data regions are
    # copied to the blob tail, so nothing SPTM walks is below physBase.
    ba = u.ba
    phys_base = managed_base
    print(f"NATIVE layout: physBase = blob base = aura's floor 0x{phys_base:x} "
          f"(iBoot physBase 0x{ba.phys_base:x})")
    if managed_end - phys_base < (2 << 30):
        raise SystemExit("not enough managed RAM left above the blob base")
    print(f"SPTM managed window [0x{phys_base:x}, 0x{managed_end:x}), blob base 0x{phys_base:x}")
    dram_base = phys_base & ~((4 << 30) - 1)
    print(f"live boot_args: physBase 0x{ba.phys_base:x} memSize 0x{ba.mem_size:x} "
          f"memSizeActual 0x{ba.mem_size_actual:x}; dram base 0x{dram_base:x}")

    adt = u.adt
    copies = iboot_copies(adt)
    images = {"sptm": args.sptm.read_bytes(), "txm": args.txm.read_bytes(),
              "xnu": args.xnu.read_bytes()}
    check_oslog(adt, sym.run_base, sym["_end"])

    keep_exclaves = env_on("AURA_KEEP_EXCLAVES")
    hide = KIS_PORT_NODES + STRIP_NODES
    if not env_on("AURA_KEEP_ERRH"):
        hide += (ERROR_HANDLER,)
    if env_on("AURA_HIDE_DOCKCHANNEL"):
        hide += ("/arm-io/dockchannel-uart",)
    if not keep_exclaves:
        hide += EXCLAVE_NODES
    cl4 = {}
    if keep_exclaves and os.environ.get("AURA_EXCLAVE_FW"):
        cl4, images["cl4"] = exclave_firmware(pathlib.Path(os.environ["AURA_EXCLAVE_FW"]), plan_mod)
    serial = os.environ.get("AURA_XNU_SERIAL")

    plan = plan_mod.plan_boot(
        images["sptm"], images["txm"], images["xnu"], u.get_adt(),
        dram_base=dram_base, phys_base=phys_base, mem_size=managed_end - phys_base,
        mem_size_actual=ba.mem_size_actual, cmdline=args.bootargs, blob_base=phys_base,
        trustcache=args.trustcache.read_bytes() if args.trustcache else b"",
        iboot_copies=copies, hide_nodes=hide,
        drop_props=() if keep_exclaves else ROUTES_PROPS,
        xnu_serial=f"/arm-io/{serial}" if serial else None, **cl4)

    entry, handoff = plan.sptm_entry_pa, plan.boot_args_pa
    print(f"bootargs: {args.bootargs}")
    print(f"SPTM load 0x{plan.sptm_pa:x}  entry 0x{entry:x}  boot_args 0x{handoff:x}  "
          f"ADT 0x{plan.adt_pa:x} ({len(plan.adt)} B)")
    print(f"virtBase 0x{plan.virt_base:x}  blob [0x{plan.blob_start:x}, 0x{plan.blob_end:x})")
    for name, (pa, size) in plan.regions.items():
        print(f"  {name:14s} 0x{pa:x} +0x{size:x}")
    for n in plan.notes:
        print("note:", n)
    if args.dry_run:
        return

    write_plan(p, u, iface, sym, plan, images)
    patch_sptm(p, sym, entry)
    configure(p, sym)
    if p.call(sym["sptm_virt_load"], entry, handoff) != 0:
        raise SystemExit("sptm_virt_load rejected the entry/handoff (see aura's console)")
    if args.no_enter:
        print("loaded and recorded; not entering (--no-enter)")
        return

    print("entering SPTM at virtual EL2 (the result is on aura's console)")
    # sptm_virt_enter does not return: the call's reply never comes.
    p.call(sym["sptm_virt_enter"], reboot=True)
    secs = env_int("AURA_TAIL_SECS")
    if secs > 0:
        tail_console(iface, secs, float(os.environ.get("AURA_TAIL_IDLE") or 10))


if __name__ == "__main__":
    sys.exit(main())
