/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef USB_CDC_STATE_H
#define USB_CDC_STATE_H

#ifdef CDC_STATE_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

/* usb_cdc_schedule() flags (proxy P_CDC_SCHEDULE argument 2). */
#define USB_CDC_FLAG_FORCE_SWAPPED (1u << 0) /* drive the ATC lanes flipped */
#define USB_CDC_FLAG_GEN1          (1u << 1) /* Gen1 descriptors and link: required */
#define USB_CDC_FLAG_HPM_RESET     (1u << 2) /* cold-reset HPM0 first, retiring KIS */
#define USB_CDC_FLAGS_ALL (USB_CDC_FLAG_FORCE_SWAPPED | USB_CDC_FLAG_GEN1 | USB_CDC_FLAG_HPM_RESET)

/* Accepted delay before a scheduled transition starts (policy, not a hardware wait). */
#define USB_CDC_SCHEDULE_MIN_MS 1000
#define USB_CDC_SCHEDULE_MAX_MS 15000

/* Re-advertise after a host disconnect: first try, spacing and budget (policy). */
#define USB_CDC_RECOVERY_FIRST_MS 25
#define USB_CDC_RECOVERY_RETRY_MS 2000
#define USB_CDC_RECOVERY_ATTEMPTS 3

enum usb_cdc_state_id {
    USB_CDC_IDLE,
    USB_CDC_SCHEDULED,
    USB_CDC_DWC_READY,
    USB_CDC_FAILED,
};

struct usb_cdc_state {
    enum usb_cdc_state_id state;
    u32 flags;
    u32 step;
    u64 deadline_ms;
};

struct usb_cdc_recovery {
    bool pending;
    u32 attempts;
    u64 deadline_ms;
};

int usb_cdc_state_schedule(struct usb_cdc_state *cdc, u32 delay_ms, u32 reserved, u32 flags,
                           u64 now_ms);
bool usb_cdc_state_due(const struct usb_cdc_state *cdc, u64 now_ms);
u32 usb_cdc_state_status(const struct usb_cdc_state *cdc);
void usb_cdc_recovery_arm(struct usb_cdc_recovery *recovery, u64 now_ms);
void usb_cdc_recovery_connected(struct usb_cdc_recovery *recovery);
bool usb_cdc_recovery_due(const struct usb_cdc_recovery *recovery, u64 now_ms);
void usb_cdc_recovery_attempted(struct usb_cdc_recovery *recovery, u64 now_ms);

bool usb_cdc_dma_may_release(bool active, int end_status, bool reset_complete);

#endif
