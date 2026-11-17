"""High-speed DWC3 session recovery: bus reset, disconnect and closed-pipe loops.

The controller model keeps its own record of which endpoints own a TRB. A
STARTTRANSFER on an endpoint the controller still owns fails, and a completion
for an endpoint the driver thinks is idle is dropped, as in usb_dwc3_impl.inc.
USB_HS_SESSION_SOURCE selects another copy of the source, e.g. the unfixed one.
"""
from pathlib import Path
import os
import re
import subprocess

REPO = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?[A-Za-z_][\w ]*?\b" + name + r"\([^;]*?\)\s*\{",
                      source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def macro(source, name):
    lines = [line for line in source.splitlines() if line.startswith("#define " + name + "(")]
    assert lines, name
    return lines[0]


PRELUDE = r'''#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define USB_DWC3_SUPERSPEED 0
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define MAX_ENDPOINTS 6
#define CDC_ACM_PIPE_MAX 2
#define XFER_BUFFER_BYTES_PER_EP 16
#define TRBS_PER_EP 2
#define USB_LEP_CTRL_OUT 0
#define USB_LEP_CTRL_IN 1
#define DWC3_DCFG 0
#define DWC3_DALEPENA 4
#define DWC3_DSTS 8
#define DWC3_DSTS_CONNECTSPD 7
#define DWC3_DSTS_HIGHSPEED 0
#define DWC3_DCFG_DEVADDR_MASK 127
#define DWC3_DCFG_DEVADDR(x) (x)
#define DWC3_DALEPENA_EP(x) (1U << (x))
#define T8122 0x8122
#define T8142 0x8142
#define T8152 0x8152
#define T6050 0x6050
enum {DWC3_DEVT_DISCONN, DWC3_DEVT_USBRST, DWC3_DEVT_CONNECTDONE, DWC3_DEVT_ULSTCHNG};
static const char *devt_names[] = {"DisconnEvt", "USBRst", "ConnectDone", "ULStChng"};
struct dwc3_event_devt {unsigned type;};
static void dbg(const char *fmt, ...) {(void)fmt;}
#define usb_debug_printf(...) dbg(__VA_ARGS__)
typedef int cdc_acm_pipe_id_t;
enum {USB_DWC3_EP0_STATE_IDLE, USB_DWC3_EP0_STATE_SETUP_HANDLE, USB_DWC3_EP0_STATE_DATA_SEND_DONE,
      USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE};
struct dwc3_trb {uint64_t a, b;};
typedef struct {size_t read, write;} ringbuffer_t;
struct endpoint {bool xfer_in_progress, zlp_pending, stalled; int resource_index, trb_count,
                 completed_trbs, transfer_length; unsigned char xfer_buffer[16]; struct dwc3_trb trb[2];};
typedef struct {unsigned regs; bool failed; int ep0_state; struct endpoint endpoints[MAX_ENDPOINTS];
                struct {bool ready; ringbuffer_t *host2device, *device2host; u8 ep_in, ep_out;} pipe[2];} dwc3_dev_t;

/* Controller model. */
static bool hw_owned[MAX_ENDPOINTS];
static unsigned address, enabled;
static int ends __attribute__((unused));
static int setup_starts, setup_failures, out_arms, in_kicks, events_polled;
static u32 chip_id = T8122;
static u64 now_ms;
static void mask32(unsigned reg, unsigned mask, unsigned value) {(void)mask; if (reg == DWC3_DCFG) address = value;}
static void write32(unsigned reg, unsigned value) {if (reg == DWC3_DALEPENA) enabled = value;}
static u32 read32(unsigned reg) {(void)reg; return DWC3_DSTS_HIGHSPEED;}
static int usb_dwc3_end_transfer(dwc3_dev_t *d, int ep) {(void)d; assert(address == 0 && enabled == 3); hw_owned[ep] = false; ends++; return 0;}
static void usb_dwc3_ep_set_stall(dwc3_dev_t *d, int ep, int value) {assert(!value); d->endpoints[ep].stalled = false;}
static int usb_dwc3_start_setup_phase(dwc3_dev_t *d) {
    setup_starts++;
    if (d->endpoints[0].xfer_in_progress || d->endpoints[1].xfer_in_progress) return -1;
    if (hw_owned[0]) {setup_failures++; return -1;} /* STARTTRANSFER on a busy endpoint */
    hw_owned[0] = true; d->endpoints[0].xfer_in_progress = true; return 0;
}
/* The driver's XFERCOMPLETE gate: "if (!xfer_in_progress) return;". */
static bool setup_completion_seen(dwc3_dev_t *d) {bool seen = hw_owned[0] && d->endpoints[0].xfer_in_progress; hw_owned[0] = false; return seen;}
static size_t ringbuffer_get_used(ringbuffer_t *r) {return r->write - r->read;}
static size_t ringbuffer_read(void *b, size_t n, ringbuffer_t *r) {size_t k = r->write - r->read; k = k < n ? k : n; memset(b, 0, k); r->read += k; return k;}
static u64 get_ticks(void) {return now_ms;}
static u64 ticks_to_msecs(u64 t) {return t;}
static int close_after = -1, stuck_ms = 0;
static dwc3_dev_t d;
static void usb_dwc3_handle_events(dwc3_dev_t *dev) {
    events_polled++; now_ms += stuck_ms;
    if (close_after >= 0 && events_polled >= close_after) dev->pipe[0].ready = false;
}
static void usb_dwc3_cdc_start_bulk_out_xfer(dwc3_dev_t *dev, u8 ep) {(void)dev; assert(ep == 4); out_arms++;}
static void usb_dwc3_cdc_start_bulk_in_xfer(dwc3_dev_t *dev, u8 ep) {(void)dev; assert(ep == 5); in_kicks++;}
'''

MAIN = r'''
static ringbuffer_t rb[4];
static void setup(void) {
    memset(&d, 0, sizeof(d)); memset(hw_owned, 0, sizeof(hw_owned));
    ends = setup_starts = setup_failures = out_arms = in_kicks = events_polled = 0;
    address = 29; enabled = 63; now_ms = 0; close_after = -1; stuck_ms = 0; chip_id = T8122;
    for (int i = 0; i < 2; i++) {
        d.pipe[i].ready = true; d.pipe[i].host2device = &rb[2 * i]; d.pipe[i].device2host = &rb[2 * i + 1];
        rb[2 * i] = (ringbuffer_t){0, 0}; rb[2 * i + 1] = (ringbuffer_t){0, 0};
    }
    d.pipe[0].ep_in = 5; d.pipe[0].ep_out = 4;
}
static void dev_event(unsigned type) {usb_dwc3_handle_event_dev(&d, (struct dwc3_event_devt){type});}
static void idle_with_setup_armed(void) {
    d.ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE; d.endpoints[0].xfer_in_progress = true; hw_owned[0] = true;
}
int main(void) {
    /* robur's USBDEVFS_RESET: idle device with SETUP armed, then RST + CONNECTDONE. */
    setup(); idle_with_setup_armed();
    dev_event(DWC3_DEVT_USBRST); dev_event(DWC3_DEVT_CONNECTDONE);
    assert(!setup_failures);
    assert(setup_completion_seen(&d)); /* SET_ADDRESS reaches the EP0 handler */

    /* Reset during a status phase: the old TRB is ended and SETUP re-armed. */
    setup(); d.ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE;
    d.endpoints[1].xfer_in_progress = true; hw_owned[1] = true;
    dev_event(DWC3_DEVT_USBRST); dev_event(DWC3_DEVT_CONNECTDONE);
    assert(!hw_owned[1] && !d.endpoints[1].xfer_in_progress && !setup_failures);
    assert(setup_completion_seen(&d));

    /* Repeated resets (port cycles, rebinds) keep EP0 serviceable. */
    setup(); idle_with_setup_armed(); d.endpoints[4].xfer_in_progress = true; hw_owned[4] = true;
    for (int i = 0; i < 5; i++) {dev_event(DWC3_DEVT_USBRST); dev_event(DWC3_DEVT_CONNECTDONE);}
    assert(!setup_failures && !hw_owned[4] && !d.endpoints[4].xfer_in_progress);
    assert(setup_completion_seen(&d));

    /* DISCONN retires the session: pipes closed, stale TX and RX dropped. */
    setup(); idle_with_setup_armed(); rb[0] = (ringbuffer_t){1, 9}; rb[1] = (ringbuffer_t){2, 8};
    d.endpoints[5].xfer_in_progress = true; hw_owned[5] = true;
    dev_event(DWC3_DEVT_DISCONN);
    assert(!d.pipe[0].ready && !d.pipe[1].ready && address == 0);
    assert(!ringbuffer_get_used(&rb[0]) && !ringbuffer_get_used(&rb[1]));
    assert(!hw_owned[5] && !d.endpoints[5].xfer_in_progress);
    dev_event(DWC3_DEVT_USBRST); dev_event(DWC3_DEVT_CONNECTDONE);
    assert(setup_completion_seen(&d));

    /* Flush stops when the host closes the pipe with TX still queued. */
    setup(); rb[1] = (ringbuffer_t){0, 100}; close_after = 3;
    usb_dwc3_flush(&d, 0);
    assert(events_polled == 3 && in_kicks == 3);
    /* An open pipe whose host never reads is bounded to 3 s on T8122. */
    setup(); rb[1] = (ringbuffer_t){0, 100}; stuck_ms = 10;
    usb_dwc3_flush(&d, 0);
    assert(now_ms >= 3000 && now_ms < 3100);

    /* A read in progress returns short when the pipe closes. */
    setup(); rb[0] = (ringbuffer_t){0, 4}; close_after = 2;
    unsigned char buf[64];
    assert(usb_dwc3_read(&d, 0, buf, sizeof(buf)) == 4);
    /* A closed pipe still reads nothing at entry. */
    setup(); d.pipe[0].ready = false; rb[0] = (ringbuffer_t){0, 4};
    assert(usb_dwc3_read(&d, 0, buf, sizeof(buf)) == 0 && !events_polled);

    /* The idle proxy loop keeps an OUT TRB armed on an open pipe only. */
    setup(); assert(usb_dwc3_can_read(&d, 0) == 0 && out_arms == 1);
    d.pipe[0].ready = false; assert(usb_dwc3_can_read(&d, 0) == 0 && out_arms == 1);

    puts("USB HS session: reset, disconnect and closed-pipe loops PASS");
    return 0;
}
'''


def build(tmp_path, source, name):
    parts = [macro(source, "CDC_LOOP_READY")] + [
        function(source, fn) for fn in (
            "usb_dwc3_handle_event_usbrst", "usb_dwc3_handle_event_connect_done",
            "usb_dwc3_handle_event_dev", "usb_dwc3_flush", "usb_dwc3_read", "usb_dwc3_can_read")]
    code = tmp_path / (name + ".c")
    code.write_text(PRELUDE + "\n".join(parts) + MAIN)
    binary = tmp_path / name
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-Wno-unused-parameter", str(code), "-o", str(binary)], check=True)
    return binary


def test_usb_hs_session(tmp_path):
    source = Path(os.environ.get("USB_HS_SESSION_SOURCE", REPO / "src/usb_dwc3_impl.inc")).read_text()
    subprocess.run([str(build(tmp_path, source, "usb-hs-session"))], check=True, timeout=20)
