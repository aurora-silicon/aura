# SPDX-License-Identifier: GPL-2.0-only
"""DEBUGUSB_PROXY: a debugusb boot with a DockChannel reader gets the aura-proxy window."""

import ctypes
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
SLACK = 256


def proxy_lib(tmp_path):
    library = tmp_path / "libstage1_proxy.dylib"
    subprocess.run(
        ["cc", "-shared", "-fPIC", "-DSTAGE1_PROXY_HOST_TEST",
         str(ROOT / "src/stage1_proxy.c"), "-o", str(library)],
        check=True,
    )
    lib = ctypes.CDLL(str(library))
    lib.stage1_proxy_dockchannel_host.argtypes = [
        ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint32]
    lib.stage1_proxy_dockchannel_host.restype = ctypes.c_bool
    return lib


def test_undrained_fifo_is_not_a_host(tmp_path):
    lib = proxy_lib(tmp_path)
    # Normal boot: the FIFO filled and stayed full.
    assert not lib.stage1_proxy_dockchannel_host(2048, 2048, 0, 0)
    # Partly filled, nothing read.
    assert not lib.stage1_proxy_dockchannel_host(2048, 1500, 548, 0)
    # iBoot left it full: nothing went in, nothing came out.
    assert not lib.stage1_proxy_dockchannel_host(0, 0, 0, 0)
    # Counting slop up to the margin is not a host.
    assert not lib.stage1_proxy_dockchannel_host(2048, 2048 + SLACK, 0, 0)
    assert not lib.stage1_proxy_dockchannel_host(2048, 100, 1948 + SLACK, 0)


def test_drained_fifo_or_host_bytes_are_a_host(tmp_path):
    lib = proxy_lib(tmp_path)
    # A reader took the boot log out: more went in than ever fit.
    assert lib.stage1_proxy_dockchannel_host(2048, 3685, 2048, 0)
    assert lib.stage1_proxy_dockchannel_host(2048, 2048 + SLACK + 1, 0, 0)
    # Less output than the FIFO holds, but the space came back.
    assert lib.stage1_proxy_dockchannel_host(4095, 1000, 4095, 0)
    # A reader drained what iBoot left behind.
    assert lib.stage1_proxy_dockchannel_host(0, 0, SLACK + 1, 0)
    # The host already sent a byte (a NOP from a catcher).
    assert lib.stage1_proxy_dockchannel_host(2048, 2048, 0, 1)
    # Large counters do not wrap.
    assert lib.stage1_proxy_dockchannel_host(0xffffffff, 1 << 40, 0, 0)
    assert not lib.stage1_proxy_dockchannel_host(0xffffffff, 0, 0xffffffff, 0)


DRIVER_HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned fifo_capacity, fifo_used, fifo_rx;
static unsigned long long host_reads;
static int host_reading;

u32 read32(u64 addr)
{
    switch (addr - uart_base) {
    case DATA_TX_FREE:
        return fifo_capacity - fifo_used;
    case DATA_RX_COUNT:
        return fifo_rx;
    }
    return 0;
}

void write32(u64 addr, u32 value)
{
    (void)value;
    assert(addr - uart_base == DATA_TX8);
    assert(fifo_used < fifo_capacity);
    fifo_used++;
}

/* The dock side: a host that reads everything, or nobody. */
static void host_poll(void)
{
    if (host_reading) {
        host_reads += fifo_used;
        fifo_used = 0;
    }
}

static void boot(unsigned capacity, unsigned left_by_iboot, int reading, unsigned log_bytes)
{
    static char line[100];
    memset(line, 'x', sizeof(line));
    uart_base = 0;
    tx_accepted = 0;
    tx_free_mask = ~0u;
    fifo_capacity = capacity;
    fifo_used = left_by_iboot;
    fifo_rx = 0;
    host_reading = reading;
    host_poll();
    assert(dockchannel_uart_init() == 0);
    for (unsigned sent = 0; sent < log_bytes; sent += sizeof(line)) {
        dockchannel_uart_iodev_write_nonblocking(NULL, line, sizeof(line));
        host_poll();
    }
}

