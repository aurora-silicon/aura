#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Write synthetic seeds for the fuzz targets of tests/fuzz/ into <dir>.

    gen_seeds.py adt <dir>     seed ADTs for adt_fuzz.c
    gen_seeds.py dt <dir>      seed ADT + device tree pairs for dt_fuzz.c

Every value is made up. The ADTs only follow the layout aura's reader walks (a node header of
property and child counts, 36-byte property headers padded to 4 bytes, children after the
properties) and the device trees the flattened device tree format, and both use the node and
property names aura looks up, so that the fuzzer starts deep.
"""
import struct
import sys
import zlib
from pathlib import Path


def u32(*v):
    return struct.pack("<%dI" % len(v), *v)


def u64(*v):
    return struct.pack("<%dQ" % len(v), *v)


def cstr(*s):
    return b"".join(x.encode() + b"\0" for x in s)


class Node:
    def __init__(self, name, props=None, children=None):
        self.props = [("name", cstr(name))] + list((props or {}).items())
        self.children = children or []

    def encode(self):
        out = u32(len(self.props), len(self.children))
        for name, data in self.props:
            out += name.encode().ljust(32, b"\0") + u32(len(data))
            out += data + b"\0" * (-len(data) % 4)
        for child in self.children:
            out += child.encode()
        return out


def cells(ac, sc):
    return {"#address-cells": u32(ac), "#size-cells": u32(sc)}


def dev(name, compat, reg, extra=None):
    props = {"compatible": cstr(*compat), "reg": reg, "AAPL,phandle": u32(zlib.crc32(name.encode()) & 0xFFFF)}
    props.update(extra or {})
    return Node(name, props)


def mac(ac=2, sc=2, ranges=True, cpus=4):
    arm_io_props = {"compatible": cstr("arm-io,t8103", "arm-io"), "device_type": cstr("t8103-io")}
    arm_io_props.update(cells(2, 2))
    if ranges:
        # child address, parent address, size
        arm_io_props["ranges"] = u64(0x0, 0x200000000, 0x100000000, 0x300000000, 0x500000000, 0x10000000)
    mapper = Node("mapper-disp0", {"reg": u32(1), "compatible": cstr("iommu-mapper")})
    dart = Node("dart-disp0", dict(cells(1, 0), compatible=cstr("dart,t8020"), reg=u64(0x3B304000, 0x4000)),
                [mapper])
    arm_io = Node("arm-io", arm_io_props, [
        dev("pmgr", ["pmgr1,t8103"], u64(0x3B700000, 0x14000, 0x3B000000, 0x8000), {"ps-regs": u32(0, 0, 0x4000)}),
        dev("aic", ["aic,1"], u64(0x3B100000, 0x8000), {"ipid-mask": u32(0xFFFFFFFF)}),
        dev("uart0", ["uart-1,samsung"], u64(0x35200000, 0x4000), {"interrupts": u32(605)}),
        dev("i2c0", ["i2c,t8101"], u64(0x35010000, 0x4000), {}),
        dev("dart@1", ["dart,t6000"], u64(0x1000, 0x100), {}),
        dart,
        dev("mcc", ["mcc,t8103"], u64(0x200000000, 0x1000), {"dram-type": cstr("LPDDR5")}),
        dev("usb-drd0", ["usb-drd,t8103"], u64(0x382280000, 0x100000)),
        dev("usb-drd1", ["usb-drd,t8103"], u64(0x502280000, 0x100000)),
        dev("acio0", ["acio"], u64(0x381f00000, 0x4000, 0x381000000, 0x4000)),
        Node("gpio0", {"compatible": cstr("gpio,t8101"), "reg": u64(0x3C100000, 0x4000),
                       "#gpio-pins": u32(0xC8), "AAPL,phandle": u32(0x80), "role": cstr("AP")}),
        Node("i2c1", {"compatible": cstr("i2c,t8101"), "reg": u64(0x35014000, 0x4000)}, [
            Node("audio-speaker", {
                "compatible": cstr("audio-control,sn012776"), "reg": u32(0x38, 400000, 0, 0),
                "AAPL,phandle": u32(0x10), "bop-config": bytes(range(24)),
                "amp-dcblocker-config": u32(0x15A), "speaker-config": bytes(range(0x40, 0x4C)),
                "amp-tx-zd-config": bytes(range(0x50, 0x68)), "speaker1": u32(0x11),
                "speaker2": u32(0x12), "find-speaker-id": u32(1), "valid-speaker-ids": u32(0, 3),
                "function-speaker-id0": u32(0x80) + b"OIPG" + u32(0x34, 0x100),
                "function-speaker-id1": u32(0x80) + b"OIPG" + u32(0x35, 0x100)}),
            Node("audio-speaker-a", {"compatible": cstr("audio-control,sn012776"),
                                     "reg": u32(0x39, 400000, 0, 0), "AAPL,phandle": u32(0x11)}),
            Node("audio-speaker-b", {"compatible": cstr("audio-control,sn012776"),
                                     "reg": u32(0x3A, 400000, 0, 0), "AAPL,phandle": u32(0x12)}),
        ]),
    ])
    cpu_nodes = [
        Node("cpu%d" % i, {
            "compatible": cstr("apple,icestorm" if i < 2 else "apple,firestorm", "ARM,v8"),
            "reg": u32(i if i < 2 else 0x10100 + i), "cpu-id": u32(i), "cluster-id": u32(i // 2),
            "device_type": cstr("cpu"), "state": cstr("running"),
            "cpu-impl-reg": u64(0x210050000 + 0x10000 * i, 0x1000),
        }) for i in range(cpus)
    ]
    cpus_node = Node("cpus", dict(cells(1, 0), **{"max_cpus": u32(cpus)}), cpu_nodes)
    memory_map = Node("memory-map", {
        "TrustCache": u64(0x800000000, 0x4000), "RAMDisk": u64(0x801000000, 0x100000),
    })
    chosen = Node("chosen", {
        "firmware-version": cstr("mBoot-18000.101.7"), "system-firmware-version": cstr("mBoot-18000.101.7"),
        "dram-vendor-id": u32(1), "chip-id": u32(0x8103), "board-id": u32(0x24),
        "random-seed": bytes(range(64)), "bootp-response": b"\x00" * 3,
        "dram-type": cstr("LPDDR5"), "dram-vendor": cstr("Vendor"),
    }, [memory_map, Node("carveout-memory-map", {"region-id-24": u64(0x10000000000, 0x1000000)})])
    product = Node("product", {"product-name": cstr("Synthetic Mac"), "product-description": cstr("Test"),
                               "product-soc-name": cstr("M0")})
    root_props = {
        "compatible": cstr("J000AP", "Mac00,0", "AppleARM"), "model": cstr("Mac00,0"),
        "target-type": cstr("J000"), "serial-number": b"\0" * 32, "clock-frequency": u32(24000000),
        "device_type": cstr("bootrom"), "manufacturer": cstr("Vendor Inc."),
        "model-number": b"Z0000" + b"\0" * 27, "platform-name": b"t8103" + b"\0" * 27,
        "region-info": b"LL/A" + b"\0" * 28, "regulatory-model-number": b"A0000" + b"\0" * 27,
        "mlb-serial-number": b"\0" * 32,
    }
    root_props.update(cells(ac, sc))
    return Node("device-tree", root_props, [arm_io, cpus_node, chosen, product,
                                            Node("defaults", {"no-ranges": b""}),
                                            Node("filesystems", {"fstab": cstr("x")})])


def edge_cases():
    yield "minimal", Node("device-tree")
    yield "deep", _deep(12)
    yield "wide", Node("device-tree", cells(1, 1), [
        Node("n%d" % i, {"reg": u32(i * 0x100, 0x100)}) for i in range(40)])
    yield "dup-names", Node("device-tree", cells(1, 1), [
        Node("a", {"reg": u32(0, 4)}), Node("a@1", {"reg": u32(4, 4)}),
        Node("a", {"reg": u32(8, 4)}, [Node("b")])])
    yield "big-prop", Node("device-tree", {"blob": bytes(range(256)) * 16})
    yield "odd-sizes", Node("device-tree", {"one": b"\x01", "three": b"abc", "five": b"hello",
                                            "empty": b"", "nonul": b"abcd"})


def _deep(depth):
    node = Node("leaf", dict(cells(1, 1), reg=u32(0x10, 0x10), ranges=u32(0, 0x1000, 0x100)))
    for i in range(depth):
        node = Node("d%d" % i, dict(cells(1, 1), reg=u32(0, 0x1000), ranges=u32(0, 0x1000 * i, 0x10000)),
                    [node])
    return Node("device-tree", cells(1, 1), [node])


def seeds():
    yield "mac-2-2", mac()
    yield "mac-1-1", mac(1, 1)
    yield "mac-noranges", mac(ranges=False, cpus=2)
    yield "mac-8cpu", mac(cpus=8)
    yield from edge_cases()


class FNode:
    """A flattened device tree node: properties are (name, bytes), values big-endian"""

    def __init__(self, name, props=None, children=None):
        self.name = name
        self.props = list((props or {}).items())
        self.children = children or []


def be(*v):
    return struct.pack(">%dI" % len(v), *v)


def be64(*v):
    return struct.pack(">%dQ" % len(v), *v)


def dtb(root):
    strings = bytearray()
    offsets = {}
    body = bytearray()

    def name_off(n):
        if n not in offsets:
            offsets[n] = len(strings)
            strings.extend(n.encode() + b"\0")
        return offsets[n]

    def emit(node):
        name = node.name.encode() + b"\0"
        body.extend(be(1) + name + b"\0" * (-len(name) % 4))
        for k, v in node.props:
            body.extend(be(3, len(v), name_off(k)) + v + b"\0" * (-len(v) % 4))
        for c in node.children:
            emit(c)
        body.extend(be(2))

    emit(root)
    body.extend(be(9))
    rsv = be64(0, 0)
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_strings = off_struct + len(body)
    total = off_strings + len(strings)
    header = be(0xD00DFEED, total, off_struct, off_strings, off_rsv, 17, 16, 0, len(strings),
                len(body))
    return header + rsv + bytes(body) + bytes(strings)


def payload_dt(soc_cells=2, missing_usb=2, cpus=4):
    cpu_nodes = [
        FNode("cpu@%d" % i, {"device_type": b"cpu\0", "compatible": b"apple,icestorm\0",
                             "reg": be(0, i), "operating-points-v2": be(0x20 + i // 2),
                             "next-level-cache": be(0x30 + i // 2), "phandle": be(0x40 + i)})
        for i in range(cpus)]
    opps = [FNode("opp-table-%d" % c, {"compatible": b"operating-points-v2\0", "phandle": be(0x20 + c)},
                  [FNode("opp%02d" % j, {"opp-hz": be64(600000000 * (j + 1)), "opp-level": be(j)})
                   for j in range(3)]) for c in range(2)]
    l2 = [FNode("l2-cache-%d" % c, {"compatible": b"cache\0", "cache-level": be(2),
                                    "cache-size": be(0x400000 << c), "phandle": be(0x30 + c)})
          for c in range(2)]
    dram = FNode("dram-opp", {"compatible": b"operating-points-v2-apple-dram\0"},
                 [FNode("opp%d" % j, {"apple,data-rate": be64(3200000000 * (j + 1))}) for j in range(2)])
    soc_props = {"compatible": b"simple-bus\0", "#address-cells": be(soc_cells),
                 "#size-cells": be(soc_cells), "phandle": be(0x50)}
    if soc_cells == 2:
        soc_props["ranges"] = be(0x3, 0x0, 0x3, 0x0, 0x2, 0x0)
        reg = lambda a, sz: be(a >> 32, a & 0xFFFFFFFF, 0, sz)
    else:
        soc_props["ranges"] = be(0x0, 0x3, 0x80000000, 0x40000000)
        reg = lambda a, sz: be(a & 0xFFFFFFFF, sz)
    usb = []
    for i, a in enumerate([0x382280000, 0x502280000] + [0x602280000 + 0x1000000 * k for k in range(missing_usb)]):
        usb.append(FNode("usb@%x" % a, {"compatible": b"apple,dwc3\0", "reg": reg(a, 0x100000),
                                       "iommus": be(0x60, 1, 0x61, 1), "phys": be(0x70, 0, 0x70, 1)}))
    soc = FNode("soc", soc_props, [
        FNode("iommu@382f00000", {"reg": reg(0x382f00000, 0x4000), "#iommu-cells": be(1), "phandle": be(0x60)}),
        FNode("iommu@382f80000", {"reg": reg(0x382f80000, 0x4000), "#iommu-cells": be(1), "phandle": be(0x61)}),
        FNode("phy@383000000", {"reg": reg(0x383000000, 0x4000), "#phy-cells": be(1), "phandle": be(0x70)}),
        FNode("i2c@235010000", {"reg": reg(0x235010000, 0x4000), "#address-cells": be(1),
                                "#size-cells": be(0)}, []),
        FNode("i2c@235014000", {"reg": reg(0x235014000, 0x4000), "#address-cells": be(1),
                                "#size-cells": be(0)},
              [FNode("codec@%x" % a, {"compatible": b"ti,sn012776\0ti,tas2764\0", "reg": be(a),
                                       "apple,bop-config": b"\xee"}) for a in (0x38, 0x39, 0x3A)]),
        FNode("cio@381f00000", {"reg": reg(0x381f00000, 0x4000)}),
    ] + usb)
    return FNode("", {"compatible": b"apple,j000\0apple,t8103\0apple,arm-platform\0",
                      "model": b"Synthetic Mac\0", "chassis-type": b"laptop\0",
                      "#address-cells": be(2), "#size-cells": be(2)}, [
        FNode("chosen", {"bootargs": b"\0", "asahi,m1n1-stage1-version": b"1.0\0"}),
        FNode("clock-ref", {"compatible": b"fixed-clock\0", "clock-frequency": be(24000000),
                            "#clock-cells": be(0)}),
        FNode("cpus", {"#address-cells": be(2), "#size-cells": be(0)}, cpu_nodes + l2),
        FNode("memory@800000000", {"device_type": b"memory\0", "reg": be(8, 0, 2, 0)}),
        dram, soc] + opps)


def dt_pair(adt_node, dt_node, flags=0):
    a = adt_node.encode()
    return struct.pack("<IB", len(a), flags) + a + dtb(dt_node)


def dt_seeds():
    yield "mac", dt_pair(mac(), payload_dt())
    yield "mac-sme", dt_pair(mac(), payload_dt(), 1 | 2)
    yield "mac-1cell", dt_pair(mac(1, 1), payload_dt(soc_cells=1))
    yield "mac-acio1", dt_pair(mac(ranges=False), payload_dt(missing_usb=6), 4)
    yield "minimal", dt_pair(Node("device-tree"), FNode("", {}))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("adt", "dt"):
        raise SystemExit(__doc__)
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    if sys.argv[1] == "adt":
        for name, node in seeds():
            (out / ("%s.adt" % name)).write_bytes(node.encode())
    else:
        for name, blob in dt_seeds():
            (out / ("%s.bin" % name)).write_bytes(blob)


if __name__ == "__main__":
    main()
