/* SPDX-License-Identifier: MIT */
/*
 * The memory a DWC3 that could not be stopped may still reach: its buffers and
 * its DART's tables stay allocated, and kboot reserves them in the kernel's
 * tree (kboot_reserve_usb_dma()).
 */
#include "dart.h"
#include "usb_dwc3.h"
#include "utils.h"

static struct usb_dwc3_dma_range usb_dwc3_retained_ranges[USB_DWC3_RETAINED_MAX];
static size_t usb_dwc3_retained_count;
static bool usb_dwc3_retained_overflow;

bool usb_dwc3_retain_dma(u64 base, u64 size)
{
    if (!base || !size)
        return true;
    if (usb_dwc3_retained_count == USB_DWC3_RETAINED_MAX) {
        usb_dwc3_retained_overflow = true;
        return false;
    }
    usb_dwc3_retained_ranges[usb_dwc3_retained_count++] =
        (struct usb_dwc3_dma_range){.base = base, .size = size};
    return true;
}

bool usb_dwc3_retain_dart(dart_dev_t *dart)
{
    u64 tables[USB_DWC3_RETAINED_MAX];
    size_t count = 0;

    if (!dart)
        return true;
    if (dart_collect_tables(dart, tables, ARRAY_SIZE(tables), &count)) {
        usb_dwc3_retained_overflow = true;
        return false;
    }
    for (size_t i = 0; i < count; i++)
        if (!usb_dwc3_retain_dma(tables[i], SZ_16K))
            return false;
    return true;
}

size_t usb_dwc3_retained(const struct usb_dwc3_dma_range **ranges, bool *overflow)
{
    if (ranges)
        *ranges = usb_dwc3_retained_ranges;
    if (overflow)
        *overflow = usb_dwc3_retained_overflow;
    return usb_dwc3_retained_count;
}
