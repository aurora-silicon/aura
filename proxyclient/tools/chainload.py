#!/usr/bin/env python3
# SPDX-License-Identifier: MIT AND GPL-2.0-only
import sys, pathlib
sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

import argparse, pathlib, time

parser = argparse.ArgumentParser(description='Mach-O loader for aura')
parser.add_argument('-q', '--quiet', action="store_true", help="Disable framebuffer")
parser.add_argument('-n', '--no-sepfw', action="store_true", help="Do not preserve SEPFW")
parser.add_argument('-c', '--call', action="store_true", help="Use call mode")
parser.add_argument('-r', '--raw', action="store_true", help="Image is raw")
parser.add_argument('-E', '--entry-point', action="store", type=int, help="Entry point for the raw image", default=0x800)
parser.add_argument('-x', '--xnu', action="store_true", help="Set up for chainloading XNU")
parser.add_argument('--vector-only', action="store_true", help="Return after vector reply without probing the next image")
parser.add_argument('--capture', type=pathlib.Path, help="Write raw post-vector console bytes to this file")
parser.add_argument('--capture-seconds', type=float, default=0, help="Seconds of raw console capture after vector")
parser.add_argument('--relocate', action='store_true', help='Allocate a new raw-image span while retaining the live stage-1 heap')
parser.add_argument('--reclaim-stage1-heap', action='store_true', help='After raw image replacement, reclaim the discarded Stage 1 heap; preserve native boot data and firmware')
parser.add_argument('--capture-until', help="Stop the capture early once the console shows this regex")
parser.add_argument('--stub', type=pathlib.Path, help="Prebuilt copy stub (8-byte LE entry, then code) for a host without a toolchain")
parser.add_argument('payload', type=pathlib.Path)
parser.add_argument('boot_args', default=[], nargs="*")
args = parser.parse_args()
if args.reclaim_stage1_heap and (not args.raw or args.call or args.relocate or args.xnu):
    parser.error('--reclaim-stage1-heap requires in-place raw replacement, not call, relocation or XNU mode')
if args.vector_only and args.call:
    parser.error('--vector-only cannot be combined with --call')
if args.capture and not args.vector_only:
    parser.error('--capture requires --vector-only')

from aura.setup import *
from aura.tgtypes import BootArgs_r1, BootArgs_r2, BootArgs_r3
from aura.macho import MachO
from aura import asm
from aura.bootmem import BOOT_BLOBS, find_boot_blobs, copy_boot_blobs
from construct import Array, Int64ul

new_base = u.base

if args.raw:
    image = args.payload.read_bytes()
    image += b"\x00\x00\x00\x00"
    entry = new_base + args.entry_point
else:
    macho = MachO(args.payload.read_bytes())
    image = macho.prepare_image()
    image += b"\x00\x00\x00\x00"
    entry = macho.entry
    entry -= macho.vmin
    entry += new_base

if args.quiet:
    p.iodev_set_usage(IODEV.FB, 0)

if args.no_sepfw:
    blob_names = tuple(name for name in BOOT_BLOBS if name != "SEPFW")
else:
    blob_names = BOOT_BLOBS

image_only_size = align(len(image))
# The boot blobs and the boot args form the firmware block, packed after the image or parked.
blobs, bootargs_off = find_boot_blobs(u.adt["chosen"]["memory-map"]._properties, blob_names, 0)
bootargs_size = 0x4000
fw_size = bootargs_off + bootargs_size
if args.relocate:
    if not args.raw or args.xnu:
        raise ValueError("Relocation requires a raw aura image")
    new_base = p.memalign(0x4000, image_only_size + fw_size)
    if not new_base:
        raise MemoryError("No space for relocated image")
    entry = new_base + args.entry_point
packed_end = new_base + image_only_size + fw_size

if not (u.ba.phys_base <= new_base < u.ba.phys_base + u.ba.mem_size):
    raise ValueError("Image base is outside inherited RAM")
ram_end = u.ba.phys_base + u.ba.mem_size
if ram_end > (1 << 64) or packed_end > ram_end or u.ba.top_of_kernel_data >= ram_end:
    raise ValueError("Packed chainload span is outside inherited RAM")

