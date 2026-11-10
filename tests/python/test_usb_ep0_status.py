# SPDX-License-Identifier: GPL-2.0-only OR MIT
"""DWC3 EP0: no-data requests start their status TRB only from XferNotReady.

Runs the production EP0 handlers of usb_dwc3_impl.inc, built as the high-speed and as the
SuperSpeed engine, for SET_CONTROL_LINE_STATE (DTR set and cleared, both ACM pipes) and
CLEAR_FEATURE(ENDPOINT_HALT), with every chip id set so that no chip-specific path can come back.
The controller model records whether it has reported XferNotReady for the status stage on an
endpoint; starting the status TRB before that is the race that left the J604's controller without
further events. On the SuperSpeed engine the primary pipe's open is reported once, after the
status stage completes. USB_EP0_STATUS_SOURCE selects another copy of the source, e.g. the
unfixed one.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

REPO = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^static [A-Za-z_][\w ]*?\b" + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def initializer(source, start):
    begin = source.index(start)
    return source[begin:source.index("};", begin) + 2]


PRELUDE = r'''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
#define PACKED __attribute__((packed))
#define MAX_ENDPOINTS 6
#define CDC_ACM_PIPE_0 0
#define CDC_ACM_PIPE_MAX 2
#define USB_LEP_CTRL_OUT 0
#define USB_LEP_CTRL_IN 1
#define min(a, b) ((a) < (b) ? (a) : (b))
#define T8103 0x8103
#define T8112 0x8112
#define T8122 0x8122
#define T8132 0x8132
#define T8142 0x8142
#define T8152 0x8152
#define T6000 0x6000
#define T6020 0x6020
static u32 chip_id;
static void dbg(const char *fmt, ...) {(void)fmt;}
#define usb_debug_printf(...) dbg(__VA_ARGS__)
struct dwc3_event_depevt {u32 one_bit : 1, endpoint_number : 5, endpoint_event : 4;};
'''

MODEL = r'''
typedef struct {
    int ep0_state;
    bool failed;
    const void *ep0_buffer;
    u32 ep0_buffer_len;
    void *ep0_read_buffer;
    u32 ep0_read_buffer_len;
    struct {void *xfer_buffer;} endpoints[MAX_ENDPOINTS];
    struct {bool ready; u8 cdc_line_coding[7];} pipe[CDC_ACM_PIPE_MAX];
#if USB_DWC3_SUPERSPEED
    bool ep0_three_stage, primary_dtr_pending, primary_was_open;
    struct usb_cdc_recovery {int connected;} recovery;
#endif
} dwc3_dev_t;
#if USB_DWC3_SUPERSPEED
static int primary_opened;
static void usb_cdc_recovery_connected(struct usb_cdc_recovery *r) {r->connected++;}
static void usb_cdc_primary_opened(void) {primary_opened++;}
#endif

/* Controller model: which endpoint has reported XferNotReady for its status stage. */
static bool status_not_ready[2];
static int status_starts[2], early_starts, setup_starts, stalls, closes, status_error;
static unsigned cleared_stall;
static int usb_dwc3_start_status_phase(dwc3_dev_t *dev, u8 ep) {
    (void)dev; assert(ep <= USB_LEP_CTRL_IN);
    if (!status_not_ready[ep]) early_starts++;
    status_starts[ep]++;
    return status_error;
}
static int usb_dwc3_start_setup_phase(dwc3_dev_t *dev) {(void)dev; setup_starts++; return 0;}
static int usb_dwc3_ep0_start_data_send_phase(dwc3_dev_t *dev) {(void)dev; return 0;}
static int usb_dwc3_ep0_start_data_recv_phase(dwc3_dev_t *dev) {(void)dev; return 0;}
static void usb_dwc3_ep_set_stall(dwc3_dev_t *dev, u8 ep, u8 stall) {
    (void)dev; if (stall) stalls++; else cleared_stall |= 1U << ep;
}
static void usb_dwc3_close_pipe(dwc3_dev_t *dev, int pipe) {dev->pipe[pipe].ready = false; closes++;}
static void usb_dwc3_ep0_handle_standard_device(dwc3_dev_t *dev, const union usb_setup_packet *setup)
{(void)dev; (void)setup; assert(!"no device request here");}
static void usb_dwc3_ep0_handle_standard_interface(dwc3_dev_t *dev, const union usb_setup_packet *setup)
{(void)dev; (void)setup; assert(!"no interface request here");}
'''

MAIN = r'''
static dwc3_dev_t d;
static u8 setup_buffer[64];
static const u32 chips[] = {T8103, T8112, T8122, T8132, T8142, T8152, T6000, T6020};
static void reset(u32 chip) {
    memset(&d, 0, sizeof(d)); memset(status_not_ready, 0, sizeof(status_not_ready));
    memset(status_starts, 0, sizeof(status_starts));
    early_starts = setup_starts = stalls = closes = status_error = 0; cleared_stall = 0;
    chip_id = chip; d.ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE; d.endpoints[0].xfer_buffer = setup_buffer;
    (void)chip_id; /* read by the source before the fix only */
#if USB_DWC3_SUPERSPEED
    primary_opened = 0;
#endif
}
/* What the SuperSpeed engine reports once the status stage of a DTR set on the primary pipe ends. */
static void primary_open(bool expected) {
#if USB_DWC3_SUPERSPEED
    assert(primary_opened == expected && d.primary_was_open == expected && d.recovery.connected == expected);
    assert(!d.primary_dtr_pending);
#else
    (void)expected;
#endif
}
static void deliver_setup(u8 type, u8 request, u16 value, u16 index) {
    union usb_setup_packet setup = {.raw = {.bmRequestType = type, .bRequest = request,
                                            .wValue = value, .wIndex = index, .wLength = 0}};
    memcpy(setup_buffer, &setup, sizeof(setup));
    usb_dwc3_ep0_handle_xfer_done(&d, (struct dwc3_event_depevt){.endpoint_number = 0});
}
/* The SETUP completion leaves the status stage to XferNotReady; XferComplete re-arms SETUP. */
static void status_stage(void) {
    assert(!status_starts[0] && !status_starts[1] && !setup_starts);
    assert(d.ep0_state == USB_DWC3_EP0_STATE_DATA_SEND_STATUS);
    status_not_ready[USB_LEP_CTRL_IN] = true;
    usb_dwc3_ep0_handle_xfer_not_ready(&d, (struct dwc3_event_depevt){.endpoint_number = 1});
    assert(status_starts[USB_LEP_CTRL_IN] == 1 && !status_starts[USB_LEP_CTRL_OUT] && !early_starts);
    assert(d.ep0_state == USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE);
#if USB_DWC3_SUPERSPEED
    assert(!primary_opened);
#endif
    usb_dwc3_ep0_handle_xfer_done(&d, (struct dwc3_event_depevt){.endpoint_number = 1});
    assert(setup_starts == 1 && d.ep0_state == USB_DWC3_EP0_STATE_SETUP_HANDLE && !stalls);
}
int main(void) {
    const u8 class_if = USB_REQUEST_TYPE_CLASS | USB_REQUEST_TYPE_RECIPIENT_INTERFACE;
    const u8 std_ep = USB_REQUEST_TYPE_STANDARD | USB_REQUEST_TYPE_RECIPIENT_ENDPOINT;
    for (unsigned c = 0; c < sizeof(chips) / sizeof(chips[0]); c++) {
        for (u16 pipe = 0; pipe < CDC_ACM_PIPE_MAX; pipe++) {
            /* SET_CONTROL_LINE_STATE, DTR set: the pipe opens, the status waits. */
            reset(chips[c]);
            deliver_setup(class_if, USB_REQUEST_CDC_SET_CTRL_LINE_STATE, 1, 2 * pipe);
            assert(closes == 1 && d.pipe[pipe].ready);
            status_stage();
            primary_open(pipe == CDC_ACM_PIPE_0);
            /* DTR cleared: the pipe closes, the status waits all the same. */
            reset(chips[c]); d.pipe[pipe].ready = true;
            deliver_setup(class_if, USB_REQUEST_CDC_SET_CTRL_LINE_STATE, 0, 2 * pipe);
            assert(closes == 1 && !d.pipe[pipe].ready);
            status_stage();
            primary_open(false);
        }
        /* CLEAR_FEATURE(ENDPOINT_HALT) on the bulk IN endpoint. */
        reset(chips[c]);
        deliver_setup(std_ep, USB_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, 0x82);
        assert(cleared_stall == 1U << ep_to_num(0x82));
        status_stage();
        primary_open(false);
        /* An endpoint out of range stalls EP0 and starts no status stage. */
        reset(chips[c]);
        deliver_setup(std_ep, USB_REQUEST_CLEAR_FEATURE, USB_FEATURE_ENDPOINT_HALT, 0x8f);
        assert(stalls == 1 && !cleared_stall && !status_starts[1] && d.ep0_state == USB_DWC3_EP0_STATE_IDLE);
        /* A status TRB that fails to start leaves EP0 idle for the next XferNotReady. */
        reset(chips[c]);
        deliver_setup(class_if, USB_REQUEST_CDC_SET_CTRL_LINE_STATE, 1, 0);
        status_not_ready[USB_LEP_CTRL_IN] = true; status_error = -1;
        usb_dwc3_ep0_handle_xfer_not_ready(&d, (struct dwc3_event_depevt){.endpoint_number = 1});
        assert(status_starts[1] == 1 && d.ep0_state == USB_DWC3_EP0_STATE_IDLE);
    }
    puts("USB EP0: no-data status stages wait for XferNotReady PASS");
    return 0;
}
'''


def harness(source, types, superspeed):
    parts = [initializer(source, "enum ep0_state {"), initializer(source, "static const char *ep0_state_names[]")]
    parts += [function(source, fn) for fn in (
        "ep_to_num", "usb_dwc3_ep0_handle_standard_endpoint", "usb_dwc3_ep0_handle_standard",
        "usb_dwc3_ep0_handle_class", "usb_dwc3_ep0_handle_setup", "usb_dwc3_ep0_handle_xfer_done",
        "usb_dwc3_ep0_handle_xfer_not_ready")]
    types = types.replace('#include "types.h"', "")
    return f"#define USB_DWC3_SUPERSPEED {superspeed}\n" + PRELUDE + types + "\n" + parts[0] + "\n" + MODEL + "\n".join(parts[1:]) + MAIN


def run(tmp_path, text):
    code, binary = tmp_path / "usb-ep0-status.c", tmp_path / "usb-ep0-status"
    code.write_text(text)
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-parameter", str(code), "-o", str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, timeout=20).returncode


@pytest.mark.parametrize("superspeed", [0, 1], ids=["high-speed", "superspeed"])
def test_usb_ep0_status(tmp_path, superspeed):
    source = Path(os.environ.get("USB_EP0_STATUS_SOURCE", REPO / "src/usb_dwc3_impl.inc")).read_text()
    types = (REPO / "src/usb_types.h").read_text()
    text = harness(source, types, superspeed)
    assert run(tmp_path, text) == 0
    # Controls: starting either status TRB from the SETUP completion must fail the run.
    early = "usb_dwc3_start_status_phase(dev, USB_LEP_CTRL_IN) ? USB_DWC3_EP0_STATE_IDLE : " \
            "USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE;"
    controls = 0
    for title, marker in [("line state", "            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;\n            break;\n\n        case USB_REQUEST_CDC_SET_LINE_CODING:"),
                          ("endpoint halt", "                    dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;\n                    break;\n                default:")]:
        if marker not in text:
            continue  # the unfixed source selected by USB_EP0_STATUS_SOURCE
        bad = text.replace(marker, marker.replace("USB_DWC3_EP0_STATE_DATA_SEND_STATUS;", early, 1), 1)
        assert bad != text, title
        assert run(tmp_path, bad) != 0, title
        controls += 1
    assert controls == 2 or "USB_EP0_STATUS_SOURCE" in os.environ
