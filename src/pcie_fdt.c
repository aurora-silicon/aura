/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include "pcie.h"
#include "dart.h"
#include "smc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* J713 /amfm/function-reg_on pKW4 template. Its firmware bit meaning is opaque. */
#define SMC_J713_RADIO_REG_ON_TEMPLATE BIT(23)

/*
 * Boards with the J713's radio port: /amfm/function-reg_on is the same
 * pKW4("gP13", 0x800000) action and the radio is the only enabled endpoint
 * behind the built-in controller. The J715 shares the J713's board; the J604
 * ADT matches, and its SD reader port stays disabled in its tree. The J773g
 * is left out: its Ethernet shares the controller.
 */
bool pcie_radio_board_fdt(void *dt)
{
    return !fdt_node_check_compatible(dt, 0, "apple,j713") ||
           !fdt_node_check_compatible(dt, 0, "apple,j715") ||
           !fdt_node_check_compatible(dt, 0, "apple,j604");
}

static bool pcie_fdt_node_enabled(void *dt, int node)
{
    const char *status = fdt_getprop(dt, node, "status", NULL);

    return !status || !strcmp(status, "okay") || !strcmp(status, "ok");
}

static int pcie_fdt_endpoint_rid(void *dt, int port, u32 *rid)
{
    int endpoint;

    /*
     * AURORA_TODO: Represent and prepare every enabled function when future
     * machines expose multifunction or more complex downstream topologies.
     * J713 currently has one enabled endpoint below this port.
     */
    fdt_for_each_subnode(endpoint, dt, port)
    {
        int len;
        const fdt32_t *reg;

        if (!pcie_fdt_node_enabled(dt, endpoint))
            continue;

        reg = fdt_getprop(dt, endpoint, "reg", &len);
        if (!reg || len <= 0 || len % (5 * sizeof(*reg)))
            return -1;

        *rid = (fdt32_ld(reg) >> 8) & 0xffff;
        return 0;
    }

    return -1;
}

static int pcie_fdt_resolve_iommu(void *dt, int host, int port, u32 *phandle, u8 *sid)
{
    int len, mask_len;
    u32 rid;
    u32 mask = ~0U;
    const fdt32_t *map = fdt_getprop(dt, host, "iommu-map", &len);
    const fdt32_t *mask_prop = fdt_getprop(dt, host, "iommu-map-mask", &mask_len);

    if (!map || len < 4 * (int)sizeof(*map) || len % (4 * sizeof(*map)) ||
        pcie_fdt_endpoint_rid(dt, port, &rid))
        return -1;
    if (mask_prop) {
        if (mask_len != (int)sizeof(*mask_prop))
            return -1;
        mask = fdt32_ld(mask_prop);
    } else if (mask_len != -FDT_ERR_NOTFOUND) {
        return -1;
    }
    rid &= mask;

    int cells = len / sizeof(*map);
    for (int pos = 0; pos < cells;) {
        if (cells - pos < 4)
            return -1;

        u32 rid_base = fdt32_ld(&map[pos]);
        if (rid_base & ~mask)
            return -1;
        u32 provider_phandle = fdt32_ld(&map[pos + 1]);
        int provider = fdt_node_offset_by_phandle(dt, provider_phandle);
        if (provider < 0)
            return -1;

        int iommu_len;
        const fdt32_t *iommu_cells = fdt_getprop(dt, provider, "#iommu-cells", &iommu_len);
        if (!iommu_cells || iommu_len != (int)sizeof(*iommu_cells) || fdt32_ld(iommu_cells) != 1)
            return -1;

        u32 sid_base = fdt32_ld(&map[pos + 2]);
        u32 count = fdt32_ld(&map[pos + 3]);
        if (count && rid >= rid_base && rid - rid_base < count) {
            u32 offset = rid - rid_base;
            /* Bound each term before adding; a wrapped sum can look valid. */
            if (sid_base > 0xff || offset > 0xff - sid_base)
                return -1;
            *phandle = provider_phandle;
            *sid = sid_base + offset;
            return 0;
        }

        pos += 4;
    }

    return -1;
}

/*
 * Trees that list the radio's ordinary streams in apple,managed-sids leave them
 * to the OS, which adopts them only while they are inactive; the radio's
 * boot-time DMA goes through the DART's static SID16 bypass role instead
 * (bindings: apple,pcie.yaml and apple,dart.yaml). Older trees map the endpoint
 * to SID16 directly. Either way SID16 is the stream to bypass, and a mapped
 * stream outside the managed list is refused.
 */
#define PCIE_STATIC_BYPASS_SID 16

static int pcie_fdt_bypass_sid(void *dt, int host, u8 sid, u8 *bypass)
{
    int len;
    const fdt32_t *managed = fdt_getprop(dt, host, "apple,managed-sids", &len);

    if (!managed) {
        *bypass = sid;
        return len == -FDT_ERR_NOTFOUND ? 0 : -1;
    }
    if (len <= 0 || len % (int)sizeof(*managed))
        return -1;

    /* Admit the whole list before using it: a bad cell anywhere refuses it. */
    bool member = false;
    for (int i = 0; i < len / (int)sizeof(*managed); i++) {
        u32 managed_sid = fdt32_ld(&managed[i]);

        if (managed_sid == PCIE_STATIC_BYPASS_SID || managed_sid > 0xff)
            return -1;
        if (managed_sid == sid)
            member = true;
    }
    if (!member)
        return -1;
    *bypass = PCIE_STATIC_BYPASS_SID;
    return 0;
}

