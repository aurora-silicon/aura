/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional J813 ownership transfer. The ordinary Windows path never calls this. */
#include "windows_n1.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "j813_n1.h"
#include "pmgr.h"
#include "smp.h"
#include "string.h"
#include "utils.h"

#define ECAM 0x1cb0000000ULL
#define DART 0x488000000ULL
#define PORT 0x488028000ULL
#define PHY  0x48f010000ULL
#define PAD  0x39a000300ULL
#define BAR  0xb80000000ULL
#define CTRL (ECAM + 0x100000)

static const u64 n1_resources[6][2] = {
    {ECAM, 0x200000}, {DART, 0x20000}, {PORT, 0x4000}, {PHY, 0x1000}, {PAD, 4}, {BAR, 0x600000},
};
static bool n1_prepared;

static u32 le32(const void *data)
{
    const u8 *p = data;
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 le64(const void *data)
{
    const u8 *p = data;
    return le32(p) | (u64)le32(p + 4) << 32;
}

static void put_le(u8 *p, u64 value, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++, value >>= 8)
        p[i] = value;
}

static int n1_property(int node, const char *name, u32 *words, unsigned count)
{
    u32 length;
    const u8 *p = adt_getprop(adt, node, name, &length);
    if (!p || length != count * 4)
        return -1;
    for (unsigned i = 0; i < count; i++)
        words[i] = le32(p + i * 4);
    return 0;
}

static int n1_reg(const char *path, int index, u64 base, u64 length)
{
    int offsets[8];
    u64 address, size;
    return adt_path_offset_trace(adt, path, offsets) < 0 ||
                   adt_get_reg(adt, offsets, "reg", index, &address, &size) < 0 ||
                   address != base || size != length
               ? -1
               : 0;
}

/* Check the parent apertures, from which the six exclusive subranges derive. */
static int n1_identity(void)
{
    if (chip_id != T8142 || board_id != 0x24)
        return -1;
    u32 len, value;
    const char *model = adt_getprop(adt, 0, "model", &len);
    int chosen = adt_path_offset(adt, "/chosen");
    int bridge = adt_path_offset(adt, "/arm-io/apcie/pci-bridge0");
    int apcie = adt_path_offset(adt, "/arm-io/apcie");
    int dart = adt_path_offset(adt, "/arm-io/dart-apcie0");
    int gpio = adt_path_offset(adt, "/arm-io/gpio0");
    int smc = adt_path_offset(adt, "/arm-io/smc");
    int smc_nub = adt_path_offset(adt, "/arm-io/smc/iop-smc-nub");
    int smc_pmu = adt_path_offset(adt, "/arm-io/smc/iop-smc-nub/smc-pmu");
    if (!model || len != sizeof("Mac17,3") || memcmp(model, "Mac17,3", len) || chosen < 0 ||
        bridge < 0 || apcie < 0 || dart < 0 || gpio < 0 || smc < 0 || smc_nub < 0 || smc_pmu < 0 ||
        n1_property(chosen, "chip-id", &value, 1) || value != T8142 ||
        n1_property(chosen, "board-id", &value, 1) || value != 0x24 ||
        !adt_is_compatible(adt, apcie, "apcie,t8142") ||
        !adt_is_compatible(adt, dart, "dart,t8110") ||
        !adt_is_compatible(adt, gpio, "gpio,t8101") ||
        !adt_is_compatible(adt, smc, "iop,ascwrap-v6") ||
        !adt_is_compatible(adt, smc_nub, "iop-nub,rtbuddy-v2") ||
        !adt_is_compatible(adt, smc_pmu, "smc-pmu"))
        return -1;
    if (n1_reg("/arm-io/apcie", 0, ECAM, 0x10000000) || n1_reg("/arm-io/apcie", 8, PORT, 0x8000) ||
        n1_reg("/arm-io/apcie", 10, PHY, 0x4000) ||
        n1_reg("/arm-io/dart-apcie0", 0, DART, 0x20000) ||
        n1_reg("/arm-io/gpio0", 0, PAD - 0x300, 0x100000))
        return -1;
    /* Training also touches these public Linux PHY/root-controller ranges. */
    if (n1_reg("/arm-io/apcie", 1, 0x48c000000, 0x4000) ||
        n1_reg("/arm-io/apcie", 2, 0x48f000000, 0x40000) ||
        n1_reg("/arm-io/apcie", 3, 0x48f020000, 0x4000) ||
        n1_reg("/arm-io/apcie", 4, 0x48f040000, 0x20000) ||
        n1_reg("/arm-io/apcie", 5, 0x48e000000, 0x1000000))
        return -1;
    u32 function[4], phandle;
    if (n1_property(gpio, "AAPL,phandle", &phandle, 1) ||
        n1_property(bridge, "function-perst", function, 4) || function[0] != phandle ||
        function[1] != 0x4750494f || function[2] != 192 || function[3] != 0 ||
        /* The function provider is the PMU child, not the ASC controller. */
        n1_property(smc_pmu, "AAPL,phandle", &phandle, 1) ||
        n1_property(bridge, "function-reset", function, 4) || function[0] != phandle ||
        function[1] != 0x704b5734 || function[2] != 0x67503133 || function[3] != 0x800000)
        return -1;
    const u8 *ranges = adt_getprop(adt, apcie, "ranges", &len);
    if (!ranges || len % 28)
        return -1;
    unsigned found = 0;
    for (u32 off = 0; off < len; off += 28)
        if (le32(ranges + off) == 0x02000000 && le64(ranges + off + 4) == 0x80000000 &&
            le64(ranges + off + 12) == BAR && le64(ranges + off + 20) == 0x40000000)
            found++;
    return found == 1 ? 0 : -1;
}

