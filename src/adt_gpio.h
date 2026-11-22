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

#endif
