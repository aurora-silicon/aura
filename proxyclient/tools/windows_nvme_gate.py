#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""D5 gates R0 and W0 (windows-m6/D5-NVME-DESIGN.md section 13), no guest.

  r0      attach the Windows partition READ-ONLY twice (NVMe wake, Identify, outer and inner GPT,
          fence), print the status, detach (controller shutdown) after each: the second RTKit
          wake in one boot and the GPT match on the real disk.  Writes nothing.  Then `exit`:
          R0 also proves that the carrier's own reset (P_REBOOT) brings Linux back.
  w0      the write path: seeded patterns (1, 8 and 64 blocks), flush, verify, detach (CC.SHN).
          With the image installed (the default) the blocks lie PAST the inner disk's end (its
          backup GPT header), in the tail that stays free until `sgdisk -e` or Windows extends C:;
          the resident refuses any test write inside the image.  With --partition-is-empty (an
          empty partition, before an install) at the partition's start, middle and end; the
          resident refuses that attach unless the partition's first 4 KiB are zero.  Any verify
          mismatch fails the gate.  Then `exit` and check the same blocks from Linux (`expect`).
  exit    leave the proxy through the carrier: P_REBOOT, which closes the disk with the
          controller shutdown (flush_and_reboot) before the watchdog reset.  Refused unless the
          disk state is DETACHED or CLEAN.  --force after a FAILED close that DETACH retries did
          not fix: P_REBOOT "FORCE" (one more shutdown attempt, then the reset anyway; restore
          or chkdsk the partition afterwards).  A Stage 1 exit that writes the watchdog
          registers directly through the proxy is NOT this: it is a hard reset that skips the
          shutdown.
  expect  host only: the bytes W0 wrote at partition block B (count N, seed S), to compare
          with `dd if=/dev/disk/by-partuuid/... bs=4096 skip=B count=N iflag=direct` on Linux.