/* Do not turn a guarded bus fault into a valid register value. */
static int n1_read(u64 address, unsigned width, u32 *value)
{
    enum exc_guard_t saved = exc_guard;
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    *value = width == 2 ? read16(address) : read32(address);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = saved;
    return before == exc_count ? 0 : -1;
}
/* Mu's v1 census only; resource membership alone never authorizes a read. */
static bool n1_census_address(u64 address)
{
    static const u64 addresses[] = {
        PORT + 0x208,  ECAM,          CTRL,          CTRL + 4,      CTRL + 0x10,   CTRL + 0x14,
        CTRL + 0x1000, CTRL + 0x1004, CTRL + 0x2000, CTRL + 0x2004, BAR + 0x8000,  DART,
        DART + 8,      DART + 0xc,    DART + 0x200,  DART + 0x1000, DART + 0x1400,
    };
    for (unsigned i = 0; i < ARRAY_SIZE(addresses); ++i)
        if (address == addresses[i])
            return true;
    return false;
}
bool hv_windows_n1_census_hvc(u64 regs[4])
{
    if (regs[0] != HV_WINDOWS_N1_READ_CALL)
        return false;

    int status = HV_WINDOWS_N1_READ_UNAVAILABLE;
    u32 value = 0;
    /* The shared exception guard is used under the HV lock, on the BSP only.
     * A pending/masked SError cannot safely be attributed to this read. */
    if (!n1_prepared || smp_id() != boot_cpu_idx || exc_guard != GUARD_OFF ||
        (mrs(DAIF) & BIT(8)) || (mrs(ISR_EL1) & BIT(8)))
        goto out;
    status = HV_WINDOWS_N1_READ_INVALID;
    if (regs[1] == HV_WINDOWS_N1_CAPS_ADDRESS && regs[2] == HV_WINDOWS_N1_READ_VERSION && !regs[3]) {
        status = HV_WINDOWS_N1_READ_OK;
        value = HV_WINDOWS_N1_CAP_RELINK;
        goto out;
    }
    if (regs[2] != HV_WINDOWS_N1_READ_VERSION || regs[3] || !n1_census_address(regs[1]))
        goto out;
    status = n1_read(regs[1], 4, &value) ? HV_WINDOWS_N1_READ_FAULT : HV_WINDOWS_N1_READ_OK;
out:
    regs[0] = HV_WINDOWS_N1_READ_ACK;
    regs[1] = status;
    regs[2] = status == HV_WINDOWS_N1_READ_OK ? value : 0;
    regs[3] = HV_WINDOWS_N1_READ_VERSION;
    return true;
}

