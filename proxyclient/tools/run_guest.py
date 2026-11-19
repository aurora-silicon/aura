#!/usr/bin/env python3
# SPDX-License-Identifier: MIT AND GPL-2.0-only
import sys, pathlib, traceback
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, hashlib, pathlib, time
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
                         'reset through WD1. Default: stay in the aura proxy. Refused with the '
                         'D1 RAM disk; with the D5 NVMe disk only on a resident with CAPS2 bit 5, '
                         'and the reset happens only if the resident reports the close CLEAN.')
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
parser.add_argument('--windows-ahci-backend', choices=('ram', 'nvme'), default='ram',
                    help='T8152: the AHCI disk\'s backend. "ram" (default) is the D1 DRAM store '
                         '(--windows-ahci-image / --windows-ahci-keep). "nvme" (D5) is a dedicated '
                         'GPT partition of the internal NVMe, read and written by the carrier: '
                         'persistent, no upload. Needs --windows-ahci-partition and '
                         '--windows-ahci-nvme-access; none of the RAM-store options.')
parser.add_argument('--windows-ahci-partition', default=None, metavar='GUID|NAME',
                    help='T8152 D5: the Windows partition, by its GPT unique GUID or GPT name as '
                         'the private config records them (never a partition number). The '
                         'resident re-checks disk GUID, partition GUID, type, name and LBAs '
                         'against the live GPT.')
parser.add_argument('--windows-ahci-nvme-config', type=pathlib.Path, default=None, metavar='PATH',
                    help='T8152 D5: the private partition config (default '
                         '$J804_PRIVATE_DATA/d5/windows-partition.env).')
parser.add_argument('--windows-ahci-nvme-access', choices=('ro', 'rw'), default=None,
                    help='T8152 D5, required with --windows-ahci-backend nvme: "ro" aborts every '
                         'write in the carrier (first sessions, WinPE); "rw" lets Windows write '
                         'its partition.')
parser.add_argument('--windows-ahci-nvme-flush', choices=('normal', 'every-write'),
                    default='normal',
                    help='T8152 D5: "every-write" adds an NVMe flush after every write command '
                         '(experiments). FLUSH CACHE always maps to an NVMe flush.')
parser.add_argument('--windows-usb', default='off', metavar='{off,drd1,drd1+drd3,drd3}',
                    help='T8152 D4: pass the usb-drd1 (left-rear USB-C) and/or usb-drd3 (right) '
                         'xHCI through to Windows, with the carrier\'s DART identity map of '
                         'guest RAM and AIC 1692 -> SPI 995 (1834 -> 997). Default off. Needs a '
                         'USB FD (Mu J804_WINDOWS_USB, marker J804USB1 at 0x120). usb-drd0, the '
                         'proxy\'s link, is refused in every spelling.')
parser.add_argument('--windows-gpu', default='off', metavar='{off,admit,log}',
                    help='T8152 G0: "admit" runs the resident\'s GPU admission once (firmware '
                         'identities against the live ADT, range table, firmware classes, the '
                         'read-only handoff page Mu\'s GPU0 reads); "log" also puts read-as-zero '
                         'logging hooks on the sgx window and the post-office page. Neither '
                         'powers the GPU, starts its firmware or passes anything through. '
                         'Default off.')
parser.add_argument('--windows-gpu-pool-mib', type=int, default=256,
                    help='T8152 G0: the GPU pool, carved from the top of guest RAM (multiple of '
                         '32, at most half of guest RAM). Default 256.')
parser.add_argument('--windows-input', default='off', metavar='{off,host,host+kbd,host+kbd+tp}',
                    help='T8152 I0-I2: the carrier\'s input device (page 0xF50000000, SPI 991). '
                         '"host" is fed from this Mac (proxyclient/aura/windows.py input frames '
                         'on the vUART CDC port, /dev/cu.usbmodemP_03 RX); "host+kbd" adds the '
                         'M6\'s own keyboard and "host+kbd+tp" its trackpad too, both through '
                         'the MTP, which the carrier brings up before entry in an 8 MiB area '
                         'carved off the top of the guest RAM window (leave such a session only '
                         'by a reboot). Default off. Needs an input FD (Mu J804_WINDOWS_INPUT, '
                         'marker J804INP1 at 0x140), --windows-serial cdc and the m6input '
                         'driver in the Windows image.')
parser.add_argument('--windows-input-fn-default', choices=('media', 'fkeys'), default='media',
                    help='T8152 I1: the internal keyboard\'s top row without Fn: media keys '
                         '(default, as macOS) or F1-F12.')
parser.add_argument('--windows-input-require-mtp', action='store_true',
                    help='T8152 I1/I2: refuse to start the guest unless every asked internal '
                         'source (keyboard, trackpad) came up at ATTACH.')
parser.add_argument('--windows-input-probe', type=int, default=0, metavar='SECONDS',
                    help='T8152 I1/I2: after ATTACH and before guest entry, let the carrier '
                         'service the MTP and log every keyboard/trackpad frame and translation '
                         'for SECONDS (1-120; type and touch on the M6 meanwhile). Needs '
                         'host+kbd or host+kbd+tp.')
parser.add_argument('--windows-input-probe-only', action='store_true',
                    help='T8152 I1/I2: stop after --windows-input-probe: no guest is started '
                         '(a carrier-only MTP check). The MTP stays running: leave by a reboot.')
parser.add_argument('--windows-input-tp-fw', type=pathlib.Path, default=None,
                    help='T8152 I2: the trackpad firmware container (HIDF, the owner\'s own file '
                         'lifted from their recoveryOS; never in a repo). Default '
                         '$J804_PRIVATE_DATA/firmware/tpmtfw-j804.bin.')
parser.add_argument('--windows-input-tp-tap', choices=('on', 'off'), default='on',
                    help='T8152 I2: tap to click (one finger left, two right). Default on.')
parser.add_argument('--windows-input-tp-scroll', choices=('traditional', 'natural'),
                    default='traditional',
                    help='T8152 I2: two-finger scroll direction. Default traditional.')
