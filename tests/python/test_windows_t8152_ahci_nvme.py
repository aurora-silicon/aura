# SPDX-License-Identifier: MIT
"""D5: Windows' AHCI disk on a dedicated internal-NVMe partition (J804), host only.

The failure list comes first: windows-m6/D5-NVME-DESIGN.md section 10 (N1-N22) and the cases of
14.3.  Each test names what it covers.

1. tests/e2e/windows_nvme_host.c builds aura's real src/nvme.c and the carrier's AHCI glue
   (hv_ahci.c, hv_ahci_nvme.c, ahci_nvme.c, gpt_check.c, ahci_model.c) unchanged, with address
   and undefined-behaviour sanitizers, against a simulated T8152 controller (nvme_sim.c) whose
   media has a volatile and a durable layer: only a normal shutdown commits, as on the M6.
   Session cases drive the launcher ops, the guest's ABAR accesses, the exit paths and the reset
   path, then cut power and check that everything Windows was told survived and nothing outside
   the partition changed (N7-N17, the guard fuzz, the exit paths, the lost completion).
2. GPT fixtures come from this file's own writer (independent of gpt_check.c), with GUIDs from a
   fixed seed and the M6's shape (iSC, APFS, Windows, Linux, ESP, RecoveryOS), including
   renumbered layouts; every refusal leaves the media unchanged, the fence unset and the
   controller shut down (N1-N5, N19).
3. Source rules: every hv_start return closes the backend, P_REBOOT goes through
   flush_and_reboot, the NVMe proxy ops refuse while the backend owns the controller, the ANS
   stays denied in stage 2 (N12-N14).
4. The launcher: config parsing, partition keys, the request ABI, refusals (no device).
Nothing here touches hardware.  The artifact is the JSON transcript of the cases.
"""

import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

import pytest

REPO = Path(__file__).resolve().parents[2]
WIN = REPO / "src/hv/windows"
E2E = REPO / "tests/e2e"
sys.path.insert(0, str(REPO / "proxyclient"))

from aura import windows as w  # noqa: E402

BLOCK = 4096
NSZE = 16384  # a 64 MiB outer disk, 4 KiB LBAs
CASES = ["attach_and_identify", "inner_gpt_read", "data_rmw", "guard_fuzz", "guard2_fence",
         "flush_mapping", "write_through", "read_only", "exit_system_off", "exit_system_reset",
         "exit_abort", "refusal_closes", "reset_other_cpu_mid_command",
         "reset_inside_controller_lock", "reset_lost_completion", "reset_shst_stuck_parks",
         "close_failed_then_retry", "quiesce_fails_after_commit", "slow_write_completion", "error_status_write",
         "second_attach", "proxy_and_reboot_rules", "gate_w0"]

# Public type GUIDs (UEFI 2.10, Discoverable Partitions Specification, Apple, Microsoft).
T_ISC = "69646961-6700-11AA-AA11-00306543ECAC"
T_APFS = "7C3457EF-0000-11AA-AA11-00306543ECAC"
T_LINUX = "0FC63DAF-8483-4772-8E79-3D69D8477DE4"
T_ESP = "C12A7328-F81F-11D2-BA4B-00A0C93EC93B"
T_RECOVERY = "52637672-7900-11AA-AA11-00306543ECAC"
T_MS_DATA = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
T_MSR = "E3C9E316-0B5C-4DB8-817D-F92DF00215AE"

RNG = random.Random(0xD5)


def synthetic_guid():
    """A version-4 GUID from the fixed seed: never one of the M6's."""
    return uuid.UUID(int=RNG.getrandbits(128), version=4)


# The synthetic Windows partition: a private type, as the install script creates.
WIN_TYPE = synthetic_guid()
WIN_GUID = synthetic_guid()
DISK_GUID = synthetic_guid()
WIN_NAME = "Aura Windows (test)"
WIN_FIRST, WIN_LAST = 2560, 10751          # 32 MiB, 1 MiB aligned
INNER_SECTORS = 49152                      # a 24 MiB "image" inside the 32 MiB partition


