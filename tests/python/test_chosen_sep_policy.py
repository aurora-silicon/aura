"""Verify APFS UUID forwarding and the inherited SEP entropy policy."""

from pathlib import Path
import subprocess


REPO = Path(__file__).resolve().parents[2]


def test_uuid_forwarding_and_sep_entropy_policy(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    chosen = source.index("static int dt_set_chosen(void)")
    start = source.index("    if (adt) {", chosen)
    end = source.index("static int dt_set_speaker_safety(", start)
    tail = source[start:end]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint8_t u8;
#define T8103 0x8103
#define T6000 0x6000
#define T6001 0x6001
#define T6002 0x6002
#define T8140 0x8140
#define T8132 0x8132
#define T6040 0x6040
#define T6041 0x6041
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
static struct { bool apple_sysregs_unlocked; } features = {true}, *cpu_features = &features;
#define T8152 0x8152
static unsigned char blob[4096];
static void *dt = blob, *adt = blob;
static int chip_id, uuid_len, sep_calls, adt_calls, sep_error, adt_error;
static const void *uuid;
static bool fallback;
static int adt_path_offset(void *tree, const char *path)
{ return !strcmp(path, "/chosen") ? 1 : -1; }
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    if ((!strcmp(name, "apfs-preboot-uuid") && !fallback) ||
        (!strcmp(name, "boot-uuid") && fallback)) {
        *len = uuid_len;
        return uuid;
    }
    return NULL;
}
static int dt_set_rng_seed_sep(int node) { sep_calls++; return sep_error; }
static int dt_set_rng_seed_adt(int node) { adt_calls++; return adt_error; }
static int chosen_tail(void)
{
    int node = fdt_path_offset(dt, "/chosen");
''' + tail + r'''
static void reset(void)
{
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
    uuid = NULL; uuid_len = 0;
    sep_calls = adt_calls = sep_error = adt_error = 0;
    fallback = false;
}
static void expect_uuid(const char *expected)
{
    int len;
    const char *value = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"),
                                    "apfs-preboot-uuid", &len);
    assert(value && len == 37 && !strcmp(value, expected));
}
int main(void)
{
    int chips[] = {T8103, T6000, T6001, T6002, 0x8112, 0x6020, 0x8122, 0x8140, 0x8152};
    for (unsigned i = 0; i < sizeof(chips) / sizeof(chips[0]); i++) {
        chip_id = chips[i];
        reset();
        assert(chosen_tail() == 0);
        assert(sep_calls == (i < 4 ? 1 : 0));
        assert(adt_calls == (i < 4 ? 0 : 1));
        reset(); sep_error = -1;
        assert(chosen_tail() == 0 && adt_calls == 1);
        reset(); sep_error = adt_error = -1;
        assert(chosen_tail() == -1);
    }

    static const u8 binary[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    static const char expected[] = "00010203-0405-0607-0809-0a0b0c0d0e0f";
    reset(); uuid = binary; uuid_len = sizeof(binary);
    assert(chosen_tail() == 0);
    expect_uuid(expected);
    for (int length = 36; length <= 37; length++) {
        reset(); uuid = expected; uuid_len = length;
        assert(chosen_tail() == 0);
        expect_uuid(expected);
    }
    reset(); uuid = expected; uuid_len = 36; fallback = true;
    assert(chosen_tail() == 0);
    expect_uuid(expected);
    reset(); uuid = expected; uuid_len = 2;
    assert(chosen_tail() == 0);
    assert(!fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "apfs-preboot-uuid", NULL));

    /* With locked system registers, only chips without their own idle policy get idle=nop. */
    features.apple_sysregs_unlocked = false;
    int locked[] = {0x8132, 0x6040, 0x6041, 0x8140, 0x8142, 0x8152};
    for (unsigned i = 0; i < sizeof(locked) / sizeof(locked[0]); i++) {
        chip_id = locked[i];
        reset();
        int chosen = fdt_path_offset(dt, "/chosen");
        assert(!fdt_setprop_string(dt, chosen, "bootargs", "console=tty0"));
        assert(chosen_tail() == 0);
        const char *args = fdt_getprop(dt, fdt_path_offset(dt, "/chosen"), "bootargs", NULL);
        /* T8152 keeps its WFI state, so it needs only arm64.nowfxt. */
        const char *want = i < 4 ? "console=tty0"
                           : chip_id == 0x8152 ? "console=tty0 arm64.nowfxt"
                                               : "console=tty0 idle=nop arm64.nowfxt";
        assert(args && !strcmp(args, want));
    }
    features.apple_sysregs_unlocked = true;
    return 0;
}
'''
    libfdt = REPO / "src/libfdt"
    sources = [libfdt / name for name in (
        "fdt.c", "fdt_addresses.c", "fdt_check.c", "fdt_empty_tree.c",
        "fdt_ro.c", "fdt_rw.c", "fdt_strerror.c", "fdt_sw.c", "fdt_wip.c",
    )]
    binary = tmp_path / "chosen-sep-policy"
    subprocess.run(
        ["cc", "-D_GNU_SOURCE", "-std=c11", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-sizeof-pointer-memaccess", "-I", str(libfdt), "-x", "c", "-",
         *map(str, sources), "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
