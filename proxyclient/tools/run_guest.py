#!/usr/bin/env python3
# SPDX-License-Identifier: MIT AND GPL-2.0-only
import sys, pathlib, traceback
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, pathlib, time
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
parser.add_argument('--windows-gic-cpuif', choices=('carrier', 'software'), default=None,
                    help='T8152: the software GIC CPU interface with every ICC_* access trapped '
                         '(default and required there: Phase 3 measurement (f) found the '
                         'hardware CPU interface path UNDEF). Off T8152 only "carrier" exists.')
parser.add_argument('--windows-wfi', choices=('poll', 'wfi'), default='poll',
                    help='T8152: carrier idle busy-polls (default). "wfi" (a real WFI at EL2) '
                         'is refused: Phase 3 measurement (a) found WFI_WIPES.')
parser.add_argument('--windows-serial', choices=('cdc', 'disabled'), default='cdc',
                    help='T8152: guest vUART on the dedicated CDC interface (default) or off.')
parser.add_argument('--windows-guest-compatibility', choices=('none', 't8152-pmu-undef'),
                    default='none',
                    help='T8152: "t8152-pmu-undef" enables the T8142 recovery for EL1/EL0 '
                         'PMUv3 accesses that UNDEF after Mu (redirecting the guest\'s sync '
                         'vectors and replacing access sites with HVCs). Not yet run on '
                         'T8152; coverage is not assured (see docs/windows-t8152.md). Mu\'s '
                         'own forwarded accesses are serviced either way.')
parser.add_argument('--windows-reset-on-exit', action='store_true',
                    help='T8152: when the guest asks for PSCI SYSTEM_RESET, shut NVMe down and '
                         'reset through WD1. Default: stay in the aura proxy.')
parser.add_argument('--windows-ahci-image', type=pathlib.Path, default=None, metavar='PATH',
                    help='T8152 D1: a raw GPT disk image (sparse allowed) for the carrier-emulated '
                         'AHCI disk. The disk lives in DRAM at the top of --windows-guest-ram; '
                         'guest RAM shrinks to what is left (at least 4 GiB). Needs an AHCI FD '
                         '(Mu J804_WINDOWS_AHCI, marker J804AHC1 at 0x110).')
parser.add_argument('--windows-ahci-store-size', default=None, metavar='SIZE',
                    help='T8152 D1: the disk size (e.g. 12G); default: the image size rounded up '
                         'to 256 MiB. At most what leaves 4 GiB of guest RAM.')
parser.add_argument('--windows-ahci-keep', action='store_true',
                    help='T8152 D1: reuse the disk a previous run sealed (after a PSCI '
                         'SYSTEM_OFF/RESET and an in-place carrier reload). The resident checks '
                         'the sealed CRC-32 before attaching it. Not with --windows-ahci-image.')
parser.add_argument('--windows-ahci-keep-unsealed', action='store_true',
                    help='T8152 D1: like --windows-ahci-keep, but also accept a store whose last '
                         'run did not end with a PSCI exit (no integrity check is possible). '
                         'UNSAFE after a hard reset, panic reboot or power cycle: DRAM is not '
                         'preserved then. The resident refuses when the system counter shows a '
                         'reset since the store was last alive, but cannot catch every reset.')
parser.add_argument('--windows-ahci-keep-expect', default=None, metavar='SHA256',
                    help='T8152 D1, with --windows-ahci-keep(-unsealed): refuse unless the kept '
                         'store was uploaded from the image with this SHA-256 (from its host '
                         'note; the note is printed either way).')
parser.add_argument('--windows-ahci-overwrite', action='store_true',
                    help='T8152 D1, with --windows-ahci-image: replace a READY, RUNNING or '
                         'SEALED store found at the window top. Without it a fresh upload over '
                         'such a store is refused, so a Windows install is not lost by mistake.')
parser.add_argument('--windows-ahci-boot', choices=('auto', 'disk', 'ramdisk'), default='auto',
                    help='T8152 D1: what Mu boots first: "auto" (default) is the appended RAM '
                         'disk when -a gives one, else the AHCI disk.')
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


