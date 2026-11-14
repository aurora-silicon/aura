#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Wrap a raw Aura image in a Mach-O boot object without __TEXT_BOOT_EXEC.

`kmutil configure-boot` without `--raw` records the boot object's segment
layout in its boot policy, including the size of __TEXT_BOOT_EXEC (kcbz).
macOS 27.2 iBoot loads and enters SPTM only when kcbz is nonzero; with no
such segment it enters the object directly, as it enters a raw boot object.

The raw image is carried byte for byte and file offsets equal virtual
offsets, so iBoot places it exactly as in raw mode, only after two header
pages. Entry is the raw image's own entry point (`--entry-point 2048` in raw
mode). A Stage 1 configuration block filled into the raw image before
wrapping, or found by its magic afterwards, is unchanged.

`--execute-window` also pads __TEXT_EXEC for the code-execute window that
M4-generation and later chips (T8132, T6040 and up) need to run an OS, the
same padding aurora-installer's Recovery and `aura-execute-window` apply. It
takes a raw image or an existing boot object. The padding is written as a
hole, so the file is sparse; enroll it with `kmutil configure-boot --compress`.
"""

import argparse
import hashlib
import os
import plistlib
import struct
import tempfile
from pathlib import Path


PAGE = 0x4000
VA_BASE = 0xFFFFFE0007004000
HEADER_SIZE = 0x8000          # __TEXT: this fileset header
KERNEL_HEADER_SIZE = 0x8000   # __PRELINK_TEXT: the primary member's header
IMAGE_OFFSET = HEADER_SIZE + KERNEL_HEADER_SIZE
ENTRY_OFFSET = 0x800          # start.S: the vector table, then _start
LOAD_HEADER_OFFSET = 0x808
LOAD_HEADER_MAGIC = b"AURALOAD"

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
CPU_SUBTYPE_ARM64E_KERNEL = 0xC0000002
MH_EXECUTE = 2
MH_FILESET = 12
MH_FILESET_MEMBER_FLAGS = 0x80200001  # NOUNDEFS | PIE | DYLIB_IN_CACHE

LC_SYMTAB = 0x2
LC_UNIXTHREAD = 0x5
LC_DYSYMTAB = 0xB
LC_SEGMENT_64 = 0x19
LC_UUID = 0x1B
LC_BUILD_VERSION = 0x32
LC_DYLD_CHAINED_FIXUPS = 0x80000034
LC_FILESET_ENTRY = 0x80000035

PROT_R, PROT_W, PROT_X = 1, 2, 4
S_ATTR_INSTRUCTIONS = 0x80000400  # S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS
ARM_THREAD_STATE64 = 6
ARM_THREAD_STATE64_COUNT = 0x44
PRIMARY_MEMBER = b"com.apple.kernel"  # kmutil requires this fileset member


def check_raw_image(raw):
    """Raise unless this is a raw Aura image (entered at its 0x800 offset)."""
    if raw[:4] == struct.pack("<I", MH_MAGIC_64):
        raise ValueError("input is already a Mach-O image")
    if raw[LOAD_HEADER_OFFSET:LOAD_HEADER_OFFSET + 8] != LOAD_HEADER_MAGIC:
        raise ValueError("raw image lacks Aura's load header after its entry point")


def segment(name, vmaddr, size, prot, sections=()):
    """A fully file-backed segment whose file offset equals its virtual offset."""
    command = struct.pack("<II16sQQQQIIII", LC_SEGMENT_64, 72 + 80 * len(sections),
                          name.encode(), vmaddr, size, vmaddr - VA_BASE, size,
                          prot, prot, len(sections), 0)
    for section, address, length, flags in sections:
        command += struct.pack("<16s16sQQIIIIIIII", section.encode(), name.encode(),
                               address, length, address - VA_BASE, 0, 0, 0, flags, 0, 0, 0)
    return command


def thread(entry, vectors):
    registers = [0] * 34
    # kmutil 27.2 records x16 as kcev; the stock BootKC points it into __TEXT_EXEC.
    registers[16] = vectors
    registers[32] = entry    # pc
    return struct.pack("<4I34Q", LC_UNIXTHREAD, 16 + 34 * 8, ARM_THREAD_STATE64,
                       ARM_THREAD_STATE64_COUNT, *registers)


def header(filetype, flags, commands):
    payload = b"".join(commands)
    return struct.pack("<8I", MH_MAGIC_64, CPU_TYPE_ARM64, CPU_SUBTYPE_ARM64E_KERNEL,
                       filetype, len(commands), len(payload), flags, 0) + payload


def chained_fixups(segment_count):
    """A valid fixup table with no fixups: Aura relocates itself at entry."""
    starts = struct.pack(f"<I{segment_count}I", segment_count, *([0] * segment_count))
    starts_offset = 28
    imports_offset = starts_offset + len(starts)
    return struct.pack("<7I", 0, starts_offset, imports_offset, imports_offset, 0, 1, 0) + starts


def wrap(raw):
    check_raw_image(raw)
    # Zero padding keeps every segment page-sized; it lies after the image's end.
    size = (len(raw) + PAGE - 1) & ~(PAGE - 1)
    identity = hashlib.sha256(raw).digest()[:16]

    text = VA_BASE
    kernel = VA_BASE + HEADER_SIZE
    image = VA_BASE + IMAGE_OFFSET
    info = image + size
    data = info + PAGE
    linkedit = data + PAGE
    end = linkedit + PAGE
    entry = image + ENTRY_OFFSET

    prelink = plistlib.dumps({
        "KMBuildVersion": "Aura",
        "ManagerVersion": 1,
        "OSPrelinkInfoContainsUUIDs": True,
        "_PrelinkInfoDictionary": [],
        "_PrelinkKCID": identity,
    }, fmt=plistlib.FMT_XML, sort_keys=True)
    if len(prelink) > PAGE:
        raise ValueError("prelink information does not fit its page")

    segments = [
        segment("__TEXT", text, HEADER_SIZE, PROT_R),
        segment("__PRELINK_TEXT", kernel, KERNEL_HEADER_SIZE, PROT_R,
                [("__text", kernel, KERNEL_HEADER_SIZE, S_ATTR_INSTRUCTIONS)]),
        segment("__TEXT_EXEC", image, size, PROT_R | PROT_X),
        segment("__PRELINK_INFO", info, PAGE, PROT_R | PROT_W,
                [("__info", info, len(prelink), 0)]),
        segment("__DATA", data, PAGE, PROT_R | PROT_W),
        segment("__LINKEDIT", linkedit, PAGE, PROT_R),
    ]
    fixups = chained_fixups(len(segments))
    member_size = (32 + len(PRIMARY_MEMBER) + 1 + 7) & ~7
    member = struct.pack("<IIQQII", LC_FILESET_ENTRY, member_size, kernel,
                         kernel - VA_BASE, 32, 0) + PRIMARY_MEMBER.ljust(member_size - 32, b"\0")
    common = [
        struct.pack("<II16s", LC_UUID, 24, identity),
        struct.pack("<6I", LC_BUILD_VERSION, 24, 0, 0, 0, 0),
        thread(entry, image),
    ]
    outer = header(MH_FILESET, 0, common + [
        struct.pack("<4I", LC_DYLD_CHAINED_FIXUPS, 16, linkedit - VA_BASE, len(fixups)),
        *segments, member,
    ])
    primary = header(MH_EXECUTE, MH_FILESET_MEMBER_FLAGS, [
        segment("__TEXT", kernel, KERNEL_HEADER_SIZE, PROT_R),
        segments[2], segments[4], segments[5],
        struct.pack("<6I", LC_SYMTAB, 24, 0, 0, 0, 0),
        struct.pack("<20I", LC_DYSYMTAB, 80, *([0] * 18)),
        *common,
    ])
    if len(outer) > HEADER_SIZE or len(primary) > KERNEL_HEADER_SIZE:
        raise ValueError("Mach-O headers do not fit their pages")

    result = bytearray(end - VA_BASE)
    result[:len(outer)] = outer
    result[HEADER_SIZE:HEADER_SIZE + len(primary)] = primary
    result[IMAGE_OFFSET:IMAGE_OFFSET + len(raw)] = raw
    result[info - VA_BASE:info - VA_BASE + len(prelink)] = prelink
    result[linkedit - VA_BASE:linkedit - VA_BASE + len(fixups)] = fixups
    return bytes(result)


# --- the code-execute window (aurora-installer MachOBootObject.expandExecuteWindow) ---------------

# CONFIG_ARM64_CTXR_EXECMEM confines kernel code to this region; an arm64 kernel and the region start
# on a 2 MiB boundary that iBoot picks, so the padding holds the region at any alignment. The first
# 16 MiB of __TEXT_EXEC stay for aura: Stage 1, then Stage 2 copied over it, with .bss and stacks.
WINDOW_SIZE = 128 << 20
WINDOW_ALIGNMENT = 2 << 20
AURA_SLOT = 16 << 20
REQUIRED_SEGMENTS = ("__TEXT", "__TEXT_EXEC", "__PRELINK_INFO", "__LINKEDIT")


def execute_window_code_size(window=WINDOW_SIZE, slot=AURA_SLOT):
    """The size __TEXT_EXEC needs to hold aura and a `window`-byte aligned region."""
    return slot + (WINDOW_ALIGNMENT - PAGE) + window


def _u32(blob, offset):
    if offset < 0 or offset + 4 > len(blob):
        raise ValueError("truncated")
    return struct.unpack_from("<I", blob, offset)[0]


def _u64(blob, offset):
    if offset < 0 or offset + 8 > len(blob):
        raise ValueError("truncated")
    return struct.unpack_from("<Q", blob, offset)[0]


def layout(blob, file_size=None):
    """Check a boot object as the installer does; return ({segment: (vmaddr, vmsize, fileoff,
    filesize)}, entry). `file_size` is the whole file's size when `blob` is only its start."""
    total = len(blob) if file_size is None else file_size
    if _u32(blob, 0) != MH_MAGIC_64 or _u32(blob, 4) != CPU_TYPE_ARM64:
        raise ValueError("not a 64-bit Arm Mach-O image")
    if _u32(blob, 12) != MH_FILESET:
        raise ValueError("not a fileset boot object")
    count, size = _u32(blob, 16), _u32(blob, 20)
    if 32 + size > len(blob):
        raise ValueError("load commands are truncated")
    segments, entry, offset = {}, None, 32
    for _ in range(count):
        command, length = _u32(blob, offset), _u32(blob, offset + 4)
        if length < 8 or length % 8 or offset + length > 32 + size:
            raise ValueError("malformed load command")
        if command == LC_SEGMENT_64:
            if length < 72:
                raise ValueError("malformed segment")
            name = blob[offset + 8:offset + 24].split(b"\0")[0].decode(errors="replace")
            if name in segments:
                raise ValueError(f"duplicate segment {name}")
            segments[name] = struct.unpack_from("<4Q", blob, offset + 24)
        elif command == LC_UNIXTHREAD:
            if length < 16 + 34 * 8 or _u32(blob, offset + 8) != ARM_THREAD_STATE64:
                raise ValueError("malformed entry thread")
            entry = _u64(blob, offset + 16 + 32 * 8)
        offset += length
    if offset != 32 + size:
        raise ValueError("load command sizes disagree")
    boot = segments.get("__TEXT_BOOT_EXEC")
    if boot and (boot[1] or boot[3]):
        raise ValueError("it has __TEXT_BOOT_EXEC, so iBoot would start SPTM")
    for name in REQUIRED_SEGMENTS:
        if name not in segments:
            raise ValueError(f"it has no {name} segment")
    base = segments["__TEXT"][0]
    if segments["__TEXT"][2] != 0:
        raise ValueError("__TEXT is not at offset 0")
    for name, (vmaddr, vmsize, fileoff, filesize) in segments.items():
        if vmsize and not (vmaddr >= base and fileoff == vmaddr - base and filesize == vmsize
                           and fileoff <= total and filesize <= total - fileoff):
            raise ValueError(f"{name} is not file-backed at its virtual offset")
    code = segments["__TEXT_EXEC"]
    if entry is None or not code[0] <= entry < code[0] + code[1]:
        raise ValueError("its entry point is outside __TEXT_EXEC")
    return segments, entry


