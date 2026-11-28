#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Boot macOS's own SPTM, TXM and XNU at EL1 under aura's classic hypervisor (M1 series).

Without FEAT_NV2 a guest cannot run at virtual EL2, so SPTM, TXM and XNU run at physical EL1
with their guarded world in GL1, while aura holds EL2 and GL2, as with a classic macOS guest.
SPTM's bootstrap already checks CurrentEL; the EL2 and GL2 registers it still uses are rewritten
in its text before upload (aura/sptm_el1_patch.py), and one in XNU's. TXM uses none.
See docs/sptm-el1-hv.md.

    run_guest_sptm_hv.py --wait --sptm sptm.bin --txm txm.bin \\
        --xnu kernelcache.macho --trustcache static.trustcache --vuart-log guest.log

The guest's serial console is the hypervisor's virtual UART, the proxy's second USB serial port
(--vuart-log copies it to a file). Shell helpers (in the hypervisor shell, after ^C):
  diag()            SPTM's diagnostic record (exception its early vector took: ESR/ELR/FAR)
  panic()           the text SPTM's panic path leaves in that record
  resume_fault()    return the guest to the instruction its last EL1 exception came from
"""
import argparse
import glob
import pathlib
import signal
import struct
import sys
import threading
import time

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

from aura.proxy import AuraProxy, UartInterface, ProxyRemoteError, IODEV
from aura.proxyutils import ProxyUtils, bootstrap_port
from aura.utils import align, irange, set_sigquit_stackdump_handler
from aura.sysreg import (SPRR_CONFIG_EL1, GXF_CONFIG_EL1, SPSR, SPSR_EL12, ESR_EL12, ELR_EL12,
                         FAR_EL12)
from aura.hv import HV
from aura.hv.types import TraceMode
from aura.hw.pmu import PMU
from aura import sptm_virt_plan as sp
from aura import sptm_el1_patch as pe
from aura import sptm_patch

# HVC immediates of SPTM's trapped early vector: base + entry index.
EARLY_VEC_HVC = 0x5e00
NOP = 0xd503201f
MOVZ = 0xd2800000

DEFAULT_CMDLINE = "serial=3 -v debug=0x14e wdt=-1 msgbuf=1048576 -noprogress"


def wait_for_proxy(timeout):
    """The USB serial ports of a proxy window (aura-proxy on the ESP), within timeout s."""
    end = time.time() + timeout
    while time.time() < end:
        if glob.glob("/dev/ttyACM*"):
            time.sleep(0.5)  # let the second port appear too
            return sorted(glob.glob("/dev/ttyACM*"))
        time.sleep(0.2)
    raise SystemExit("no proxy appeared")


def log_vuart(port, path):
    """Copy the guest's console (the proxy's second USB serial port) to a file."""
    import serial
    s = serial.Serial(port, timeout=1)
    with open(path, "ab", buffering=0) as f:
        while True:
            b = s.read(4096)
            if b:
                f.write(b)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--sptm", type=pathlib.Path, required=True)
    ap.add_argument("--txm", type=pathlib.Path, required=True)
    ap.add_argument("--xnu", type=pathlib.Path, required=True)
    ap.add_argument("--trustcache", type=pathlib.Path, required=True)
    ap.add_argument("--script", "-m", type=pathlib.Path, action="append", default=[],
                    help="hypervisor script to run before the jump (tracers)")
    ap.add_argument("--shell", "-S", action="store_true", help="shell before the jump")
    ap.add_argument("--logfile", "-l", type=pathlib.Path)
    ap.add_argument("--cmdline", default=DEFAULT_CMDLINE)
    ap.add_argument("--wait", action="store_true",
                    help="wait up to 3 minutes for the proxy's window and use its port")
    ap.add_argument("--vuart-log", type=pathlib.Path,
                    help="copy the guest's console (the second USB serial port) here")
    ap.add_argument("--dry-run", action="store_true", help="plan, print and stop")
    ap.add_argument("--auto-fix", action="store_true",
                    help="bring-up aid: at an UNDEFINED MRS/MSR that reaches SPTM's early vector, "
                         "rewrite it in guest RAM (MSR -> NOP, MRS -> mov #0), log it, continue")
    args = ap.parse_args()

    sptm = bytearray(args.sptm.read_bytes())
    txm = bytearray(args.txm.read_bytes())
    xnu = bytearray(args.xnu.read_bytes())
    tc = args.trustcache.read_bytes()

    # Patch before waiting for the proxy window: XNU's text takes a while to scan.
    for name, img in (("sptm", sptm), ("txm", txm), ("xnu", xnu)):
        for line in pe.patch(name, img):
            print(f"patch {name}: {line}")
    # SPTM's checks of the hardware against its own placement (sptm_patch); which exist depends
    # on the SPTM (T8103's has only the APSTS poll).
    for what, off, _m, old, new in sptm_patch.plan(bytes(sptm), 0)["patches"]:
        if struct.unpack_from("<I", sptm, off)[0] != old:
            raise SystemExit(f"sptm {what} at {off:#x}: unexpected word")
        struct.pack_into("<I", sptm, off, new)
        print(f"patch sptm: {what} at file offset {off:#x}")
    vbar = pe.early_vector_trap(sptm, EARLY_VEC_HVC)
    print(f"patch sptm: early vector {vbar:#x} -> hvc #{EARLY_VEC_HVC:#x}+n")

    if args.wait:
        ports = wait_for_proxy(180)
        iface = UartInterface(device=ports[0])
    else:
        ports = sorted(glob.glob("/dev/ttyACM*"))
        iface = UartInterface()
    if args.vuart_log and len(ports) > 1:
        threading.Thread(target=log_vuart, args=(ports[1], args.vuart_log), daemon=True).start()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024, aura_heap=128 * 1024 * 1024)
    if not u.cpu_features.apple_sysregs_unlocked:
        raise SystemExit("this path needs unlocked Apple system registers (aura at EL2)")
    hv = HV(iface, p, u)
    hv.init()

    live_adt = hv.adt.build()
    dram_base = u.ba.phys_base & ~0xffffffff
    ram_top = u.ba.phys_base + u.ba.mem_size
    phys_base = align(u.heap_top + 0x2000000, 0x2000000)
    mem_size = ram_top - phys_base
    mem_size_actual = u.ba.mem_size_actual
    print(f"heap_top={u.heap_top:#x} guest phys_base={phys_base:#x} mem_size={mem_size:#x} "
          f"ram_top={ram_top:#x}")

    # iBoot's data regions, copied to the blob tail as on a native boot (see plan_boot).
    mm = u.adt["/chosen/memory-map"]._properties
    copies = []
    for name in ("RTBuddySeg", "SEPFW", "SEPPatches", "uStuff", "preoslog"):
        try:
            pa, size = int(mm[name][0]), int(mm[name][1])
        except (KeyError, IndexError, TypeError, ValueError):
            continue
        if size and pa != sp.ABSENT and not (pa & 0x3fff):
            copies.append((name, pa, size))

    plan = sp.plan_boot(
        bytes(sptm), bytes(txm), bytes(xnu), live_adt,
        dram_base=dram_base, phys_base=phys_base, mem_size=mem_size,
        mem_size_actual=mem_size_actual, trustcache=tc, iboot_copies=copies,
        drop_props=(("/arm-io/dcp/iop-dcp-nub", "routes"),
                    ("/arm-io/aop/iop-aop-nub", "routes"),
                    ("/arm-io/ans/iop-ans-nub", "quiesced")),
        cmdline=args.cmdline, xnu_serial="/arm-io/uart0", diag_record=True)
    print(f"plan: sptm_entry={plan.sptm_entry_pa:#x} boot_args={plan.boot_args_pa:#x} "
          f"blob={plan.blob_start:#x}..{plan.blob_end:#x} adt={plan.adt_pa:#x} "
          f"diag={plan.diag_pa:#x}")
    for note in plan.notes:
        print("  note:", note)
    if args.dry_run:
        return

    hv.phys_base = phys_base
    hv.ram_base = dram_base
    hv.ram_size = mem_size_actual
    hv.entry = plan.sptm_entry_pa
    hv.add_tracer(irange(hv.ram_base, u.ba.phys_base - hv.ram_base), "RAM-LOW", TraceMode.OFF)
    hv.add_tracer(irange(phys_base, mem_size_actual - phys_base + hv.ram_base), "RAM-HIGH",
                  TraceMode.OFF)
    hv.unmap_carveouts()

    imgs = {"sptm": bytes(sptm), "txm": bytes(txm), "xnu": bytes(xnu)}
    for ld in plan.loads:
        if ld.image == "bytes":
            data = ld.payload
        elif ld.image == "copy":
            data = iface.readmem(ld.fileoff, ld.size)
        else:
            data = imgs[ld.image][ld.fileoff:ld.fileoff + ld.size]
        print(f"  upload {ld.what:24s} -> {ld.pa:#x} ({len(data):#x}/{ld.total:#x})")
        if data:
            u.compressed_writemem(ld.pa, data, True)
        if ld.total > len(data):
            p.memset8(ld.pa + len(data), 0, ld.total - len(data))
    u.compressed_writemem(plan.adt_pa, plan.adt, True)
    iface.writemem(plan.boot_args_pa, plan.boot_args)
    p.memset8(plan.diag_pa, 0, sp.BOOT_ARGS_DIAG_SIZE)
    p.dc_cvau(plan.blob_start, plan.blob_end - plan.blob_start)
    p.ic_ivau(plan.blob_start, plan.blob_end - plan.blob_start)

    # Secondaries reset into SPTM's reset vector, as natively.
    rvbar = hv.entry & ~0xfff
    for cpu in hv.adt["cpus"]:
        if cpu.state != "running":
            p.write64(cpu.cpu_impl_reg[0], rvbar)

    PMU(u).reset_panic_counter()

    def diag():
        w = [p.read64(plan.diag_pa + 8 * i) for i in range(7)]
        print(f"diag @{plan.diag_pa:#x}: magic={w[0]:#x} taken={w[2]} vector={w[3]} "
              f"ESR={w[4]:#x} (EC {(w[4] >> 26) & 0x3f:#x}) ELR={w[5]:#x} FAR={w[6]:#x}")

    def panic():
        b = iface.readmem(plan.diag_pa, sp.BOOT_ARGS_DIAG_SIZE)
        for chunk in b.split(b"\0"):
            if len(chunk) >= 8 and chunk.isascii():
                print(chunk.decode())

    def resume_fault(skip=False):
        hv.ctx.elr = u.mrs(ELR_EL12) + (4 if skip else 0)
        hv.ctx.spsr = SPSR(u.mrs(SPSR_EL12))

    orig_hvc = hv.handle_hvc

    def early_vector(ctx):
        """An exception SPTM's early vector took, now an HVC (see early_vector_trap)."""
        idx = ctx.esr.ISS
        if not EARLY_VEC_HVC <= idx < EARLY_VEC_HVC + 16:
            return orig_hvc(ctx)
        esr, elr, far, spsr = (u.mrs(r) for r in (ESR_EL12, ELR_EL12, FAR_EL12, SPSR_EL12))
        pa = p.hv_translate(elr, False, False) & ~0xfff | (elr & 0xfff)
        word = p.read32(pa)
        hv.log(f"SPTM early vector #{idx - EARLY_VEC_HVC}: ESR={esr:#x} (EC "
               f"{(esr >> 26) & 0x3f:#x}) ELR={elr:#x} (PA {pa:#x}, {word:#010x}) "
               f"FAR={far:#x} SPSR={spsr:#x}")
        d = pe.decode(word)
        if not (args.auto_fix and (esr >> 26) & 0x3f == 0 and d):
            return False  # to the shell, guest state intact
        is_read, reg, rt = d
        new = (MOVZ | rt if rt != 31 else NOP) if is_read else NOP
        p.write32(pa, new)
        p.dc_cvau(pa & ~0x7f, 0x80)
        p.ic_ivau(pa & ~0x7f, 0x80)
        hv.log(f"  auto-fix: {'MRS' if is_read else 'MSR'} S{reg[0]}_{reg[1]}_C{reg[2]}_C{reg[3]}"
               f"_{reg[4]} -> {new:#010x}; add it to sptm_el1_patch")
        ctx.elr = elr
        ctx.spsr = SPSR(spsr)
        return True

    hv.handle_hvc = early_vector
    hv.shell_locals.update(diag=diag, panic=panic, resume_fault=resume_fault, plan=plan)

    if args.logfile:
        hv.set_logfile(args.logfile.open("w"))
    for sc in args.script:
        hv.run_script(sc)
    if args.shell:
        hv.run_shell("Shell before the SPTM jump; ^D to start.")

    for iodev in IODEV:
        if iodev != hv.iodev:
            p.iodev_set_usage(iodev, 0)
    hv.map_essential()
    hv.pt_update()
    p.fb_improve_logo()
    p.fb_shutdown(True)
    u.msr(SPRR_CONFIG_EL1, 1)
    u.msr(GXF_CONFIG_EL1, 1)

    print(f"Jumping to SPTM {plan.sptm_entry_pa:#x}, x0={plan.boot_args_pa:#x}", flush=True)
    iface.dev.timeout = None
    hv.default_sigint = signal.signal(signal.SIGINT, hv._handle_sigint)
    set_sigquit_stackdump_handler()
    if hv.wdt_cpu is not None:
        p.hv_wdt_start(hv.wdt_cpu)
    hv.started = True
    boot_cpu = next(c for c in hv.adt["cpus"] if c.state == "running")
    hv.started_cpus[boot_cpu.cpu_id] = (getattr(boot_cpu, "die_id", 0), boot_cpu.cluster_id,
                                        boot_cpu.cpu_id)
    hv.sysreg[boot_cpu.cpu_id] = {}
    try:
        p.hv_start(plan.sptm_entry_pa, plan.boot_args_pa)
    except ProxyRemoteError as exc:
        print(f"Hypervisor failed: {exc}")
    hv.run_shell("Hypervisor exited.")


if __name__ == "__main__":
    main()
