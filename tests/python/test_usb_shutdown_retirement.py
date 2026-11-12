# SPDX-License-Identifier: MIT
"""DWC3 shutdown retires active transfers before it releases their memory.

The production usb_dwc3_shutdown(), usb_dwc3_retire_transfer(),
usb_dwc3_handle_event(), usb_dwc3_handle_shutdown_event() and usb_dwc3_retain()
from usb_dwc3_impl.inc run against a modelled controller, once per engine.

Ways this can go wrong, each a case below:
- a buffer is unmapped or freed while an endpoint still has a transfer (no
  ENDTRANSFER, or one that is not waited for);
- ENDTRANSFER without the resource index, the interrupt on completion or the
  forced removal, so the controller never reports it done;
- the completion never comes, or the command is refused: the memory must stay
  retained (buffers and DART tables recorded), never freed;
- an endpoint whose transfer completed on its own while another one was retired
  gets a redundant ENDTRANSFER; a completion with a bus error must not count;
- the event drain during shutdown handles resets, EP0 or other traffic events;
- a session that already failed (and reset the controller) is retired again,
  or a device already retained is touched again.
"""
from pathlib import Path
import re
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|void|int) " + name + r"\([^;]*?\)\s*\{", source,
                      re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


@pytest.mark.parametrize("superspeed", [0, 1])
def test_shutdown_retires_transfers_first(tmp_path, superspeed):
    source = (ROOT / "src/usb_dwc3_impl.inc").read_text()
    timeout = re.search(r"#define USB_DWC3_RETIRE_TIMEOUT_US \d+", source).group(0)
    harness = f"#define USB_DWC3_SUPERSPEED {superspeed}\n" + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
#define SZ_16K 0x4000
#define max(a, b) ((a) > (b) ? (a) : (b))
#define usb_debug_printf(...) ((void)0)
#define CDC_ACM_PIPE_MAX 2
#define MAX_ENDPOINTS 6
#define DWC3_DEVTEN 0x1
#define DWC3_DALEPENA 0x2
#define DWC3_DCTL 0x3
#define DWC3_DSTS 0x4
#define DWC3_DCTL_RUN_STOP 1
#define DWC3_DCTL_CSFTRST 2
#define DWC3_DSTS_DEVCTRLHLT 4
#define DWC3_DEPCMD_ENDTRANSFER 0x08
#define DWC3_DEPCMD_CMDIOC (1 << 8)
#define DWC3_DEPCMD_HIPRI_FORCERM (1 << 11)
#define DWC3_DEPCMD_PARAM(x) ((x) << 16)
#define DWC3_DEPEVT_XFERCOMPLETE 0x01
#define DWC3_DEPEVT_XFERNOTREADY 0x03
#define DWC3_DEPEVT_EPCMDCMPLT 0x07
#define DEPEVT_STATUS_BUSERR 1
#define DWC3_EVENT_TYPE_DEV 0
#define DWC3_SCRATCHPAD_SIZE 0x1000
#define DWC3_EVENT_BUFFERS_SIZE 0x1000
#define TRB_BUFFER_IOVA 0x10000
#define TRB_BUFFER_SIZE 0x4000
#define XFER_BUFFER_IOVA 0x20000
#define XFER_BUFFER_SIZE 0x8000
#define SCRATCHPAD_IOVA 0x30000
#define EVENT_BUFFER_IOVA 0x40000
''' + timeout + r'''
struct dwc3_event_depevt {
    u32 one_bit : 1; u32 endpoint_number : 5; u32 endpoint_event : 4;
    u32 reserved11_10 : 2; u32 status : 4; u32 parameters : 16;
};
struct dwc3_event_type { u32 is_devspec : 1; u32 type : 7; u32 reserved8_31 : 24; };
union dwc3_event { u32 raw; struct dwc3_event_type type; struct dwc3_event_depevt depevt; int devt; };
typedef struct dart_dev dart_dev_t;
typedef struct ringbuffer ringbuffer_t;
typedef struct {
    u64 regs;
    dart_dev_t *dart;
    bool failed, dma_unsafe, shutting_down;
    void *evtbuffer, *scratchpad, *xferbuffer, *trbs;
    struct { bool ready; ringbuffer_t *device2host, *host2device; } pipe[CDC_ACM_PIPE_MAX];
    struct { bool xfer_in_progress, end_cmd_pending; u8 resource_index; } endpoints[MAX_ENDPOINTS];
} dwc3_dev_t;

/* The modelled controller: an event queue, ENDTRANSFER answers, a sequence log. */
static union dwc3_event queue[32];
static int nqueue;
static bool stuck[MAX_ENDPOINTS], refuse[MAX_ENDPOINTS];
static int self_complete = -1, self_complete_status;
static u32 issued[MAX_ENDPOINTS];
static int nissued, polls, unmaps, dart_shutdowns, frees, retained_dma, retained_dart, writes;
static int fail_halt, fail_reset;
static char seq[128];
static void log1(char c, int ep)
{
    size_t n = strlen(seq);
    seq[n] = c;
    seq[n + 1] = ep < 0 ? 0 : '0' + ep;
    seq[n + 2] = 0;
}
static void push_ep(int ep, int event, int status, int params)
{
    union dwc3_event e = {0};
    e.depevt.endpoint_number = ep;
    e.depevt.endpoint_event = event;
    e.depevt.status = status;
    e.depevt.parameters = params;
    queue[nqueue++] = e;
}
static void push_dev(void)
{
    union dwc3_event e = {0};
    e.type.is_devspec = 1;
    queue[nqueue++] = e;
}
static void write32(u64 a, u32 v) { (void)a; (void)v; writes++; }
static void set32(u64 a, u32 v) { (void)a; (void)v; writes++; }
static void clear32(u64 a, u32 v) { (void)a; (void)v; writes++; }
static int poll32(u64 a, u32 m, u32 v, unsigned us)
{
    (void)m; (void)v; (void)us;
    if (a == DWC3_DSTS) { log1('H', -1); return fail_halt ? -1 : 0; }
    if (a == DWC3_DCTL) { log1('R', -1); return fail_reset ? -1 : 0; }
    return 0;
}
static int usb_dwc3_ep_command(dwc3_dev_t *d, u8 ep, u32 c, u32 a, u32 b, u32 e)
{
    (void)d; (void)a; (void)b; (void)e;
    assert((c & 0xf) == DWC3_DEPCMD_ENDTRANSFER);
    issued[ep] = c;
    nissued++;
    log1('E', ep);
    if (refuse[ep])
        return -1;
    if (self_complete >= 0) {
        push_ep(self_complete, DWC3_DEPEVT_XFERCOMPLETE, self_complete_status, 0);
        self_complete = -1;
    }
    if (!stuck[ep])
        push_ep(ep, DWC3_DEPEVT_EPCMDCMPLT, 0, DWC3_DEPCMD_ENDTRANSFER << 8);
    return 0;
}
static u64 timeout_calculate(u32 us) { assert(us == USB_DWC3_RETIRE_TIMEOUT_US); polls = 0; return 0; }
static bool timeout_expired(u64 t) { (void)t; return ++polls > 50; }
/* Normal traffic handlers: never reached during shutdown. */
static void usb_dwc3_handle_event_ep(dwc3_dev_t *d, struct dwc3_event_depevt e) { (void)d; (void)e; abort(); }
static void usb_dwc3_handle_event_dev(dwc3_dev_t *d, int e) { (void)d; (void)e; abort(); }
static void usb_dwc3_handle_shutdown_event(dwc3_dev_t *dev, const union dwc3_event event);
static void usb_dwc3_handle_event(dwc3_dev_t *dev, const union dwc3_event event);
static void usb_dwc3_handle_events(dwc3_dev_t *dev)
{
    union dwc3_event batch[32];
    int n = nqueue;
    memcpy(batch, queue, sizeof(batch));
    nqueue = 0;
    for (int i = 0; i < n; i++) {
        if (!batch[i].type.is_devspec && batch[i].depevt.endpoint_event == DWC3_DEPEVT_EPCMDCMPLT)
            log1('c', batch[i].depevt.endpoint_number);
        usb_dwc3_handle_event(dev, batch[i]);
    }
}
static bool dart_unmap_checked(dart_dev_t *d, u64 iova, u64 len)
{ (void)d; (void)iova; (void)len; log1('U', -1); unmaps++; return true; }
static bool dart_shutdown_checked(dart_dev_t *d) { (void)d; dart_shutdowns++; return true; }
static bool usb_dwc3_retain_dma(u64 base, u64 size) { (void)size; assert(base); retained_dma++; return true; }
static bool usb_dwc3_retain_dart(dart_dev_t *d) { (void)d; retained_dart++; return true; }
static void ringbuffer_free(ringbuffer_t *r) { (void)r; }
static void mock_free(void *p) { (void)p; log1('F', -1); frees++; }
#define free mock_free
''' + function(source, "usb_dwc3_retire_transfer") \
    + function(source, "usb_dwc3_handle_shutdown_event") \
    + function(source, "usb_dwc3_handle_event") \
    + function(source, "usb_dwc3_retain") \
    + function(source, "usb_dwc3_shutdown") + r'''
#undef free
static char bufs[4][16];
static dwc3_dev_t *make(void)
{
    static dwc3_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.dart = (dart_dev_t *)&dev;
    dev.evtbuffer = bufs[0]; dev.scratchpad = bufs[1]; dev.xferbuffer = bufs[2]; dev.trbs = bufs[3];
    dev.endpoints[1].xfer_in_progress = true;
    dev.endpoints[1].resource_index = 5;
    dev.endpoints[3].xfer_in_progress = true;
    dev.endpoints[3].resource_index = 7;
    memset(stuck, 0, sizeof(stuck));
    memset(refuse, 0, sizeof(refuse));
    memset(issued, 0, sizeof(issued));
    self_complete = -1; self_complete_status = 0;
    nqueue = nissued = unmaps = dart_shutdowns = frees = retained_dma = retained_dart = writes = 0;
    fail_halt = fail_reset = 0;
    seq[0] = 0;
    return &dev;
}
static void kept(dwc3_dev_t *dev)
{
    assert(dev->dma_unsafe);
    assert(!unmaps && !dart_shutdowns && !frees);
    assert(retained_dma == 4 && retained_dart == 1);
}
int main(void)
{
    /* Both transfers retired, each waited for, before any unmap or free. */
    dwc3_dev_t *dev = make();
    assert(usb_dwc3_shutdown(dev));
    assert(!strncmp(seq, "E1c1E3c3HR", 10));
    assert(strchr(seq, 'U') > strstr(seq, "c3"));
    assert(issued[1] == (DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_HIPRI_FORCERM |
                         DWC3_DEPCMD_PARAM(5)));
    assert(issued[3] == (DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_HIPRI_FORCERM |
                         DWC3_DEPCMD_PARAM(7)));
    assert(unmaps == 4 && dart_shutdowns == 1 && frees == 5 && !retained_dma);

    /* EP 3 completes on its own while EP 1 is retired: no ENDTRANSFER for it. */
    dev = make();
    self_complete = 3;
    assert(usb_dwc3_shutdown(dev));
    assert(nissued == 1 && !strncmp(seq, "E1c1HR", 6));

    /* A completion with a bus error is not a retirement. */
    dev = make();
    self_complete = 3;
    self_complete_status = DEPEVT_STATUS_BUSERR;
    assert(usb_dwc3_shutdown(dev));
    assert(nissued == 2 && !strncmp(seq, "E1c1E3c3HR", 10));

    /* No completion: the memory is retained, the controller still stopped. */
    dev = make();
    stuck[1] = true;
    assert(!usb_dwc3_shutdown(dev));
    kept(dev);
    assert(nissued == 1 && !strcmp(seq, "E1HR"));

    /* The command itself refused: the same. */
    dev = make();
    refuse[3] = true;
    assert(!usb_dwc3_shutdown(dev));
    kept(dev);
    assert(!strcmp(seq, "E1c1E3HR"));

    /* Halt or reset failing after the retirement: retained too. */
    dev = make();
    fail_halt = 1;
    assert(!usb_dwc3_shutdown(dev));
    kept(dev);
    dev = make();
    fail_reset = 1;
    assert(!usb_dwc3_shutdown(dev));
    kept(dev);

    /* Resets, EP0 and traffic events in the drain are not handled (the handlers abort). */
    dev = make();
    push_dev();
    push_ep(0, DWC3_DEPEVT_XFERNOTREADY, 0, 0);
    push_ep(1, DWC3_DEPEVT_EPCMDCMPLT, 0, 0x02 << 8);
    assert(usb_dwc3_shutdown(dev));
    /* The first EP 1 command completion is for another command: it does not end the wait. */
    assert(!strncmp(seq, "E1c1c1E3c3HR", 12));

    /* A failed session already reset the controller: no ENDTRANSFER, release. */
    dev = make();
    dev->failed = true;
    assert(usb_dwc3_shutdown(dev));
    assert(!nissued && unmaps == 4 && frees == 5);

    /* A controller reset failed earlier: its memory is recorded, the controller not touched. */
    dev = make();
    dev->dma_unsafe = true;
    assert(!usb_dwc3_shutdown(dev));
    assert(!nissued && !writes);
    kept(dev);

    assert(usb_dwc3_shutdown(NULL));
    return 0;
}
'''
    binary = tmp_path / "usb-retire"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