def expand_execute_window(blob, window=WINDOW_SIZE, slot=AURA_SLOT):
    """Pad a boot object's __TEXT_EXEC so iBoot arms a larger execute window.

    aura's code keeps its bytes and addresses, __TEXT_EXEC becomes
    execute_window_code_size() bytes and every later segment moves up by the same amount in file and
    memory, so all segments stay file-backed at their virtual offsets. Load commands this cannot move
    are refused. Returns (head, padding, tail, read_only_size): the file is head, `padding` zero
    bytes, then tail; read_only_size is the kcrz kmutil should derive.
    """
    if window <= 0 or window % PAGE or slot % PAGE:
        raise ValueError("its execute window is not a whole number of pages")
    if window > 1 << 40 or slot > 1 << 40:
        raise ValueError("its execute window is too large")
    segments, entry = layout(blob)
    c_vmaddr, c_vmsize, c_fileoff, c_filesize = segments["__TEXT_EXEC"]
    if entry != c_vmaddr + ENTRY_OFFSET:
        raise ValueError("it does not enter aura at its entry point")
    b = bytearray(blob)
    start = c_fileoff
    if start + 0x820 > len(b) or b[start + LOAD_HEADER_OFFSET:start + LOAD_HEADER_OFFSET + 8] != LOAD_HEADER_MAGIC:
        raise ValueError("__TEXT_EXEC does not start with aura")
    if _u64(b, start + LOAD_HEADER_OFFSET + 16) > slot or c_vmsize > slot:
        raise ValueError("aura does not fit the room kept for it")
    code_size = execute_window_code_size(window, slot)
    growth = code_size - c_vmsize
    code_end = c_vmaddr + c_vmsize
    file_end = c_fileoff + c_filesize
    if file_end % PAGE or c_vmaddr % PAGE:
        raise ValueError("__TEXT_EXEC is not page-aligned")

    def moved32(value):
        value += growth
        if value >= 1 << 32:
            raise ValueError("its execute window is too large")
        return value

    headers, index = [0], 0
    while index < len(headers):
        if len(set(headers)) != len(headers):
            raise ValueError("a fileset member repeats")
        header_at = headers[index]
        index += 1
        if _u32(b, header_at) != MH_MAGIC_64:
            raise ValueError("a fileset member has no Mach-O header")
        offset = header_at + 32
        for _ in range(_u32(b, header_at + 16)):
            command, length = _u32(b, offset), _u32(b, offset + 4)
            if length < 8 or offset + length > len(b):
                raise ValueError("malformed load command")
            if command == LC_SEGMENT_64:
                if length < 72:
                    raise ValueError("malformed segment")
                vmaddr, vmsize = _u64(b, offset + 24), _u64(b, offset + 32)
                name = b[offset + 8:offset + 24].split(b"\0")[0].decode(errors="replace")
                if name == "__TEXT_EXEC":
                    if (vmaddr, vmsize) != (c_vmaddr, c_vmsize):
                        raise ValueError("two headers disagree about __TEXT_EXEC")
                    struct.pack_into("<Q", b, offset + 32, code_size)
                    struct.pack_into("<Q", b, offset + 48, code_size)
                elif vmaddr >= code_end:
                    struct.pack_into("<Q", b, offset + 24, vmaddr + growth)
                    struct.pack_into("<Q", b, offset + 40, _u64(b, offset + 40) + growth)
                    sections = _u32(b, offset + 64)
                    if length != 72 + 80 * sections:
                        raise ValueError("malformed segment")
                    for section in range(sections):
                        at = offset + 72 + 80 * section
                        struct.pack_into("<Q", b, at + 32, _u64(b, at + 32) + growth)
                        file_offset = _u32(b, at + 48)
                        if file_offset:
                            struct.pack_into("<I", b, at + 48, moved32(file_offset))
                elif vmaddr + vmsize > c_vmaddr:
                    raise ValueError(f"{name} overlaps __TEXT_EXEC")
            elif command == LC_DYLD_CHAINED_FIXUPS:
                # aura relocates itself, so its table must name no fixup to move.
                data = _u32(b, offset + 8)
                starts = data + _u32(b, data + 4)
                for seg in range(_u32(b, starts)):
                    if _u32(b, starts + 4 + 4 * seg):
                        raise ValueError("it has chained fixups")
                if data >= file_end:
                    struct.pack_into("<I", b, offset + 8, moved32(data))
            elif command == LC_FILESET_ENTRY:
                if not (_u64(b, offset + 8) < c_vmaddr and _u64(b, offset + 16) < c_fileoff):
                    raise ValueError("a fileset member is not before __TEXT_EXEC")
                headers.append(_u64(b, offset + 16))
            elif command in (LC_SYMTAB, LC_DYSYMTAB):
                if any(b[offset + 8:offset + length]):
                    raise ValueError("it has a symbol table")
            elif command not in (LC_UNIXTHREAD, LC_UUID, LC_BUILD_VERSION):
                raise ValueError("it has a load command that cannot be moved")
            offset += length

    head, tail = bytes(b[:file_end]), bytes(b[file_end:])
    layout(head, file_size=len(head) + growth + len(tail))
    return head, growth, tail, c_fileoff + code_size


