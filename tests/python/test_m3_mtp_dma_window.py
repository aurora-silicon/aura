# SPDX-License-Identifier: GPL-2.0-only
"""Exercise production dt_set_mtp_dma_window() across capability-matched and refusal cases."""

from pathlib import Path
import subprocess

from host_compiler import host_cc


def test_m3_mtp_dma_window(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    text = (repo / "src/kboot.c").read_text()

    assert "if (dt_set_mtp_dma_window())" in text

    start = text.index("static int dt_set_mtp_dma_window(void)")
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    production = text[start:end]

    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"

typedef uint64_t u64;
typedef uint32_t u32;

#define SZ_16K 0x4000ULL
#define DART_T8110_PARAMS3 0x08
#define DART_T8110_PARAMS3_VA_WIDTH 0x003f0000u
#define FIELD_GET(mask, val) (((val) & (mask)) >> __builtin_ctz(mask))
#define bail(...) do { printf(__VA_ARGS__); return -1; } while (0)
#define ADT_GETPROP(a, n, p, v) mock_adt_getprop((n), (p), (v), sizeof(*(v)))

enum {
    T6020 = 0x6020,
    T8122 = 0x8122,
    T6030 = 0x6030,
    T6031 = 0x6031,
    T6034 = 0x6034,
    T8132 = 0x8132,
};

static u32 chip_id;
static void *dt, *adt = (void *)1;

static int mock_asc_off = 1;
static int mock_adart_off = 2;
static bool mock_has_vm_base = true, mock_has_vm_size = true;
static u64 mock_vm_base, mock_vm_size;
static int mock_adt_reg_rc = 0;
static u64 mock_adt_mmio = 0x39b000000ULL;
static int mock_dart_mmio_rc = 0;
static u64 mock_dart_mmio = 0x39b000000ULL;
static u64 mock_dart_mmio_size = 0x4000;
static u32 mock_va_bits = 42;
static int mock_read32_calls;
static u64 mock_last_read_addr;

static int adt_path_offset(void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/arm-io/mtp"))
        return mock_asc_off;
    return -1;
}

static int adt_path_offset_trace(void *a, const char *path, int *trace)
{
    (void)a;
    if (!strcmp(path, "/arm-io/dart-mtp")) {
        trace[0] = mock_adart_off;
        return mock_adart_off;
    }
    return -1;
}

static int mock_adt_getprop(int node, const char *prop, void *out, size_t len)
{
    if (node != mock_adart_off || len != sizeof(u64))
        return -1;
    if (!strcmp(prop, "vm-base") && mock_has_vm_base) {
        memcpy(out, &mock_vm_base, sizeof(u64));
        return sizeof(u64);
    }
    if (!strcmp(prop, "vm-size") && mock_has_vm_size) {
        memcpy(out, &mock_vm_size, sizeof(u64));
        return sizeof(u64);
    }
    return -1;
}

static int adt_get_reg(void *a, int *trace, const char *prop, int idx, u64 *base, u64 *size)
{
    (void)a;
    (void)trace;
    (void)prop;
    (void)idx;
    if (mock_adt_reg_rc)
        return mock_adt_reg_rc;
    if (base)
        *base = mock_adt_mmio;
    if (size)
        *size = 0x4000;
    return 0;
}

static int dt_device_dart_mmio(int fdt_node, int adt_node, u64 *addr, u64 *size)
{
    (void)fdt_node;
    (void)adt_node;
    if (mock_dart_mmio_rc)
        return mock_dart_mmio_rc;
    *addr = mock_dart_mmio;
    *size = mock_dart_mmio_size;
    return 0;
}

static int dt_get_iommu_node(int node, u32 num)
{
    int len = 0;
    const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &len);
    if (!iommus || len < (int)(8 * (num + 1)))
        return -FDT_ERR_NOTFOUND;
    return fdt_node_offset_by_phandle(dt, fdt32_to_cpu(iommus[num * 2]));
}

static u32 read32(u64 addr)
{
    mock_read32_calls++;
    mock_last_read_addr = addr;
    return mock_va_bits << 16;
}
''' + production + r'''

static unsigned char blob[8192];

static void setup(const char *status, const char *dart_compat, bool add_mtp, bool pre_existing)
{
    mock_asc_off = 1;
    mock_adart_off = 2;
    mock_has_vm_base = true;
    mock_has_vm_size = true;
    mock_vm_base = 0x10000004000ULL;
    mock_vm_size = 0xffff0000ULL;
    mock_adt_reg_rc = 0;
    mock_adt_mmio = 0x39b000000ULL;
    mock_dart_mmio_rc = 0;
    mock_dart_mmio = 0x39b000000ULL;
    mock_dart_mmio_size = 0x4000;
    mock_va_bits = 42;
    mock_read32_calls = 0;
    mock_last_read_addr = 0;

    memset(blob, 0, sizeof(blob));
    dt = blob;
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    int soc = fdt_add_subnode(dt, 0, "soc");
    assert(soc >= 0);

    int dart = fdt_add_subnode(dt, soc, "iommu@39b000000");
    assert(dart >= 0);
    if (dart_compat)
        assert(fdt_setprop_string(dt, dart, "compatible", dart_compat) == 0);
    assert(fdt_setprop_u32(dt, dart, "phandle", 7) == 0);
    if (pre_existing) {
        fdt64_t existing[] = {cpu_to_fdt64(0x20000000ULL), cpu_to_fdt64(0x10000000ULL)};
        assert(fdt_setprop(dt, dart, "apple,dma-range", existing, sizeof(existing)) == 0);
    }

    if (add_mtp) {
        soc = fdt_path_offset(dt, "/soc");
        int mtp = fdt_add_subnode(dt, soc, "mtp@39bc00000");
        assert(mtp >= 0);
        assert(fdt_setprop_string(dt, mtp, "compatible", "apple,mtp") == 0);
        if (status)
            assert(fdt_setprop_string(dt, mtp, "status", status) == 0);
        fdt32_t iommus[] = {cpu_to_fdt32(7), cpu_to_fdt32(0)};
        assert(fdt_setprop(dt, mtp, "iommus", iommus, sizeof(iommus)) == 0);
    }
}