int main(void)
{
    /* Normal boot, no reader: the console fills the FIFO and stays out. */
    boot(2048, 0, 0, 3700);
    assert(!dockchannel_uart_host_seen());
    /* Normal boot, iBoot left the FIFO full. */
    boot(2048, 2048, 0, 3700);
    assert(!dockchannel_uart_host_seen());
    /* Normal boot with less output than the FIFO holds. */
    boot(4095, 0, 0, 1000);
    assert(!dockchannel_uart_host_seen());
    /* VDM debugusb with kisd reading. */
    boot(2048, 0, 1, 3700);
    assert(dockchannel_uart_host_seen());
    boot(4095, 0, 1, 1000);
    assert(dockchannel_uart_host_seen());
    boot(2048, 2048, 1, 3700);
    assert(dockchannel_uart_host_seen());
    /* A catcher that sends NOPs before reading anything. */
    boot(2048, 0, 0, 3700);
    fifo_rx = 4;
    assert(dockchannel_uart_host_seen());
    /* The atomic startup frame and the blocking writer count too. */
    boot(2048, 0, 1, 0);
    for (int i = 0; i < 40; i++) {
        assert(dockchannel_uart_iodev_write_atomic(NULL, "0123456789", 10) == 10);
        dockchannel_uart_putbyte('x');
        host_poll();
    }
    assert(dockchannel_uart_host_seen());
    /* No DockChannel: never a host. */
    uart_base = 0;
    assert(!dockchannel_uart_host_seen());
    /* The ADT mask is applied to the free count. */
    boot(2048, 0, 0, 3700);
    assert(tx_free_mask == 0xfff);
    return 0;
}
'''

STUB_HEADERS = {
    "adt.h": r'''
#pragma once
#include "types.h"
#define T8140 0x8140
extern void *adt;
static inline int adt_path_offset_trace(const void *a, const char *p, int *o)
{ (void)a; (void)p; o[0] = 1; return 1; }
static inline int adt_get_reg(const void *a, int *p, const char *n, int i, u64 *addr, u64 *size)
{ (void)a; (void)p; (void)n; (void)i; (void)size; *addr = 0x23d128000ULL; return 0; }
static inline int adt_path_offset(const void *a, const char *p) { (void)a; (void)p; return -1; }
static inline int adt_getprop_mask(const char *name, void *val, size_t len)
{
    if (strcmp(name, "dock-wstat-mask") || len != 4)
        return -1;
    *(u32 *)val = 0xfff;
    return 4;
}
#define ADT_GETPROP(a, node, name, val) adt_getprop_mask(name, (val), sizeof(*(val)))
''',
    "types.h": r'''
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long u64;
typedef long s64;
''',
    "utils.h": r'''
#pragma once
#include "types.h"
#include <stdio.h>
u32 read32(u64 addr);
void write32(u64 addr, u32 value);
#define mrs(reg) ((u64)0)
#define UNUSED(x) ((void)(x))
#define min(a, b) ((a) < (b) ? (a) : (b))
''',
    "iodev.h": r'''