parser.add_argument('--windows-input-tp-gain', type=float, default=6.0,
                    help='T8152 I2: pointer counts per mm of finger travel when slow. Default 6.')
parser.add_argument('--windows-input-tp-accel', type=float, default=3.0,
                    help='T8152 I2: pointer acceleration: the extra factor reached at 150 mm/s '
                         '(0 = none). Default 3 (4x gain when fast).')
parser.add_argument('--windows-input-tp-scroll-mm', type=float, default=5.0,
                    help='T8152 I2: finger travel per wheel or pan detent. Default 5 mm.')
parser.add_argument('--windows-serror', choices=('abort', 'continue'), default='abort',
                    help='What a guest (lower-EL) SError does after the carrier has printed its '
                         'report: "abort" exits the guest (default, unchanged); "continue" logs '
                         'the guest frame and resumes the guest, at most 8 times per session, '
                         'to collect more reports. The SError is consumed; the access that '
                         'raised it is lost, so a continued session is diagnostic only.')
parser.add_argument('payload', type=pathlib.Path)
parser.add_argument('boot_args', default=[], nargs="*")
args = parser.parse_args()

from aura.proxy import *
from aura.proxyutils import *
from aura.utils import *
from aura.shell import run_shell
from aura.sysreg import *
from aura.hv import HV, WindowsGuestAborted, WindowsGuestExitTimeout
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


def ahci_ram_requested():
    return bool(args.windows_ahci_image or args.windows_ahci_store_size or args.windows_ahci_keep
                or args.windows_ahci_keep_unsealed or args.windows_ahci_keep_expect
                or args.windows_ahci_overwrite)


def ahci_nvme_requested():
    return bool(args.windows_ahci_backend == 'nvme' or args.windows_ahci_partition
                or args.windows_ahci_nvme_config or args.windows_ahci_nvme_access
                or args.windows_ahci_nvme_flush != 'normal')


def ahci_requested():
    return bool(ahci_ram_requested() or ahci_nvme_requested() or args.windows_ahci_boot != 'auto')


def no_timeout(call, *a):
    """A resident op that walks the whole store (zero-fill, CRC-32): no link timeout."""
    timeout = iface.dev.timeout
    iface.dev.timeout = None
    try:
        return call(*a)
    finally:
        iface.dev.timeout = timeout


def bounded_timeout(seconds, call, *a):
    """A resident op that may take long but must not hang the launcher: a finite link
    timeout (UartTimeout past it) instead of no_timeout's None."""
    timeout = iface.dev.timeout
    iface.dev.timeout = seconds
    try:
        return call(*a)
    finally:
        iface.dev.timeout = timeout


# DETACH retries the NVMe close: at most a 12 s lock wait, a 10 s flush and the shutdown.
NVME_DETACH_TIMEOUT_S = 90


def windows_t8152_ahci_plan(base, top, payload):
    """D1: check the resident, the FD and the options, and split the window.  Nothing on the
    machine changes here; a refusal leaves the proxy as it was."""
    from aura import windows as w
    try:
        if args.windows_ahci_image and (args.windows_ahci_keep or args.windows_ahci_keep_unsealed):
            raise ValueError("--windows-ahci-keep reuses the sealed disk: it takes no "
                             "--windows-ahci-image")
        if args.windows_reset_on_exit and ahci_ram_requested():
            raise ValueError("--windows-reset-on-exit resets the machine through WD1, which "
                             "loses the DRAM disk: not with an AHCI RAM disk")
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


def windows_t8152_ahci_nvme_plan(base, top, payload):
    """D5: the AHCI disk on an internal NVMe partition.  Checks the options, the private config,
    the resident (CAPS2 bit 5) and the FD; nothing on the machine changes here."""
    from aura import windows as w
    try:
        if ahci_ram_requested():
            raise ValueError("--windows-ahci-backend nvme has no DRAM store: not with "
                             "--windows-ahci-image, -store-size, -keep, -keep-unsealed, "
                             "-keep-expect or -overwrite")
        if args.windows_ahci_backend != 'nvme':
            raise ValueError("--windows-ahci-partition and --windows-ahci-nvme-* need "
                             "--windows-ahci-backend nvme")
        if not args.windows_ahci_nvme_access:
            raise ValueError("--windows-ahci-backend nvme needs --windows-ahci-nvme-access ro|rw "
                             "(no default: the first sessions are ro)")
        if not args.windows_ahci_partition:
            raise ValueError("--windows-ahci-backend nvme needs --windows-ahci-partition "
                             "GUID|NAME")
        config = args.windows_ahci_nvme_config or w.default_nvme_config_path()
        if config is None:
            raise ValueError("no partition config: pass --windows-ahci-nvme-config or set "
                             "J804_PRIVATE_DATA")
        part = w.resolve_nvme_partition(w.load_nvme_config(config), args.windows_ahci_partition)
        rw = args.windows_ahci_nvme_access == 'rw'
        boot = w.resolve_ahci_boot(args.windows_ahci_boot, bool(args.append_payload))
        if rw and boot == 'ramdisk':
            raise ValueError("a WinPE ramdisk boot with a writable internal disk is not a D5 "
                             "session: use --windows-ahci-nvme-access ro with ramdisk")
        try:
            caps2 = p.hv_windows_capabilities2()
        except ProxyRemoteError:
            caps2 = 0  # an older resident: Bad Command
        w.validate_nvme_contract(caps2)
        query = lambda name: p.hv_windows_ahci(w.AHCI_OP_QUERY, w.AHCI_Q[name])
        w.validate_ahci_contract(caps2, query, payload)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    flags = (w.NVME_RW if rw else 0) | \
        (w.NVME_FLUSH_EVERY if args.windows_ahci_nvme_flush == 'every-write' else 0)
    plan = w.nvme_ahci_plan(base, top)
    print(f"T8152 AHCI NVMe: partition '{part.name}' (by GUID and name from {config}), NVMe "
          f"LBAs [{part.first}, {part.last}] = {part.size / 2**30:.2f} GiB, "
          f"{'READ-WRITE' if rw else 'read-only'}, boot {boot}; guest RAM is the whole window "
          f"[{base:#x}, {top:#x}) {plan.guest_bytes / 2**30:.2f} GiB")
    return {"backend": "nvme", "plan": plan, "boot": boot, "keep": False, "image": None,
            "unsealed": False, "overwrite": False, "part": part, "flags": flags}


