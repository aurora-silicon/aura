# SPDX-License-Identifier: MIT
"""G0 (windows-m6/WINDOWS-GPU-PLAN.md H2/H3): GPU admission port, range table, handoff page and
the carrier glue for the J804 Windows guest, host only.

Nothing here touches hardware.  Every address, line, UUID, size and digest is synthetic; the
only machine-shaped facts are the ones the repository already carries (the TLV sequence of
src/kboot_gpu_t8152_profile.h, the ascwrap-v8 reg shape of src/kboot_gpu_t8152.c).

Failure modes written down before the code (the tests name the ones they cover):

Admission (gpu_t8152_admit.c)
  F1  wrong chip or board, or no profile/receipt -> refuse.
  F2  sgx missing, wrong compatible, mode, UAT width or VM base -> refuse.
  F3  a firmware region missing, unaligned, outside DRAM or overlapping another; a UAT region
      inside boot_args RAM, or a DATA region at or above its base -> refuse.
  F4  gfx*-data-shared-rw present and non-zero -> refuse.
  F5  ASC shape: compatible, post-office, mailbox number, segment names, reg sizes; a wrapper
      or iop-vbar outside the sgx window; iop-vbar not wrapper - 0x5b0000; the two roles on
      different post-office banks; the bank inside the sgx window -> refuse.
  F6  segment-ranges: phys != remap, DATA not the sgx-named region, TEXT size not the profile's,
      TEXT overlapping a region or the other TEXT, TEXT at or above boot_args RAM -> refuse.
  F7  firmware UUID not the profile's -> refuse.
  F8  a TLV value disagrees with this boot's ADT, a tag or length is wrong, or the patchbay span
      is not 0x23a -> refuse.
  F9  TEXT altered outside the excluded fields -> digest mismatch, refuse; altered only inside
      them -> still admitted.
  F10 interrupts: not two per ASC, outside [32, 1020), shared between roles, or inside the
      carrier's own block [980, 1000) (AHCI 990, the USB/ANS aliases) -> refuse.
  F11 perf-states / perf-states-sram malformed or of different lengths -> refuse.
  F12 a refusal leaves the output zeroed (no partial admission).
Range table
  F13 another node's register inside the sgx window (even sub-page or straddling its end) is
      never classified passable: holes round outward and are clipped to the window.
  F14 duplicate or nested holes give disjoint entries.
  F15 the CPU_CONTROL page and the iop-vbar pages get their own classes, ahead of holes.
  F16 entries are 16 KiB aligned, disjoint and tile the sgx window exactly.
  F17 a table that does not fit is refused, never truncated.
  F18 both GFX mailbox windows sit in one page outside the sgx window.
Handoff page
  F25 header layout, offsets and CRC-32 (zlib) as documented; nothing written past "used".
  F26 the ranges in the page are the table.
  F27 a page too small is refused without writing.
Carrier policy (hv_t8152.c), alias table (hv_aic_alias.c) and glue (hv_gpu.c)
  F19 the firmware class is accepted outside boot_args RAM only inside DRAM and clear of CTXR /
      CTRR, the denylist and guest RAM; refused inside boot_args RAM.
  F20 a denied TEXT segment can never be mapped afterwards.
  F21 the allow table takes the 8 firmware entries plus the shadow, the ADT and the handoff page.
  F28 GPU alias slots are owned only while live; illegal or colliding lines are refused.
  F32 ADMIT is refused without policy or guest RAM, after guest entry, and a second time; a
      malformed pool is refused before the one admission is spent.
  F33 HOOK only after ADMIT; it hooks exactly the sgx window and the mailbox page.
  F34 the hook is read-as-zero / write-ignored, counts by class and never touches memory.
  F35 the handoff page is mapped read-only at IPA = PA and its CRC is valid after every update.
Launcher
  F29 --windows-gpu defaults to off and off sends nothing; unknown values are refused; a
      resident without CAPS2 bit 2 is refused; the 16-byte handshake is written only after
      ADMIT returned 0.
"""

from dataclasses import dataclass, field
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import zlib

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"
E2E = REPO / "tests/e2e"
sys.path.insert(0, str(REPO / "proxyclient"))

CFLAGS = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]

# ---- the synthetic machine ---------------------------------------------------------------------

PAGE = 0x4000
BUS = 0x2_0000_0000              # arm-io ranges: bus 0 -> PA 0x2_0000_0000
SGX = 0x4_4000_0000
SGX_SIZE = 0x400_0000
SGX1 = (SGX + 0xd0_0000, 0x18_0000)
WRAP = (SGX + 0x180_0000, SGX + 0x200_0000)
VBAR = tuple(w - 0x5b_0000 for w in WRAP)
MBOX = 0x3_6008_0000
DRAM, DRAM_SIZE = 0x8_0000_0000, 0x2_0000_0000
RAM_BASE, RAM_TOP = 0x8_0400_0000, 0x9_f000_0000
REGIONS = [  # kboot order
    ("gpu-region", 0x9_ff80_0000, 0x4000),
    ("gfx-shared-region", 0x9_ff40_0000, 0x80000),
    ("gfx-shared-l2-region", 0x9_ff3f_c000, 0x4000),
    ("gfx-handoff", 0x9_ff3f_8000, 0x4000),
    ("gfx-data", 0x8_0200_0000, 0xa0000),
    ("gfx1-data", 0x8_020b_0000, 0x98000),
    ("gfx-data-shared-ro", 0x8_020a_0000, 0x8000),
    ("gfx1-data-shared-ro", 0x8_0214_8000, 0x8000),
]
TEXT = [(0x8_0100_0000, 0x48000), (0x8_0108_0000, 0x4c000)]
PATCHBAY = [0x1000, 0x2400]
VM_BASE = 0xffff_ff80_0000_0000
IRQS = [(700, 600), (701, 601)]
UUIDS = ["5E5E5E5E-0000-4000-8000-0000000000A0", "5E5E5E5E-0000-4000-8000-0000000000A1"]
RECEIPT = "G18G-RTKit-0000.0.0.synthetic"
CHIP, BOARD = 0x8152, 0x22
PERF = [(0, 600)] + [(400_000_000 + 50_000_000 * i, 650 + 10 * i) for i in range(13)]
PERF_SRAM = [(f, 760) for f, _ in PERF]
HOLES = {
    "pmgr-child": [(SGX + 0xe8_0000, 0x30000), (SGX + 0xec_0000, 0x4000),
                   (SGX + 0xec_0000, 0x8000), (SGX + 0xd1_0000, 0x8000),
                   (MBOX - 0x20000, 0x1fc000), (0x3_1000_0000, 0x4000)],
    "error-handler": [(SGX + 0xf4_8000, 0x4000), (SGX + 0xf4_c000, 0x4000),
                      (SGX + 0x10_00e8, 0x1268), (WRAP[0] + 0x10000, 0x4000),
                      (SGX + SGX_SIZE - 0x4000, 0x8000), (MBOX, 0x1c000)],
    "soc-misc": [(MBOX, 0x10000)],
}
POOL = (0x9_0000_0000, 0x1000_0000)
HANDOFF_IPA = 0x8_0600_0000
# G1: the synthetic PSD blocks (tests/e2e/windows_gpu_hw_model.h MODEL_PSD_BASE) sit inside the
# reg of an /arm-io node named "pmgr"; /chosen/memory-map carries the two zero slots iBoot leaves.
PSD_BASE = 0x3_0700_0000
# G2: a node with two regs and a gap, for a fw-mmio window anchored in the hull of both
# (windows_gpu_hw_model.h test_g2 slot 8).
AON_X = [(0x3_2000_0000, 0x4000), (0x3_2001_0000, 0x4000)]
MARKER_SLOT, POOL_SLOT = "MemoryMapReserved-15", "MemoryMapReserved-14"

# Classes (gpu_t8152_admit.h)
C_SGX, C_HOLE, C_CTRL, C_ASC, C_VBAR, C_MBOX, C_FW_RW, C_FW_RO, C_TEXT, C_POOL, C_HANDOFF = \
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11

# ---- a binary ADT in the layout rust/src/adt.rs walks ------------------------------------------


@dataclass
class Node:
    name: str
    props: dict = field(default_factory=dict)
    children: list = field(default_factory=list)

    def child(self, name):
        return next(c for c in self.children if c.name == name)

    def encode(self):
        props = [("name", self.name.encode() + b"\0")] + list(self.props.items())
        out = struct.pack("<II", len(props), len(self.children))
        for key, value in props:
            out += key.encode().ljust(32, b"\0") + struct.pack("<I", len(value))
            out += value + b"\0" * (-len(value) % 4)
        for c in self.children:
            out += c.encode()
        return out


def u32(*v):
    return struct.pack("<%dI" % len(v), *v)


def u64(*v):
    return struct.pack("<%dQ" % len(v), *v)


def s(text):
    return text.encode() + b"\0"


def reg(*pairs):
    return b"".join(u64(a - BUS, n) for a, n in pairs)


def segment_ranges(role):
    data, ro = REGIONS[4 + role], REGIONS[6 + role]
    out = b""
    for phys, size, iova in ((TEXT[role][0], TEXT[role][1], VM_BASE),
                             (data[1], data[2], VM_BASE + 0x50000),
                             (ro[1], ro[2], VM_BASE + 0x10c000)):
        out += struct.pack("<QQQII", phys, iova, phys, size, 0)
    return out


