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


def amp(name, addr, phandle, extra=None):
    props = {
        "compatible": cstr("audio-control,sn012776"),
        "reg": u32(addr) + u32(400000) + u32(0) + u32(0),
        "AAPL,phandle": u32(phandle),
    }
    props.update(extra or {})
    return Node(name, props)


# A made-up AP GPIO controller: /arm-io maps its child address 0 to GPIO_MMIO
GPIO_PHANDLE = 0x80
GPIO_MMIO = 0x200000000
GPIO_REG = 0x1000000
GPIO_NPINS = 0xC8
GPIO_BASE = GPIO_MMIO + GPIO_REG


def gpio_controller(phandle=GPIO_PHANDLE, compat="gpio,t8101"):
    return Node("gpio0", {
        "compatible": cstr(compat),
        "reg": struct.pack("<QQ", GPIO_REG, 0x4000),
        "#gpio-pins": u32(GPIO_NPINS),
        "AAPL,phandle": u32(phandle),
        "role": cstr("AP"),
    })


def adt(buses, extra=None):
    arm_io = {"compatible": cstr("arm-io"), "#address-cells": u32(2), "#size-cells": u32(2),
              "ranges": struct.pack("<QQQ", 0, GPIO_MMIO, 0x100000000)}
    return Node("device-tree", {"compatible": cstr("J000AP\0Apple"), "#address-cells": u32(2),
                                "#size-cells": u32(2)},
                [Node("arm-io", arm_io, [gpio_controller()] + (extra or []) + buses)])


def dts(amps, stale=None, chosen="", compatible=None, card=None):
    """A device tree with one ti,sn012776 node per (bus, addr); stale: {(bus, addr): props}

    card: (codec addrs in the dai-link's order, {codec index: TDM idle mask}) for a sound card
    whose speaker dai-link lists those amps, as macaudio's trees do."""
    out = ["/dts-v1/;", "/ {", "\t#address-cells = <2>;", "\t#size-cells = <2>;",
           "\tchosen { %s};" % chosen]
    if compatible:
        out.insert(2, "\tcompatible = %s;" % ", ".join('"%s"' % c for c in compatible))
    if card:
        order, masks = card
        out += ["\tsound {", "\t\tdai-link@0 {", "\t\t\treg = <0>;"]
        for i, m in sorted(masks.items()):
            out += ['\t\t\tdai-tdm-idle-mode-%d = "zero";' % i,
                    "\t\t\tdai-tdm-slot-tx-idle-mask-%d = <0x%x>;" % (i, m)]
        bus = {a: b for b, a in reversed(amps)}
        out += ["\t\t\tcodec { sound-dai = %s; };" %
                ", ".join("<&amp_%x_%x>" % (bus[a], a) for a in order),
                "\t\t};", "\t};"]
    buses = sorted({b for b, _ in amps})
    for b in buses:
        out += ["\ti2c@%x {" % (0x100000 * (b + 1)), "\t\treg = <0x0 0x%x 0x0 0x4000>;" %
                (0x100000 * (b + 1)), "\t\t#address-cells = <1>;", "\t\t#size-cells = <0>;"]
        for bb, a in amps:
            if bb != b:
                continue
            out += ["\t\tamp_%x_%x: codec@%x {" % (b, a, a),
                    '\t\t\tcompatible = "ti,sn012776", "ti,tas2764";',
                    "\t\t\treg = <0x%x>;" % a, "\t\t\t#sound-dai-cells = <0>;"]
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


def strap(pin, flags=0x100, phandle=GPIO_PHANDLE, fourcc=b"GPIO"):
    return u32(phandle) + struct.pack(">I", struct.unpack(">I", fourcc)[0])[::-1] + u32(pin) + \
        u32(flags)


def speaker_group(straps, valid=(0, 3), find=1):
    g = dict(GROUP3)
    if find is not None:
        g["find-speaker-id"] = u32(find)
    if valid is not None:
        g["valid-speaker-ids"] = b"".join(u32(v) for v in valid)
    for i, st in enumerate(straps):
        if st is not None:
            g["function-speaker-id%d" % i] = st
    return [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                              amp("audio-speaker-b", 0x42, 0x12)]),
            Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]


