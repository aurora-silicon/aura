# SPDX-License-Identifier: MIT
"""D1: the carrier-emulated AHCI disk on T8152 (J804), host only.

Failure modes F1-F18 are listed in windows-m6/D1-AHCI-DESIGN.md section 7; each test names
the ones it covers.

1. The device model, src/hv/windows/ahci_model.c, and the SPI routing, hv_vdev_spi.c, are
   compiled unchanged with the host clang (address and undefined-behaviour sanitizers) and
   replayed by tests/e2e/windows_ahci_host.c: register reset values, IDENTIFY, DMA/PIO reads
   and writes, count-0 transfers, range and PRD errors, every out-of-window span, port state
   rules, the interrupt matrix, COMRESET/SRST/HBA reset (PxCLB/PxFB kept), aborted commands,
   device settings (transfer mode, SET MULTIPLE MODE, the MULTIPLE commands, EXECUTE DEVICE
   DIAGNOSTIC), widths, routing, the one-state-machine SPI plan, CRC-32 and a seeded
   10^6-step fuzz with canaries around guest RAM and the store (F1-F9, F16).  IDENTIFY bytes
   are checked against an independent encoder below (F7), the CRC against zlib.
   The carrier glue hv_ahci.c is compiled unchanged against stub headers and driven by
   tests/e2e/windows_ahci_store_host.c: attach/keep/seal, the overwrite guard, keep-unsealed
   after a failed entry and after a SoC reset (F10).
2. Source rules for the carrier glue (F6, F12, F18): the EOI path re-applies a still-high
   carrier SPI, distributor writes and the GIC claim re-evaluate it, 990 is never an AIC
   alias, hv_start refuses without a READY store, the store is denied to stage 2.
3. The launcher (F10, F11, F17, F18): the window split, the descriptor, the FD marker, the
   image checks, sparse upload with the host CRC, and the HV lifecycle with the handshake
   property in the guest ADT.
Nothing here touches hardware.  The artifact is the replay transcript (JSON lines).
"""

import hashlib
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

TYPES_H = r"""
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef int32_t s32;
typedef int64_t s64;
#define BIT(x) (1UL << (x))
"""

CASES = ["reset_values", "identify", "dma_ext", "pio", "count_zero", "range_errors",
         "prd_space", "prd_format", "out_of_window", "port_state", "irq_matrix", "resets",
         "abort_paths", "non_data", "device_settings", "access_widths", "spi_route", "spi_plan",
         "crc", "fuzz"]
STORE_CASES = ["lifecycle", "flags", "overwrite_guard", "unsealed_failed_entry",
               "unsealed_after_reset", "fatal_trace"]

# Stub headers for compiling hv_ahci.c on the host (windows_ahci_store_host.c).  u64 is
# unsigned long, as on the AArch64 target, so the resident's printf formats stay valid.
STORE_STUBS = {
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
#define BIT(x) (1UL << (x))
''',
    "string.h": "#pragma once\n#include <string.h>\n",
    "soc.h": '#pragma once\n#include "types.h"\n#define T8152 0x8152\nextern u32 chip_id;\n',
    "smp.h": r'''
#pragma once
#include "types.h"
#define MAX_CPUS 16
extern int stub_cpu;
static inline int smp_id(void) { return stub_cpu; }
''',
    "utils.h": r'''
#pragma once
#include <stdio.h>
#include "types.h"
#define UNUSED(x)     (void)(x)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define GENMASK(h, l) (((~0UL) << (l)) & (~0UL >> (63 - (h))))
#define FIELD_GET(m, v) (((v) & (m)) >> __builtin_ctzl(m))
extern u64 stub_cntpct, stub_cntfrq;
#define stub_mrs_CNTPCT_EL0 stub_cntpct
#define stub_mrs_CNTFRQ_EL0 stub_cntfrq
#define mrs(reg) (stub_mrs_##reg)
#define sysop(op) do { } while (0)
typedef struct { int unused; } spinlock_t;
#define DECLARE_SPINLOCK(name) spinlock_t name = {0}
static inline void spin_lock(spinlock_t *l) { (void)l; }
static inline void spin_unlock(spinlock_t *l) { (void)l; }
void memcpy128(void *dst, void *src, size_t size);
void memset64(void *dst, u64 value, size_t size);
''',
    "hv.h": r'''
#pragma once
#include "types.h"
#include "hv_ahci.h"
struct exc_info;
#define HV_MAX_RW_WORDS 8
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
typedef bool(hv_pt_leaf_fn)(void *ctx, u64 ipa, u64 size, u64 pte, int level);
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx);
void hv_t8152_gic_spi_level(u32 intid, bool level);
''',
    "hv_t8152.h": r'''