static int n1_write(u64 address, unsigned width, u32 value)
{
    enum exc_guard_t saved = exc_guard;
    int before = exc_count;
    exc_guard = GUARD_SKIP | GUARD_SILENT;
    if (width == 2)
        write16(address, value);
    else
        write32(address, value);
    sysop("dsb sy");
    sysop("isb");
    exc_guard = saved;
    return before == exc_count ? 0 : -1;
}

static int n1_checked(u64 address, unsigned width, u32 value, u32 mask)
{
    u32 back;
    return n1_write(address, width, value) || n1_read(address, width, &back) ||
                   ((back ^ value) & mask)
               ? -1
               : 0;
}

static int n1_poll(u64 address, u32 mask, u32 target, unsigned timeout)
{
    for (unsigned elapsed = 0; elapsed < timeout; elapsed += 100) {
        u32 value;
        if (n1_read(address, 4, &value))
            return -1;
        if ((value & mask) == target)
            return 0;
        udelay(100);
    }
    return -1;
}

static int n1_dart_clean(void)
{
    const u32 offsets[] = {0, 8, 12, 0x200, 0x1000, 0x1400, 0x80};
    u32 value[ARRAY_SIZE(offsets)];
    for (unsigned i = 0; i < ARRAY_SIZE(offsets); i++) {
        printf("Windows N1: DART read %#llx begin\n", (unsigned long long)(DART + offsets[i]));
        if (n1_read(DART + offsets[i], 4, &value[i])) {
            printf("Windows N1: DART read fault\n");
            return -1;
        }
        printf("Windows N1: DART read complete %#x\n", value[i]);
    }
    u32 p1 = value[0], p3 = value[1], p4 = value[2], protect = value[3];
    u32 tcr = value[4], ttbr = value[5], command = value[6];
    return ((p1 >> 24) & 15) != 14 || ((p3 >> 24) & 63) != 42 || ((p3 >> 16) & 63) < 36 ||
                   ((p3 >> 16) & 63) > 42 || (p3 & 0xffff) < 0x100 || (p3 & 0xffff) >= 0x300 ||
                   !(p4 & 511) || (p4 & 511) > 256 || (protect & 1) || tcr || ttbr ||
                   (command & 0x80000000)
               ? -1
               : 0;
}

/* Do not infer cold ownership from ANS or shared parents. State 4 retains
 * clock-gated hardware; only power-gated target/actual state 0 is admitted. */