def machine():
    sgx = Node("sgx", {
        "compatible": s("gpu,t8152"),
        "reg": reg((SGX, SGX_SIZE), SGX1),
        "agx-address-space-mgmt-mode": u32(1),
        "rtkit-private-vm-region-base": u64(VM_BASE),
        "rtkit-private-vm-region-size": u64(0x20_0000_0000),
        "gfx-data-shared-rw-base": u64(0), "gfx-data-shared-rw-size": u64(0),
        "gfx1-data-shared-rw-base": u64(0), "gfx1-data-shared-rw-size": u64(0),
        "perf-states": b"".join(u32(f, v) for f, v in PERF),
        "perf-states-sram": b"".join(u32(f, v) for f, v in PERF_SRAM),
        "perf-state-count": u32(len(PERF)),
        "interrupts": u32(1100, 1101),
    })
    for name, base, size in REGIONS:
        sgx.props[name + "-base"] = u64(base)
        sgx.props[name + "-size"] = u64(size)
    ascs = []
    for role in range(2):
        prefix = "gfx1" if role else "gfx"
        ascs.append(Node(f"{prefix}-asc", {
            "compatible": s("iop,ascwrap-v8"),
            "msgbox-using-postoffice": s("true"),
            "msgbox-mailbox-num": u32(role + 1),
            "segment-names": s("__TEXT;__DATA;__DATA_SHARED_RO"),
            "reg": reg((WRAP[role], 0x74000), (VBAR[role], 8), (MBOX, 0x10000)),
            "segment-ranges": segment_ranges(role),
            "interrupts": u32(*IRQS[role]),
        }, [Node(f"iop-{prefix}-nub", {"uuid": s(UUIDS[role])})]))
    holes = [Node(name, {"reg": reg(*regs)}) for name, regs in HOLES.items()]
    armio = Node("arm-io", {"#address-cells": u32(2), "#size-cells": u32(2),
                            "ranges": u64(0, BUS, 0x4_0000_0000)},
                 [Node("aic", {"reg": reg((0x2_8000_0000, 0x100000))}), sgx, *ascs, *holes,
                  Node("pmgr", {"reg": reg((PSD_BASE, 0x4000))}),
                  Node("aon-x", {"reg": reg(*AON_X)})])
    memory_map = Node("memory-map", {"MemoryMapReserved-13": u64(0, 0), POOL_SLOT: u64(0, 0),
                                     MARKER_SLOT: u64(0, 0)})
    root = Node("device-tree", {"#address-cells": u32(2), "#size-cells": u32(2)}, [
        Node("chosen", {"dram-base": u64(DRAM), "dram-size": u64(DRAM_SIZE)}, [memory_map]),
        Node("defaults", {"uat-vaddr-size": u32(43)}),
        armio,
    ])
    return root


# ---- firmware TEXT with the real patchbay sequence ---------------------------------------------

def tlv_table():
    text = (REPO / "src/kboot_gpu_t8152_profile.h").read_text()
    body = text.split("static const struct gpu_tlv tlvs[] = {", 1)[1].split("};", 1)[0]
    entries = re.findall(r'\{"(.{4})", (\d+), (true|false)\}', body)
    assert len(entries) == 40 and sum(8 + int(n) for _, n, _ in entries) == 0x23a
    return [(tag, int(n), ex == "true") for tag, n, ex in entries], body