#pragma once
#include "types.h"
bool hv_t8152_guest_ram(u64 *base, u64 *size);
int hv_t8152_reserve_store(u64 start, u64 end);
''',
}
SECTORS = 81920  # windows_ahci_host.c: a 40 MiB disk


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
    out = tmp_path_factory.mktemp("ahci")
    (out / "types.h").write_text(TYPES_H)
    for name in ("ahci_model.c", "ahci_model.h", "hv_vdev_spi.c", "hv_vdev_spi.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "ahci-host"
    subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-I", str(out),
                    str(out / "ahci_model.c"), str(out / "hv_vdev_spi.c"),
                    str(REPO / "tests/e2e/windows_ahci_host.c"), "-o", str(binary)],
                   check=True, capture_output=True, text=True, timeout=180)
    result = subprocess.run([str(binary), str(out), "1000000"], capture_output=True, text=True,
                            timeout=600)
    lines = [json.loads(line) for line in result.stdout.splitlines()]
    artifact = os.environ.get("AURA_AHCI_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text(result.stdout)
    return result, lines, out


@pytest.fixture(scope="module")
def store_replay(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    out = tmp_path_factory.mktemp("ahci-store")
    for name, text in STORE_STUBS.items():
        (out / name).write_text(text)
    for name in ("hv_ahci.c", "hv_ahci.h", "ahci_model.c", "ahci_model.h", "hv_vdev_spi.h",
                 "hv_fatal.h", "ahci_nvme.h", "hv_ahci_nvme.h"):
        shutil.copy2(WIN / name, out / name)
    binary = out / "ahci-store-host"
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-function", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", "-iquote", str(out),
                            str(out / "ahci_model.c"),
                            str(REPO / "tests/e2e/windows_ahci_store_host.c"), "-o", str(binary)],
                           capture_output=True, text=True, timeout=180)
    assert build.returncode == 0, build.stderr[-4000:]
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=300)
    lines = [json.loads(line) for line in result.stdout.splitlines() if line.startswith("{")]
    return result, lines


def test_store_glue_replay_passes(store_replay):
    # F10 and the review of a70881d9: overwrite guard, unsealed keep after a failed entry,
    # unsealed keep after a SoC reset, the heartbeat, flag combinations.
    result, lines = store_replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    assert [line["case"] for line in lines] == STORE_CASES
    assert all(line["result"] == "passed" for line in lines)
    assert "store note: image sha256 " in result.stdout
    assert "the machine was reset since the store was written" in result.stdout
    assert "pass --windows-ahci-overwrite to replace it" in result.stdout


def test_store_glue_fatal_trace(store_replay):
    # D3: what the EL2 fatal report prints about the disk.  The last access is the one under
    # way (IN PROGRESS), the last command is the refused one on cpu 2 with its spans.
    result, _ = store_replay
    text = result.stdout
    text = text[text.index("ahci-report-begin\n"):text.index("ahci-report-end\n")]
    assert re.search(r"HV: fatal: AHCI guest RAM \[0x[0-9a-f]+, 0x[0-9a-f]+\), store "
                     r"\[0x[0-9a-f]+, 0x[0-9a-f]+\), 1 commands\n", text)
    assert re.search(r"AHCI last access: cpu 2, 1000 us ago, IN PROGRESS: 4-byte write "
                     r"ABAR\+0x138 value 0x2\n", text)
    assert re.search(r"AHCI last command access: cpu 2, 1000 us ago, done: ABAR\+0x138 = 0x2; "
                     r"1 command\(s\), last slot 1 ATA 0x25 LBA 0x18 count 2\n", text)
    spans = re.findall(r"AHCI   span (\d+)/(\d+): IPA (0x[0-9a-f]+) \+(0x[0-9a-f]+) "
                       r"(to guest|from guest)( REFUSED \(outside guest RAM\))?\n", text)
    assert spans and spans[-1][5] and spans[-1][3] == "0x200" and spans[-1][4] == "to guest"
    assert len(spans) == min(8, int(spans[-1][1]))


def test_model_replay_passes(replay):
    result, lines, _ = replay
    assert result.returncode == 0, result.stdout[-4000:] + result.stderr[-4000:]
    cases = [line for line in lines if "case" in line]
    assert [line["case"] for line in cases] == CASES
    assert all(line["result"] == "passed" for line in cases)


def test_fuzz_reaches_every_outcome(replay):
    _, lines, _ = replay
    fuzz = next(line["fuzz"] for line in lines if "fuzz" in line)
    # The seeded fuzz must exercise success, ATA errors and fatal errors, and move data.
    assert fuzz["completed"] > 100000 and fuzz["ata_errors"] > 10000 and fuzz["fatal"] > 1000
    assert fuzz["sectors"] > 1000000


def ata_string(text, words):
    raw = text.ljust(words * 2).encode("ascii")
    return [raw[i] << 8 | raw[i + 1] for i in range(0, words * 2, 2)]


def expected_identify(sectors, write_cache=True):
    """ACS-3 7.12.7 / D1-AHCI-DESIGN 3.6 at power-on defaults (UDMA 6 selected, write cache
    on, 16 sectors per DRQ block), written independently of the C model."""
    wd = [0] * 256
    wd[0], wd[1], wd[2], wd[3], wd[6] = 0x0040, 16383, 0xC837, 16, 63
    wd[10:20] = ata_string("AURA-AHCI-0000000001", 10)
    wd[23:27] = ata_string("D1.0", 4)
    wd[27:47] = ata_string("Aura carrier RAM disk", 20)
    wd[47], wd[49], wd[50], wd[53] = 0x8010, 0x0300, 0x4000, 0x0006
    wd[59] = 0x0110                       # 16 sectors per DRQ block, setting valid
    lba28 = min(sectors, 0x0FFFFFFF)
    wd[60], wd[61] = lba28 & 0xFFFF, lba28 >> 16
    wd[63], wd[64] = 0x0007, 0x0003
    wd[65:69] = [120] * 4
    wd[76], wd[80] = 0x000E, 0x07F0
    wd[82], wd[83], wd[84] = 0x4020, 0x7400, 0x4000
    wd[85] = 0x4000 | (0x0020 if write_cache else 0)
    wd[86], wd[87], wd[88] = 0x3400, 0x4000, 0x407F
    for i in range(4):
        wd[100 + i] = (sectors >> (16 * i)) & 0xFFFF
    wd[106], wd[217] = 0x4000, 0x0001
    data = bytearray(struct.pack("<256H", *wd))
    data[510] = 0xA5
    data[511] = (-sum(data[:511])) & 0xFF
    return bytes(data)


def test_identify_matches_an_independent_encoder(replay):
    # F7: byte-exact IDENTIFY, checksum, capacity, NCQ/SMART/TRIM bits consistent with CAP.
    _, lines, _ = replay
    got = bytes.fromhex(next(line["identify"] for line in lines if "identify" in line))
    want = expected_identify(SECTORS)
    assert got == want
    assert sum(got) % 256 == 0
    words = struct.unpack("<256H", got)
    assert not words[76] & 0x100 and not w.AHCI_CAP & (1 << 30)   # no NCQ either side
    assert not words[82] & 1 and not words[169] & 1                # no SMART, no TRIM
    assert words[217] == 1                                          # non-rotating (SSD)


def test_crc_matches_zlib(replay):
    _, lines, out = replay
    data = (out / "crc_input.bin").read_bytes()
    for size, crc in next(line["crc"] for line in lines if "crc" in line):
        assert zlib.crc32(data[:size]) == crc, size


def test_model_and_routing_are_pure():
    for name, allowed in (("ahci_model", {"types.h", "ahci_model.h"}),
                          ("hv_vdev_spi", {"types.h", "hv_vdev_spi.h"})):
        text = source(name + ".c") + source(name + ".h")
        code = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for forbidden in ("mrs(", "msr(", "read32", "write32", "printf", "smp_id", "chip_id",
                          "aic_", "spin_lock", "hv_"):
            if name == "hv_vdev_spi" and forbidden == "hv_":
                continue
            assert forbidden not in code, (name, forbidden)
        assert set(re.findall(r'#include "([^"]+)"', text)) == allowed


def test_clean_room_citations():
    model = source("ahci_model.c") + source("ahci_model.h")
    for cite in ("AHCI §", "ACS", "SATA §"):
        assert cite in model, cite
    for name in ("ahci_model.c", "ahci_model.h", "hv_ahci.c", "hv_ahci.h", "hv_vdev_spi.c",
                 "hv_vdev_spi.h"):
        text = source(name)
        assert text.startswith("/* SPDX-License-Identifier: MIT */"), name
        for foreign in ("qemu", "QEMU", "libahci", "libata", "Copyright"):
            assert foreign not in text.replace("QEMU hw/ide", "").replace(
                "Linux libahci/libata", ""), (name, foreign)


def test_contract_constants_agree():
    hdr = source("hv_ahci.h")
    model = source("ahci_model.h")
    spi = source("hv_vdev_spi.h")
    assert re.search(r"#define HV_AHCI_ABAR\s+0xf40000000UL", hdr)
    assert re.search(r"#define HV_AHCI_ABAR_SIZE\s+0x4000UL", hdr)
    assert re.search(r"#define HV_AHCI_INTID\s+990u", hdr)
    assert re.search(r"#define HV_VDEV_SPI_AHCI 990u", spi)
    assert re.search(r"#define HV_AHCI_DESC_SIZE\s+0x10000UL", hdr)
    assert re.search(r"#define HV_AHCI_STORE_ALIGN \(32UL << 20\)", hdr)
    assert re.search(r"#define HV_AHCI_MIN_GUEST\s+\(4UL << 30\)", hdr)
    assert re.search(r"#define AHCI_MODEL_CAP\s+0x81341f00u", model)
    assert re.search(r"#define HV_WINDOWS_CAPS2_MAGIC 0x4157493200000000ULL", hdr)
    assert (w.AHCI_ABAR, w.AHCI_ABAR_SIZE, w.AHCI_GSIV, w.AHCI_CAP) == \
        (0xF40000000, 0x4000, 990, 0x81341F00)
    assert (w.AHCI_DESC_SIZE, w.AHCI_STORE_ALIGN, w.AHCI_MIN_GUEST) == \
        (0x10000, 32 << 20, 4 << 30)
    assert w.CAPS2_MAGIC == 0x4157493200000000 and w.CAPS2_AHCI == 1
    assert re.search(r"#define HV_WINDOWS_T8152_AHCI BIT\(19\)", source("hv.h"))
    assert w.CONFIG_T8152_AHCI == 1 << 19
    # CAPS keeps its ABI 1 value; CAPS2 and the AHCI op are new opcodes on both sides.
    proxy_c = (REPO / "src/proxy.c").read_text()
    proxy_h = (REPO / "src/proxy.h").read_text()
    proxy_py = (REPO / "proxyclient/aura/proxy.py").read_text()
    assert "reply->retval = 0x4157494e00017fffULL;" in proxy_c
    assert "P_HV_WINDOWS_CAPS2 = 0xc8d," in proxy_h and "P_HV_WINDOWS_AHCI = 0xc8e," in proxy_h
    assert "P_HV_WINDOWS_CAPS2 = 0xc8d" in proxy_py and "P_HV_WINDOWS_AHCI = 0xc8e" in proxy_py
    assert "HV_WINDOWS_CAPS2_MAGIC | HV_WINDOWS_CAPS2_AHCI" in proxy_c
    # The descriptor layout: Python and C agree on every offset.
    assert struct.calcsize(w.AHCI_DESC_FORMAT) == w.AHCI_DESC_CRC_OFF == 0x40
    assert "u8 pad[0x200 - 0x44];" in hdr and w.AHCI_NOTE_OFF == 0x200


def test_contract_matches_mu_header():
    tree = os.environ.get("AURA_MU_TREE")
    if not tree:
        pytest.skip("AURA_MU_TREE not set")
    header = Path(tree) / "Platform/MacBookPro2026Pkg/Include/J804Windows.h"
    if not header.exists() or "J804_AHCI_BASE" not in header.read_text():
        pytest.skip("Mu tree without the D1 header")
    text = header.read_text()
    values = dict(re.findall(r"#define (J804_\w+)\s+(0x[0-9A-Fa-f]+|\d+)\b", text))
    assert int(values["J804_AHCI_BASE"], 0) == w.AHCI_ABAR
    assert int(values["J804_AHCI_SIZE"], 0) == w.AHCI_ABAR_SIZE
    assert int(values["J804_GSIV_AHCI"], 0) == w.AHCI_GSIV
    assert int(values["J804_AHCI_FD_MARKER_OFFSET"], 0) == w.AHCI_FD_MARKER_OFFSET
    assert int(values["J804_AHCI_ADT_PROPERTY_SIZE"], 0) == len(
        w.ahci_handshake(w.plan_t8152_ahci(BASE, TOP, 1 << 30), "disk"))
    assert (int(values["J804_HANDOFF_BASE"], 0), int(values["J804_HANDOFF_TOP"], 0)) == \
        w.J804_HANDOFF
    assert f'"{w.AHCI_PROPERTY}"' in text


# A synthetic window of the same shape as a J804 one (17.52 GiB, above 1 TiB, holding the PrePi
# hand-off copies); no value read from a machine is committed (PROVENANCE.md).
BASE, TOP = 0x10100000000, 0x10561600000


# ---- carrier glue (source rules) -----------------------------------------------------------

def test_eoi_and_distributor_writes_reapply_the_level():
    # F6: deactivation of a carrier SPI re-pends it while the level is high; enables, routing
    # and the claim re-evaluate it; nothing unmasks an AIC line for it.
    exc = source("hv_exc.c")
    handler = function_body(exc, "static bool hv_handle_t8152_gic_cpuif(")
    eoi = handler[handler.index("if (fx.deactivated >= 0)"):]
    assert eoi.index("gicv3_cpuif_clear_pending(c, intid);") < eoi.index(
        "if (vdev_spi_is_virtual(intid))\n                hv_t8152_vspi_reapply(intid);")
    claim = function_body(exc, "void hv_t8152_gic_claim(const char *why)")
    assert claim.index("t8152_gic_owned, true") < claim.index("hv_t8152_vspi_refresh();")
    apply = function_body(exc, "static void hv_t8152_vspi_apply(u32 intid)")
    assert "vdev_spi_route(hv_vgic3_spi_route(intid)" in apply
    assert "vdev_spi_should_pend(level, hv_vgic3_spi_enabled(intid))" in apply
    assert "aic_" not in apply
    assert "vdev_spi_plan(pend, target, pending, active)" in apply
    assert "gicv3_cpuif_is_active(&t8152_cpuif[cpu], intid)" in apply
    level = function_body(exc, "void hv_t8152_gic_spi_level(u32 intid, bool level)")
    assert "spin_lock(&t8152_vspi_lock);" in level and "hv_t8152_gic_owned()" in level
    # Without the AHCI configure flag the M3 path is untouched: every entry returns at once
    # (I0: the gate is per INTID, and 990's device is the AHCI flag).
    assert level.split("{", 1)[1].lstrip().startswith("if (!hv_t8152_vspi_live(intid) ||")
    reapply = function_body(exc, "static void hv_t8152_vspi_reapply(u32 intid)")
    assert "if (!hv_t8152_vspi_live(intid) || !hv_t8152_gic_owned()" in reapply
    live = function_body(exc, "static bool hv_t8152_vspi_live(u32 intid)")
    assert "hv_ahci_enabled() ? VDEV_SPI_DEV_AHCI : 0" in live
    vgic = source("hv_vgic.c")
    dist = function_body(vgic, "static bool handle_vgic_dist_access(")
    assert dist.rstrip().endswith("hv_t8152_vspi_refresh();\n    return register_handled;") or \
        "if (write)\n        hv_t8152_vspi_refresh();\n    return register_handled;" in dist


def test_990_is_never_an_aic_alias():
    alias = source("hv_aic_alias.c")
    assert "990" not in re.sub(r"/\*.*?\*/", "", alias, flags=re.S)
    table = re.findall(r"\{(\d+), (\d+)\}", alias)
    assert all(int(published) != 990 for published, _ in table)


def test_hv_start_refuses_without_a_ready_store():
    # F12, F18: the pre-entry check, RUNNING only once entry is certain, exit recorded.
    hv_c = source("hv.c")
    start = function_body(hv_c, "static int hv_start_session(void *entry, u64 regs[4])\n{")
    assert "hv_ahci_verify() < 0" in start
    assert start.index("hv_ahci_verify() < 0") < start.index("hv_ahci_guest_entered();") < \
        start.index("hv_enter_guest(regs[0]")
    assert start.index("hv_join_cpus()") < start.index("hv_ahci_guest_exited(status);")
    configure = function_body(hv_c, "int hv_windows_configure(u64 flags)")
    assert "(flags & HV_WINDOWS_T8152_AHCI) && !(flags & HV_WINDOWS_T8152_SOFT_GIC)" in configure
    ahci = source("hv_ahci.c")
    verify = function_body(ahci, "int hv_ahci_verify(void)")
    assert "desc->state != HV_AHCI_READY" in verify and "hv_pt_for_each_leaf(hook_leaf" in verify
    seal = function_body(ahci, "static u64 seal_exit(void)")
    assert "exit_status != 1 && exit_status != 2" in seal


def test_store_is_reserved_and_dma_is_bounded_to_guest_ram():
    # F1, F11, F12: the store joins the denylist; every DMA span stays inside guest RAM.
    ahci = source("hv_ahci.c")
    span = function_body(ahci, "static void *env_span(")
    assert "ipa < ram_lo || ipa >= ram_hi || len > ram_hi - ipa" in span
    attach = function_body(ahci, "static u64 attach(")
    assert "base != ram_hi" in attach and "hv_t8152_reserve_store(base, top) < 0" in attach
    assert attach.index("hv_t8152_reserve_store") < attach.index("memset64")
    assert attach.index("hv_t8152_reserve_store") < attach.index("hv_map_hook(HV_AHCI_ABAR")
    t8152 = source("hv_t8152.c")
    reserve = function_body(t8152, "int hv_t8152_reserve_store(u64 start, u64 end)")
    assert "ram_conflicts(start, end, ram_host_top)" in reserve
    assert 'deny_add(start, end - start, "AHCI disk store (D1)")' in reserve
    hook = function_body(ahci, "static bool hv_ahci_mmio(")
    assert hook.rstrip().endswith("return true; /* RAZ/WI, never a guest abort */")


def test_makefile_links_the_model():
    make = (REPO / "Makefile").read_text()
    objects = make[make.index("HV_OBJECTS := $(patsubst %,hv/windows/%"):]
    objects = objects[:objects.index("\nendif")]
    for obj in ("ahci_model.o", "hv_ahci.o", "hv_vdev_spi.o", "ahci_nvme.o", "gpt_check.o",
                "hv_ahci_nvme.o"):
        assert obj in objects, obj


# ---- launcher ------------------------------------------------------------------------------

def test_plan_matches_the_worked_example():
    plan = w.plan_t8152_ahci(BASE, TOP, 12 << 30)
    assert plan.desc_base == 0x105615f0000
    assert plan.store_base == 0x10260000000
    assert plan.store_base + plan.disk_bytes == 0x10560000000
    assert plan.guest_bytes == int(5.5 * 2**30)
    assert w.ahci_max_disk_bytes(BASE, TOP) == 0x3615f0000


@pytest.mark.parametrize("disk, image, message", [
    (0x3615f0000 + 512, None, "less than 4 GiB of guest RAM"),          # F11: RAM floor
    (14 << 30, None, "largest is"),
    (1000, None, "multiple of 512"),
    (512 << 10, None, "at least 1 MiB"),
    (1 << 30, 2 << 30, "larger than the AHCI store"),
])
def test_plan_refusals(disk, image, message):
    with pytest.raises(ValueError, match=message):
        w.plan_t8152_ahci(BASE, TOP, disk, image)


def test_plan_refuses_windows_that_cannot_hold_the_handoff():
    # F11: guest RAM must contain the PrePi hand-off copies below the UEFI region.
    with pytest.raises(ValueError, match="hand-off copies"):
        w.plan_t8152_ahci(0x10230000000, 0x10230000000 + (6 << 30), 1 << 30)
    with pytest.raises(ValueError, match="64 KiB aligned"):
        w.plan_t8152_ahci(BASE, TOP - 0x4000, 1 << 30)
    plan = w.plan_t8152_ahci(BASE, TOP, 12 << 30)
    w.check_t8152_ahci_layout(plan, BASE + (16 << 20), BASE + (600 << 20))
    with pytest.raises(ValueError, match="hand-off"):
        w.check_t8152_ahci_layout(plan, BASE + (16 << 20), w.J804_HANDOFF[0] + 1)
    with pytest.raises(ValueError, match="UEFI region"):
        w.check_t8152_ahci_layout(plan, w.J804_HANDOFF[1], plan.store_base - (64 << 20))


def test_sizes_and_boot_choice():
    assert w.parse_size("12G") == 12 << 30 and w.parse_size("12GiB") == 12 << 30
    assert w.parse_size("0x300000000") == 0x300000000 and w.parse_size("4096M") == 4 << 30
    with pytest.raises(ValueError):
        w.parse_size("twelve")
    assert w.ahci_default_disk_bytes(9 * 2**30 + 1) == 9 * 2**30 + (256 << 20)
    assert w.ahci_default_disk_bytes(100) == 256 << 20
    assert w.resolve_ahci_boot("auto", True) == "ramdisk"
    assert w.resolve_ahci_boot("auto", False) == "disk"
    with pytest.raises(ValueError):
        w.resolve_ahci_boot("ramdisk", False)


def test_handshake_property_layout():
    plan = w.plan_t8152_ahci(BASE, TOP, 12 << 30)
    value = w.ahci_handshake(plan, "ramdisk")
    assert len(value) == 40
    assert struct.unpack("<QQIIQQ", value) == (0xF40000000, 0x4000, 990, 1, plan.store_base, TOP)


def fd_with_marker(marker=None):
    fd = bytearray(b"\xff" * 0x8000)
    if marker is not None:
        fd[0x110:0x120] = marker
    return bytes(fd)


def test_fd_marker_and_contract_check():
    # F17: old resident, wrong values, non-AHCI FD: all refused before hv.init().
    good = fd_with_marker(b"J804AHC1" + struct.pack("<II", 0xF40000, 990))
    assert w.fd_ahci_marker(good) == (0xF40000000, 990)
    assert w.fd_ahci_marker(fd_with_marker()) is None
    answers = {"version": 1, "abar": 0xF40000000, "abar_size": 0x4000, "intid": 990,
               "cap": 0x81341F00, "min_guest": 4 << 30, "store_align": 32 << 20,
               "desc_size": 0x10000}
    w.validate_ahci_contract(w.CAPS2_MAGIC | 1, answers.get, good)
    with pytest.raises(ValueError, match="no AHCI disk"):
        w.validate_ahci_contract(0, answers.get, good)
    with pytest.raises(ValueError, match="no AHCI disk"):
        w.validate_ahci_contract(w.CAPS2_MAGIC, answers.get, good)
    with pytest.raises(ValueError, match="intid"):
        w.validate_ahci_contract(w.CAPS2_MAGIC | 1, {**answers, "intid": 991}.get, good)
    with pytest.raises(ValueError, match="not an AHCI FD"):
        w.validate_ahci_contract(w.CAPS2_MAGIC | 1, answers.get, fd_with_marker())
    bad = fd_with_marker(b"J804AHC1" + struct.pack("<II", 0xF50000, 990))
    with pytest.raises(ValueError, match="differs"):
        w.validate_ahci_contract(w.CAPS2_MAGIC | 1, answers.get, bad)


def test_built_ahci_fd_carries_the_marker():
    fd = os.environ.get("AURA_J804_AHCI_FD")
    if not fd or not Path(fd).exists():
        pytest.skip("AURA_J804_AHCI_FD not set")
    assert w.fd_ahci_marker(Path(fd).read_bytes()) == (w.AHCI_ABAR, w.AHCI_GSIV)


def test_descriptor_round_trip_and_refusals():
    # F10: bad magic, CRC, window, state; keep-unsealed only on request.
    plan = w.plan_t8152_ahci(BASE, TOP, 12 << 30)
    blob = w.pack_ahci_desc(TOP, plan.store_base, plan.disk_bytes, 4, 3, 0x1234abcd, 2, 99)
    desc = w.parse_ahci_desc(blob)
    assert (desc["state_name"], desc["generation"], desc["disk_crc32"], desc["last_exit"]) == \
        ("SEALED", 3, 0x1234abcd, 2)
    assert w.ahci_keep_plan(BASE, TOP, desc) == plan
    with pytest.raises(ValueError, match="CRC"):
        w.parse_ahci_desc(blob[:0x30] + b"\x01" + blob[0x31:])
    with pytest.raises(ValueError, match="magic"):
        w.parse_ahci_desc(b"\0" * 0x44)
    with pytest.raises(ValueError, match="window ending"):
        w.ahci_keep_plan(BASE, TOP + 0x10000, desc)
    running = w.parse_ahci_desc(w.pack_ahci_desc(TOP, plan.store_base, plan.disk_bytes, 3))
    with pytest.raises(ValueError, match="keep-unsealed"):
        w.ahci_keep_plan(BASE, TOP, running)
    assert w.ahci_keep_plan(BASE, TOP, running, accept_unsealed=True) == plan
    loading = w.parse_ahci_desc(w.pack_ahci_desc(TOP, plan.store_base, plan.disk_bytes, 1))
    with pytest.raises(ValueError, match="LOADING"):
        w.ahci_keep_plan(BASE, TOP, loading, accept_unsealed=True)
    moved = w.parse_ahci_desc(w.pack_ahci_desc(TOP, plan.store_base - (32 << 20),
                                               plan.disk_bytes, 4))
    with pytest.raises(ValueError, match="base does not match"):
        w.ahci_keep_plan(BASE, TOP, moved)


def descriptor_blob(state, top=None, note=None, generation=2):
    top = TOP if top is None else top
    plan = w.plan_t8152_ahci(BASE, top, 12 << 30)
    blob = bytearray(w.AHCI_NOTE_OFF + w.AHCI_NOTE_SIZE)
    head = w.pack_ahci_desc(top, plan.store_base, plan.disk_bytes, state, generation, 0x1234, 2)
    blob[:len(head)] = head
    if note is not None:
        blob[w.AHCI_NOTE_OFF:] = note
    return bytes(blob)


def test_host_note_and_keep_expect():
    # Review: keep shows which image the store holds and can insist on one.
    sha = "ab" * 32
    note = w.ahci_host_note(sha, 123456, "2026-11-17T00:00:00Z")
    blob = descriptor_blob(4, note=note)
    parsed = w.parse_ahci_note(blob)
    assert parsed == {"sha256": sha, "bytes": 123456, "uploaded": "2026-11-17T00:00:00Z"}
    assert sha in w.describe_ahci_note(parsed)
    assert w.parse_ahci_note(descriptor_blob(4)) is None
    assert "none" in w.describe_ahci_note(None)
    w.check_ahci_keep_expect(parsed, None)
    w.check_ahci_keep_expect(parsed, sha.upper())
    with pytest.raises(ValueError, match="not " + "cd" * 32):
        w.check_ahci_keep_expect(parsed, "cd" * 32)
    with pytest.raises(ValueError, match="no host note"):
        w.check_ahci_keep_expect(None, sha)
    with pytest.raises(ValueError, match="64-digit"):
        w.check_ahci_keep_expect(parsed, "abc")


def test_fresh_upload_refuses_to_overwrite_a_live_store():
    # Review: --windows-ahci-image over a sealed install is refused without the flag.
    note = w.ahci_host_note("ef" * 32, 1 << 30, "2026-11-17T00:00:00Z")
    for state in (2, 3, 4):  # READY, RUNNING, SEALED
        blob = descriptor_blob(state, note=note)
        with pytest.raises(ValueError, match="--windows-ahci-overwrite"):
            w.check_ahci_fresh_overwrite(blob, TOP, False)
        assert w.check_ahci_fresh_overwrite(blob, TOP, True)["state"] == state
    for state in (0, 1):  # EMPTY, LOADING: nothing to lose
        assert w.check_ahci_fresh_overwrite(descriptor_blob(state), TOP, False) is None
    assert w.check_ahci_fresh_overwrite(bytes(0x300), TOP, False) is None   # no descriptor
    other = descriptor_blob(4, top=TOP + (1 << 30))                          # another window
    assert w.check_ahci_fresh_overwrite(other, TOP, False) is None


def gpt_image(path, sectors, backup=True, sparse_gap=0):
    """A synthetic GPT disk: protective MBR, header at LBA 1, data, optional backup header."""
    with open(path, "wb") as f:
        mbr = bytearray(512)
        mbr[446 + 4] = 0xEE
        mbr[510:512] = b"\x55\xaa"
        f.write(mbr)
        f.write(b"EFI PART" + bytes(504))
        f.write(os.urandom(64 << 10))
        if sparse_gap:
            f.seek(sparse_gap, os.SEEK_CUR)
            f.write(b"\x5a" * 4096)
        f.truncate(sectors * 512)
        if backup:
            f.seek(sectors * 512 - 512)
            f.write(b"EFI PART" + bytes(504))


def test_image_checks(tmp_path):
    img = tmp_path / "disk.img"
    gpt_image(img, 4096)
    assert w.ahci_image_info(img) == (4096 * 512, [])
    gpt_image(img, 4096, backup=False)
    assert w.ahci_image_info(img)[1] == ["no backup GPT header at the last LBA"]
    img.write_bytes(bytes(4096 * 512))
    with pytest.raises(ValueError, match="protective MBR"):
        w.ahci_image_info(img)
    img.write_bytes(bytes(1000))
    with pytest.raises(ValueError, match="multiple of 512"):
        w.ahci_image_info(img)


def test_sparse_upload_skips_holes_and_crc_covers_the_disk(tmp_path):
    img = tmp_path / "disk.img"
    sectors = (96 << 20) // 512
    gpt_image(img, sectors, sparse_gap=40 << 20)
    disk = 128 << 20  # larger than the image: the tail is zero
    writes = {}
    crc, sha, sent = w.upload_ahci_image(img, disk, lambda off, d: writes.__setitem__(off, d),
                                         chunk=16 << 20)
    data = img.read_bytes()
    assert sha == hashlib.sha256(data).hexdigest()
    assert crc == zlib.crc32(data + bytes(disk - len(data)))
    # Only the chunks holding data were sent: 0 (MBR/GPT/random), 32 MiB (the 0x5a block at
    # 40 MiB + 64 KiB + 1 KiB) and 80 MiB (the backup header at 96 MiB - 512).
    assert sorted(writes) == [0, 32 << 20, 80 << 20] and sent == 48 << 20
    rebuilt = bytearray(disk)
    for off, chunk in writes.items():
        rebuilt[off:off + len(chunk)] = chunk
    assert bytes(rebuilt[:len(data)]) == data


# ---- the HV lifecycle with a disk ------------------------------------------------------------

def test_hv_launch_with_ahci_sets_flag_ram_and_handshake(monkeypatch):
    import test_windows_t8152_launch as launch
    from aura.adt import load_adt
    from aura.hv import HV
    from aura.tgtypes import BootArgs_r2

    plan = w.AhciPlan(launch.WINDOW[0], launch.WINDOW[1], 0x100e0000000, 0xffe0000)
    captured = {}
    real_init = HV.init

    def init(self):
        self.windows_guest_ram = (plan.window_base, plan.store_base)
        self.windows_ahci = {"plan": plan, "boot": "disk"}
        captured["hv"] = self
        return real_init(self)

    monkeypatch.setattr(HV, "init", init)
    r = launch.run()
    hv = captured["hv"]
    config = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "config")
    assert config[1] & w.CONFIG_T8152_AHCI and config[1] & w.CONFIG_T8152_SOFT_GIC
    ram = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "guest_ram")
    assert ram[1:3] == (plan.window_base, plan.store_base - plan.window_base)
    # Nothing the host maps reaches the store.
    for ipa, to, size, incr in r.maps:
        if to & 1:
            pa = to & ~0x3fff
            assert pa + size <= plan.store_base or pa >= plan.window_top, hex(pa)
    start = next(t for t in r.trace if isinstance(t, tuple) and t[0] == "start")
    boot = BootArgs_r2.parse(r.writes[start[2]])
    assert boot.phys_base + boot.mem_size == plan.store_base
    guest = load_adt(r.writes[hv.adt_base])
    prop = next(p for p in guest["/chosen"].tostruct()["properties"]
                if p["name"] == w.AHCI_PROPERTY)
    assert prop["value"] == w.ahci_handshake(plan, "disk")


def test_hv_refuses_an_ahci_ram_split_that_does_not_match(monkeypatch):
    import test_windows_t8152_launch as launch
    from aura.hv import HV

    plan = w.AhciPlan(launch.WINDOW[0], launch.WINDOW[1], 0x100e0000000, 0xffe0000)
    real_init = HV.init

    def init(self):
        self.windows_ahci = {"plan": plan, "boot": "disk"}  # guest RAM left at the whole window
        return real_init(self)

    monkeypatch.setattr(HV, "init", init)
    with pytest.raises(ValueError, match="window base, store base"):
        launch.run()


def test_run_guest_ahci_order_and_refusals():
    # F17/F18: refusals before hv.init(); ATTACH after load_raw, upload sealed before start;
    # SEAL_EXIT only after a PSCI exit; WD1 reset refused with a DRAM disk.
    src = (REPO / "proxyclient/tools/run_guest.py").read_text()
    for flag in ("--windows-ahci-image", "--windows-ahci-store-size", "--windows-ahci-keep",
                 "--windows-ahci-keep-unsealed", "--windows-ahci-boot",
                 "--windows-ahci-keep-expect", "--windows-ahci-overwrite"):
        assert f"'{flag}'" in src, flag
    t8152 = src[src.index("def run_windows_t8152"):src.index("_windows_caps = windows_resident_caps()")]
    assert t8152.index("windows_t8152_ahci_plan(base, top, payload)") < t8152.index("hv.init()")
    assert t8152.index("hv.load_raw(data") < t8152.index("windows_t8152_ahci_attach(hv, u, ahci)") \
        < t8152.index("result = hv.start()") < t8152.index("windows_t8152_ahci_seal(result, ahci)")
    plan = src[src.index("def windows_t8152_ahci_plan"):src.index("def windows_t8152_ahci_attach")]
    assert "args.windows_reset_on_exit and ahci_ram_requested()" in plan
    assert "--windows-ahci-keep reuses the sealed disk" in plan
    assert "this is an AHCI FD" in plan
    assert plan.index("check_ahci_fresh_overwrite(blob, top, args.windows_ahci_overwrite)") > \
        plan.index("validate_ahci_contract")
    assert "check_ahci_keep_expect(note, args.windows_ahci_keep_expect)" in plan
    assert "UNSAFE after a hard reset" in src
    attach = src[src.index("def windows_t8152_ahci_attach"):src.index("def windows_t8152_ahci_seal")]
    assert attach.index("AHCI_OP_ATTACH") < attach.index("upload_ahci_image") < \
        attach.index("AHCI_OP_SEAL_UPLOAD")
    seal = src[src.index("def windows_t8152_ahci_seal"):src.index("def run_windows_t8152")]
    assert "flags |= w.AHCI_ATTACH_OVERWRITE" in attach and w.AHCI_ATTACH_OVERWRITE == 4
    assert "if result not in (1, 2):" in seal and seal.index("if result not in (1, 2):") < \
        seal.index("AHCI_OP_SEAL_EXIT")
    tail = src.split("_windows_caps = windows_resident_caps()", 1)[1]
    assert "ahci_requested()" in tail
