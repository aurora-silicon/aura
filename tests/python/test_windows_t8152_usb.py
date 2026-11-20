# SPDX-License-Identifier: MIT
"""D4: usb-drd1 (and drd3) xHCI passthrough for the J804 Windows guest, host only.

Failure modes F1-F14 are listed in windows-m6/D4-USB-DESIGN.md section 9; the tests name the
ones they cover.  Nothing here touches hardware, and every address window is synthetic or one
of the repo's existing constants (j873.c, J804Windows.h); the only machine facts are the D4
hardware constants PROVENANCE.md names (AIC 1692/1834, the DART PARAMS3 fields).

1. The DART table builder, src/hv/windows/hv_usb_dart.c, compiled unchanged (ASan + UBSan) and
   replayed by tests/e2e/windows_usb_dart_host.c, against an independent model written here
   from the format alone (io-pgtable-dart.c APPLE_DART2_* masks and dart_alloc_pgtable, cited):
   counts, encoders, a full sweep, faults, refusals, byte-for-byte arenas (F5, F6).
2. The carrier glue, hv_usb.c, compiled unchanged against stub headers and driven by
   tests/e2e/windows_usb_host.c over a fake register file: every refusal writes nothing
   (F1-F5), the success path writes exactly the design's registers in order, F7 and stage-2
   failures tear down, teardown/the reset-path stop/the storm guard/the report (F7-F10, F13,
   F14).
3. The stage-2 policy, hv_t8152.c, compiled unchanged with the policy test's stand-ins and a
   synthetic ADT with usb-drd, atc-phy and dart-usb nodes.
4. The alias table, hv_aic_alias.c, compiled unchanged: 995/997 owned only while attached (F11).
5. Source rules: the T8152 IRQ path, every exit path, configure, the proxy op, the Makefile.
6. The launcher: --windows-usb parsing and refusals, FD markers in both directions, CAPS2, the
   HV lifecycle with the configure flag and the /chosen marker (F12), and Mu's 40-byte
   /chosen/aura,windows-usb-v1 handshake (layout, refusals, the guest ADT, Mu's header and
   parser when AURA_MU_TREE is set).
"""

