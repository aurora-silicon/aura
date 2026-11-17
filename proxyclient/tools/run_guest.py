#!/usr/bin/env python3
# SPDX-License-Identifier: MIT AND GPL-2.0-only
import sys, pathlib, traceback
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, pathlib
from io import BytesIO

def volumespec(s):
    return tuple(s.split(":", 2))

parser = argparse.ArgumentParser(description='Run a Mach-O payload under the hypervisor')
parser.add_argument('-s', '--symbols', type=pathlib.Path)
parser.add_argument('-m', '--script', type=pathlib.Path, action='append', default=[])
parser.add_argument('-c', '--command', action="append", default=[])
parser.add_argument('-S', '--shell', action="store_true")
parser.add_argument('-e', '--hook-exceptions', action="store_true")
parser.add_argument('-d', '--debug-xnu', action="store_true")
parser.add_argument('-l', '--logfile', type=pathlib.Path)
parser.add_argument('-C', '--cpus', default=None)
parser.add_argument('--strip-node', action="append", default=[], metavar='SUBSTR',
                    help='Remove every ADT node whose name contains SUBSTR.')
parser.add_argument('-r', '--raw', action="store_true")
parser.add_argument('-E', '--entry-point', action="store", type=int, help="Entry point for the raw image", default=0x800)
parser.add_argument('-a', '--append-payload', type=pathlib.Path, action="append", default=[])
parser.add_argument('-P', '--emulate-sprr', action="store_true",
                    help='Force the EL2 SPRR/GXF emulation on.')
parser.add_argument('-v', '--volume', type=volumespec, action='append',
                    help='Attach a 9P virtio device for file export to the guest. The argument is a host path to the '
                         'exported tree, joined by colon (\':\') with a tag under which the tree will be advertised '
                         'on the guest side.')
parser.add_argument('--windows-guest-ram', metavar='BASE:TOP', default=None,
                    help='Windows personality on T8152 (J804): the only guest RAM, [BASE, TOP). '
                         'Required there; 64 KiB aligned, outside every carve-out, CTXR window '
                         'and aura itself (the resident checks it again).')
parser.add_argument('--windows-ipi', choices=('poll', 'fast'), default='poll',
                    help='T8152: kick guest CPUs by polling (default) or, only after Phase 3 '
                         'measurement (c) passed, with Apple Fast IPIs.')
parser.add_argument('--windows-gic-cpuif', choices=('carrier', 'software'), default='carrier',
                    help='T8152: the software GIC CPU interface (as T8142), only after Phase 3 '
                         'measurement (f) showed ICC_* accesses trap to EL2.')
parser.add_argument('--windows-serial', choices=('cdc', 'disabled'), default='cdc',
                    help='T8152: guest vUART on the dedicated CDC interface (default) or off.')
parser.add_argument('--windows-reset-on-exit', action='store_true',
                    help='T8152: when the guest asks for PSCI SYSTEM_RESET, shut NVMe down and '
                         'reset through WD1. Default: stay in the aura proxy.')
parser.add_argument('payload', type=pathlib.Path)
parser.add_argument('boot_args', default=[], nargs="*")
args = parser.parse_args()

from aura.proxy import *
from aura.proxyutils import *
from aura.utils import *
from aura.shell import run_shell
from aura.sysreg import *
from aura.hv import HV
from aura.hv.virtio import Virtio9PTransport
from aura.hw.pmu import PMU

iface = UartInterface()
p = AuraProxy(iface, debug=False)
bootstrap_port(iface, p)


def windows_resident_caps():
    from aura.windows import CAPS_MAGIC
    try:
        caps = p.hv_windows_capabilities()
    except ProxyRemoteError:
        return 0
    return caps if caps >> 32 == CAPS_MAGIC >> 32 else 0


