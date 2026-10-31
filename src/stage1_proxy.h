/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STAGE1_PROXY_H
#define STAGE1_PROXY_H

#ifdef STAGE1_PROXY_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#else
#include "types.h"
#endif

enum stage1_proxy_result {
    STAGE1_PROXY_WAIT,
    STAGE1_PROXY_CDC,
    STAGE1_PROXY_KIS,
    STAGE1_PROXY_TIMEOUT,
    STAGE1_PROXY_CDC_FAILED,
};

enum stage1_payload_action {
    STAGE1_PAYLOAD_NATIVE,
    STAGE1_PAYLOAD_CDC_FALLBACK,
    STAGE1_PAYLOAD_KIS_FALLBACK,
    STAGE1_PAYLOAD_HALT,
};

struct stage1_proxy_window {
    u32 timeout_ms;
    bool kis_only;
};

void stage1_proxy_window_init(struct stage1_proxy_window *window, u32 timeout_ms);
enum stage1_proxy_result stage1_proxy_window_step(struct stage1_proxy_window *window,
                                                  u32 elapsed_ms, bool cdc_ready, bool cdc_failed,
                                                  bool carrier_taken, bool cdc_sync, bool kis_sync);
enum stage1_payload_action stage1_proxy_payload_action(enum stage1_proxy_result window_result,
                                                       bool payload_valid, bool cdc_live,
                                                       bool kis_live);
bool stage1_proxy_action_needs_cleanup(enum stage1_payload_action action);

/*
 * Bytes a DockChannel host must have read beyond the FIFO accounting before a boot counts as a
 * debugusb boot. It absorbs counting slop; a host that reads the console reads kilobytes.
 */
#define STAGE1_PROXY_DOCKCHANNEL_SLACK 256
bool stage1_proxy_dockchannel_host(u32 tx_free_at_init, u64 tx_accepted, u32 tx_free_now,
                                   u32 rx_pending);

#endif
