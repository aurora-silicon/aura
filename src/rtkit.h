/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef RTKIT_H
#define RTKIT_H

#include "../build/build_cfg.h"

#include "asc.h"
#include "dart.h"
#include "iova.h"
#include "sart.h"
#include "types.h"

typedef struct rtkit_dev rtkit_dev_t;

struct rtkit_message {
    u8 ep;
    u64 msg;
};

struct rtkit_buffer {
    void *bfr;
    u64 dva;
    size_t sz;
    bool owned;
#ifdef AURA_WINDOWS
    bool pooled;
#endif
};

rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, dart_dev_t *dart,
                        iova_domain_t *dart_iovad, sart_dev_t *sart, bool sram);
bool rtkit_quiesce(rtkit_dev_t *rtk);
bool rtkit_quiesce_ap(rtkit_dev_t *rtk);
bool rtkit_sleep(rtkit_dev_t *rtk);
void rtkit_stop(rtkit_dev_t *rtk);
void rtkit_free(rtkit_dev_t *rtk);
bool rtkit_has_owned_buffers(const rtkit_dev_t *rtk);

bool rtkit_start_ep(rtkit_dev_t *rtk, u8 ep);
bool rtkit_boot(rtkit_dev_t *rtk);
bool rtkit_boot_or_adopt(rtkit_dev_t *rtk, bool *adopted);
bool rtkit_boot_timed(rtkit_dev_t *rtk, u32 timeout_usec);
u32 rtkit_get_timeout(rtkit_dev_t *rtk);

bool rtkit_can_recv(rtkit_dev_t *rtk);

int rtkit_recv(rtkit_dev_t *rtk, struct rtkit_message *msg);
bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg);

bool rtkit_map(rtkit_dev_t *rtk, void *phys, size_t sz, u64 *dva);
bool rtkit_unmap(rtkit_dev_t *rtk, u64 dva, size_t sz);

bool rtkit_alloc_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr, size_t sz);
bool rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr);

#ifdef AURA_WINDOWS
enum rtkit_rejection_reason {
    RTKIT_REJECT_NONE,
    RTKIT_REJECT_ENDPOINT_RANGE,
    RTKIT_REJECT_SYSTEM_ENDPOINT,
    RTKIT_REJECT_MESSAGE,
    RTKIT_REJECT_CRASH,
};

struct rtkit_rejection {
    enum rtkit_rejection_reason reason;
    u64 msg0, msg1;
};

struct rtkit_notification_counts {
    u64 app, debug, oslog_type3;
};

/* Explicit pre-boot mode. Caller owns/stops the IOP before releasing storage. */
bool rtkit_set_phys_window(rtkit_dev_t *rtk, u64 base, size_t size);
bool rtkit_set_buffer_pool(rtkit_dev_t *rtk, u64 base, size_t size);
bool rtkit_set_early_ap_power(rtkit_dev_t *rtk, bool enable);
/* Single ASC owner; returns handled count, or a latched failure. Never waits. */
int rtkit_service_quiet(rtkit_dev_t *rtk, int max_msgs);
/* Sole owner, after service returns; copies metadata only, never reads MMIO. */
bool rtkit_get_first_rejection(const rtkit_dev_t *rtk, struct rtkit_rejection *out);
bool rtkit_get_notification_counts(const rtkit_dev_t *rtk, struct rtkit_notification_counts *out);
#endif

#endif