static bool get_dma_range(u64 *base, u64 *size)
{
    int dart = fdt_path_offset(dt, "/soc/iommu@39b000000");
    int len = 0;
    const fdt64_t *prop = fdt_getprop(dt, dart, "apple,dma-range", &len);
    if (!prop)
        return false;
    assert(len == 16);
    *base = fdt64_to_cpu(prop[0]);
    *size = fdt64_to_cpu(prop[1]);
    return true;
}

int main(void)
{
    u64 base = 0, size = 0;

    /* Positive cases across valid status forms and unlisted chip IDs with matching capabilities. */
    const u32 chips[] = {T8122, T6030, T6031, T6034, T8132, 0x9999};
    const char *statuses[] = {NULL, "okay", "ok", "okay", "okay", "okay"};
    for (unsigned i = 0; i < 6; i++) {
        chip_id = chips[i];
        setup(statuses[i], "apple,t8110-dart", true, false);
        assert(dt_set_mtp_dma_window() == 0);
        assert(get_dma_range(&base, &size));
        assert(base == 0x10000004000ULL && size == 0xffff0000ULL);
        assert(mock_read32_calls == 1);
        assert(mock_last_read_addr == mock_dart_mmio + DART_T8110_PARAMS3);
    }

    /* Non-t8110 DART provider is skipped before MMIO access. */
    chip_id = T6030;
    setup("okay", "apple,t6000-dart", true, false);
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    /* Missing or disabled MTP node is skipped. */
    setup(NULL, "apple,t8110-dart", false, false);
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("disabled", "apple,t8110-dart", true, false);
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    /* Explicit apple,dma-range is preserved. */
    setup("okay", "apple,t8110-dart", true, true);
    assert(dt_set_mtp_dma_window() == 0);
    assert(get_dma_range(&base, &size));
    assert(base == 0x20000000ULL && size == 0x10000000ULL && mock_read32_calls == 0);

    /* Missing ADT nodes are skipped. */
    setup("okay", "apple,t8110-dart", true, false);
    mock_asc_off = -1;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("okay", "apple,t8110-dart", true, false);
    mock_adart_off = -1;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    /* Zero size or non-16KiB-aligned base/size is rejected before MMIO access. */
    setup("okay", "apple,t8110-dart", true, false);
    mock_vm_size = 0;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("okay", "apple,t8110-dart", true, false);
    mock_vm_base += 0x1000;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("okay", "apple,t8110-dart", true, false);
    mock_vm_size += 0x1000;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    /* Provider MMIO mismatch, powered-down DART, or short MMIO window skips register read. */
    setup("okay", "apple,t8110-dart", true, false);
    mock_dart_mmio = 0x39b004000ULL;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("okay", "apple,t8110-dart", true, false);
    mock_dart_mmio_rc = -1;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    setup("okay", "apple,t8110-dart", true, false);
    mock_dart_mmio_size = DART_T8110_PARAMS3;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 0);

    /* Out-of-range VA width or window past 1<<bits is rejected. */
    setup("okay", "apple,t8110-dart", true, false);
    mock_va_bits = 31;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 1);

    setup("okay", "apple,t8110-dart", true, false);
    mock_va_bits = 45;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 1);

    setup("okay", "apple,t8110-dart", true, false);
    mock_va_bits = 40;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 1);

    setup("okay", "apple,t8110-dart", true, false);
    mock_va_bits = 41;
    mock_vm_base = 0x1ffff000000ULL;
    mock_vm_size = 0x20000000ULL;
    assert(dt_set_mtp_dma_window() == 0);
    assert(!get_dma_range(&base, &size) && mock_read32_calls == 1);

    return 0;
}
'''

    source = tmp_path / "mtp-dma-window.c"
    source.write_text(harness)
    binary = tmp_path / "mtp-dma-window"
    libfdt = repo / "src/libfdt"
    sources = [
        libfdt / (n + ".c")
        for n in ("fdt", "fdt_ro", "fdt_rw", "fdt_sw", "fdt_wip", "fdt_empty_tree", "fdt_strerror")
    ]
    subprocess.run(
        [
            *host_cc(),
            "-D_GNU_SOURCE",
            "-std=gnu11",
            "-Wall",
            "-Wextra",
            "-Werror",
            "-Wno-unused-parameter",
            "-Wno-format",
            "-I",
            str(libfdt),
            str(source),
            *map(str, sources),
            "-o",
            str(binary),
        ],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
