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
