# SPDX-License-Identifier: GPL-2.0-only
"""Run the production PCIe initializer against a fail-closed MMIO model.

The model checks that T8140 skips both PHY IP groups after the shared PHY
clock/reset handshake, then reaches common clock mode, RC readiness and port
bring-up. It also checks that T8132 still applies both groups. APPCLK, port
PHY and RUN prerequisites are checked at ECAM access. The model does not
qualify the T8140 reset bit on hardware.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc


def test_t8140_sequence(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/pcie.c").read_text()
    source = "\n".join(line for line in source.splitlines()
                       if not line.startswith("#include"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FIELD_PREP(f, v) (((v) * ((f) & -(f))) & (f))
#define T8140 0x8140
#define T6050 0x6050
#define ADT_GETPROP(a, n, p, v) adt_getprop_copy(a, n, p, v, sizeof(*(v)))
static void *adt;
static int chip_id = T8140;
static const char *controller_compat = "apcie,t8140";
static u64 regs[][2] = {
    {0x1cb0000000, 0x10000000}, {0x394000000, 0x4000},
    {0x397000000, 0x40000}, {0x397040000, 0x28000},
    {0x396000000, 0x1000000}, {0x395046200, 0x4000}, {0x395044000, 0x4000},
    {0x390028000, 0x8000}, {0x39003c000, 0x4000}, {0x397020000, 0x4000},
    {0x390024000, 0x4000}, {0x390048000, 0x4000}, {0x390044000, 0x4000},
    {0x391028000, 0x8000}, {0x39103c000, 0x4000}, {0x397024000, 0x4000},
    {0x391024000, 0x4000}, {0x391048000, 0x4000}, {0x391044000, 0x4000},
    {0x392028000, 0x8000}, {0x39203c000, 0x4000}, {0x397028000, 0x4000},
    {0x392024000, 0x4000}, {0x392048000, 0x4000}, {0x392044000, 0x4000},
};
#define PHY 0x397008000ULL
#define PORT_PHY 0x397020000ULL
#define PORT 0x390028000ULL
#define ECAM 0x1cb0000000ULL
static int powered, accesses, polls, fail_poll, invalid_adt, stuck_reset;
static int reset_delay, pll, auspma, port_run, fail_tunable;
static int common_mode, rc_ready, port_tunables;
static int bad_common;
static u32 reg_len_override;
static u32 phy_ctrl, port_ctrl, appclk;
static int adt_path_offset(const void *a, const char *path)
{
    if (!strcmp(path, "/arm-io/apcie")) return 1;
    if (!strcmp(path, "/arm-io/apcie/pci-bridge0")) return 2;
    return -1;
}
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    trace[0] = adt_path_offset(a, path);
    return trace[0];
}
static bool adt_is_compatible(const void *a, int node, const char *compat)
{
    return !strcmp(compat, node == 1 ? controller_compat : "apcie-bridge");
}
static const void *adt_getprop(const void *a, int node, const char *prop, u32 *len)
{
    if (!strcmp(prop, "apcie-common-tunables")) return bad_common ? regs : NULL;
    if (len) *len = reg_len_override ? reg_len_override : sizeof(regs);
    return regs;
}
static int adt_getprop_copy(const void *a, int node, const char *prop, void *out, size_t len)
{
    assert(len == 4);
    if (!strcmp(prop, "#ports")) *(u32 *)out = 3;
    else if (!strcmp(prop, "maximum-link-speed")) *(u32 *)out = 2;
    else return -1;
    return 4;
}
static int adt_get_reg(const void *a, int *path, const char *prop, int i, u64 *base, u64 *size)
{
    assert(i >= 0 && i < (int)ARRAY_SIZE(regs));
    *base = regs[i][0];
    if (size) *size = regs[i][1];
    return 0;
}
static int adt_first_child_offset(const void *a, int node) { return -1; }
static int tunables_validate_local(const char *path, const char *prop, u64 span)
{
    assert(!powered);
    if (invalid_adt) return -1;
    if (!strcmp(prop, "apcie-common-tunables") && span < 0x100) return -1;
    if (!strcmp(prop, "apcie-config-tunables") && span < 0x5000) return -1;
    return 0;
}
static int pmgr_adt_power_enable(const char *path) { powered++; return 0; }
/* T6050 hardware is outside this T8140/T8132 MMIO model. */
static int pcie_t6050_init(void) { abort(); }
static int pmgr_power_steps(int die, const char *name, bool on) { abort(); }
static int pmgr_adt_power_disable_index(const char *path, int i) { assert(0); return -1; }
static int pmgr_adt_power_disable_under(const char *path, const char *name) { assert(0); return -1; }
static void access_mmio(u64 addr)
{
    assert(powered);
    accesses++;
    if (addr >= ECAM && addr < ECAM + 0x10000000)
        assert(common_mode && rc_ready && port_tunables && port_run &&
               (appclk & 1) && (port_ctrl & 0xf) == 0xf && !(port_ctrl & 0x10));
}
static u32 read32(u64 addr)
{
    access_mmio(addr);
    if (addr == PHY) return phy_ctrl;
    /* RUN_EN and RUN_EN2: ANS runs on the PHY, as on every boot this models. */
    if (addr == PHY + 4) return 0x11;
    if (addr == PORT_PHY) return port_ctrl;
    if (addr == PORT + 0x800) return appclk;
    return 0;
}
static void write32(u64 addr, u32 val)
{
    access_mmio(addr);
    if (addr == PHY || addr == PORT_PHY) {
        u32 *ctrl = addr == PHY ? &phy_ctrl : &port_ctrl;
        if ((*ctrl & 0x10) && !(val & 0x10)) assert((*ctrl & 0xf) == 0xf);
        if (addr == PORT_PHY) assert(appclk & 1);
        if (stuck_reset && addr == PHY) val |= 0x10;
        *ctrl = val | ((val & 3) << 2);
    }
    if (addr == PORT + 0x800) appclk = val;
    if (addr == 0x394000050ULL || addr == 0x394000054ULL) {
        assert(common_mode);
        if (addr == 0x394000050ULL) rc_ready = 1;
    }
}
static void set32(u64 addr, u32 val) { write32(addr, read32(addr) | val); }
static void clear32(u64 addr, u32 val) { write32(addr, read32(addr) & ~val); }
static void mask32(u64 addr, u32 mask, u32 val)
{
    if (addr == 0x397004000ULL) {
        assert((phy_ctrl & 0xf) == 0xf && !(phy_ctrl & 0x10) && reset_delay >= 1);
        common_mode = 1;
    }
    write32(addr, (read32(addr) & ~mask) | val);
}
static void mask16(u64 addr, u16 mask, u16 val) { mask32(addr, mask, val); }
static void udelay(int us) { if (!(phy_ctrl & 0x10)) reset_delay += us; }
static int poll32(u64 addr, u32 mask, u32 target, int timeout)
{
    assert(timeout > 0 && timeout <= 250000);
    polls++;
    if (polls == fail_poll) return -1;
    if (addr == PHY || addr == PORT_PHY) return (read32(addr) & mask) != target;
    access_mmio(addr);
    if (addr == PORT + 0x804) { assert(port_tunables); port_run = 1; }
    return 0;
}
static int tunables_apply_local_addr(const char *path, const char *prop, uintptr_t base)
{
    if (strstr(prop, "phy-ip-")) {
        assert(base == 0x397040000);
        assert(chip_id != T8140 && !common_mode);
        assert((phy_ctrl & 0xf) == 0xf && !(phy_ctrl & 0x10) && reset_delay >= 1);
        if (strstr(prop, "-pll-")) { assert(!auspma); pll++; }
        else { assert(pll == 1); auspma++; }
        access_mmio(base + (strstr(prop, "-pll-") ? 0x90 : 0xa000));
    } else {
        if (!strcmp(prop, "apcie-config-tunables")) {
            assert(rc_ready && common_mode);
            port_tunables = 1;
        }
        access_mmio(base);
    }
    return fail_tunable && strstr(prop, "phy-ip-pll") ? -1 : 0;
}
static int tunables_apply_local(const char *path, const char *prop, u32 index)
{
    return tunables_apply_local_addr(path, prop, regs[index][0]);
}
''' + source + r'''
static void reset_model(void)
{
    memset(controllers, 0, sizeof(controllers));
    pcie_initialized = false;
    chip_id = T8140;
    controller_compat = "apcie,t8140";
    powered = accesses = polls = fail_poll = invalid_adt = stuck_reset = 0;
    reset_delay = pll = auspma = port_run = fail_tunable = 0;
    common_mode = rc_ready = port_tunables = 0;
    bad_common = 0;
    reg_len_override = 0;
    regs[1][1] = 0x4000;
    regs[7][1] = 0x8000;
    phy_ctrl = port_ctrl = 0x10;
    appclk = 0;
}
static void expect_failure(void)
{
    assert(pcie_init() == -1);
    assert(!pcie_initialized && !controllers[APCIE].initialized);
    int before = accesses;
    assert(pcie_init() == -1);
    assert(accesses == before && powered == 1);
}
int main(void)
{
    reset_model();
    assert(pcie_init() == 0);
    assert(pll == 0 && auspma == 0 && common_mode && rc_ready &&
           port_tunables && port_run && controllers[APCIE].active_ports == 1);
    int total_polls = polls, before = accesses;
    assert(total_polls >= 8);
    assert(pcie_init() == 0 && accesses == before);
    for (int i = 1; i <= total_polls; i++) {
        reset_model();
        fail_poll = i;
        expect_failure();
        assert(pll == 0 && auspma == 0);
    }
    reset_model();
    stuck_reset = 1;
    expect_failure();
    assert(pll == 0 && auspma == 0);
    reset_model();
    fail_tunable = 1;
    assert(pcie_init() == 0 && pll == 0 && auspma == 0 && port_run);
    reset_model();
    invalid_adt = 1;
    assert(pcie_init() == 1 && powered == 0 && accesses == 0);
    reset_model();
    reg_len_override = sizeof(regs) - 16;
    assert(pcie_init() == 1 && powered == 0 && accesses == 0);
    reset_model();
    bad_common = 1;
    regs[1][1] = 0x5c;
    assert(pcie_init() == 1 && powered == 0 && accesses == 0);
    reset_model();
    regs[7][1] = 0x4000;
    assert(pcie_init() == 1 && powered == 0 && accesses == 0);
    reset_model();
    chip_id = 0x8132;
    controller_compat = "apcie,t8132";
    assert(pcie_init() == 0 && pll == 1 && auspma == 1 && port_run);
    reset_model();
    chip_id = 0x8132;
    controller_compat = "apcie,t8132";
    fail_tunable = 1;
    assert(pcie_init() == -1 && pll == 1 && auspma == 0 && !port_run);
    return 0;
}
'''
    binary = tmp_path / "pcie-sequence"
    subprocess.run(
        [*host_cc(), "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
