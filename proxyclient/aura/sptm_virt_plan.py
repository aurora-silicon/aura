# SPDX-License-Identifier: MIT
"""
iBoot -> SPTM handoff planner for the virtualized-SPTM track (docs/sptm-nv2-virt.md).

The stock SPTM's entry registers are tiny: x0 is the physical address of an
XNU-style ``boot_args`` structure (the 1024-byte-command-line "macOS" layout,
revision 2 / version 2, 0x480 bytes). Everything else SPTM needs is described
by two things iBoot normally builds for it before the jump:

  * a contiguous physical "blob" holding the TXM, BootKC (kernelcache), SPTM,
    TrustCache and device-tree pieces in a fixed order; and
  * /chosen/memory-map properties, each a {physical address, size} pair, that
    tell SPTM where every piece of that blob went (``SPTM-ro``, ``TXM-rx``,
    ``BootKC-rs`` ...), plus the ``*-virt`` / ``*-entry`` link-time anchors.

In kcbz=0 mode iBoot skips its SPTM loader, so aura's host side has to do this
itself. This module is the pure-math half: it takes the three Mach-O images,
the live ADT and the live managed-RAM window, and produces a ``Plan`` that
says what to write where. It performs no I/O so it can be unit-tested off
hardware (tests/python/test_sptm_virt_plan.py).

What SPTM's own ``__start`` is known (from its disassembly) to do with the
block, which fixes the semantics of the fields below:

  * ``[0x00]``  revision/version; ``__start`` also reads ``[0x480]`` (just past
    the structure) when the 64-bit value at ``[0x00]`` is >= 3 and zero-checks
    it, so the byte range [0x480, 0x488) after the struct must be readable
    (we zero it).
  * ``[0x08]``  virtBase  (x22), ``[0x10]`` physBase (x23), ``[0x18]`` memSize
    (x24). SPTM turns these into a 32 MiB-block map ``VA = PA + virtBase -
    physBase`` over the managed window, after rounding physBase up to the next
    32 MiB boundary. SPTM's own image therefore runs at
    ``sptm_pa + virtBase - physBase``.
  * ``[0x60]/[0x68]`` ADT VA and length, ``[0x6c]`` command line.

The layout order and the L2-index/stride rules were established against a
fixture that boots the real 25G83 SPTM with a synthetic iBoot; this is an
independent implementation of that observable contract.
"""

import re
import struct
from dataclasses import dataclass, field

PAGE = 0x4000
SPTM_EXPECTED_STRIDE = 0x10000000  # SPTM puts TXM/BootKC 1x/2x this below itself (VA)
L2_INDEX_MASK = (1 << 36) - 1
MIN_PLACEMENT_SHIFT = 0x2000000  # the smallest phys slide in front of TXM that boots
BLOCK = 0x2000000  # SPTM maps its window in 32 MiB blocks
MEMORY_MAP_PATH = "chosen/memory-map"
ABSENT = 0xFFFFFFFFFFFFFFFF  # an absent memory-map region ({ABSENT, ABSENT})
OS_LOG_SEGMENT = 0xA  # segment-ranges flags of a coprocessor's __OS_LOG buffer

BOOT_ARGS_SIZE = 0x480
# SPTM's __start reads a diagnostic-record pointer at +0x480 when the revision is 3 or more, zeroes
# 0x17c0 bytes there and stamps it; its EL1 exception vector records ESR/ELR/FAR (+0x20/+0x28/+0x30)
# into it and halts. Without it an unexpected exception halts with nothing recorded. It is a
# physical address: __start zeroes the record before it turns its MMU on.
BOOT_ARGS_DIAG_OFF = 0x480
BOOT_ARGS_DIAG_SIZE = 0x17c0
BOOT_ARGS_REVISION = 2
BOOT_ARGS_VERSION = 2
CMDLINE_MAX = 1024

LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x5
ARM_THREAD_STATE64 = 6


def round_page(x):
    return (x + PAGE - 1) & ~(PAGE - 1)


@dataclass
class Seg:
    name: str
    vmaddr: int
    vmsize: int
    fileoff: int
    filesize: int


@dataclass
class MachOInfo:
    segs: dict
    virtlo: int
    virthi: int
    entry: int
    data: bytes = field(repr=False, default=b"")

    def seg(self, name):
        return self.segs.get(name)

    def seglen(self, name):
        s = self.segs.get(name)
        return s.vmsize if s else 0