def windows_t8152_ahci_nvme_attach(hv, u, ahci):
    """After load_raw: the resident wakes the NVMe, checks the live GPT against the request,
    sets its I/O fence and installs the disk.  A refusal shuts the NVMe down again."""
    from aura import windows as w
    try:
        w.check_t8152_ahci_layout(ahci["plan"], hv.adt_base, hv.tba.top_of_kernel_data)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. hv_init() ran, so the carrier session is used "
                 "up: reload the carrier before another attempt. No guest was started.")
    blob = w.nvme_request(ahci["part"], ahci["flags"])
    addr = u.malloc(len(blob))
    iface.writemem(addr, blob)
    print("T8152 AHCI NVMe: attaching (NVMe wake, Identify, outer and inner GPT checks)")
    result = no_timeout(p.hv_windows_ahci, w.AHCI_OP_ATTACH_NVME, addr)
    u.free(addr)
    if result != 0:
        sys.exit("T8152: the resident refused the NVMe disk (its console says why; the NVMe was "
                 "shut down again and nothing was written). The carrier session is used up: "
                 "reload the carrier. No guest was started.")
    status = lambda name: p.hv_windows_ahci(w.AHCI_OP_NVME_STATUS, w.NVME_STATUS[name])
    found = (status("first"), status("last"))
    if found != (ahci["part"].first, ahci["part"].last):
        no_timeout(p.hv_windows_ahci, w.AHCI_OP_DETACH)
        sys.exit(f"T8152: the resident attached LBAs {found}, not the configured "
                 f"[{ahci['part'].first}, {ahci['part'].last}]; detached. No guest was started.")
    print(f"T8152 AHCI NVMe: attached LBAs [{found[0]}, {found[1]}], inner disk "
          f"{status('inner_bytes') >> 20} MiB; I/O fence set")


def windows_t8152_ahci_nvme_detach():
    """Any launcher exit between ATTACH and the guest: close (idempotent)."""
    from aura import windows as w
    try:
        no_timeout(p.hv_windows_ahci, w.AHCI_OP_DETACH)
    except ProxyRemoteError:
        pass


def windows_t8152_ahci_nvme_close(result, ahci, hv=None):
    """After hv.start(): the resident closed the disk on its way out (CLEAN or FAILED).  DETACH
    is idempotent and retries the shutdown of a FAILED close once.  True if a reset is safe.
    Every read is bounded: a resident that does not answer gives the verdict UNKNOWN, never a
    hang."""
    from aura import windows as w
    status = lambda name: p.hv_windows_ahci(w.AHCI_OP_NVME_STATUS, w.NVME_STATUS[name])
    try:
        state = status("state")
        if state != 3:
            try:
                bounded_timeout(NVME_DETACH_TIMEOUT_S, p.hv_windows_ahci, w.AHCI_OP_DETACH)
            except ProxyRemoteError:
                pass
            state = status("state")
        counters = (f"{status('commands')} commands, {status('failed_commands')} failed, "
                    f"{status('blocks_read') >> 8} MiB read, {status('blocks_written') >> 8} MiB "
                    f"written, {status('flushes')} flushes, {status('rmw')} RMW blocks; longest "
                    f"NVMe command {status('longest_us')} us, longest flush "
                    f"{status('longest_flush_us')} us; guard hits {status('guard_hits')}")
        wrote = status("wrote")
    except (UartError, ProxyError, OSError) as exc:
        # OSError: pyserial's SerialException, the link itself is gone (a carrier reset after
        # the close or a park, a watchdog bark, a cable pull): the verdict is UNKNOWN too.
        print(f"\x1b[31mT8152 AHCI NVMe: UNKNOWN: the resident did not answer the close-state "
              f"read ({exc!r}). Treat the disk as not closed: do not leave the proxy (no "
              "P_REBOOT, no direct watchdog write through the proxy, no macvdmtool, no power "
              "hold) until p.hv_windows_ahci(7, 0) reports 3 (CLEAN). If the M6 cannot be "
              "reached any more, any reset risks the Windows partition: restore it from the "
              "image or run chkdsk from WinPE before the next read-write session.\x1b[0m")
        said = hv.windows_nvme_console if hv is not None else None
        if said:
            print(f"T8152 AHCI NVMe: the carrier's console last said: {said!r} (not re-read: "
                  "the state may have changed since)")
        return False
    print(f"T8152 AHCI NVMe: {counters}")
    name = w.describe_nvme_state(state)
    if state == 3:
        print(f"T8152 AHCI NVMe: CLEAN: the controller confirmed its shutdown"
              f"{' after this session wrote' if wrote else ' (nothing written)'}. "
              "A reset is safe now: through the carrier (P_REBOOT, i.e. "
              "tools/windows_nvme_gate.py exit, or --windows-reset-on-exit).")
        return True
    print(f"\x1b[31mT8152 AHCI NVMe: {name}: the controller did NOT confirm its shutdown after "
          "this session wrote. Do NOT leave the proxy (no P_REBOOT, no direct watchdog write "
          "through the proxy: that is a hard reset; no macvdmtool, no power hold): retry "
          "p.hv_windows_ahci(6) from the proxy shell. If it stays FAILED, any reset (P_REBOOT "
          "\"FORCE\" included) risks the Windows partition: restore it from the image or run "
          "chkdsk from WinPE, and run the read-only checks of the D5 runbook from Linux "
          "afterwards.\x1b[0m")
    return False