# ---- an independent GPT writer (UEFI 2.10 section 5) ---------------------------------------

def gpt_header(block, my, alt, first_usable, last_usable, disk_guid, entry_lba, count, esize,
               array_crc, revision=0x00010000, size=92):
    h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", revision, size, 0, 0, my, alt,
                    first_usable, last_usable, disk_guid.bytes_le, entry_lba, count, esize,
                    array_crc)
    crc = zlib.crc32(h[:size])
    h = h[:16] + struct.pack("<I", crc) + h[20:]
    return h.ljust(block, b"\0")


def gpt_entry(type_guid, part_guid, first, last, name, esize=128):
    n = name.encode("utf-16-le")[:72].ljust(72, b"\0")
    return struct.pack("<16s16sQQQ72s", uuid.UUID(str(type_guid)).bytes_le, part_guid.bytes_le,
                       first, last, 0, n).ljust(esize, b"\0")


def write_gpt(disk, block, nsze, entries, disk_guid, count=128, esize=128, tweak=None):
    """Primary and backup headers and arrays into `disk` (a bytearray of nsze*block)."""
    tweak = tweak or {}
    array = b"".join(gpt_entry(*e, esize=esize) for e in entries)
    array = array.ljust(count * esize, b"\0")
    blocks = -(-len(array) // block)
    first_usable = 2 + blocks
    last_usable = nsze - 2 - blocks
    crc = zlib.crc32(array)
    prim = gpt_header(block, 1, tweak.get("alt", nsze - 1), first_usable, last_usable,
                      disk_guid, 2, count, esize, crc)
    back = gpt_header(block, nsze - 1, 1, tweak.get("backup_first_usable", first_usable),
                      last_usable, disk_guid, nsze - 1 - blocks, count, esize, crc)
    disk[block:2 * block] = prim
    disk[2 * block:2 * block + len(array)] = array
    disk[(nsze - 1 - blocks) * block:(nsze - 1 - blocks) * block + len(array)] = array
    disk[(nsze - 1) * block:nsze * block] = back
    return first_usable, last_usable


def inner_disk(sectors):
    """A tiny Windows-shaped disk image: protective MBR, 512-byte-LBA GPT (ESP, MSR, data)."""
    disk = bytearray(sectors * 512)
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1,
                               min(sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    disk[:512] = mbr
    entries = [(T_ESP, synthetic_guid(), 2048, 10239, "EFI system partition"),
               (T_MSR, synthetic_guid(), 10240, 12287, "Microsoft reserved partition"),
               (T_MS_DATA, synthetic_guid(), 12288, sectors - 34, "Basic data partition")]
    write_gpt(disk, 512, sectors, entries, synthetic_guid())
    rnd = random.Random(7)
    for start in (2048, 12288, sectors // 2):
        disk[start * 512:start * 512 + 65536] = rnd.randbytes(65536)
    return disk


def m6_disk(win_index=5, win_type=WIN_TYPE, win_guid=WIN_GUID, win_first=WIN_FIRST,
            win_last=WIN_LAST, win_name=WIN_NAME, inner=None, extra=(), count=128, esize=128,
            tweak=None, disk_guid=DISK_GUID):
    """The M6 shape, scaled down: iSC, APFS, [gap], Windows, Linux, ESP, RecoveryOS (room left
    at both ends for a 1024-entry table)."""
    disk = bytearray(NSZE * BLOCK)
    parts = [(T_ISC, synthetic_guid(), 40, 261, "iBootSystemContainer"),
             (T_APFS, synthetic_guid(), 262, 2047, "Container"),
             (T_LINUX, synthetic_guid(), 10752, 14847, "Omarchy"),
             (T_ESP, synthetic_guid(), 14848, 15103, "EFI"),
             (T_RECOVERY, synthetic_guid(), 15104, NSZE - 40, "RecoveryOSContainer")]
    parts += list(extra)
    win = (win_type, win_guid, win_first, win_last, win_name)
    empty = ("00000000-0000-0000-0000-000000000000", uuid.UUID(int=0), 0, 0, "")
    others = iter(parts)
    entries = [win if i == win_index else next(others, empty) for i in range(count)]
    write_gpt(disk, BLOCK, NSZE, entries, disk_guid, count=count, esize=esize, tweak=tweak)
    rnd = random.Random(11)
    # Neighbours with content, so "unchanged outside" means something.
    for b in (261, 2047, 2048, win_first - 1, win_last + 1, 10752, 14848, 15104):
        if 6 <= b < NSZE - 6 and not (win_first <= b <= win_last):
            disk[b * BLOCK:(b + 1) * BLOCK] = rnd.randbytes(BLOCK)
    inner = inner_disk(INNER_SECTORS) if inner is None else inner
    off = win_first * BLOCK
    disk[off:off + len(inner)] = inner
    return disk


def config_values(**over):
    v = {"J804_WIN_PART_TYPE": str(WIN_TYPE), "J804_WIN_PART_UUID": str(WIN_GUID),
         "J804_WIN_PART_NAME": WIN_NAME, "J804_WIN_DISK_GUID": str(DISK_GUID),
         "J804_WIN_PART_FIRST_LBA": str(WIN_FIRST), "J804_WIN_PART_LAST_LBA": str(WIN_LAST),
         "J804_WIN_LBA_BYTES": "4096", "J804_WIN_IMAGE_BYTES": str(INNER_SECTORS * 512)}
    v.update(over)
    return v


def request(flags=w.NVME_RW, **over):
    return w.nvme_request(w.nvme_partition_config(config_values(**over)), flags)


# ---- the build and the runs -------------------------------------------------------------

@pytest.fixture(scope="module")
def host(tmp_path_factory):
    cc = shutil.which("clang")
    if cc is None:
        pytest.skip("no host clang")
    out = tmp_path_factory.mktemp("nvme")
    src = out / "src"
    (out / "build").mkdir()
    src.mkdir()
    (out / "build/build_cfg.h").write_text("#pragma once\n")
    for f in (E2E / "windows_nvme_stubs").glob("*.h"):
        shutil.copy2(f, src / f.name)
    shutil.copy2(REPO / "src/nvme.c", src / "nvme.c")
    shutil.copy2(REPO / "src/nvme.h", src / "nvme.h")
    for name in ("ahci_model.c", "ahci_model.h", "ahci_nvme.c", "ahci_nvme.h", "gpt_check.c",
                 "gpt_check.h", "hv_ahci_nvme.c", "hv_ahci_nvme.h", "hv_ahci.c", "hv_ahci.h",
                 "hv_vdev_spi.h", "hv_fatal.h"):
        shutil.copy2(WIN / name, src / name)
    binary = out / "nvme-host"
    sources = [src / n for n in ("nvme.c", "ahci_model.c", "ahci_nvme.c", "gpt_check.c",
                                 "hv_ahci_nvme.c", "hv_ahci.c")]
    build = subprocess.run([cc, "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unused-parameter", "-Wno-unused-function",
                            "-Wno-format", "-Wno-sign-compare",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-iquote", str(src), "-I", str(E2E), *map(str, sources),
                            str(E2E / "nvme_sim.c"), str(E2E / "windows_nvme_host.c"),
                            "-o", str(binary)], capture_output=True, text=True, timeout=300)
    assert build.returncode == 0, build.stderr[-6000:]
    return binary, out


def run_attach(host, disk, req, name):
    binary, out = host
    img, rq = out / f"{name}.img", out / f"{name}.req"
    img.write_bytes(disk)
    rq.write_bytes(req)
    r = subprocess.run([str(binary), str(img), str(rq), "attach"], capture_output=True,
                       text=True, timeout=120, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    assert r.returncode == 0, r.stdout[-3000:] + r.stderr[-3000:]
    result = json.loads([l for l in r.stdout.splitlines() if l.startswith("{")][-1])
    return result, r.stdout


@pytest.fixture(scope="module")
def sessions(host):
    binary, out = host
    img, rq = out / "valid.img", out / "valid.req"
    img.write_bytes(m6_disk())
    rq.write_bytes(request())
    r = subprocess.run([str(binary), str(img), str(rq), "cases"], capture_output=True, text=True,
                       timeout=1200, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
    lines = [json.loads(l) for l in r.stdout.splitlines() if l.startswith("{")]
    artifact = os.environ.get("AURA_NVME_TRANSCRIPT")
    if artifact:
        Path(artifact).write_text("\n".join(json.dumps(l) for l in lines) + "\n")
    return r, lines


# ---- 1. the session cases ---------------------------------------------------------------

def test_every_session_case_passes(sessions):
    # N7-N17, N21 (the carrier side), 14.3 items 2, 3, 5, 6 (deterministic), 7.
    r, lines = sessions
    cases = [l for l in lines if "case" in l]
    assert [c["case"] for c in cases] == CASES, r.stderr[-6000:]
    bad = [c for c in cases if c["result"] != "passed"]
    assert not bad and r.returncode == 0, (bad, r.stderr[-6000:])


def test_identify_is_512e(sessions):
    # 14.3 item 4: words 106/209/117-118, strings and checksum, decoded independently here.
    _, lines = sessions
    raw = bytes.fromhex(next(l["identify"] for l in lines if "identify" in l))
    words = struct.unpack("<256H", raw)
    assert words[106] == 0x6003          # valid, multiple logical per physical, 2^3
    assert words[209] == 0x4000          # alignment valid, logical sector 0 at offset 0
    assert words[117] == words[118] == 0  # 512-byte logical sectors
    assert words[84] & 0x0040 == 0        # no FUA
    assert words[169] == 0                # no TRIM

    def ata(first, n):
        return b"".join(struct.pack(">H", x) for x in words[first:first + n]).decode().strip()
    assert ata(27, 20) == "Aura carrier NVMe disk"
    assert ata(10, 10) == "AURA-AHCI-NVME000001"
    assert ata(23, 4) == "D5.0"
    sectors = words[100] | words[101] << 16 | words[102] << 32 | words[103] << 48
    assert sectors == (WIN_LAST - WIN_FIRST + 1) * 8
    assert words[255] & 0xFF == 0xA5 and sum(raw) % 256 == 0


def test_fuzz_stayed_inside_the_partition(sessions):
    # 14.3 item 2: every NVMe command of 10^5 random AHCI commands around both ends.
    _, lines = sessions
    fuzz = next(l["fuzz"] for l in lines if "fuzz" in l)
    assert WIN_FIRST <= fuzz["io_min"] and fuzz["io_max"] <= WIN_LAST
    assert fuzz["io_min"] == WIN_FIRST and fuzz["io_max"] == WIN_LAST
    assert fuzz["writes"] > 10000


def test_w0_pattern_matches_the_launcher(sessions):
    # The W0 gate: what the resident writes, the launcher's tools can recompute from Linux.
    _, lines = sessions
    words = next(l["pattern"] for l in lines if "pattern" in l)
    assert int(words[0], 16) == w.nvme_test_pattern(0x5EED, 0, 0)
    assert int(words[1], 16) == w.nvme_test_pattern(0x5EED, 12345, 511)


def test_reset_paths_never_reset_before_the_commit(sessions):
    # 9.3 / 14.3 item 5: X (reset) only after C (SHST done) once anything was written; a stuck
    # shutdown parks (P) and never resets.
    _, lines = sessions
    ev = {l["case"]: l["events"] for l in lines if "case" in l}
    for name in ("exit_system_reset", "reset_other_cpu_mid_command",
                 "reset_inside_controller_lock", "reset_lost_completion"):
        e = ev[name]
        assert "X" in e and e.rfind("C", 0, e.index("X")) > e.rfind("W", 0, e.index("X")), name
    assert "P" in ev["reset_shst_stuck_parks"] and "X" not in ev["reset_shst_stuck_parks"]


# ---- 2. GPT and inner-disk refusals -------------------------------------------------------

def refused(result):
    return (not result["attach"] and not result["fence_after_refusal"] and result["writes"] == 0
            and result["unchanged"] and not result["enabled"])


def corrupt(disk, offset, value=b"\xff"):
    disk[offset:offset + len(value)] = value
    return disk


def renamed_disk():
    return m6_disk(win_name="Something else")


VARIANTS = {
    # name: (disk builder, request overrides, message the resident prints)
    "header_crc": (lambda: corrupt(m6_disk(), BLOCK + 0x30, b"\x01"), {},
                   "GPT header CRC mismatch"),
    "array_crc": (lambda: corrupt(m6_disk(), 2 * BLOCK + 200), {}, "array CRC mismatch"),
    "backup_missing": (lambda: corrupt(m6_disk(), (NSZE - 1) * BLOCK, b"\0" * BLOCK), {},
                       "backup GPT header invalid"),
    "backup_disagrees": (lambda: m6_disk(tweak={"backup_first_usable": 7}), {},
                         "backup GPT header disagrees"),
    "alternate_not_last": (lambda: m6_disk(tweak={"alt": NSZE - 2}), {},
                           "AlternateLBA is not the last LBA"),
    "disk_guid": (lambda: m6_disk(disk_guid=synthetic_guid()), {}, "disk GUID is not the configured"),
    "missing": (lambda: m6_disk(win_guid=synthetic_guid()), {}, "no GPT entry has the configured"),
    "duplicate_guid": (lambda: m6_disk(extra=[(T_LINUX, WIN_GUID, 2100, 2200, "dup")]), {},
                       "share a unique partition GUID"),
    "duplicate_name": (lambda: m6_disk(extra=[(T_LINUX, synthetic_guid(), 2100, 2200, WIN_NAME)]),
                       {}, "another GPT entry has the same name"),
    "name": (renamed_disk, {}, "name is not the configured one"),
    "type": (lambda: m6_disk(win_type=synthetic_guid()), {}, "type GUID is not the configured"),
    "moved": (lambda: m6_disk(win_first=2816, win_last=10751), {}, "first/last LBA"),
    "overlap_neighbour": (lambda: m6_disk(win_last=10800), {"J804_WIN_PART_LAST_LBA": "10800"},
                          "two GPT entries overlap"),
    "overlap_far": (lambda: m6_disk(extra=[(T_LINUX, synthetic_guid(), 15200, 15210, "x")]), {},
                    "two GPT entries overlap"),
    "outside_usable": (lambda: m6_disk(extra=[(T_LINUX, synthetic_guid(), 3, 4, "x")]), {},
                       "outside the usable range"),
    "unaligned": (lambda: m6_disk(win_first=2561), {"J804_WIN_PART_FIRST_LBA": "2560"},
                  "first/last LBA"),
    "inner_missing": (lambda: m6_disk(inner=bytearray(BLOCK)), {}, "no MBR signature"),
    "inner_too_big": (lambda: m6_disk(inner=inner_disk(70000)), {}, "larger than the partition"),
    "image_bigger": (lambda: m6_disk(), {"J804_WIN_IMAGE_BYTES": str(30 << 20)},
                     "smaller than the image"),
}


@pytest.mark.parametrize("name", sorted(VARIANTS))
def test_attach_refusals_leave_everything_untouched(host, name):
    # N1-N4, 14.3 item 1: each refusal leaves the media unchanged, the fence unset, the
    # controller shut down, and prints why.
    build, over, message = VARIANTS[name]
    result, out = run_attach(host, build(), request(**over), name)
    assert refused(result), (result, out[-2000:])
    assert message in out, out[-2000:]
    assert "attach refused" in out


def test_denied_types_are_refused_by_the_resident(host):
    # N3: a request naming a well-known type is refused even if the GPT matches it.  The
    # launcher refuses it first (config check); here the request is built around that check.
    for t in (T_MS_DATA, T_LINUX, T_APFS, T_ESP):
        part = w.nvme_partition_config(config_values())
        blob = bytearray(w.nvme_request(part, w.NVME_RW))
        blob[0x30:0x40] = uuid.UUID(t).bytes_le
        result, out = run_attach(host, m6_disk(win_type=t), bytes(blob), "denied")
        assert refused(result) and "well-known type" in out


@pytest.mark.parametrize("index", [0, 5, 100, 127])
def test_renumbered_layouts_are_found_by_guid(host, index):
    # N19: the entry's position (its "partition number") never matters.
    result, out = run_attach(host, m6_disk(win_index=index), request(), f"index{index}")
    assert result["attach"], out[-2000:]
    assert result["sectors"] == (WIN_LAST - WIN_FIRST + 1) * 8
    assert result["unchanged"] and result["writes"] == 0
    assert "never by number" in out


def test_larger_tables_and_backup_warning(host):
    # Entry size 256 and 1024 entries are valid tables; an inner backup header that is not at
    # the partition's end (sgdisk -e not run) is a warning, not a refusal.
    for kw in ({"esize": 256}, {"count": 1024}):
        result, out = run_attach(host, m6_disk(**kw), request(), "table")
        assert result["attach"], out[-2000:]
    result, out = run_attach(host, m6_disk(), request(), "warn")
    assert result["attach"] and "backup header is not at the partition's end" in out
    inner = inner_disk((WIN_LAST - WIN_FIRST + 1) * 8)
    result, out = run_attach(host, m6_disk(inner=inner), request(), "at-end")
    assert result["attach"] and "backup header is not at" not in out


def test_identify_lba_size_and_events(host):
    # N5 needs a controller with 512-byte LBAs; the simulator's 4 KiB namespace attaches, wakes
    # RTKit once (B), identifies (I) and, on the close, shuts down normally (S, C) and quiesces.
    result, out = run_attach(host, m6_disk(), request(), "events")
    assert result["attach"] and result["state"] == 3
    assert re.fullmatch(r"BE[I]R+S?C?DQ|BEIR+SCDQ", result["events"].replace("RR", "R")) or \
        result["events"].startswith("BEI"), result["events"]


# ---- 3. source rules ---------------------------------------------------------------------

def body(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}\n", start)]


def test_every_hv_start_return_closes_the_backend():
    # 9.2 / 14.3 item 9.
    hv = (WIN / "hv.c").read_text()
    start = body(hv, "int hv_start(void *entry, u64 regs[4])")
    assert "hv_start_session(entry, regs)" in start and "hv_ahci_session_end(ret)" in start
    assert start.count("return") == 1
    assert "hv_ahci_nvme_close(" in body((WIN / "hv_ahci.c").read_text(),
                                         "void hv_ahci_session_end(int status)")


def test_reset_paths_go_through_the_backend():
    utils = (REPO / "src/utils.c").read_text()
    reboot = body(utils, "void flush_and_reboot(void)")
    assert "hv_ahci_nvme_reset_close()" in reboot
    assert reboot.index("hv_ahci_nvme_reset_close()") < reboot.index("reboot();")
    assert "nvme_shutdown()" not in re.sub(r"/\*.*?\*/", "", reboot, flags=re.S)
    proxy = (REPO / "src/proxy.c").read_text()
    case = proxy[proxy.index("case P_REBOOT:"):proxy.index("case P_SLEEP:")]
    assert "hv_ahci_nvme_reboot_allowed" in case and "flush_and_reboot();" in case
    nv = proxy[proxy.index("case P_NVME_INIT:"):proxy.index("case P_MCC_GET_CARVEOUTS:")]
    assert nv.index("hv_ahci_nvme_owns_controller()") < nv.index("nvme_init()")
    assert "HV_WINDOWS_CAPS2_AHCI_NVME" in proxy


def test_ans_stays_hidden_from_the_guest():
    # 5.4: the ANS, its SART and every DART are denied by name; no AIC alias names an ANS line.
    t8152 = (WIN / "hv_t8152.c").read_text()
    prefixes = t8152[t8152.index("deny_prefixes[] = {"):]
    prefixes = prefixes[:prefixes.index("};")]
    for name in ('"ans"', '"sart"', '"dart"'):
        assert name in prefixes
    alias = (WIN / "hv_aic_alias.c").read_text()
    assert "ans" not in re.sub(r"/\*.*?\*/|//[^\n]*", "", alias, flags=re.S).lower()


def test_pure_parts_have_no_platform_calls():
    for name in ("ahci_nvme.c", "gpt_check.c"):
        text = (WIN / name).read_text()
        for word in ("printf", "malloc", "memalign", "spin_lock", "read32", "write32", "mrs("):
            assert word not in text, (name, word)


def test_abi_numbers_agree():
    h = (WIN / "hv_ahci.h").read_text()
    n = (WIN / "hv_ahci_nvme.h").read_text()
    assert "HV_WINDOWS_CAPS2_AHCI_NVME BIT(5)" in h
    assert w.CAPS2_AHCI_NVME == 1 << 5
    for name, value in (("ATTACH_NVME", 5), ("DETACH", 6), ("NVME_STATUS", 7), ("NVME_TEST", 8)):
        assert re.search(rf"HV_AHCI_OP_{name} = {value},", h)
        assert getattr(w, f"AHCI_OP_{name}") == value
    for name, value in (("RW", 0), ("FLUSH_EVERY", 1), ("GATE", 2), ("SCRATCH", 3)):
        assert f"#define HV_AHCI_NVME_{name:<12}BIT({value})" in n or \
            re.search(rf"#define HV_AHCI_NVME_{name}\s+BIT\({value}\)", n)
    assert w.NVME_REQ_SIZE == struct.calcsize(w.NVME_REQ_FORMAT) == 0xC0
    assert w.REBOOT_FORCE == int((REPO / "src/proxy.h").read_text().split(
        "#define P_REBOOT_FORCE ")[1].split("ULL")[0], 16)
    for i, s in enumerate(w.NVME_STATES):
        assert f"HV_AHCI_NVME_{s} = {i}" in n


def test_no_private_values_in_the_tree():
    # 14.3 item 8: the M6's GUIDs and serials live only in j804-private-data.
    private = Path(os.environ.get("J804_PRIVATE_DATA", "/nonexistent")) / w.NVME_CONFIG_NAME
    if not private.is_file():
        pytest.skip("no private config here")
    values = [v for k, v in w.parse_env_file(private.read_text()).items()
              if k in ("J804_WIN_PART_UUID", "J804_WIN_DISK_GUID", "J804_WIN_PART_TYPE") and v]
    tracked = subprocess.run(["git", "-C", str(REPO), "ls-files"], capture_output=True,
                             text=True, check=True).stdout.split()
    for rel in tracked:
        path = REPO / rel
        if not path.is_file() or path.stat().st_size > 4 << 20:
            continue
        data = path.read_bytes().lower()
        for v in values:
            assert v.lower().encode() not in data, rel


# ---- 4. the launcher ---------------------------------------------------------------------

def test_request_layout():
    blob = request(w.NVME_RW | w.NVME_FLUSH_EVERY)
    assert len(blob) == 0xC0 and blob[:8] == b"AWNVME01"
    assert struct.unpack_from("<II", blob, 8) == (1, 3)
    assert blob[0x10:0x20] == DISK_GUID.bytes_le and blob[0x20:0x30] == WIN_GUID.bytes_le
    assert blob[0x30:0x40] == WIN_TYPE.bytes_le
    assert struct.unpack_from("<QQQ", blob, 0x40) == (WIN_FIRST, WIN_LAST, INNER_SECTORS * 512)
    assert blob[0x60:0xA8].decode("utf-16-le").rstrip("\0") == WIN_NAME


def test_config_parsing_and_refusals(tmp_path):
    text = "# comment\nexport J804_WIN_PART_NAME='A b'\nJ804_WIN_PART_TYPE=\"x\"\n"
    assert w.parse_env_file(text) == {"J804_WIN_PART_NAME": "A b", "J804_WIN_PART_TYPE": "x"}
    with pytest.raises(ValueError, match="lacks J804_WIN_DISK_GUID"):
        w.nvme_partition_config({k: v for k, v in config_values().items()
                                 if k != "J804_WIN_DISK_GUID"})
    for over, message in (({"J804_WIN_PART_TYPE": T_MS_DATA}, "private type GUID"),
                          ({"J804_WIN_LBA_BYTES": "512"}, "4096-byte LBAs"),
                          ({"J804_WIN_PART_FIRST_LBA": "2561"}, "1 MiB boundary"),
                          ({"J804_WIN_IMAGE_BYTES": str(64 << 20)}, "smaller than the image"),
                          ({"J804_WIN_OTHER_PARTS": "10000-11000"}, "overlaps another"),
                          ({"J804_WIN_PART_UUID": "nope"}, "is not a GUID"),
                          ({"J804_WIN_PART_FIRST_LBA": "p3"}, "decimal LBA")):
        with pytest.raises(ValueError, match=message):
            w.nvme_partition_config(config_values(**over))
    ok = w.nvme_partition_config(config_values(J804_WIN_OTHER_PARTS="6-261,10752-14847"))
    assert ok.size == 32 << 20 and ok.sectors == 65536
    path = tmp_path / "windows-partition.env"
    path.write_text("".join(f"{k}={v}\n" for k, v in config_values().items()))
    assert w.load_nvme_config(path) == w.nvme_partition_config(config_values())
    with pytest.raises(ValueError, match="no partition config"):
        w.load_nvme_config(tmp_path / "missing.env")


def test_partition_key_is_guid_or_name_never_a_number():
    part = w.nvme_partition_config(config_values())
    assert w.resolve_nvme_partition(part, str(WIN_GUID).upper()) is part
    assert w.resolve_nvme_partition(part, WIN_NAME) is part
    for key in ("6", "p6", "nvme0n1p6", "/dev/nvme0n1p6"):
        with pytest.raises(ValueError, match="partition number"):
            w.resolve_nvme_partition(part, key)
    with pytest.raises(ValueError, match="matches neither"):
        w.resolve_nvme_partition(part, str(synthetic_guid()))


def test_contract_and_handshake():
    with pytest.raises(ValueError, match="bit 5"):
        w.validate_nvme_contract(w.CAPS2_MAGIC | w.CAPS2_AHCI)
    w.validate_nvme_contract(w.CAPS2_MAGIC | w.CAPS2_AHCI | w.CAPS2_AHCI_NVME)
    hs = w.ahci_nvme_handshake("disk")
    assert len(hs) == 24 and struct.unpack("<QQII", hs) == (w.AHCI_ABAR, w.AHCI_ABAR_SIZE,
                                                          w.AHCI_GSIV, 0)
    plan = w.nvme_ahci_plan(0x10000000000, 0x10460000000)
    assert plan.guest_bytes == 0x460000000 and plan.store_base == plan.window_top


RUN_GUEST = (REPO / "proxyclient/tools/run_guest.py").read_text()


def pyfunc(text, signature):
    start = text.index(signature)
    end = text.find("\ndef ", start + 1)
    return text[start:end if end > 0 else len(text)]


def test_run_guest_nvme_options_and_refusals():
    for opt in ("--windows-ahci-backend", "--windows-ahci-partition", "--windows-ahci-nvme-access",
                "--windows-ahci-nvme-config", "--windows-ahci-nvme-flush"):
        assert f"'{opt}'" in RUN_GUEST
    plan = pyfunc(RUN_GUEST, "def windows_t8152_ahci_nvme_plan(")
    for message in ("not with \"\n", "--windows-ahci-image, -store-size", "--windows-ahci-nvme-access",
                    "--windows-ahci-partition", "bit 5", "ramdisk"):
        assert message in plan, message
    exit_ = pyfunc(RUN_GUEST, "def run_windows_t8152(")
    assert exit_.index("windows_t8152_ahci_nvme_close(") < exit_.index("windows_system_reset(p)")
    reset = pyfunc(RUN_GUEST, "def windows_t8152_ahci_nvme_close(")
    assert "CLEAN" in reset and "AHCI_OP_DETACH" in reset