def run_windows_t8152(caps):
    """The Windows personality (gmake WINDOWS=1) on T8152: the guest gets only its
    RAM window, the AIC (Device-nGnRnE) and emulated MMIO; the resident refuses to
    enter it unless every T8152 safety precondition holds (src/hv/windows/hv_t8152.c)."""
    from aura.windows import (AURA_HEAP_RESERVE, PROXY_HEAP_SIZE, T8152_CHIP, number,
                              t8152_check_window, windows_system_reset)
    if not args.raw:
        sys.exit("T8152 Windows guests are raw Mu FDs: pass -r")
    if args.symbols or args.debug_xnu or args.volume or args.emulate_sprr or args.cpus or \
            args.strip_node or args.shell or args.script or args.command:
        sys.exit("T8152 Windows guests take only -r, -E, -a, -l and boot arguments")
    if not args.windows_guest_ram or ":" not in args.windows_guest_ram:
        sys.exit("T8152 needs --windows-guest-ram BASE:TOP (see docs/windows-t8152.md)")
    base, top = (number(v, "guest RAM") for v in args.windows_guest_ram.split(":", 1))

    u = ProxyUtils(p, heap_size=PROXY_HEAP_SIZE, aura_heap=AURA_HEAP_RESERVE)
    if u.adt["/chosen"].chip_id != T8152_CHIP:
        sys.exit("this Windows resident is not on T8152: use tools/boot_windows.py")
    ba = u.ba
    adt_pa = (ba.devtree - ba.virt_base + ba.phys_base) & ((1 << 64) - 1)
    occupied = [("aura resident and heaps", u.base, u.heap_top - u.base),
                ("live ADT", adt_pa, ba.devtree_size)]
    if ba.video.base and ba.video.stride and ba.video.height:
        occupied.append(("scanout", ba.video.base, ba.video.stride * ba.video.height))
    t8152_check_window(u.adt, ba, base, top, occupied)
    print(f"T8152: guest RAM [{base:#x}, {top:#x}) passes the host check; NVRAM, QSPI and the "
          f"audio/AOP/ADMAC/i2c nodes are hidden, and the resident re-checks everything")

    hv = HV(iface, p, u, windows=True)
    hv.windows_framebuffer = "bgra-shadow"
    hv.windows_serial = args.windows_serial
    hv.windows_guest_ram = (base, top)
    hv.windows_ipi = args.windows_ipi
    hv.windows_gic_cpuif = args.windows_gic_cpuif
    hv.init()
    if args.logfile:
        hv.set_logfile(args.logfile.open("w"))
    if args.boot_args:
        hv.set_bootargs(" ".join(args.boot_args))
    data = args.payload.read_bytes()
    for part in args.append_payload:
        data += part.read_bytes()
    hv.load_raw(data, args.entry_point)
    if hv.guest_base % 0x10000:
        sys.exit("Mu payload base is not 64 KiB aligned")
    print(f"T8152: Mu at {hv.guest_base:#x}, entry {hv.entry:#x}, "
          f"boot_args {hv.guest_base + hv.bootargs_off:#x}")
    try:
        result = hv.start()
    except ProxyRemoteError as exc:
        sys.exit(f"Hypervisor refused or failed: {exc}. The aura proxy keeps running; "
                 "its console says why.")
    print(f"Guest exit status: {result} (1 SYSTEM_OFF, 2 SYSTEM_RESET)")
    if result == 2 and args.windows_reset_on_exit:
        print("SYSTEM_RESET: shutting NVMe down, then resetting through WD1")
        windows_system_reset(p)
    else:
        print("The aura proxy stays up. Nothing was written to disk or NVRAM by the guest.")
    sys.exit(0)


_windows_caps = windows_resident_caps()
if _windows_caps and p.get_chipid() == 0x8152:
    run_windows_t8152(_windows_caps)
elif (args.windows_guest_ram or args.windows_ipi != 'poll' or args.windows_reset_on_exit or
      args.windows_gic_cpuif != 'carrier'):
    sys.exit("--windows-* options need the Windows personality (gmake WINDOWS=1) on T8152")

if args.emulate_sprr or not p.get_cpu_features().apple_sysregs_unlocked:
    aura_heap_mb = 1024
else:
    aura_heap_mb = 128
u = ProxyUtils(p, heap_size = 128 * 1024 * 1024,
               aura_heap = aura_heap_mb * 1024 * 1024)

# Setup counter redirect / AHCR_EL2 as expected by macOS for macho payloads
if not args.raw:
    chip_id = u.adt["/chosen"].chip_id
    if chip_id in (0x6030, 0x6031, 0x6032, 0x6034, 0x8122):
        u.msr(AGTCNTRDIR_EL1, 3)
        u.msr(AGTCNTRDIR_EL12, 3)

hv = HV(iface, p, u)

hv.hook_exceptions = args.hook_exceptions

if args.emulate_sprr:
    hv.emulate_sprr = True

hv.init()

if args.cpus:
    avail = [i.name for i in hv.adt["/cpus"]]
    want = set(f"cpu{i}" for i in args.cpus)
    for cpu in avail:
        if cpu in want:
            continue
        try:
            del hv.adt[f"/cpus/{cpu}"]
            print(f"Disabled {cpu}")
        except KeyError:
            continue

if args.strip_node:
    def strip_nodes(node, path=""):
        for child in list(node):
            child_path = f"{path}/{child.name}"
            if any(pat.lower() in child.name.lower() for pat in args.strip_node):
                print(f"Removing ADT node {child_path}")
                del node[child.name]
            else:
                strip_nodes(child, child_path)

    strip_nodes(hv.adt)

if args.debug_xnu:
    hv.adt["chosen"].debug_enabled = 1

if args.volume:
    for path, tag in args.volume:
        hv.attach_virtio(Virtio9PTransport(root=path, tag=tag))

if args.logfile:
    hv.set_logfile(args.logfile.open("w"))

if len(args.boot_args) > 0:
    boot_args = " ".join(args.boot_args)
    hv.set_bootargs(boot_args)

symfile = None
if args.symbols:
    symfile = args.symbols.open("rb")

payload = args.payload.open("rb")

if args.append_payload:
    concat = BytesIO()
    concat.write(payload.read())
    for part in args.append_payload:
        concat.write(part.open("rb").read())
    concat.seek(0)
    payload = concat

if args.raw:
    hv.load_raw(payload.read(), args.entry_point)
else:
    hv.load_macho(payload, symfile=symfile)

PMU(u).reset_panic_counter()

for i in args.script:
    try:
        hv.run_script(i)
    except:
        traceback.print_exc()
        args.shell = True

for i in args.command:
    try:
        hv.run_code(i)
    except:
        traceback.print_exc()
        args.shell = True

if args.shell:
    run_shell(hv.shell_locals, "Entering hypervisor shell. Type ^D to start the guest.")

try:
    hv.start()
except ProxyRemoteError as exc:
    print(f"Hypervisor failed: {exc}. Target state retained for inspection.")

run_shell(hv.shell_locals, "Hypervisor exited. Entering shell.")

try:
    p.smp_stop_secondaries(True)
except ProxyRemoteError as exc:
    print(f"Secondary shutdown incomplete: {exc}. Continuing best-effort sleep.")
p.sleep(True)