#pragma once
#include "types.h"
#define USAGE_CONSOLE 1
#define USAGE_UARTPROXY 2
#define SPINLOCK_INIT 0
struct iodev_ops {
    bool (*can_write)(void *);
    ssize_t (*can_read)(void *);
    ssize_t (*read)(void *, void *, size_t);
    ssize_t (*write)(void *, const void *, size_t);
    ssize_t (*write_nonblocking)(void *, const void *, size_t);
    ssize_t (*write_atomic)(void *, const void *, size_t);
};
struct iodev { struct iodev_ops *ops; int usage; int lock; };
''',
    "vsprintf.h": "#pragma once\n#include <stdio.h>\n",
}


def build_driver(tmp_path):
    src = tmp_path / "src"
    src.mkdir()
    (tmp_path / "build").mkdir()
    (tmp_path / "build/build_cfg.h").write_text("#define DEBUGUSB_PROXY\n")
    for name, text in STUB_HEADERS.items():
        (src / name).write_text(text)
    for name in ("dockchannel_uart.h", "stage1_proxy.h", "stage1_proxy.c"):
        (src / name).write_text((ROOT / "src" / name).read_text())
    driver = (ROOT / "src/dockchannel_uart.c").read_text()
    (src / "harness.c").write_text(
        "void *adt;\n" + driver + DRIVER_HARNESS)
    binary = tmp_path / "dockchannel-host"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-I", str(src),
         str(src / "harness.c"), str(src / "stage1_proxy.c"), "-o", str(binary)],
        check=True,
    )
    return binary


def test_driver_counts_every_byte_into_the_fifo(tmp_path):
    subprocess.run([str(build_driver(tmp_path))], check=True)


def test_driver_without_flag_has_no_accounting():
    driver = (ROOT / "src/dockchannel_uart.c").read_text()
    # Off by default: the counter and the check exist only under the build flag.
    assert re.search(r"#ifdef DEBUGUSB_PROXY\n/\* TX FIFO accounting", driver)
    assert re.search(r"#else\n#define TX_ACCEPTED\(n\) \(\(void\)0\)", driver)
    assert driver.rstrip().endswith("#endif")
    makefile = (ROOT / "Makefile").read_text()
    assert "ifeq ($(DEBUGUSB_PROXY),1)\nCFG += DEBUGUSB_PROXY\nendif" in makefile


def window_harness(tmp_path, extra=""):
    source = (ROOT / "src/main.c").read_text()
    start = source.index("/* Stage 1 runs at most one proxy window per boot")
    end = source.index("void run_actions(void)")
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define DEBUGUSB_PROXY
#define STAGE1_PROXY_REQUEST_MS 15000
enum stage1_proxy_result { STAGE1_PROXY_WAIT, STAGE1_PROXY_CDC, STAGE1_PROXY_KIS,
                           STAGE1_PROXY_TIMEOUT, STAGE1_PROXY_CDC_FAILED };
static struct { void *entry; } next_stage;
static bool stage1_chained, stage1_window_passed, host_seen;
static int waits, host_checks;
static enum stage1_proxy_result host_result = STAGE1_PROXY_TIMEOUT;
static bool dockchannel_uart_host_seen(void) { host_checks++; return host_seen; }
static enum stage1_proxy_result uartproxy_wait_stage1(unsigned ms)
{
    assert(ms == 15000);
    waits++;
    return host_result;
}
static void fb_set_active(bool on) { (void)on; }
static int uartproxy_run_presynced(void) { next_stage.entry = (void *)1; return 0; }
static int uartproxy_run(void *start) { (void)start; return 0; }
''' + source[start:end] + r'''
static void reset(void)
{
    stage1_window_done = false;
    stage1_chained = stage1_window_passed = host_seen = false;
    waits = host_checks = 0;
    host_result = STAGE1_PROXY_TIMEOUT;
    next_stage.entry = NULL;
}
int main(void)
{
    /* Normal boot: no reader, no window, no wait. */
    reset();
    assert(!debugusb_proxy_window() && waits == 0 && !stage1_window_offered());
    /* debugusb boot, nobody takes the window: one 15 s wait, then boot as normal. */
    reset();
    host_seen = true;
    assert(!debugusb_proxy_window() && waits == 1 && stage1_window_offered());
    /* Once per boot, and the ESP file adds no second window. */
    assert(!debugusb_proxy_window() && !stage1_proxy_request_window() && waits == 1);
    /* A host takes it: the proxy runs until it sets the next stage. */
    reset();
    host_seen = true;
    host_result = STAGE1_PROXY_KIS;
    assert(debugusb_proxy_window() && next_stage.entry && waits == 1);
    /* Stage 2 after a Stage 1 that offered a window, or that handed over a live ANS: none. */
    reset();
    host_seen = true;
    stage1_window_passed = true;
    assert(!debugusb_proxy_window() && waits == 0 && host_checks == 0);
    reset();
    host_seen = true;
    stage1_chained = true;
    assert(!debugusb_proxy_window() && waits == 0 && host_checks == 0);
    /* A config or boot-argument window already ran: none. */
    reset();
    host_seen = true;
    stage1_window_done = true;
    assert(!debugusb_proxy_window() && waits == 0);
    return 0;
}
'''
    binary = tmp_path / "debugusb-window"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    return subprocess.run([str(binary)], check=True, capture_output=True, text=True).stdout


