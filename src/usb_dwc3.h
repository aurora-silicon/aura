/* SPDX-License-Identifier: MIT */

#ifndef USB_DWC3_H
#define USB_DWC3_H

#include "dart.h"
#include "types.h"

typedef struct dwc3_dev dwc3_dev_t;

typedef enum _cdc_acm_pipe_id_t {
    CDC_ACM_PIPE_0,
    CDC_ACM_PIPE_1,
    CDC_ACM_PIPE_MAX
} cdc_acm_pipe_id_t;

dwc3_dev_t *usb_dwc3_init(uintptr_t regs, dart_dev_t *dart);
dwc3_dev_t *usb_dwc3_init_mode(uintptr_t regs, dart_dev_t *dart, bool superspeed);
/* False when the controller may still reach its DMA buffers; they are then retained. */
bool usb_dwc3_shutdown(dwc3_dev_t *dev);

/*
 * Memory a DWC3 that could not be stopped may still reach (its buffers and
 * its DART's tables). It stays allocated, and the handoff reserves it.
 */
struct usb_dwc3_dma_range {
    u64 base;
    u64 size;
};
#define USB_DWC3_RETAINED_MAX 32
bool usb_dwc3_retain_dma(u64 base, u64 size);
bool usb_dwc3_retain_dart(dart_dev_t *dart);
/* The retained ranges; *overflow is set when some could not be recorded. */
size_t usb_dwc3_retained(const struct usb_dwc3_dma_range **ranges, bool *overflow);

void usb_dwc3_handle_events(dwc3_dev_t *dev);

ssize_t usb_dwc3_can_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
bool usb_dwc3_can_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);

u8 usb_dwc3_getbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);
void usb_dwc3_putbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte);

size_t usb_dwc3_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count);
size_t usb_dwc3_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
size_t usb_dwc3_queue(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count);
void usb_dwc3_flush(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe);

#endif
