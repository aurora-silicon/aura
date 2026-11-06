/* SPDX-License-Identifier: GPL-2.0-only */
/* J873g USB0, from the 26A428 USB0 power/CDC bring-up. USB2 only. */
#include "usb.h"
#include "adt.h"
#include "dart.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#define USB0_DWC  0x402430000ULL
#define USB0_PIPE 0x402414000ULL
#define USB0_PHY  0x402418000ULL
#define USB0_EVT  0x402448000ULL
#define USB0_PSD  0x303a14700ULL
#define USB0_AON  0x308290100ULL
#define USB0_GATE 0x308290120ULL
#define USB0_WAIT 250000

static bool reg_is(const char *path, const char *compat, unsigned idx, u64 base, u64 size)
{
    int trace[8];
    int node = adt_path_offset_trace(adt, path, trace);
    u64 got_base, got_size;
    return node >= 0 && adt_is_compatible(adt, node, compat) &&
           !adt_get_reg(adt, trace, "reg", idx, &got_base, &got_size) && got_base == base &&
           got_size == size;
}

/* Resolve the three ATC0_USB dependencies before writing any gate. */
static bool power_layout(void)
{
    int trace[8];
    int node = adt_path_offset_trace(adt, "/arm-io/pmgr-child", trace);
    u32 len, physical_count;
    if (node < 0 || !adt_is_compatible(adt, node, "pmgr2,t8152") ||
        ADT_GETPROP(adt, node, "virt-service-start", &physical_count) != 4 || physical_count != 170)
        return false;
    static const u8 service[] = {5,    0,   0,    0,    1, 15, 1,   0, 0x48, 1,   71,  0,
                                 0x37, 1,   0xff, 0xff, 0, 0,  174, 0, 'A',  'T', 'C', '0',
                                 '_',  'U', 'S',  'B',  0, 0,  0,   0, 0,    0,   0,   0};
    const u8 *records = adt_getprop(adt, node, "service", &len);
    unsigned found = 0;
    if (!records || len % 36)
        return false;
    for (u32 off = 0; off < len; off += 36) {
        u16 id;
        memcpy(&id, records + off + 18, 2);
        if (id == 174) {
            if (memcmp(records + off, service, sizeof(service)))
                return false;
            found++;
        }
    }
    if (found != 1)
        return false;

    const u32 *ps = adt_getprop(adt, node, "ps-groups", &len);
    if (!ps || len < 24 || len % 12 || ps[3] != 1 || ps[4] != 0)
        return false;
    u64 base, size;
    if (adt_get_reg(adt, trace, "reg", 1, &base, &size) || base + 0x10100 != USB0_AON ||
        size < 0x10124)
        return false;
    records = adt_getprop(adt, node, "devices", &len);
    if (!records || len % 48)
        return false;
    found = 0;
    for (u32 off = 0; off < len; off += 48) {
        const u8 *r = records + off;
        u16 id;
        u32 packed;
        memcpy(&id, r + 26, 2);
        memcpy(&packed, r + 16, 4);
        if (id == 310 || id == 311) {
            static const u8 zero[16];
            const char *name = id == 310 ? "ATC0_USB_AON" : "ATC0_USB";
            if (memcmp(r, zero, 16) || packed != (id == 310 ? 0x01010100u : 0x01010120u) ||
                memcmp(r + 32, name, strlen(name) + 1))
                return false;
            if (found & BIT(id - 310))
                return false;
            found |= BIT(id - 310);
        } else if (id == 328) {
            static const u8 header[16] = {16, 0, 0, 0, 0x36, 1};
            if (memcmp(r, header, 16) || memcmp(r + 32, "AUSB0_AONUSB-V", 14) || (found & 4))
                return false;
            found |= 4;
        }
    }
    if (found != 7)
        return false;

    struct group {
        u16 id, map;
        u32 offset;
    };
    const struct group *groups = adt_getprop(adt, node, "reg-groups", &len);
    if (!groups || len % sizeof(*groups))
        return false;
    found = 0;
    for (u32 i = 0; i < len / sizeof(*groups); i++)
        if (groups[i].id == 0x1d) {
            if (groups[i].map != 0x1c1 || groups[i].offset != 0xa10000)
                return false;
            found++;
        }
    if (found != 1)
        return false;
    struct mapping {
        u16 reg, id;
    };
    const struct mapping *maps = adt_getprop(adt, node, "reg-maps", &len);
    if (!maps || len % sizeof(*maps))
        return false;
    found = 0;
    for (u32 i = 0; i < len / sizeof(*maps); i++)
        if (maps[i].id == 0x1c1) {
            if (adt_get_reg(adt, trace, "reg", maps[i].reg, &base, &size) ||
                base + 0xa14700 != USB0_PSD || size < 0xa147c0)
                return false;
            found++;
        }
    return found == 1;
}