def write_sparse(path, head, padding, tail):
    """Write head, `padding` zero bytes as a hole, then tail; fsync and rename into place."""
    path = Path(path)
    fd, temporary = tempfile.mkstemp(prefix="." + path.name + ".", dir=path.parent)
    try:
        os.ftruncate(fd, len(head) + padding + len(tail))
        os.pwrite(fd, head, 0)
        os.pwrite(fd, tail, len(head) + padding)
        if os.pread(fd, len(head), 0) != head or os.pread(fd, len(tail), len(head) + padding) != tail:
            raise OSError("the padded object did not read back")
        os.fsync(fd)
        os.close(fd)
        fd = -1
        os.chmod(temporary, 0o644)
        os.replace(temporary, path)
    finally:
        if fd >= 0:
            os.close(fd)
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("raw", type=Path,
                        help="raw Aura image, e.g. build/aura.bin (or, with --execute-window, a boot object)")
    parser.add_argument("output", type=Path)
    parser.add_argument("--execute-window", action="store_true",
                        help="pad __TEXT_EXEC for the M4-and-later code-execute window")
    parser.add_argument("--window-mib", type=int, default=WINDOW_SIZE >> 20)
    parser.add_argument("--slot-mib", type=int, default=AURA_SLOT >> 20)
    args = parser.parse_args()
    if args.raw.resolve() == args.output.resolve():
        parser.error("use a separate output path")
    data = args.raw.read_bytes()
    if args.execute_window and data[:4] == struct.pack("<I", MH_MAGIC_64):
        result = data
    else:
        result = wrap(data)
    if not args.execute_window:
        args.output.write_bytes(result)
        print(f"  BOOT  {args.output}: {len(result)} bytes, entry at file offset "
              f"{IMAGE_OFFSET + ENTRY_OFFSET:#x}, no __TEXT_BOOT_EXEC")
        return
    try:
        head, padding, tail, kcrz = expand_execute_window(result, args.window_mib << 20, args.slot_mib << 20)
    except ValueError as error:
        raise SystemExit(f"macho_boot_object.py: cannot pad {args.raw}: {error}")
    write_sparse(args.output, head, padding, tail)
    print(f"  BOOT  {args.output}: {len(head) + padding + len(tail)} bytes ({padding} of them a hole), "
          f"kcrz {kcrz}; enroll with kmutil configure-boot --compress")


if __name__ == "__main__":
    main()