def windows_t8152_fatal_summary(hv):
    """After an aborted session: the carrier's own fatal report (its guest SError report among
    them) is in the console output above; repeat the lines that say what happened, so they
    stand next to the disk verdict that follows."""
    if hv.windows_abort_reason is not None:
        print(f"T8152: the guest session was aborted on a forwarded exception: "
              f"{hv.windows_abort_reason} (reason 2 = guest exception; code 3 = SError)")
    lines = hv.windows_fatal_summary()
    if not lines:
        if hv.windows_abort_reason is not None:
            print("T8152: the carrier printed no fatal report this session")
        return
    print(f"T8152: the carrier's fatal report ({len(hv.windows_fatal_lines)} lines above), "
          "summary:")
    for line in lines:
        print(f"  {line}")


def windows_t8152_usb_plan(payload, u):
    """D4: check --windows-usb against the chip, the board, the resident and the FD before
    anything is uploaded.  Returns the port tuple (empty when off)."""
    from aura import windows as w
    try:
        ports = w.parse_windows_usb(args.windows_usb)
        caps2 = 0
        if ports:
            try:
                caps2 = p.hv_windows_capabilities2()
            except ProxyRemoteError:
                caps2 = 0  # an older resident: Bad Command
        chosen = u.adt["/chosen"]
        w.validate_usb_request(ports, chosen.chip_id, getattr(chosen, "board_id", -1), caps2,
                               payload)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    if ports:
        print(f"T8152 USB: passing {w.usb_spelling(ports)} through (CAPS2 {caps2:#x}, FD "
              f"markers {sorted(w.fd_usb_markers(payload))})")
    return ports


def windows_t8152_usb_attach(hv, ports, ahci):
    """After the AHCI store, before hv.start(): the resident checks the port, builds the DART
    map of guest RAM and passes the xHCI through.  Only then does the guest ADT get Mu's
    /chosen/aura,windows-usb-v1 handshake, with the DART window the resident reports.  A
    refusal stops the launch."""
    from aura import windows as w
    if not ports:
        return
    result = p.hv_windows_usb(w.USB_OP_ATTACH, w.usb_port_mask(ports))
    if result != 0:
        p.hv_windows_usb(w.USB_OP_DETACH)
        sys.exit(f"T8152: the resident refused USB passthrough: {w.describe_usb_attach(result)} "
                 "(its console says why). hv_init() ran, so the carrier session is used up: "
                 "reload the carrier. No guest was started.")
    try:
        dma = (p.hv_windows_usb(w.USB_OP_STATUS, 0, w.USB_STATUS_DMA_BASE),
               p.hv_windows_usb(w.USB_OP_STATUS, 0, w.USB_STATUS_DMA_TOP))
        value = w.usb_handshake(ports, *dma, hv.windows_guest_ram,
                                ahci["plan"] if ahci else None)
        hv.set_windows_usb_handshake(value)
    except (ValueError, ProxyRemoteError) as exc:
        p.hv_windows_usb(w.USB_OP_DETACH)
        sys.exit(f"T8152: USB attached but the Mu handshake was refused: {exc}. USB was torn "
                 "down again; the carrier session is used up: reload the carrier. No guest was "
                 "started.")
    print(f"T8152 USB: {w.usb_spelling(ports)} attached, DART window [{dma[0]:#x}, "
          f"{dma[1]:#x}), /chosen/{w.USB_HANDSHAKE_PROPERTY} written; plug the keyboard and "
          "mouse in before Windows boots")


def windows_t8152_gpu_plan(u):
    """G0: check --windows-gpu against the chip, the board and the resident before anything is
    uploaded.  Returns the mode ("off" asks nothing of the resident)."""
    from aura import windows as w
    try:
        mode = w.parse_windows_gpu(args.windows_gpu)
        caps2 = 0
        if mode != "off":
            try:
                caps2 = p.hv_windows_capabilities2()
            except ProxyRemoteError:
                caps2 = 0  # an older resident: Bad Command
        chosen = u.adt["/chosen"]
        w.validate_gpu_request(mode, chosen.chip_id, getattr(chosen, "board_id", -1), caps2)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    return mode


def windows_t8152_gpu_admit(hv, mode, usb_ports=(), ahci=None):
    """After USB, before hv.start(): the resident admits the GPU firmware and builds the
    handoff page (and, for "log", hooks the sgx window).  Only then does the guest ADT get Mu's
    /chosen/aura,windows-gpu-v1.  A refusal stops the launch: the session is used up.  USB was
    attached first, so every refusal here detaches it again, as the USB and hv.start() failure
    paths do: no xHCI passthrough or DART map of guest RAM outlives a launch with no guest."""
    from aura import windows as w
    if mode == "off":
        return

    def refuse(message):
        if usb_ports:
            try:
                p.hv_windows_usb(w.USB_OP_DETACH)
            except ProxyRemoteError:
                pass
            message += " USB was detached again."
        sys.exit(message)

    base, top = hv.windows_guest_ram
    try:
        pool = w.gpu_pool(base, top, args.windows_gpu_pool_mib)
        # The pool is the top of guest RAM: the loaded image (Mu FD, appended payloads, SEPFW,
        # preoslog, boot_args) must end below it, and with an AHCI store Windows keeps its floor.
        w.gpu_pool_check_layout(pool, base, hv.tba.top_of_kernel_data,
                                w.AHCI_MIN_GUEST if ahci else None)
    except ValueError as exc:
        refuse(f"T8152 launch refused: {exc}. hv_init() ran, so the carrier session is used "
               "up: reload the carrier. No guest was started.")
    result = p.hv_windows_gpu(w.GPU_OP_ADMIT, *pool)
    if result != 0:
        refuse(f"T8152: the resident refused GPU admission: {w.describe_gpu(result)} (its "
               "console says why). Reload the carrier; no guest was started.")
    if mode == "log":
        result = p.hv_windows_gpu(w.GPU_OP_HOOK)
        if result != 0:
            refuse(f"T8152: the resident refused the GPU logging hooks: "
                   f"{w.describe_gpu(result)}. Reload the carrier; no guest was started.")
    ipa = p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_HANDOFF_IPA)
    size = p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_HANDOFF_SIZE)
    try:
        hv.set_windows_gpu_handshake(w.gpu_handshake(ipa, size))
    except ValueError as exc:
        refuse(f"T8152: GPU admitted but the Mu handshake was refused: {exc}. Reload the "
               "carrier; no guest was started.")
    ranges = p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_RANGES)
    print(f"T8152 GPU: admitted ({mode}), {ranges} ranges, pool [{pool[0]:#x}, "
          f"{pool[0] + pool[1]:#x}), handoff page {ipa:#x} read-only, "
          f"/chosen/{w.GPU_HANDSHAKE_PROPERTY} written; not powered, nothing passed through")


