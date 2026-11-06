/* SPDX-License-Identifier: GPL-2.0-only */
/* Optional J813 ownership transfer. The ordinary Windows path never calls this. */
#include "windows_n1.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "j813_n1.h"
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
    if (!model || len != sizeof("Mac17,3") || memcmp(model, "Mac17,3", len) || chosen < 0 ||
        bridge < 0 || apcie < 0 || dart < 0 || gpio < 0 || smc < 0 ||
        n1_property(chosen, "chip-id", &value, 1) || value != T8142 ||
        n1_property(chosen, "board-id", &value, 1) || value != 0x24 ||
        !adt_is_compatible(adt, apcie, "apcie,t8142") ||
        !adt_is_compatible(adt, dart, "dart,t8110") || !adt_is_compatible(adt, gpio, "gpio,t8101"))
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
        n1_property(smc, "AAPL,phandle", &phandle, 1) ||
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
    u32 p1, p3, p4, protect, tcr, ttbr, command;
    if (n1_read(DART, 4, &p1) || n1_read(DART + 8, 4, &p3) || n1_read(DART + 12, 4, &p4) ||
        n1_read(DART + 0x200, 4, &protect) || n1_read(DART + 0x1000, 4, &tcr) ||
        n1_read(DART + 0x1400, 4, &ttbr) || n1_read(DART + 0x80, 4, &command))
        return -1;
    return ((p1 >> 24) & 15) != 14 || ((p3 >> 24) & 63) != 42 || ((p3 >> 16) & 63) < 36 ||
                   ((p3 >> 16) & 63) > 42 || (p3 & 0xffff) < 0x100 || (p3 & 0xffff) >= 0x300 ||
                   !(p4 & 511) || (p4 & 511) > 256 || (protect & 1) || tcr || ttbr ||
                   (command & 0x80000000)
               ? -1
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

static int n1_assign_control(void)
{
    u32 old, high = 0, mask = 0, high_mask = ~0U;
    if (n1_checked(CTRL + 4, 2, 0, 7) || n1_read(CTRL + 0x10, 4, &old) ||
        ((old & 15) != 0 && (old & 15) != 4))
        return -1;
    bool wide = (old & 6) == 4;
    if ((wide && n1_read(CTRL + 0x14, 4, &high)) || n1_write(CTRL + 0x10, 4, ~0U) ||
        (wide && n1_write(CTRL + 0x14, 4, ~0U)))
        return -1;
    int result = n1_read(CTRL + 0x10, 4, &mask) || (wide && n1_read(CTRL + 0x14, 4, &high_mask));
    /* Restore the probe before validation, including a failed read. */
    if (n1_write(CTRL + 0x10, 4, old) || (wide && n1_write(CTRL + 0x14, 4, high)))
        return -1;
    u32 size = ~(mask & ~15U) + 1;
    if (result || high_mask != ~0U || (mask & 15) != (old & 15) || !size || (size & (size - 1)) ||
        size < 0x10000 || size > 0x200000)
        return -1;
    return n1_checked(ECAM + 0x18, 4, 0x00010100, 0x00ffffff) ||
                   n1_checked(ECAM + 0x20, 4, 0x80508000, 0xfff0fff0) ||
                   n1_checked(ECAM + 0x24, 4, 0x0000fff0, 0xfff0fff0) ||
                   n1_checked(ECAM + 4, 2, 2, 7) ||
                   n1_checked(CTRL + 0x10, 4, 0x80000000 | (old & 15), ~0U) ||
                   (wide && n1_checked(CTRL + 0x14, 4, 0, ~0U)) || n1_checked(CTRL + 4, 2, 2, 7)
               ? -1
               : 0;
}

int hv_windows_n1_prepare(void *descriptor, size_t size)
{
    static bool attempted;
    if (!descriptor || size != HV_WINDOWS_N1_DESCRIPTOR_SIZE)
        return -1;
    memset(descriptor, 0, size);
    if (!hv_windows_can_prepare_device() || attempted || n1_identity())
        return -1;
    attempted = true;
    if (n1_dart_clean())
        return -1;
    u32 link, pad;
    if (n1_read(PAD, 4, &pad) || (pad & BIT(21)) || n1_read(PORT + 0x208, 4, &link) ||
        ((link & 1) && n1_endpoints_clean()))
        return -1;

    /* One explicit reset/training operation; no firmware or DART programming. */
    if (j813_n1_train_windows() || n1_poll(PORT + 0x208, 1, 1, 1000000) || n1_endpoints_clean() ||
        n1_assign_control() || n1_dart_clean() || n1_endpoints_clean() ||
        n1_read(BAR + 0x8000, 4, &link) || link != 0) {
        if (n1_hold()) {
            printf("Windows N1: cannot prove PERST/link drain; quarantined\n");
            return -2;
        }
        printf("Windows N1: handoff rejected; held in reset\n");
        return -1;
    }

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