static int n1_cold(void)
{
    static const struct {
        u16 id, parent[2];
        const char *name;
    } expected[] = {
        {74, {60, 0}, "APCIE_GP"},         {109, {74, 0}, "APCIE_SYS_GP"},
        {110, {76, 0}, "APCIE_ST"},        {144, {76, 110}, "APCIE_SYS_ST"},
        {152, {144, 109}, "APCIE_PHY_SW"}, {155, {152, 0}, "ST0_AUS5_AON"},
        {156, {155, 0}, "ST0_AUS5"},
    };
    u32 gates[8];
    int apcie = adt_path_offset(adt, "/arm-io/apcie");
    if (n1_property(apcie, "clock-gates", gates, ARRAY_SIZE(gates)) || gates[0] != 76)
        return -1;
    struct pmgr_gate gate[ARRAY_SIZE(expected)];
    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        if (gates[i + 1] != expected[i].id ||
            pmgr_adt_gate("/arm-io/apcie", "clock-gates", i + 1, &gate[i]) || !gate[i].found ||
            gate[i].virt || gate[i].die || gate[i].id != expected[i].id ||
            gate[i].entry != expected[i].id || !gate[i].name ||
            strncmp(gate[i].name, expected[i].name, 16) || !gate[i].psreg ||
            gate[i].parent[0] != expected[i].parent[0] ||
            gate[i].parent[1] != expected[i].parent[1])
            return -1;
        for (unsigned j = 0; j < i; j++)
            if (gate[j].psreg == gate[i].psreg)
                return -1;
    }
    bool cold = true;
    for (unsigned i = 0; i < ARRAY_SIZE(expected); i++) {
        u32 state;
        if (n1_read(gate[i].psreg, 4, &state))
            return -1;
        printf("Windows N1: cold gate %s target=%u actual=%u reset=%u auto=%u raw=%#x\n",
               expected[i].name, state & 15, (state >> 4) & 15, (state >> 31) & 1,
               (state >> 28) & 1, state);
        if (state & (0xffU | (1U << 31) | (1U << 28)))
            cold = false;
    }
    return cold ? 0 : -1;
}

static int n1_gpio_hold(void)
{
    u32 pad;
    return n1_read(PAD, 4, &pad) || (pad & BIT(21)) || n1_checked(PAD, 4, (pad & ~15U) | 2, 15) ? -1
                                                                                                : 0;
}

static int n1_endpoints_clean(void)
{
    u32 id, command;
    if (n1_read(ECAM, 4, &id) || id != 0x100c106b)
        return -1;
    for (unsigned function = 0; function < 3; function++) {
        u64 base = CTRL + function * 0x1000;
        if (n1_read(base, 4, &id))
            return -1;
        if (function && (id & 0xffff) == 0xffff)
            continue;
        if ((!function && id != 0x1900106b && id != 0x1901106b) ||
            (function && id != ((0x1901 + function) << 16 | 0x106b)) ||
            n1_read(base + 4, 2, &command) || (command & 4))
            return -1;
    }
    return 0;
}

/* Same owned-controller PERST/port drain used by the public Windows PCIe core.
 * Never depend on further endpoint reads once the reset starts. */
static int n1_hold(void)
{
    u32 pad, value;
    int failed = 0;
    if (n1_read(PAD, 4, &pad) || (pad & BIT(21)))
        return -1;
    if (n1_read(PORT + 0x800, 4, &value) || n1_write(PORT + 0x800, 4, value | 0x100))
        failed = 1;
    if (n1_write(PORT + 0x88, 4, 0x11))
        failed = 1;
    (void)n1_poll(PORT + 0x88, 1, 0, 500000);
    if (n1_write(ECAM + 4, 2, 0) || n1_write(ECAM + 0x18, 4, 0))
        failed = 1;
    if (n1_read(PHY, 4, &value) || n1_write(PHY, 4, value & ~0x80000000U))
        failed = 1;
    /* Assert PERST even when earlier port operations failed. */
    if (n1_checked(PAD, 4, (pad & ~15U) | 2, 15))
        failed = 1;
    if (n1_poll(PORT + 0x804, 4, 0, 50000) || n1_poll(PORT + 0x820, 0x1ff, 0, 100000) ||
        n1_read(PORT + 0x13c, 4, &value) || n1_write(PORT + 0x13c, 4, value | 0x100) ||
        n1_poll(ECAM + 0x728, 0x3f, 0x14, 200000) || n1_poll(PORT + 0x208, 0x1001, 0, 200000))
        failed = 1;
    return failed ? -1 : 0;
}