THREE = [(0, 0x40), (1, 0x41), (0, 0x42)]
SPEAKER_ID_SCENARIOS = {
    # name: (straps, valid-speaker-ids, find-speaker-id, other ADT nodes, stale chosen)
    "speaker-id-3": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-0": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-not-valid": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-no-input-stage": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-output-pin": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-peripheral-pin": ([strap(0x34), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-inverted": ([strap(0x34, 0x0), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-upper-control-bytes": ([strap(0x34, 0x10100), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-not-gpio": ([strap(0x34, fourcc=b"pKW4"), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-bad-phandle": ([strap(0x34, phandle=0x99), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-pin-out-of-range": ([strap(GPIO_NPINS), strap(0x35)], (0, 3), 1, None, ""),
    "speaker-id-one-strap": ([None, strap(0x35)], (0, 2), 1, None, ""),
    "speaker-id-no-strap-resolves": ([strap(0x34, phandle=0x99), strap(0x35, phandle=0x99)], (0, 3),
                                     1, None, ""),
    "speaker-id-no-valid-ids": ([strap(0x34), strap(0x35)], None, 1, None, ""),
    "speaker-id-find-off": ([strap(0x34), strap(0x35)], (0, 3), 0, None, ""),
    "speaker-id-find-2": ([strap(0x34), strap(0x35)], (0, 3), 2, None, ""),
    "speaker-id-stale": ([], None, None, None, "asahi,speaker-id = <7>; "),
    "speaker-id-third-strap-ignored": ([strap(0x30), strap(0x31), strap(0x32)], (0, 1, 5), 1, None,
                                       ""),
}


def scenario(name):
    """(ADT, DTS) for one scenario"""
    if name in SPEAKER_ID_SCENARIOS:
        straps, valid, find, extra, chosen = SPEAKER_ID_SCENARIOS[name]
        return adt(speaker_group(straps, valid, find), extra), dts(THREE, chosen=chosen)
    three = [(0, 0x40), (1, 0x41), (0, 0x42)]
    group = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, GROUP3),
                                amp("audio-speaker-b", 0x42, 0x12)]),
             Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
    if name == "group":
        return adt(group), dts(three, {(1, 0x41): STALE})
    # The made-up rows Z0, Z1, Z2 of amps 0x40, 0x41, 0x42 as slot masks
    z0, z1, z2 = (int.from_bytes(ZD[8 * i:8 * i + 4], "little") for i in range(3))
    if name == "card-masks-agree":
        # As on the 14"/16" Pros: the card's masks are the rows (amp 0x41's on its first two
        # bytes only, all its mask spans); amp 0x42 has no mask
        return adt(group), dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1 & 0xffff}))
    if name == "card-mask-contradicts":
        # Amp 0x41's row disagrees with the card's mask: the card's mask applies to it alone
        return adt(group), dts(three, {(1, 0x41): STALE},
                               card=([0x40, 0x41, 0x42], {0: z0, 1: z1 ^ 0x100}))
    if name == "card-mask-contradicts-t8122":
        # The same on an M3 tree: decided by the masks, not by the SoC
        return adt(group), dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1 ^ 0x100}),
                               compatible=["apple,j613", "apple,t8122", "apple,arm-platform"])
    if name == "card-masks-agree-t8122":
        return adt(group), dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1}),
                               compatible=["apple,j613", "apple,t8122", "apple,arm-platform"])
    if name == "card-masks-two-links-disagree":
        # A second dai-link gives amp 0x40 another mask (one its row agrees with, on the two bytes
        # it spans): the card does not say one thing, so no row for it
        tree = dts(three, card=([0x40, 0x41, 0x42], {0: z0}))
        extra = ("\tsound2 {\n\t\tdai-link@0 {\n\t\t\treg = <0>;\n"
                 "\t\t\tdai-tdm-idle-mode-1 = \"zero\";\n\t\t\tdai-tdm-slot-tx-idle-mask-1 = <0x%x>;\n"
                 "\t\t\tcodec { sound-dai = <&amp_1_41>, <&amp_0_40>; };\n\t\t};\n\t};\n" % (z0 & 0xffff))
        return adt(group), tree.replace("\tsound {", extra + "\tsound {", 1)
    if name == "card-mask-not-zero-mode":
        # A mask without the "zero" idle mode is one macaudio does not apply: rows go on
        tree = dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1 ^ 0x100}))
        return adt(group), tree.replace('dai-tdm-idle-mode-1 = "zero";', 'dai-tdm-idle-mode-1 = "pulldown";')
    if name == "card-mask-short-rows":
        # Too short a speaker-config and amp-tx-zd-config, beside a card mask: neither goes on
        g = dict(GROUP3)
        g["speaker-config"] = ROWS[:8]
        g["amp-tx-zd-config"] = ZD[:12]
        short = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                                    amp("audio-speaker-b", 0x42, 0x12)]),
                 Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
        return adt(short), dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1 ^ 0x100}))
    if name == "card-codecs-not-bare":
        # A codec list whose entries are not one phandle each: its masks cannot be matched to
        # amps, so every row is passed on as without a card
        tree = dts(three, card=([0x40, 0x41, 0x42], {0: z0, 1: z1 ^ 0x100}))
        at = tree.index("amp_0_42: codec@42")
        cells = tree.index("#sound-dai-cells = <0>;", at)
        return adt(group), tree[:cells] + "#sound-dai-cells = <1>;" + tree[cells + 23:]
    if name == "j613":
        # The 13" M3 Air's woofer rows and card masks (left woofer 0x40 is codec 0, right woofer
        # 0x42 codec 2): each row is the complement of its mask, so neither goes to the kernel
        g = dict(GROUP3)
        g["amp-tx-zd-config"] = bytes.fromhex("f0f0f0ff00000000" "0000000000000000" "0f0f0fff00000000")
        j613 = [Node("i2c0", {}, [amp("audio-speaker", 0x40, 0x10, g),
                                   amp("audio-speaker-b", 0x42, 0x12)]),
                Node("i2c1", {}, [amp("audio-speaker-a", 0x41, 0x11)])]
        return adt(j613), dts(three, card=([0x40, 0x41, 0x42, ], {0: 0x0f0f, 2: 0xf0f0}))
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
    raise SystemExit("unknown scenario %s" % name)


SCENARIOS = ["group", "card-masks-agree", "card-mask-contradicts", "card-mask-contradicts-t8122",
             "card-masks-agree-t8122", "card-masks-two-links-disagree", "card-codecs-not-bare", "card-mask-not-zero-mode", "card-mask-short-rows", "j613", "no-dcblocker", "six-amps", "short-speaker-config", "single-amp", "amp-missing-in-dt", "amp-not-unique-in-dt",
             "dangling-phandle", "no-amps"] + list(SPEAKER_ID_SCENARIOS)


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