static int phy_tuning(int node, const char *name, bool apply)
{
    u32 len;
    const u32 *p = adt_getprop(adt, node, name, &len);
    if (!p || !len || len % 12)
        return -1;
    for (u32 i = 0; i < len / 4; i += 3) {
        u32 off = p[i] & 0xffffff;
        if (p[i] >> 24 != 32 || (off != 8 && off != 0x10 && off != 0x14 && off != 0x18))
            return -1;
        if (apply)
            mask32(USB0_PHY + off, p[i + 1], p[i + 2]);
    }
    return 0;
}

dwc3_dev_t *usb_t8152_init(void (*set_step)(unsigned step))
{
    int phy = adt_path_offset(adt, "/arm-io/atc-phy0");
    if (chip_id != T8152 || board_id != 0x24 ||
        !reg_is("/arm-io/usb-drd0", "usb-drd,t8152", 0, USB0_DWC, 0x11800) ||
        !reg_is("/arm-io/usb-drd0", "usb-drd,t8152", 3, USB0_PIPE, 0x4000) ||
        !reg_is("/arm-io/atc-phy0", "atc-phy,t8152", 0, USB0_PHY, 0x4000) ||
        !reg_is("/arm-io/atc-phy0", "atc-phy,t8152", 1, USB0_EVT, 0x4000) ||
        !reg_is("/arm-io/dart-usb0", "dart,gen3", 1, 0x403040000ULL, 0xc000) || !power_layout() ||
        phy_tuning(phy, "tunable_USB2PHY_DFLT", false) ||
        phy_tuning(phy, "tunable_USB2PHY_DEV", false)) {
        printf("USB0: unsupported T8152 ADT layout\n");
        return NULL;
    }
    set_step(2);
    if (read32(USB0_AON) != 0x300 || read32(USB0_GATE) != 0x300 ||
        (read32(USB0_PSD + 0x80) & 7) != 6) {
        printf("USB0: power dependencies are not cold\n");
        return NULL;
    }
    write32(USB0_AON, 15);
    if (poll32(USB0_AON, 0xf0, 0xf0, USB0_WAIT))
        return NULL;
    write32(USB0_PSD + 0x90, 0);
    if (poll32(USB0_PSD + 0xbc, 1, 1, USB0_WAIT) || poll32(USB0_PSD + 0x80, 7, 0, USB0_WAIT))
        return NULL;
    write32(USB0_GATE, 15);
    if (poll32(USB0_GATE, 0xf0, 0xf0, USB0_WAIT))
        return NULL;

    set_step(3);
    if (read32(USB0_PIPE + 0xc) != 0x42 || read32(USB0_PIPE + 0x1c) != 0 || read32(USB0_PHY) != 4 ||
        read32(USB0_PHY + 4) != 0xb)
        return NULL;
    clear32(USB0_DWC + 0xc200, 0x40);
    clear32(USB0_DWC + 0xc2c0, 0x20000);
    write32(USB0_PIPE + 0x1c, 1);
    if (read32(USB0_DWC + 0xc120) != 0x33313130 || (read32(USB0_DWC + 0xc704) & BIT(31))) {
        write32(USB0_PIPE + 0x1c, 0);
        return NULL;
    }
    write32(USB0_PIPE + 0x1c, 0);
    phy_tuning(phy, "tunable_USB2PHY_DFLT", true);
    phy_tuning(phy, "tunable_USB2PHY_DEV", true);
    set32(USB0_PHY + 8, 4);
    set32(USB0_PHY + 8, 8);
    mdelay(10);
    clear32(USB0_PHY + 4, 8);
    udelay(10);
    clear32(USB0_PHY + 4, 1);
    clear32(USB0_PHY + 4, 2);
    set32(USB0_EVT, 9);
    set32(USB0_PHY + 4, 4);
    clear32(USB0_PHY + 0x1c, BIT(29));
    clear32(USB0_PHY + 0x1c, BIT(30));
    udelay(30);
    if (!read32(USB0_EVT + 0x20)) {
        printf("USB0: no UTMI clock; leaving controller in reset\n");
        return NULL;
    }
    mdelay(5);
    mask32(USB0_PHY, 7, 2);
    write32(USB0_PIPE + 0x1c, 1);
    set32(USB0_PIPE + 0x20, BIT(15)); /* DUMMY_PHY_READY */

    set_step(4);
    dart_dev_t *dart = dart_gen3_init_adt_cold("/arm-io/dart-usb0", 1, 1);
    if (!dart)
        return NULL;
    return usb_dwc3_init(USB0_DWC, dart);
}
