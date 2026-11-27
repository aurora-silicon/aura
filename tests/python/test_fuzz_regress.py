# SPDX-License-Identifier: MIT
"""Replay the fuzz targets of tests/fuzz/ over their synthetic seeds and the inputs that once
broke them (tests/fuzz/regress/<target>/), without libFuzzer and with ASan and UBSan.

The ADT reader (rust/src/adt.rs) is built with overflow checks and panic=abort, so a Rust panic
fails the run as a crash does.
"""

from pathlib import Path
import re
import shutil
import subprocess
import sys

import pytest

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]
FUZZ = ROOT / "tests/fuzz"


@pytest.fixture(scope="module")
def adt_lib(tmp_path_factory):
    tmp = tmp_path_factory.mktemp("fuzz-regress")
    lib = tmp / "libadt_host.a"
    out = subprocess.run(
        ["rustc", "--edition", "2021", "--crate-type", "staticlib", "--crate-name", "adt_host",
         "-C", "opt-level=1", "-C", "debug-assertions=on", "-C", "overflow-checks=on",
         "-C", "panic=abort", "--print", "native-static-libs", "-o", str(lib),
         str(FUZZ / "adt_host.rs")],
        check=True, capture_output=True, text=True)
    native = re.search(r"native-static-libs: (.*)", out.stderr)
    return tmp, lib, native.group(1).split() if native else []


def build(adt_lib, target, sources, incs=()):
    tmp, lib, native = adt_lib
    binary = tmp / target
    # As tests/fuzz/build.sh: aura allows unaligned loads and libfdt reads bytewise
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-D_GNU_SOURCE", "-O1", "-g", "-Wall", "-Wno-unused-function",
         "-fsanitize=address,undefined", "-fno-sanitize=alignment", "-fno-omit-frame-pointer",
         "-fno-sanitize-recover=undefined", *incs, "-I", str(ROOT / "src"), "-I", str(ROOT),
         *map(str, sources), str(FUZZ / "replay_main.c"), str(lib), *native, "-o", str(binary)],
        check=True)
    return binary


# Each input takes milliseconds; a replay that is still running after this long is a hang
REPLAY_TIMEOUT_S = 300


def replay(binary, inputs):
    assert inputs
    out = subprocess.run([str(binary), *map(str, inputs)], capture_output=True, text=True,
                         timeout=REPLAY_TIMEOUT_S,
                         env={"ASAN_OPTIONS": "detect_leaks=0", "UBSAN_OPTIONS": "print_stacktrace=1"})
    assert out.returncode == 0, out.stdout[-4000:] + out.stderr[-4000:]
    assert out.stdout.count("ok ") == len(inputs)


def test_adt_fuzz_regress(adt_lib, tmp_path):
    seeds = tmp_path / "seeds"
    subprocess.run([sys.executable, "-I", str(FUZZ / "gen_seeds.py"), "adt", str(seeds)],
                   check=True)
    binary = build(adt_lib, "adt-replay", [FUZZ / "adt_fuzz.c"])
    replay(binary, sorted(seeds.iterdir()) + sorted((FUZZ / "regress/adt").glob("*")))


def test_dt_fuzz_regress(adt_lib, tmp_path):
    seeds = tmp_path / "seeds"
    subprocess.run([sys.executable, "-I", str(FUZZ / "gen_seeds.py"), "dt", str(seeds)], check=True)
    # The code under test sits alone, so that its #include "utils.h" finds tests/fuzz/host's
    src = tmp_path / "src"
    src.mkdir()
    subprocess.run([sys.executable, "-I", str(FUZZ / "src_copy.py"), str(src / "src_copy.c")],
                   check=True)
    for name in ("kboot_audio.c", "devicetree.c"):
        shutil.copy(ROOT / "src" / name, src / name)
    libfdt = [ROOT / "src/libfdt" / name for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_strerror.c", "fdt_check.c",
        "fdt_empty_tree.c", "fdt_addresses.c")]
    binary = build(adt_lib, "dt-replay",
                   [FUZZ / "dt_fuzz.c", src / "src_copy.c", src / "devicetree.c",
                    src / "kboot_audio.c", *libfdt],
                   incs=["-I", str(FUZZ / "host")])
    replay(binary, sorted(seeds.iterdir()) + sorted((FUZZ / "regress/dt").glob("*")))


def test_pmgr_fuzz_regress(adt_lib, tmp_path):
    seeds = tmp_path / "seeds"
    subprocess.run([sys.executable, "-I", str(FUZZ / "gen_seeds.py"), "pmgr", str(seeds)],
                   check=True)
    # pmgr.c includes ../build/build_cfg.h, which only a firmware build generates
    src = tmp_path / "src"
    src.mkdir()
    (tmp_path / "build").mkdir()
    (tmp_path / "build/build_cfg.h").write_text("")
    for name in ("pmgr.c", "sptm_virt_s2r_soc.c"):
        shutil.copy(ROOT / "src" / name, src / name)
    binary = build(adt_lib, "pmgr-replay",
                   [FUZZ / "pmgr_fuzz.c", src / "pmgr.c", src / "sptm_virt_s2r_soc.c"],
                   incs=["-I", str(FUZZ / "host")])
    replay(binary, sorted(seeds.iterdir()) + sorted((FUZZ / "regress/pmgr").glob("*")))
