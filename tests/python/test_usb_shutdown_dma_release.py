# SPDX-License-Identifier: MIT
"""DWC3 shutdown frees DMA buffers only after the controller and DART let go.

The production usb_dwc3_shutdown() from usb_dwc3_impl.inc runs against a
modelled controller and DART, once per engine (HighSpeed and SuperSpeed).
"""
from pathlib import Path
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def function(source, signature):
    start = source.index(signature)
    return source[start:source.index("\n}", source.index("{", start)) + 2] + "\n"


@pytest.mark.parametrize("superspeed", [0, 1])
def test_shutdown_releases_dma_only_when_confirmed(tmp_path, superspeed):
    source = (ROOT / "src/usb_dwc3_impl.inc").read_text()
    harness = f"#define USB_DWC3_SUPERSPEED {superspeed}\n" + r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
#define SZ_16K 0x4000
#define max(a, b) ((a) > (b) ? (a) : (b))
#define usb_debug_printf(...) ((void)0)
#define CDC_ACM_PIPE_MAX 2
#define MAX_ENDPOINTS 4
#define DWC3_DEVTEN 0x1
#define DWC3_DALEPENA 0x2
#define DWC3_DCTL 0x3
#define DWC3_DSTS 0x4
#define DWC3_DCTL_RUN_STOP 1
#define DWC3_DCTL_CSFTRST 2
#define DWC3_DSTS_DEVCTRLHLT 4
#define DWC3_DEPCMD_ENDTRANSFER 8
#define DWC3_SCRATCHPAD_SIZE 0x1000
#define DWC3_EVENT_BUFFERS_SIZE 0x1000
#define TRB_BUFFER_IOVA 0x10000
#define TRB_BUFFER_SIZE 0x4000
#define XFER_BUFFER_IOVA 0x20000
#define XFER_BUFFER_SIZE 0x8000
#define SCRATCHPAD_IOVA 0x30000
#define EVENT_BUFFER_IOVA 0x40000
typedef struct dart_dev dart_dev_t;
typedef struct ringbuffer ringbuffer_t;
typedef struct {
    uintptr_t regs;
    dart_dev_t *dart;
    bool failed, dma_unsafe, shutting_down;
    void *evtbuffer, *scratchpad, *xferbuffer, *trbs;
    struct { bool ready; ringbuffer_t *device2host, *host2device; } pipe[CDC_ACM_PIPE_MAX];
    struct { bool xfer_in_progress; } endpoints[MAX_ENDPOINTS];
} dwc3_dev_t;
static int fail_halt, fail_reset, fail_unmap, fail_dart_shutdown;
static int unmaps, dart_shutdowns, frees, ring_frees;
static void write32(u64 a, u32 v) { (void)a; (void)v; }
static void set32(u64 a, u32 v) { (void)a; (void)v; }
static void clear32(u64 a, u32 v) { (void)a; (void)v; }
static int poll32(u64 a, u32 m, u32 v, unsigned us)
{
    (void)m; (void)v; (void)us;
    if (a == DWC3_DSTS) return fail_halt ? -1 : 0;
    if (a == DWC3_DCTL) return fail_reset ? -1 : 0;
    return 0;
}
static int retires, retained;
static int usb_dwc3_retire_transfer(dwc3_dev_t *d, u8 ep)
{ d->endpoints[ep].xfer_in_progress = false; retires++; return 0; }
static bool usb_dwc3_retain_dma(uintptr_t base, size_t size) { (void)base; (void)size; retained++; return true; }
static bool usb_dwc3_retain_dart(dart_dev_t *d) { (void)d; retained++; return true; }
static bool dart_unmap_checked(dart_dev_t *d, uintptr_t iova, size_t len)
{
    (void)d; (void)iova; (void)len;
    return ++unmaps != fail_unmap;
}
static bool dart_shutdown_checked(dart_dev_t *d) { (void)d; dart_shutdowns++; return !fail_dart_shutdown; }
static void ringbuffer_free(ringbuffer_t *r) { (void)r; ring_frees++; }
static void mock_free(void *p) { (void)p; frees++; }
#define free mock_free
'''
    harness += function(source, "static void usb_dwc3_retain(")
    harness += function(source, "bool usb_dwc3_shutdown(")
    harness += r'''
#undef free
static dwc3_dev_t *make(void)
{
    static dwc3_dev_t dev;
    memset(&dev, 0, sizeof(dev));
    dev.regs = 0;
    dev.dart = (dart_dev_t *)&dev;
    dev.endpoints[1].xfer_in_progress = true;
    fail_halt = fail_reset = fail_unmap = fail_dart_shutdown = 0;
    unmaps = dart_shutdowns = frees = ring_frees = retires = retained = 0;
    return &dev;
}
int main(void)
{
    dwc3_dev_t *dev = make();
    assert(usb_dwc3_shutdown(dev));
    assert(retires == 1 && unmaps == 4 && dart_shutdowns == 1 && frees == 5 && ring_frees == 4);
    assert(!retained);

    for (int i = 1; i <= 4; i++) {
        dev = make();
        fail_unmap = i;
        assert(!usb_dwc3_shutdown(dev));
        assert(dev->dma_unsafe && unmaps == i && !dart_shutdowns && !frees && !ring_frees);
        assert(retained == 5);
    }

    dev = make();
    fail_dart_shutdown = 1;
    assert(!usb_dwc3_shutdown(dev));
    assert(dev->dma_unsafe && !frees && !ring_frees && retained == 5);

    dev = make();
    fail_halt = 1;
    assert(!usb_dwc3_shutdown(dev));
    assert(dev->dma_unsafe && !unmaps && !frees && retained == 5);

    dev = make();
    fail_reset = 1;
    assert(!usb_dwc3_shutdown(dev));
    assert(dev->dma_unsafe && !unmaps && !frees && retained == 5);
    return 0;
}
'''
    binary = tmp_path / "usb-shutdown"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
                    "-x", "c", "-", "-o", str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True, timeout=5)
