/* SPDX-License-Identifier: GPL-2.0-only OR MIT */

#include "pcie.h"
#include "adt.h"
#include "dart.h"
#include "malloc.h"
#include "smc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* J713 /amfm/function-reg_on pKW4 template. Its firmware bit meaning is opaque. */
#define SMC_J713_RADIO_REG_ON_TEMPLATE BIT(23)

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

    /* Only this board's upper-case gP13 action has a qualified template.
     * A generic GPIO output command is not known to be equivalent. */
    if (fdt_node_check_compatible(dt, 0, "apple,j713") ||
        fdt_node_check_compatible(dt, gpio, "apple,smc-gpio"))
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

    if (fdt_node_check_compatible(dt, 0, "apple,j713"))
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

    if (fdt_node_check_compatible(dt, 0, "apple,j713"))
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

/* The qualified J713 path has only its radio endpoint behind this controller.
 * No native PCIe device driver may adopt a partially prepared rail/link/DART.
 * ANS storage is independent. Never change another board's topology here.
 */
/* The ADT's local tunable format. */
struct pcie_adt_tunable {
    u32 offset;
    u32 size;
    u64 mask;
    u64 value;
} PACKED;

/*
 * Post-M4: what an OS needs to bring the APCIe block ANS runs on back by itself after its power
 * was cycled, as Linux's deep sleep does, which only the ADT has. Each
 * table is <offset mask value> records relative to its /arm-io/apcie window, named as in the DT
 * PCIe node: reg[0] "config", reg[2] "phy-block", reg[3] "phy-ip", reg[4] "axi2af", reg[5]
 * "cio3pll", reg[6] "clkgen" and reg[7] "port0". The port's own come from pci-bridge0, and go on
 * the DT's pci@0,0.
 */
static const struct {
    const char *adt;
    const char *fdt;
    int reg;
    bool port;
} pcie_apcie_tables[] = {
    {"apcie-axi2af-tunables", "apple,tunable-axi2af", 4, false},
    {"apcie-phy-tunables", "apple,tunable-phy", 2, false},
    {"apcie-phy-ip-pll-tunables", "apple,tunable-phy-ip-pll", 3, false},
    {"apcie-phy-ip-auspma-tunables", "apple,tunable-phy-ip-auspma", 3, false},
    {"apcie-cio3pllcore-tunables", "apple,tunable-cio3pll", 5, false},
    {"apcie-pcieclkgen-tunables", "apple,tunable-clkgen", 6, false},
    {"apcie-config-tunables", "apple,tunable-port", 7, true},
    {"pcie-rc-tunables", "apple,tunable-rc", 0, true},
    {"pcie-rc-gen3-shadow-tunables", "apple,tunable-rc-gen3", 0, true},
    {"pcie-rc-gen4-shadow-tunables", "apple,tunable-rc-gen4", 0, true},
};

/* Adding to the host moves its port, so the port is looked up each time. */
static int pcie_apcie_table_node(void *dt, int host, size_t i)
{
    return pcie_apcie_tables[i].port ? fdt_subnode_offset(dt, host, "pci@0,0") : host;
}

/* One table, whole or not at all. */
static int pcie_apcie_table_fdt(void *dt, int host, int adt_node, int *path, size_t i)
{
    const struct pcie_adt_tunable *t;
    int node = pcie_apcie_table_node(dt, host, i);
    u64 base, size;
    u32 len;

    t = adt_getprop(adt, adt_node, pcie_apcie_tables[i].adt, &len);
    if (!t || !len || len % sizeof(*t) ||
        adt_get_reg(adt, path, "reg", pcie_apcie_tables[i].reg, &base, &size) < 0 || size < 4) {
        printf("FDT: %s missing or malformed\n", pcie_apcie_tables[i].adt);
        return -1;
    }

    u32 n = len / sizeof(*t);
    fdt32_t *cells = malloc(n * 3 * sizeof(*cells));
    if (!cells)
        return -1;
    for (u32 j = 0; j < n; j++) {
        if (t[j].size != 4 || t[j].offset % 4 || t[j].offset > size - 4 || t[j].mask >> 32 ||
            t[j].value >> 32) {
            printf("FDT: %s record %u unsupported\n", pcie_apcie_tables[i].adt, j);
            free(cells);
            return -1;
        }
        cells[3 * j] = cpu_to_fdt32(t[j].offset);
        cells[3 * j + 1] = cpu_to_fdt32(t[j].mask);
        cells[3 * j + 2] = cpu_to_fdt32(t[j].value);
    }

    int ret = fdt_setprop(dt, node, pcie_apcie_tables[i].fdt, cells, n * 3 * sizeof(*cells));
    free(cells);
    if (ret < 0) {
        printf("FDT: couldn't add %s: %s\n", pcie_apcie_tables[i].fdt, fdt_strerror(ret));
        return -1;
    }
    return n;
}

