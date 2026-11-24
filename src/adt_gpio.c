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

int adt_gpio_function_pin(const void *adt, int node, const char *prop, struct adt_gpio_pin *pin)
{
    struct adt_function_gpio fn;
    u32 len, npins;
    u64 base;

    if (!adt_getprop(adt, node, prop, &len))
        return 1;
    if (ADT_GETPROP(adt, node, prop, &fn) != sizeof(fn) || fn.fourcc != ADT_GPIO_FOURCC ||
        adt_gpio_by_phandle(adt, fn.phandle, &base, &npins) || fn.pin >= npins)
        return -1;
    pin->reg = base + 4 * fn.pin;
    pin->active_high = FIELD_GET(ADT_GPIO_CTRL_POLARITY, fn.control) != 0;
    return 0;
}

int adt_gpio_pin_sample(const struct adt_gpio_pin *pin, u32 *cfg)
{
    u32 val = read32(pin->reg);

    *cfg = val;
    if (FIELD_GET(ADT_GPIO_PIN_MODE, val) == ADT_GPIO_PIN_MODE_OUT ||
        FIELD_GET(ADT_GPIO_PIN_PERIPH, val) || !(val & ADT_GPIO_PIN_INPUT_EN))
        return -1;
    return !!(val & ADT_GPIO_PIN_DATA) == pin->active_high;
}