/* Assignment diagnostics reuse the original accesses: no failure-only MMIO. */
static int n1_assign_read(const char *stage, u64 address, u32 *value)
{
    if (n1_read(address, 4, value)) {
        printf("Windows N1: BAR %s read fault at %#llx\n", stage, (unsigned long long)address);
        return -1;
    }
    printf("Windows N1: BAR %s address=%#llx value=%#x\n", stage, (unsigned long long)address,
           *value);
    return 0;
}

static int n1_assign_write(const char *stage, u64 address, u32 value)
{
    int result = n1_write(address, 4, value);
    printf("Windows N1: BAR %s address=%#llx value=%#x status=%d\n", stage,
           (unsigned long long)address, value, result);
    return result;
}

static int n1_assign_checked(const char *stage, u64 address, unsigned width, u32 value, u32 mask)
{
    u32 back;
    if (n1_write(address, width, value)) {
        printf("Windows N1: BAR %s write fault at %#llx\n", stage, (unsigned long long)address);
        return -1;
    }
    if (n1_read(address, width, &back)) {
        printf("Windows N1: BAR %s readback fault at %#llx\n", stage, (unsigned long long)address);
        return -1;
    }
    printf("Windows N1: BAR %s address=%#llx readback=%#x expected=%#x mask=%#x\n", stage,
           (unsigned long long)address, back, value, mask);
    return ((back ^ value) & mask) ? -1 : 0;
}

static int n1_assign_control(void)
{
    u32 old, high = 0, mask = 0, high_mask = ~0U;
    if (n1_assign_checked("control-command-off", CTRL + 4, 2, 0, 7) ||
        n1_assign_read("original-low", CTRL + 0x10, &old))
        return -1;
    /* PCI PREFETCH bit3 is independent of memory type bits2:1. Keep it in
     * the assigned BAR; Linux also permits this BAR in a nonprefetch window. */
    u32 memory_type = old & 6;
    if ((old & 1) || (memory_type != 0 && memory_type != 4))
        return -1;
    bool wide = (old & 6) == 4;
    if ((wide && n1_assign_read("original-high", CTRL + 0x14, &high)) ||
        n1_assign_write("probe-low", CTRL + 0x10, ~0U) ||
        (wide && n1_assign_write("probe-high", CTRL + 0x14, ~0U)))
        return -1;
    int result = n1_assign_read("mask-low", CTRL + 0x10, &mask) ||
                 (wide && n1_assign_read("mask-high", CTRL + 0x14, &high_mask));
    /* Restore the probe before validation, including a failed read. */
    if (n1_assign_write("restore-low", CTRL + 0x10, old) ||
        (wide && n1_assign_write("restore-high", CTRL + 0x14, high)))
        return -1;
    u32 size = ~(mask & ~15U) + 1;
    printf("Windows N1: BAR probe old=%#x high=%#x mask=%#x high_mask=%#x size=%#x status=%d\n",
           old, high, mask, high_mask, size, result);
    if (result || high_mask != ~0U || (mask & 15) != (old & 15) || !size || (size & (size - 1)) ||
        size < 0x10000 || size > 0x200000)
        return -1;
    return n1_assign_checked("root-buses", ECAM + 0x18, 4, 0x00010100, 0x00ffffff) ||
                   n1_assign_checked("root-memory-window", ECAM + 0x20, 4, 0x80508000,
                                     0xfff0fff0) ||
                   n1_assign_checked("root-prefetch-disable", ECAM + 0x24, 4, 0x0000fff0,
                                     0xfff0fff0) ||
                   n1_assign_checked("root-memory-enable", ECAM + 4, 2, 2, 7) ||
                   n1_assign_checked("control-address-low", CTRL + 0x10, 4, 0x80000000 | (old & 15),
                                     ~0U) ||
                   (wide && n1_assign_checked("control-address-high", CTRL + 0x14, 4, 0, ~0U)) ||
                   n1_assign_checked("control-memory-enable", CTRL + 4, 2, 2, 7)
               ? -1
               : 0;
}