/* The DT PCIe host whose "rc" window is the ADT controller's reg[1]. */
static int pcie_apcie_host(void *dt, int *path)
{
    u64 rc;

    if (adt_get_reg(adt, path, "reg", 1, &rc, NULL) < 0)
        return -1;
    for (int node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL)) {
        int idx = fdt_stringlist_search(dt, node, "reg-names", "rc"), len;
        const fdt32_t *reg = fdt_getprop(dt, node, "reg", &len);
        int parent = fdt_parent_offset(dt, node);

        if (idx < 0 || !reg || fdt_address_cells(dt, parent) != 2 ||
            fdt_size_cells(dt, parent) != 2 || len < (idx + 1) * 16)
            continue;
        if (((u64)fdt32_to_cpu(reg[4 * idx]) << 32 | fdt32_to_cpu(reg[4 * idx + 1])) == rc)
            return node;
    }
    return -1;
}

/* 0 on a Mac whose ANS doesn't run on the APCIe block: nothing to publish. */
int pcie_apcie_tables_fdt(void *dt)
{
    int ans = adt_path_offset(adt, "/arm-io/ans");
    if (ans < 0 || !adt_get_property(adt, ans, "nvme-secure-bar"))
        return 0;

    const char *apcie =
        adt_path_offset(adt, "/arm-io/apcie") >= 0 ? "/arm-io/apcie" : "/arm-io/apcie0";
    char bridge[32];
    int path[8], bridge_path[8];
    snprintf(bridge, sizeof(bridge), "%s/pci-bridge0", apcie);
    int adt_node = adt_path_offset_trace(adt, apcie, path);
    int adt_bridge = adt_path_offset_trace(adt, bridge, bridge_path);
    int host = adt_node < 0 ? -1 : pcie_apcie_host(dt, path);
    int records = 0;

    if (adt_bridge < 0 || host < 0 || fdt_subnode_offset(dt, host, "pci@0,0") < 0) {
        printf("FDT: no PCIe host or port in the DT for %s's tables\n", apcie);
        return -1;
    }

    /* Properties added to the host or its port leave the host's offset as it is. */
    for (size_t i = 0; i < ARRAY_SIZE(pcie_apcie_tables); i++) {
        int from = pcie_apcie_tables[i].port ? adt_bridge : adt_node;
        int n = pcie_apcie_table_fdt(dt, host, from, path, i);
        if (n < 0) {
            for (size_t j = 0; j < i; j++)
                fdt_delprop(dt, pcie_apcie_table_node(dt, host, j), pcie_apcie_tables[j].fdt);
            return -1;
        }
        records += n;
    }

    printf("FDT: %s tables published, %d records\n", apcie, records);
    return records;
}

int pcie_disable_j713_fdt(void *dt)
{
    int host = -1;
    if (fdt_node_check_compatible(dt, 0, "apple,j713"))
        return -1;
    while ((host = fdt_node_offset_by_compatible(dt, host, "apple,t8132-pcie")) >= 0) {
        int ret = fdt_setprop_string(dt, host, "status", "disabled");
        if (ret)
            return ret;
    }
    printf("pcie: optional J713 radio controller disabled after failed handoff\n");
    return 0;
}
