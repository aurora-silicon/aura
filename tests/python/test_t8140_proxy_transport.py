# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the Neo proxy carrier from usb_init through the CDC watchdog.

A skipped fallback, invalid delay or refused schedule must not look like a
working proxy. Guard against leaving a schedule active without its watchdog,
or scheduling over a debug session or an existing carrier.
"""

import subprocess
from pathlib import Path

import pytest

from host_compiler import host_cc
from test_proxy_fallback import _body


ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "src"

STUBS = """
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc.h"
#include "usb_cdc_state.h"

#define BIT(n) (1u << (n))
#define USB_IODEV_COUNT 4

static u32 chip_id = T8140;
static u32 board_id;
static struct usb_cdc_state cdc;
static bool watchdog_armed;
static bool carrier_owned;
static bool usb_is_initialized;
static bool debug;
static bool watchdog_fails;
static unsigned watchdog_calls;
static u64 now_ms = 10;

static bool aura_debug_enabled(void) { return debug; }
static bool usb_cdc_deferred_usb2(void) { return false; }
static u64 get_ticks(void) { return now_ms; }
static u64 ticks_to_msecs(u64 ticks) { return ticks; }

static int wdt_arm_seconds(unsigned seconds)
{
    assert(seconds > 0);
    ++watchdog_calls;
    return watchdog_fails ? -1 : 0;
}

/* The Neo must take the deferred path before generic USB initialization. */
#define adt_path_offset(...) (abort(), 0)
#define tps6598x_foreach_hpm(...) abort()
#define usb_phy_bringup(...) abort()
"""

HARNESS = """
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *test = argv[1];

    debug = !strcmp(test, "debug");
    watchdog_fails = !strcmp(test, "watchdog-error");
    carrier_owned = !strcmp(test, "owned");
    if (!strcmp(test, "ready"))
        cdc.state = USB_CDC_DWC_READY;

    if (!strcmp(test, "zero-delay"))
        assert(usb_cdc_schedule(0, 0, BIT(1) | BIT(2)) == -1);
    else
        usb_init();

    u32 flags = cdc.state == USB_CDC_SCHEDULED ? cdc.flags : 0;
    printf("%u %u %u %u %u %u\\n", (unsigned)cdc.state, flags,
           watchdog_calls, (unsigned)watchdog_armed,
           (unsigned)usb_cdc_state_due(&cdc, now_ms + 999),
           (unsigned)usb_cdc_state_due(&cdc, now_ms + 1000));
}
"""


@pytest.fixture(scope="module")
def proxy_harness(tmp_path_factory):
    build = tmp_path_factory.mktemp("neo-proxy")
    source = build / "proxy.c"
    executable = build / "proxy"
    cdc = (SRC / "usb_cdc.c").read_text()
    usb = (SRC / "usb.c").read_text()
    functions = [
        _body(cdc, "static bool usb2_target(void)"),
        _body(cdc, "int usb_cdc_arm_watchdog(void)"),
        _body(cdc, "int usb_cdc_schedule("),
        _body(cdc, "bool usb_cdc_ready(void)"),
        _body(cdc, "bool usb_cdc_owns_carrier(void)"),
        _body(usb, "void usb_init(void)"),
    ]
    source.write_text("\n".join([STUBS, *functions, HARNESS]))
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Werror", "-DCDC_STATE_HOST_TEST",
         "-iquote", str(SRC), str(source), str(SRC / "usb_cdc_state.c"),
         "-o", str(executable)],
        check=True,
    )
    return executable


@pytest.mark.parametrize("case, expected, refused", [
    # State, active flags, watchdog calls, armed, due at 999 ms, due at 1000 ms.
    ("fallback",       (1, 6, 1, 1, 0, 1), False),
    ("zero-delay",     (0, 0, 0, 0, 0, 0), False),
    ("watchdog-error", (0, 0, 1, 0, 0, 0), True),
    ("debug",          (0, 0, 0, 0, 0, 0), False),
    ("owned",          (0, 0, 0, 0, 0, 0), False),
    ("ready",          (2, 0, 0, 0, 0, 0), False),
])
def test_neo_proxy_schedule(proxy_harness, case, expected, refused):
    result = subprocess.run([str(proxy_harness), case], check=True,
                            capture_output=True, text=True)
    *messages, state = result.stdout.splitlines()
    assert tuple(map(int, state.split())) == expected
    assert messages == (["usb: unable to schedule proxy transport"] if refused else [])
