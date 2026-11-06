/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef __UARTPROXY_H__
#define __UARTPROXY_H__

#include "iodev.h"
#include "stage1_proxy.h"

extern iodev_id_t uartproxy_iodev;

typedef enum _uartproxy_start_reason_t {
    START_BOOT,
    START_EXCEPTION,
    START_EXCEPTION_LOWER,
    START_HV,
} uartproxy_boot_reason_t;

typedef enum _uartproxy_exc_code_t {
    EXC_SYNC,
    EXC_IRQ,
    EXC_FIQ,
    EXC_SERROR,
} uartproxy_exc_code_t;

typedef enum _uartproxy_exc_ret_t {
    EXC_RET_UNHANDLED = 1,
    EXC_RET_HANDLED = 2,
    EXC_EXIT_GUEST = 3,
} uartproxy_exc_ret_t;

typedef enum _uartproxy_event_type_t {
    EVT_MMIOTRACE = 1,
    EVT_IRQTRACE = 2,
    EVT_WINDOWS_SERIAL = 0x80,
} uartproxy_event_type_t;

struct uartproxy_msg_start {
    u32 reason;
    u32 code;
    void *info;
    void *reserved;
};

int uartproxy_run(struct uartproxy_msg_start *start);
bool uartproxy_wait_dockchannel(unsigned int timeout_ms);
enum stage1_proxy_result uartproxy_wait_stage1(unsigned int timeout_ms);
int uartproxy_run_presynced(void);
void uartproxy_send_event(u16 event_type, void *data, u16 length);
#ifdef AURA_WINDOWS
/* One complete frame, or no bytes. No blocking/partial-write fallback. */
bool uartproxy_try_send_event(u16 event_type, const void *data, u16 length);
#endif

#endif