def windows_t8152_gpu_status(mode):
    from aura import windows as w
    if mode == "off":
        return
    try:
        total = p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_ACCESSES)
        gfx = p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_MAILBOX_GFX)
        per = {name: p.hv_windows_gpu(w.GPU_OP_STATUS, w.GPU_STATUS_CLASS + i)
               for i, name in enumerate(w.GPU_CLASSES)}
    except ProxyRemoteError as exc:
        print(f"T8152 GPU: status unavailable: {exc}")
        return
    busy = ", ".join(f"{k} {v}" for k, v in per.items() if v)
    print(f"T8152 GPU: {total} guest GPU accesses hooked ({gfx} in the GFX mailbox windows)"
          f"{': ' + busy if busy else ''}")


def windows_t8152_usb_status(ports):
    from aura import windows as w
    for port in ports:
        try:
            st = {name: p.hv_windows_usb(w.USB_OP_STATUS, port, index)
                  for name, index in w.USB_STATUS.items()}
        except ProxyRemoteError as exc:
            print(f"T8152 USB: drd{port} status unavailable: {exc}")
            continue
        print(f"T8152 USB: drd{port} at exit: USBSTS {st['usbsts']:#x} PORTSC "
              f"{st['portsc1']:#x}/{st['portsc2']:#x}, {st['irqs']} interrupts, DART exceptions "
              f"{st['dart0_exc']:#x}/{st['dart1_exc']:#x}, GCTL {st['gctl']:#x}, unowned lines "
              f"{st['unowned']}{', STORM' if st['storm'] else ''}")


def windows_t8152_input_plan(payload, u):
    """I0: check --windows-input against the chip, the board, the resident, the serial mode
    and the FD (both directions) before anything is uploaded.  Returns the source tuple (empty
    when off)."""
    from aura import windows as w
    try:
        sources = w.parse_windows_input(args.windows_input)
        caps2 = 0
        if sources or w.fd_input_marker(payload) is not None:
            try:
                caps2 = p.hv_windows_capabilities2()
            except ProxyRemoteError:
                caps2 = 0  # an older resident: Bad Command
        chosen = u.adt["/chosen"]
        w.validate_input_request(sources, chosen.chip_id, getattr(chosen, "board_id", -1), caps2,
                                 payload, args.windows_serial)
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    if sources:
        print(f"T8152 input: {'+'.join(sources)} (CAPS2 {caps2:#x}, FD marker "
              f"{w.fd_input_marker(payload)[0]:#x}/{w.fd_input_marker(payload)[1]})")
    return sources


def windows_t8152_input_attach(hv, sources):
    """After USB, before hv.start(): QUERY the contract, ATTACH, then (only then) write Mu's
    /chosen/aura,windows-input-v1.  A refusal stops the launch."""
def windows_t8152_input_area(top):
    """I1/I2, before any plan: with the internal keyboard (and trackpad), carve the 8 MiB MTP
    area off the window top and, with the trackpad, read and check the owner's firmware and
    build the parameter page.  Returns (new top, the area plan or None).  Nothing on the
    machine changes here."""
    from aura import windows as w
    try:
        sources = w.parse_windows_input(args.windows_input)
        w.input_attach_mask(sources, args.windows_input_fn_default)
        if args.windows_input_probe_only and not args.windows_input_probe:
            raise ValueError("--windows-input-probe-only needs --windows-input-probe SECONDS")
        if args.windows_input_probe and not 1 <= args.windows_input_probe <= 120:
            raise ValueError("--windows-input-probe takes 1 to 120 seconds")
        if not w.input_needs_area(sources):
            if args.windows_input_require_mtp or args.windows_input_tp_fw or \
                    args.windows_input_probe:
                raise ValueError("--windows-input-require-mtp, --windows-input-tp-fw and "
                                 "--windows-input-probe need --windows-input host+kbd or "
                                 "host+kbd+tp")
            return top, None
        base, new_top = w.plan_t8152_input_area(top)
        mtp = {"base": base, "size": w.INPUT_AREA_SIZE, "payload": None, "params": None}
        if "tp" in sources:
            path = args.windows_input_tp_fw or w.default_tp_firmware_path()
            if path is None:
                raise ValueError("no trackpad firmware: pass --windows-input-tp-fw or set "
                                 "J804_PRIVATE_DATA (firmware/tpmtfw-j804.bin there)")
            try:
                blob = pathlib.Path(path).read_bytes()
            except OSError as exc:
                raise ValueError(f"cannot read the trackpad firmware {path}: {exc.strerror}. "
                                 "Copy the owner's own lifted file there (never into a repo)")
            payload, iface_off = w.parse_tp_firmware(blob)
            opts = w.TpOptions(tap=args.windows_input_tp_tap == 'on',
                               natural=args.windows_input_tp_scroll == 'natural',
                               gain=args.windows_input_tp_gain,
                               accel=args.windows_input_tp_accel,
                               scroll_mm=args.windows_input_tp_scroll_mm)
            mtp["payload"] = payload
            mtp["params"] = w.input_params(payload, iface_off, opts)
            # The SHA-256 only, for the private run log; the file's bytes are never printed.
            print(f"T8152 input: trackpad firmware {len(payload)} payload bytes, SHA-256 "
                  f"{hashlib.sha256(blob).hexdigest()}; tap {args.windows_input_tp_tap}, scroll "
                  f"{args.windows_input_tp_scroll}, gain {opts.gain}/mm, accel {opts.accel}, "
                  f"{opts.scroll_mm} mm per detent")
    except ValueError as exc:
        sys.exit(f"T8152 launch refused: {exc}. The aura proxy keeps running; no guest was "
                 "started.")
    print(f"T8152 input: MTP area [{base:#x}, {base + w.INPUT_AREA_SIZE:#x}) carved off the "
          f"window top {top:#x}; guest RAM and everything else plan below {new_top:#x}")
    return new_top, mtp


