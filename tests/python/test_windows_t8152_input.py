# SPDX-License-Identifier: GPL-2.0-only
"""I0: the carrier's Mac-fed input device for the J804 Windows guest.

Failure list, written before the code (windows-m6/INPUT-DESIGN.md section 5):

Device model (input_model.c, replayed unchanged by tests/e2e/windows_input_host.c):
  F1  garbage before, between and inside frames: skipped, counted, the next frame still found
  F2  a bad CRC: the frame is dropped and the scan resumes at the byte after its sync, so a
      valid frame inside it is still found
  F3  len > 64 and F4 an unknown type: refused at the header, before waiting for a payload;
      F3b a damaged header claiming a long payload (or a frame cut short) holds the bytes
      behind it only until the feed is quiet for 50 ms, then they are re-scanned
  F5  a good CRC with an invalid payload (lengths, n > 16, usages outside 0x04..0x9F, X/Y above
      32767, -32768 motion, -128 wheel, buttons above 5, consumer above 0x3FF, HELLO version or
      flags): no effect, counted
  F6  any split of the byte stream gives the same reports and counters
  F7  sequence gaps counted, never enforced; HELLO restarts the sequence
  F8  held-key watchdog: 2 s without a valid frame releases every held host key, button and
      consumer usage; KEEPALIVE refreshes it; invalid frames do not
  F9  frames before the driver's ENABLE only update state; ENABLE emits the current state
  F10 ENABLE 1->0 flushes; RESET flushes, re-emits, clears OVERFLOW; ENABLE+RESET emit once
  F11 overflow keeps every collection's final state; pointer reports are dropped first;
      F11b the O(n) victim choice drops exactly what the original quadratic rule dropped
  F12 POP on an empty queue or with the wrong value does nothing
  F13 the line is exactly ENABLE && IRQ_EN && a report latched
  F14 8/16-bit, unaligned, LDP-style and out-of-page accesses: RAZ/WI, counted
  F15 RO registers ignore writes, WO registers and holes read 0
  F16 output reports: the keyboard LED report is taken, everything else refused and counted
  F17 the latched report and SEQ never change under the driver while the queue overflows
  F18 the keyboard report is the OR of all sources (the internal keyboard joins at I1)
  F19 CRC-32 is zlib's
  F20 report bytes are the driver descriptor's layout (checked against this file's encoder)
  F21 SPI 990 alone behaves as before; 991 is live only with the input configure flag
  F22 seeded random streams and register traffic keep every invariant (ASan/UBSan)

Glue (hv_input.c, replayed unchanged by tests/e2e/windows_input_glue_host.c against stubs):
  G1  ATTACH refusals (flag, chip, board, sources, area, after entry, hook failure) attach
      nothing and leave the vUART RX to the guest
  G2  one ATTACH per carrier session
  G3  the vUART RX belongs to the host feed exactly while attached
  G4  the tick reads at most 256 bytes, never more than is buffered (the CDC read would
      block), only while the guest runs, and survives a read failure
  G5  the SPI is driven only on line changes, only INTID 991
  G6  bad widths through the hook are RAZ/WI, never a guest abort
  G7  the watchdog runs on the tick with the system counter
  G8  hv_start refuses with the flag set and nothing (or a foreign leaf) on the page
  G9  guest exit releases every host key, state only (the vSPI is not driven after exit);
      DETACH after entry is refused
  G11 host bytes sent between ATTACH and entry are discarded at entry; the parser and the
      sequence restart clean
  G10 STATUS indices

Launcher (windows.py, hv/__init__.py, run_guest.py):
  L1  --windows-input defaults to off: no configure flag, no ATTACH, no handshake
  L2  host needs T8152, J804, CAPS2 bit 3 and the FD's J804INP1 marker; an FD with the marker
      needs --windows-input (both directions); a marker with other values is refused
  L3  host needs --windows-serial cdc and the software GIC CPU interface
  L4  host+kbd and host+kbd+tp are accepted (I1/I2); kbd or tp without host is refused; the
      resident's Q_SOURCES must cover them; the MTP area plan, the firmware container check
      and the parameter page (parsed by the carrier's own C parser) are in
      test_windows_t8152_input_mtp.py
  L5  the handshake is written only after ATTACH returned 0, and is 24 bytes <QIIII>
  L6  the frame encoder's bytes are parsed by the carrier model into the expected reports
"""

import json
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
sys.path.insert(0, str(REPO / "proxyclient"))
sys.path.insert(0, str(Path(__file__).resolve().parent))

from aura import windows as w  # noqa: E402

MODEL_CASES = ["crc", "reset_values", "report_layout", "before_enable", "pop_and_line",
               "enable_reset", "no_duplicate", "garbage_resync", "bad_crc_rescan", "bad_header",
               "stalled_header", "bad_payload", "split_feed", "seq_gaps", "watchdog",
               "release_all", "merge", "sources", "overflow", "overflow_reference", "access_widths",
               "out_reports", "spi_gate", "fuzz"]
GLUE_CASES = ["attach_refusals", "attach_once", "poll_budget", "line_sync", "bad_width",
              "watchdog", "exit", "pre_entry", "status", "host_only_no_mtp", "kbd_attach",
              "kbd_attach_down", "kbd_merge", "tp_attach", "tp_attach_kbd_only", "tp_attach_none",
              "kbd_attach_no_tp"]

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

