#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""D5 gates R0 and W0 (windows-m6/D5-NVME-DESIGN.md section 13), no guest.

  r0      attach the Windows partition READ-ONLY twice (NVMe wake, Identify, outer and inner GPT,
          fence), print the status, detach (controller shutdown) after each: the second RTKit
          wake in one boot and the GPT match on the real disk.  Writes nothing.
  w0      ONLY on the empty partition, BEFORE the install: write seeded patterns at the
          partition's start, middle and end (1, 8 and 64 blocks), flush, verify, detach.  Then
          leave through the carrier and check the same blocks from Linux (see `expect`).
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
parser.add_argument('gate', choices=('r0', 'w0', 'expect'))
parser.add_argument('--partition', required=True, metavar='GUID|NAME')
parser.add_argument('--config', type=pathlib.Path, default=None,
                    help='default $J804_PRIVATE_DATA/d5/windows-partition.env')
parser.add_argument('--seed', type=lambda v: int(v, 0), default=0xD5)
parser.add_argument('--block', type=int, default=0, help='expect: partition-relative block')
parser.add_argument('--count', type=int, default=1, help='expect: blocks')
parser.add_argument('--partition-is-empty', action='store_true',
                    help='w0: required; confirms the install has not written the image yet')
args = parser.parse_args()

from aura import windows as w

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

if args.gate == 'w0' and not args.partition_is_empty:
    sys.exit("w0 writes test patterns into the partition: only before the install, on an "
             "empty partition; pass --partition-is-empty to confirm")

from aura.proxy import *
from aura.proxyutils import ProxyUtils
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
        sys.exit("the resident refused the attach (its console says why); nothing was written")
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
    print("R0 passed on the carrier side: two wakes, GPT matched, two clean closes. Leave "
          "through the carrier (P_REBOOT / leave2.sh); check Linux boots normally.")
    sys.exit(0)

attach(w.NVME_GATE | w.NVME_SCRATCH | w.NVME_RW)
spots = [(0, 1), (part.blocks // 2, 8), (part.blocks - 64, 64)]
try:
    for block, count in spots:
        if op(w.AHCI_OP_NVME_TEST, block, count, args.seed, w.NVME_TEST["write"]) != 0:
            sys.exit(f"W0 write at block {block} (+{count}) failed (lost completion?)")
    if op(w.AHCI_OP_NVME_TEST, 0, 0, 0, w.NVME_TEST["flush"]) != 0:
        sys.exit("W0 flush failed")
    for block, count in spots:
        bad = op(w.AHCI_OP_NVME_TEST, block, count, args.seed, w.NVME_TEST["verify"])
        print(f"W0 verify block {block} (+{count}): {bad} mismatching words")
finally:
    print(f"W0 status: {status()}")
    detach()
print("W0 carrier side done. Reset through the carrier (p.reboot()), then from Linux compare:")
for block, count in spots:
    print(f"  windows_nvme_gate.py expect --partition '{args.partition}' --seed {args.seed:#x} "
          f"--block {block} --count {count} | cmp - <(ssh M6 sudo dd "
          f"if=/dev/disk/by-partuuid/<GUID> bs=4096 skip={block} count={count} iflag=direct)")