ro_start = ro_end = 0
if p.get_chipid() == 0x8140:
    ro_start = u.mrs("CTRR_C_LWR_EL2")
    upper = u.mrs("CTRR_C_UPR_EL2")
    ro_end = upper + 0x1000
    if ((ro_start | upper) & 0xfff or ro_end - ro_start != 0xc000 or
            not (u.ba.phys_base <= ro_start < ro_end <= ram_end) or
            not (u.ba.phys_base <= u.ba.top_of_kernel_data < ram_end)):
        raise ValueError("Invalid T8140 firmware RO range")

live_heap = p.heapblock_alloc(0)
inherited_top = u.ba.top_of_kernel_data
image_end = new_base + image_only_size
if ro_end and image_end > ro_start and new_base < ro_end:
    raise ValueError("Image itself overlaps firmware RO memory")
if not args.relocate and live_heap > inherited_top and image_end > inherited_top and new_base < live_heap:
    raise ValueError("Image overlaps live Stage 1 heap")

park_fw = not args.relocate and ((ro_end and packed_end > ro_start and image_end < ro_end) or
           (live_heap > inherited_top and packed_end > inherited_top and image_end < live_heap))
fw_dest = new_base + image_only_size
if park_fw:
    print("Packed span would overlap protected/live memory; parking firmware block")
    fw_dest = p.memalign(0x4000, fw_size)
    if not fw_dest or fw_dest + fw_size > ram_end:
        raise ValueError("No safe location for firmware block")
    if fw_dest < live_heap or (ro_end and fw_dest < ro_end and fw_dest + fw_size > ro_start):
        raise ValueError("Parked firmware block overlaps protected/live memory")

copy_size = image_only_size if park_fw else image_only_size + fw_size
print(f"Copy span: 0x{copy_size:x} bytes; firmware block: 0x{fw_dest:x}")
image_addr = p.memalign(0x4000, copy_size + 0x4000)
if not image_addr:
    raise MemoryError("No aligned staging block")
fw_stage = fw_dest if park_fw else image_addr + image_only_size

copy_boot_blobs(p, [(name, src, size, fw_stage + off) for name, src, size, off in blobs])

print(f"Loading kernel image (0x{len(image):x} bytes)...")
u.compressed_writemem(image_addr, image, True)
p.dc_cvac(image_addr, len(image))

print("Adjusting addresses in ADT...")
for name, src, size, off in blobs:
    mm = u.adt["chosen"]["memory-map"]
    mm._properties[name] = (fw_dest + off, size)
    mm._types[name] = (Array(2, Int64ul), False)
u.adt["chosen"]["memory-map"].BootArgs = (fw_dest + bootargs_off, bootargs_size)

if args.xnu:
    def remove_oslog(node):
        names = node.segment_names.split(";")
        try:
            idx = names.index("__OS_LOG")
        except ValueError:
            return
        print(f"Removing __OS_LOG from {node.name}")
        names = names[:idx] + names[idx + 1:]
        node.segment_names = ";".join(names)
        node.segment_ranges = node.segment_ranges[:idx * 32] + node.segment_ranges[32 + idx * 32: ]

    for node in u.adt["/arm-io"]:
        if hasattr(node, "segment_names"):
            remove_oslog(node)

        for nub in node:
            if hasattr(nub, "segment_names"):
                remove_oslog(nub)

rvbar = entry & ~0xfff
if rvbar != u.base:
    print("Setting secondary CPU RVBARs...")

    for cpu in u.adt["cpus"]:
        if cpu.state == "running":
            continue
        if u.adt["/chosen"].chip_id == 0x8142:
            # Host writes to a cold T8142 secondary's RVBAR can wedge CPU0.
            # Leave the cold cores and their reset vectors to the next stage.
            print(f"  {cpu.name}: preserving T8142 secondary RVBAR")
            continue
        addr, size = cpu.cpu_impl_reg
        if p.read64(addr) & 1:
            print(f"  {cpu.name}: RVBAR locked, leaving it unchanged")
            continue
        print(f"  {cpu.name}: [0x{addr:x}] = 0x{rvbar:x}")
        p.write64(addr, rvbar)

u.push_adt()

print("Setting up bootargs...")
tba = u.ba.copy()

retained_top = max(image_end, fw_dest + fw_size, inherited_top, ro_end)
if args.reclaim_stage1_heap:
    # After an in-place raw replacement Stage 1's heap is dead; the image, the firmware, the
    # inherited boot data and the ADT stay below the new top.
    retained_top = max(retained_top,
                       u.ba.devtree - u.ba.virt_base + u.ba.phys_base + u.ba.devtree_size)
