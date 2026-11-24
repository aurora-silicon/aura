/* SPDX-License-Identifier: MIT */
/*
 * T8132 (M4) rules. They live here, not in kboot.c: kboot.c's code is linked into Stage 1's
 * .init slot (aura-raw.ld), which has almost no room left.
 */

#include "kboot.h"
#include "adt.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/* Whether a node other than skip names the IOMMU phandle in its iommus list. */
static bool dt_iommu_shared(const void *dt, u32 phandle, int skip)
{
    int node;

    for (node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL)) {
        if (node == skip)
            continue;

        int len;
        const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &len);
        if (!iommus) {
            if (len != -FDT_ERR_NOTFOUND)
                return true;
            continue;
        }

        if (len % sizeof(*iommus))
            return true;

        int cells = len / sizeof(*iommus);
        for (int i = 0; i < cells;) {
            u32 provider_phandle = fdt32_ld(&iommus[i++]);
            if (provider_phandle == phandle)
                return true;

            /* If the next tuple cannot be located, ownership is uncertain. */
            int provider = fdt_node_offset_by_phandle(dt, provider_phandle);
            if (provider < 0)
                return true;

            int size;
            const fdt32_t *args = fdt_getprop(dt, provider, "#iommu-cells", &size);
            if (!args || size != sizeof(*args))
                return true;

            u32 count = fdt32_ld(args);
            if (count > (u32)(cells - i))
                return true;
            i += count;
        }
    }
    return node != -FDT_ERR_NOTFOUND;
}

/*
 * A T8132 Mac without a camera (the J773g) has no isp or isp0 in its ADT. A tree that enables
 * the ISP there would have the OS probe a camera and power domains that do not exist: disable
 * the ISP and the DARTs that only it uses. A tree that leaves it disabled is not changed.
 */
int dt_disable_t8132_absent_isp(void *dt)
{
    if (adt_path_offset(adt, "/arm-io/isp") >= 0 || adt_path_offset(adt, "/arm-io/isp0") >= 0)
        return 0;

    int isp = -1;
    while ((isp = fdt_node_offset_by_compatible(dt, isp, "apple,t8132-isp")) >= 0) {
        const char *status = fdt_getprop(dt, isp, "status", NULL);
        if (status && !strcmp(status, "disabled"))
            continue;

        /* One cell of stream ID per DART, as in t8132.dtsi. Decide before any write. */
        u32 darts[8];
        bool own[8];
        int len, count = 0;
        const fdt32_t *iommus = fdt_getprop(dt, isp, "iommus", &len);
        int cells = iommus && len > 0 && !(len % sizeof(*iommus)) ? len / sizeof(*iommus) : 0;
        if (!(cells % 2) && cells / 2 <= (int)ARRAY_SIZE(darts))
            for (; count < cells / 2; count++) {
                darts[count] = fdt32_ld(&iommus[2 * count]);
                own[count] = !dt_iommu_shared(dt, darts[count], isp);
            }

        if (fdt_setprop_string(dt, isp, "status", "disabled"))
            bail("FDT: cannot disable the T8132 ISP\n");
        for (int i = 0; i < count; i++) {
            int dart = own[i] ? fdt_node_offset_by_phandle(dt, darts[i]) : -1;
            if (dart >= 0 && fdt_setprop_string(dt, dart, "status", "disabled"))
                bail("FDT: cannot disable a T8132 ISP DART\n");
        }
        printf("FDT: no ISP in the ADT; T8132 ISP disabled with the DARTs only it uses\n");
        isp = -1; /* Offsets moved; start over. Disabled nodes are skipped. */
    }
    return 0;
}