The partition is named by GUID or GPT name from the private config, never by number.
"""
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse
import struct

parser = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('gate', choices=('r0', 'w0', 'expect', 'exit'))
parser.add_argument('--partition', metavar='GUID|NAME', help='r0, w0, expect: required')
parser.add_argument('--config', type=pathlib.Path, default=None,
                    help='default $J804_PRIVATE_DATA/d5/windows-partition.env')
parser.add_argument('--seed', type=lambda v: int(v, 0), default=0xD5)
parser.add_argument('--block', type=int, default=0, help='expect: partition-relative block')
parser.add_argument('--count', type=int, default=1, help='expect: blocks')
parser.add_argument('--partition-is-empty', action='store_true',
                    help='w0: the partition holds no image yet (scratch attach, whole partition)')
parser.add_argument('--force', action='store_true',
                    help='exit: after a FAILED close, reset anyway (P_REBOOT "FORCE")')
args = parser.parse_args()

from aura import windows as w

if args.gate == 'exit':
    from aura.proxy import *
    from aura.proxyutils import bootstrap_port
    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    try:
        w.validate_nvme_contract(p.hv_windows_capabilities2())
    except (ValueError, ProxyRemoteError, ProxyCommandError) as exc:
        sys.exit(f"refused: not a D5 carrier ({exc}); its P_REBOOT closes no NVMe disk: use "
                 "the Stage 1 exit (a direct watchdog write)")
    state = p.hv_windows_ahci(w.AHCI_OP_NVME_STATUS, w.NVME_STATUS["state"])
    name = w.describe_nvme_state(state)
    if name in ("DETACHED", "CLEAN"):
        print(f"NVMe disk {name}: P_REBOOT through the carrier (flush_and_reboot, then WD1)")
        p.reboot()
    elif name == "FAILED" and args.force:
        print("NVMe disk FAILED: P_REBOOT FORCE: one more shutdown attempt, then the reset even "
              "without it. Restore the Windows partition from the image or chkdsk it (WinPE) "
              "before the next read-write session.")
        p.request(p.P_REBOOT, w.REBOOT_FORCE, no_reply=True)
    else:
        sys.exit(f"refused: the NVMe disk is {name}. RUNNING: end the guest first; FAILED: "
                 "retry p.hv_windows_ahci(6) (DETACH) until CLEAN, or --force knowing writes "
                 "may be lost.")
    print("Reset requested: Linux should answer on SSH in about a minute. If the proxy still "
          "answers after 60 s, the carrier's reset does not work on this machine: report it; "
          "only then, with CLEAN or DETACHED printed above, a direct watchdog write is safe.")
    sys.exit(0)

if not args.partition:
    sys.exit("--partition GUID|NAME is required for r0, w0 and expect")

config = args.config or w.default_nvme_config_path()
if config is None:
    sys.exit("no partition config: pass --config or set J804_PRIVATE_DATA")
try:
    part = w.resolve_nvme_partition(w.load_nvme_config(config), args.partition)
except ValueError as exc:
    sys.exit(f"refused: {exc}")

if args.gate == 'expect':
    if not 0 <= args.block or args.block + args.count > part.blocks:
        sys.exit("the blocks are outside the partition")
    out = sys.stdout.buffer
    for b in range(args.block, args.block + args.count):
        out.write(b"".join(struct.pack("<Q", w.nvme_test_pattern(args.seed, b, k))
                           for k in range(512)))
    sys.exit(0)

from aura.proxy import *
from aura.proxyutils import ProxyUtils, bootstrap_port
from aura.utils import *

iface = UartInterface()
p = AuraProxy(iface, debug=False)
bootstrap_port(iface, p)
try:
    w.validate_nvme_contract(p.hv_windows_capabilities2())
except (ValueError, ProxyRemoteError) as exc:
    sys.exit(f"refused: {exc}")
u = ProxyUtils(p, heap_size=16 << 20)


def op(*a):
    timeout = iface.dev.timeout
    iface.dev.timeout = None
    try:
        return p.hv_windows_ahci(*a)
    finally:
        iface.dev.timeout = timeout


def status():
    return {k: op(w.AHCI_OP_NVME_STATUS, i) for k, i in w.NVME_STATUS.items()}


def attach(flags):
    blob = w.nvme_request(part, flags)
    addr = u.malloc(len(blob))
    iface.writemem(addr, blob)
    r = op(w.AHCI_OP_ATTACH_NVME, addr)
    u.free(addr)
    if r != 0:
        why = (" (with --partition-is-empty: the partition's first 4 KiB are not zero, so it "
               "holds an image; run w0 without it)" if flags & w.NVME_SCRATCH else "")
        sys.exit(f"the resident refused the attach (its console says why){why}; nothing was "
                 "written")
    st = status()
    if (st["first"], st["last"]) != (part.first, part.last):
        op(w.AHCI_OP_DETACH)
        sys.exit(f"attached {st['first']}..{st['last']}, not the configured range; detached")
    print(f"attached LBAs [{st['first']}, {st['last']}], inner disk {st['inner_bytes'] >> 20} MiB")


def detach():
    r = op(w.AHCI_OP_DETACH)
    state = w.describe_nvme_state(op(w.AHCI_OP_NVME_STATUS, 0))
    print(f"detached: {state}")
    if state != "CLEAN":
        sys.exit("NOT CLEAN: do not hard reset; retry this tool's detach from the proxy shell "
                 "(p.hv_windows_ahci(6))")
    return r


if args.gate == 'r0':
    for attempt in (1, 2):
        attach(w.NVME_GATE)
        print(f"R0 attach {attempt}: {status()}")
        detach()
    print("R0 passed on the carrier side: two wakes, GPT matched, two clean closes. Now "
          "`windows_nvme_gate.py exit` (P_REBOOT through the carrier) and check that Linux comes "
          "back: that is R0's last step, the reset path every later session relies on.")
    sys.exit(0)

if args.partition_is_empty:
    attach(w.NVME_GATE | w.NVME_SCRATCH | w.NVME_RW)
    spots = [(0, 1), (part.blocks // 2, 8), (part.blocks - 64, 64)]
else:
    attach(w.NVME_GATE | w.NVME_RW)
    # Rounded up, as the resident does: a block holding any byte of the image is not tail.
    inner = -(-op(w.AHCI_OP_NVME_STATUS, w.NVME_STATUS["inner_bytes"]) // w.NVME_LBA_BYTES)
    if part.blocks - inner < 128:
        detach()
        sys.exit("no free tail past the inner disk (backup GPT already at the partition end, or "
                 "C: extended): W0 has no safe blocks on this partition")
    spots = [(inner, 1), ((inner + part.blocks) // 2, 8), (part.blocks - 64, 64)]
    print(f"W0 in the free tail: partition blocks [{inner}, {part.blocks}) (the image ends at "
          f"block {inner})")
failed = []
try:
    for block, count in spots:
        if op(w.AHCI_OP_NVME_TEST, block, count, args.seed, w.NVME_TEST["write"]) != 0:
            sys.exit(f"W0 write at block {block} (+{count}) failed (lost completion?)")
    if op(w.AHCI_OP_NVME_TEST, 0, 0, 0, w.NVME_TEST["flush"]) != 0:
        sys.exit("W0 flush failed")
    for block, count in spots:
        bad = op(w.AHCI_OP_NVME_TEST, block, count, args.seed, w.NVME_TEST["verify"])
        print(f"W0 verify block {block} (+{count}): {bad} mismatching words")
        if bad != 0:
            failed.append((block, count, bad))
finally:
    print(f"W0 status: {status()}")
    detach()
if failed:
    sys.exit(f"W0 FAILED: read-back mismatches {failed} (multi-block writes or reads are not "
             "trustworthy on this ANS); no read-write guest session. Still `exit`, then compare "
             "from Linux to see which side is wrong.")
print("W0 carrier side done. `windows_nvme_gate.py exit` (P_REBOOT through the carrier), then "
      "from Linux compare (also one block at a time, count=1):")
for block, count in spots:
    print(f"  windows_nvme_gate.py expect --partition '{args.partition}' --seed {args.seed:#x} "
          f"--block {block} --count {count} | cmp - <(ssh M6 sudo dd "
          f"if=/dev/disk/by-partuuid/<GUID> bs=4096 skip={block} count={count} iflag=direct)")