def tlv_expected(role):
    pt = REGIONS[1]
    return {"BPTP": pt[1] + role * (pt[2] // 2), "SPTP": pt[2] // 2, "BlpP": REGIONS[3][1],
            "S2xG": REGIONS[2][1], "BVTP": VM_BASE, "dArW": WRAP[role], "_COS": CHIP}


def make_text(role, overrides=None, tamper=None):
    tlvs, _ = tlv_table()
    size, bay = TEXT[role][1], PATCHBAY[role]
    data = bytearray(hashlib.sha256(b"synthetic text %d" % role).digest() * (size // 32))
    off = bay
    want = tlv_expected(role)
    want.update(overrides or {})
    for i, (tag, n, _) in enumerate(tlvs):
        value = want.get(tag, 0x1111111111111111 * (i + 1) & ((1 << (8 * n)) - 1))
        data[off:off + 8] = tag.encode() + struct.pack("<I", n)
        data[off + 8:off + 8 + n] = value.to_bytes(8, "little")[:n]
        off += 8 + n
    if tamper:
        tamper(data)
    return bytes(data)


def digest(role, data):
    tlvs, _ = tlv_table()
    copy = bytearray(data[:TEXT[role][1]])
    off = PATCHBAY[role]
    for tag, n, exclude in tlvs:
        if exclude:
            copy[off + 8:off + 8 + n] = bytes(n)
        off += 8 + n
    copy[0x224:0x228] = bytes(4)
    copy[0x23c:0x244] = bytes(8)
    return hashlib.sha256(copy).hexdigest()


# ---- the independent model of the class table ---------------------------------------------------

def model_ranges():
    sgx_lo, sgx_hi = SGX, SGX + SGX_SIZE
    holes = []
    for regs in HOLES.values():
        for a, n in regs:
            if a < sgx_hi and sgx_lo < a + n:
                holes.append((max(a, sgx_lo), min(a + n, sgx_hi)))
    pages = []
    for p in range(sgx_lo, sgx_hi, PAGE):
        cls, tag = C_SGX, 0
        for r in range(2):
            if WRAP[r] <= p < WRAP[r] + 0x74000:
                cls, tag = C_ASC, r
        if any(p < h1 and h0 < p + PAGE for h0, h1 in holes):
            cls, tag = C_HOLE, 0
        for r in range(2):
            if p == WRAP[r]:
                cls, tag = C_CTRL, r
            if p <= VBAR[r] < p + PAGE:
                cls, tag = C_VBAR, r
        if pages and pages[-1][2:] == (cls, tag) and pages[-1][0] + pages[-1][1] == p:
            pages[-1] = (pages[-1][0], pages[-1][1] + PAGE, cls, tag)
        else:
            pages.append((p, PAGE, cls, tag))
    out = list(pages)
    out.append((MBOX, PAGE, C_MBOX, 0))
    for i, (_, base, size) in enumerate(REGIONS):
        out.append((base, size, C_FW_RW if i < 4 else C_FW_RO, i))
    for r in range(2):
        out.append((TEXT[r][0], TEXT[r][1], C_TEXT, r))
    return out


# ---- build and run the admission driver --------------------------------------------------------

@pytest.fixture(scope="module")
def admit_exe(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    d = tmp_path_factory.mktemp("gpu_admit")
    stubs = d / "stubs"
    stubs.mkdir()
    (stubs / "types.h").write_text(
        "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n"
        "typedef unsigned long u64; typedef uint32_t u32; typedef uint16_t u16;\n"
        "typedef uint8_t u8; typedef long s64;\n#define BIT(x) (1UL << (x))\n"
        "#define PACKED __attribute__((packed))\n")
    (stubs / "utils.h").write_text(
        "#pragma once\n#include <stdio.h>\n#include \"types.h\"\n"
        "#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n"
        "#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))\n"
        "#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))\n")
    (stubs / "malloc.h").write_text("#pragma once\n#include <stdlib.h>\n")
    (stubs / "string.h").write_text("#pragma once\n#include <string.h>\n")
    _, body = tlv_table()
    (stubs / "tlvs.inc").write_text("static const struct gpu_tlv tlvs[] = {" + body + "};\n")
    shutil.copyfile(REPO / "src/adt.h", stubs / "adt.h")
    for name in ("gpu_t8152_admit.c", "gpu_t8152_admit.h"):
        shutil.copyfile(WIN / name, stubs / name)
    exe = d / "admit"
    subprocess.run([cc, *CFLAGS, "-iquote", str(stubs), "-I", str(E2E),
                    str(stubs / "gpu_t8152_admit.c"), str(E2E / "windows_gpu_admit_host.c"),
                    "-o", str(exe)], check=True)
    return exe


def run_admit(exe, tmp_path, root=None, texts=None, profile=None, extra="", cap=None,
              board=BOARD, chip=CHIP, receipt=RECEIPT):
    root = root or machine()
    (tmp_path / "adt.bin").write_bytes(root.encode())
    texts = texts or [make_text(0), make_text(1)]
    lines = [f"chip {chip:#x}", f"board {board:#x}", f"ram {RAM_BASE:#x} {RAM_TOP:#x}",
             f"receipt {receipt}", f"adt {tmp_path / 'adt.bin'}",
             f"pool {POOL[0]:#x} {POOL[1]:#x}", f"handoff {HANDOFF_IPA:#x}",
             f"out {tmp_path / 'handoff.bin'}"]
    for r in range(2):
        (tmp_path / f"text{r}.bin").write_bytes(texts[r])
        lines.append(f"text {r} {TEXT[r][0]:#x} {tmp_path / f'text{r}.bin'}")
        p = {"uuid": UUIDS[r], "size": TEXT[r][1], "bay": PATCHBAY[r],
             "digest": digest(r, texts[r] if not profile or "keep_digest" not in profile
                              else make_text(r))}
        if profile and r in profile:
            p.update(profile[r])
        lines.append(f"profile {r} {p['uuid']} {p['size']:#x} {p['bay']:#x} {p['digest']}")
    if cap is not None:
        lines.append(f"cap {cap}")
    lines.append(extra)
    (tmp_path / "cfg").write_text("\n".join(lines) + "\n")
    out = subprocess.run([str(exe), str(tmp_path / "cfg")], check=True, capture_output=True,
                         text=True).stdout
    return out


def fields(out, kind):
    return [line.split()[1:] for line in out.splitlines() if line.startswith(kind + " ")]


def ranges_of(out):
    return [(int(b, 16), int(n, 16), int(c), int(t))
            for i, b, n, c, t, _ in (f for f in fields(out, "R") if len(f) == 6)]


def test_admission_lifts_every_value(admit_exe, tmp_path):
    out = run_admit(admit_exe, tmp_path)
    assert "A rc 0" in out, out
    for i, (_, base, size) in enumerate(REGIONS):
        assert f"A region {i} {base:#x} {size:#x}" in out
    for r in range(2):
        assert f"A text {r} {TEXT[r][0]:#x} {TEXT[r][1]:#x}" in out
        assert f"A wrapper {r} {WRAP[r]:#x} 0x74000" in out
        assert f"A vbar {r} {VBAR[r]:#x} 0x8" in out
        assert f"A mbnum {r} {r + 1}" in out
        assert f"A irq {r} {IRQS[r][0]} {IRQS[r][1]}" in out
        assert f"A uuid {r} {UUIDS[r]}" in out
    assert f"A sgx {SGX:#x} {SGX_SIZE:#x}" in out
    assert f"A sgx1 {SGX1[0]:#x} {SGX1[1]:#x}" in out
    assert f"A mailbox {MBOX:#x} 0x10000" in out
    assert f"A dram {DRAM:#x} {DRAM_SIZE:#x}" in out
    assert f"A vm {VM_BASE:#x}" in out
    assert f"A perf {len(PERF)}" in out
    assert f"A receipt {RECEIPT}" in out
    assert "A text_reads 2" in out


def test_range_table_matches_model(admit_exe, tmp_path):
    """F13-F16: the sgx window page by page, holes rounded out and clipped, CPU_CONTROL and
    iop-vbar pages first, the table tiling the window; then mailbox, firmware, pool, handoff."""
    out = run_admit(admit_exe, tmp_path)
    got = ranges_of(out)
    want = model_ranges() + [(POOL[0], POOL[1], C_POOL, 0), (HANDOFF_IPA, PAGE, C_HANDOFF, 0)]
    assert got == want, "\n".join(f"{b:#x} {n:#x} {c} {t}" for b, n, c, t in got)
    window = [r for r in got if SGX <= r[0] < SGX + SGX_SIZE]
    assert sum(n for _, n, _, _ in window) == SGX_SIZE
    for b, n, _, _ in got:
        assert b % PAGE == 0 and n % PAGE == 0 and n
    spans = sorted((b, b + n) for b, n, _, _ in got)
    assert all(a[1] <= b[0] for a, b in zip(spans, spans[1:]))
    assert "F lookup-miss" not in out
    # The unaligned 0x1268-byte error-handler range and the one straddling the window's end.
    assert any(b <= SGX + 0x10_00e8 < b + n and c == C_HOLE for b, n, c, _ in got)
    assert any(b <= SGX + SGX_SIZE - PAGE < b + n and c == C_HOLE for b, n, c, _ in got)
    # The hole inside wrapper 0 beats the wrapper class; CPU_CONTROL and iop-vbar have theirs.
    assert (WRAP[0] + 0x10000, PAGE, C_HOLE, 0) in got
    assert (WRAP[0], PAGE, C_CTRL, 0) in got and (WRAP[1], PAGE, C_CTRL, 1) in got
    assert any(b <= VBAR[1] < b + n and c == C_VBAR and t == 1 for b, n, c, t in got)
    holes = fields(out, "H")
    assert holes[0] == ["count", "9"], holes
    assert all(h[-1] in ("pmgr-child", "error-handler") for h in holes[1:])


def test_mailbox_windows(admit_exe, tmp_path):
    """F18: the GFX windows are [0x100, 0x180) and [0x200, 0x280) of the post-office page."""
    out = run_admit(admit_exe, tmp_path)
    m = {int(o, 16): (int(i), int(r)) for o, i, r in fields(out, "M")}
    for off, (inside, role) in m.items():
        want = (1, 0) if 0x100 <= off < 0x180 else (1, 1) if 0x200 <= off < 0x280 else (0, 9)
        assert (inside, role) == want, hex(off)


def test_handoff_page_layout(admit_exe, tmp_path):
    """F25-F27, from the documented layout (gpu_t8152_admit.h), not from the C structs."""
    out = run_admit(admit_exe, tmp_path)
    used = int(fields(out, "P")[0][1])
    assert fields(out, "P")[1] == ["small", "0"]
    page = (tmp_path / "handoff.bin").read_bytes()
    magic, version, header, total, crc = struct.unpack_from("<IHHII", page, 0)
    assert (magic, version, header, total) == (0x31484741, 1, 0x100, used)
    assert page[total:] == bytes(len(page) - total), "bytes past the used size"
    zeroed = bytearray(page[:total])
    zeroed[0x0c:0x10] = bytes(4)
    assert crc == zlib.crc32(bytes(zeroed))
    (state, chip, board, nr, roff, rstride, npf, poff, pstride) = struct.unpack_from(
        "<9I", page, 0x10)
    assert (state, chip, board) == (0x3, CHIP, BOARD)
    assert (roff, rstride, pstride) == (0x100, 32, 16)
    assert struct.unpack_from("<4I", page, 0x34) == (*IRQS[0], *IRQS[1])
    assert struct.unpack_from("<2I", page, 0x44) == (1, 2)
    assert struct.unpack_from("<6Q", page, 0x50) == (POOL[0], POOL[1], HANDOFF_IPA, VM_BASE,
                                                      DRAM, DRAM_SIZE)
    assert page[0x80:0xc0].rstrip(b"\0").decode() == RECEIPT
    want = model_ranges() + [(POOL[0], POOL[1], C_POOL, 0), (HANDOFF_IPA, PAGE, C_HANDOFF, 0)]
    assert nr == len(want)
    for i, (b, n, c, t) in enumerate(want):
        rb, rn, rc, rt, rf, zero = struct.unpack_from("<QQIIII", page, roff + i * rstride)
        assert (rb, rn, rc, rt, zero) == (b, n, c, t, 0)
    assert npf == len(PERF) and poff == roff + nr * rstride
    for i in range(npf):
        assert struct.unpack_from("<4I", page, poff + i * pstride) == (
            PERF[i][0], PERF[i][1], PERF_SRAM[i][1], 0)
    assert total == poff + npf * pstride


def test_table_capacity_refused_not_truncated(admit_exe, tmp_path):
    """F17."""
    out = run_admit(admit_exe, tmp_path, cap=len(model_ranges()) - 1)
    assert "R count -1" in out and "does not fit" in out


def _refuses(exe, tmp_path, why, **kw):
    out = run_admit(exe, tmp_path, **kw)
    assert "A rc -1" in out, out
    assert "A zeroed 1" in out, "F12: partial admission left behind"
    assert why in out, out
    return out


def mutate(fn):
    root = machine()
    fn(root)
    return root


def armio(root, name=None):
    a = root.child("arm-io")
    return a.child(name) if name else a


REFUSALS = [
    # F1
    ("board", dict(board=0x24), "board"),
    ("chip", dict(chip=0x8122), "chip"),
    # F2
    ("sgx compatible", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "compatible", s("gpu,t8122")))), "sgx"),
    ("uat width", dict(root=mutate(lambda r: r.child("defaults").props.__setitem__(
        "uat-vaddr-size", u32(42)))), "uat-vaddr-size"),
    ("mgmt mode", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "agx-address-space-mgmt-mode", u32(0)))), "agx-address-space-mgmt-mode"),
    # F3
    ("region unaligned", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "gfx-handoff-base", u64(REGIONS[3][1] + 0x1000)))), "gfx-handoff"),
    ("uat in boot RAM", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "gpu-region-base", u64(RAM_TOP - 0x4000)))), "boot_args RAM"),
    ("regions overlap", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "gfx-shared-l2-region-base", u64(REGIONS[1][1] + 0x4000)))), "overlaps"),
    ("region missing", dict(root=mutate(lambda r: armio(r, "sgx").props.pop(
        "gfx1-data-size"))), "gfx1-data"),
    # F4
    ("shared rw", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "gfx1-data-shared-rw-size", u64(0x4000)))), "shared-rw"),
    # F5
    ("mailbox num", dict(root=mutate(lambda r: armio(r, "gfx1-asc").props.__setitem__(
        "msgbox-mailbox-num", u32(1)))), "gfx1-asc"),
    ("vbar shape", dict(root=mutate(lambda r: armio(r, "gfx-asc").props.__setitem__(
        "reg", reg((WRAP[0], 0x74000), (VBAR[0] + 0x4000, 8), (MBOX, 0x10000))))), "iop-vbar"),
    ("wrapper outside sgx", dict(root=mutate(lambda r: [armio(r, "gfx1-asc").props.__setitem__(
        "reg", reg((SGX + SGX_SIZE, 0x74000), (SGX + SGX_SIZE - 0x5b0000, 8), (MBOX, 0x10000))),
        None][1])), "sgx window"),
    ("two banks", dict(root=mutate(lambda r: armio(r, "gfx1-asc").props.__setitem__(
        "reg", reg((WRAP[1], 0x74000), (VBAR[1], 8), (MBOX + 0x10000, 0x10000))))),
        "post-office"),
    ("postoffice", dict(root=mutate(lambda r: armio(r, "gfx-asc").props.__setitem__(
        "msgbox-using-postoffice", s("false")))), "gfx-asc"),
    # F6
    ("text size", dict(profile={1: {"size": TEXT[1][1] - 0x4000}}), "TEXT"),
    ("text above phys_base", dict(root=mutate(lambda r: armio(r, "gfx-asc").props.__setitem__(
        "segment-ranges", struct.pack("<QQQII", RAM_BASE, VM_BASE, RAM_BASE, TEXT[0][1], 0) +
        segment_ranges(0)[32:]))), "segment"),
    ("data not region", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "gfx-data-size", u64(0x9c000)))), "segment"),
    # F7
    ("uuid", dict(profile={0: {"uuid": "5E5E5E5E-0000-4000-8000-0000000000FF"}}), "uuid"),
    # F8
    ("tlv value", dict(texts=[make_text(0), make_text(1, {"dArW": WRAP[0]})]), "TLV dArW"),
    ("tlv span", dict(texts=[make_text(0, tamper=lambda d: d.__setitem__(
        slice(PATCHBAY[0] + 4, PATCHBAY[0] + 8), struct.pack("<I", 4)))]
        + [make_text(1)]), "patchbay"),
    # F9
    ("digest", dict(texts=[make_text(0, tamper=lambda d: d.__setitem__(0x10, d[0x10] ^ 1)),
                           make_text(1)], profile={"keep_digest": True}), "digest"),
    # F10
    ("irq count", dict(root=mutate(lambda r: armio(r, "gfx-asc").props.__setitem__(
        "interrupts", u32(700)))), "interrupts"),
    ("irq range", dict(root=mutate(lambda r: armio(r, "gfx1-asc").props.__setitem__(
        "interrupts", u32(1020, 601)))), "interrupts"),
    ("irq shared", dict(root=mutate(lambda r: armio(r, "gfx1-asc").props.__setitem__(
        "interrupts", u32(700, 601)))), "interrupts"),
    ("irq virtual", dict(root=mutate(lambda r: armio(r, "gfx1-asc").props.__setitem__(
        "interrupts", u32(701, 995)))), "interrupts"),
    # F11
    ("perf", dict(root=mutate(lambda r: armio(r, "sgx").props.__setitem__(
        "perf-states-sram", b"".join(u32(f, v) for f, v in PERF_SRAM[:-1])))), "perf"),
]


@pytest.mark.parametrize("name,kw,why", REFUSALS, ids=[r[0] for r in REFUSALS])
def test_admission_refusals(admit_exe, tmp_path, name, kw, why):
    _refuses(admit_exe, tmp_path, why, **kw)


