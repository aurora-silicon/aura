/* SPDX-License-Identifier: MIT */

#ifndef ADT_GPIO_H
#define ADT_GPIO_H

#include "types.h"

/*
 * An ADT function-* property that names a pin of an AP-visible GPIO controller:
 * <controller phandle> 'GPIO' <pin> <control word>. The control word's second byte is the
 * polarity: nonzero means a high pin is the asserted level, zero means it is inverted.
 */
struct adt_function_gpio {
    u32 phandle;
    u32 fourcc;
    u32 pin;
    u32 control;
} PACKED;

#define ADT_GPIO_FOURCC        0x4750494f /* 'GPIO' */
#define ADT_GPIO_CTRL_POLARITY GENMASK(15, 8)

/* Apple GPIO pin register (one u32 per pin) */
#define ADT_GPIO_PIN_DATA     BIT(0)
#define ADT_GPIO_PIN_MODE     GENMASK(3, 1)
#define ADT_GPIO_PIN_MODE_OUT 1
#define ADT_GPIO_PIN_MODE_IN  7 /* input, interrupt off */
#define ADT_GPIO_PIN_PERIPH   GENMASK(6, 5)
#define ADT_GPIO_PIN_INPUT_EN BIT(9)

/* The base of the /arm-io GPIO controller whose AAPL,phandle is @phandle, and its pin count */
int adt_gpio_by_phandle(const void *adt, u32 phandle, u64 *base, u32 *npins);

/* A function-* property resolved to its pin register and the level that means asserted */
struct adt_gpio_pin {
    u64 reg;
    bool active_high;
};

/*
 * Resolve @node's function-* GPIO property @prop to its pin register. 0 when it names a pin of an
 * /arm-io GPIO controller, 1 when @prop is absent, -1 when it is present but is not such a pin.
 */
int adt_gpio_function_pin(const void *adt, int node, const char *prop, struct adt_gpio_pin *pin);

/*
 * Sample a resolved pin without touching its configuration: 1 asserted, 0 not asserted, -1 when
 * the pin is not configured so that its level can be read (an output, muxed to a peripheral
 * function, or its input stage off). @cfg receives the pin register as read.
 */
int adt_gpio_pin_sample(const struct adt_gpio_pin *pin, u32 *cfg);

#endif