def ahci_requested():
    return bool(args.windows_ahci_image or args.windows_ahci_store_size or args.windows_ahci_keep
                or args.windows_ahci_keep_unsealed or args.windows_ahci_boot != 'auto'
                or args.windows_ahci_keep_expect or args.windows_ahci_overwrite)


def no_timeout(call, *a):
    """A resident op that walks the whole store (zero-fill, CRC-32): no link timeout."""
    timeout = iface.dev.timeout
    iface.dev.timeout = None
    try:
        return call(*a)
    finally:
        iface.dev.timeout = timeout


def windows_t8152_ahci_plan(base, top, payload):
    """D1: check the resident, the FD and the options, and split the window.  Nothing on the
    machine changes here; a refusal leaves the proxy as it was."""
    from aura import windows as w
    try:
        if args.windows_ahci_image and (args.windows_ahci_keep or args.windows_ahci_keep_unsealed):
            raise ValueError("--windows-ahci-keep reuses the sealed disk: it takes no "
                             "--windows-ahci-image")
        if args.windows_reset_on_exit and ahci_requested():
            raise ValueError("--windows-reset-on-exit resets the machine through WD1, which "
                             "loses the DRAM disk: not with an AHCI disk")
        marker = w.fd_ahci_marker(payload)
        if not ahci_requested():
            if marker is not None:
                raise ValueError("this is an AHCI FD (J804AHC1 marker): pass "
                                 "--windows-ahci-image or --windows-ahci-keep, or use the "
                                 "non-AHCI FD")
            return None
        keep = args.windows_ahci_keep or args.windows_ahci_keep_unsealed
        if not args.windows_ahci_image and not keep:
            raise ValueError("an AHCI option needs --windows-ahci-image or --windows-ahci-keep")
        if keep and args.windows_ahci_store_size:
            raise ValueError("--windows-ahci-keep takes the size from the sealed store")
        if keep and args.windows_ahci_overwrite:
            raise ValueError("--windows-ahci-overwrite replaces a store: not with "
                             "--windows-ahci-keep")
        if args.windows_ahci_keep_expect and not keep:
            raise ValueError("--windows-ahci-keep-expect checks a kept store: it needs "
                             "--windows-ahci-keep")
        try:
            caps2 = p.hv_windows_capabilities2()
        except ProxyRemoteError:
            caps2 = 0  # an older resident: Bad Command
        query = lambda name: p.hv_windows_ahci(w.AHCI_OP_QUERY, w.AHCI_Q[name])
        w.validate_ahci_contract(caps2, query if caps2 else (lambda name: None), payload)
        boot = w.resolve_ahci_boot(args.windows_ahci_boot, bool(args.append_payload))
        blob = iface.readmem(top - w.AHCI_DESC_SIZE, w.AHCI_NOTE_OFF + w.AHCI_NOTE_SIZE)
        if keep:
            desc = w.parse_ahci_desc(blob)
            note = w.parse_ahci_note(blob)
            print(f"T8152 AHCI: the store's host note: {w.describe_ahci_note(note)}")
            plan = w.ahci_keep_plan(base, top, desc, args.windows_ahci_keep_unsealed)
            w.check_ahci_keep_expect(note, args.windows_ahci_keep_expect)
            print(f"T8152 AHCI: keeping the {desc['state_name']} store, generation "
                  f"{desc['generation']}, disk CRC-32 {desc['disk_crc32']:#010x}")
            image = None
        else:
            w.check_ahci_fresh_overwrite(blob, top, args.windows_ahci_overwrite)
            image = args.windows_ahci_image
            size, warnings = w.ahci_image_info(image)
            for warning in warnings:
                print(f"T8152 AHCI: warning: {image}: {warning}")
            disk = (w.parse_size(args.windows_ahci_store_size) if args.windows_ahci_store_size
                    else w.ahci_default_disk_bytes(size))
            plan = w.plan_t8152_ahci(base, top, disk, size)
            if disk > size:
                print(f"T8152 AHCI: the disk ({disk} bytes) is larger than the image ({size}); "
                      "the GPT backup header is not at the disk's last LBA until the guest "
                      "repairs it")
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    print(f"T8152 AHCI: disk [{plan.store_base:#x}, {plan.store_base + plan.disk_bytes:#x}) "
          f"{plan.disk_bytes / 2**30:.2f} GiB, descriptor {plan.desc_base:#x}, guest RAM "
          f"[{plan.window_base:#x}, {plan.guest_top:#x}) {plan.guest_bytes / 2**30:.2f} GiB, "
          f"boot {boot}")
    return {"plan": plan, "boot": boot, "keep": keep, "image": image,
            "unsealed": args.windows_ahci_keep_unsealed,
            "overwrite": args.windows_ahci_overwrite}