def test_excluded_fields_do_not_change_identity(admit_exe, tmp_path):
    """F9, the other direction: the per-boot fields (excluded TLVs, 0x224, 0x23c) may differ."""
    def per_boot(d):
        d[0x224:0x228] = b"\x12\x34\x56\x78"
        d[0x23c:0x244] = b"\x9a" * 8
    texts = [make_text(0, tamper=per_boot), make_text(1, {"GKTS": 0x55})]
    out = run_admit(admit_exe, tmp_path, texts=texts, profile={"keep_digest": True})
    assert "A rc 0" in out, out


# ---- the carrier policy (hv_t8152.c) for the firmware class ------------------------------------

def test_policy_firmware_class():
    """F19-F21 run inside the existing policy harness (test_windows_t8152_policy.py), which
    compiles hv_t8152.c unchanged; here only check that the block is there and still runs."""
    policy = (REPO / "tests/python/test_windows_t8152_policy.py").read_text()
    assert "GPU FIRMWARE CLASS CHECK" in policy
    assert "hv_t8152_gpu_fw_allow" in policy and "hv_t8152_gpu_deny" in policy


# ---- the alias table (hv_aic_alias.c) ----------------------------------------------------------

ALIAS_HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include "hv_aic_alias.h"
u32 chip_id = 0x8152;
int main(void)
{
    u32 pub;
    const u32 lines[4] = {700, 600, 701, 601};
    for (u32 i = 0; i < 4; i++)
        assert(!hv_aic_alias_is_owned(lines[i]) && !hv_aic_alias_lookup_physical(lines[i], &pub));
    assert(hv_aic_alias_t8152_gpu(lines, true) == 0);
    for (u32 i = 0; i < 4; i++) {
        assert(hv_aic_alias_is_owned(lines[i]));
        assert(hv_aic_alias_lookup_physical(lines[i], &pub) && pub == lines[i]);
        assert(hv_aic_alias_to_physical(lines[i]) == lines[i]);
    }
    hv_aic_alias_t8152_usb(1u << 1);  /* USB slots are independent of the GPU ones */
    assert(hv_aic_alias_is_owned(995) && hv_aic_alias_is_owned(700));
    assert(hv_aic_alias_t8152_gpu(lines, false) == 0);
    for (u32 i = 0; i < 4; i++)
        assert(!hv_aic_alias_is_owned(lines[i]) && !hv_aic_alias_lookup_physical(lines[i], &pub));
    assert(hv_aic_alias_is_owned(995));
    /* Refusals leave the slots withdrawn. */
    const u32 bad[][4] = {{700, 600, 701, 31}, {700, 600, 701, 1020}, {700, 600, 701, 700},
                          {700, 600, 701, 990}, {700, 600, 701, 995}, {700, 600, 701, 1692}};
    for (u32 b = 0; b < sizeof(bad) / sizeof(bad[0]); b++) {
        assert(hv_aic_alias_t8152_gpu(bad[b], true) < 0);
        assert(!hv_aic_alias_is_owned(700) && !hv_aic_alias_is_owned(600));
    }
    hv_aic_alias_t8152_usb(0);
    chip_id = 0x8142;
    assert(hv_aic_alias_t8152_gpu(lines, true) < 0);
    puts("GPU ALIAS OK");
    return 0;
}
"""


def test_gpu_alias_slots(tmp_path):
    """F28."""
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    stubs = tmp_path / "stubs"
    stubs.mkdir()
    (stubs / "types.h").write_text(
        "#pragma once\n#include <stdbool.h>\n#include <stdint.h>\ntypedef uint32_t u32;\n"
        "typedef unsigned long u64;\n#define BIT(x) (1UL << (x))\n")
    (stubs / "soc.h").write_text("#pragma once\n#define T8142 0x8142\n#define T8152 0x8152\n")
    (stubs / "utils.h").write_text(
        "#pragma once\n#include <stdio.h>\n#include \"types.h\"\nextern u32 chip_id;\n"
        "#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n")
    for name in ("hv_aic_alias.c", "hv_aic_alias.h"):
        shutil.copyfile(WIN / name, stubs / name)
    (tmp_path / "harness.c").write_text(ALIAS_HARNESS)
    exe = tmp_path / "alias"
    subprocess.run([cc, *CFLAGS, "-iquote", str(stubs), str(stubs / "hv_aic_alias.c"),
                    str(tmp_path / "harness.c"), "-o", str(exe)], check=True)
    out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    assert "GPU ALIAS OK" in out


# ---- the carrier glue (hv_gpu.c) ---------------------------------------------------------------

def glue_profile_inc():
    """The synthetic J804 profile (as the private build header would hold it) and the real
    TLV sequence, for hv_gpu.c's accessors."""
    rows = []
    for r in range(2):
        role = "gfx1-asc" if r else "gfx-asc"
        dig = ", ".join(hex(b) for b in bytes.fromhex(digest(r, make_text(r))))
        rows.append('    {"%s", "%s", %#x, %#x, {%s}},\n' % (role, UUIDS[r], TEXT[r][1],
                                                         PATCHBAY[r], dig))
    return ("static const struct gpu_profile profiles_j804[2] = {\n" + "".join(rows) + "};\n"
            "static const struct gpu_tlv tlvs[] = {" + tlv_table()[1] + "};\n")


