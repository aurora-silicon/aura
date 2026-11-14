/* SPDX-License-Identifier: MIT */
/*
 * T8132 (M4) board rules. They live here, not in kboot.c: kboot.c's code is linked into Stage 1's
 * .init slot (aura-raw.ld), which has almost no room left.
 */

#include "kboot.h"
#include "adt.h"
#include "firmware.h"
#include "soc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

/*
 * J773g (Mac mini, M4): the J773g admissions in this tree were checked against one ADT, that
 * of 26.6.2 (target J773gAP, board-id 0x2a, iBoot mBoot-18000.161.10). Admit them only when
 * the live machine, its firmware and the OS tree all say J773g.
 */
bool kboot_is_j773g(const void *fdt)
{
    return fdt && chip_id == T8132 && board_id == 0x2a && adt_is_compatible(adt, 0, "J773gAP") &&
           os_firmware.version == V26_6_2 && !fdt_node_check_compatible(fdt, 0, "apple,j773g") &&
           !fdt_node_check_compatible(fdt, 0, "apple,t8132");
}

/* Whether a node other than skip names the IOMMU phandle in its iommus list. */
static bool dt_iommu_shared(const void *dt, u32 phandle, int skip)
{
    int node;

    for (node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL)) {
        int len;
        const fdt32_t *iommus = node == skip ? NULL : fdt_getprop(dt, node, "iommus", &len);

        for (int i = 0; iommus && i + 1 < len / (int)sizeof(*iommus); i += 2)
            if (fdt32_ld(&iommus[i]) == phandle)
                return true;
    }
    return false;
}

/*
 * A T8132 Mac without a camera (the J773g) has no isp or isp0 in its ADT. A tree that enables
 * the ISP there would have the OS probe a camera and power domains that do not exist: disable
 * the ISP and the DARTs that only it uses. A tree that leaves it disabled is not changed.
 */
int dt_disable_t8132_absent_isp(void *dt)
{
    if (chip_id != T8132 || adt_path_offset(adt, "/arm-io/isp") >= 0 ||
        adt_path_offset(adt, "/arm-io/isp0") >= 0)
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