def windows_t8152_ahci_attach(hv, u, ahci):
    """After load_raw: attach the store (fresh: zero it, upload the image, seal the upload)."""
    from aura import windows as w
    plan = ahci["plan"]
    try:
        w.check_t8152_ahci_layout(plan, hv.adt_base, hv.tba.top_of_kernel_data)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. hv_init() ran, so the carrier session is used "
                 "up: reload the carrier before another attempt. No guest was started.")
    flags = 0
    if ahci["keep"]:
        flags |= w.AHCI_ATTACH_KEEP
    if ahci["unsealed"]:
        flags |= w.AHCI_ATTACH_ACCEPT_UNSEALED
    if ahci["overwrite"]:
        flags |= w.AHCI_ATTACH_OVERWRITE
    print("T8152 AHCI: attaching the store (a fresh one is zeroed; a kept one is CRC-checked)")
    if no_timeout(p.hv_windows_ahci, w.AHCI_OP_ATTACH, plan.store_base, plan.window_top,
                  plan.disk_bytes, flags) != 0:
        sys.exit("T8152: the resident refused the AHCI store (its console says why). The carrier "
                 "session is used up: reload the carrier. No guest was started.")
    if ahci["keep"]:
        return
    start = time.time()

    def write(offset, data):
        u.compressed_writemem(plan.store_base + offset, data, True)

    def progress(done, total, sent):
        if done % (1 << 30) < w.AHCI_CHUNK or done == total:
            print(f"T8152 AHCI: {done >> 20} / {total >> 20} MiB scanned, {sent >> 20} MiB "
                  f"uploaded, {time.time() - start:.0f} s")

    crc, sha, sent = w.upload_ahci_image(ahci["image"], plan.disk_bytes, write, progress=progress)
    size = ahci["image"].stat().st_size
    iface.writemem(plan.desc_base + w.AHCI_NOTE_OFF,
                   w.ahci_host_note(sha, size, time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())))
    sealed = no_timeout(p.hv_windows_ahci, w.AHCI_OP_SEAL_UPLOAD, crc)
    if sealed != crc:
        sys.exit(f"T8152: the resident's CRC-32 of the store does not match the host's "
                 f"{crc:#010x}: the upload is damaged. Reload the carrier. No guest was started.")
    print(f"T8152 AHCI: {sent >> 20} MiB uploaded in {time.time() - start:.0f} s, image sha256 "
          f"{sha}, disk CRC-32 {crc:#010x} verified by the resident")


def windows_t8152_ahci_seal(result, ahci):
    from aura import windows as w
    if result not in (1, 2):
        print("T8152 AHCI: the guest did not end with a PSCI exit: the store is NOT sealed "
              "(--windows-ahci-keep-unsealed can still reuse it, unverified, after an in-place "
              "carrier reload; not after a hard reset or power cycle)")
        return
    sealed = no_timeout(p.hv_windows_ahci, w.AHCI_OP_SEAL_EXIT)
    if sealed == w.AHCI_ERROR:
        print("T8152 AHCI: the resident refused to seal the store (its console says why)")
        return
    print(f"T8152 AHCI: store sealed, generation {sealed >> 32}, disk CRC-32 "
          f"{sealed & 0xffffffff:#010x}. To keep it: reload the carrier in place (no hard reset, "
          f"no power cycle) and run again with --windows-ahci-keep and the same "
          f"--windows-guest-ram")