@pytest.fixture(scope="module")
def glue_exe(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    d = tmp_path_factory.mktemp("gpu_glue")
    stubs = d / "stubs"
    stubs.mkdir()
    (stubs / "types.h").write_text(
        "#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n#include <stdint.h>\n"
        "typedef unsigned long u64; typedef uint32_t u32; typedef uint16_t u16;\n"
        "typedef uint8_t u8; typedef long s64;\n#define BIT(x) (1UL << (x))\n"
        "#define PACKED __attribute__((packed))\n")
    (stubs / "utils.h").write_text(
        "#pragma once\n#include <stdio.h>\n#include \"types.h\"\n"
        "#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n"
        "#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))\n"
        "#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))\n"
        "extern u32 board_id, chip_id;\n")
    (stubs / "soc.h").write_text("#pragma once\n#define T8152 0x8152\n")
    (stubs / "malloc.h").write_text(
        "#pragma once\n#include <stdlib.h>\nvoid *memalign(size_t align, size_t size);\n")
    (stubs / "string.h").write_text("#pragma once\n#include <string.h>\n")
    (stubs / "xnuboot.h").write_text(
        "#pragma once\n#include \"types.h\"\n"
        "struct boot_args { u64 phys_base, virt_base, mem_size; };\n"
        "extern struct boot_args cur_boot_args;\n")
    (stubs / "hv.h").write_text(
        "#pragma once\n#include \"types.h\"\nstruct exc_info;\n"
        "typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, "
        "int width);\nint hv_map_hook(u64 from, hv_hook_t *hook, u64 size);\n"
        "int hv_map_hw_ro(u64 from, u64 to, u64 size);\nint hv_unmap(u64 from, u64 size);\n"
        "int hv_map_hw(u64 from, u64 to, u64 size);\n"
        "bool hv_windows_guest_entered(void);\n"
        "#include \"hv_t8152.h\"\n")
    (stubs / "hv_t8152.h").write_text(
        "#pragma once\n#include \"types.h\"\nbool hv_t8152_policy_armed(void);\n"
        "bool hv_t8152_guest_ram(u64 *base, u64 *size);\n"
        "int hv_t8152_allow(u64 ipa, u64 pa, u64 size, bool read_only, const char *name);\n"
        "int hv_t8152_gpu_fw_allow(u64 pa, u64 size, bool read_only, u64 dram_lo, u64 dram_hi,"
        " const char *name);\nint hv_t8152_gpu_deny(u64 start, u64 end, const char *name);\n"
        "unsigned int hv_t8152_allow_mark(void);\n"
        "void hv_t8152_allow_rollback(unsigned int mark, const char *why);\n"
        "int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name);\n")
    (stubs / "memory.h").write_text(
        "#pragma once\n#include \"types.h\"\nvoid dc_civac_range(void *addr, size_t length);\n")
    (stubs / "glue_profile.inc").write_text(glue_profile_inc())
    shutil.copyfile(REPO / "src/adt.h", stubs / "adt.h")
    for name in GLUE_SOURCES:
        shutil.copyfile(WIN / name, stubs / name)
    exes = {}
    for kind, driver in (("glue", "windows_gpu_glue_host.c"), ("probe", "windows_gpu_probe_host.c")):
        exes[kind] = d / kind
        subprocess.run([cc, *CFLAGS, "-DHV_GPU_HOST_TEST", "-iquote", str(stubs), "-I", str(E2E),
                        *(str(stubs / n) for n in GLUE_SOURCES if n.endswith(".c")),
                        str(E2E / driver), "-o", str(exes[kind])], check=True)
    return exes


GLUE_SOURCES = ("gpu_t8152_admit.c", "gpu_t8152_admit.h", "gpu_t8152_power.c", "gpu_t8152_power.h",
                "gpu_t8152_probe.c", "gpu_t8152_probe.h", "gpu_t8152_g2.c", "gpu_t8152_g2.h",
                "gpu_t8152_uat.c", "gpu_t8152_uat.h", "hv_gpu.c", "hv_gpu.h")


def test_glue(glue_exe, tmp_path):
    """F32-F35, with the synthetic machine and TEXT loaded where the ADT says it is."""
    (tmp_path / "adt.bin").write_bytes(machine().encode())
    for r in range(2):
        (tmp_path / f"text{r}.bin").write_bytes(make_text(r))
    args = [str(glue_exe["glue"]), str(tmp_path / "adt.bin"), f"{TEXT[0][0]:#x}",
            str(tmp_path / "text0.bin"), f"{TEXT[1][0]:#x}", str(tmp_path / "text1.bin"),
            f"{SGX:#x}", f"{SGX_SIZE:#x}", f"{MBOX:#x}", f"{POOL[0]:#x}", f"{POOL[1]:#x}",
            f"{RAM_BASE:#x}", f"{RAM_TOP:#x}"]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    assert "GPU GLUE OK" in out, out
    assert "HV: GPU: admitted J804 G18G-RTKit-3514.41.1.release" in out
    assert "refused: ADMIT runs once per carrier session" in out
    assert "pool [" in out and "must be 32 MiB aligned" in out


# ---- G1: power-on, boot marker, mediated forwarding (hv_gpu.c POWER / PROBE / STOP) ------------
#
# Failure modes written down before the code (windows_gpu_probe_host.c covers each):
#   G1-1 POWER before ADMIT, after guest entry or in a G0 "log" session; PROBE before POWER.
#   G1-2 the PSD requests go out once per service, in recipe order, with the recipe's values, each
#        only from cold idle; the marker is claimed in the live ADT before the first request; the
#        ID words land in the handoff page; PROBE forwards exactly the measured set (ID words,
#        CPU_CONTROL/CPU_STATUS, the GFX windows' RX/TX/status) and nothing else reaches the
#        hardware; CPU_CONTROL writes are never performed (RUN is never cleared or toggled); the
#        firmware regions are mapped identity (UAT RW, DATA RO); every access is in the map.
#   G1-3 a carrier reload in the same boot (same ADT, same heap) refuses POWER before any PMGR
#        access, even when the marker page itself was overwritten.
#   G1-4 a reboot (fresh ADT) frees it again.
#   G1-5 a set slot that is not a GPU marker, or a taken pool slot, refuses POWER.
#   G1-6 a service already on without a marker: claimed as used, nothing requested.
#   G1-7 no acknowledge or the wrong state: the sequence stops there, nothing is powered off, the
#        marker says POWER_FAILED and a reload cannot retry.
#   G1-8 no recipe, or a PSD block outside every ADT pmgr range: nothing touched, nothing spent.
#   G1-9 STOP / teardown / the reset path turn forwarding off without touching the GPU; a failed
#        firmware map leaves nothing mapped.


def test_g1_probe(glue_exe, tmp_path):
    (tmp_path / "adt.bin").write_bytes(machine().encode())
    for r in range(2):
        (tmp_path / f"text{r}.bin").write_bytes(make_text(r))
    hole = SGX + 0xe8_0000
    args = [str(glue_exe["probe"]), str(tmp_path / "adt.bin"), f"{TEXT[0][0]:#x}",
            str(tmp_path / "text0.bin"), f"{TEXT[1][0]:#x}", str(tmp_path / "text1.bin"),
            f"{SGX:#x}", f"{SGX_SIZE:#x}", f"{MBOX:#x}", f"{POOL[0]:#x}", f"{POOL[1]:#x}",
            f"{RAM_BASE:#x}", f"{RAM_TOP:#x}", f"{WRAP[0]:#x}", f"{WRAP[1]:#x}", f"{VBAR[0]:#x}",
            f"{hole:#x}"]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    for tag in ("ORDER", "PROBE", "RELOAD", "REBOOT", "SLOT", "FOREIGN", "FAIL", "RECIPE", "STOP"):
        assert f"G1 {tag} OK" in out, out
    assert "GPU PROBE OK" in out
    assert "HV: GPU: boot marker claimed: /chosen/memory-map/MemoryMapReserved-15" in out
    assert "the boot marker says a session ran and ended with the firmware left running" in out
    assert "set but holds no valid GPU marker" in out
    assert "probe read sgx+0xd04000 (sgx, 4 bytes) -> 0x5100: passed" in out
    assert "probe read sgx+0xe80000 (sgx-hole, 4 bytes) -> 0x0: denied" in out
    assert "GFX1 RX msg 0x0010000000000c0c ep 0x0" in out
    assert "GFX1 TX msg 0x0020000000000c0c ep 0x0" in out
    assert "CPU_CONTROL write 0x0 refused" in out
    assert re.search(r"map sgx\+0xd04000 R32 sgx passed n=1 first 0x5100", out), out
    assert "never stopped, powered off or restarted by the carrier" in out
    assert "PSD core (service 5): request 1 acknowledged after" in out


# ---- G2: grants, guards and the UAT audit (hv_gpu.c G2, gpu_t8152_g2.c, gpu_t8152_uat.c) -------
#
# Failure modes written down before the code (windows_gpu_probe_host.c g2_tests() covers each):
#   G2-1 G2 before ADMIT, after POWER, in a G0 log session, twice, after guest entry; unknown
#        grant bits; no or a malformed private recipe: refused, and a refusal spends nothing.
#   G2-2 a fw-mmio window outside the hull of its ADT node's regs, on an unknown node, over an ASC
#        wrapper's CPU_CONTROL page or an iop-vbar page: refused; granted, every window is
#        published as a class-12 handoff entry (tag = slot, RO flag) after the class table, which
#        itself is unchanged; not granted, nothing is declared.
#   G2-3 the boot-control register is touched only when G2-D2 is granted, only at POWER: read,
#        refused unless the recipe accepts the value (nothing written), written once, read back;
#        a wrong read-back is reported, never retried; a register outside a pmgr hole is
#        refused at G2.
#   G2-4 G2 takes whole messages only; a second init-data message to one role, the recipe's
#        refused types and an RTKit IOP power state other than on never reach the firmware; after init data the guest's sgx accesses
#        (CPU_CONTROL included) are locked; every message is counted by role, direction, kind.
#   G2-5 the UAT audit: the PROBE baseline keeps the firmware's own mappings as known; a fresh
#        outlier anywhere refuses init data; a fresh outlier in a guest-writable table (pool,
#        context table, and every table in an fw-rw region PROBE maps RW, the page-table region
#        included, also behind a context root the guest points there) refuses every
#        application-endpoint message; tables outside the table regions, unreadable tables,
#        malformed descriptors and a page reached as a table at two levels are outliers; the
#        teardown audits again and marks the boot marker.
#   G2-6 a G1 probe session is unchanged (halves forwarded, no guards) but records the audit.
#   G2-7 a firmware baseline of thousands of outliers is found by lookup, not capped; an outlier
#        in a guest table at PROBE (a stale context root into the pool) is never inherited as
#        known: init data waits until the guest clears it; a new entry in the host-owned
#        shared-L2 table refuses a doorbell, so does a changed firmware-table leaf (guest-writable
#        region) until it is restored.


def test_g2_guards(glue_exe, tmp_path):
    (tmp_path / "adt.bin").write_bytes(machine().encode())
    for r in range(2):
        (tmp_path / f"text{r}.bin").write_bytes(make_text(r))
    hole = SGX + 0xe8_0000
    args = [str(glue_exe["probe"]), str(tmp_path / "adt.bin"), f"{TEXT[0][0]:#x}",
            str(tmp_path / "text0.bin"), f"{TEXT[1][0]:#x}", str(tmp_path / "text1.bin"),
            f"{SGX:#x}", f"{SGX_SIZE:#x}", f"{MBOX:#x}", f"{POOL[0]:#x}", f"{POOL[1]:#x}",
            f"{RAM_BASE:#x}", f"{RAM_TOP:#x}", f"{WRAP[0]:#x}", f"{WRAP[1]:#x}", f"{VBAR[0]:#x}",
            f"{hole:#x}"]
    out = subprocess.run(args, check=True, capture_output=True, text=True).stdout
    for tag in ("ORDER", "DECLARE", "BOOT", "GUARD", "G1-SESSION", "BASELINE"):
        assert f"G2 {tag} OK" in out, out
    assert "HV: GPU: G2 armed (synthetic G2 recipe): fw-mmio declared (G2-D1), boot-control " \
        "granted (G2-D2)" in out
    assert "fw-mmio slot 8 [0x320000000, +0x14000) RW, anchored in /arm-io/aon-x" in out
    assert "fw-mmio slot 2 [0x280100000, +0x4000) leaves /arm-io/aic regs 0..0" in out
    assert "fw-mmio slot 6 [0x440001800, +0x27f8) RO, anchored in /arm-io/sgx" in out
    assert "touches asc-ctrl range" in out and "touches iop-vbar range" in out
    assert "boot-control sgx+0xe94000 0x700 -> 0x701 (G2-D2, once this boot)" in out
    assert "boot-control sgx+0xe94000 reads 0x123, not a value the recipe accepts: not written" \
        in out
    assert "NOT the expected value; reboot the M6" in out
    assert "is not inside a pmgr sgx hole; refused" in out
    assert re.search(r"UAT audit baseline \(before the guest\): 1 roots, 3 tables; .*outliers 1 "
                     r"\(1 in the baseline\): text 0 dram 0 mmio 1 bad 0 table 0; fresh 0", out), out
    assert "GFX TX 0x00810c20001a8000 refused: the GPU page tables map outside" in out
    assert "refused: a second init-data message to this role" in out
    assert "refused: a message type the G2 recipe never forwards" in out
    assert "GFX init data forwarded: sgx is locked for the guest from now on" in out
    assert re.search(r"UAT outlier: entry 0x[0-9a-f]+ \(level 3, guest-writable table\) descriptor "
                     r"0x[0-9a-f]+: text", out), out
    assert re.search(r"UAT outlier: .*: table", out) and re.search(r"UAT outlier: .*: bad", out)
    # A firmware-region table the guest wrote, and a page reached as a table at two levels.
    assert re.search(r"UAT outlier: entry 0x[0-9a-f]+ \(level 3, guest-writable table\) descriptor "
                     r"0x3500004403: mmio", out), out
    assert re.search(r"UAT outlier: entry 0x[0-9a-f]+ \(level 1, guest-writable table\) descriptor "
                     r"0x[0-9a-f]+: bad", out), out
    assert "UAT audit at teardown" in out
    assert re.search(r"map post-office\+0x110 W128 mailbox blocked", out), out
    assert re.search(r"map sgx\+0xd04000 R32 sgx locked", out), out
    assert "messages GFX TX init-data n=1" in out and "messages GFX RX init-ack n=1" in out
    assert "refused: an IOP power state other than on" in out
    assert "G2 guards: 2 second init-data refused, 3 refused types, 8 refused by the UAT audit, " \
        "3 sgx accesses locked, 3 mailbox halves denied; init data sent to GFX GFX1" in out


def test_g2_recipe_is_not_in_the_repository():
    """D1(a): the G2 recipe (upstream values) only comes from private data at build time, and no
    G2 source carries an address, a boot-control value or a window table."""
    recipe = (WIN / "gpu_t8152_g2_recipe.c").read_text()
    assert '#include "../build/j804_gpu_g2.h"' in recipe
    assert "#define J804_GPU_G2_RECIPE" not in recipe
    for name in ("gpu_t8152_g2.c", "gpu_t8152_g2.h", "gpu_t8152_g2_recipe.c", "gpu_t8152_uat.c",
                 "gpu_t8152_uat.h", "hv_gpu.c", "hv_gpu.h"):
        code = re.sub(r"/\*.*?\*/", "", (WIN / name).read_text(), flags=re.S)
        assert not re.search(r"0x[0-9a-fA-F]{6,}", code), name   # no offsets, no addresses
        assert not re.search(r"0x(86|3000[01])\b", code), name    # no recovery type, no value
        assert "e90000" not in code and "aon-ptd" not in code, name
    makefile = (REPO / "Makefile").read_text()
    assert "$(J804_PRIVATE_DATA)/gpu/carrier/g18g-j804-gpu-g2.h" in makefile
    assert "#define J804_GPU_G2_RECIPE {.available = false}" in makefile
    tracked = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True).stdout
    assert "j804_gpu_g2.h" not in tracked and "g18g-j804-gpu-g2.h" not in tracked


