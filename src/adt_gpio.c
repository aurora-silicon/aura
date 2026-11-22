/* SPDX-License-Identifier: MIT */

#include "adt_gpio.h"
#include "adt.h"
#include "string.h"
#include "utils.h"

int adt_gpio_by_phandle(const void *adt, u32 phandle, u64 *base, u32 *npins)
{
    int arm_io = adt_path_offset(adt, "/arm-io");
    int node = arm_io;

    if (arm_io < 0)
        return -1;

    ADT_FOREACH_CHILD(adt, node)
    {
        u32 ph;
        u32 len;
        const char *compat;
        char path[64];
        int trace[8];

        if (ADT_GETPROP(adt, node, "AAPL,phandle", &ph) != sizeof(ph) || ph != phandle)
            continue;
        compat = adt_getprop(adt, node, "compatible", &len);
        if (!compat || len < 5 || memcmp(compat, "gpio,", 5))
            return -1;
        if (ADT_GETPROP(adt, node, "#gpio-pins", npins) != sizeof(*npins))
            return -1;
        snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, node));
        if (adt_path_offset_trace(adt, path, trace) < 0 ||
            adt_get_reg(adt, trace, "reg", 0, base, NULL))
            return -1;
        return 0;
    }
    return -1;
}