# Stub headers for compiling hv_input.c unchanged on the host.
GLUE_STUBS = {
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
extern u64 stub_cntpct, stub_cntfrq;
#define stub_mrs_CNTPCT_EL0 stub_cntpct
#define stub_mrs_CNTFRQ_EL0 stub_cntfrq
#define mrs(reg) (stub_mrs_##reg)
typedef struct { int held; } spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = {0}
static inline void spin_lock(spinlock_t *l) { if (l->held++) __builtin_trap(); }
static inline void spin_unlock(spinlock_t *l) { if (!l->held--) __builtin_trap(); }
''',
    "iodev.h": r'''
#pragma once
#include "types.h"
typedef enum { IODEV_UART, IODEV_USB_VUART = 7 } iodev_id_t;
ssize_t iodev_can_read(iodev_id_t id);
ssize_t iodev_read(iodev_id_t id, void *buf, size_t length);
''',
    "hv.h": r'''
#pragma once
#include "types.h"
#include "hv_input.h"
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


def clang():
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    return cc


def corpus():
    """L6: frames from the launcher's encoder, with junk and one damaged frame between them."""
    parts = [b"\x00\xffM", w.input_hello(seq=1),
             w.input_keys(0x02, [0x04, 0x28], seq=2),
             b"M6\x02\x40",                                 # a header that will fail its CRC
             w.input_abs(0x01, 16384, 32767, wheel=-3, pan=1, seq=3),
             w.input_rel(0x04, -5, 300, seq=4),
             w.input_consumer(0xE9, seq=5)]
    bad = bytearray(w.input_keys(0, [0x05], seq=6))
    bad[-1] ^= 0x01
    parts += [bytes(bad), w.input_frame(w.INPUT_KEEPALIVE, seq=7),
              w.input_frame(w.INPUT_RELEASE_ALL, seq=8)]
    return b"".join(parts)


@pytest.fixture(scope="module")
def replay(tmp_path_factory):
    cc = clang()
    out = tmp_path_factory.mktemp("input")
    (out / "types.h").write_text(TYPES_H)
    for name in ("input_model.c", "input_model.h", "hv_vdev_spi.c", "hv_vdev_spi.h"):
        shutil.copy2(WIN / name, out / name)
    (out / "corpus.bin").write_bytes(corpus())
    binary = out / "input-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-iquote", str(out), str(out / "input_model.c"),
                            str(out / "hv_vdev_spi.c"),
                            str(REPO / "tests/e2e/windows_input_host.c"), "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary), "1000000", str(out / "corpus.bin")],
                            capture_output=True, text=True, timeout=600)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    artifact = os.environ.get("AURA_INPUT_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text(result.stdout)
    return result, {line["case"]: line for line in lines}, [line["case"] for line in lines]


@pytest.fixture(scope="module")
def glue_replay(tmp_path_factory):
    cc = clang()
    out = tmp_path_factory.mktemp("input-glue")
    for name, text in GLUE_STUBS.items():
        (out / name).write_text(text)
    for name in ("hv_input.c", "hv_input.h", "input_model.c", "input_model.h", "hv_vdev_spi.c",
                 "hv_vdev_spi.h", "hv_mtp_t8152.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "input-glue-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-DHV_INPUT_HOST_TEST", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-iquote", str(out),
                            str(out / "hv_input.c"), str(out / "input_model.c"),
                            str(out / "hv_vdev_spi.c"),
                            str(REPO / "tests/e2e/windows_input_glue_host.c"), "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    return result, {line["case"]: line for line in lines}, [line["case"] for line in lines], \
        result.stdout


# ---- the device model ------------------------------------------------------------------------

def test_model_replay_passes(replay):
    result, cases, order = replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert order == MODEL_CASES + ["corpus"]
    assert all(cases[name]["result"] == "passed" for name in order)
    assert cases["fuzz"]["iterations"] == 1000000 and cases["fuzz"]["frames_ok"] > 100000


def test_crc_is_zlib(replay):
    # F19, independently of the model's own check value.
    _, cases, _ = replay
    buf = bytes.fromhex(cases["crc"]["buf"])
    assert cases["crc"]["crc"] == zlib.crc32(buf)


def keyboard_report(mods, usages):
    """ID 1 from the driver descriptor (INPUT-DESIGN.md 1.5): id, 8 modifier bits, 160 bits of
    usages 0x00-0x9F, LSB first."""
    bitmap = bytearray(20)
    for u in usages:
        bitmap[u // 8] |= 1 << (u % 8)
    return bytes([1, mods]) + bytes(bitmap)


def absolute_report(buttons, x, y, wheel=0, pan=0):
    return struct.pack("<BBHHbb", 2, buttons, x, y, wheel, pan)


def relative_report(buttons, dx, dy, wheel=0, pan=0):
    return struct.pack("<BBhhbb", 3, buttons, dx, dy, wheel, pan)


def consumer_report(usage):
    return struct.pack("<BH", 4, usage)


def test_report_bytes_follow_the_descriptor(replay):
    # F20: the model's bytes against this encoder of the HID descriptor's field order.
    _, cases, _ = replay
    c = cases["report_layout"]
    assert bytes.fromhex(c["keyboard"]) == keyboard_report(0x02, [0x04, 0x9F, 0x28])
    assert bytes.fromhex(c["absolute"]) == absolute_report(0x01, 32767, 12345, -1, 2)
    assert bytes.fromhex(c["relative"]) == relative_report(0x12, -32767, 100, 127, -127)
    assert bytes.fromhex(c["consumer"]) == consumer_report(0xE9)


def test_launcher_frames_reach_the_model(replay):
    # L6: the launcher's encoder -> the carrier's parser, end to end.
    _, cases, _ = replay
    c = cases["corpus"]
    reports = [bytes.fromhex(r) for r in c["reports"]]
    assert reports == [
        keyboard_report(0, []), consumer_report(0),            # ENABLE: the current state
        keyboard_report(0x02, [0x04, 0x28]),
        absolute_report(0x01, 16384, 32767, -3, 1),
        relative_report(0x04, -5, 300),
        consumer_report(0xE9),
        keyboard_report(0, []), absolute_report(0, 16384, 32767), relative_report(0, 0, 0),
        consumer_report(0),                                    # RELEASE_ALL
    ]
    # HELLO, KEYS, ABS, REL, CONSUMER, KEEPALIVE, RELEASE_ALL; the damaged KEYS (seq 6) is the
    # one gap.
    assert c["frames_ok"] == 7 and c["bad_crc"] >= 2 and c["seq_gaps"] == 1


# ---- the glue --------------------------------------------------------------------------------

def test_glue_replay_passes(glue_replay):
    result, cases, order, _ = glue_replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert order == GLUE_CASES
    assert all(cases[name]["result"] == "passed" for name in order)


def test_glue_drives_only_991_on_changes(glue_replay):
    # G5: the logged hv_t8152_gic_spi_level() calls of the line case.
    _, cases, _, _ = glue_replay
    assert cases["line_sync"]["spi"] == "991:1,991:0,991:1,991:0"
    assert cases["attach_refusals"]["spi"] == ""


def test_glue_logs_once_and_reports(glue_replay):
    _, _, _, stdout = glue_replay
    assert stdout.count("is not an aligned 32/64-bit access") == 1
    assert stdout.count("the vUART RX read failed") == 1
    assert "HV: input: host frames 1 ok, 1 bad" in stdout
    assert "host keys and buttons released" in stdout


# ---- carrier sources (rules the replays cannot see) ------------------------------------------

def test_contract_constants_agree():
    hdr = source("hv_input.h")
    model = source("input_model.h")
    spi = source("hv_vdev_spi.h")
    assert re.search(r"#define HV_INPUT_BASE\s+0xf50000000UL", hdr)
    assert re.search(r"#define HV_INPUT_SIZE\s+0x4000UL", hdr)
    assert re.search(r"#define HV_INPUT_INTID\s+991u", hdr)
    assert re.search(r"#define HV_VDEV_SPI_INPUT 991u", spi)
    assert re.search(r"#define HV_WINDOWS_CAPS2_INPUT BIT\(3\)", hdr)
    assert re.search(r"#define HV_INPUT_RX_BUDGET 256u", hdr)
    assert re.search(r"#define HV_INPUT_WATCHDOG_MS 2000u", hdr)
    assert re.search(r"#define INPUT_ID_VALUE\s+0x4e49364du", model)
    assert re.search(r"#define INPUT_HOST_SYNC0\s+0x4du", model)
    assert re.search(r"#define INPUT_HOST_SYNC1\s+0x36u", model)
    assert re.search(r"#define HV_WINDOWS_T8152_INPUT BIT\(21\)", source("hv.h"))
    assert (w.INPUT_BASE, w.INPUT_SIZE, w.INPUT_GSIV, w.CAPS2_INPUT, w.CONFIG_T8152_INPUT) == \
        (0xF50000000, 0x4000, 991, 1 << 3, 1 << 21)
    assert w.INPUT_SYNC == b"M6" and w.INPUT_MAX_PAYLOAD == 64 and w.INPUT_WATCHDOG_S == 2.0
    types = dict(re.findall(r"INPUT_HOST_(\w+) = (0x[0-9a-f]+),", model))
    assert {k: int(v, 16) for k, v in types.items()} == {
        "HELLO": w.INPUT_HELLO, "KEYS": w.INPUT_KEYS, "ABS": w.INPUT_ABS, "REL": w.INPUT_REL,
        "CONSUMER": w.INPUT_CONSUMER, "KEEPALIVE": w.INPUT_KEEPALIVE,
        "RELEASE_ALL": w.INPUT_RELEASE_ALL}
    stats = dict(re.findall(r"INPUT_ST_(\w+) = (\d+),", model))
    names = {"FRAMES_OK": "frames_ok", "BAD_CRC": "bad_crc", "BAD_HEADER": "bad_header",
             "BAD_PAYLOAD": "bad_payload", "SKIPPED": "skipped", "SEQ_GAPS": "seq_gaps",
             "WATCHDOG": "watchdog", "REPORTS": "reports", "HIGH_WATER": "high_water",
             "MTP": "mtp", "OUT_OK": "out_ok", "OUT_BAD": "out_bad", "HELLOS": "hellos",
             "BAD_ACCESS": "bad_access", "RX_BYTES": "rx_bytes", "DROPPED": "dropped"}
    assert {names[k]: int(v) for k, v in stats.items() if k in names} == \
        {v: w.INPUT_STATUS[v] for v in names.values()}
    status = dict(re.findall(r"HV_INPUT_S_(\w+) = (\d+),", hdr))
    assert {k.lower(): int(v) for k, v in status.items()} == {
        "attached": 16, "ctrl": 17, "depth": 18, "dropped": 19, "leds": 20, "line": 21,
        "owns_rx": 22, "mtp_state": 23, "mtp_reason": 24, "mtp_kbd": 25, "mtp_tp": 26,
        "mtp_rtkit": 27, "mtp_poll": 28, "mtp_errors": 29, "mtp_smc": 30, "mtp_gpio": 31}
    assert w.INPUT_STATUS["dropped_now"] == 19
    query = dict(re.findall(r"HV_INPUT_Q_(\w+) = (\d+),", hdr))
    assert {k.lower(): int(v) for k, v in query.items()} == w.INPUT_Q
    proxy_c = (REPO / "src/proxy.c").read_text()
    proxy_h = (REPO / "src/proxy.h").read_text()
    proxy_py = (REPO / "proxyclient/aura/proxy.py").read_text()
    assert "P_HV_WINDOWS_INPUT = 0xc89," in proxy_h and "P_HV_WINDOWS_INPUT = 0xc89" in proxy_py
    assert "P_HV_WINDOWS_GPU = 0xc88," in proxy_h  # G0's op, merged in j804-windows-next
    assert "HV_WINDOWS_CAPS2_USB |\n                            HV_WINDOWS_CAPS2_GPU | " \
        "HV_WINDOWS_CAPS2_INPUT" in proxy_c
    assert "hv_input_proxy(request->args[0], request->args[1], request->args[2]," in proxy_c
    # CAPS keeps its ABI 1 value.
    assert "reply->retval = 0x4157494e00017fffULL;" in proxy_c


def test_merged_feature_numbers_are_distinct():
    # j804-windows-next carries D1, D4, G0 and I0 together: proxy ops, CAPS2 bits, configure
    # flags, FD markers, emulated SPIs and the launcher's CAPS2/config constants must not clash.
    proxy_h = re.sub(r"/\*.*?\*/", "", (REPO / "src/proxy.h").read_text(), flags=re.S)
    ops = re.findall(r"(P_HV_WINDOWS_\w+) = (0x[0-9a-f]+),", proxy_h)
    values = [int(v, 16) for _, v in ops]
    assert len(values) == len(set(values))
    assert dict(ops)["P_HV_WINDOWS_GPU"] == "0xc88" and dict(ops)["P_HV_WINDOWS_INPUT"] == "0xc89"
    py = (REPO / "proxyclient/aura/proxy.py").read_text()
    assert "P_HV_WINDOWS_GPU = 0xc88" in py and "P_HV_WINDOWS_INPUT = 0xc89" in py
    caps2 = {}
    for name in ("ahci", "usb", "gpu", "input"):
        hdr = (WIN / f"hv_{name}.h").read_text()
        caps2[name] = int(re.search(r"#define HV_WINDOWS_CAPS2_%s\s+BIT\((\d+)\)" % name.upper(),
                                    hdr).group(1))
    assert caps2 == {"ahci": 0, "usb": 1, "gpu": 2, "input": 3}
    assert (w.CAPS2_AHCI, w.CAPS2_USB, w.CAPS2_GPU, w.CAPS2_INPUT) == (1, 2, 4, 8)
    proxy_c = (REPO / "src/proxy.c").read_text()
    for name in caps2:
        assert f"HV_WINDOWS_CAPS2_{name.upper()}" in proxy_c
    hv_h = (WIN / "hv.h").read_text()
    flags = dict(re.findall(r"#define (HV_WINDOWS_T8152_\w+) BIT\((\d+)\)", hv_h))
    assert len(set(flags.values())) == len(flags)
    assert (flags["HV_WINDOWS_T8152_AHCI"], flags["HV_WINDOWS_T8152_USB"],
            flags["HV_WINDOWS_T8152_INPUT"]) == ("19", "20", "21")
    assert (w.CONFIG_T8152_AHCI, w.CONFIG_T8152_USB, w.CONFIG_T8152_INPUT) == \
        (0x80000, 0x100000, 0x200000)
    # FD markers: AHCI 0x110, USB 0x120 (two 16-byte slots), input 0x140; G0 has none (Mu's GPU0
    # comes from the handoff page the 16-byte /chosen property points at).
    markers = [(w.AHCI_FD_MARKER_OFFSET, w.AHCI_FD_MARKER), *w.USB_FD_MARKER.values(),
               (w.INPUT_FD_MARKER_OFFSET, w.INPUT_FD_MARKER)]
    assert markers == [(0x110, b"J804AHC1"), (0x120, b"J804USB1"), (0x130, b"J804USB3"),
                       (0x140, b"J804INP1")]
    spans = sorted((off, off + 16) for off, _ in markers)
    assert all(a[1] <= b[0] for a, b in zip(spans, spans[1:]))
    # Emulated SPIs 990/991 and the USB aliases 995/997 sit in the carrier block that the G0
    # alias slots refuse; the four /chosen handshakes have distinct names.
    assert len({990, w.INPUT_GSIV, *w.USB_GSIV.values()}) == 4
    alias = (WIN / "hv_aic_alias.c").read_text()
    assert "#define T8152_CARRIER_SPI_LO 980" in alias and \
        "#define T8152_CARRIER_SPI_HI 1000" in alias
    assert len({w.USB_HANDSHAKE_PROPERTY, w.GPU_HANDSHAKE_PROPERTY,
                w.INPUT_HANDSHAKE_PROPERTY}) == 3


def test_vspi_is_gated_per_device():
    # F21 in the carrier: every vSPI entry asks for the INTID's own device.
    exc = source("hv_exc.c")
    live = function_body(exc, "static bool hv_t8152_vspi_live(u32 intid)")
    assert "hv_ahci_enabled() ? VDEV_SPI_DEV_AHCI : 0" in live
    assert "hv_input_enabled() ? VDEV_SPI_DEV_INPUT : 0" in live
    assert "vdev_spi_live(intid, devices)" in live
    level = function_body(exc, "void hv_t8152_gic_spi_level(u32 intid, bool level)")
    assert level.split("{", 1)[1].lstrip().startswith("if (!hv_t8152_vspi_live(intid) ||")
    reapply = function_body(exc, "static void hv_t8152_vspi_reapply(u32 intid)")
    assert "if (!hv_t8152_vspi_live(intid) || !hv_t8152_gic_owned()" in reapply
    refresh = function_body(exc, "void hv_t8152_vspi_refresh(void)")
    assert "hv_t8152_vspi_reapply(vdev_spi_intids[i]);" in refresh
    alias = re.sub(r"/\*.*?\*/", "", source("hv_aic_alias.c"), flags=re.S)
    assert "991" not in alias
    assert all(int(p) != 991 for p, _ in re.findall(r"\{(\d+), (\d+)\}", alias))


def test_vuart_rx_belongs_to_the_feed():
    # G3 in the carrier: the guest's UART never sees the host feed's bytes.
    vuart = (REPO / "src/hv/hv_vuart.c").read_text()
    queued = function_body(vuart, "static ssize_t vuart_rx_queued(void)")
    assert queued.index("hv_input_owns_vuart_rx()") < queued.index("return 0;") < \
        queued.index("return iodev_can_read(IODEV_USB_VUART);")
    update = function_body(vuart, "static void update_irq(void)")
    assert "if ((rx_queued = vuart_rx_queued())) {" in update
    assert "iodev_can_read(IODEV_USB_VUART)" not in update
    handler = function_body(vuart, "static bool handle_vuart(")
    urxh = handler[handler.index("case URXH:"):]
    assert urxh.index("if (vuart_rx_queued()) {") < urxh.index("iodev_read(IODEV_USB_VUART")
    # Only the tick's service reads the RX for the feed, after the vUART poll took the events.
    hv = source("hv.c")
    tick = function_body(hv, "void hv_tick(struct exc_info *ctx)").replace(
        "hv_tick_host_io(ctx);",
        function_body(hv, "static void hv_tick_host_io(struct exc_info *ctx)"))
    assert tick.index("hv_vuart_poll();") < tick.index("hv_input_poll();") < \
        tick.index("HV_FATAL_STAGE_DONE")


def test_hv_start_and_configure():
    # G8 and the configure rules.
    hv_c = source("hv.c")
    start = hv_c[hv_c.index("static int hv_start_session(void *entry, u64 regs[4])\n{"):]
    start = start[:start.index("\n}\n")]
    assert "hv_ahci_verify() < 0 || hv_input_verify() < 0" in start
    assert start.index("hv_input_verify()") < start.index("hv_input_guest_entered();") < \
        start.index("hv_enter_guest(") < start.index("hv_input_guest_exited(status);")
    configure = function_body(hv_c, "int hv_windows_configure(u64 flags)")
    assert ("HV_WINDOWS_T8152_USB | HV_WINDOWS_T8152_INPUT | HV_WINDOWS_T8152_SCANOUT))\n"
            "        return -1;") in configure
    assert ("if ((flags & HV_WINDOWS_T8152_INPUT) &&\n        (!(flags & HV_WINDOWS_T8152_SOFT_GIC)"
            " || board_id != 0x22))\n        return -1;") in configure
    assert "if (flags & HV_WINDOWS_T8152_INPUT)\n        hv_input_enable();" in configure
    assert "hv_input_report();" in function_body(hv_c, "void hv_fb_report(void)")
    make = (REPO / "Makefile").read_text()
    assert "input_model.o hv_input.o" in make


def test_clean_room_headers():
    for name in ("input_model.c", "input_model.h", "hv_input.c", "hv_input.h"):
        text = source(name)
        assert text.startswith("/* SPDX-License-Identifier: GPL-2.0-only */")
        for foreign in ("AuroraSilicon", "Apple", "x1n1"):
            assert foreign not in text, (name, foreign)


# ---- launcher --------------------------------------------------------------------------------

@pytest.mark.parametrize("value, sources", [
    ("off", ()), ("disabled", ()), ("OFF", ()), ("host", ("host",)), (" Host ", ("host",))])
def test_input_flag_parsing(value, sources):
    assert w.parse_windows_input(value) == sources


@pytest.mark.parametrize("value, sources", [("host+kbd", ("host", "kbd")),
                                            ("HOST+KBD+TP", ("host", "kbd", "tp"))])
def test_internal_sources_parse(value, sources):
    # L4
    assert w.parse_windows_input(value) == sources
    assert w.input_needs_area(sources)
    assert not w.input_needs_area(("host",))


@pytest.mark.parametrize("value", ["kbd+tp", "host+tp", "tp+host", "kbd+host"])
def test_internal_sources_need_host_and_keyboard(value):
    with pytest.raises(ValueError, match="host source always stays on"):
        w.parse_windows_input(value)


@pytest.mark.parametrize("value", ["kbd", "on", "host+host", "tp", "usb"])
def test_input_flag_refuses_unknown(value):
    with pytest.raises(ValueError):
        w.parse_windows_input(value)


def input_fd(base=0xF50000000, gsiv=991, magic=b"J804INP1"):
    fd = bytearray(0x1000)
    fd[0x140:0x150] = magic + struct.pack("<II", base >> 12, gsiv)
    return bytes(fd)


CAPS2 = w.CAPS2_MAGIC | w.CAPS2_AHCI | w.CAPS2_USB | w.CAPS2_INPUT


def test_fd_marker():
    assert w.fd_input_marker(input_fd()) == (0xF50000000, 991)
    assert w.fd_input_marker(bytes(0x1000)) is None
    assert w.fd_input_marker(input_fd(magic=b"J804USB1")) is None
    assert w.fd_input_marker(b"\0" * 0x145) is None


@pytest.mark.parametrize("sources, chip, board, caps2, fd, serial, message", [
    ((), 0x8152, 0x22, CAPS2, input_fd(), "cdc", "--windows-input is off"),
    (("host",), 0x8152, 0x22, CAPS2, input_fd(gsiv=990), "cdc", "differs from the carrier"),
    (("host",), 0x8152, 0x22, CAPS2, input_fd(base=0xF40000000), "cdc", "differs"),
    (("host",), 0x8142, 0x22, CAPS2, input_fd(), "cdc", "T8152"),
    (("host",), 0x8152, 0x24, CAPS2, input_fd(), "cdc", "J804 board"),
    (("host",), 0x8152, 0x22, w.CAPS2_MAGIC | w.CAPS2_USB, input_fd(), "cdc", "CAPS2 bit 3"),
    (("host",), 0x8152, 0x22, 0, input_fd(), "cdc", "CAPS2 bit 3"),
    (("host",), 0x8152, 0x22, CAPS2, bytes(0x1000), "cdc", "no J804INP1 marker"),
    (("host",), 0x8152, 0x22, CAPS2, input_fd(), "disabled", "--windows-serial cdc"),
])
def test_input_request_refusals(sources, chip, board, caps2, fd, serial, message):
    # L2, L3
    with pytest.raises(ValueError, match=re.escape(message)):
        w.validate_input_request(sources, chip, board, caps2, fd, serial)


def test_input_request_accepts():
    w.validate_input_request(("host",), 0x8152, 0x22, CAPS2, input_fd(), "cdc")
    # Off with an FD that has no marker: nothing to check, any chip.
    w.validate_input_request((), 0x8142, 0x24, 0, bytes(0x1000), "disabled")


def test_contract_query_and_handshake():
    answers = {"version": 1, "base": 0xF50000000, "size": 0x4000, "intid": 991,
               "features": 0x1000F, "sources": 1}
    assert w.validate_input_contract(answers.get) == 0x1000F
    for name, bad in (("version", 2), ("base", 0xF40000000), ("size", 0x8000), ("intid", 990),
                      ("features", 0x10007)):
        with pytest.raises(ValueError):
            w.validate_input_contract({**answers, name: bad}.get)
    value = w.input_handshake(("host",), 0x1000F)
    assert len(value) == w.INPUT_HANDSHAKE_SIZE == 24
    assert struct.unpack("<QIIII", value) == (0xF50000000, 0x4000, 991, 0x1000F, 1)
    # I1/I2: the handshake carries what FEATURES says is live after ATTACH, never more than
    # was asked; a resident without the host bit is refused.
    sources = ("host", "kbd", "tp")
    assert struct.unpack("<QIIII", w.input_handshake(sources, 0x7000F))[3] == 0x7000F
    assert struct.unpack("<QIIII", w.input_handshake(sources, 0x3000F))[3] == 0x3000F
    assert struct.unpack("<QIIII", w.input_handshake(("host",), 0x7000F))[3] == 0x1000F
    with pytest.raises(ValueError, match="host source"):
        w.input_handshake(sources, 0x6000F)
    assert w.input_live_sources(sources, 0x3000F) == (["host", "kbd"], ["tp"])
    # Q_SOURCES: an I0 carrier (1) refuses host+kbd; I1 (3) refuses the trackpad.
    w.check_input_sources(("host", "kbd"), 3)
    w.check_input_sources(sources, 7)
    for have, want in ((1, ("host", "kbd")), (3, sources)):
        with pytest.raises(ValueError, match="rebuild the carrier"):
            w.check_input_sources(want, have)
    assert w.input_attach_mask(sources) == 7
    assert w.input_attach_mask(("host", "kbd"), "fkeys") == 0x103
    with pytest.raises(ValueError):
        w.input_attach_mask(("host",), "fkeys")
    with pytest.raises(ValueError):
        w.input_handshake((), 0x1000F)
    assert w.describe_input_attach(0) == "attached"
    assert "source" in w.describe_input_attach((1 << 64) - 2)
    assert w.describe_input_attach((1 << 64) - 9) == "error -9"


def test_encoder_layout_and_limits():
    frame = w.input_keys(0x02, [0x04], seq=0x1234)
    assert frame[:6] == b"M6\x02\x03\x34\x12" and frame[6:9] == b"\x02\x01\x04"
    assert struct.unpack("<I", frame[9:])[0] == zlib.crc32(frame[2:9])
    assert w.input_frame(w.INPUT_KEEPALIVE)[3] == 0 and len(w.input_frame(w.INPUT_KEEPALIVE)) == 10
    assert len(w.input_frame(w.INPUT_KEYS, b"\0" * 64)) == 74
    for call in (lambda: w.input_frame(w.INPUT_KEYS, b"\0" * 65),
                 lambda: w.input_keys(0, [0x03]), lambda: w.input_keys(0, [0xA0]),
                 lambda: w.input_keys(0, list(range(4, 21))),
                 lambda: w.input_abs(0, 32768, 0), lambda: w.input_abs(0x20, 0, 0),
                 lambda: w.input_rel(0, -32768, 0), lambda: w.input_rel(0, 0, 0, wheel=-128),
                 lambda: w.input_consumer(0x400)):
        with pytest.raises(ValueError):
            call()


def _launch_with_input(monkeypatch, value, cpuif=None, serial=None):
    import test_windows_t8152_launch as launch
    from aura.hv import HV
    captured = {}
    real_init = HV.init

    def init(self):
        self.windows_input = value
        if cpuif:
            self.windows_gic_cpuif = cpuif
        if serial:
            self.windows_serial = serial
        captured["hv"] = self
        return real_init(self)

    monkeypatch.setattr(HV, "init", init)
    return launch.run(), captured["hv"]


def test_hv_launch_without_input_sets_no_flag(monkeypatch):
    # L1: off by default.
    r, hv = _launch_with_input(monkeypatch, "off")
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert not config[1] & w.CONFIG_T8152_INPUT


def test_hv_launch_flag_follows_the_option(monkeypatch):
    # host sets BIT(10); the handshake is never written by init.
    from aura.adt import load_adt
    r, hv = _launch_with_input(monkeypatch, "host")
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert config[1] & w.CONFIG_T8152_INPUT and config[1] & w.CONFIG_T8152_SOFT_GIC
    props = {p["name"] for p in load_adt(r.writes[hv.adt_base])["/chosen"].tostruct()
             ["properties"]}
    assert w.INPUT_HANDSHAKE_PROPERTY not in props


def test_hv_default_is_off():
    from aura.hv import HV
    assert "self.windows_input = \"off\"" in (REPO / "proxyclient/aura/hv/__init__.py").read_text()
    assert HV.__init__.__code__ is not None


@pytest.mark.parametrize("cpuif, serial", [("carrier", None), (None, "disabled")])
def test_hv_refuses_input_without_soft_gic_or_cdc(monkeypatch, cpuif, serial):
    # L3
    with pytest.raises(ValueError, match="T8152 input"):
        _launch_with_input(monkeypatch, "host", cpuif=cpuif, serial=serial)


def test_hv_handshake_reaches_the_guest_adt_only_when_set(monkeypatch):
    # L5
    from aura.adt import load_adt
    from aura.hv import HV
    value = w.input_handshake(("host",), 0x1000F)
    real_start = HV.start

    def start(self):
        self.set_windows_input_handshake(value)
        return real_start(self)

    monkeypatch.setattr(HV, "start", start)
    r, hv = _launch_with_input(monkeypatch, "host")
    props = {p["name"]: p["value"]
             for p in load_adt(r.writes[hv.adt_base])["/chosen"].tostruct()["properties"]}
    assert props[w.INPUT_HANDSHAKE_PROPERTY] == value
    with pytest.raises(ValueError, match="24 bytes"):
        hv.set_windows_input_handshake(value[:20])
    hv.windows_input = "off"
    with pytest.raises(ValueError, match="--windows-input"):
        hv.set_windows_input_handshake(value)


def test_run_guest_input_order_and_refusals():
    # L1, L5: plan before init, ATTACH after USB and before start, the handshake only after
    # ATTACH returned 0 and the contract matched, STATUS at exit.
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    assert "parser.add_argument('--windows-input', default='off'" in src
    t8152 = src[src.index("def run_windows_t8152"):
                src.index("_windows_caps = windows_resident_caps()")]
    assert t8152.index("input_sources = windows_t8152_input_plan(payload, u)") < \
        t8152.index("hv.init()")
    assert t8152.index('hv.windows_input = "+".join(input_sources) if input_sources else "off"') \
        < t8152.index("hv.init()")
    # G5b order: input ATTACH (the MTP bring-up, which can refuse) before the GPU's ADMIT/POWER.
    assert t8152.index("windows_t8152_usb_attach(hv, usb_ports, ahci)") < \
        t8152.index("windows_t8152_input_attach(hv, input_sources, usb_ports, input_mtp, "
                    "gpu_mode)") < \
        t8152.index("windows_t8152_gpu_admit(hv, gpu_mode, usb_ports, ahci, input_sources, "
                    "input_mtp)") < \
        t8152.index("result = hv.start()") < \
        t8152.index("windows_t8152_input_status(input_sources)")
    plan = src[src.index("def windows_t8152_input_plan"):
               src.index("def windows_t8152_input_attach")]
    assert "w.parse_windows_input(args.windows_input)" in plan
    assert "w.validate_input_request(sources, chosen.chip_id" in plan
    assert "args.windows_serial" in plan
    attach = src[src.index("def windows_t8152_input_attach"):
                 src.index("def windows_t8152_input_status")]
    assert attach.index("w.validate_input_contract(") < \
        attach.index("mask = w.input_attach_mask(sources, args.windows_input_fn_default)") < \
        attach.index("w.INPUT_OP_ATTACH, mask") < \
        attach.index("if result != 0:") < \
        attach.index("features = p.hv_windows_input(w.INPUT_OP_QUERY, w.INPUT_Q[\"features\"])") \
        < attach.index("hv.set_windows_input_handshake(w.input_handshake(sources, features))")
    # I1/I2: Q_SOURCES first, then the firmware and the page into the area, then ATTACH.
    assert attach.index("w.check_input_sources(") < \
        attach.index("iface.writemem(mtp[\"base\"] + w.INPUT_AREA_FW, mtp[\"payload\"])") < \
        attach.index("iface.writemem(mtp[\"base\"], mtp[\"params\"])") < \
        attach.index("w.INPUT_OP_ATTACH, mask, mtp[\"base\"], mtp[\"size\"]")
    require = attach[attach.index("if down and args.windows_input_require_mtp:"):]
    assert require.index("w.INPUT_OP_DETACH") < require.index("refuse(")
    # The area is carved before any plan, the window check covers it.
    assert t8152.index("top, input_mtp = windows_t8152_input_area(top)") < \
        t8152.index("ahci = windows_t8152_ahci_nvme_plan(base, top, payload)")
    assert "t8152_check_window(u.adt, ba, base, window_top, occupied)" in t8152
    area = src[src.index("def windows_t8152_input_area"):
               src.index("def windows_t8152_input_attach")]
    assert "w.parse_tp_firmware(blob)" in area and "hashlib.sha256(blob).hexdigest()" in area
    assert "print(blob" not in area and "print(payload" not in area
    failed = attach[attach.index("except ValueError as exc:\n"):]
    assert failed.index("w.INPUT_OP_DETACH") < failed.index("refuse(")
    # Merged with G0: every input refusal after USB ATTACH detaches USB before exiting.
    helper = attach[attach.index("def refuse(message):"):attach.index("features = ")]
    assert helper.index("w.USB_OP_DETACH") < helper.index("sys.exit(message)")
    assert "sys.exit(" not in attach.replace("sys.exit(message)", "")
    # A failed DETACH after a refused handshake still reaches refuse() (USB detach).
    detach = failed[:failed.index("refuse(")]
    assert "try:" in detach and "except ProxyRemoteError:" in detach
    # hv.start() refused before entry: input is detached as USB is (RX back to the console).
    start_fail = t8152[t8152.index("result = hv.start()"):]
    start_fail = start_fail[:start_fail.index("sys.exit(f\"Hypervisor refused")]
    assert "if input_sources and not stuck:" in start_fail
    assert start_fail.index("p.hv_windows_input(INPUT_OP_DETACH)") < \
        start_fail.index("except (ProxyError, UartError, OSError):", start_fail.index("INPUT_OP_DETACH"))
    assert "INPUT_OP_DETACH" in t8152[:t8152.index("hv.init()")]
    tail = src.split("_windows_caps = windows_resident_caps()", 1)[1]
    assert "args.windows_input not in ('off', 'disabled')" in tail


def test_contract_matches_mu_header():
    # The Mu side (docs/windows-t8152.md "I0"): checked once a Mu tree carries it.
    tree = os.environ.get("AURA_MU_TREE")
    if not tree:
        pytest.skip("AURA_MU_TREE not set")
    header = Path(tree) / "Platform/MacBookPro2026Pkg/Include/J804Windows.h"
    if not header.exists() or "J804_INPUT_BASE" not in header.read_text():
        pytest.skip("Mu tree without the I0 header")
    text = header.read_text()
    values = dict(re.findall(r"#define (J804_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text))
    assert int(values["J804_INPUT_BASE"], 0) == w.INPUT_BASE
    assert int(values["J804_INPUT_SIZE"], 0) == w.INPUT_SIZE
    assert int(values["J804_GSIV_INPUT"], 0) == w.INPUT_GSIV
    assert int(values["J804_INPUT_FD_MARKER_OFFSET"], 0) == w.INPUT_FD_MARKER_OFFSET
    assert int(values["J804_INPUT_ADT_PROPERTY_SIZE"], 0) == w.INPUT_HANDSHAKE_SIZE
    assert f'"{w.INPUT_HANDSHAKE_PROPERTY}"' in text
