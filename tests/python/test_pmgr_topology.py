# SPDX-License-Identifier: GPL-2.0-only
"""Run the production pmgr_init() against synthetic ADT power-state tables.

Apple's device trees name parents they do not list: C0-USBCTL-V (id 293)
names parent 81 on base M1 (J274, J293, J313, J456, J457), and ISP_CPU_CORE0/1
(ids 177, 178) name parent 176 on base M3 (J433, J434, J504, J613, J615).
pmgr_init() must still succeed on those tables, as upstream does, while a
duplicate ID, a parent cycle or a real device without an address still fails
the whole table.
"""

from pathlib import Path
import subprocess


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define FIELD_GET(f, v) (((v) & (f)) / ((f) & -(f)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define PACKED __attribute__((packed))
#define T8140 0x8140
#define T8132 0x8132
#define ADT_GETPROP(a, n, p, v) (-1)

static int chip_id = 0x8122;
static bool kboot_is_j773g_machine(void) { return false; }
static void *adt = (void *)1;
static const void *devices_prop;
static u32 devices_len;
static const u32 ps_regs[] = {0, 0, 0};

static int adt_path_offset(const void *a, const char *path) { return 1; }
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    trace[0] = 1;
    trace[1] = 0;
    return 1;
}
static bool adt_is_compatible(const void *a, int node, const char *compat) { return false; }
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    if (!strcmp(name, "ps-regs")) {
        *len = sizeof(ps_regs);
        return ps_regs;
    }
    if (!strcmp(name, "devices")) {
        *len = devices_len;
        return devices_prop;
    }
    *len = 0;
    return NULL;
}
static int adt_get_reg(const void *a, const int *path, const char *prop, int idx, u64 *addr,
                       u64 *size)
{
    *addr = 0x2d0000000;
    return 0;
}
static u32 read32(uintptr_t addr) { return 0; }
static void write32(uintptr_t addr, u32 v) {}
static u32 mask32(uintptr_t addr, u32 clear, u32 set) { return 0; }
static u32 set32(uintptr_t addr, u32 set) { return 0; }
static u32 clear32(uintptr_t addr, u32 clear) { return 0; }
static int poll32(uintptr_t addr, u32 mask, u32 target, u32 timeout) { return 0; }
static void udelay(u32 us) {}
"""

TESTS = r"""
static struct pmgr_device table[8];

static void device(int i, u16 id, u16 parent, u8 flags, const char *name)
{
    memset(&table[i], 0, sizeof(table[i]));
    table[i].flags = flags;
    table[i].id2 = id;
    table[i].u16id.parent[0] = parent;
    table[i].addr_offset = (u8)i;
    strncpy((char *)table[i].name, name, sizeof(table[i].name));
}

static int init(int n)
{
    devices_prop = table;
    devices_len = n * sizeof(table[0]);
    return pmgr_init();
}

int main(void)
{
    assert(sizeof(struct pmgr_device) == 48);

    /* Base M3: virtual ISP_CPU_CORE0/1 name parent 176, which is not listed. */
    device(0, 100, 0, 0, "ISP_SYS");
    device(1, 177, 176, 0x10, "ISP_CPU_CORE0");
    device(2, 178, 176, 0x10, "ISP_CPU_CORE1");
    assert(init(3) == 0);

    /* Base M1: virtual C0-USBCTL-V names parent 81, which is not listed. */
    device(0, 80, 0, 0, "USB");
    device(1, 293, 81, 0x10, "C0-USBCTL-V");
    assert(init(2) == 0);

    /* A complete table still passes. */
    device(0, 1, 0, 0, "A");
    device(1, 2, 1, 0, "B");
    assert(init(2) == 0);

    /* Still refused: a duplicate ID, a cycle, and an ID of zero. */
    device(0, 1, 0, 0, "A");
    device(1, 1, 0, 0, "A2");
    assert(init(2) == -1);
    device(0, 1, 2, 0, "A");
    device(1, 2, 1, 0, "B");
    assert(init(2) == -1);
    device(0, 0, 0, 0, "ZERO");
    device(1, 2, 0, 0, "B");
    assert(init(2) == -1);
    return 0;
}
"""


def test_missing_parents_do_not_refuse_the_table(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    header = (repo / "src/pmgr.h").read_text()
    source = (repo / "src/pmgr.c").read_text()
    strip = lambda text: "\n".join(line for line in text.splitlines()
                                   if not line.startswith("#include"))
    program = HARNESS + strip(header) + strip(source) + TESTS
    binary = tmp_path / "pmgr-topology"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=program, text=True, check=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "ignoring parent 176" in result.stdout
    assert "ignoring parent 81" in result.stdout
