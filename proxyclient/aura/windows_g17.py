# SPDX-License-Identifier: MIT
"""Reservation-only G17 handoff. No device startup or firmware writes.

The firmware profile follows public Aura 671c0cefdb39b58a19933c6fe254efb503d16212
src/kboot_gpu.c and Linux e50ba38b24140fc0751b811ea093c8f843bcec76 asahi/m5.
Addresses always come from this boot's ADT, never from a captured device.
"""
import hashlib
import struct
import zlib

PROPERTY = "aura,windows-g17-preserve-v1"
POLICY = "j813-preserve-v1"
SIZE = 304
FIRMWARE = b"M5-RTKit-3255.160.4.release"
BUILD = b"RTKit-3255.160.4.release"
NAMES = ("gfx-handoff", "gfx-shared-l2-region", "gfx-shared-region", "gpu-region",
         "gfx-data", "gfx1-data", "gfx-text", "gfx1-text")
TEXT_PROFILES = (("gfx-asc", 0x4c000, 0x45d09, 0x45dec),
                 ("gfx1-asc", 0x48000, 0x45841, 0x45924))


def require(value, message):
    if not value:
        raise ValueError("G17 preservation: " + message)


def raw_property(node, name):
    from .adt import build_prop
    require(name in node._properties and name in node._types, "missing " + name)
    kind, template = node._types[name]
    require(not template, "template property " + name)
    return build_prop(node._path, name, node._properties[name], t=kind)


def scalar(node, name, width):
    raw = raw_property(node, name)
    require(len(raw) == width, "wrong property width: " + name)
    return int.from_bytes(raw, "little")


def bounds(base, size, label):
    require(isinstance(base, int) and not isinstance(base, bool)
            and isinstance(size, int) and not isinstance(size, bool) and 0 < base < (1 << 42)
            and size > 0 and base + size <= (1 << 42)
            and (base | size) & 0x3fff == 0, "invalid span: " + label)
    return base + size


def observe(adt, args):
    """Validate current source identity and all eight spans before any TEXT read."""
    chosen = adt["/chosen"]
    require((scalar(chosen, "chip-id", 4), scalar(chosen, "board-id", 4),
             raw_property(adt, "model")) == (0x8142, 0x24, b"Mac17,3\0"),
            "requires exact J813 chip, board and model")
    dram_base = args.phys_base & ~0xffffffff
    dram_end = bounds(dram_base, args.mem_size_actual, "installed DRAM")
    require(dram_base <= args.phys_base < args.phys_base + args.mem_size <= dram_end,
            "BootArgs usable RAM is outside installed DRAM")
    sgx = adt["/arm-io/sgx"]
    regions = [{"name": name, "base": scalar(sgx, name + "-base", 8),
                "size": scalar(sgx, name + "-size", 8)} for name in NAMES[:6]]
    segments = []
    for index, (role, text_size, _, _) in enumerate(TEXT_PROFILES):
        node = adt["/arm-io/" + role]
        require(raw_property(node, "segment-names") == b"__TEXT;__DATA\0",
                "unexpected segment names for " + role)
        raw = raw_property(node, "segment-ranges")
        require(len(raw) == 64, "expected exactly two segments for " + role)
        text, data = [struct.unpack_from("<QQQII", raw, offset) for offset in (0, 32)]
        require(text[3] == text_size, "unexpected TEXT size for " + role)
        require((data[0], data[3]) == (regions[4 + index]["base"], regions[4 + index]["size"]),
                "DATA segment disagrees with SGX for " + role)
        regions.append({"name": NAMES[6 + index], "base": text[0], "size": text_size})
        # Bind even the segment fields that reservation-only policy does not use.
        segments.append(raw.hex())
    require(regions[2]["size"] >= 0x44000, "page-table region is too short")
    require(regions[4]["size"] == 0x134000 and regions[5]["size"] == 0x130000,
            "unexpected DATA sizes")
    for region in regions:
        end = bounds(region["base"], region["size"], region["name"])
        require(dram_base <= region["base"] < end <= dram_end, "region outside installed DRAM")
    ordered = sorted(regions, key=lambda region: region["base"])
    require(all(a["base"] + a["size"] <= b["base"] for a, b in zip(ordered, ordered[1:])),
            "firmware regions overlap")
    return {"dram_base": dram_base, "dram_end": dram_end, "regions": regions,
            "segments": segments}


def classify(base, size, guest_base, guest_end):
    if guest_base <= base and base + size <= guest_end:
        return 1
    if base + size <= guest_base:
        return 2
    if base >= guest_end:
        return 3
    raise ValueError("G17 preservation: region crosses a guest RAM boundary")