def macho_info(data):
    """Top-level segments and the LC_UNIXTHREAD entry of a thin arm64 Mach-O."""
    magic, _cpu, _sub, _ft, ncmds, _sz, _fl, _r = struct.unpack_from("<IIIIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise ValueError("not a 64-bit little-endian Mach-O")
    off, segs, entry = 32, {}, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            name = data[off + 8:off + 24].split(b"\0")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            if vmsize:
                segs[name] = Seg(name, vmaddr, vmsize, fileoff, filesize)
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from("<II", data, off + 8)
            if flavor == ARM_THREAD_STATE64:
                # arm_thread_state64: x0-x28, fp, lr, sp, pc, cpsr -> pc is word 32.
                (entry,) = struct.unpack_from("<Q", data, off + 16 + 32 * 8)
        off += size
    real = [s for s in segs.values() if s.vmsize]
    if not real or entry is None:
        raise ValueError("Mach-O has no segments or no LC_UNIXTHREAD entry")
    return MachOInfo(segs, min(s.vmaddr for s in real), max(s.vmaddr + s.vmsize for s in real),
                     entry, data)


# --- minimal raw ADT editor ----------------------------------------------------
# aura.adt's parser rejects some real device trees, and the planner only needs
# to find a node, read/overwrite a 16-byte property and append a missing one.

class RawNode:
    def __init__(self):
        self.props = []  # [name, bytes, flags]
        self.kids = []

    def name(self):
        for n, v, _f in self.props:
            if n == "name":
                return v.split(b"\0")[0].decode()
        return ""


def adt_parse(data, off=0):
    node = RawNode()
    nprops, nkids = struct.unpack_from("<II", data, off)
    off += 8
    for _ in range(nprops):
        name = data[off:off + 32].split(b"\0")[0].decode()
        (ln,) = struct.unpack_from("<I", data, off + 32)
        size, flags = ln & 0x7FFFFFFF, ln >> 31
        off += 36
        node.props.append([name, bytes(data[off:off + size]), flags])
        off += (size + 3) & ~3
    for _ in range(nkids):
        kid, off = adt_parse(data, off)
        node.kids.append(kid)
    return node, off


def adt_build(node):
    out = bytearray(struct.pack("<II", len(node.props), len(node.kids)))
    for name, val, flags in node.props:
        out += name.encode().ljust(32, b"\0")
        out += struct.pack("<I", len(val) | (flags << 31))
        out += val + b"\0" * (-len(val) % 4)
    for kid in node.kids:
        out += adt_build(kid)
    return bytes(out)


def adt_find(root, path):
    node = root
    for part in path.strip("/").split("/"):
        for kid in node.kids:
            if kid.name() == part:
                node = kid
                break
        else:
            raise KeyError(path)
    return node


def adt_set_region(node, name, pa, size):
    val = struct.pack("<QQ", pa, size)
    for p in node.props:
        if p[0] == name:
            if len(p[1]) != 16:
                raise ValueError(f"{name}: expected a 16-byte region property")
            p[1] = val
            return False
    node.props.append([name, val, 0])
    return True  # inserted: the device tree grew


# --- the plan -----------------------------------------------------------------

@dataclass
class Load:
    """Copy ``size`` bytes of ``image`` from ``fileoff`` to ``pa``, zero-filled to ``total``."""
    what: str
    pa: int
    image: str  # "sptm" | "txm" | "xnu" | "bytes" | "copy" (from PA ``fileoff`` on the device)
    fileoff: int = 0
    size: int = 0
    total: int = 0
    payload: bytes = b""


@dataclass
class Plan:
    regions: dict  # memory-map name -> (pa, size)
    loads: list
    boot_args: bytes
    boot_args_pa: int
    diag_pa: int
    adt: bytes
    adt_pa: int
    adt_grew: bool
    sptm_pa: int
    sptm_entry_pa: int
    txm_pa: int
    virt_base: int
    phys_base: int
    mem_size: int
    top_of_kernel_data: int
    blob_start: int
    blob_end: int
    notes: list


def build_boot_args(virt_base, phys_base, mem_size, top_of_kernel_data, adt_va, adt_len,
                    cmdline, mem_size_actual, boot_flags=0, video=None, diag_pa=0):
    cmd = cmdline.encode() if isinstance(cmdline, str) else cmdline
    if len(cmd) >= CMDLINE_MAX:
        raise ValueError("boot-args command line too long")
    b = bytearray(BOOT_ARGS_SIZE + 8)  # +8: the diagnostic-record pointer
    struct.pack_into("<Q", b, BOOT_ARGS_DIAG_OFF, diag_pa)
    struct.pack_into("<HH", b, 0, 3 if diag_pa else BOOT_ARGS_REVISION, BOOT_ARGS_VERSION)
    struct.pack_into("<QQQQ", b, 8, virt_base, phys_base, mem_size, top_of_kernel_data)
    vid = video or (0, 0, 0, 0, 0, 0)
    struct.pack_into("<6Q", b, 0x28, *vid)
    struct.pack_into("<Q", b, 0x60, adt_va)
    struct.pack_into("<I", b, 0x68, adt_len)
    b[0x6C:0x6C + len(cmd)] = cmd
    struct.pack_into("<QQ", b, 0x470, boot_flags, mem_size_actual)
    return bytes(b)


def set_os_long_version(root, version, notes):
    """TXM checks a cryptex against the manifest's OS long version, which a tree a non-macOS
    stage booted lacks."""
    if not re.fullmatch(r"[0-9]+(\.[0-9]+){4},[0-9]+", version):
        raise ValueError(f"not an OS long version: {version!r}")
    try:
        manifest = adt_find(root, "chosen/manifest-properties")
    except KeyError:
        raise ValueError("the device tree has no /chosen/manifest-properties") from None
    value = version.encode() + b"\0"
    for prop in manifest.props:
        if prop[0] == "love":
            if prop[1].rstrip(b"\0") != value.rstrip(b"\0"):
                raise ValueError("the live OS long version differs from the guest's")
            return
    manifest.props.append(["love", value, 0])
    notes.append(f"manifest love = {version}")


def adt_hide_nodes(root, hide_nodes, notes):
    """Remove ``hide_nodes`` (ADT paths) from the guest tree, and drop a hidden ATC's
    display-crossbar endpoint, the way aura hv's setup_adt hides a USB proxy port.

    The proxy rides KIS (DockChannel + hpm0 in DBMa debug mode); XNU's AppleHPM/Type-C/USB
    drivers reset that port during IOKit start and the link drops."""
    for path in hide_nodes:
        parent_path, _, leaf = path.rstrip("/").rpartition("/")
        try:
            parent = adt_find(root, parent_path) if parent_path.strip("/") else root
        except KeyError:
            notes.append(f"hide: {path} absent (parent)")
            continue
        before = len(parent.kids)
        parent.kids = [k for k in parent.kids if k.name() != leaf]
        notes.append(f"hide: {path} {'removed' if len(parent.kids) < before else 'absent'}")
    leaves = {p.rstrip("/").rpartition("/")[2] for p in hide_nodes}
    for idx in ("0", "1"):
        atc = "atc" + idx
        if "atc-phy" + idx not in leaves and not any(l.startswith(atc + "-") for l in leaves):
            continue
        try:
            io = adt_find(root, "arm-io")
        except KeyError:
            return
        for kid in io.kids:
            if not kid.name().startswith("display-crossbar"):
                continue
            for prop in kid.props:
                if prop[0] != "dfp-endpoints":
                    continue
                eps = [e for e in prop[1].split(b"\0") if e]
                keep = [e for e in eps if e.decode(errors="replace") != atc]
                if len(keep) != len(eps):
                    prop[1] = b"\0".join(keep) + b"\0"
                    notes.append(f"hide: dropped {atc} from {kid.name()} dfp-endpoints")


def adt_drop_props(root, drop_props, notes):
    """Delete properties: ``drop_props`` is a list of (node path, property name). The SPTM
    emulator's run_guest clears ``routes`` on the dcp/aop/aop2 iop nubs: RTBuddy resolves
    each route with an untimed waitForMatchingService on a secure-rtbuddy-proxy mailbox
    that never attaches while exclaves stay NOT_SUPPORTED, so the nub would block forever;
    without ``routes`` the firmware falls back to the plain ASC mailbox."""
    for path, name in drop_props:
        try:
            node = adt_find(root, path.strip("/"))
        except KeyError:
            notes.append(f"drop: {path} absent")
            continue
        before = len(node.props)
        node.props = [pr for pr in node.props if pr[0] != name]
        notes.append(f"drop: {path}/{name} {'removed' if len(node.props) < before else 'absent'}")


def adt_set_serial_device(root, node_path, notes):
    """Point XNU's console (/defaults/serial-device) at ``node_path`` by phandle. The SPTM
    emulator's HV does this (uart0) so XNU's IOKit serial stack never takes over the
    DockChannel that carries the KIS proxy link."""
    try:
        target = adt_find(root, node_path.strip("/"))
        defaults = adt_find(root, "defaults")
    except KeyError:
        notes.append(f"serial: {node_path} or /defaults absent")
        return
    ph = next((v for n, v, _f in target.props if n == "AAPL,phandle"), None)
    for prop in defaults.props:
        if prop[0] == "serial-device" and ph is not None:
            old = prop[1]
            prop[1] = ph
            notes.append(f"serial: /defaults/serial-device {old.hex()} -> {ph.hex()} ({node_path})")
            return
    notes.append("serial: no /defaults/serial-device or phandle")


def plan_boot(sptm, txm, xnu, adt, *, dram_base, phys_base, mem_size, mem_size_actual,
              cmdline="serial=3 -v", trustcache=b"", boot_flags=0, ramdisk_size=0,
              placement_shift=None, blob_base=None, hide_nodes=(), xnu_serial=None,
              drop_props=(), cl4_regions=None, cl4_entry=0, cl4_virt=0,
              exclave_integrity=b"", exclave_trustcache=b"", iboot_copies=(),
              diag_record=False, os_long_version=None):
    """Compute the whole iBoot->SPTM handoff.

    sptm/txm/xnu are the raw Mach-O files, ``adt`` the live device tree,
    ``dram_base`` the DRAM base (4 GiB aligned), ``phys_base``/``mem_size`` the
    SPTM managed window (aura's carve-out already removed from its end).

    ``iboot_copies``: (name, src_pa, size) of iBoot's data regions (RTBuddySeg, SEPFW,
    SEPPatches, uStuff, preoslog) to copy to the blob tail, after the images, as iBoot lays
    them out on a native boot. Then nothing SPTM walks lies below ``phys_base``, which can sit
    above aura and everything else iBoot left low: XNU frees the phys-slide gap between
    physBase and TXM to its VM (arm_vm_prot_finalize), so nothing live may be in it. The
    coprocessors' __OS_LOG segments, which live in iBoot's RTBuddySeg, are pointed into the
    copy, as natively they lie inside RTBuddySeg in managed RAM.

    ``diag_record``: give SPTM a diagnostic record (boot_args revision 3, an SPTMDebug region).

    ``os_long_version``: the guest's Ap,OSLongVersion (its BuildIdentity), given to TXM as the
    manifest's ``love`` when the live tree has none; a different live value is refused.
    """
    notes = []
    si, ti, xi = macho_info(sptm), macho_info(txm), macho_info(xnu)
    root, _ = adt_parse(adt)
    if os_long_version is not None:
        set_os_long_version(root, os_long_version, notes)

    adt_hide_nodes(root, hide_nodes, notes)
    adt_drop_props(root, drop_props, notes)
    if xnu_serial:
        adt_set_serial_device(root, xnu_serial, notes)

    mm = adt_find(root, MEMORY_MAP_PATH)

    # iBoot's chosen/memory-map TrustCache is a {num_caches:u32, offsets[num]:u32} + caches
    # container (TXM logs "trust cache modules"); the real boot's holds the full OS coverage
    # (e.g. 2 caches, 15166 + 1073 entries). If the caller hands such a container verbatim,
    # place it unchanged; otherwise wrap a single raw trust cache as a 1-module container.
    def _is_tc_container(b):
        if len(b) < 8:
            return False
        num = struct.unpack_from("<I", b, 0)[0]
        if not (1 <= num <= 15) or len(b) < 4 + 4 * num:
            return False
        return struct.unpack_from("<I", b, 4)[0] == 4 + 4 * num  # offsets[0] == header size
    if trustcache and _is_tc_container(trustcache):
        tc_blob = trustcache
    else:
        tc_blob = struct.pack("<II", 1, 8) + trustcache  # {num_caches, offsets[0]=8} + caches
    tc_size = round_page(len(tc_blob))
    adt_size_rounded = round_page(len(adt) + 0x2000)  # headroom: inserting props + /chosen/hibernation

    regions, loads = {}, []
    head = [dram_base]  # mutable blob head
    tail = [dram_base]

    def push_seg(img, info, segname, what):
        s = info.seg(segname)
        if not s:
            notes.append(f"{what}: segment {segname} absent, zero length")
            return
        loads.append(Load(f"{what}:{segname}", head[0], img, s.fileoff, s.filesize, s.vmsize))
        head[0] += s.vmsize

    def end_entry(name):
        assert head[0] % PAGE == 0, name
        regions[name] = (tail[0], head[0] - tail[0])
        tail[0] = head[0]

    def skip(n):
        head[0] += n
        tail[0] = head[0]

    # Where SPTM-ro would land with no leading gap, then shift so its offset
    # from dram_base has the BootKC's low 36 virtual bits (stride alignment).
    # The cL4 bundle + exic/extc are laid down between BootKC-rs and the ADT, so their
    # total must be in before_sptm or SPTM lands off its L2 stride.
    # Only CL4-rx + CL4-ro sit before SPTM (after BootKC-rs); CL4-rw/le go after SPTM and
    # the exic/extc blobs at the blob tail, so none of those are part of before_sptm.
    cl4_total = 0
    if cl4_regions is not None:
        cl4_total = round_page(cl4_regions["CL4-rx"][1]) + round_page(cl4_regions["CL4-ro"][1])
    before_sptm = (
        sum(ti.seglen(n) for n in ("__TEXT", "__DATA_CONST", "__TEXT_EXEC", "__TEXT_BOOT_EXEC"))
        + sum(xi.seglen(n) for n in ("__TEXT_EXEC", "__TEXT_BOOT_EXEC", "__TEXT", "__PRELINK_TEXT",
                                     "__DATA_CONST", "__DATA_SPTM"))
        + cl4_total
        + adt_size_rounded + tc_size + PAGE)  # PAGE: spare page after the ADT (in its entry)
    if blob_base is None:
        blob_base = phys_base
    lead = (xi.virtlo & L2_INDEX_MASK) - before_sptm
    if lead <= 0:
        # This lead only sets the layout when the blob starts at the DRAM base; otherwise the
        # placement shift below moves the whole blob to SPTM's first mapped block and supersedes
        # it. 26A428 with exclaves needs 0x%x more than the BootKC's L2 index allows.
        if blob_base <= dram_base:
            raise ValueError("BootKC L2 index leaves no room for the TXM/BootKC pieces below SPTM")
        notes.append("the pieces below SPTM (%#x) exceed the BootKC's L2 index (%#x) by %#x; the "
                     "placement shift sets the layout instead"
                     % (before_sptm, xi.virtlo & L2_INDEX_MASK, -lead))
        lead = 0
    skip(lead)
    # SPTM's __start rounds physBase up to the NEXT 32 MiB boundary (strictly
    # above it) and only maps its window from there; a TXM below that has no
    # mapping and panics "PHYS_SLIDE: Required PAPT range has no mappings" (this
    # is why TXM exactly at physBase failed). The whole blob must start at or
    # above that boundary.
    # blob_base lets the caller place the image blob above physBase (SPTM's frame-table base);
    # by default the blob starts at physBase (resolved above, for the lead).
    first_mapped = (blob_base + BLOCK) & ~(BLOCK - 1)
    shift = placement_shift
    if shift is None:
        shift = 0
        if blob_base > head[0]:
            shift = max(round_page(first_mapped - head[0]), MIN_PLACEMENT_SHIFT)
            notes.append(f"placement shift 0x{shift:x} (blob head 0x{head[0]:x} -> first mapped 0x{first_mapped:x})")
    skip(shift)
    if head[0] < first_mapped:
        raise ValueError(f"blob would start at 0x{head[0]:x}, below SPTM's first mapped block 0x{first_mapped:x}")

    push_seg("txm", ti, "__TEXT", "TXM-ro")
    push_seg("txm", ti, "__DATA_CONST", "TXM-ro")
    end_entry("TXM-ro")
    push_seg("txm", ti, "__TEXT_EXEC", "TXM-rx")
    end_entry("TXM-rx")
    push_seg("txm", ti, "__TEXT_BOOT_EXEC", "TXM-bx")
    end_entry("TXM-bx")
    txm_pa = regions["TXM-ro"][0]

    loads.append(Load("TrustCache", head[0], "bytes", total=tc_size, payload=tc_blob))
    head[0] += tc_size
    end_entry("TrustCache")

    push_seg("xnu", xi, "__TEXT_EXEC", "BootKC-rx")
    end_entry("BootKC-rx")
    push_seg("xnu", xi, "__TEXT_BOOT_EXEC", "BootKC-bx")
    end_entry("BootKC-bx")
    for sn in ("__TEXT", "__PRELINK_TEXT", "__DATA_CONST"):
        push_seg("xnu", xi, sn, "BootKC-ro")
    end_entry("BootKC-ro")
    push_seg("xnu", xi, "__DATA_SPTM", "BootKC-rs")
    end_entry("BootKC-rs")

    # --- ExclaveCore (cL4) image regions, when exclaves are on ---
    # SPTM's validate_region_order follows the BootKC split: the executable/read-only
    # groups CL4-rx then CL4-ro go here (immediately after BootKC-rs, then the ADT), and
    # the writable/linkedit groups CL4-rw, CL4-le go after SPTM with the other rw/le
    # regions. iBoot (not SPTM) parses the BUND bundle; we slice it per region (SPTM maps
    # them via CL4-rx + the cl4_entry-cl4_virt bias). before_sptm includes only CL4-rx +
    # CL4-ro so SPTM still lands on its L2 stride. The exic/extc blobs are not image
    # regions -> placed at the blob tail with the other named blobs.
    cl4_set = {}
    if cl4_regions is not None:
        for rn in ("CL4-rx", "CL4-ro"):
            off, size = cl4_regions[rn]
            loads.append(Load(rn, head[0], "cl4", off, size, round_page(size)))
            cl4_set[rn] = (head[0], size)
            head[0] += round_page(size)
            tail[0] = head[0]

    # The DeviceTree entry must START at the tree: SPTM hands cL4 the entry's base as the ADT
    # address (cL4 handoff +0x38; boot_args' tree VA goes to +0x128), and cL4 and its Roottask
    # parse the tree from there. With a spare page in front of the tree they parsed that zero
    # page: "Could not find required mapper mapper-admac-base-s". The
    # spare page (kept so the layout and SPTM's stride do not move) now follows the tree;
    # the page in front is the previous region's, readable as before.
    adt_pa = head[0]
    head[0] += adt_size_rounded + PAGE
    end_entry("DeviceTree")

    # SPTM is laid down whole (flat, fileoff == vmaddr - virtlo), then described
    # by four overlapping slices.
    sptm_pa = head[0]
    loads.append(Load("SPTM", sptm_pa, "sptm", 0, len(sptm), si.virthi - si.virtlo))

    def vtop(v):
        return sptm_pa + (v - si.virtlo)

    def slice_of(names):
        segs = [si.seg(n) for n in names]
        lo = vtop(segs[0].vmaddr)
        return lo, sum(s.vmsize for s in segs)

    regions["SPTM-ro"] = slice_of(("__TEXT", "__DATA_CONST", "__LATE_CONST"))
    regions["SPTM-rx"] = slice_of(("__TEXT_EXEC", "__LAST"))
    regions["SPTM-rw"] = slice_of(("__DATA", "__BOOTDATA"))
    regions["SPTM-le"] = slice_of(("__LINKEDIT",))
    # hib_bootstrap_register_iboot_loaded_range looks up SPTM-ro/SPTM-rm/SPTM-rx/SPTM-le and
    # registers [SPTM-ro.begin, SPTM-rm.begin) and [SPTM-rm.end, SPTM-rx.end). The plan never set
    # SPTM-rm, so the boot-stage ADT left it the all-ones "absent" sentinel -> size = allones -
    # SPTM-ro.begin -> "not page aligned" panic. SPTM-rm = (SPTM-rx.begin, 0) splits the SPTM image
    # into [SPTM-ro] + [SPTM-rx], both page-aligned, no underflow.
    regions["SPTM-rm"] = (regions["SPTM-rx"][0], 0)
    skip(si.virthi - si.virtlo)

    push_seg("txm", ti, "__DATA", "TXM-rw")
    end_entry("TXM-rw")
    push_seg("txm", ti, "__LINKEDIT", "TXM-le")
    end_entry("TXM-le")
    push_seg("xnu", xi, "__PRELINK_INFO", "BootKC-rw")
    push_seg("xnu", xi, "__DATA", "BootKC-rw")
    end_entry("BootKC-rw")
    push_seg("xnu", xi, "__LINKEDIT", "BootKC-le")
    end_entry("BootKC-le")
    # cL4 writable + linkedit groups, after SPTM with the other rw/le regions (before
    # CL4-dummypage), per SPTM's validate_region_order.
    if cl4_regions is not None:
        for rn in ("CL4-rw", "CL4-le"):
            off, size = cl4_regions[rn]
            loads.append(Load(rn, head[0], "cl4", off, size, round_page(size)))
            cl4_set[rn] = (head[0], size)
            head[0] += round_page(size)
            tail[0] = head[0]
        # cL4 never reads the CL4-ro address: it takes its data window as the bundle bytes
        # [CL4-rw offset, end) laid out linearly from CL4-rw (rw + le + the read-only tail with
        # the Mach-O headers and the DERs), and hands its Roottask the bundle DER page at
        # cl4_rw_pa + 0x2e0000 (cL4 rx 0x60c8 / 0xd0d8). SPTM meanwhile requires CL4-ro right
        # after CL4-rx. So the tail exists twice: CL4-ro after CL4-rx for SPTM, and a copy
        # after CL4-le that the CL4-le entry grows to cover, so SPTM types it as cL4's too.
        # Without it the Roottask parsed the integrity catalog: "found wrong DER in bundle".
        rw_off = cl4_regions["CL4-rw"][0]
        ro_off, ro_size = cl4_regions["CL4-ro"]
        if head[0] != cl4_set["CL4-rw"][0] + (ro_off - rw_off):
            raise ValueError("CL4-rw/CL4-le are not laid out linearly by bundle offset")
        loads.append(Load("CL4-ro (data-window copy)", head[0], "cl4", ro_off, ro_size,
                          round_page(ro_size)))
        le_pa, le_size = cl4_set["CL4-le"]
        cl4_set["CL4-le"] = (le_pa, le_size + round_page(ro_size))
        head[0] += round_page(ro_size)
        tail[0] = head[0]
    head[0] += PAGE
    end_entry("CL4-dummypage")

    boot_args_pa = head[0]
    head[0] += round_page(BOOT_ARGS_SIZE + 8)
    end_entry("BootArgs")
    diag_pa = 0
    if diag_record:
        diag_pa = head[0]
        head[0] += round_page(BOOT_ARGS_DIAG_SIZE)
        end_entry("SPTMDebug")
    rd_pa = head[0]
    head[0] += round_page(ramdisk_size)
    end_entry("RAMDisk")

    # ExclaveOS integrity catalog + trust cache: named blobs (not image regions), at the tail.
    if cl4_regions is not None:
        for rn, blob in (("ExclaveOSIntegrityCatalog", exclave_integrity),
                         ("ExclaveOSTrustCache", exclave_trustcache)):
            if not blob:
                continue
            total = round_page(len(blob))
            loads.append(Load(rn, head[0], "bytes", total=total, payload=blob))
            cl4_set[rn] = (head[0], len(blob))
            head[0] += total
        tail[0] = head[0]

    # iBoot's data regions, copied to the tail in iBoot's native order (see iboot_copies).
    iboot_moved = {}
    for rn, src, size in iboot_copies:
        total = round_page(size)
        loads.append(Load(rn, head[0], "copy", fileoff=src, size=size, total=total))
        iboot_moved[rn] = (src, head[0], size)
        regions[rn] = (head[0], size)
        head[0] += total
    tail[0] = head[0]

    blob_end = head[0]

    # The ADT memory-map entry for DeviceTree covers the tree and the spare page after it.
    regions["DeviceTree"] = (adt_pa, adt_size_rounded + PAGE)
    regions["RAMDisk"] = (rd_pa, round_page(ramdisk_size))
    regions["TXM-virt"] = (ti.virtlo, 0)
    regions["TXM-entry"] = (ti.entry, 0)
    regions["BootKC-virt"] = (xi.virtlo, 0)
    regions["BootKC-entry"] = (xi.entry, 0)
    regions["SPTM-virt"] = (si.virtlo, 0)
    regions["SPTM-entry"] = (si.entry, 0)

    # XNU-bootstrap image regions SPTM looks up (init_get_image_region) that iBoot
    # would have provided on a slid-KC boot but aura's boot-stage ADT lacks:
    #  - slide: XNU vm_kernel_slide, pure pass-through metadata. aura applies no
    #    KASLR and maps the KC at its link VA, so the self-consistent value is 0.
    #  - CL4-entry/virt: unconditional required lookups right after 'slide'.
    #  - CL4-rx/CL4-ro: without exclaves, the absent sentinel (all-ones) makes SPTM compute a
    #    CL4 bias of -1 and skip the secure-kernel bootstrap.
    # exclaves-enabled is a /product DT property, NOT a memory-map region - do not inject it.
    regions["slide"] = (0, 0)                  # vm_kernel_slide; aura applies no KASLR
    if cl4_regions is not None:
        # Exclaves ON: hand SPTM the laid-out cL4 regions so its CL4 bias is valid
        # (CL4-rx.begin + cl4_entry - cl4_virt) and it runs the SK/exclaves bootstrap.
        regions["CL4-entry"] = (cl4_entry, 0)
        regions["CL4-virt"] = (cl4_virt, 0)
        regions["CL4-rx"] = cl4_set["CL4-rx"]
        regions["CL4-rw"] = cl4_set["CL4-rw"]
        regions["CL4-le"] = cl4_set["CL4-le"]
        regions["CL4-ro"] = cl4_set["CL4-ro"]
        for rn in ("ExclaveOSIntegrityCatalog", "ExclaveOSTrustCache"):
            if rn in cl4_set:
                regions[rn] = cl4_set[rn]
    else:
        regions["CL4-entry"] = (0, 0)          # required lookup; contributes 0 to the CL4 bias
        regions["CL4-virt"] = (0, 0)           # required lookup; contributes 0 to the CL4 bias
        regions["CL4-rx"] = (ABSENT, ABSENT)   # absent sentinel: CL4 bias = -1 -> SK bootstrap skipped
        regions["CL4-ro"] = (ABSENT, ABSENT)   # same (optional); skipped by validate_region_order

    # SPTM derives TXM at sptm.virtlo + 1 stride, BootKC at + 2: sanity-check the
    # images still have the link-time relation the layout assumes.
    if (si.virtlo - ti.virtlo) & L2_INDEX_MASK != SPTM_EXPECTED_STRIDE:
        notes.append("WARNING: TXM link base is not 1 stride below SPTM")
    if (si.virtlo - xi.virtlo) & L2_INDEX_MASK != 2 * SPTM_EXPECTED_STRIDE:
        notes.append("WARNING: BootKC link base is not 2 strides below SPTM")

    for name, (pa, size) in regions.items():
        adt_grew_here = adt_set_region(mm, name, pa, size)
        if adt_grew_here:
            notes.append(f"inserted missing /chosen/memory-map/{name}")
    # SPTM cross-checks boot_args against /chosen/carveout-memory-map/region-id-24 (its managed
    # RAM), which aura's carve-out shortens. The carve-out itself becomes an SPTM-reserved region
    # of its own, as the firmware carveouts are, so SPTM's page-type init leaves it alone.
    try:
        carve = adt_find(root, "chosen/carveout-memory-map")
    except KeyError:
        carve = None
        notes.append("WARNING: no /chosen/carveout-memory-map; SPTM will not find region-id-24")
    if carve is not None:
        old = [v for n, v, _f in carve.props if n == "region-id-24"]
        adt_set_region(carve, "region-id-24", phys_base, mem_size)
        if old and len(old[0]) == 16:
            iboot_base, iboot_size = struct.unpack("<QQ", old[0])
            if (iboot_base, iboot_size) != (phys_base, mem_size):
                notes.append("region-id-24 rewritten %#x+%#x -> %#x+%#x (aura carve-out removed)"
                             % (iboot_base, iboot_size, phys_base, mem_size))
            aura_base, aura_end = phys_base + mem_size, iboot_base + iboot_size
            if aura_end > aura_base:
                used = {int(n.rsplit("-", 1)[1]) for n, _v, _f in carve.props
                        if n.startswith("region-id-") and n.rsplit("-", 1)[1].isdigit()}
                rid = next(i for i in range(26, 512) if i not in used)
                adt_set_region(carve, f"region-id-{rid}", aura_base, aura_end - aura_base)
                notes.append("aura HIGH carve-out declared as region-id-%d %#x+%#x "
                             "(SPTM-reserved, must not manage)"
                             % (rid, aura_base, aura_end - aura_base))
    # /chosen/hibernation: SPTM sptm_hibentry (hib_get_key) REQUIRES this node at cold boot.
    # key-sptm-ctrr(48) + key-xnu-ctrr(48) are required present+non-zero (SPTM/XNU CTRR-image HMAC
    # keys); key-exclave(32)/key-exclave-entropy(32) are optional (exclaves skipped) but included
    # non-zero. SPTM only checks presence+exact-size+not-all-zero then copies+scrubs the DT source -
    # no boot-time crypto validation - so dummy non-zero material passes. aura's chainload ADT lacks
    # this node (iBoot populates it on a full/slid boot). The capture's keys are zeroed post-scrub.
    chosen = adt_find(root, "chosen")
    if not any(k.name() == "hibernation" for k in chosen.kids):
        hib = RawNode()
        hib.props.append(["name", b"hibernation\0", 0])
        hib.props.append(["key-sptm-ctrr", bytes([0x5a]) * 48, 0])
        hib.props.append(["key-xnu-ctrr", bytes([0xa5]) * 48, 0])
        hib.props.append(["key-exclave", bytes([0x3c]) * 32, 0])
        hib.props.append(["key-exclave-entropy", bytes([0xc3]) * 32, 0])
        chosen.kids.append(hib)
        notes.append("injected /chosen/hibernation (dummy non-zero CTRR/exclave keys)")

    # /chosen/exclave-memory-map + /chosen/exclave-misc: iBoot's ADT does NOT carry these
    # (a native boot has XNU build them at runtime), so aura's chainload tree lacks them.
    # Without them ExclaveOS component 1's launcher throws Launcher.DeviceTreeError out of a
    # try! in DERComponentGraph.swift ("Could not find exclave-memory-map region <name>") and
    # takes ExclaveOS down before any component starts; omitting /chosen/exclave-misc panics
    # DERComponentGraph.swift:157 even with a real catalog+trust cache. Each child carries
    # range={u64 base,u64 size} + u32 flags; the node names ExclaveOSIntegrityCatalog and
    # ExclaveOSTrustCache point at the exic/extc blobs we laid down. bootargstring is an empty
    # string (clears the panic; no arguments requested). Ported from the J700 SPTM emulator's
    # exclaves/adt.py add_exclave_memory_map (origin/J700-eoshv).
    if cl4_regions is not None and "ExclaveOSIntegrityCatalog" in regions \
            and "ExclaveOSTrustCache" in regions:
        if not any(k.name() == "exclave-memory-map" for k in chosen.kids):
            emm = RawNode()
            emm.props.append(["name", b"exclave-memory-map\0", 0])
            for rn in ("ExclaveOSIntegrityCatalog", "ExclaveOSTrustCache"):
                pa, size = regions[rn]
                child = RawNode()
                child.props.append(["name", rn.encode() + b"\0", 0])
                child.props.append(["range", struct.pack("<QQ", pa, size), 0])
                child.props.append(["flags", struct.pack("<I", 0), 0])
                emm.kids.append(child)
            chosen.kids.append(emm)
            notes.append("injected /chosen/exclave-memory-map "
                         "(ExclaveOSIntegrityCatalog %#x+%#x, ExclaveOSTrustCache %#x+%#x)"
                         % (*regions["ExclaveOSIntegrityCatalog"],
                            *regions["ExclaveOSTrustCache"]))
        if not any(k.name() == "exclave-misc" for k in chosen.kids):
            misc = RawNode()
            misc.props.append(["name", b"exclave-misc\0", 0])
            misc.props.append(["bootargstring", b"\0", 0])
            chosen.kids.append(misc)
            notes.append("injected /chosen/exclave-misc (empty bootargstring)")

    if iboot_moved:
        # Native layout: XNU never needs a pre-physBase page. iBoot's Kernel-mach_header is the
        # stage-1 object's (absent on a native boot), and the coprocessor __OS_LOG segments
        # point into iBoot's RTBuddySeg, now below physBase: point them at the copy.
        regions_absent = struct.pack("<QQ", ABSENT, ABSENT)
        try:
            mmn = adt_find(root, MEMORY_MAP_PATH)
            for pr in mmn.props:
                if pr[0] == "Kernel-mach_header" and len(pr[1]) == 16:
                    pr[1] = regions_absent
                    notes.append("Kernel-mach_header set absent (stage 1's, below physBase)")
        except KeyError:
            pass
        if "RTBuddySeg" in iboot_moved:
            _src, _dst, _size = iboot_moved["RTBuddySeg"]
            logs = []

            def _walk(n, path):
                for k in n.kids:
                    kp = path + "/" + k.name()
                    for pr in k.props:
                        if pr[0] == "segment-ranges" and len(pr[1]) % 32 == 0:
                            logs.append((kp, pr))
                    _walk(k, kp)
            _walk(root, "")
            # The live buffers are where the coprocessors' segments say, which can differ
            # from the memory-map entry (chainload re-homes that entry): take the lowest
            # __OS_LOG (OS_LOG_SEGMENT) below physBase as the start of the live RTBuddySeg.
            live = [struct.unpack_from("<Q", pr[1], 32 * i)[0]
                    for _kp, pr in logs for i in range(len(pr[1]) // 32)
                    if struct.unpack_from("<I", pr[1], 32 * i + 28)[0] == OS_LOG_SEGMENT
                    and struct.unpack_from("<Q", pr[1], 32 * i)[0] < phys_base]
            if live:
                base = min(live)
                for kp, pr in logs:
                    raw = bytearray(pr[1])
                    for i in range(len(raw) // 32):
                        ph, = struct.unpack_from("<Q", raw, 32 * i)
                        sz, fl = struct.unpack_from("<II", raw, 32 * i + 24)
                        if fl == OS_LOG_SEGMENT and base <= ph and ph + sz <= base + _size:
                            struct.pack_into("<Q", raw, 32 * i, _dst + (ph - base))
                            notes.append(f"{kp} __OS_LOG 0x{ph:x} -> 0x{_dst + (ph - base):x} "
                                         f"(RTBuddySeg copy)")
                        elif fl == OS_LOG_SEGMENT and ph < phys_base:
                            notes.append(f"WARNING: {kp} __OS_LOG 0x{ph:x}+0x{sz:x} is below "
                                         f"physBase and outside RTBuddySeg")
                    pr[1] = bytes(raw)

    new_adt = adt_build(root)
    adt_grew = len(new_adt) > len(adt)  # the tree itself, before the page padding below
    # iBoot hands the device tree over as whole 16 KiB pages (the live boot_args length is
    # 0x68000), SPTM forwards that length to cL4 unchanged (handoff +0x40), and cL4 maps the ADT
    # as a region that must be page-sized (an unpadded tree trips an assert in it). Pad to match
    # iBoot; the tree parsers stop at the structure's end.
    new_adt += bytes(round_page(len(new_adt)) - len(new_adt))
    if len(new_adt) > adt_size_rounded:
        raise ValueError("edited ADT outgrew its reserved space")

    # Virtual placement. The stock-XNU case keeps the fixture's convention:
    # virtBase = BootKC virtlo with the low 36 bits cleared, moved up by how far
    # the managed window starts above the DRAM base.
    virt_base = (xi.virtlo & ~L2_INDEX_MASK) + (phys_base - dram_base)
    adt_va = adt_pa - phys_base + virt_base
    top = blob_end
    if not (phys_base <= txm_pa and blob_end <= phys_base + mem_size):
        raise ValueError(
            f"blob [0x{txm_pa:x}, 0x{blob_end:x}) does not fit managed RAM "
            f"[0x{phys_base:x}, 0x{phys_base + mem_size:x})")
    ba = build_boot_args(virt_base, phys_base, mem_size, top, adt_va, len(new_adt), cmdline,
                         mem_size_actual, boot_flags, diag_pa=diag_pa)
    if diag_pa:
        notes.append("boot_args revision 3: SPTM diagnostic record at %#x, %#x bytes"
                     % (diag_pa, BOOT_ARGS_DIAG_SIZE))

    return Plan(regions=regions, loads=loads, boot_args=ba, boot_args_pa=boot_args_pa,
                diag_pa=diag_pa, adt=new_adt, adt_pa=adt_pa, adt_grew=adt_grew, sptm_pa=sptm_pa,
                sptm_entry_pa=vtop(si.entry), txm_pa=txm_pa, virt_base=virt_base,
                phys_base=phys_base, mem_size=mem_size, top_of_kernel_data=top,
                blob_start=txm_pa, blob_end=blob_end, notes=notes)