tba.top_of_kernel_data = max(retained_top, 0 if args.reclaim_stage1_heap else live_heap)
if args.reclaim_stage1_heap:
    print(f"Reclaiming Stage 1 temporary heap after vector: inherited top 0x{inherited_top:x}, "
          f"new top 0x{tba.top_of_kernel_data:x}, old heap high-water 0x{live_heap:x}")

if len(args.boot_args) > 0:
    boot_args = " ".join(args.boot_args)
    if "-v" in boot_args.split():
        tba.video.display = 0
    else:
        tba.video.display = 1
    print(f"Setting boot arguments to {boot_args!r}")
    tba.cmdline = boot_args

if args.xnu:
    # Fix virt_base, since we often install aura with it set to 0 which xnu does not like
    tba.virt_base = 0xfffffe0010000000 + (tba.phys_base & (32 * 1024 * 1024 - 1))
    tba.devtree = u.ba.devtree - u.ba.virt_base + tba.virt_base

if tba.revision <= 1:
    iface.writemem(fw_stage + bootargs_off, BootArgs_r1.build(tba))
elif tba.revision == 2:
    iface.writemem(fw_stage + bootargs_off, BootArgs_r2.build(tba))
elif tba.revision == 3:
    iface.writemem(fw_stage + bootargs_off, BootArgs_r3.build(tba))

p.dc_cvac(fw_stage, fw_size)

print(f"Copying stub...")

class PrebuiltStub:
    """
    A stub assembled elsewhere, with the entry address it was built for in its
    first 8 bytes. The code is position independent and takes the entry from a
    literal, so that literal is rewritten with this boot's entry.
    """
    def __init__(self, path, addr, entry):
        blob = path.read_bytes()
        baked = blob[:8]
        code = blob[8:]
        if code.count(baked) != 1:
            raise ValueError(f"{path}: expected exactly one copy of the entry literal in the stub")
        self.addr = addr
        self.data = code.replace(baked, entry.to_bytes(8, "little"))
        self.len = len(self.data)
        print(f"Prebuilt stub: entry literal set to 0x{entry:x}")

if args.stub:
    stub = PrebuiltStub(args.stub, image_addr + copy_size, entry)
else:
    stub = asm.ARMAsm(f"""
        mov x7, x2
        mov x8, x3
1:
        ldp x4, x5, [x1], #16
        stp x4, x5, [x2]
        add x2, x2, #16
        subs x3, x3, #16
        b.ne 1b

        mov x2, x7
        mov x3, x8
2:
        dc cvac, x2
        add x2, x2, #64
        subs x3, x3, #64
        b.ne 2b
        dsb sy

        mov x2, x7
        mov x3, x8
3:
        ic ivau, x2
        add x2, x2, #64
        subs x3, x3, #64
        b.ne 3b
        dsb sy
        isb
        ldr x1, ={entry}
        br x1
""", image_addr + copy_size)

iface.writemem(stub.addr, stub.data)
p.dc_cvac(stub.addr, stub.len)
p.ic_ivau(stub.addr, stub.len)

print(f"Entry point: 0x{entry:x}")

if args.xnu and p.display_is_external():
    if p.display_start_dcp() >= 0:
        p.display_shutdown(0)

if args.call:
    print(f"Shutting down MMU...")
    try:
        p.mmu_shutdown()
    except ProxyCommandError:
        pass
    print(f"Jumping to stub at 0x{stub.addr:x}")
    p.call(stub.addr, fw_dest + bootargs_off, image_addr, new_base, copy_size, reboot=True)
elif args.vector_only:
    print(f"Vectoring to 0x{stub.addr:x}; no next-stage proxy request will be sent")
    p.request(p.P_VECTOR, stub.addr, fw_dest + bootargs_off, image_addr, new_base, copy_size)
    if args.capture:
        from aura.console import capture
        capture(iface.dev, args.capture, args.capture_seconds, args.capture_until)
    sys.exit(0)
else:
    print(f"Reloading into stub at 0x{stub.addr:x}")
    p.reload(stub.addr, fw_dest + bootargs_off, image_addr, new_base, copy_size)

iface.nop()
print("Proxy is alive again")