def test_g2_forwards_no_new_register():
    """G2-DESIGN section 7: G2 adds no forwarded register; the sgx read set is the G1 one."""
    code = (WIN / "gpu_t8152_probe.c").read_text()
    body = code.split("static const struct gpu_probe_reg sgx_reads[] = {", 1)[1].split("};", 1)[0]
    assert len(re.findall(r"\{0x[0-9a-f]+,", body)) == 7
    decide = code.split("static u32 decide(", 1)[1].split("\n}\n", 1)[0]
    assert "whole" in decide and "GPU_P_PASS" in decide
    glue = (WIN / "hv_gpu.c").read_text()
    hook = glue.split("static bool probe_hook(", 1)[1].split("\n}\n", 1)[0]
    # Every hardware access of the hook is the one hw_rw() of a decided PASS/CPU_CONTROL read.
    assert hook.count("hw_rw(") == 2


def test_g1_marker_layout():
    """The record the next carrier reads: 96 bytes, CRC-32 (zlib) over everything before it."""
    hdr = (WIN / "gpu_t8152_power.h").read_text()
    assert "_Static_assert(sizeof(struct gpu_boot_marker) == 96" in hdr
    assert '#define GPU_BOOT_MARKER_MAGIC   0x314b4d4255504741ULL /* "AGPUBMK1" */' in hdr
    assert struct.pack("<Q", 0x314b4d4255504741) == b"AGPUBMK1"
    glue = (WIN / "hv_gpu.h").read_text()
    assert '#define HV_GPU_BOOT_MARKER "MemoryMapReserved-15"' in glue
    assert '#define HV_GPU_POOL_SLOT   "MemoryMapReserved-14"' in glue


def test_g1_wired():
    hv = (WIN / "hv.c").read_text()
    start = hv.split("int hv_start(void *entry, u64 regs[4])\n{", 1)[1].split("\n}", 1)[0]
    assert "hv_usb_teardown(why);" in start and "hv_gpu_teardown(why);" in start
    utils = (REPO / "src/utils.c").read_text()
    reset = utils.split("void flush_and_reboot(void)", 1)[1].split("reboot();", 1)[0]
    assert reset.index("hv_usb_emergency_stop();") < reset.index("hv_gpu_emergency_stop();") < \
        reset.index("hv_ahci_nvme_reset_close()")
    proxy_c = (REPO / "src/proxy.c").read_text()
    assert "HV_WINDOWS_CAPS2_GPU_PROBE |\n                            HV_WINDOWS_CAPS2_GPU_G2;" in proxy_c
    makefile = (REPO / "Makefile").read_text()
    objs = makefile.split("ifeq ($(WINDOWS),1)\nHV_OBJECTS", 1)[1].split("endif", 1)[0]
    for o in ("gpu_t8152_power.o", "gpu_t8152_power_recipe.o", "gpu_t8152_probe.o"):
        assert o in objs
    assert "build/hv/windows/gpu_t8152_power_recipe.o: build/j804_gpu_power.h" in makefile
    assert "$(J804_PRIVATE_DATA)/aura/g18g-j804-gpu-power.h" in makefile
    assert "#define J804_GPU_PSD_RECIPE {.available = false}" in makefile


def test_g1_recipe_is_not_in_the_repository():
    """D1(a): the PSD recipe (upstream values) only comes from private data at build time."""
    recipe = (WIN / "gpu_t8152_power_recipe.c").read_text()
    assert '#include "../build/j804_gpu_power.h"' in recipe
    assert "#define J804_GPU_PSD_RECIPE" not in recipe
    for name in ("gpu_t8152_power.c", "gpu_t8152_power_recipe.c", "gpu_t8152_power.h",
                 "hv_gpu.c", "gpu_t8152_probe.c"):
        code = re.sub(r"/\*.*?\*/", "", (WIN / name).read_text(), flags=re.S)
        code = code.replace("0x314b4d4255504741ULL", "MAGIC")   # "AGPUBMK1", not an address
        assert not re.search(r"0x3[0-9a-f]{2}a1", code, re.I), name
        assert not re.search(r"\b(81|82)\b", code), name   # no PSD service ids
        assert not re.search(r"0x[0-9a-fA-F]{9,}", code), name
    tracked = subprocess.run(["git", "ls-files"], cwd=REPO, capture_output=True, text=True).stdout
    assert "j804_gpu_power.h" not in tracked and "g18g-j804-gpu-power.h" not in tracked


def test_g1_forwarded_set_is_the_measured_one():
    """gpu_t8152_probe.c forwards exactly the sgx words macOS read at bring-up (ledger G-10/B-09)
    and nothing from pmgr-child, the KSM block or the rest of B-09."""
    code = (WIN / "gpu_t8152_probe.c").read_text()
    body = code.split("static const struct gpu_probe_reg sgx_reads[] = {", 1)[1].split("};", 1)[0]
    offsets = [int(o, 16) for o in re.findall(r"\{(0x[0-9a-f]+),", body)]
    assert offsets == [0xd04000, 0xd04008, 0xd04010, 0xd04014, 0xd04018, 0xd0401c, 0xe01500]


# ---- source rules ------------------------------------------------------------------------------

def test_sources_wired():
    makefile = (REPO / "Makefile").read_text()
    objs = makefile.split("ifeq ($(WINDOWS),1)\nHV_OBJECTS", 1)[1].split("endif", 1)[0]
    assert "hv_gpu.o" in objs and "gpu_t8152_admit.o" in objs
    proxy_h = (REPO / "src/proxy.h").read_text()
    assert "P_HV_WINDOWS_GPU = 0xc88," in proxy_h
    proxy_c = (REPO / "src/proxy.c").read_text()
    assert "case P_HV_WINDOWS_GPU:" in proxy_c and "HV_WINDOWS_CAPS2_GPU" in proxy_c
    py = (REPO / "proxyclient/aura/proxy.py").read_text()
    assert "P_HV_WINDOWS_GPU = 0xc88" in py
    kboot = (REPO / "src/kboot_gpu_t8152.c").read_text()
    assert '#include "hv/windows/gpu_t8152_admit.h"' in kboot
    assert "struct gpu_profile {" not in kboot
    assert "const struct gpu_profile *gpu_t8152_j804_profiles(void)" in kboot
    # The carrier never hard-codes a GPU address: everything comes from the admission.
    glue = "".join((WIN / n).read_text() for n in ("hv_gpu.c", "gpu_t8152_admit.c",
                                                    "gpu_t8152_power.c", "gpu_t8152_probe.c"))
    code = re.sub(r"/\*.*?\*/", "", glue, flags=re.S)
    assert not re.search(r"0x[0-9a-fA-F]{9,}", code), "hard-coded physical address"


# ---- the launcher ------------------------------------------------------------------------------

def test_launcher_gpu_option():
    """F29."""
    from aura import windows as w
    assert w.parse_windows_gpu("off") == "off" and w.parse_windows_gpu("disabled") == "off"
    assert w.parse_windows_gpu("admit") == "admit" and w.parse_windows_gpu("LOG") == "log"
    assert w.parse_windows_gpu("probe") == "probe"
    for bad in ("on", "pass", "run", "", "power"):
        with pytest.raises(ValueError):
            w.parse_windows_gpu(bad)
    caps2 = w.CAPS2_MAGIC | w.CAPS2_GPU
    w.validate_gpu_request("off", 0x8122, 0x24, 0)  # off asks nothing of anyone
    w.validate_gpu_request("log", w.T8152_CHIP, w.J804_BOARD_ID, caps2)
    for chip, board, c2 in ((0x8122, w.J804_BOARD_ID, caps2), (w.T8152_CHIP, 0x24, caps2),
                            (w.T8152_CHIP, w.J804_BOARD_ID, w.CAPS2_MAGIC)):
        with pytest.raises(ValueError):
            w.validate_gpu_request("admit", chip, board, c2)
    value = w.gpu_handshake(0x8_0600_0000, 0x4000)
    assert value == struct.pack("<QQ", 0x8_0600_0000, 0x4000)
    for ipa, size in ((0, 0x4000), (0x8_0600_1000, 0x4000), (0x8_0600_0000, 0x8000),
                      ((1 << 64) - 1, 0x4000)):
        with pytest.raises(ValueError):
            w.gpu_handshake(ipa, size)
    assert w.gpu_pool(0x8_0000_0000, 0x9_2000_0000, 256) == (0x9_1000_0000, 0x1000_0000)
    with pytest.raises(ValueError):
        w.gpu_pool(0x8_0000_0000, 0x8_1000_0000, 256)   # more than half of guest RAM
    with pytest.raises(ValueError):
        w.gpu_pool(0x8_0000_0000, 0x9_2100_0000, 256)   # top not 32 MiB aligned
    run_guest = (REPO / "proxyclient/tools/run_guest.py").read_text()
    assert "parser.add_argument('--windows-gpu', default='off'" in run_guest
    attach = run_guest.split("def windows_t8152_gpu_admit(", 1)[1].split("\ndef ", 1)[0]
    assert attach.index("GPU_OP_ADMIT") < attach.index("set_windows_gpu_handshake")
    assert "if mode == \"off\":\n        return" in attach
    assert "windows_t8152_gpu_admit(hv, gpu_mode, usb_ports, ahci)" in run_guest


