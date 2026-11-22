# SPDX-License-Identifier: MIT
"""G6-S: the carrier's scanout service (virtual vblank and flips) for the J804 Windows guest.

Failure list, written with the code (docs/windows-t8152.md "G6-S"):

Model (scanout_model.c) and glue (hv_scanout.c), replayed unchanged by
tests/e2e/windows_scanout_host.c under ASan/UBSan:
  S1  reset values: ID "M6SO", version 1, FEATURES without HW_VBLANK/RETARGET, panel geometry,
      A2R10G10B10 panel format, 60 Hz = 400000 ticks of the 24 MHz counter; RO ignores writes
  S2  the vblank clock: edges at epoch + floor(n * F / mHz) with no drift (59.94 Hz over 10^7
      edges), one event per edge at the 5 kHz tick, a late tick counts every edge once, the
      guest-virtual edge time is host edge - CNTVOFF
  S3  STATUS W1C, ENABLE gates latching, the line is exactly ENABLE && (STATUS & enabled IRQs)
  S4  flip refusals (mode, cookie 0, format, stride, address, rect) change nothing but the error
  S5  a flip starts copying at once when nothing is in flight and is reported at the first
      vblank after its last band; BGRA is repacked exactly as hv_fb_convert_slice; the panel
      format is copied; a dirty rect touches nothing outside it; a 64-bit RECT_BR+SUBMIT write
      submits after the rect
  S6  one in flight plus one pending, the third is BUSY; order is kept; LATE counts a copy that
      spans a whole period
  S7  FLIP_MODE or ENABLE off retires both slots (DONE = the later); stale bands are ignored
  S8  bad widths/alignment/offsets: RAZ/WI and counted; holes read 0
  S9  fuzz: every accepted flip completes once and in order, the line follows the formula,
      bands stay inside the active rect
  S10 glue ATTACH refusals (flag, board, refresh, after entry, hook failure), one per session,
      QUERY values, verify (no panel, foreign leaf, no leaf), DETACH after entry refused
  S11 glue session: the SPI is 992 only and driven on changes only, the tick clock, a guest-RAM
      frame copied by work() bands and by the tick's floor, the shadow is a valid source and the
      scanout is not, exit retires and never drives the SPI again

Carrier wiring and launcher:
  W1  constants agree (page, size, SPI, CAPS2 bit 7, configure BIT(11), op 0xc8b) and are
      distinct from every other feature's
  W2  the SPI is live only with the scanout flag; hv_fb_convert_slice stands down while flips own
      the panel; the idle path and the FIQ fast path run one band; the tick runs the clock
  W3  configure needs the soft GIC, the FB shadow and J804; hv_start verifies, enters, exits
  W4  launcher: --windows-scanout off|60|120|<mHz>, CAPS2 bit 7, the FD marker J804SCN1 at 0x150,
      the 32-byte handshake <QIIIII> only after ATTACH returned 0
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

from aura import windows as w  # noqa: E402

CASES = ["reset_values", "vblank_timing", "status_and_line", "flip_refusals", "flip_lifecycle",
         "busy_and_order", "retire_and_stale", "copy_formats", "access_widths", "fuzz",
         "glue_attach", "glue_session"]

TYPES_H = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
typedef unsigned long u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef long s64;
#define BIT(x) (1UL << (x))
"""

STUBS = {
    "types.h": TYPES_H,
    "soc.h": '#pragma once\n#include "types.h"\n#define T8152 0x8152\n',
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define UNUSED(x)     (void)(x)
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzl(m))
extern u32 chip_id, board_id;
extern u64 stub_cntpct, stub_cntfrq, stub_cntvoff;
#define stub_mrs_CNTPCT_EL0 stub_cntpct
#define stub_mrs_CNTFRQ_EL0 stub_cntfrq
#define stub_mrs_CNTVOFF_EL2 stub_cntvoff
#define mrs(reg) (stub_mrs_##reg)
typedef struct { int held; } spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = {0}
static inline void spin_lock(spinlock_t *l) { if (l->held++) __builtin_trap(); }
static inline void spin_unlock(spinlock_t *l) { if (!l->held--) __builtin_trap(); }
''',
    "memory.h": '#pragma once\n#include "types.h"\nvoid dc_civac_range(void *addr, size_t length);\n',
    "hv_fatal.h": '#pragma once\n#include "types.h"\nvoid hv_fatal_fb_begin(u64 start, u64 end);\n'
                  'void hv_fatal_fb_end(void);\n',
    "hv_t8152.h": '#pragma once\n#include "types.h"\nbool hv_t8152_guest_ram(u64 *base, u64 *size);\n',
    "hv.h": r'''