#include "windows_n1_dart.h"
#include "windows_n1_scope.h"

static int n1_prepare(void *descriptor, size_t size, bool acquire)
{
    static bool attempted;
    if (!descriptor || size != HV_WINDOWS_N1_DESCRIPTOR_SIZE) {
        printf("Windows N1: descriptor buffer/size rejected before admission\n");
        return -1;
    }
    memset(descriptor, 0, size);
    if (!hv_windows_can_prepare_device()) {
        printf("Windows N1: Windows lifecycle rejected before admission\n");
        return -1;
    }
    if (attempted) {
        printf("Windows N1: repeated attempt rejected before admission\n");
        return -1;
    }
    if (acquire && n1_scope_tree()) {
        printf("Windows N1: ADT structure rejected before ownership admission\n");
        return -1;
    }
    if (n1_identity()) {
        printf("Windows N1: J813 identity/resources rejected before ownership admission\n");
        return -1;
    }
    attempted = true;
    if (j813_n1_validate_windows() || (acquire ? n1_scope_admit() : n1_cold())) {
        printf("Windows N1: ownership/tunable admission rejected before reset or power changes\n");
        return -1;
    }
    bool port_ready = false;
    u32 link;
    const char *phase = "GPIO/power/Linux fabric";
    printf("Windows N1: %s ownership admitted; clamping GPIO PERST\n",
           acquire ? "whole-N1 scope" : "cold gate");
    if (n1_gpio_hold() ||
        (acquire ? (n1_scope_power() || j813_n1_prepare_fabric_acquired_windows() ||
                    n1_scope_preserved())
                 : j813_n1_prepare_fabric_windows()))
        goto fail;
    /* The PHY/root prefix does not make DART accessible on J813. Match the
     * Linux loader's complete port/refclk/reset sequence first. The selected
     * admission proved ownership above; no endpoint command enables bus mastering.
     * Check every present endpoint before DART reads or BAR assignment. */
    printf("Windows N1: finishing Linux reset/port sequence before DART census\n");
    phase = "Linux reset/port";
    if (j813_n1_finish_windows(&port_ready) || n1_poll(PORT + 0x208, 1, 1, 1000000))
        goto fail;
    phase = "initial endpoint BME";
    if (n1_endpoints_clean())
        goto fail;
    phase = "N1 controller acquisition";
    if (acquire && (n1_scope_preserved() || n1_dart_acquire() || n1_scope_preserved()))
        goto fail;
    phase = "DART before BAR assignment";
    if (n1_dart_clean())
        goto fail;
    phase = "control BAR assignment";
    if (n1_assign_control())
        goto fail;
    phase = "DART after BAR assignment";
    if (n1_dart_clean())
        goto fail;
    phase = "final N1 ownership";
    if (acquire && (n1_dart_all_clean() || n1_scope_preserved()))
        goto fail;
    phase = "final endpoint BME";
    if (n1_endpoints_clean())
        goto fail;
    phase = "control ROM";
    if (n1_read(BAR + 0x8000, 4, &link) || link != 0)
        goto fail;
    goto publish;
fail:
    printf("Windows N1: primary failure at %s; starting owned-controller quarantine\n", phase);
    if (!port_ready) {
        int clamped = n1_gpio_hold();
        printf("Windows N1: pre-port failure; GPIO clamp %s; drain unproved, quarantined\n",
               clamped ? "failed" : "held");
        return -2;
    }
    if (n1_hold()) {
        printf("Windows N1: cannot prove PERST/link drain; quarantined\n");
        return -2;
    }
    printf("Windows N1: handoff rejected; held in reset\n");
    return -1;
publish:;
    u8 *out = descriptor;
    put_le(out, 0x4831574e, 4);
    put_le(out + 4, 1, 2);
    put_le(out + 6, size, 2);
    put_le(out + 8, 7, 4);
    put_le(out + 12, T8142, 4);
    put_le(out + 16, 0x24, 4);
    put_le(out + 20, 0x32354776, 4);
    for (unsigned i = 0; i < 3; i++)
        put_le(out + 24 + i * 2, 0x100 + i, 2);
    for (unsigned i = 0; i < 6; i++) {
        put_le(out + 32 + i * 16, n1_resources[i][0], 8);
        put_le(out + 40 + i * 16, n1_resources[i][1], 8);
    }
    printf("Windows N1: NW1H v1 ready; cold ROM, SID0 disabled, BME off\n");
    n1_prepared = true;
    return 0;
}