def run_windows_t8152(caps):
    """The Windows personality (gmake WINDOWS=1) on T8152: the guest gets only its
    RAM window, the AIC (Device-nGnRnE) and emulated MMIO; the resident refuses to
    enter it unless every T8152 safety precondition holds (src/hv/windows/hv_t8152.c)."""
    from aura.windows import (AURA_HEAP_RESERVE, PROXY_HEAP_SIZE, T8152_CHIP, WindowsLaunchRefused,
                              number, t8152_check_window, windows_system_reset)
    if not args.raw:
        sys.exit("T8152 Windows guests are raw Mu FDs: pass -r")
    if args.symbols or args.debug_xnu or args.volume or args.emulate_sprr or args.cpus or \
            args.strip_node or args.shell or args.script or args.command:
        sys.exit("T8152 Windows guests take only -r, -E, -a, -l and boot arguments")
    if not args.windows_guest_ram or ":" not in args.windows_guest_ram:
        sys.exit("T8152 needs --windows-guest-ram BASE:TOP (see docs/windows-t8152.md)")
    base, top = (number(v, "guest RAM") for v in args.windows_guest_ram.split(":", 1))
    payload = args.payload.read_bytes()
    ahci = windows_t8152_ahci_plan(base, top, payload)

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
    hv.windows_guest_ram = (base, ahci["plan"].store_base) if ahci else (base, top)
    hv.windows_ahci = ahci
    hv.windows_ipi = args.windows_ipi
    hv.windows_gic_cpuif = args.windows_gic_cpuif or "software"
    hv.windows_wfi = args.windows_wfi
    hv.windows_guest_compatibility = args.windows_guest_compatibility
    log = args.logfile.open("w") if args.logfile else None
    if log:
        iface.tty_log = log  # the resident's hv_init() refusals ("HV: T8152: ...") land here too
    try:
        hv.init()
    except (ValueError, WindowsLaunchRefused) as exc:
        # The launcher's own checks, or hv_windows_configure() refusing: nothing changed.
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    except ProxyRemoteError as exc:
        # The resident failed a request, most likely hv_init().  Its console says why.
        sys.exit(f"T8152: the resident failed during hv.init(): {exc}. Read its last "
                 "'TTY> HV: ...' lines (also in the -l log). If they end with a preflight "
                 "refusal ('HV: T8152: ... refusing' and 'precondition(s) failed'), nothing was "
                 "changed: fix the cause and run again. Otherwise hv_init() got past preflight "
                 "and the session is used up: reload the carrier before another attempt. No "
                 "guest was started.")
    except (UartError, ProxyReplyError):
        # A timeout or a broken link says nothing about where the resident stopped.
        print("T8152: the link to the aura proxy failed during hv.init(); the proxy's state is "
              "unknown. Check the M6 before doing anything else.", file=sys.stderr)
        raise
    if log:
        hv.set_logfile(log)
    if args.boot_args:
        hv.set_bootargs(" ".join(args.boot_args))
    data = payload
    for part in args.append_payload:
        data += part.read_bytes()
    hv.load_raw(data, args.entry_point)
    if hv.guest_base % 0x10000:
        sys.exit("Mu payload base is not 64 KiB aligned")
    print(f"T8152: Mu at {hv.guest_base:#x}, entry {hv.entry:#x}, "
          f"boot_args {hv.guest_base + hv.bootargs_off:#x}")
    if ahci:
        windows_t8152_ahci_attach(hv, u, ahci)
    try:
        result = hv.start()
    except ProxyRemoteError as exc:
        sys.exit(f"Hypervisor refused or failed: {exc}. The aura proxy keeps running; "
                 "its console says why.")
    print(f"Guest exit status: {result} (1 SYSTEM_OFF, 2 SYSTEM_RESET)")
    if ahci:
        windows_t8152_ahci_seal(result, ahci)
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
      args.windows_gic_cpuif not in (None, 'carrier') or args.windows_wfi != 'poll' or
      args.windows_guest_compatibility != 'none' or ahci_requested()):
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