#pragma once
#include "types.h"
#include "hv_scanout.h"
struct exc_info;
#define HV_MAX_RW_WORDS 8
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
typedef bool(hv_pt_leaf_fn)(void *ctx, u64 ipa, u64 size, u64 pte, int level);
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx);
void hv_t8152_gic_spi_level(u32 intid, bool level);
bool hv_windows_guest_entered(void);
''',
}


def source(name):
    return (WIN / name).read_text()


def function_body(text, signature):
    start = text.index(signature)
    end = text.index("\n}\n", start)
    return text[start:end]


@pytest.fixture(scope="module")
def replay(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    out = tmp_path_factory.mktemp("scanout")
    for name, text in STUBS.items():
        (out / name).write_text(text)
    for name in ("scanout_model.c", "scanout_model.h", "hv_scanout.c", "hv_scanout.h",
                 "hv_vdev_spi.c", "hv_vdev_spi.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "scanout-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-DHV_SCANOUT_HOST_TEST", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-iquote", str(out),
                            str(out / "scanout_model.c"), str(out / "hv_scanout.c"),
                            str(out / "hv_vdev_spi.c"),
                            str(REPO / "tests/e2e/windows_scanout_host.c"), "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary), "300000"], capture_output=True, text=True,
                            timeout=600)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    artifact = os.environ.get("AURA_SCANOUT_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text(result.stdout + result.stderr)
    return result, {line["case"]: line for line in lines}, [line["case"] for line in lines]


def test_replay_passes(replay):
    result, cases, order = replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert order == CASES
    assert all(cases[name]["result"] == "passed" for name in order)


def test_fuzz_exercised_both_outcomes(replay):
    _, cases, _ = replay
    fuzz = cases["fuzz"]
    assert fuzz["iterations"] == 300000
    assert fuzz["accepted"] > 10000 and fuzz["done"] > 1000 and fuzz["retired"] > 1000


# ---- the carrier ----------------------------------------------------------------------------

def test_contract_constants_agree():
    hdr = source("hv_scanout.h")
    model = source("scanout_model.h")
    spi = source("hv_vdev_spi.h")
    assert re.search(r"#define HV_SCANOUT_BASE\s+0xf60000000UL", hdr)
    assert re.search(r"#define HV_SCANOUT_SIZE\s+0x4000UL", hdr)
    assert re.search(r"#define HV_SCANOUT_INTID\s+992u", hdr)
    assert re.search(r"#define HV_VDEV_SPI_SCANOUT 992u", spi)
    assert re.search(r"#define HV_WINDOWS_CAPS2_SCANOUT BIT\(7\)", hdr)
    assert re.search(r"#define HV_WINDOWS_T8152_SCANOUT BIT\(22\)", source("hv.h"))
    assert re.search(r"#define SCANOUT_ID_VALUE\s+0x4f53364du", model)
    assert struct.pack("<I", 0x4f53364d) == b"M6SO"
    assert (w.SCANOUT_BASE, w.SCANOUT_SIZE, w.SCANOUT_GSIV, w.CAPS2_SCANOUT,
            w.CONFIG_T8152_SCANOUT) == (0xF60000000, 0x4000, 992, 1 << 7, 1 << 22)
    assert "P_HV_WINDOWS_SCANOUT = 0xc8b," in (REPO / "src/proxy.h").read_text()
    assert "P_HV_WINDOWS_SCANOUT = 0xc8b" in (REPO / "proxyclient/aura/proxy.py").read_text()
    proxy_c = (REPO / "src/proxy.c").read_text()
    assert "HV_WINDOWS_CAPS2_GPU_G2 | HV_WINDOWS_CAPS2_SCANOUT |" in proxy_c
    assert "hv_scanout_proxy(request->args[0], request->args[1], request->args[2]," in proxy_c
    query = dict(re.findall(r"HV_SCANOUT_Q_(\w+) = (\d+),", hdr))
    assert {k.lower(): int(v) for k, v in query.items()} == w.SCANOUT_Q
    status = dict(re.findall(r"HV_SCANOUT_S_(\w+) = (\d+),", hdr))
    assert {k.lower(): int(v) for k, v in status.items()} == w.SCANOUT_S
    regs = dict(re.findall(r"SCANOUT_(\w+) = (0x[0-9a-f]+),", model))
    assert {k: int(v, 16) for k, v in regs.items() if k in w.SCANOUT_REGS} == w.SCANOUT_REGS


def test_feature_numbers_are_distinct():
    proxy_h = re.sub(r"/\*.*?\*/", "", (REPO / "src/proxy.h").read_text(), flags=re.S)
    ops = re.findall(r"(P_HV_WINDOWS_\w+) = (0x[0-9a-f]+),", proxy_h)
    values = [int(v, 16) for _, v in ops]
    assert len(values) == len(set(values))
    hv_h = source("hv.h")
    flags = dict(re.findall(r"#define (HV_WINDOWS_T8152_\w+) BIT\((\d+)\)", hv_h))
    assert len(set(flags.values())) == len(flags)
    caps2 = [int(b) for b in re.findall(r"#define HV_WINDOWS_CAPS2_\w+\s+BIT\((\d+)\)",
                                        "".join(p.read_text() for p in WIN.glob("hv_*.h")))]
    assert len(caps2) == len(set(caps2)) and 7 in caps2
    assert len({990, w.INPUT_GSIV, w.SCANOUT_GSIV, *w.USB_GSIV.values()}) == 5
    assert 980 <= w.SCANOUT_GSIV < 1000
    alias = re.sub(r"/\*.*?\*/", "", source("hv_aic_alias.c"), flags=re.S)
    assert all(int(p) != 992 for p, _ in re.findall(r"\{(\d+), (\d+)\}", alias))
    pages = [w.AHCI_ABAR if hasattr(w, "AHCI_ABAR") else 0xF40000000, w.INPUT_BASE,
             w.SCANOUT_BASE]
    assert len(set(pages)) == 3
    assert w.SCANOUT_FD_MARKER_OFFSET == 0x150 and w.SCANOUT_FD_MARKER == b"J804SCN1"
    taken = [w.AHCI_FD_MARKER_OFFSET, *(o for o, _ in w.USB_FD_MARKER.values()),
             w.INPUT_FD_MARKER_OFFSET]
    assert all(abs(w.SCANOUT_FD_MARKER_OFFSET - o) >= 16 for o in taken)
    assert w.SCANOUT_HANDSHAKE_PROPERTY == "aura,windows-scanout-v1"


def test_wiring():
    exc = source("hv_exc.c")
    live = function_body(exc, "static bool hv_t8152_vspi_live(u32 intid)")
    assert "hv_scanout_enabled() ? VDEV_SPI_DEV_SCANOUT : 0" in live
    # T8152's idle path (T8142 keeps j813-windows' hv_fb_convert_idle()).
    assert re.search(r"if \(chip_id == T8152\) \{\n(?:\s*/?\*.*\n)*\s*if \(!hv_scanout_work\("
                     r"HV_SCANOUT_IDLE_ROWS\)\)\n\s*hv_fb_convert_slice\(HV_FB_SLICE_IDLE\);", exc)
    fiq = function_body(exc, "void hv_exc_fiq(struct exc_info *ctx)")
    fast = fiq[:fiq.index("// Slow (single threaded) path")]
    assert fast.index("hv_update_fiq(ctx);") < fast.index("hv_scanout_work(HV_SCANOUT_FAST_ROWS);") \
        < fast.index("hv_arm_tick(true);")
    hv_c = source("hv.c")
    conv = function_body(hv_c, "void hv_fb_convert_slice(u32 lines)")
    assert "if (!hv_fb_shadow || hv_scanout_owns_panel())\n        return;" in conv
    tick = function_body(hv_c, "void hv_tick(struct exc_info *ctx)")
    assert tick.index("hv_fatal_stage(HV_FATAL_STAGE_FB);") < tick.index("hv_scanout_tick();") < \
        tick.index("hv_fb_convert_slice(HV_FB_SLICE_TICK);")
    init = function_body(hv_c, "static bool hv_fb_init(struct boot_args *gba)")
    assert "hv_scanout_set_panel((u64)hv_fb_shadow, hv_fb_scanout, gba->video.width" in init
    assert "hv_scanout_report();" in function_body(hv_c, "void hv_fb_report(void)")
    assert "hv_scanout_fatal_report();" in source("hv_fatal.c")
    vspi = source("hv_vdev_spi.c")
    assert "case HV_VDEV_SPI_SCANOUT:\n            return VDEV_SPI_DEV_SCANOUT;" in vspi
    assert "#define VDEV_SPI_COUNT 3" in source("hv_vdev_spi.h")
    make = (REPO / "Makefile").read_text()
    assert "input_model.o hv_input.o scanout_model.o hv_scanout.o \\" in make


def test_configure_and_session():
    hv_c = source("hv.c")
    configure = function_body(hv_c, "int hv_windows_configure(u64 flags)")
    assert ("if ((flags & HV_WINDOWS_T8152_SCANOUT) &&\n        (!(flags & HV_WINDOWS_T8152_SOFT_GIC)"
            " || !(flags & HV_WINDOWS_FB_SHADOW) ||\n         board_id != 0x22))\n        return -1;"
            ) in configure
    assert "if (flags & HV_WINDOWS_T8152_SCANOUT)\n        hv_scanout_enable();" in configure
    start = hv_c[hv_c.index("static int hv_start_session(void *entry, u64 regs[4])\n{"):]
    start = start[:start.index("\n}\n")]
    assert "hv_input_verify() < 0 ||\n                             hv_scanout_verify() < 0" in start
    assert start.index("hv_fb_init(") < start.index("hv_scanout_verify()") < \
        start.index("hv_scanout_guest_entered();") < start.index("hv_enter_guest(") < \
        start.index("hv_scanout_guest_exited(status);")


def test_dcp_untouched():
    # The GPU plan's Do-not list: no DCP access, no denylist change, no AIC line for 992.
    for name in ("hv_scanout.c", "scanout_model.c"):
        text = re.sub(r"/\*.*?\*/", "", source(name), flags=re.S)
        text = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', text)  # log strings may say "no DCP"
        assert not re.search(r"dcp|disp0|pmgr|dart|aic_", text, re.I), name
    assert "992" not in re.sub(r"/\*.*?\*/", "", source("hv_aic_alias.c"), flags=re.S)


# ---- the launcher ---------------------------------------------------------------------------

@pytest.mark.parametrize("value, mhz", [("off", 0), ("60", 60000), ("120", 120000),
                                        ("59940", 59940), ("on", 60000)])
def test_scanout_flag_parsing(value, mhz):
    assert w.parse_scanout_flag(value) == mhz


@pytest.mark.parametrize("value", ["", "fast", "0", "999", "240001", "-60"])
def test_scanout_flag_refuses(value):
    with pytest.raises(ValueError):
        w.parse_scanout_flag(value)


def test_fd_marker():
    fd = bytearray(b"\xff" * 0x1000)
    assert w.fd_scanout_marker(bytes(fd)) is None
    fd[0x150:0x160] = w.scanout_fd_marker_bytes()
    assert w.fd_scanout_marker(bytes(fd)) == (0xF60000000, 992)
    assert bytes(fd[0x150:0x160]) == b"J804SCN1" + struct.pack("<II", 0xF60000000 >> 12, 992)
    fd[0x15C] = 0
    with pytest.raises(ValueError):
        w.fd_scanout_marker(bytes(fd))


CAPS2_FULL = 0x41574932000000ff


@pytest.mark.parametrize("mhz, chip, board, caps2, fd, shadow, message", [
    (60000, 0x8122, 0x22, CAPS2_FULL, True, True, "T8152"),
    (60000, 0x8152, 0x24, CAPS2_FULL, True, True, "J804"),
    (60000, 0x8152, 0x22, 0x415749320000007f, True, True, "CAPS2"),
    (60000, 0x8152, 0x22, 0xff, True, True, "CAPS2"),
    (60000, 0x8152, 0x22, CAPS2_FULL, False, True, "J804SCN1"),
    (60000, 0x8152, 0x22, CAPS2_FULL, True, False, "shadow"),
    (0, 0x8152, 0x22, CAPS2_FULL, True, True, "--windows-scanout"),
])
def test_scanout_request_refusals(mhz, chip, board, caps2, fd, shadow, message):
    with pytest.raises(ValueError, match=message):
        w.check_scanout_request(mhz, chip, board, caps2, fd, shadow)


def test_scanout_request_accepts():
    w.check_scanout_request(60000, 0x8152, 0x22, CAPS2_FULL, True, True)
    w.check_scanout_request(0, 0x8152, 0x22, 0x7f, False, True)


def test_handshake_bytes():
    blob = w.scanout_handshake(120000)
    assert len(blob) == w.SCANOUT_HANDSHAKE_SIZE == 32
    base, size, gsiv, features, mhz, version = struct.unpack("<QIIIII", blob[:28])
    assert (base, size, gsiv, mhz, version) == (0xF60000000, 0x4000, 992, 120000, 1)
    assert features == 0x1f and blob[28:] == b"\0" * 4


def test_launcher_order():
    hv = (REPO / "proxyclient/aura/hv/__init__.py").read_text()
    body = hv[hv.index("def windows_scanout_attach"):]
    body = body[:body.index("\n    def ", 10)]
    assert body.index("SCANOUT_OP_ATTACH") < body.index("scanout_handshake(")
    assert "if ret != 0" in body
    run = (REPO / "proxyclient/tools/run_guest.py").read_text()
    assert "--windows-scanout" in run


def test_contract_matches_mu_header():
    # The Mu side (docs/windows-t8152.md "G6-S"): checked once a Mu tree carries it.
    tree = os.environ.get("AURA_MU_TREE")
    if not tree:
        pytest.skip("AURA_MU_TREE not set")
    header = Path(tree) / "Platform/MacBookPro2026Pkg/Include/J804Windows.h"
    if not header.exists() or "J804_SCANOUT_BASE" not in header.read_text():
        pytest.skip("Mu tree without the G6-S header")
    text = header.read_text()
    values = dict(re.findall(r"#define (J804_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text))
    assert int(values["J804_SCANOUT_BASE"], 0) == w.SCANOUT_BASE
    assert int(values["J804_SCANOUT_SIZE"], 0) == w.SCANOUT_SIZE
    assert int(values["J804_GSIV_SCANOUT"], 0) == w.SCANOUT_GSIV
    assert int(values["J804_SCANOUT_FD_MARKER_OFFSET"], 0) == w.SCANOUT_FD_MARKER_OFFSET
    assert int(values["J804_SCANOUT_ADT_PROPERTY_SIZE"], 0) == w.SCANOUT_HANDSHAKE_SIZE
    assert int(values["J804_SCANOUT_VERSION"], 0) == w.SCANOUT_VERSION
    required = int(values["J804_SCANOUT_FEATURES_REQUIRED"], 0)
    known = int(values["J804_SCANOUT_FEATURES_KNOWN"], 0)
    assert w.SCANOUT_FEATURES & required == required and w.SCANOUT_FEATURES & ~known == 0
    assert f'"{w.SCANOUT_HANDSHAKE_PROPERTY}"' in text


def test_marker_in_a_built_fd():
    # A scanout FD built from that tree (AURA_SCANOUT_FD), and the plain G5b FD if given.
    fd = os.environ.get("AURA_SCANOUT_FD")
    if not fd:
        pytest.skip("AURA_SCANOUT_FD not set")
    data = Path(fd).read_bytes()
    assert w.fd_scanout_marker(data) == (w.SCANOUT_BASE, w.SCANOUT_GSIV)
    assert w.fd_input_marker(data) == (w.INPUT_BASE, w.INPUT_GSIV)
    plain = os.environ.get("AURA_PLAIN_FD")
    if plain:
        assert w.fd_scanout_marker(Path(plain).read_bytes()) is None