static bool pcie_fdt_t8132_port(void *dt, int host, int port, u32 *phandle, u8 *bypass)
{
    u8 sid;

    if (pcie_fdt_resolve_iommu(dt, host, port, phandle, &sid) ||
        pcie_fdt_bypass_sid(dt, host, sid, bypass))
        return false;

    int dart = fdt_node_offset_by_phandle(dt, *phandle);
    return dart >= 0 && !fdt_node_check_compatible(dt, dart, "apple,t8132-dart");
}

static u8 pcie_hex_digit(u32 value)
{
    return value < 10 ? '0' + value : 'a' + value - 10;
}

static int pcie_fdt_enable_pwren(void *dt, int port)
{
    int len;
    const fdt32_t *pwren = fdt_getprop(dt, port, "pwren-gpios", &len);
    if (!pwren)
        return 0;
    if (len != 3 * (int)sizeof(*pwren)) {
        printf("pcie: invalid pwren-gpios on %s\n", fdt_get_name(dt, port, NULL));
        return -1;
    }

    int gpio = fdt_node_offset_by_phandle(dt, fdt32_ld(&pwren[0]));
    if (gpio < 0)
        return -1;

    /* Only these boards' upper-case gP13 action has a qualified template.
     * A generic GPIO output command is not known to be equivalent. */
    if (!pcie_radio_board_fdt(dt) || fdt_node_check_compatible(dt, gpio, "apple,smc-gpio"))
        return -1;
    u32 key = 0x67500000; /* gP00 */

    u32 line = fdt32_ld(&pwren[1]);
    u32 flags = fdt32_ld(&pwren[2]);
    if (line != 0x13 || flags)
        return -1;

    key |= (u32)pcie_hex_digit(line >> 4) << 8;
    key |= pcie_hex_digit(line & 0xf);

    smc_dev_t *smc = smc_init();
    if (!smc)
        return -1;

    int ret = smc_write_u32(smc, key, SMC_J713_RADIO_REG_ON_TEMPLATE | 1);
    smc_shutdown(smc);
    /* Firmware refusal is positive; do not count it as a prepared rail. */
    if (ret)
        return -1;

    printf("pcie: enabled described rail %c%c%02x for %s\n", key >> 24, key >> 16, line,
           fdt_get_name(dt, port, NULL));
    return 1;
}

int pcie_prepare_fdt(void *dt)
{
    int host = -1;
    int prepared = 0;

    if (!pcie_radio_board_fdt(dt))
        return 0;

    while ((host = fdt_node_offset_by_compatible(dt, host, "apple,t8132-pcie")) >= 0) {
        if (!pcie_fdt_node_enabled(dt, host))
            continue;

        int port;
        fdt_for_each_subnode(port, dt, host)
        {
            u32 phandle;
            u8 sid;

            if (!pcie_fdt_node_enabled(dt, port) ||
                !pcie_fdt_t8132_port(dt, host, port, &phandle, &sid))
                continue;

            int ret = pcie_fdt_enable_pwren(dt, port);
            if (ret < 0)
                return -1;
            prepared += ret;
        }
    }

    return prepared;
}

int pcie_handoff_fdt(void *dt)
{
    int host = -1;
    int prepared = 0;

    if (!pcie_radio_board_fdt(dt))
        return 0;

    while ((host = fdt_node_offset_by_compatible(dt, host, "apple,t8132-pcie")) >= 0) {
        if (!pcie_fdt_node_enabled(dt, host))
            continue;

        int port;
        fdt_for_each_subnode(port, dt, host)
        {
            u32 phandle;
            u8 sid;

            if (!pcie_fdt_node_enabled(dt, port) || !fdt_getprop(dt, port, "pwren-gpios", NULL))
                continue;
            if (!pcie_fdt_t8132_port(dt, host, port, &phandle, &sid)) {
                printf("pcie: cannot resolve IOMMU mapping for described port %s\n",
                       fdt_get_name(dt, port, NULL));
                return -1;
            }
            if (dart_set_bypass_fdt(dt, phandle, sid))
                return -1;

            /* Keep the GPIO binding: Linux owns the endpoint rail after
             * adoption and may need to power-cycle the device later. */

            printf("pcie: resolved %s through DART phandle %u stream %u\n",
                   fdt_get_name(dt, port, NULL), phandle, sid);
            prepared++;
        }
    }

    return prepared;
}

/* The qualified radio boards have only their radio endpoint enabled behind
 * this controller. No native PCIe device driver may adopt a partially prepared
 * rail/link/DART. ANS storage is independent. Never change another board's
 * topology here.
 */
int pcie_disable_j713_fdt(void *dt)
{
    int host = -1;
    if (!pcie_radio_board_fdt(dt))
        return -1;
    while ((host = fdt_node_offset_by_compatible(dt, host, "apple,t8132-pcie")) >= 0) {
        int ret = fdt_setprop_string(dt, host, "status", "disabled");
        if (ret)
            return ret;
    }
    printf("pcie: optional radio controller disabled after failed handoff\n");
    return 0;
}