def test_launcher_pool_layout():
    """The pool is the top of guest RAM: the loaded image must end below it, and with an AHCI
    store the guest RAM under it keeps the 4 GiB floor."""
    from aura import windows as w
    base, top = 0x8_0000_0000, 0x9_2000_0000
    pool = w.gpu_pool(base, top, 256)
    w.gpu_pool_check_layout(pool, base, pool[0])                 # ends exactly at the pool
    w.gpu_pool_check_layout(pool, base, base + 0x100_0000, w.AHCI_MIN_GUEST)
    with pytest.raises(ValueError, match="inside the GPU pool"):
        w.gpu_pool_check_layout(pool, base, pool[0] + 0x4000)
    with pytest.raises(ValueError):
        w.gpu_pool_check_layout(pool, base, base - 1)
    small = w.gpu_pool(0x8_0000_0000, 0x9_0000_0000, 256)          # 4 GiB window, 3.75 below
    with pytest.raises(ValueError, match="Windows floor"):
        w.gpu_pool_check_layout(small, 0x8_0000_0000, 0x8_0100_0000, w.AHCI_MIN_GUEST)
    w.gpu_pool_check_layout(small, 0x8_0000_0000, 0x8_0100_0000)   # no AHCI: no floor


class _Exit(Exception):
    pass


def _gpu_admit_under_test(results, image_end=None, g2_grant="none"):
    """windows_t8152_gpu_admit() from run_guest.py, run against a recording proxy stand-in."""
    import ast
    import types
    from aura import windows as w
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "windows_t8152_gpu_admit")
    calls = []

    class RemoteError(Exception):
        pass

    class Proxy:
        def hv_windows_gpu(self, op, *a):
            calls.append(("gpu", op, *a))
            if op == w.GPU_OP_STATUS:
                return {w.GPU_STATUS_HANDOFF_IPA: 0x8_0600_0000,
                        w.GPU_STATUS_HANDOFF_SIZE: 0x4000, w.GPU_STATUS_RANGES: 40}.get(a[0], 0)
            return results.get(op, 0)

        def hv_windows_usb(self, op, *a):
            calls.append(("usb", op, *a))
            return 0

    def sys_exit(message):
        calls.append(("exit", message))
        raise _Exit(message)

    base, top = 0x8_0000_0000, 0x9_2000_0000
    hv = types.SimpleNamespace(windows_guest_ram=(base, top),
                               tba=types.SimpleNamespace(top_of_kernel_data=image_end or
                                                         base + 0x200_0000),
                               set_windows_gpu_handshake=lambda v: calls.append(("hs", v)))
    env = {"p": Proxy(), "sys": types.SimpleNamespace(exit=sys_exit),
           "ProxyRemoteError": RemoteError,
           "args": types.SimpleNamespace(windows_gpu_pool_mib=256, windows_gpu_g2_grant=g2_grant),
           "print": lambda *a: None}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "run_guest.py", "exec"), env)
    return env["windows_t8152_gpu_admit"], hv, calls


@pytest.mark.parametrize("case", ["admit", "hook", "pool"])
def test_launcher_gpu_refusal_detaches_usb(case):
    """Every GPU refusal after USB ATTACH detaches USB before exiting, as the USB and hv.start()
    failure paths do; without USB nothing is detached."""
    from aura import windows as w
    results = {w.GPU_OP_ADMIT: -3 & ((1 << 64) - 1)} if case == "admit" else \
        {w.GPU_OP_HOOK: -5 & ((1 << 64) - 1)} if case == "hook" else {}
    image_end = 0x9_1800_0000 if case == "pool" else None
    for ports in ((1,), ()):
        admit, hv, calls = _gpu_admit_under_test(results, image_end)
        with pytest.raises(_Exit):
            admit(hv, "log", ports, None)
        kinds = [c[0] for c in calls]
        assert kinds[-1] == "exit" and "hs" not in kinds
        detach = [c for c in calls if c[:2] == ("usb", w.USB_OP_DETACH)]
        if ports:
            assert detach and kinds.index("usb") == len(kinds) - 2
            assert "USB was detached again" in calls[-1][1]
        else:
            assert not detach
        if case == "pool":
            assert not any(c[0] == "gpu" for c in calls)   # refused before ADMIT


def test_launcher_gpu_success_keeps_usb():
    from aura import windows as w
    admit, hv, calls = _gpu_admit_under_test({})
    admit(hv, "log", (1,), None)
    assert ("usb", w.USB_OP_DETACH) not in calls
    assert ("hs", w.gpu_handshake(0x8_0600_0000, 0x4000)) in calls
    assert not any(c[0] == "exit" for c in calls)


def test_g1_needs_a_boot_wide_marker():
    """'ADMIT once' lasts one carrier session.  G0 starts no firmware, so that is enough today;
    the first change that powers the GPU or lets RUN through must also add a boot-wide marker
    that survives a carrier reload (plan 2.1(1), 7 'Do not')."""
    code = re.sub(r"/\*.*?\*/", "", (WIN / "hv_gpu.c").read_text(), flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)
    starts_firmware = re.search(r"GPU_HS_(POWERED|ASC_RUN)|ASC_CPU_CONTROL", code)
    if starts_firmware:
        assert "HV_GPU_BOOT_MARKER" in code, \
            "hv_gpu.c powers or starts the GPU without a boot-wide admission marker"


def test_launcher_probe_request():
    """G1: probe needs CAPS2 bit 4, the resident's recipe and a free boot marker, all checked
    before anything is uploaded."""
    from aura import windows as w
    caps2 = w.CAPS2_MAGIC | w.CAPS2_GPU | w.CAPS2_GPU_PROBE
    w.validate_gpu_request("probe", w.T8152_CHIP, w.J804_BOARD_ID, caps2, 1, 0)
    w.validate_gpu_request("log", w.T8152_CHIP, w.J804_BOARD_ID, w.CAPS2_MAGIC | w.CAPS2_GPU)
    for c2, recipe, boot, why in (
            (w.CAPS2_MAGIC | w.CAPS2_GPU, 1, 0, "bit 4"),
            (caps2, 0, 0, "without the private GPU power recipe"),
            (caps2, 1, 0x3, "already used: marker claimed, powered"),
            (caps2, 1, w.GPU_BOOT_FOREIGN, "holds no GPU marker"),
            (caps2, 1, w.GPU_BOOT_NO_SLOT, "has no /chosen/memory-map/MemoryMapReserved-15"),
            (caps2, 1, None, "unknown")):
        with pytest.raises(ValueError, match=why):
            w.validate_gpu_request("probe", w.T8152_CHIP, w.J804_BOARD_ID, c2, recipe, boot)
    assert w.describe_gpu_boot(0x33) == "marker claimed, powered, forwarding, ended"
    e = w.gpu_map_entry(SGX + 0xd04000, 3, 2 | 0 << 8 | 1 << 16 | 1 << 24, 0x5100)
    assert e == {"addr": SGX + 0xd04000, "count": 3, "bits": 32, "write": False,
                 "disposition": "passed", "class": "sgx", "first": 0x5100}
    assert w.describe_gpu(-8 & ((1 << 64) - 1)).startswith("this boot's GPU firmware session")
    run_guest = (REPO / "proxyclient/tools/run_guest.py").read_text()
    plan = run_guest.split("def windows_t8152_gpu_plan(", 1)[1].split("\ndef ", 1)[0]
    assert "GPU_STATUS_POWER_RECIPE" in plan and "GPU_STATUS_BOOT" in plan


def test_launcher_probe_order():
    """ADMIT, then POWER, then PROBE, then the Mu handshake; nothing else."""
    from aura import windows as w
    admit, hv, calls = _gpu_admit_under_test({})
    admit(hv, "probe", (1,), None)
    gpu_ops = [c[1] for c in calls if c[0] == "gpu" and c[1] != w.GPU_OP_STATUS]
    assert gpu_ops == [w.GPU_OP_ADMIT, w.GPU_OP_POWER, w.GPU_OP_PROBE]
    kinds = [c[0] for c in calls]
    assert kinds.index("hs") > max(i for i, c in enumerate(calls)
                                   if c[0] == "gpu" and c[1] == w.GPU_OP_PROBE)
    assert ("usb", w.USB_OP_DETACH) not in calls and not any(c[0] == "exit" for c in calls)


@pytest.mark.parametrize("failing", ["power", "probe"])
def test_launcher_probe_refusal_stops_and_detaches(failing):
    """A POWER or PROBE refusal runs STOP (which never touches the GPU) and detaches USB; no
    handshake is written."""
    from aura import windows as w
    op = w.GPU_OP_POWER if failing == "power" else w.GPU_OP_PROBE
    admit, hv, calls = _gpu_admit_under_test({op: -8 & ((1 << 64) - 1)})
    with pytest.raises(_Exit):
        admit(hv, "probe", (1,), None)
    kinds = [c[0] for c in calls]
    assert "hs" not in kinds and kinds[-1] == "exit"
    stop = calls.index(("gpu", w.GPU_OP_STOP))
    assert stop < calls.index(("usb", w.USB_OP_DETACH))
    assert "reboot the M6" in calls[-1][1] or "boot marker" in calls[-1][1]


# ---- G2 launcher ------------------------------------------------------------------------------

