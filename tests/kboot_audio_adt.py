#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fixtures for tests/kboot_audio_host.c, in the binary ADT layout aura's ADT reader
(rust/src/adt.rs) walks.

    kboot_audio_adt.py scenario <name> <out.adt> <out.dts>
    kboot_audio_adt.py scenarios
    kboot_audio_adt.py ioreg <IODeviceTree plist> <out.adt>

"scenario" writes a synthetic ADT and device tree for one test case: every value in them is made
up for the test. "ioreg" turns a Mac's `ioreg -a -p IODeviceTree` plist into a binary ADT, so
that aura's code can run on that Mac's own ADT; the plist and the ADT stay outside the tree.
"""
import plistlib
import struct
import sys

# Made-up configuration blobs: the code under test copies bytes and never reads them
BOP = bytes(range(0xA0, 0xA0 + 24))
DCB = bytes([0x5A, 0x01, 0x00, 0x00])
# One four-byte row per amp of a three-amp group, in the group's order
ROWS = bytes([0xC0, 0xC1, 0xC2, 0xC3, 0xD0, 0xD1, 0xD2, 0xD3, 0xE0, 0xE1, 0xE2, 0xE3])
# One eight-byte row per amp
ZD = bytes(range(0x10, 0x28))


def u32(v):
    return struct.pack("<I", v)


def cstr(s):
    return s.encode() + b"\0"


class Node:
    def __init__(self, name, props=None, children=None):
        self.name = name
        self.props = [("name", cstr(name))] + list((props or {}).items())
        self.children = children or []

    def encode(self):
        out = struct.pack("<II", len(self.props), len(self.children))
        for name, data in self.props:
            if len(name) >= 32:
                raise SystemExit("property name %s too long" % name)
            out += name.encode().ljust(32, b"\0") + struct.pack("<I", len(data))
            out += data + b"\0" * (-len(data) % 4)
        for child in self.children:
            out += child.encode()
        return out


def amp(name, addr, phandle, extra=None, compat="audio-control,sn012776"):
    props = {
        "compatible": cstr(compat),
        "reg": u32(addr) + u32(400000) + u32(0) + u32(0),
        "AAPL,phandle": u32(phandle),
    }
    props.update(extra or {})
    return Node(name, props)


def adt(buses):
    return Node("device-tree", {"compatible": cstr("J000AP\0Apple")},
                [Node("arm-io", {"compatible": cstr("arm-io")}, buses)])


def dts(amps, stale=None, compat='"ti,sn012776", "ti,tas2764"'):
    """A device tree with one amp node per (bus, addr); stale: {(bus, addr): props}"""
    out = ["/dts-v1/;", "/ {", "\t#address-cells = <2>;", "\t#size-cells = <2>;"]
    buses = sorted({b for b, _ in amps})
    for b in buses:
        out += ["\ti2c@%x {" % (0x100000 * (b + 1)), "\t\treg = <0x0 0x%x 0x0 0x4000>;" %
                (0x100000 * (b + 1)), "\t\t#address-cells = <1>;", "\t\t#size-cells = <0>;"]
        for bb, a in amps:
            if bb != b:
                continue
            out += ["\t\tcodec@%x {" % a, "\t\t\tcompatible = %s;" % compat,
                    "\t\t\treg = <0x%x>;" % a]
            for k, v in (stale or {}).get((b, a), {}).items():
                out.append("\t\t\t%s = [%s];" % (k, " ".join("%02x" % x for x in v)))
            out.append("\t\t};")
        out.append("\t};")
    out += ["\tother@0 { compatible = \"vendor,other\"; reg = <0x0 0x0 0x0 0x1>; };", "};", ""]
    return "\n".join(out)


GROUP3 = {"bop-config": BOP, "amp-dcblocker-config": DCB, "speaker-config": ROWS,
          "amp-tx-zd-config": ZD, "speaker1": u32(0x11), "speaker2": u32(0x12)}
STALE = {"apple,amp-dcblocker-config": b"\xee\xee", "apple,bop-config": b"\xee",
         "apple,speaker-config": b"\xee\xee\xee\xee", "apple,amp-tx-zd-config": b"\xee" * 8}
# Six amps over two buses, as the 14"/16" MacBook Pros have them: the first amp and speaker1..5
SIX = [(0, 0x38), (0, 0x39), (0, 0x3a), (1, 0x3b), (1, 0x3c), (1, 0x3d)]
ROWS6 = bytes(b for i in range(6) for b in (0x60 + i, 0x70 + i, 0x80 + i, 0x90 + i))
ZD6 = bytes(range(0x30, 0x30 + 48))
TAS_ROWS = bytes([0x06, 0x03, 0x22, 0x24, 0x08, 0x04, 0x26, 0x28])


def scenario(name):
    """(ADT, DTS) for one scenario"""
    three = [(0, 0x40), (1, 0x41), (0, 0x42)]
    group = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, GROUP3),
                                amp("audio-speaker-b", 0x42, 0x12)]),
             Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
    if name == "group":
        return adt(group), dts(three, {(1, 0x41): STALE})
    if name == "no-dcblocker":
        g = dict(GROUP3)
        del g["amp-dcblocker-config"]
        buses = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                                    amp("audio-speaker-b", 0x42, 0x12)]),
                 Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
        return adt(buses), dts(three, {(1, 0x41): STALE})
    if name == "six-amps":
        # group order: 0x38, then 0x3b, 0x39, 0x3c, 0x3a, 0x3d (speaker1..5), as on those Macs
        order = [0x38, 0x3b, 0x39, 0x3c, 0x3a, 0x3d]
        g = {"bop-config": BOP, "amp-dcblocker-config": DCB, "speaker-config": ROWS6,
             "amp-tx-zd-config": ZD6}
        for k, a in enumerate(order[1:], 1):
            g["speaker%d" % k] = u32(0x20 + k)
        nodes = {0: [], 1: []}
        for k, a in enumerate(order):
            bus = 0 if a <= 0x3a else 1
            nodes[bus].append(amp("audio-speaker" if k == 0 else "audio-speaker-%d" % k, a,
                                  0x20 + k, g if k == 0 else None))
        return adt([Node("i2c0", {}, nodes[0]), Node("i2c1", {}, nodes[1])]), dts(SIX)
    if name == "short-speaker-config":
        g = dict(GROUP3)
        g["speaker-config"] = ROWS[:8]
        g["amp-tx-zd-config"] = ZD[:16]
        buses = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                                    amp("audio-speaker-b", 0x42, 0x12)]),
                 Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
        return adt(buses), dts(three, {(1, 0x41): STALE})
    if name == "single-amp":
        one = {"bop-config": BOP}
        return adt([Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, one)])]), dts([(0, 0x40)])
    if name == "amp-missing-in-dt":
        return adt(group), dts([(0, 0x40), (0, 0x42)], {(0, 0x42): STALE})
    if name == "amp-not-unique-in-dt":
        return adt(group), dts(three + [(1, 0x42)])
    if name == "dangling-phandle":
        g = dict(GROUP3)
        g["speaker2"] = u32(0x77)
        buses = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                                    amp("audio-speaker-b", 0x42, 0x12)]),
                 Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
        return adt(buses), dts(three, {(1, 0x41): STALE})
    if name == "no-amps":
        return adt([Node("i2c0", {}, [])]), dts(three, {(1, 0x41): STALE})
    if name in ("tas-group", "tas-no-speaker-config"):
        g = {"speaker1": u32(0x32)}
        if name == "tas-group":
            g["speaker-config"] = TAS_ROWS
        amps = [(0, 0x51), (0, 0x54)]
        buses = [Node("i2c0", {}, [
            amp("audio-speaker", 0x51, 0x31, g, "audio-control,tas5770"),
            amp("audio-speaker-a", 0x54, 0x32, compat="audio-control,tas5770"),
        ])]
        stale = {a: {"apple,speaker-config": b"\xee" * 4} for a in amps}
        return adt(buses), dts(amps, stale, '"ti,tas5770l", "ti,tas2770"')
    raise SystemExit("unknown scenario %s" % name)


SCENARIOS = ["group", "no-dcblocker", "six-amps", "short-speaker-config", "single-amp", "amp-missing-in-dt", "amp-not-unique-in-dt",
             "dangling-phandle", "no-amps", "tas-group", "tas-no-speaker-config"]


def from_ioreg(path):
    root = plistlib.load(open(path, "rb"))

    def find(n):
        if n.get("IORegistryEntryName") == "device-tree":
            return n
        for c in n.get("IORegistryEntryChildren", []):
            r = find(c)
            if r is not None:
                return r
        return None

    def conv(n):
        node = Node.__new__(Node)
        node.props = [(k, v) for k, v in n.items() if isinstance(v, bytes) and len(k) < 32]
        if not any(k == "name" for k, _ in node.props):
            node.props.insert(0, ("name", cstr(n.get("IORegistryEntryName", "?"))))
        node.children = [conv(c) for c in n.get("IORegistryEntryChildren", [])]
        return node

    dt = find(root)
    if dt is None:
        raise SystemExit("%s: no device-tree plane" % path)
    return conv(dt)


def main():
    if len(sys.argv) == 2 and sys.argv[1] == "scenarios":
        print(" ".join(SCENARIOS))
    elif len(sys.argv) == 5 and sys.argv[1] == "scenario":
        a, d = scenario(sys.argv[2])
        open(sys.argv[3], "wb").write(a.encode())
        open(sys.argv[4], "w").write(d)
    elif len(sys.argv) == 4 and sys.argv[1] == "ioreg":
        open(sys.argv[3], "wb").write(from_ioreg(sys.argv[2]).encode())
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
