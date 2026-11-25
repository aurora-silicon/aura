# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""The T6040 USB2 device tunables are exported as a pair, or the DT keeps its own pair."""
from pathlib import Path
import subprocess


def test_t6040_usb2_device_tunables_all_or_nothing(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot_atc.c").read_text()
    definitions = source[source.index("struct atc_tunable {"):source.index("struct atc_fuse_info {")]
    start = source.index("static int dt_append_atc_tunable(")
    append = source[start:source.index("\n}", start) + 2] + "\n"
    start = source.index("static const char *const j614s_phys[]")
    phys = source[start:source.index(";", start) + 1] + "\n"
    start = source.index("static void t6040_export_usb2_device_tunables(")
    export = source[start:source.index("\n}", start) + 2] + "\n"

    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <libfdt.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define BANK 0x382a90000ULL
#define BANK_SIZE 0x4000ULL
void *adt;
/* Synthetic lists (offset/size, mask, value); not taken from any Mac. */
static const u32 dflt[] = {0x20000010, 0x0000000f, 0x00000003, 0x20000020, 0x00000100, 0x0};
static const u32 dev[] = {0x20000030, 0xffff0000, 0x00120000};
static const u32 unaligned[] = {0x20000032, 1, 1};
static const u32 outside[] = {0x20004000, 1, 1};
static const u32 narrow[] = {0x10000030, 1, 1};
static const char *omit, *replace;
static const void *replacement;
static u32 replacement_len;
static int append_fail = -1;
static int append_u32(void *dt, int node, const char *name, uint32_t value)
{
    if (append_fail >= 0 && !append_fail--)
        return -FDT_ERR_NOSPACE;
    return fdt_appendprop_u32(dt, node, name, value);
}
#define fdt_appendprop_u32 append_u32
const void *adt_getprop(const void *tree, int node, const char *name, u32 *len)
{
    const void *data = NULL;
    u32 size = 0;
    if (omit && !strcmp(omit, name))
        return NULL;
    if (replace && !strcmp(replace, name)) {
        data = replacement;
        size = replacement_len;
    } else if (!strcmp(name, "tunable_USB2PHY_DFLT")) {
        data = dflt;
        size = sizeof(dflt);
    } else if (!strcmp(name, "tunable_USB2PHY_DEV")) {
        data = dev;
        size = sizeof(dev);
    }
    if (data && len)
        *len = size;
    return data;
}
int adt_path_offset_trace(const void *tree, const char *path, int *offsets)
{
    if (strcmp(path, "/arm-io/atc-phy0"))
        return -1;
    offsets[0] = 1;
    return 1;
}
int adt_get_reg(const void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    if (idx)
        return -1;
    *addr = BANK;
    *size = BANK_SIZE;
    return 0;
}
''' + definitions + append + phys + export + r'''
static const u32 old_pair[] = {0xdead0000, 0xdead0001, 0xdead0002};
static int phy_node(void *dt)
{
    int node = fdt_path_offset(dt, "/phy@382a90000");
    assert(node >= 0);
    return node;
}
static void setup(void *dt, int size, bool compatible, u64 bank)
{
    assert(!fdt_create_empty_tree(dt, size));
    assert(!fdt_setprop_u32(dt, 0, "#address-cells", 2));
    assert(!fdt_setprop_u32(dt, 0, "#size-cells", 2));
    int node = fdt_add_subnode(dt, 0, "phy@382a90000");
    assert(node >= 0);
    assert(!fdt_setprop_string(dt, node, "compatible",
                               compatible ? "apple,t6040-atcphy" : "apple,t8103-atcphy"));
    assert(!fdt_setprop_string(dt, node, "reg-names", "bank0"));
    assert(!fdt_setprop_u64(dt, node, "reg", bank));
    assert(!fdt_appendprop_u64(dt, node, "reg", BANK_SIZE));
    fdt32_t old[ARRAY_SIZE(old_pair)];
    for (size_t i = 0; i < ARRAY_SIZE(old_pair); i++)
        old[i] = cpu_to_fdt32(old_pair[i]);
    assert(!fdt_setprop(dt, node, "apple,tunable-usb2-default", old, sizeof(old)));
    assert(!fdt_setprop(dt, node, "apple,tunable-usb2-device", old, sizeof(old)));
}
static void expect(void *dt, const char *name, const u32 *want, int count)
{
    int len;
    const fdt32_t *data = fdt_getprop(dt, phy_node(dt), name, &len);
    assert(data && len == count * 4);
    for (int i = 0; i < count; i++)
        assert(fdt32_ld(data + i) == want[i]);
}
static void expect_list(void *dt, const char *name, const u32 *list, int count)
{
    u32 want[16];
    assert(count <= 16);
    for (int i = 0; i < count; i += 3) {
        want[i] = list[i] & 0xffffff;
        want[i + 1] = list[i + 1];
        want[i + 2] = list[i + 2];
    }
    expect(dt, name, want, count);
}
static void expect_untouched(void *dt)
{
    expect(dt, "apple,tunable-usb2-default", old_pair, 3);
    expect(dt, "apple,tunable-usb2-device", old_pair, 3);
}
int main(void)
{
    static char dt[4096];

    /* Both lists: the DT's pair is replaced by the ADT's. */
    setup(dt, sizeof(dt), true, BANK);
    t6040_export_usb2_device_tunables(dt);
    expect_list(dt, "apple,tunable-usb2-default", dflt, ARRAY_SIZE(dflt));
    expect_list(dt, "apple,tunable-usb2-device", dev, ARRAY_SIZE(dev));

    /* Either list missing or empty: the DT keeps its pair, both halves. */
    const char *names[] = {"tunable_USB2PHY_DFLT", "tunable_USB2PHY_DEV"};
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        setup(dt, sizeof(dt), true, BANK);
        omit = names[i];
        t6040_export_usb2_device_tunables(dt);
        omit = NULL;
        expect_untouched(dt);

        setup(dt, sizeof(dt), true, BANK);
        replace = names[i];
        replacement = dev;
        replacement_len = 0;
        t6040_export_usb2_device_tunables(dt);
        replace = NULL;
        expect_untouched(dt);
    }

    /* Either list invalid: checked before anything is written, so nothing changes. */
    const struct {
        const void *data;
        u32 len;
    } bad[] = {
        {unaligned, sizeof(unaligned)},
        {outside, sizeof(outside)},
        {narrow, sizeof(narrow)},
        {dev, sizeof(dev) - 1},
    };
    for (size_t i = 0; i < ARRAY_SIZE(names); i++)
        for (size_t j = 0; j < ARRAY_SIZE(bad); j++) {
            setup(dt, sizeof(dt), true, BANK);
            replace = names[i];
            replacement = bad[j].data;
            replacement_len = bad[j].len;
            t6040_export_usb2_device_tunables(dt);
            replace = NULL;
            expect_untouched(dt);
        }

    /* The node compatible decides, not the chip; a foreign bank0 is not ours either. */
    setup(dt, sizeof(dt), false, BANK);
    t6040_export_usb2_device_tunables(dt);
    expect_untouched(dt);
    setup(dt, sizeof(dt), true, BANK + BANK_SIZE);
    t6040_export_usb2_device_tunables(dt);
    expect_untouched(dt);

    /* A failed FDT write never leaves half a pair behind. */
    for (int fail = 0; fail < (int)(ARRAY_SIZE(dflt) + ARRAY_SIZE(dev)); fail++) {
        setup(dt, sizeof(dt), true, BANK);
        append_fail = fail;
        t6040_export_usb2_device_tunables(dt);
        append_fail = -1;
        int node = phy_node(dt);
        assert(!fdt_getprop(dt, node, "apple,tunable-usb2-default", NULL));
        assert(!fdt_getprop(dt, node, "apple,tunable-usb2-device", NULL));
    }
    return 0;
}
'''
    cfile = tmp_path / "t6040-usb2-pair.c"
    binary = tmp_path / "t6040-usb2-pair"
    cfile.write_text(harness)
    # Link the in-tree libfdt rather than a host-installed one.
    libfdt = repo / "src/libfdt"
    sources = [str(libfdt / name) for name in (
        "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c", "fdt_empty_tree.c",
        "fdt_addresses.c", "fdt_strerror.c",
    )]
    subprocess.run(["cc", "-D_GNU_SOURCE", "-Wall", "-Werror", "-Wno-unused-function", "-I",
                    str(libfdt), str(cfile), *sources, "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, capture_output=True)