def test_launcher_g2_request():
    """G2: g2 needs CAPS2 bits 4 and 5, both private recipes and a free boot marker before the
    upload; the owner grants are explicit, named, and only valid with g2."""
    from aura import windows as w
    assert w.parse_windows_gpu("G2") == "g2"
    assert w.parse_windows_gpu_g2_grant("none", "probe") == 0
    assert w.parse_windows_gpu_g2_grant("", "g2") == 0
    assert w.parse_windows_gpu_g2_grant("fw-mmio", "g2") == 1
    assert w.parse_windows_gpu_g2_grant("boot-control, FW-MMIO", "g2") == 3
    for bad, mode in (("fw-mmio", "probe"), ("all", "g2"), ("fw-mmio,fw-mmio", "g2"),
                      ("boot", "g2")):
        with pytest.raises(ValueError):
            w.parse_windows_gpu_g2_grant(bad, mode)
    assert w.describe_gpu_g2_grants(3) == "fw-mmio+boot-control"
    assert w.describe_gpu_g2_grants(0) == "none"
    caps2 = w.CAPS2_MAGIC | w.CAPS2_GPU | w.CAPS2_GPU_PROBE | w.CAPS2_GPU_G2
    w.validate_gpu_request("g2", w.T8152_CHIP, w.J804_BOARD_ID, caps2, 1, 0, 1)
    for c2, recipe, boot, g2r, why in (
            (caps2 & ~w.CAPS2_GPU_G2, 1, 0, 1, "bit 6"),
            (caps2 & ~w.CAPS2_GPU_PROBE, 1, 0, 1, "bit 4"),
            (caps2, 1, 0, 0, "without the private G2 recipe"),
            (caps2, 1, 0, None, "without the private G2 recipe"),
            (caps2, 0, 0, 1, "without the private GPU power recipe"),
            (caps2, 1, 0xc3, 1, "already used: marker claimed, powered, boot-control step done, G2 armed")):
        with pytest.raises(ValueError, match=why):
            w.validate_gpu_request("g2", w.T8152_CHIP, w.J804_BOARD_ID, c2, recipe, boot, g2r)
    assert w.describe_gpu(-11 & ((1 << 64) - 1)).startswith("G2: no private G2 recipe")
    e = w.gpu_map_entry(MBOX + 0x110, 2, 4 | 1 << 8 | 6 << 16 | 6 << 24, 0x81 << 48)
    assert e["disposition"] == "blocked" and e["class"] == "mailbox" and e["bits"] == 128
    assert w.GPU_CLASSES[12] == "fw-mmio" and w.GPU_DISPOSITIONS[5] == "locked"
    run_guest = (REPO / "proxyclient/tools/run_guest.py").read_text()
    plan = run_guest.split("def windows_t8152_gpu_plan(", 1)[1].split("\ndef ", 1)[0]
    assert "GPU_STATUS_G2_RECIPE" in plan and "parse_windows_gpu_g2_grant" in plan
    assert "parser.add_argument('--windows-gpu-g2-grant', default='none'" in run_guest


def test_launcher_g2_order():
    """ADMIT, G2 with the owner's grants, POWER, PROBE, then the Mu handshake."""
    from aura import windows as w
    admit, hv, calls = _gpu_admit_under_test({}, g2_grant="fw-mmio,boot-control")
    admit(hv, "g2", (1,), None)
    gpu_ops = [c[1:] for c in calls if c[0] == "gpu" and c[1] != w.GPU_OP_STATUS]
    assert gpu_ops == [(w.GPU_OP_ADMIT, 0x9_1000_0000, 0x1000_0000), (w.GPU_OP_G2, 3),
                       (w.GPU_OP_POWER,), (w.GPU_OP_PROBE,)]
    kinds = [c[0] for c in calls]
    assert kinds.index("hs") > max(i for i, c in enumerate(calls)
                                   if c[0] == "gpu" and c[1] == w.GPU_OP_PROBE)
    assert not any(c[0] == "exit" for c in calls)
    # Without grants the guards still arm (op 7 with 0).
    admit, hv, calls = _gpu_admit_under_test({})
    admit(hv, "g2", (), None)
    assert ("gpu", w.GPU_OP_G2, 0) in calls


@pytest.mark.parametrize("failing", ["g2", "power", "probe"])
def test_launcher_g2_refusal_stops_and_detaches(failing):
    from aura import windows as w
    op = {"g2": w.GPU_OP_G2, "power": w.GPU_OP_POWER, "probe": w.GPU_OP_PROBE}[failing]
    admit, hv, calls = _gpu_admit_under_test({op: -11 & ((1 << 64) - 1)}, g2_grant="fw-mmio")
    with pytest.raises(_Exit):
        admit(hv, "g2", (1,), None)
    kinds = [c[0] for c in calls]
    assert "hs" not in kinds and kinds[-1] == "exit"
    assert calls.index(("gpu", w.GPU_OP_STOP)) < calls.index(("usb", w.USB_OP_DETACH))
    if failing == "g2":
        assert ("gpu", w.GPU_OP_POWER) not in calls and "not powered" in calls[-1][1]


def test_launcher_names_the_reserved_pool():
    """After a probe session the pool slot keeps later guest RAM off the GPU-reachable pool, and
    the host check says why."""
    from aura import windows as w
    problems = w.t8152_window_conflicts(0x8_0000_0000, 0x9_2000_0000, 0x8_0000_0000,
                                        0x2_0000_0000, {}, {w.GPU_POOL_SLOT: POOL}, ())
    assert len(problems) == 1 and "reboot the M6" in problems[0]
    assert w.t8152_window_conflicts(0x8_0000_0000, 0x9_2000_0000, 0x8_0000_0000, 0x2_0000_0000,
                                    {}, {w.GPU_POOL_SLOT: (0, 0)}, ()) == []


# ---- G1 review fixes ---------------------------------------------------------------------------

def _input_attach_under_test(query_fails=False, attach_result=0):
    """windows_t8152_input_attach() from run_guest.py against a recording proxy stand-in."""
    import ast
    import types
    from aura import windows as w
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "windows_t8152_input_attach")
    calls = []

    class RemoteError(Exception):
        pass

    class Proxy:
        def hv_windows_input(self, op, *a):
            calls.append(("input", op, *a))
            if op == w.INPUT_OP_QUERY:
                if query_fails:
                    raise RemoteError("Bad Command")
                return {w.INPUT_Q["version"]: w.INPUT_VERSION, w.INPUT_Q["base"]: w.INPUT_BASE,
                        w.INPUT_Q["size"]: w.INPUT_SIZE, w.INPUT_Q["intid"]: w.INPUT_GSIV,
                        w.INPUT_Q["features"]: w.INPUT_FEATURES_REPORTS}[a[0]]
            if op == w.INPUT_OP_ATTACH:
                return attach_result
            return 0

        def hv_windows_usb(self, op, *a):
            calls.append(("usb", op, *a))
            return 0

        def hv_windows_gpu(self, op, *a):
            calls.append(("gpu", op, *a))
            return 0

    def sys_exit(message):
        calls.append(("exit", message))
        raise _Exit(message)

    env = {"p": Proxy(), "sys": types.SimpleNamespace(exit=sys_exit),
           "ProxyRemoteError": RemoteError, "print": lambda *a: None}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), "run_guest.py", "exec"), env)
    hv = types.SimpleNamespace(set_windows_input_handshake=lambda v: calls.append(("hs", v)))
    return env["windows_t8152_input_attach"], hv, calls


@pytest.mark.parametrize("gpu_mode", ["off", "admit", "probe", "g2"])
@pytest.mark.parametrize("failing", ["query", "attach"])
def test_launcher_input_refusal_detaches_usb(gpu_mode, failing):
    """An input refusal after USB ATTACH detaches USB (and, with probe, runs GPU STOP first)
    instead of dying on a NameError: the refuse() closure only uses names it was given."""
    from aura import windows as w
    attach, hv, calls = _input_attach_under_test(
        query_fails=failing == "query", attach_result=0 if failing == "query" else 2)
    with pytest.raises(_Exit):
        attach(hv, ["host"], (1,), gpu_mode)
    kinds = [c[0] for c in calls]
    assert kinds[-1] == "exit" and "hs" not in kinds
    detach = calls.index(("usb", w.USB_OP_DETACH))
    stops = [i for i, c in enumerate(calls) if c[:2] == ("gpu", w.GPU_OP_STOP)]
    if gpu_mode in ("probe", "g2"):
        assert stops and stops[0] < detach
    else:
        assert not stops
    # The default (no GPU) still detaches USB.
    attach, hv, calls = _input_attach_under_test(query_fails=True)
    with pytest.raises(_Exit):
        attach(hv, ["host"], (1,))
    assert ("usb", w.USB_OP_DETACH) in calls and not any(c[0] == "gpu" for c in calls)


def test_launcher_reports_after_a_host_stop():
    """Ctrl-C is the only way out of the M6 probe build: the abort RuntimeError is caught (only
    when the guest was aborted) and the USB / GPU / input status, including the G1 access map,
    is still read from the proxy."""
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    t8152 = src[src.index("def run_windows_t8152"):src.index("_windows_caps = windows_resident_caps()")]
    start = t8152.index("result = hv.start()")
    caught = t8152.index("except RuntimeError as exc:", start)
    assert t8152.index("if hv.windows_abort_reason is None:\n            raise", caught) < \
        t8152.index("result = None", caught) < \
        t8152.index("windows_t8152_usb_status(usb_ports)") < \
        t8152.index("windows_t8152_gpu_status(gpu_mode)")
    hv = (REPO / "proxyclient/aura/hv/__init__.py").read_text()
    assert 'raise RuntimeError(f"Windows guest aborted: {self.windows_abort_reason}")' in hv


def test_kboot_refuses_after_a_gpu_session():
    """The boot marker and the pool slot also stop a same-boot kernel on T8152 (the Linux GPU
    path would start the firmware a second time and could use the pool as RAM)."""
    kboot = (REPO / "src/kboot.c").read_text()
    body = kboot.split("static int dt_prepare_t8152(void)\n{", 1)[1]
    assert body.index("kboot_t8152_gpu_session_check()") < body.index("aurora,minimal-boot")
    gpu = (REPO / "src/kboot_gpu_t8152.c").read_text()
    check = gpu.split("int kboot_t8152_gpu_session_check(void)\n{", 1)[1].split("\n}", 1)[0]
    assert "HV_GPU_BOOT_MARKER" in check and "HV_GPU_POOL_SLOT" in check and "return -1" in check
    assert '#include "hv/windows/hv_gpu.h"' in gpu


def test_g1_read_order_is_recipe_data():
    """The acknowledge/state read order (ledger B-01, upstream) comes from the private recipe; the
    tree neither hard-codes it nor states it as fact."""
    hdr = (WIN / "gpu_t8152_power.h").read_text()
    assert "u32 read_order;" in hdr
    code = (WIN / "gpu_t8152_power.c").read_text()
    assert "r->read_order == GPU_PSD_READ_ACK_FIRST ||" in code
    for name in ("gpu_t8152_power.c", "gpu_t8152_power.h", "hv_gpu.c", "hv_gpu.h"):
        assert "consume" not in (WIN / name).read_text(), name
    doc = (REPO / "docs/windows-t8152.md").read_text()
    g1 = doc.split("## G1:", 1)[1].split("\n## ", 1)[0]
    assert "consumes the acknowledge" not in g1 and "does not survive" not in g1
