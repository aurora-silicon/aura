# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the production ISP init path with synthetic ADT segments."""

from pathlib import Path
import subprocess

from host_compiler import host_cc


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define T8103 0x8103
#define T8112 0x8112
#define T8122 0x8122
#define T6000 0x6000
#define T6002 0x6002
#define T6020 0x6020
#define T6022 0x6022
#define T6030 0x6030
#define T6031 0x6031
#define T6034 0x6034
#define T6041 0x6041
#define T6050 0x6050
#define T8140 0x8140
#define T8142 0x8142
#define V12_3 1
#define V12_4 2
#define V13_5 3
#define V13_6_1 4
#define V14_7 5
#define V26_6 7
#define V26_6_2 6
#define V27_0 8
#define PMGR_PS_ACTIVE 15
#define PMGR_PS_PWRGATE 0
#define SZ_16K 0x4000
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define GENMASK(h, l) (((~0ULL) << (l)) & (~0ULL >> (63 - (h))))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
/* Stub: the T6041 ISP handoff isn't exercised here. */
__attribute__((unused)) static int dt_set_isp_t6041(int node) { (void)node; return 0; }

struct adt_segment_ranges {
    u64 phys, iova, remap;
    u32 size, unk;
} __attribute__((packed));
struct { int version; const char *string; } os_firmware;
struct { u64 mem_size; } cur_boot_args;
u32 chip_id;
void *adt;
static struct adt_segment_ranges segments[2];
static u32 segments_len;
static bool segments_present, allocation_fails, dapf_fails, adt_power_fails;
static u32 revision;
static bool invalid_aperture;
static u64 read_address, expected_heap_size;
static int enabled, disabled, active, gated, dapf_calls, allocations, power_fail_at;
static u64 active_addrs[8], gated_addrs[8];
static u8 global_mode[4], initial_mode[4], adt_mode, initial_adt_mode;
static u64 mode_addrs[16];
static u8 mode_values[16];
static int mode_calls;
struct pmgr_saved_mode { uintptr_t addr; u8 target; };
struct pmgr_saved_modes { struct pmgr_saved_mode modes[64]; size_t count; };