def windows_t8152_input_attach(hv, sources, usb_ports=(), mtp=None):
    """After USB (and G0), before hv.start(): QUERY the contract, ATTACH, then (only then)
    write Mu's /chosen/aura,windows-input-v1.  A refusal stops the launch.  USB was attached
    first, so every refusal here detaches it again, as the GPU and hv.start() failure paths
    do: no xHCI passthrough or DART map of guest RAM outlives a launch with no guest.  With
    the internal keyboard/trackpad, the parameter page and the firmware go into the area first
    and ATTACH brings the MTP up (seconds, no link timeout)."""
    from aura import windows as w
    if not sources:
        return
    try:
        features = w.validate_input_contract(
            lambda name: p.hv_windows_input(w.INPUT_OP_QUERY, w.INPUT_Q[name]))
    except (ValueError, ProxyRemoteError) as exc:
        sys.exit(f"T8152: the resident's input device is not this launcher's contract: {exc}. "
                 "No guest was started; the carrier session is used up: reload the carrier.")
    result = p.hv_windows_input(w.INPUT_OP_ATTACH, w.input_source_mask(sources))
    if result != 0:
        sys.exit(f"T8152: the resident refused the input device: "
                 f"{w.describe_input_attach(result)} (its console says why). The carrier "
                 "session is used up: reload the carrier. No guest was started.")
        refuse(f"T8152: the resident's input device is not this launcher's contract: {exc}. "
               "No guest was started; the carrier session is used up: reload the carrier.")
    mask = w.input_attach_mask(sources, args.windows_input_fn_default)
    if mtp:
        try:
            w.check_input_sources(sources,
                                  p.hv_windows_input(w.INPUT_OP_QUERY, w.INPUT_Q["sources"]))
        except (ValueError, ProxyRemoteError) as exc:
            refuse(f"T8152 launch refused: {exc}. No guest was started; reload the carrier.")
        if mtp["payload"] is not None:
            iface.writemem(mtp["base"] + w.INPUT_AREA_FW, mtp["payload"])
            iface.writemem(mtp["base"], mtp["params"])
        print("T8152 input: ATTACH brings the MTP up (keyboard"
              f"{', trackpad' if 'tp' in sources else ''}); from here on leave only by a reboot")
        result = no_timeout(p.hv_windows_input, w.INPUT_OP_ATTACH, mask, mtp["base"], mtp["size"])
    else:
        result = p.hv_windows_input(w.INPUT_OP_ATTACH, mask)
    if result != 0:
        refuse(f"T8152: the resident refused the input device: "
               f"{w.describe_input_attach(result)} (its console says why). The carrier "
               "session is used up: reload the carrier. No guest was started.")
    # The handshake carries FEATURES as they are after ATTACH: bits 17/18 only for what is live.
    features = p.hv_windows_input(w.INPUT_OP_QUERY, w.INPUT_Q["features"])
    if mtp:
        live, down = w.input_live_sources(sources, features)
        print(f"T8152 input: live sources {'+'.join(live)}"
              f"{'; NOT up: ' + '+'.join(down) if down else ''} (FEATURES {features:#x})")
        if down:
            try:
                st = {name: p.hv_windows_input(w.INPUT_OP_STATUS, index)
                      for name, index in w.INPUT_STATUS_MTP.items()}
                print(f"T8152 input: {w.describe_mtp_status(st)}")
            except ProxyRemoteError:
                pass
        if down and args.windows_input_require_mtp:
            try:
                p.hv_windows_input(w.INPUT_OP_DETACH)
            except ProxyRemoteError:
                pass
            refuse(f"T8152: --windows-input-require-mtp: {'+'.join(down)} did not come up (the "
                   "resident's console says why). No guest was started. Leave by a reboot "
                   "(the MTP may be running and DMAing into the area).")
    try:
        hv.set_windows_input_handshake(w.input_handshake(sources, features))
    except ValueError as exc:
        p.hv_windows_input(w.INPUT_OP_DETACH)
        sys.exit(f"T8152: input attached but the Mu handshake was refused: {exc}. The resident "
                 "now refuses guest entry; reload the carrier. No guest was started.")
        # Best effort: a failed DETACH must not skip the USB detach below.
        try:
            p.hv_windows_input(w.INPUT_OP_DETACH)
        except ProxyRemoteError:
            pass
        refuse(f"T8152: input attached but the Mu handshake was refused: {exc}. The resident "
               "now refuses guest entry; reload the carrier. No guest was started.")
    print(f"T8152 input: attached; /chosen/{w.INPUT_HANDSHAKE_PROPERTY} written. Feed it from "
          "this Mac on the guest CDC port (never the proxy port)")


def windows_t8152_input_probe(sources):
    """Step A (carrier only): after ATTACH, before entry, PROBE in slices of at most 10 s: the
    carrier services the MTP and logs frames, keyboard states and pointer reports on its
    console (rate-limited) while the owner types and touches.  Prints the frame counts and the
    MTP STATUS.  True when --windows-input-probe-only asks the caller to stop the launch here
    (it closes the NVMe disk and reports its verdict); the MTP stays running: leave by a reboot."""
    from aura import windows as w
    if not args.windows_input_probe or not w.input_needs_area(sources):
        return False
    print(f"T8152 input PROBE: {args.windows_input_probe} s from now: type on the M6's keyboard "
          "and touch, move and click its trackpad (no guest is running; the carrier's console "
          "logs each frame and translation)")
    total, left_ms, live = 0, args.windows_input_probe * 1000, True
    while left_ms > 0:
        ms = min(10000, left_ms)
        got = no_timeout(p.hv_windows_input, w.INPUT_OP_PROBE, ms)
        left_ms -= ms
        if got >= 1 << 63:
            live = False
            print("T8152 input PROBE: the MTP is not live (latched or never up): stopped")
            break
        total += got
        print(f"T8152 input PROBE: {got} frame(s) in {ms} ms ({total} in all)")
    try:
        st = {name: p.hv_windows_input(w.INPUT_OP_STATUS, index)
              for name, index in w.INPUT_STATUS_MTP.items()}
        print(f"T8152 input PROBE result: {'live' if live else 'NOT live'}, {total} frame(s); "
              f"{w.describe_mtp_status(st)}")
    except ProxyRemoteError as exc:
        print(f"T8152 input PROBE: MTP status unavailable: {exc}")
    return args.windows_input_probe_only