def test_window_once_per_chain_and_logged(tmp_path):
    out = window_harness(tmp_path)
    assert out.count("aura: debugusb boot, opening proxy window\n") == 2
    assert out.count("Stage 1: waiting 15 s for a host\n") == 2
    assert "aura-proxy on the ESP" not in out


def test_stage1_tells_the_next_stage_about_its_window(tmp_path):
    source = (ROOT / "src/chainload.c").read_text()
    start = source.index("int chainload_load(")
    source = source[start:source.index("#else", start)]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define DEBUGUSB_PROXY
static struct { void *entry; } next_stage;
static char image[16];
static bool offered, stage1_chained;
static char *seen[4];
static size_t seen_count;
static bool nvme_init(void) { return true; }
static bool nvme_has_live_post_m4_session(void) { return false; }
static bool nvme_shutdown(void) { return true; }
static int rust_load_image(const char *spec, void **data, size_t *size, bool *proxy_request)
{
    if (proxy_request)
        *proxy_request = false;
    *data = image;
    *size = sizeof(image);
    return 0;
}
static int chainload_image(void *data, size_t size, char **vars, size_t count)
{
    seen_count = count;
    for (size_t i = 0; i < count; i++)
        seen[i] = vars[i];
    next_stage.entry = (void *)1;
    return 0;
}
static void rust_free_image(void *data, size_t size) {}
static void stage1_log_handoff(char **vars, size_t *var_cnt, size_t var_capacity) {}
static bool stage1_proxy_request_window(void) { return false; }
static bool stage1_window_offered(void) { return offered; }
''' + source + r'''
int main(void)
{
    char *vars[2];
    size_t count = 0;
    /* No window on this boot: the next stage may offer its own. */
    assert(chainload_load("test", vars, &count, 2) == 0 && count == 0 && seen_count == 0);
    /* A window was offered: pass it on. */
    offered = true;
    assert(chainload_load("test", vars, &count, 2) == 0 && count == 1 && seen_count == 1);
    assert(!strcmp(seen[0], "aura.host-window=done"));
    /* No room: boot anyway without it. */
    count = 2;
    vars[0] = vars[1] = "chosen.x=1";
    assert(chainload_load("test", vars, &count, 2) == 0 && count == 2);
    return 0;
}
'''
    binary = tmp_path / "chainload-window-var"
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_stage2_reads_the_window_var():
    payload = (ROOT / "src/payload.c").read_text()
    parse = re.search(
        r'#ifdef DEBUGUSB_PROXY\n    \} else if \(IS_VAR\("aura.host-window="\)\) \{\n'
        r'        stage1_window_passed = true;\n#endif', payload)
    assert parse
    # The window runs after the payload scan (which reads that var) and before any NVMe use.
    run = payload[payload.index("int payload_run(void)"):]
    hook = run.index("debugusb_proxy_window()")
    assert run.index("payload_scanned = true;") < hook
    assert hook < run.index("chainload_load(") and hook < run.index("boot_storage_load(")