int hv_windows_n1_prepare(void *descriptor, size_t size)
{
    return n1_prepare(descriptor, size, false);
}

int hv_windows_n1_acquire(void *descriptor, size_t size)
{
    return n1_prepare(descriptor, size, true);
}

/* Guest-requested re-link (N1-09: by the time Windows starts its bus driver
 * the endpoint is silent, the root port's buses/windows are cleared and the
 * link no longer trains from PERST alone). Re-run the handoff's proven
 * Linux reset/port sequence, which includes the SMC N1 reset, then the same
 * endpoint, DART and BAR checks as the initial handoff. The port/PHY prefix
 * is not repeated: the N1-09 snapshot shows it retained. */
static unsigned n1_relinks;

static int n1_relink(u32 *detail)
{
    u32 value;
    bool port_ready = false;
    if (n1_relinks >= 8) {
        *detail = n1_relinks;
        return HV_WINDOWS_N1_RELINK_EXHAUSTED;
    }
    n1_relinks++;
    printf("Windows N1: guest relink %u requested; clamping GPIO PERST\n", n1_relinks);
    *detail = 1;
    if (n1_gpio_hold())
        goto fail;
    *detail = 2;
    if (j813_n1_relink_windows(&port_ready) || n1_poll(PORT + 0x208, 1, 1, 1000000))
        goto fail;
    *detail = 3;
    if (n1_endpoints_clean() || n1_dart_clean())
        goto fail;
    *detail = 4;
    if (n1_assign_control())
        goto fail;
    *detail = 5;
    if (n1_dart_clean() || n1_endpoints_clean())
        goto fail;
    *detail = 6;
    if (n1_read(BAR + 0x8000, 4, &value) || value != 0)
        goto fail;
    *detail = n1_relinks;
    printf("Windows N1: guest relink %u complete; cold ROM, SID0 disabled, BME off\n", n1_relinks);
    return HV_WINDOWS_N1_RELINK_OK;
fail:
    printf("Windows N1: guest relink %u failed at phase %u (port_ready=%d); holding PERST\n",
           n1_relinks, *detail, port_ready);
    (void)n1_gpio_hold();
    return HV_WINDOWS_N1_RELINK_FAILED;
}

bool hv_windows_n1_relink_hvc(u64 regs[4])
{
    if (regs[0] != HV_WINDOWS_N1_RELINK_CALL)
        return false;
    int status = HV_WINDOWS_N1_RELINK_UNAVAILABLE;
    u32 detail = 0;
    /* Same rules as the census: under the HV lock, BSP only, guard free and
     * no pending or masked SError that could be misattributed. */
    if (!n1_prepared || smp_id() != boot_cpu_idx || exc_guard != GUARD_OFF ||
        (mrs(DAIF) & BIT(8)) || (mrs(ISR_EL1) & BIT(8)))
        goto out;
    status = HV_WINDOWS_N1_RELINK_INVALID;
    if (regs[1] || regs[2] != HV_WINDOWS_N1_RELINK_VERSION || regs[3])
        goto out;
    status = n1_relink(&detail);
out:
    regs[0] = HV_WINDOWS_N1_RELINK_ACK;
    regs[1] = (u64)(long long)status;
    regs[2] = detail;
    regs[3] = HV_WINDOWS_N1_RELINK_VERSION;
    return true;
}