u64 isp_iova_base(void);
int adt_path_offset_trace(const void *tree, const char *path, int *trace)
{
    (void)tree;
    trace[0] = 1;
    return strstr(path, "dart-") ? 2 : 1;
}
int pmgr_adt_power_enable(const char *path)
{
    (void)path;
    enabled++;
    return adt_power_fails ? -1 : 0;
}
int pmgr_adt_power_disable(const char *path)
{
    (void)path;
    disabled++;
    return 0;
}
int pmgr_adt_power_enable_traced(const char *path)
{
    return pmgr_adt_power_enable(path);
}
int pmgr_adt_power_enable_traced_saved(const char *path, struct pmgr_saved_modes *saved)
{
    saved->count = 1;
    saved->modes[0].target = adt_mode;
    adt_mode = PMGR_PS_ACTIVE;
    int ret = pmgr_adt_power_enable_traced(path);
    if (ret) {
        adt_mode = saved->modes[0].target;
        saved->count = 0;
    }
    return ret;
}
void pmgr_restore_modes(struct pmgr_saved_modes *saved)
{
    assert(saved->count == 1);
    adt_mode = saved->modes[0].target;
    saved->count = 0;
    disabled++;
}
int adt_get_reg(const void *tree, int *path, const char *name, int index, u64 *addr, u64 *size)
{
    (void)tree; (void)path; (void)name;
    static const u64 bases[] = {0x320000000ULL, 0x300700000ULL, 0x300724000ULL};
    static const u64 sizes[] = {0x3fe8000, 0x18000, 0x4000};
    assert(index >= 0 && index < 3);
    *addr = bases[index];
    if (size)
        *size = invalid_aperture && index == 0 ? 4 : sizes[index];
    return 0;
}
int pmgr_set_mode(u64 addr, u8 mode)
{
    assert(mode_calls < 16);
    mode_addrs[mode_calls] = addr;
    mode_values[mode_calls++] = mode;
    if (mode == PMGR_PS_ACTIVE) {
        active_addrs[active++] = addr;
        if (active == power_fail_at)
            return -1;
    } else {
        gated_addrs[gated++] = addr;
    }
    if (addr >= 0x300704000ULL && addr <= 0x300704018ULL)
        global_mode[(addr - 0x300704000ULL) / 8] = mode;
    return 0;
}
u32 read32(u64 addr)
{
    read_address = addr;
    if (addr >= 0x300704000ULL && addr <= 0x300704018ULL) {
        u8 mode = global_mode[(addr - 0x300704000ULL) / 8];
        return (mode << 4) | mode;
    }
    return revision;
}
const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node; (void)name;
    *len = segments_len;
    return segments_present ? segments : NULL;
}
int dapf_init(const char *path, int index)
{
    assert(strcmp(path, "/arm-io/dart-isp") == 0);
    assert(index == 5);
    dapf_calls++;
    return dapf_fails ? -1 : 0;
}
u64 top_of_memory_alloc(size_t size)
{
    assert(size == expected_heap_size || chip_id != T8140);
    allocations++;
    return allocation_fails ? 0 : 0x100000000ULL;
}
"""


TESTS = r"""
static void reset(void)
{
    memset(segments, 0, sizeof(segments));
    segments[0].iova = 0;
    segments[0].size = 0xe94000;
    segments[1].iova = 0xe94000;
    segments[1].size = 0x1358000;
    segments_len = sizeof(segments);
    segments_present = true;
    chip_id = T8140;
    os_firmware.version = V26_6_2;
    os_firmware.string = "26.6.2";
    cur_boot_args.mem_size = 0x1000000;
    expected_heap_size = ISP_T8140_HEAP_TOP - 0x21ec000;
    revision = 0x110000;
    read_address = 0;
    allocation_fails = dapf_fails = adt_power_fails = false;
    invalid_aperture = false;
    enabled = disabled = active = gated = dapf_calls = allocations = power_fail_at = 0;
    mode_calls = 0;
    memset(global_mode, 0, sizeof(global_mode));
    memset(initial_mode, 0, sizeof(initial_mode));
    adt_mode = initial_adt_mode = PMGR_PS_PWRGATE;
}
static void success(void)
{
    u64 phys, iova, size;
    assert(isp_init() == 0);
    assert(read_address == 0x321800000ULL);
    assert(isp_get_heap(&phys, &iova, &size) == 0);
    assert(phys == 0x100000000ULL);
    assert(iova == 0x21ec000 && size == expected_heap_size);
    if (ISP_T8140_HEAP_TOP == 0x2200000)
        assert(size == 0x14000);
    assert(isp_iova_base() == 0);
    assert(enabled == 1 && disabled == 0 && active == 4 && gated == 0);
    assert(active_addrs[0] == 0x300704000ULL && active_addrs[1] == 0x300704008ULL);
    assert(active_addrs[2] == 0x300704010ULL && active_addrs[3] == 0x300704018ULL);
    assert(dapf_calls == 1 && allocations == 1);
}
static void failure(void)
{
    u64 phys, iova, size;
    assert(isp_init() != 0);
    assert(isp_get_heap(&phys, &iova, &size) != 0);
    assert(enabled == 1 && disabled == (adt_power_fails ? 0 : 1));
    assert(adt_mode == initial_adt_mode);
    assert(memcmp(global_mode, initial_mode, sizeof(global_mode)) == 0);
    if (!adt_power_fails) {
        int attempted = power_fail_at ? power_fail_at : 4;
        assert(mode_calls == 2 * attempted);
        for (int i = 0; i < attempted; i++) {
            assert(mode_addrs[attempted + i] == mode_addrs[attempted - i - 1]);
            assert(mode_values[attempted + i] == initial_mode[attempted - i - 1]);
        }
    }
}
int main(void)
{
    reset(); invalid_aperture = true;
    assert(isp_init() != 0);
    assert(!isp_initialized);
    assert(enabled == 1 && disabled == 1 && active == 0 && allocations == 0);
    assert(adt_mode == initial_adt_mode);
    reset(); success();
    reset(); os_firmware.version = V14_7; failure();
    reset(); revision = 0x110001; failure();
    reset(); segments_present = false; failure();
    reset(); segments_len = sizeof(segments[0]) - 1; failure();
    reset(); segments_len = sizeof(segments) - 1; failure();
    reset(); segments[1].iova = UINT64_MAX - 1; segments[1].size = 4; failure();
    reset(); segments[1].iova = UINT64_MAX - 0x1000; segments[1].size = 1; failure();
    reset(); segments[1].size = 0x1354000; failure();
    reset(); segments[1].size = 0x1357fff; success();
    reset();
    struct adt_segment_ranges first = segments[0];
    segments[0] = segments[1];
    segments[1] = first;
    success();
    reset(); allocation_fails = true; failure();
    reset(); cur_boot_args.mem_size = expected_heap_size; failure();
    reset(); dapf_fails = true; failure();
    reset(); adt_power_fails = true; failure();
    reset(); power_fail_at = 3; failure();
    reset(); initial_mode[1] = global_mode[1] = PMGR_PS_ACTIVE;
    initial_adt_mode = adt_mode = PMGR_PS_ACTIVE;
    power_fail_at = 3; failure();

    reset(); chip_id = T6000; revision = 0xb3091;
    os_firmware.version = V13_5; os_firmware.string = "13.5";
    segments[1].iova = 0xe00000; segments[1].size = 0x40000;
    assert(isp_init() == 0);
    u64 phys, iova, size;
    assert(isp_get_heap(&phys, &iova, &size) == 0);
    assert(iova == 0xe94000 && size == 0x6c000);
    assert(disabled == 1 && gated == 1 && dapf_calls == 0);

    /* 27.0 splits the T8142 firmware differently but ends where 26.6 does. */
    reset(); chip_id = T8142;
    os_firmware.version = V27_0; os_firmware.string = "27.0";
    segments[0].size = 0xd68000;
    segments[1].iova = 0xd68000; segments[1].size = 0x984000;
    assert(isp_init() == 0);
    assert(isp_get_heap(&phys, &iova, &size) == 0);
    assert(iova == 0x16ec000 && size == 0x114000);
    assert(dapf_calls == 1 && allocations == 1);
    return 0;
}
"""


def test_isp_init_synthetic_adt(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/isp.c").read_text()
    source = source[source.index("#define ISP_ASC_VERSION"):]
    harness = HARNESS + source + TESTS
    program = tmp_path / "isp-heap"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-format",
         "-fsanitize=address,undefined", "-x", "c", "-", "-o", str(program)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(program)], check=True)


def test_t8140_heap_top_must_exceed_aligned_end(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/isp.c").read_text()
    source = source[source.index("#define ISP_ASC_VERSION"):]
    helpers = TESTS[:TESTS.index("int main(void)")]
    definition = next(line for line in source.splitlines()
                      if line.startswith("#define ISP_T8140_HEAP_TOP "))
    for top in (0x21EC000, 0x21E8000):
        variant = source.replace(definition, f"#define ISP_T8140_HEAP_TOP 0x{top:x}")
        assert variant != source
        program = tmp_path / f"isp-invalid-top-{top:x}"
        subprocess.run(
            [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-format",
             "-Wno-unused-function",
             "-fsanitize=address,undefined", "-x", "c", "-", "-o", str(program)],
            input=HARNESS + variant + helpers + "int main(void) { reset(); failure(); return 0; }",
            text=True, check=True,
        )
        subprocess.run([str(program)], check=True)


def test_isp_fdt_fails_closed_and_exports_heap(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    start = source.index("static int dt_set_isp_fwdata(void)")
    end = source.index("\n}\n", start) + 3
    helper = source[start:end]
    harness = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define T6041 0x6041
static u32 chip_id;
static int dt_set_isp_t6041(int node) { (void)node; return 0; }
#define V13_6_1 1
#define V13_5 2
#define bail(...) do { fprintf(stderr, __VA_ARGS__); return -1; } while (0)
struct fw_version_info { int version; };
struct fw_version_info os_firmware = {3}, fw_versions[4];
void *dt, *adt;
static bool initialized, was_disabled;
static int reserved, mapped, fw_properties;
static u64 exported_phys, exported_iova, exported_size;
int fdt_path_offset(const void *tree, const char *path)
{ (void)tree; assert(strcmp(path, "isp") == 0); return 1; }
int firmware_set_fdt(void *tree, int node, const char *name,
                     const struct fw_version_info *version)
{ (void)tree; (void)node; (void)name; (void)version; fw_properties++; return 0; }
int isp_get_heap(u64 *phys, u64 *iova, u64 *size)
{
    if (!initialized) return -1;
    *phys = 0x100000000ULL; *iova = 0x21ec000; *size = 0x14000;
    return 0;
}
const void *fdt_getprop(const void *tree, int node, const char *name, int *len)
{ (void)tree; (void)node; (void)name; (void)len; return "okay"; }
int fdt_setprop_string(void *tree, int node, const char *name, const char *value)
{
    (void)tree; (void)node; assert(strcmp(name, "status") == 0);
    assert(strcmp(value, "disabled") == 0); was_disabled = true; return 0;
}
int adt_path_offset(const void *tree, const char *path)
{ (void)tree; (void)path; return 1; }
u32 fdt_get_phandle(const void *tree, int node)
{ (void)tree; (void)node; return 9; }
int fdt_generate_phandle(void *tree, u32 *phandle)
{ (void)tree; *phandle = 9; return 0; }
int fdt_setprop_u32(void *tree, int node, const char *name, u32 value)
{ (void)tree; (void)node; (void)name; (void)value; return 0; }
int dt_get_or_add_reserved_mem(const char *name, const char *compat, bool no_map,
                               u64 phys, u64 size)
{
    assert(strcmp(name, "isp-heap") == 0);
    assert(strcmp(compat, "apple,asc-mem") == 0 && no_map);
    reserved++; exported_phys = phys; exported_size = size; return 2;
}
int dt_device_set_reserved_mem(int node, const char *name, u32 phandle, u64 iova, u64 size)
{
    assert(node == 2 && strcmp(name, "isp-heap") == 0 && phandle == 9);
    mapped++; exported_iova = iova; assert(size == exported_size); return 0;
}
""" + helper + r"""
int main(void)
{
    assert(dt_set_isp_fwdata() == 0);
    assert(was_disabled && !reserved && !mapped && fw_properties == 2);
    initialized = true; was_disabled = false;
    assert(dt_set_isp_fwdata() == 0);
    assert(!was_disabled && reserved == 1 && mapped == 1);
    assert(exported_phys == 0x100000000ULL);
    assert(exported_iova == 0x21ec000 && exported_size == 0x14000);
    return 0;
}
"""
    program = tmp_path / "isp-fdt"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-",
         "-o", str(program)], input=harness, text=True, check=True,
    )
    subprocess.run([str(program)], check=True)