def windows_t8152_input_status(sources):
    from aura import windows as w
    if not sources:
        return
    try:
        st = {name: p.hv_windows_input(w.INPUT_OP_STATUS, index)
              for name, index in w.INPUT_STATUS.items()}
    except ProxyRemoteError as exc:
        print(f"T8152 input: status unavailable: {exc}")
        return
    print(f"T8152 input at exit: {st['frames_ok']} host frames ok, {st['bad_crc']} bad CRC, "
          f"{st['bad_header']} bad header, {st['bad_payload']} bad payload, {st['seq_gaps']} seq "
          f"gaps, {st['watchdog']} watchdog releases; {st['reports']} reports queued, "
          f"{st['dropped']} dropped, high water {st['high_water']}; {st['mtp']} internal "
          f"(MTP keyboard/trackpad) reports; driver CTRL {st['ctrl']:#x}, LEDs "
          f"{st['leds']:#x}, {st['bad_access']} bad accesses")
    if w.input_needs_area(sources):
        try:
            mst = {name: p.hv_windows_input(w.INPUT_OP_STATUS, index)
                   for name, index in w.INPUT_STATUS_MTP.items()}
            print(f"T8152 input at exit: {w.describe_mtp_status(mst)}")
        except ProxyRemoteError as exc:
            print(f"T8152 input: MTP status unavailable: {exc}")
        print("T8152 input: the MTP keeps running and DMAing into its area: leave this session "
              "only by a reboot (P_REBOOT through the carrier), never by loading something else")


