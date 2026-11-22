# Host fuzz targets

libFuzzer targets for the code in aura that parses what it is handed: the ADT from iBoot and the
device tree from the payload. They build and run on a Linux or macOS host with clang; nothing
here is part of the aura image.

| Target | Input | Code under test |
| --- | --- | --- |
| `adt-fuzz` (`adt_fuzz.c`) | one ADT image | `rust/src/adt.rs` through its C API and the `ADT_FOREACH_*` loops of `src/adt.h`: full tree walk, every property, path lookups (the node's own path and common ones), `adt_get_reg()`, compatible checks, `adt_setprop()` in place |
| `dt-fuzz` (`dt_fuzz.c`) | u32 ADT length, flags byte, ADT, DTB | `load_fdt()` (`src/payload.c`), `src/devicetree.c`, kboot.c's SMBIOS node and `dt_disable_missing_devs()`, `src/kboot_audio.c`, libfdt, with the ADT reader |
| `pmgr-fuzz` (`pmgr_fuzz.c`) | flags byte, 16 register seeds, ADT | `src/pmgr.c` (`pmgr_init()` on `/arm-io/pmgr`, then power requests for every `/arm-io` node's clock-gates) and `src/sptm_virt_s2r_soc.c`, with the ADT reader and simulated power state registers |

Besides sanitizer reports and Rust panics, adt-fuzz checks that what the reader hands out lies
inside the image, and dt-fuzz that the device tree builders leave a tree `fdt_check_full()`
accepts.

## Build and run

    sh tests/fuzz/build.sh <out>
    python3 tests/fuzz/gen_seeds.py adt <out>/seeds-adt
    python3 tests/fuzz/gen_seeds.py dt <out>/seeds-dt
    python3 tests/fuzz/gen_seeds.py pmgr <out>/seeds-pmgr
    mkdir -p <out>/corpus-adt <out>/corpus-dt <out>/corpus-pmgr
    ASAN_OPTIONS=detect_leaks=0 <out>/adt-fuzz -max_len=16384 -timeout=10 <out>/corpus-adt <out>/seeds-adt
    ASAN_OPTIONS=detect_leaks=0 <out>/dt-fuzz -max_len=32768 -timeout=10 <out>/corpus-dt <out>/seeds-dt
    ASAN_OPTIONS=detect_leaks=0 <out>/pmgr-fuzz -max_len=16384 -timeout=10 <out>/corpus-pmgr <out>/seeds-pmgr

`build.sh` needs clang with libFuzzer and a Rust toolchain whose LLVM version matches clang's.
The ADT reader is built with ASan, sanitizer coverage, overflow checks and `panic=abort`
(`RUSTC_BOOTSTRAP=1` lets a stable rustc take the sanitizer flags). The C code is built with
ASan and UBSan without the alignment check: aura runs on arm64 with unaligned loads allowed and
libfdt reads the tree byte by byte.

`gen_seeds.py` writes synthetic seeds that use the node and property names aura looks up; every
value in them is made up. A Mac's own ADT (see `tests/kboot_audio_adt.py ioreg`) and kernel DTB
make good extra seeds; keep them out of the tree.

`dt_fuzz.c` takes `load_fdt()` and the kboot.c functions from copies `src_copy.py` makes of the
current sources, so the code under test is always the code in `src/`.

## Regressions

`regress/<target>/` holds inputs that once broke a target. `tests/python/test_fuzz_regress.py`
builds the targets without libFuzzer (`replay_main.c`), with the host compiler, ASan and UBSan,
and runs them over the synthetic seeds and every regression input. Add the reproducer of each fix
there.