import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"
sys.path.insert(0, str(REPO / "proxyclient"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from aura import windows as w  # noqa: E402

CFLAGS = ["-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
          "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


def code(text):
    return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def clang():
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    return cc


# ---- 1. the DART table builder against an independent model --------------------------------

PAGE = 0x4000
PTES = 2048
DVA_LIMIT = 1 << 42


class DartModel:
    """The Gen3 four-level identity map, written from the format (TTBR -> L1 [46:36] -> L2
    [35:25] -> leaf [24:14]; table = ((pa >> 4) & GENMASK(37, 10)) | 1; leaf adds SUBPAGE_END
    0xfff at [51:40]; NO_READ/NO_WRITE/NO_CACHE (bits 3..1) clear).  Shares no code with the C."""

    ADDR = ((1 << 38) - 1) & ~((1 << 10) - 1)

    @classmethod
    def table(cls, pa):
        return ((pa >> 4) & cls.ADDR) | 1

    @classmethod
    def leaf(cls, pa):
        return (0xfff << 40) | ((pa >> 4) & cls.ADDR) | 1

    @staticmethod
    def count(start, end):
        return 1 + ((end - 1) >> 36) - (start >> 36) + 1 + ((end - 1) >> 25) - (start >> 25) + 1

    @classmethod
    def build(cls, start, end, arena_pa):
        """{table index: {entry index: value}} in the C layout: root, L2s, leaves."""
        l1_lo, l1_hi = start >> 36, (end - 1) >> 36
        b_lo, b_hi = start >> 25, (end - 1) >> 25
        n_l2 = l1_hi - l1_lo + 1
        tables = {}
        for block in range(b_lo, b_hi + 1):
            l2 = 1 + (block >> 11) - l1_lo
            leaf = 1 + n_l2 + block - b_lo
            tables.setdefault(0, {})[(block >> 11) & 0x7ff] = cls.table(arena_pa + l2 * PAGE)
            tables.setdefault(l2, {})[block & 0x7ff] = cls.table(arena_pa + leaf * PAGE)
            lo, hi = max(start, block << 25), min(end, (block + 1) << 25)
            entries = tables.setdefault(leaf, {})
            for pa in range(lo, hi, PAGE):
                entries[(pa >> 14) & 0x7ff] = cls.leaf(pa)
        return tables

    @classmethod
    def blob(cls, tables, n):
        out = bytearray(n * PAGE)
        for t, entries in tables.items():
            for i, v in entries.items():
                struct.pack_into("<Q", out, t * PAGE + i * 8, v)
        return bytes(out)

    @classmethod
    def walk(cls, blob, arena_pa, dva):
        if dva >= DVA_LIMIT:
            return None
        n = len(blob) // PAGE

        def entry(table_pa, index):
            off = table_pa - arena_pa
            if off < 0 or off % PAGE or off // PAGE >= n:
                return None
            return struct.unpack_from("<Q", blob, off + index * 8)[0]

        table = arena_pa
        for shift in (36, 25):
            pte = entry(table, (dva >> shift) & 0x7ff)
            if not pte or not pte & 1 or pte & ~(cls.ADDR | 1):
                return None
            table = (pte & cls.ADDR) << 4
        pte = entry(table, (dva >> 14) & 0x7ff)
        if not pte or not pte & 1 or pte & 0xe or pte >> 52 or (pte >> 40) & 0xfff != 0xfff:
            return None
        return ((pte & cls.ADDR) << 4) | (dva & (PAGE - 1))


@pytest.fixture(scope="module")
def dart_replay(tmp_path_factory):
    cc = clang()
    out = tmp_path_factory.mktemp("usb-dart")
    (out / "types.h").write_text("#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n"
                                 "#include <stdint.h>\ntypedef unsigned long u64;\n"
                                 "typedef uint32_t u32;\ntypedef uint8_t u8;\n")
    for name in ("hv_usb_dart.c", "hv_usb_dart.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "usb-dart-host"
    build = subprocess.run([cc, *CFLAGS, "-iquote", str(out), str(out / "hv_usb_dart.c"),
                            str(REPO / "tests/e2e/windows_usb_dart_host.c"), "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary), str(out)], capture_output=True, text=True, timeout=300)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    return result, lines, out


def test_dart_replay_passes(dart_replay):
    result, lines, _ = dart_replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert lines[-1] == {"case": "done", "result": "passed", "failures": 0}


def test_dart_table_counts_match_the_model(dart_replay):
    # 4.5 GiB -> 146 (design 4.6), 17.52 GiB -> 563, one page -> 3, a 64 GiB crossing -> 2 L2s.
    _, lines, _ = dart_replay
    counts = [line for line in lines if line["case"] == "count"]
    assert [c["tables"] for c in counts] == [146, 563, 3, 19, 4]
    for c in counts:
        assert c["tables"] == DartModel.count(c["start"], c["end"])
    assert sum(1 for c in counts if (c["end"] - 1) >> 36 != c["start"] >> 36) == 1


def test_dart_encoders(dart_replay):
    _, lines, _ = dart_replay
    enc = next(line for line in lines if line["case"] == "encoders")
    # The leaf format: page P -> 0x000fff << 40 | (P >> 4) | VALID, SUBPAGE_END at [51:40].
    # (D4-USB-DESIGN.md 4.4's worked vector has a nibble slip in that field; the synthetic page
    # here shows the right placement.)
    assert enc["leaf"] == DartModel.leaf(0x10100000000) == 0x000fff1010000001
    assert enc["table"] == DartModel.table(0x10008000000) == 0x1000800001
    assert enc["ttbr"] == 1 | ((0x10008000000 >> 14) << 2)
    # aura's dart_gen3_tlb_invalidate and linux-j804 apple-dart-t8152-gen3-tlbi.py: route 2.
    assert enc["tlbi_r2_s0"] == 1 << 60 | 2 << 56
    assert enc["tlbi_r2_s1"] == 1 << 60 | 2 << 56 | 1 << 48
    assert enc["tlbi_r2_s14"] == 1 << 60 | 2 << 56 | 14 << 48
    assert (enc["tcr_on"], enc["tcr_idle"]) == (0x9, 0x1)


@pytest.mark.parametrize("name", ["w45", "cross", "ragged"])
def test_dart_arena_matches_the_model_byte_for_byte(dart_replay, name):
    _, lines, out = dart_replay
    info = next(line for line in lines if line["case"] == "window" and line["name"] == name)
    blob = (out / f"{name}.bin").read_bytes()
    assert len(blob) == info["tables"] * PAGE
    model = DartModel.blob(DartModel.build(info["start"], info["end"], info["arena_pa"]),
                           info["tables"])
    assert blob == model
    # The C side swept every page; the model walks a sample and the faults.
    start, end, pa = info["start"], info["end"], info["arena_pa"]
    assert info["swept"] == info["leaves"] == (end - start) // PAGE
    for dva in (start, start + 0x1234, end - 1, (start | 0x1ffffff) + 1, end - PAGE):
        if start <= dva < end:
            assert DartModel.walk(blob, pa, dva) == dva
    for dva in (start - PAGE, end, 0, pa, pa + len(blob) - 8, (1 << 41) - PAGE,
                (1 << 42) - PAGE, end + (4 << 30)):
        if not start <= dva < end:
            assert DartModel.walk(blob, pa, dva) is None
    # Coherent: no leaf sets NO_CACHE (bit 1), NO_WRITE or NO_READ.
    leaves = [v for (v,) in struct.iter_unpack("<Q", blob[PAGE:]) if v >> 40 == 0xfff]
    assert len(leaves) == info["leaves"] and not any(v & 0xe for v in leaves)


def test_dart_full_window_model_sweep():
    # The model alone over a whole synthetic 4.5 GiB window: every page maps to itself.
    start, end, pa = 0x10100000000, 0x10220000000, 0x10008000000
    n = DartModel.count(start, end)
    blob = DartModel.blob(DartModel.build(start, end, pa), n)
    for dva in range(start, end, 64 * PAGE):
        assert DartModel.walk(blob, pa, dva + 0x10) == dva + 0x10
    assert DartModel.walk(blob, pa, end) is None and DartModel.walk(blob, pa, start - 1) is None


# ---- 2. the carrier glue over a fake register file -----------------------------------------

GLUE_STUBS = {
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t s32;
typedef long s64;
''',
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define BIT(x) (1UL << (x))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
u32 read32(u64 addr);
void write32(u64 addr, u32 val);
void mask32(u64 addr, u32 clear, u32 set);
int poll32(u64 addr, u32 mask, u32 target, u32 timeout);
extern u64 stub_cntpct, stub_cntfrq;
#define stub_mrs_CNTPCT_EL0 stub_cntpct
#define stub_mrs_CNTFRQ_EL0 stub_cntfrq
#define mrs(reg) (stub_mrs_##reg)
#define sysop(op) do { } while (0)
typedef struct { int held; } spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = {0}
static inline void spin_lock(spinlock_t *l) { if (l->held) __builtin_trap(); l->held = 1; }
static inline void spin_unlock(spinlock_t *l) { if (!l->held) __builtin_trap(); l->held = 0; }
''',
    "adt.h": r'''
#pragma once
#include "types.h"
extern void *adt;
int adt_path_offset_trace(const void *adt, const char *path, int *offsets);
int adt_get_reg(const void *adt, int *path, const char *prop, int idx, u64 *addr, u64 *size);
const void *adt_getprop(const void *adt, int nodeoffset, const char *name, u32 *lenp);
bool adt_is_compatible(const void *adt, int nodeoffset, const char *compat);
''',
    "dart.h": "#pragma once\nint dart_gen3_tlbi_adt(const char *path, int instance, int device);\n",
    "heapblock.h": '#pragma once\n#include "types.h"\nuintptr_t heapblock_high_water(void);\n'
                   "uintptr_t heapblock_limit(void);\n",
    "hv.h": '#pragma once\n#include "types.h"\nint hv_map_hw(u64 from, u64 to, u64 size);\n'
            "int hv_unmap(u64 from, u64 size);\nbool hv_windows_guest_entered(void);\n",
    "hv_aic_alias.h": '#pragma once\n#include "types.h"\nvoid hv_aic_alias_t8152_usb(u32 port_mask);\n',
    "hv_t8152.h": r'''
#pragma once
#include "types.h"
bool hv_t8152_guest_ram(u64 *base, u64 *size);
int hv_t8152_usb_allow(int port, u64 xhci);
void hv_t8152_usb_revoke(int port);
int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name);
''',
    "malloc.h": "#pragma once\n#include <stddef.h>\nvoid *stub_memalign(size_t align, size_t size);\n"
                "void stub_free(void *ptr);\n#define memalign stub_memalign\n#define free stub_free\n",
    "memory.h": "#pragma once\n#include <stddef.h>\nvoid dc_cvac_range(void *addr, size_t length);\n",
    "soc.h": '#pragma once\n#include "types.h"\n#define T8152 0x8152\nextern u32 chip_id, board_id;\n',
    "string.h": "#pragma once\n#include <string.h>\n",
    "windows_compat.h": '#pragma once\n#include "types.h"\nvoid aic_set_mask(int irq, bool active);\n'
                        "u32 aic_irq_config(int irq);\n",
}

REFUSALS = {
    "not_configured": -1, "drd0": -2, "drd0_and_drd1": -2, "drd2": -2, "empty": -2,
    "high_bits": -2, "not_j804": -1, "no_guest_ram": -1, "f1_not_up": -3, "f1_no_utmi": -3,
    "f1_gctl_device": -3, "adt_wrong_controller": -4, "adt_wrong_line": -4,
    "f2_protected": -5, "f2_locked": -5, "f2_streams_live": -5, "f2_ttbr_valid": -5,
    "f2_tcr_live": -5, "f2_tcr_bypass": -5, "f3_page_size": -5, "f3_params3": -5,
    "f3_one_sid": -5, "f4_running": -6, "f4_not_halted": -6, "f4_cnr": -6, "f4_dbc": -6,
    "f4_runtime_outside": -6, "f4_doorbells_outside": -6, "f4_chain_loop": -6,
    "f4_cap_outside": -6, "drd3_probe_fails_after_drd1": -5, "f5_no_heap_room": -7,
    "f5_placement": -7, "guest_entered": -1, "again_after_teardown": -1,
}


@pytest.fixture(scope="module")
def glue_replay(tmp_path_factory):
    cc = clang()
    out = tmp_path_factory.mktemp("usb-glue")
    for name, text in GLUE_STUBS.items():
        (out / name).write_text(text)
    for name in ("hv_usb.c", "hv_usb.h", "hv_usb_dart.c", "hv_usb_dart.h"):
        shutil.copy2(WIN / name, out / name)
    shutil.copy2(REPO / "src/j873.h", out / "j873.h")
    binary = out / "usb-glue-host"
    build = subprocess.run([cc, *CFLAGS, "-DHV_USB_HOST_TEST", "-iquote", str(out),
                            str(out / "hv_usb.c"),
                            str(out / "hv_usb_dart.c"), str(REPO / "tests/e2e/windows_usb_host.c"),
                            "-o", str(binary)], capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)
    cases = {}
    current = None
    for line in result.stdout.splitlines():
        if line.startswith("CASE "):
            current = line[5:]
            cases[current] = []
        elif current is not None:
            cases[current].append(line)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    artifact = os.environ.get("AURA_USB_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text(result.stdout)
    return result, lines, cases


def events(block):
    return [line[3:] for line in block if line.startswith("EV ")]


def test_glue_replay_passes(glue_replay):
    result, lines, _ = glue_replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert lines[-1] == {"case": "done", "result": "passed", "failures": 0}


def test_every_refusal_writes_nothing(glue_replay):
    # F1-F5 and the port/mask rules: the resident says why and touches no register.
    _, lines, cases = glue_replay
    got = {line["case"]: line for line in lines if line["case"] in REFUSALS}
    assert set(got) == set(REFUSALS)
    for name, want in REFUSALS.items():
        assert got[name]["ret"] == want and got[name]["writes"] == 0, name
        assert not [e for e in events(cases[name])
                    if e.split()[0] in ("W", "AIC", "TLBI", "MAP", "ALLOW", "ALIAS")], name
    text = "\n".join(sum(cases.values(), []))
    assert "usb-drd0 is the proxy's link and never passed through" in text
    assert "xHCI Debug Capability" in text
    assert "not an idle stream" in text and "not idle and unprotected" in text
    assert "is not in host mode as aura's j804_usb_prep() leaves it" in text
    assert "no UTMI clock events" in text


def test_success_writes_exactly_the_design_sequence(glue_replay):
    # Design 4.7: SUSPHY (only if set), mask 1692 and the DART line, TTBR then TCR 0x9 per
    # stream, TLBI per stream, ENABLE_STREAMS 0x3 / 0x2 (never 0xffffffff), then stage 2 and
    # the alias.  Instance 1 SID 0 is never touched.
    _, _, cases = glue_replay
    ev = [e for e in events(cases["success"]) if not e.startswith(("ALLOC", "RESERVE"))]
    ttbr = ev[3].split()[2]
    assert ev == [
        "W 0x40a43c200 0x102500",
        "AIC 1692 mask", "AIC 1600 mask",
        f"W 0x40b021400 {ttbr}", "W 0x40b021000 0x9",
        f"W 0x40b021404 {ttbr}", "W 0x40b021004 0x9",
        f"W 0x40b041404 {ttbr}", "W 0x40b041004 0x9",
        "TLBI /arm-io/dart-usb1 0 0", "TLBI /arm-io/dart-usb1 0 1", "TLBI /arm-io/dart-usb1 1 1",
        "W 0x40b020c00 0x3", "W 0x40b040c00 0x2",
        "ALLOW 1 0x40a430000", "MAP 0x40a430000 0x40a430000 0x8000",
        "ALIAS 0x2",
    ]
    assert int(ttbr, 16) & 1
    alloc = events(cases["success"])
    assert alloc.index("ALLOC 0x10000") < alloc.index("RESERVE 0x10000 USB DART tables (D4)") \
        < alloc.index(ev[0])
    assert not any("0x40a43c" in e and "0x40a43c200" not in e for e in ev)  # no other global
    assert "AIC 1692 -> SPI 995" in "\n".join(cases["success"])


def test_commit_failures_tear_down(glue_replay):
    # F7 (TCR 0x9 does not read back), a failed TLBI, a refused stage-2 allow or map: the
    # streams go back to idle, stage 2 is restored, the tables are freed, nothing is owned.
    _, _, cases = glue_replay
    for name in ("f7_readback", "tlbi_fails", "allow_fails", "map_fails",
                 "both_ports_commit_fails"):
        ev = events(cases[name])
        assert ev[-1] == "FREE", name
        assert "ALIAS 0x2" not in ev and "ALIAS 0xa" not in ev, name
        assert "W 0x40b020c20 0x1" in ev and "W 0x40b040c20 0x2" in ev, name
    assert "REVOKE 1" not in events(cases["f7_readback"])
    assert "REVOKE 1" in events(cases["map_fails"])
    assert "UNMAP 0x40a430000 0x8000" not in events(cases["map_fails"])
    both = events(cases["both_ports_commit_fails"])
    assert both.index("MAP 0x40a430000 0x40a430000 0x8000") < both.index(
        "UNMAP 0x40a430000 0x8000") and "REVOKE 1" in both
    assert "did not take the four-level identity map" in "\n".join(cases["f7_readback"])


def test_teardown_order(glue_replay):
    # F13 and design 6: withdraw the alias and mask, halt the controller, disable, clear TTBR,
    # idle TCR, TLBI, then stage 2, and the tables last.  A second teardown and the reset-path
    # stop write nothing.
    _, _, cases = glue_replay
    ev = events(cases["teardown"])
    assert ev[0] == "ALIAS 0x0" and ev[1] == "AIC 1692 mask"
    assert ev[2] == "W 0x40a430020 0x0"
    assert ev[3:12] == ["W 0x40b020c20 0x1", "W 0x40b021400 0x0", "W 0x40b021000 0x1",
                        "W 0x40b020c20 0x2", "W 0x40b021404 0x0", "W 0x40b021004 0x1",
                        "W 0x40b040c20 0x2", "W 0x40b041404 0x0", "W 0x40b041004 0x1"]
    assert ev[12:15] == ["TLBI /arm-io/dart-usb1 0 0", "TLBI /arm-io/dart-usb1 0 1",
                         "TLBI /arm-io/dart-usb1 1 1"]
    assert ev[15:] == ["UNMAP 0x40a430000 0x8000", "REVOKE 1", "FREE"]
    emergency = events(cases["emergency"])
    stop = emergency[emergency.index("ALIAS 0x0"):]
    head = stop[:stop.index("FREE")] if "FREE" in stop else stop
    assert "UNMAP 0x40a430000 0x8000" not in head[:16] and "W 0x40a430020 0x0" in head
    assert "controller halted and DART streams disabled before the reset" in \
        "\n".join(cases["emergency"])


def test_one_attach_per_session_and_no_detach_under_a_running_guest(glue_replay):
    # Review: ATTACH is refused once the guest session started (in-session proxy) and after an
    # earlier ATTACH reached the commit, even when teardown cleared everything; DETACH is
    # refused while a started guest holds the port and is a no-op once the session ended.
    _, lines, cases = glue_replay
    got = {line["case"]: line for line in lines}
    assert got["guest_entered"]["writes"] == 0 and got["again_after_teardown"]["writes"] == 0
    assert got["detach_in_session"]["writes"] == 0
    text = "\n".join(cases["detach_in_session"])
    assert "DETACH refused while the guest session owns the controller" in text
    assert "the guest session has started" in text
    assert "already attached this session" in "\n".join(cases["again_after_teardown"])
    # The DART window the launcher puts in aura,windows-usb-v1 is the window the tables map.
    assert (got["success"]["dma_base"], got["success"]["dma_top"]) == \
        (0x10100000000, 0x10100000000 + (64 << 20))
    usb = source("hv_usb.c")
    attach = function_body(usb, "static int attach(u64 mask)")
    assert attach.index("hv_windows_guest_entered()") < attach.index("probe(&ports[i])")
    assert attach.index("session_used = true;") < attach.index("build_tables(ram_base")
    proxy = function_body(usb, "u64 hv_usb_proxy(u64 op, u64 a1, u64 a2)")
    detach = proxy[proxy.index("case HV_USB_OP_DETACH:"):]
    assert detach.index("hv_windows_guest_entered() && __atomic_load_n(&attached_mask") < \
        detach.index('hv_usb_teardown("launcher DETACH")')
    assert "bool hv_windows_guest_entered(void)\n{\n    return hv_windows_entered;\n}" in \
        source("hv.c")


def test_report_storm_and_fatal(glue_replay):
    # F8: a DART exception is logged with its raw record and acknowledged (W1C); F14: a dwc3
    # global that changed is logged (a SUSPHY the carrier cleared is not a drift); F10: a line
    # that never drops is withdrawn; the fatal report uses recorded values.
    _, _, cases = glue_replay
    report = "\n".join(cases["report"])
    assert "DART 1 exception bitmap 0x2" in report
    assert "SID 1 record 0xdead0001 0x0 0x0 0x0" in report
    assert "W 0x40b044000 0x2" in report
    # The ERROR register is acknowledged as well (linux-j804 gen3-irqdump), also on its own.
    assert "DART 1 exception bitmap 0x2 ERROR 0x80000001" in report
    assert "W 0x40b040100 0x80000001" in report and "W 0x40b020100 0x4" in report
    ev = events(cases["report"])
    assert ev.index("W 0x40b044000 0x2") < ev.index("W 0x40b040100 0x80000001")
    assert "dwc3 GCTL 0x30c11006 -> 0x30c11007" in report
    assert "GUSB2PHYCFG0" not in report
    assert "usbsts 0x8 portsc 0x206e1/0x2a0 irq 1000" in report
    storm = "\n".join(cases["storm"])
    assert events(cases["storm"])[:2] == ["ALIAS 0x0", "AIC 1692 mask"]
    assert "the line never drops; AIC 1692 masked and SPI 995 withdrawn" in storm
    assert "HV: fatal: USB usb-drd1: attached 1" in storm and ", storm" in storm


# ---- 3. the stage-2 policy with usb-drd, atc-phy and dart-usb nodes ------------------------

POLICY_HARNESS = r"""
#include <assert.h>
#include <stdlib.h>
#include "adt.h"
#include "aic.h"
#include "ctxr_exec.h"
#include "hv.h"
#include "hv_t8152.h"
#include "xnuboot.h"
#include "utils.h"
#include "string.h"

/* Synthetic machine (the policy test's), with synthetic USB MMIO. */
#define DRAM      0x10000000000UL
#define DRAM_SIZE 0x100000000UL
#define AURA      0x10008000000UL
#define CTXR_D_LO 0x10008000000UL
#define CTXR_D_HI 0x10010000000UL
#define HEAP_TOP  0x10012000000UL
#define HEAP_LIM  0x10018000000UL
#define HOST_TOP  0x10020000000UL
#define WIN_LO    0x10040000000UL
#define WIN_HI    0x100f0000000UL
#define AIC_BASE  0x2a0000000UL
#define DRD0      0x3a0000000UL
#define DRD1      0x3a1000000UL
#define DRD3      0x3a3000000UL
#define PHY1      0x3a1100000UL
#define DART1     0x3a1200000UL
#define TLBI      0x3a8000000UL
#define S2_HW     (1UL | (1UL << 10) | (3UL << 8) | (3UL << 6) | (0xfUL << 2))

u64 fake_mpidr = 0x10100, fake_hcr = 0x80001UL, fake_cntfrq = 1000000000, fake_actlr12;
void fake_msr_SYS_ACTLR_EL12(u64 value) { fake_actlr12 = value; }
u32 board_id = 0x22, chip_id = 0x8152;
char *fake_base = (char *)AURA, *fake_rodata_end = (char *)(AURA + 0x120000);
int boot_cpu_idx = 6;
bool smp_is_alive(int cpu) { return cpu >= 0 && cpu < 12; }
struct aic fake_aic = {AIC_BASE, 3, 0x960}, *aic = &fake_aic;
struct boot_args cur_boot_args;
uintptr_t heapblock_high_water(void) { return HEAP_TOP; }
uintptr_t heapblock_limit(void) { return HEAP_LIM; }
bool ctxr_range(enum ctxr_range range, u64 *start, u64 *end)
{
    if (range != CTXR_RANGE_D) return false;
    *start = CTXR_D_LO; *end = CTXR_D_HI; return true;
}

enum { N_ROOT = 1, N_ARMIO, N_QSPI, N_SEP, N_WDT, N_AIC, N_DRD0, N_DRD1, N_DRD3, N_PHY1, N_DART1,
       N_TLBI, N_CO, N_MM, N_LAST };
static const char *names[] = {"", "", "arm-io", "qspi", "sep", "wdt", "aic", "usb-drd0",
                              "usb-drd1", "usb-drd3", "atc-phy1", "dart-usb1",
                              "dart-tlbi-aperture", "carveout-memory-map", "memory-map"};
static bool drd3_overlap;
static const u64 node_regs[N_LAST][4][2] = {
    [N_QSPI] = {{0x2b0000000, 0x8000}}, [N_SEP] = {{0x2a8000000, 0x74000}},
    [N_WDT] = {{0x2b8000000, 0x4000}}, [N_AIC] = {{AIC_BASE, 0x100000}},
    [N_DRD0] = {{DRD0, 0x11800}, {DRD0 + 0xc000, 0x1000}},
    [N_DRD1] = {{DRD1, 0x11800}, {DRD1 + 0x10000, 0x4000}, {DRD1 + 0xc000, 0x1000}},
    [N_DRD3] = {{DRD3, 0x11800}, {DRD3 + 0x4000, 0x4000}},
    [N_PHY1] = {{PHY1, 0x4000}, {PHY1 + 0x30000, 0x4000}},
    [N_DART1] = {{DART1, 0xc000}, {DART1 + 0x20000, 0xc000}},
    [N_TLBI] = {{TLBI, 0x4000}},
};
static u8 propbuf[4096];
void *adt = propbuf;
static int co_first, co_count, mm_first, mm_count, prop_end = 16;
static void add_prop(const char *name, u64 base, u64 size)
{
    struct adt_property *p = (struct adt_property *)(propbuf + prop_end);
    strncpy(p->name, name, sizeof(p->name));
    p->size = 16;
    memcpy(p->value, &base, 8);
    memcpy(p->value + 8, &size, 8);
    prop_end += sizeof(*p) + 16;
}
int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io")) return N_ARMIO;
    if (!strcmp(path, "/chosen/carveout-memory-map")) return N_CO;
    if (!strcmp(path, "/chosen/memory-map")) return N_MM;
    return -1;
}
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    if (strcmp(path, "/arm-io/aic")) return -1;
    offsets[0] = N_ARMIO; offsets[1] = N_AIC; offsets[2] = 0;
    return N_AIC;
}
const char *adt_get_name(const void *a, int n) { (void)a; return names[n]; }
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a; (void)prop;
    if (path[2] || idx < 0 || idx >= 4) return -1;
    if (path[1] == N_DRD3 && idx == 1 && drd3_overlap) {
        *addr = DRD3 + 0x4000; *size = 0x4000; /* inside the xHCI window */
        return 0;
    }
    if (path[1] == N_DRD3 && idx == 1) {
        *addr = DRD3 + 0x20000; *size = 0x4000;
        return 0;
    }
    if (!node_regs[path[1]][idx][1]) return -1;
    *addr = node_regs[path[1]][idx][0]; *size = node_regs[path[1]][idx][1];
    return 0;
}
int adt_get_child_count(const void *a, int n) { (void)a; return n == N_ARMIO ? N_TLBI - N_QSPI + 1 : 0; }
int adt_first_child_offset(const void *a, int n) { (void)a; (void)n; return N_QSPI; }
int adt_next_sibling_offset(const void *a, int n) { (void)a; return n + 1; }
int adt_get_property_count(const void *a, int n) { (void)a; return n == N_CO ? co_count : n == N_MM ? mm_count : 0; }
int adt_first_property_offset(const void *a, int n) { (void)a; return n == N_CO ? co_first : mm_first; }
int adt_next_property_offset(const void *a, int off) { (void)a; return off + sizeof(struct adt_property) + 16; }
const void *adt_getprop(const void *a, int n, const char *name, u32 *len)
{
    int count = adt_get_property_count(a, n), off = adt_first_property_offset(a, n);
    for (int i = 0; i < count; i++, off = adt_next_property_offset(a, off)) {
        const struct adt_property *p = ADT_PROP(a, off);
        if (!strcmp(p->name, name)) { *len = p->size; return p->value; }
    }
    return NULL;
}
struct leaf { u64 ipa, size, pte; int level; };
static struct leaf leaves[16];
static int nleaves;
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx)
{
    for (int i = 0; i < nleaves; i++)
        if (!fn(ctx, leaves[i].ipa, leaves[i].size, leaves[i].pte, leaves[i].level))
            return;
}
static u64 map(u64 ipa, u64 to, u64 size)
{
    u64 v = to;
    return hv_t8152_map_filter(ipa, &v, size) ? v : 0xdead;
}
static bool denied(u64 pa, u64 size) { return map(pa, pa | S2_HW, size) == 0xdead; }

int main(void)
{
    cur_boot_args.phys_base = DRAM;
    cur_boot_args.mem_size = DRAM_SIZE;
    cur_boot_args.virt_base = 0xfffffe0000000000UL;
    cur_boot_args.devtree = (void *)(0xfffffe0000000000UL + 0x7004000UL);
    cur_boot_args.devtree_size = 0x80000;
    co_first = prop_end;
    add_prop("region-id-24", DRAM, DRAM_SIZE);
    co_count = 1;
    mm_first = prop_end;
    mm_count = 0;

    assert(hv_t8152_preflight() == 0);
    assert(hv_t8152_usb_allow(1, DRD1) < 0);                 /* no guest RAM yet */
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) == 0);

    puts("FLAG OFF");
    /* Every usb-drd/atc-phy/dart-usb reg, the aperture and the proxy's drd0: denied. */
    assert(denied(DRD1, 0x4000) && denied(DRD1 + 0x4000, 0x4000) && denied(DRD1, 0x8000));
    assert(denied(DRD1 + 0x8000, 0x4000) && denied(DRD1 + 0xc000, 0x4000));
    assert(denied(DRD1 + 0x10000, 0x4000));
    assert(denied(DRD3, 0x8000) && denied(DRD0, 0x8000) && denied(DRD0 + 0xc000, 0x4000));
    assert(denied(PHY1, 0x4000) && denied(PHY1 + 0x30000, 0x4000));
    assert(denied(DART1, 0x4000) && denied(DART1 + 0x20000, 0x4000) && denied(TLBI, 0x4000));

    puts("ALLOW REFUSALS");
    assert(hv_t8152_usb_allow(0, DRD0) < 0);                 /* the proxy's port: never */
    assert(hv_t8152_usb_allow(2, DRD1) < 0);                 /* no such node */
    assert(hv_t8152_usb_allow(1, DRD1 + 0x4000) < 0);        /* not the ADT window */
    assert(hv_t8152_usb_allow(1, DRD3) < 0);
    assert(denied(DRD1, 0x8000));                            /* still denied */

    puts("ALLOW DRD1");
    assert(hv_t8152_usb_allow(1, DRD1) == 0);
    u64 pte = map(DRD1, DRD1 | S2_HW, 0x8000);
    assert(pte != 0xdead && ((pte >> 2) & 0xf) == 0);        /* Device-nGnRnE */
    assert(map(DRD1 + 0x4000, (DRD1 + 0x4000) | S2_HW, 0x4000) != 0xdead);
    assert(denied(DRD1 + 0x8000, 0x4000));                   /* rest of reg 0 */
    assert(denied(DRD1, 0xc000));                            /* runs past the window */
    assert(denied(DRD1 + 0xc000, 0x4000));                   /* dwc3 globals */
    assert(denied(DRD0, 0x8000) && denied(DRD3, 0x8000));
    assert(denied(PHY1, 0x4000) && denied(DART1, 0x4000) && denied(TLBI, 0x4000));
    assert(map(0x3f0000000, DRD1 | S2_HW, 0x8000) == 0xdead);   /* not identity */
    assert(hv_t8152_usb_allow(1, DRD1) < 0);                 /* once */

    puts("CARRIER TABLES");
    assert(hv_t8152_reserve_carrier(WIN_LO, WIN_LO + 0x10000, "USB DART tables (D4)") < 0);
    assert(hv_t8152_reserve_carrier(HEAP_TOP + 0x1000, HEAP_TOP + 0x5000, "x") < 0);
    assert(hv_t8152_reserve_carrier(HEAP_LIM, HEAP_LIM + 0x4000, "x") < 0);
    assert(hv_t8152_reserve_carrier(HEAP_TOP, HEAP_TOP + 0x248000, "USB DART tables (D4)") == 0);
    assert(denied(HEAP_TOP, 0x4000));

    puts("VERIFY");
    u64 hcr_ok = HCR_VM | HCR_TSC | HCR_TWI | HCR_TWE | HCR_PTW;
    fake_hcr = hcr_ok;
    leaves[nleaves++] = (struct leaf){WIN_LO, 0x2000000, WIN_LO | S2_HW | 2, 2};
    leaves[nleaves++] = (struct leaf){DRD1, 0x4000, DRD1 | (S2_HW & ~(0xfUL << 2)) | 2, 3};
    leaves[nleaves++] = (struct leaf){DRD1 + 0x4000, 0x4000,
                                      (DRD1 + 0x4000) | (S2_HW & ~(0xfUL << 2)) | 2, 3};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO + 0x2000000, hcr_ok, true) == 0);
    leaves[nleaves++] = (struct leaf){HEAP_TOP, 0x4000, HEAP_TOP | S2_HW | 2, 3};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO + 0x2000000, hcr_ok, true) < 0); /* tables */
    nleaves--;
    leaves[nleaves++] = (struct leaf){DRD1 + 0xc000, 0x4000,
                                      (DRD1 + 0xc000) | (S2_HW & ~(0xfUL << 2)) | 2, 3};
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO + 0x2000000, hcr_ok, true) < 0); /* globals */
    nleaves--;

    puts("REVOKE");
    hv_t8152_usb_revoke(1);
    assert(denied(DRD1, 0x8000));
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO + 0x2000000, hcr_ok, true) < 0);
    nleaves -= 2;
    assert(hv_t8152_verify(WIN_LO + 0x800, WIN_LO + 0x2000000, hcr_ok, true) == 0);
    assert(hv_t8152_usb_allow(1, DRD1) == 0);                /* again, after a revoke */
    assert(hv_t8152_usb_allow(3, DRD3) == 0);                /* both ports: AIC + 2 */
    hv_t8152_usb_revoke(1);
    assert(denied(DRD1, 0x8000) && !denied(DRD3, 0x8000));
    hv_t8152_usb_revoke(3);
    assert(denied(DRD3, 0x8000));

    puts("OVERLAP");
    drd3_overlap = true;                                     /* another drd3 reg in the window */
    assert(hv_t8152_preflight() == 0);
    assert(hv_t8152_set_guest_ram(WIN_LO, WIN_HI - WIN_LO, HOST_TOP) == 0);
    assert(hv_t8152_usb_allow(3, DRD3) < 0);
    assert(denied(DRD3, 0x4000));                            /* the entry is back */
    assert(hv_t8152_usb_allow(1, DRD1) == 0);
    puts("T8152 USB POLICY OK");
    return 0;
}
"""


def test_stage2_policy_with_usb_nodes(tmp_path):
    import test_windows_t8152_policy as policy
    cc = clang()
    for name, text in policy.STUBS.items():
        (tmp_path / name).write_text(text)
    for name in ("hv_t8152.c", "hv_t8152.h"):
        shutil.copy2(WIN / name, tmp_path / name)
    (tmp_path / "harness.c").write_text(POLICY_HARNESS)
    exe = tmp_path / "policy-usb"
    build = subprocess.run([cc, *CFLAGS, "-Wno-unused-parameter", "-iquote", str(tmp_path),
                            str(tmp_path / "hv_t8152.c"), str(tmp_path / "harness.c"), "-o",
                            str(exe)], capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    out = result.stdout
    assert "T8152 USB POLICY OK" in out
    off = out.split("FLAG OFF", 1)[1].split("ALLOW REFUSALS", 1)[0]
    for name in ("usb-drd1", "usb-drd3", "usb-drd0", "atc-phy1", "dart-usb1",
                 "dart-tlbi-aperture"):
        assert f": {name}\n" in off, name
    refusals = out.split("ALLOW REFUSALS", 1)[1].split("ALLOW DRD1", 1)[0]
    assert "no split usb-drd0 reg 0" in refusals and "is not usb-drd1's ADT window" in refusals
    allow = out.split("ALLOW DRD1", 1)[1].split("CARRIER TABLES", 1)[0]
    assert "usb-drd1 xHCI [0x3a1000000, 0x3a1008000) passed through" in allow
    assert "already passed through" in allow
    assert "MMIO passthrough must be identity mapped" in allow
    tables = out.split("CARRIER TABLES", 1)[1].split("VERIFY", 1)[0]
    assert tables.count("refused: it must be 16 KiB aligned carrier heap") == 3
    assert "USB DART tables (D4) [0x10012000000, 0x10012248000) denied to stage 2" in tables
    verify = out.split("VERIFY", 1)[1].split("REVOKE", 1)[0]
    assert ": USB DART tables (D4)" in verify and "3 MMIO passthrough" not in verify
    assert "usb-drd1 xHCI denied again" in out
    overlap = out.split("OVERLAP", 1)[1]
    assert "USB xHCI [0x3a3000000, 0x3a3008000) also overlaps usb-drd3; refusing" in overlap


# ---- 4. the alias table -------------------------------------------------------------------

ALIAS_HARNESS = r"""
#include <assert.h>
#include <stdio.h>
#include "hv_aic_alias.h"
u32 chip_id = 0x8152;
int main(void)
{
    u32 pub;
    /* Off: nothing is owned, nothing is translated, 1692 is not a known line. */
    assert(!hv_aic_alias_is_owned(995) && !hv_aic_alias_is_owned(997));
    assert(!hv_aic_alias_lookup_physical(1692, &pub) && hv_aic_alias_to_physical(995) == 995);
    hv_aic_alias_t8152_usb(1u << 1);
    assert(hv_aic_alias_is_owned(995) && !hv_aic_alias_is_owned(997));
    assert(hv_aic_alias_to_physical(995) == 1692 && hv_aic_alias_to_published(1692) == 995);
    assert(hv_aic_alias_lookup_physical(1692, &pub) && pub == 995);
    assert(!hv_aic_alias_lookup_physical(1834, &pub) && !hv_aic_alias_lookup_physical(1600, &pub));
    hv_aic_alias_t8152_usb((1u << 1) | (1u << 3));
    assert(hv_aic_alias_is_owned(997) && hv_aic_alias_to_physical(997) == 1834);
    hv_aic_alias_t8152_usb(1u << 3);                 /* the storm guard withdrew drd1 only */
    assert(!hv_aic_alias_is_owned(995) && hv_aic_alias_is_owned(997));
    assert(!hv_aic_alias_lookup_physical(1692, &pub));
    for (u32 intid = 0; intid < 1024; intid++)
        assert(!hv_aic_alias_is_owned(intid) || intid == 997);
    hv_aic_alias_t8152_usb(0);
    for (u32 intid = 0; intid < 4096; intid++) {
        assert(!hv_aic_alias_is_owned(intid));
        assert(!hv_aic_alias_lookup_physical(intid, &pub));
    }
    /* T8142 keeps its own table; 990 (AHCI) and 996 are never USB's. */
    hv_aic_alias_t8152_usb((1u << 1) | (1u << 3));
    assert(!hv_aic_alias_is_owned(990) && !hv_aic_alias_is_owned(996));
    chip_id = 0x8142;
    assert(hv_aic_alias_is_owned(995) && hv_aic_alias_to_physical(995) == 1277);
    puts("ALIAS OK");
    return 0;
}
"""


def test_alias_ownership_follows_attach(tmp_path):
    # F11 and design 3.1: 995/997 are owned only while the port is attached, so a guest enable
    # of 995 with USB off can never unmask AIC 1692, and a stray line is never "found".
    cc = clang()
    (tmp_path / "types.h").write_text("#pragma once\n#include <stdbool.h>\n#include <stddef.h>\n"
                                      "#include <stdint.h>\ntypedef uint32_t u32;\n"
                                      "typedef unsigned long u64;\n")
    (tmp_path / "soc.h").write_text('#pragma once\n#include "types.h"\n#define T8142 0x8142\n'
                                    "#define T8152 0x8152\nextern u32 chip_id;\n")
    (tmp_path / "utils.h").write_text('#pragma once\n#include "types.h"\n'
                                      "#define BIT(x) (1UL << (x))\n"
                                      "#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n")
    for name in ("hv_aic_alias.c", "hv_aic_alias.h"):
        shutil.copy2(WIN / name, tmp_path / name)
    (tmp_path / "harness.c").write_text(ALIAS_HARNESS)
    exe = tmp_path / "alias"
    build = subprocess.run([cc, *CFLAGS, "-iquote", str(tmp_path),
                            str(tmp_path / "hv_aic_alias.c"), str(tmp_path / "harness.c"), "-o",
                            str(exe)], capture_output=True, text=True, timeout=120)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and "ALIAS OK" in result.stdout, result.stderr


# ---- 5. source rules ------------------------------------------------------------------------

def test_t8152_irq_path_pends_only_owned_lines():
    # F11 and design 3.3: with USB configured the identity fallback is gone; an unowned line is
    # masked and logged.  Without USB (hv_usb_enabled false) the D3-validated path is kept
    # byte for byte: hv_aic_alias_to_published, then pend.
    exc = source("hv_exc.c")
    branch = exc[exc.index("if (chip_id == T8152 && hv_t8152_gic_owned()) {\n        if (type == "
                           "AIC_EVENT_TYPE_HW)"):]
    branch = branch[:branch.index("u64 misr = mrs(ICH_MISR_EL2);")]
    body = code(branch)
    assert body.index("if (!hv_usb_enabled()) {") < \
        body.index("guest_irq = hv_aic_alias_to_published(irq);") < \
        body.index("} else if (!hv_aic_alias_lookup_physical(irq, &guest_irq)) {") < \
        body.index("aic_set_mask(irq, true);") < body.index("t8152_unowned_irq(irq);") < \
        body.index("return;") < body.index("hv_usb_note_irq(irq);") < \
        body.index("hv_gic_cpuif_set_pending(guest_irq);")
    assert body.count("hv_aic_alias_to_published") == 1
    unowned = function_body(exc, "static void t8152_unowned_irq(u32 irq)")
    assert "__atomic_fetch_or(&t8152_unowned_seen" in unowned and "printf" in unowned
    # Deactivation unmasks only owned lines (existing rule, now load-bearing for 995).
    eoi = function_body(exc, "static bool hv_handle_t8152_gic_cpuif(")
    assert "if (hv_aic_alias_is_owned(intid))\n                aic_set_mask(" \
           "hv_aic_alias_to_physical(intid), false);" in eoi


def test_every_exit_path_tears_usb_down():
    # F13: hv_start() wraps the whole session; the reset path stops the controller lock-free;
    # the fatal report prints the USB records; the 1 Hz report polls.
    hv_c = source("hv.c")
    wrapper = function_body(hv_c, "int hv_start(void *entry, u64 regs[4])\n{")
    assert wrapper.index("int ret = hv_start_session(entry, regs);") < \
        wrapper.index("hv_usb_teardown(") < wrapper.index("return ret;")
    assert hv_c.count("hv_start_session(") == 3  # declaration, call, definition
    session = function_body(hv_c, "static int hv_start_session(void *entry, u64 regs[4])\n{")
    assert "hv_usb_teardown" not in session  # one place, after every return
    report = function_body(hv_c, "void hv_fb_report(void)")
    assert report.index("hv_ahci_report();") < report.index("hv_usb_report();")
    utils = (REPO / "src/utils.c").read_text()
    reboot = function_body(utils, "void flush_and_reboot(void)")
    assert reboot.index('printf("reset re-entered; parking this CPU\\n");') < \
        reboot.index("hv_usb_emergency_stop();") < reboot.index("nvme_shutdown()")
    fatal = function_body(source("hv_fatal.c"), "void hv_windows_fatal_report(")
    assert fatal.index("hv_ahci_fatal_report(now, freq);") < fatal.index("hv_usb_fatal_report();")
    usb = source("hv_usb.c")
    quiesce = function_body(usb, "static void quiesce_port(struct usb_port *p, bool emergency)")
    assert quiesce.index("aic_set_mask(p->irq, true);") < quiesce.index("USBCMD_RS") < \
        quiesce.index("DART_DISABLE_STREAMS") < quiesce.index("DART_TTBR(sid), 0)") < \
        quiesce.index("USB_DART_TCR_IDLE") < quiesce.index("tlbi_stream(") < \
        quiesce.index("if (emergency)\n        return;") < quiesce.index("hv_unmap(") < \
        quiesce.index("hv_t8152_usb_revoke(")
    teardown = function_body(usb, "void hv_usb_teardown(const char *why)")
    assert teardown.index("hv_aic_alias_t8152_usb(0);") < teardown.index("quiesce_port(p, false);") \
        < teardown.index("free_tables();")
    emergency = function_body(usb, "void hv_usb_emergency_stop(void)")
    assert "spin_lock" not in emergency and "free" not in code(emergency)
    fatal_usb = function_body(usb, "void hv_usb_fatal_report(void)")
    assert "read32" not in fatal_usb


def test_drd0_is_never_reachable():
    usb = source("hv_usb.c")
    assert "#define DRD0_XHCI  0x402430000UL" in usb
    assert "#define DRD0_DART0 0x403020000UL" in usb and "#define DRD0_DART1 0x403040000UL" in usb
    # No path, table entry or address names drd0's nodes; only messages mention it.
    bare = re.sub(r'"(?:[^"\\]|\\.)*"', '""', code(usb))
    assert "dart-usb0" not in bare and "usb-drd0" not in bare and "drd0" not in bare.replace(
        "DRD0_", "")
    assert re.search(r"#define HV_USB_PORTS\s+\(HV_USB_PORT_DRD1 \| HV_USB_PORT_DRD3\)",
                     source("hv_usb.h"))
    j873 = (REPO / "src/j873.c").read_text()
    state = function_body(j873, "int j804_usb_port_state(int port, struct j804_usb_port *out)")
    assert "(port != 1 && port != 3)" in state and "write" not in state and "mask32" not in state
    policy = source("hv_t8152.c")
    port_of = function_body(policy, "static int usb_port_of(const char *name)")
    assert "name[7] < '1'" in port_of
    assert '"usb-drd", "pmgr"' in policy and '"atc-phy",' in policy and '"dart",' in policy


def test_streams_and_dart_registers_match_the_design():
    usb = source("hv_usb.c")
    assert "} streams[] = {{0, 0}, {0, 1}, {1, 1}};" in usb
    assert "static const u32 inst_streams[2] = {BIT(0) | BIT(1), BIT(1)};" in usb
    assert "0xffffffff" not in code(usb)
    for define in ("DART_ENABLE_STREAMS  0xc00", "DART_DISABLE_STREAMS 0xc20",
                   "DART_TCR(sid)        (0x1000 + 4 * (sid))",
                   "DART_TTBR(sid)       (0x1400 + 4 * (sid))", "DART_EXCEPTION       0x4000",
                   "DART_ROUTE           0x4604",
                   "DART_PARAMS3_USB     ((42u << 24) | (42u << 16) | (3u << 8))"):
        assert f"#define {define}" in usb, define
    hdr = source("hv_usb_dart.h")
    assert "#define USB_DART_TCR_ON         (USB_DART_TCR_TRANSLATE | USB_DART_TCR_FOUR_LEVEL)" in hdr
    dart = (REPO / "src/dart.c").read_text()
    tlbi = function_body(dart, "int dart_gen3_tlbi_adt(const char *path, int instance, int device)")
    assert "dart_gen3_stream_adt(path, instance, device, &d)" in tlbi
    assert "dart_gen3_tlb_invalidate(&d);" in tlbi
    # No TLBI while the guest runs: only commit and quiesce call it.
    assert usb.count("tlbi_stream(p, i,") == 2
    report = function_body(usb, "void hv_usb_report(void)")
    assert "tlbi" not in report and "write32" not in report


def test_configure_proxy_and_build_agree():
    hv_h, hv_c = source("hv.h"), source("hv.c")
    assert re.search(r"#define HV_WINDOWS_T8152_USB BIT\(9\)", hv_h)
    conf = function_body(hv_c, "int hv_windows_configure(u64 flags)")
    # The accepted-flags mask ends with USB (I0 appends its own flag after it).
    assert re.search(r"HV_WINDOWS_T8152_USB(?: \| HV_WINDOWS_T8152_INPUT(?: \| HV_WINDOWS_T8152_SCANOUT)?)?\)\)\n        return -1;",
                     conf)
    assert "(!(flags & HV_WINDOWS_T8152_SOFT_GIC) || board_id != 0x22)" in conf
    assert "if (flags & HV_WINDOWS_T8152_USB)\n        hv_usb_enable();" in conf
    proxy_c = (REPO / "src/proxy.c").read_text()
    proxy_h = (REPO / "src/proxy.h").read_text()
    proxy_py = (REPO / "proxyclient/aura/proxy.py").read_text()
    assert "P_HV_WINDOWS_USB = 0xc87," in proxy_h and "P_HV_WINDOWS_USB = 0xc87" in proxy_py
    assert "HV_WINDOWS_CAPS2_MAGIC | HV_WINDOWS_CAPS2_AHCI | HV_WINDOWS_CAPS2_USB" in proxy_c
    assert "hv_usb_proxy(request->args[0], request->args[1], request->args[2])" in proxy_c
    assert re.search(r"#define HV_WINDOWS_CAPS2_USB BIT\(1\)", source("hv_usb.h"))
    assert (w.CAPS2_USB, w.CONFIG_T8152_USB) == (1 << 1, 1 << 9)
    assert (w.USB_OP_ATTACH, w.USB_OP_STATUS, w.USB_OP_DETACH) == (1, 2, 3)
    hdr = source("hv_usb.h")
    for name, index in w.USB_STATUS.items():
        assert re.search(rf"HV_USB_S_\w+ = {index},", hdr), name
    assert f"HV_USB_S_DMA_BASE = {w.USB_STATUS_DMA_BASE}," in hdr
    assert f"HV_USB_S_DMA_TOP = {w.USB_STATUS_DMA_TOP}," in hdr
    assert not {w.USB_STATUS_DMA_BASE, w.USB_STATUS_DMA_TOP} & set(w.USB_STATUS.values())
    for value in w.USB_ERRORS:
        assert re.search(rf"HV_USB_E_\w+ = {value},", hdr), value
    assert (w.USB_GSIV[1], w.USB_GSIV[3]) == (995, 997)
    assert "#define HV_USB_GSIV_DRD1 995u" in hdr and "#define HV_USB_GSIV_DRD3 997u" in hdr
    makefile = (REPO / "Makefile").read_text()
    assert re.search(r"hv_fatal\.o hv_usb\.o hv_usb_dart\.o[) ]", makefile)


def test_clean_room_headers():
    for name in ("hv_usb.c", "hv_usb.h", "hv_usb_dart.c", "hv_usb_dart.h"):
        text = source(name)
        assert text.startswith("/* SPDX-License-Identifier: MIT */"), name
        assert "Copyright" not in text, name
    dart = source("hv_usb_dart.c")
    assert set(re.findall(r'#include "([^"]+)"', dart)) == {"hv_usb_dart.h"}
    assert "io-pgtable-dart.c" in source("hv_usb_dart.h")
    assert "xHCI 1.2" in source("hv_usb.c")


# ---- 6. the launcher ------------------------------------------------------------------------

@pytest.mark.parametrize("value, ports", [
    ("off", ()), ("disabled", ()), ("drd1", (1,)), ("drd3", (3,)), ("drd1+drd3", (1, 3)),
    ("usb-drd1", (1,)), ("DRD3,drd1", (1, 3)),
])
def test_usb_flag_parsing(value, ports):
    assert w.parse_windows_usb(value) == ports
    assert w.parse_windows_usb(w.usb_spelling(ports)) == ports


@pytest.mark.parametrize("value", ["drd0", "usb-drd0", "0", "0x0", "usb0", "port0",
                                   "drd1+drd0", "drd0+drd3", "DRD0", "drd-0"])
def test_usb_flag_refuses_drd0_in_every_spelling(value):
    with pytest.raises(ValueError, match="proxy's CDC link"):
        w.parse_windows_usb(value)


@pytest.mark.parametrize("value", ["drd2", "drd4", "all", "drd1+drd1", "on"])
def test_usb_flag_refuses_unknown(value):
    with pytest.raises(ValueError):
        w.parse_windows_usb(value)


def usb_fd(*ports, base=None, gsiv=None):
    fd = bytearray(b"\xff" * 0x8000)
    for port in ports:
        offset, magic = w.USB_FD_MARKER[port]
        struct.pack_into("<8sII", fd, offset, magic, (base or w.USB_XHCI[port]) >> 12,
                         gsiv or w.USB_GSIV[port])
    return bytes(fd)


CAPS2_OK = w.CAPS2_MAGIC | w.CAPS2_AHCI | w.CAPS2_USB


def test_fd_markers_and_contract():
    assert w.fd_usb_markers(usb_fd()) == {}
    assert w.fd_usb_markers(usb_fd(1)) == {1: (0x40A430000, 995)}
    assert w.fd_usb_markers(usb_fd(1, 3)) == {1: (0x40A430000, 995), 3: (0x412430000, 997)}
    # The marker sits after the AHCI one (0x110) in the region before the FV.
    assert w.USB_FD_MARKER[1][0] == w.AHCI_FD_MARKER_OFFSET + 0x10
    w.validate_usb_request((1,), 0x8152, 0x22, CAPS2_OK, usb_fd(1))
    w.validate_usb_request((1, 3), 0x8152, 0x22, CAPS2_OK, usb_fd(1, 3))
    w.validate_usb_request((), 0x8152, 0x22, 0, usb_fd())


@pytest.mark.parametrize("ports, chip, board, caps2, fd, message", [
    ((), 0x8152, 0x22, 0, usb_fd(1), "would touch a denied page"),          # marker, flag off
    ((1,), 0x8152, 0x22, CAPS2_OK, usb_fd(1, 3), "would touch a denied page"),
    ((1,), 0x8152, 0x22, CAPS2_OK, usb_fd(), "no J804USB1 marker"),       # flag on, no marker
    ((1, 3), 0x8152, 0x22, CAPS2_OK, usb_fd(1), "no J804USB3 marker"),
    ((1,), 0x8142, 0x24, CAPS2_OK, usb_fd(1), "T8152"),
    ((1,), 0x8152, 0x24, CAPS2_OK, usb_fd(1), "J804 board"),
    ((1,), 0x8152, 0x22, w.CAPS2_MAGIC | w.CAPS2_AHCI, usb_fd(1), "CAPS2 bit 1"),
    ((1,), 0x8152, 0x22, 0, usb_fd(1), "CAPS2 bit 1"),
    ((1,), 0x8152, 0x22, CAPS2_OK, usb_fd(1, gsiv=996), "differs from the carrier"),
    ((1,), 0x8152, 0x22, CAPS2_OK, usb_fd(1, base=0x402430000), "differs from the carrier"),
])
def test_usb_request_refusals(ports, chip, board, caps2, fd, message):
    # F12: both marker directions, the chip, the board and the resident.
    with pytest.raises(ValueError, match=message):
        w.validate_usb_request(ports, chip, board, caps2, fd)


def test_attach_results_are_named():
    assert w.describe_usb_attach(0) == "attached"
    assert "host mode" in w.describe_usb_attach(-3)
    assert "host mode" in w.describe_usb_attach((1 << 64) - 3)
    assert w.describe_usb_attach(-99) == "error -99"


def test_profile_takes_no_usb_passthrough():
    import test_windows_t8152_profile as profile_tests  # noqa: F401  (fixture module exists)
    src = (REPO / "proxyclient/aura/windows.py").read_text()
    body = src[src.index('    usb = profile.get("usb", "disabled")'):]
    body = body[:body.index("serial = profile.get(")]
    assert 'require(usb in ("disabled", "off")' in body and "run_guest.py" in body


def _launch_with_usb(monkeypatch, usb):
    import test_windows_t8152_launch as launch
    from aura.hv import HV
    captured = {}
    real_init = HV.init

    def init(self):
        self.windows_usb = usb
        captured["hv"] = self
        return real_init(self)

    monkeypatch.setattr(HV, "init", init)
    return launch.run(), captured["hv"]


def test_hv_launch_with_usb_sets_flag_and_marker(monkeypatch):
    from aura.adt import load_adt
    r, hv = _launch_with_usb(monkeypatch, "drd1")
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert config[1] & w.CONFIG_T8152_USB and config[1] & w.CONFIG_T8152_SOFT_GIC
    guest = load_adt(r.writes[hv.adt_base])
    props = {p["name"]: p["value"] for p in guest["/chosen"].tostruct()["properties"]}
    assert w.USB_PROPERTY in props and w.USB_DISABLED_PROPERTY not in props
    assert "/arm-io/usb-drd0" in hv.windows_usb_removed
    assert not any(node.name.startswith(("usb-drd", "dart-usb")) for node in guest["/arm-io"])
    # The host maps no USB page itself: the resident maps the xHCI window at ATTACH.
    for ipa, to, size, incr in r.maps:
        assert not (0x400000000 <= ipa < 0x420000000), hex(ipa)


def test_hv_launch_without_usb_keeps_the_disabled_marker(monkeypatch):
    from aura.adt import load_adt
    r, hv = _launch_with_usb(monkeypatch, "disabled")
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert not config[1] & w.CONFIG_T8152_USB
    guest = load_adt(r.writes[hv.adt_base])
    props = {p["name"]: p["value"] for p in guest["/chosen"].tostruct()["properties"]}
    assert w.USB_DISABLED_PROPERTY in props and w.USB_PROPERTY not in props


def test_hv_refuses_usb_with_the_carrier_cpuif(monkeypatch):
    import test_windows_t8152_launch as launch
    from aura.hv import HV
    real_init = HV.init

    def init(self):
        self.windows_usb = "drd1"
        self.windows_gic_cpuif = "carrier"
        return real_init(self)

    monkeypatch.setattr(HV, "init", init)
    with pytest.raises(ValueError):
        launch.run()


def test_run_guest_usb_order_and_refusals():
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    assert "parser.add_argument('--windows-usb', default='off'" in src
    t8152 = src[src.index("def run_windows_t8152"):src.index("_windows_caps = windows_resident_caps()")]
    assert "windows_t8152_usb_attach(hv, usb_ports, ahci)" in t8152
    # Refusals before anything is uploaded; ATTACH after the AHCI store, before start; STATUS
    # at exit; DETACH if start fails.
    assert t8152.index("usb_ports = windows_t8152_usb_plan(payload, u)") < t8152.index("hv.init()")
    assert t8152.index("hv.windows_usb = usb_spelling(usb_ports)") < t8152.index("hv.init()")
    assert t8152.index("windows_t8152_ahci_attach(hv, u, ahci)") < \
        t8152.index("windows_t8152_usb_attach(hv, usb_ports, ahci)") < \
        t8152.index("result = hv.start()") \
        < t8152.index("windows_t8152_usb_status(usb_ports)")
    # hv.start() failed, refused, or aborted on a forwarded exception: DETACH (skipped only
    # when the carrier never finished the guest exit: the link then answers nothing).
    fail = t8152[t8152.index("except (ProxyRemoteError, WindowsGuestAborted, "
                             "WindowsGuestExitTimeout) as exc:"):]
    fail = fail[fail.index("if usb_ports and not stuck:"):]
    assert "p.hv_windows_usb(USB_OP_DETACH)" in fail[:200]
    plan = src[src.index("def windows_t8152_usb_plan"):src.index("def windows_t8152_usb_attach")]
    assert "w.parse_windows_usb(args.windows_usb)" in plan
    assert "w.validate_usb_request(ports, chosen.chip_id" in plan
    assert "p.hv_windows_capabilities2()" in plan
    attach = src[src.index("def windows_t8152_usb_attach"):src.index("def windows_t8152_usb_status")]
    assert "w.USB_OP_ATTACH, w.usb_port_mask(ports)" in attach and "sys.exit(" in attach
    # The handshake only after ATTACH returned 0, from the resident's DART window; any problem
    # with it detaches again and stops the launch.
    assert attach.index("if result != 0:") < attach.index("w.USB_STATUS_DMA_BASE") < \
        attach.index("w.usb_handshake(ports, *dma, hv.windows_guest_ram,") < \
        attach.index("hv.set_windows_usb_handshake(value)")
    failed = attach[attach.index("except (ValueError, ProxyRemoteError) as exc:"):]
    assert failed.index("p.hv_windows_usb(w.USB_OP_DETACH)") < failed.index("sys.exit(")
    tail = src.split("_windows_caps = windows_resident_caps()", 1)[1]
    assert "args.windows_usb not in ('off', 'disabled')" in tail


def test_contract_matches_mu_header():
    tree = os.environ.get("AURA_MU_TREE")
    if not tree:
        pytest.skip("AURA_MU_TREE not set")
    header = Path(tree) / "Platform/MacBookPro2026Pkg/Include/J804Windows.h"
    if not header.exists() or "J804_XHC1_BASE" not in header.read_text():
        pytest.skip("Mu tree without the D4 header (it lives on mu j804-windows)")
    values = dict(re.findall(r"#define (J804_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", header.read_text()))
    assert int(values["J804_XHC1_BASE"], 0) == w.USB_XHCI[1]
    assert int(values["J804_GSIV_XHC1"], 0) == w.USB_GSIV[1]
    assert int(values["J804_XHC_SIZE"], 0) == w.USB_XHCI_SIZE
    assert int(values["J804_USB_FD_MARKER_OFFSET"], 0) == w.USB_FD_MARKER[1][0]
    # The /chosen handshake Mu's J804XhciAcpiDxe gates XHC1 on: name, size, port bit.
    prop = re.search(r'#define J804_USB_ADT_PROPERTY\s+"([^"]+)"', header.read_text())
    assert prop and prop.group(1) == w.USB_HANDSHAKE_PROPERTY
    assert int(values["J804_USB_ADT_PROPERTY_SIZE"], 0) == w.USB_HANDSHAKE_SIZE
    assert int(values["J804_USB_PORT_DRD1"], 0) == w.USB_PORT_BIT[1]
    assert struct.calcsize(w.USB_HANDSHAKE_FORMAT) == w.USB_HANDSHAKE_SIZE


MU_PARSER_HARNESS = r"""
#include <stdio.h>
#include <Base.h>
#include <J804UsbHandshake.h>
int main(int argc, char **argv)
{
    UINT8 buf[64];
    FILE *f = argc > 1 ? fopen(argv[1], "rb") : NULL;
    if (!f)
        return 2;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    J804_USB_HANDSHAKE h;
    int st = (int)J804UsbParseHandshake(buf, n, &h);
    printf("%d %u %u 0x%llx 0x%llx 0x%llx 0x%llx\n", st, h.PortMask, h.Gsiv,
           (unsigned long long)h.XhciBase, (unsigned long long)h.XhciSize,
           (unsigned long long)h.DmaBase, (unsigned long long)h.DmaTop);
    return 0;
}
"""


def test_handshake_bytes_parse_in_mu(tmp_path):
    # The launcher's bytes through Mu's own parser (J804UsbHandshake.h, header-only), when a
    # Mu tree on j804-windows is given (AURA_MU_TREE).
    tree = os.environ.get("AURA_MU_TREE")
    if not tree:
        pytest.skip("AURA_MU_TREE not set")
    inc = Path(tree) / "Platform/MacBookPro2026Pkg/Include"
    shim = Path(tree) / "Tools/HostTests/J804UsbHandshake/shim"
    if not (inc / "J804UsbHandshake.h").exists() or not shim.exists():
        pytest.skip("Mu tree without the D4 handshake parser (it lives on mu j804-windows)")
    (tmp_path / "h.c").write_text(MU_PARSER_HARNESS)
    exe = tmp_path / "h"
    build = subprocess.run([clang(), *CFLAGS, "-Wno-unused-function", "-I", str(shim),
                            "-I", str(inc), str(tmp_path / "h.c"), "-o", str(exe)],
                           capture_output=True, text=True, timeout=120)
    assert build.returncode == 0, build.stderr[-4000:]
    ram = (0x10100000000, 0x10220000000)
    blob = tmp_path / "v1.bin"
    blob.write_bytes(w.usb_handshake((1,), *ram, ram))
    out = subprocess.run([str(exe), str(blob)], capture_output=True, text=True, timeout=60)
    assert out.stdout.split() == ["0", "2", "995", "0x40a430000", "0x8000", hex(ram[0]),
                                  hex(ram[1])]
    blob.write_bytes(w.usb_handshake((1,), *ram, ram)[:36])
    out = subprocess.run([str(exe), str(blob)], capture_output=True, text=True, timeout=60)
    assert out.stdout.split()[0] != "0"


def test_handshake_layout_and_refusals():
    ram = (0x10100000000, 0x10220000000)
    value = w.usb_handshake((1,), *ram, ram)
    assert len(value) == 40 == w.USB_HANDSHAKE_SIZE
    assert struct.unpack("<IIQQQQ", value) == (0x2, 995, 0x40A430000, 0x8000, *ram)
    plan = w.AhciPlan(window_base=ram[0], window_top=0x10300000000, store_base=ram[1],
                      disk_bytes=0x40000000)
    assert w.usb_handshake((1,), *ram, ram, plan) == value
    bad = [
        (((3,), *ram, ram, None), "usb-drd1 only"),
        (((1, 3), *ram, ram, None), "usb-drd1 only"),
        (((1,), (1 << 64) - 1, ram[1], ram, None), "nothing is attached"),
        (((1,), ram[0] + 0x1000, ram[1], (ram[0] + 0x1000, ram[1]), None), "16 KiB"),
        (((1,), ram[0], ram[0], (ram[0], ram[0]), None), "empty"),
        (((1,), ram[0], ram[1] - 0x4000, ram, None), "not the guest RAM window"),
        (((1,), ram[0], ram[1] + 0x4000, (ram[0], ram[1] + 0x4000), plan), "AHCI store"),
        (((1,), 0x40A42C000, 0x40A43C000, (0x40A42C000, 0x40A43C000), None), "xHCI"),
    ]
    for args, message in bad:
        with pytest.raises(ValueError, match=message):
            w.usb_handshake(*args)


def test_hv_handshake_reaches_the_guest_adt_only_when_set(monkeypatch):
    # The property is in the uploaded guest ADT only after set_windows_usb_handshake (which
    # run_guest calls after ATTACH returned 0); setup_adt alone never writes it.
    import test_windows_t8152_launch as launch
    from aura.adt import load_adt
    from aura.hv import HV
    value = w.usb_handshake((1,), *launch.WINDOW, launch.WINDOW)
    real_start = HV.start

    def start(self):
        self.set_windows_usb_handshake(value)
        return real_start(self)

    r, hv = _launch_with_usb(monkeypatch, "drd1")
    props = {p["name"]: p["value"]
             for p in load_adt(r.writes[hv.adt_base])["/chosen"].tostruct()["properties"]}
    assert w.USB_HANDSHAKE_PROPERTY not in props
    monkeypatch.setattr(HV, "start", start)
    r, hv = _launch_with_usb(monkeypatch, "drd1")
    props = {p["name"]: p["value"]
             for p in load_adt(r.writes[hv.adt_base])["/chosen"].tostruct()["properties"]}
    assert props[w.USB_HANDSHAKE_PROPERTY] == value
    assert struct.unpack("<IIQQQQ", props[w.USB_HANDSHAKE_PROPERTY])[4:] == launch.WINDOW
    with pytest.raises(ValueError, match="40 bytes"):
        hv.set_windows_usb_handshake(value[:36])
    hv.windows_usb = "disabled"
    with pytest.raises(ValueError, match="--windows-usb"):
        hv.set_windows_usb_handshake(value)