def test_isp_firmware_revision_admission_matrix(tmp_path):
    """The combined switch must not broaden the source contributions' admission."""
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/isp.c").read_text()
    source = source[source.index("#define ISP_ASC_VERSION"):]
    helpers = TESTS[:TESTS.index("int main(void)")]
    matrix = r'''
int main(void)
{
    const u32 chips[] = {T6030, T6031, T6034, T8122};
    const int firmware[] = {V14_7, V26_6_2};
    for (unsigned int i = 0; i < ARRAY_SIZE(chips); i++) {
        for (unsigned int j = 0; j < ARRAY_SIZE(firmware); j++) {
            reset();
            chip_id = chips[i];
            revision = chip_id == T8122 ? ISP_VER_T8122 : ISP_VER_T603X;
            os_firmware.version = firmware[j];
            os_firmware.string = j ? "26.6.2" : "14.7";
            segments[0].iova = 0; segments[0].size = 0x9d4000;
            segments[1].iova = 0x9d4000; segments[1].size = 0x41c000;
            int ret = isp_init();
            u64 phys, iova, size;
            if (firmware[j] == V26_6_2 && chip_id != T8122) {
                assert(ret != 0);
                assert(isp_get_heap(&phys, &iova, &size) != 0);
                assert(!isp_initialized && !allocations);
            } else {
                assert(ret == 0);
                assert(isp_get_heap(&phys, &iova, &size) == 0);
                assert(phys == 0x100000000ULL);
                assert(iova == (0xdf0000 | isp_iova_base()));
                assert(size == 0x210000 && allocations == 1);
            }
            assert(enabled == 1 && disabled == 1 && active == 1 && gated == 1);
            assert(dapf_calls == 0);
        }
    }
    /* Keep the native T8140 sequence and its one-segment policy untouched. */
    reset(); success();
    reset();
    segments[0].iova = 0; segments[0].size = ISP_T8140_SEG_END;
    segments_len = sizeof(segments[0]);
    success();
    return 0;
}
'''
    binary = tmp_path / "isp-firmware-admission"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-format",
         "-Wno-unused-function", "-fsanitize=address,undefined",
         "-x", "c", "-", "-o", str(binary)],
        input=HARNESS + source + helpers + matrix, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