def validate_exclusions(source, guard, layout):
    guest_base, guest_end = layout["heap_top"], guard["ram_end"]
    require((guest_base | guest_end) & 0x3fff == 0, "guest RAM bounds are not 16-KiB aligned")
    require(source["dram_base"] == guest_base & ~0xffffffff,
            "guest relocation changes installed DRAM envelope")
    require(source["dram_base"] <= guest_base < guest_end <= source["dram_end"],
            "guest RAM is outside installed DRAM")
    occupied = [*guard["occupied"], *guard["reservations"],
                {"name": "raw image and relocated metadata", "base": guest_base,
                 "size": layout["image_end"] - guest_base}]
    classes = []
    for region in source["regions"]:
        start, end = region["base"], region["base"] + region["size"]
        classes.append(classify(start, region["size"], guest_base, guest_end))
        for other in occupied:
            require(end <= other["base"] or other["base"] + other["size"] <= start,
                    region["name"] + " overlaps " + other["name"])
    return classes


def admit(source, guard, read_text):
    """Read only the bounded TEXT spans, then encode the complete Mu contract."""
    classes = validate_exclusions(source, guard, guard["layout"])
    firmware = []
    for region, (role, size, gkts, ecap) in zip(source["regions"][6:], TEXT_PROFILES):
        image = read_text(region["base"], size)
        require(isinstance(image, bytes) and len(image) == size, "incomplete TEXT read for " + role)
        require(BUILD in image, "unknown firmware build for " + role)
        for offset, tag in ((gkts, b"GKTS"), (ecap, b"ECAP")):
            require(image[offset:offset + 8] == tag + struct.pack("<I", 8),
                    "unknown firmware TLV layout for " + role)
        # Evidence only: per-boot TLV patches make this unsuitable as an allowlist.
        firmware.append({"role": role, "sha256": hashlib.sha256(image).hexdigest()})
    record = bytearray(struct.pack("<4sHHIIIIQQQQ32s16sII", b"AG17", 1, SIZE,
                                  0x8142, 0x24, 8, 1, guard["layout"]["heap_top"],
                                  guard["ram_end"], source["dram_base"], source["dram_end"],
                                  FIRMWARE, b"Mac17,3", 0, 0))
    for tag, (region, classification) in enumerate(zip(source["regions"], classes), 1):
        record.extend(struct.pack("<IIQQ", tag, classification, region["base"], region["size"]))
    struct.pack_into("<I", record, 104, zlib.crc32(record))
    return bytes(record), {"policy": POLICY, "firmware": firmware,
                           "descriptor_sha256": hashlib.sha256(record).hexdigest(),
                           "regions": [dict(region, classification=c)
                                       for region, c in zip(source["regions"], classes)]}


def verify_record(record, source, guard):
    require(isinstance(record, bytes) and len(record) == SIZE, "invalid record length")
    header = struct.unpack_from("<4sHHIIIIQQQQ32s16sII", record)
    require(header[:7] == (b"AG17", 1, SIZE, 0x8142, 0x24, 8, 1)
            and header[7:11] == (guard["layout"]["heap_top"], guard["ram_end"],
                                 source["dram_base"], source["dram_end"])
            and header[11] == FIRMWARE.ljust(32, b"\0")
            and header[12] == b"Mac17,3".ljust(16, b"\0") and header[14] == 0,
            "record header disagrees with admission")
    require(zlib.crc32(record[:104] + bytes(4) + record[108:]) == header[13], "record CRC mismatch")
    classes = validate_exclusions(source, guard, guard["layout"])
    for i, (region, classification) in enumerate(zip(source["regions"], classes)):
        require(struct.unpack_from("<IIQQ", record, 112 + i * 24) ==
                (i + 1, classification, region["base"], region["size"]), "record region mismatch")


def update_adt(hv):
    from construct import Bytes
    chosen = hv.adt["/chosen"]
    chosen._properties.pop(PROPERTY, None)
    chosen._types.pop(PROPERTY, None)
    require(hv.windows_g17 in ("disabled", POLICY), "unknown policy")
    if hv.windows_g17 == "disabled":
        return
    require(observe(hv.adt, hv.u.ba) == hv.windows_g17_source, "source ADT changed after admission")
    verify_record(hv.windows_g17_record, hv.windows_g17_source, hv.windows_memory_guard)
    chosen._properties[PROPERTY] = hv.windows_g17_record
    chosen._types[PROPERTY] = (Bytes(SIZE), False)


def verify_adt(hv):
    chosen = hv.adt["/chosen"]
    if hv.windows_g17 == "disabled":
        require(PROPERTY not in chosen._properties, "disabled policy inherited a marker")
        return
    require(raw_property(chosen, PROPERTY) == hv.windows_g17_record, "published marker changed")
    require(observe(hv.adt, hv.u.ba) == hv.windows_g17_source, "source ADT changed after admission")
    verify_record(hv.windows_g17_record, hv.windows_g17_source, hv.windows_memory_guard)