def windows_t8152_mtp_idle_check(u):
    """Every J804 launch, before anything on the machine changes: refuse while the MTP runs.
    An earlier carrier session in this power cycle that brought up the internal keyboard or
    trackpad leaves it running and DMAing into that session's area (its stream-4 entries stay),
    so no later launch may plan RAM there.  Read-only: the 13 PMGR states, and ASC_CPU_CONTROL
    only when every one is on (aura/windows.py t8152_mtp_running)."""
    from aura import windows as w
    if getattr(u.adt["/chosen"], "board_id", -1) != w.J804_BOARD_ID:
        return
    running, detail = w.t8152_mtp_running(p.read32)
    print(f"T8152: {detail}")
    if running:
        sys.exit(f"T8152 launch refused: {w.T8152_MTP_RUNNING_REFUSAL}. The aura proxy keeps "
                 "running; no guest was started.")


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
    from aura.windows import INPUT_OP_DETACH, USB_OP_DETACH, usb_spelling
    if not args.raw:
        sys.exit("T8152 Windows guests are raw Mu FDs: pass -r")
    if args.symbols or args.debug_xnu or args.volume or args.emulate_sprr or args.cpus or \
            args.strip_node or args.shell or args.script or args.command:
        sys.exit("T8152 Windows guests take only -r, -E, -a, -l and boot arguments")
    if not args.windows_guest_ram or ":" not in args.windows_guest_ram:
        sys.exit("T8152 needs --windows-guest-ram BASE:TOP (see docs/windows-t8152.md)")
    base, top = (number(v, "guest RAM") for v in args.windows_guest_ram.split(":", 1))
    window_top = top
    # I1/I2: the MTP area comes off the window top first; every plan below uses the new top.
    top, input_mtp = windows_t8152_input_area(top)
    payload = args.payload.read_bytes()
    if ahci_nvme_requested():
        ahci = windows_t8152_ahci_nvme_plan(base, top, payload)
    else:
        ahci = windows_t8152_ahci_plan(base, top, payload)
    nvme = bool(ahci and ahci.get("backend") == "nvme")

    u = ProxyUtils(p, heap_size=PROXY_HEAP_SIZE, aura_heap=AURA_HEAP_RESERVE)
    if u.adt["/chosen"].chip_id != T8152_CHIP:
        sys.exit("this Windows resident is not on T8152: use tools/boot_windows.py")
    windows_t8152_mtp_idle_check(u)
    ba = u.ba
    adt_pa = (ba.devtree - ba.virt_base + ba.phys_base) & ((1 << 64) - 1)
    occupied = [("aura resident and heaps", u.base, u.heap_top - u.base),
                ("live ADT", adt_pa, ba.devtree_size)]
    if ba.video.base and ba.video.stride and ba.video.height:
        occupied.append(("scanout", ba.video.base, ba.video.stride * ba.video.height))
    t8152_check_window(u.adt, ba, base, window_top, occupied)
    usb_ports = windows_t8152_usb_plan(payload, u)
    gpu_mode = windows_t8152_gpu_plan(u)
    input_sources = windows_t8152_input_plan(payload, u)
    print(f"T8152: guest RAM [{base:#x}, {top:#x}) passes the host check; NVRAM, QSPI and the "
          f"audio/AOP/ADMAC/i2c nodes are hidden, and the resident re-checks everything")

    hv = HV(iface, p, u, windows=True)
    hv.windows_framebuffer = "bgra-shadow"
    hv.windows_serial = args.windows_serial
    hv.windows_guest_ram = (base, ahci["plan"].store_base) if ahci else (base, top)
    hv.windows_ahci = ahci
    hv.windows_usb = usb_spelling(usb_ports)
    hv.windows_gpu = gpu_mode
    hv.windows_input = "+".join(input_sources) if input_sources else "off"
    hv.windows_serror = args.windows_serror
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
    if nvme:
        windows_t8152_ahci_nvme_attach(hv, u, ahci)
    elif ahci:
        windows_t8152_ahci_attach(hv, u, ahci)
    windows_t8152_usb_attach(hv, usb_ports, ahci)
    windows_t8152_gpu_admit(hv, gpu_mode, usb_ports, ahci)
    windows_t8152_input_attach(hv, input_sources)
    try:
        windows_t8152_usb_attach(hv, usb_ports, ahci)
        windows_t8152_gpu_admit(hv, gpu_mode, usb_ports, ahci)
        windows_t8152_input_attach(hv, input_sources, usb_ports, input_mtp)
        probe_only = windows_t8152_input_probe(input_sources)
    except BaseException:
        if nvme:
            # D5: the NVMe disk is attached; never leave it owning the controller.
            windows_t8152_ahci_nvme_detach()
        raise
    if probe_only:
        # Step A (carrier only): no guest.  Close the disk with its verdict, release the input
        # device; the MTP keeps running, so the only way out is a reboot through the carrier.
        clean = windows_t8152_ahci_nvme_close(-1, ahci, hv) if nvme else True
        try:
            p.hv_windows_input(INPUT_OP_DETACH)
        except (ProxyError, UartError, OSError):
            pass
        sys.exit("T8152: --windows-input-probe-only: no guest was started. "
                 f"{'NVMe disk CLEAN. ' if nvme and clean else ''}"
                 f"{'The NVMe disk did NOT close CLEAN: do not hard-reset. ' if not clean else ''}"
                 "The MTP stays running and DMAing into its area: leave only by a reboot through "
                 "the carrier (P_REBOOT).")
    try:
        result = hv.start()
    except (ProxyRemoteError, WindowsGuestAborted, WindowsGuestExitTimeout) as exc:
        # A failed or refused hv_start(), a session aborted on a forwarded exception (a guest
        # SError: the carrier printed its report before forwarding it), or a carrier that never
        # finished the exit.  Never a hang: every read below is bounded.
        stuck = isinstance(exc, WindowsGuestExitTimeout)
        windows_t8152_fatal_summary(hv)
        if stuck:
            print(f"\x1b[31mT8152: {exc}. The carrier may still be inside the guest exit; "
                  "USB and input are left as they are.\x1b[0m")
        if usb_ports and not stuck:
            # hv_start() tears USB down on every exit itself; DETACH is idempotent.
            try:
                p.hv_windows_usb(USB_OP_DETACH)
            except (ProxyError, UartError, OSError):
                pass
        if input_sources and not stuck:
            # A pre-entry refusal (hv_t8152_verify, hv_ahci_verify, hv_input_verify) leaves the
            # input device attached and owning the vUART RX; DETACH works until guest entry.
            try:
                p.hv_windows_input(INPUT_OP_DETACH)
            except (ProxyError, UartError, OSError):
                pass
        if nvme:
            # hv_start() closes the NVMe disk on every return itself, a failed or aborted session
            # included: report its counters and CLEAN or FAILED (with the matching exit advice),
            # retrying the close once if it is not CLEAN; UNKNOWN if the resident does not answer.
            if windows_t8152_ahci_nvme_close(-1, ahci, hv) and ahci["flags"] & 1:
                print("The NVMe disk closed CLEAN: leave the proxy through the carrier "
                      "(tools/windows_nvme_gate.py exit).")
        if stuck:
            sys.exit(f"Hypervisor exit did not finish: {exc}. Check the M6 (its console, the "
                     "camera) before anything else.")
        sys.exit(f"Hypervisor refused or failed: {exc}. The aura proxy keeps running; "
                 "its console says why. USB and input (if any) were detached; a GPU "
                 "admission has no release and lasts until the carrier is reloaded.")
    print(f"Guest exit status: {result} (1 SYSTEM_OFF, 2 SYSTEM_RESET)")
    windows_t8152_usb_status(usb_ports)
    windows_t8152_gpu_status(gpu_mode)
    windows_t8152_input_status(input_sources)
    clean = True
    if nvme:
        clean = windows_t8152_ahci_nvme_close(result, ahci, hv)
    elif ahci:
        windows_t8152_ahci_seal(result, ahci)
    if result == 2 and args.windows_reset_on_exit and not clean:
        print("SYSTEM_RESET: NOT resetting: the NVMe disk did not close CLEAN (see above). The "
              "aura proxy stays up.")
    elif result == 2 and args.windows_reset_on_exit:
        print("SYSTEM_RESET: NVMe closed CLEAN; resetting through WD1 (P_REBOOT goes through "
              "flush_and_reboot)")
        windows_system_reset(p)
    elif nvme and not clean:
        print("The aura proxy stays up. The NVMe disk is NOT closed CLEAN (see above): do not "
              "leave the proxy yet; retry p.hv_windows_ahci(6) (DETACH) until it reports CLEAN.")
    elif nvme and ahci["flags"] & 1:
        print("The aura proxy stays up. The guest wrote only its NVMe partition, closed CLEAN "
              "above; leave the proxy through the carrier (tools/windows_nvme_gate.py exit, "
              "which is P_REBOOT through flush_and_reboot).")
    else:
        print("The aura proxy stays up. Nothing was written to disk or NVRAM by the guest.")
    sys.exit(0)


_windows_caps = windows_resident_caps()
if _windows_caps and p.get_chipid() == 0x8152:
    run_windows_t8152(_windows_caps)
elif (args.windows_guest_ram or args.windows_ipi != 'poll' or args.windows_reset_on_exit or
      args.windows_gic_cpuif not in (None, 'carrier') or args.windows_wfi != 'poll' or
      args.windows_guest_compatibility != 'none' or ahci_requested() or
      args.windows_usb not in ('off', 'disabled') or
      args.windows_gpu not in ('off', 'disabled') or
      args.windows_input not in ('off', 'disabled')):
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
