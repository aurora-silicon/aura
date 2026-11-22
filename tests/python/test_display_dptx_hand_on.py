# SPDX-License-Identifier: MIT
"""display_shutdown() hands a DCP with a connected AFKv2 dptx-port on running.

Once such a port has been connected, its DCP acknowledges no power request (M2 Mac mini, 27.2
firmware), so display_shutdown() stops its endpoints and leaves it running instead of sleeping it.
Every other DCP keeps the sleep/quiesce it had. display_shutdown() and display_dptx_hands_on() are
taken from src/display.c; the DCP calls are stubs that record what was asked.
"""
from pathlib import Path
import os
import subprocess

SOURCE = Path(os.environ.get("DART_DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


def function(text, signature):
    start = text.index(signature)
    depth = 0
    for i in range(text.index("{", start), len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if not depth:
                return text[start:i + 1] + "\n"
    raise AssertionError("unterminated " + signature)


HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define T8112 0x8112
#define T8132 0x8132
#define T8140 0x8140
#define T8152 0x8152
typedef enum { DCP_QUIESCED, DCP_SLEEP_IF_EXTERNAL, DCP_SLEEP, DCP_HANDOFF } dcp_shutdown_mode;
typedef struct { bool afkv2; } dcp_dptx_if_t;
typedef struct { dcp_dptx_if_t *dptx_ep; bool endpoint_fails; } dcp_dev_t;
typedef struct { int unused; } dcp_iboot_if_t;
static unsigned chip_id = T8112;
static bool has_dcp = true, display_is_external = true, display_is_dptx;
static dcp_dev_t *dcp;
static dcp_iboot_if_t *iboot;
static bool running, handed_on;
static char calls[256];
static void call(const char *what) { strcat(calls, what); strcat(calls, " "); }
static bool display_hands_on_live(void) { return chip_id == T8140 || chip_id == T8132 || chip_id == T8152; }
static int display_handoff_t8152(void) { call("t8152"); return 0; }
static bool dcp_dptx_is_afkv2(const dcp_dptx_if_t *ep) { return ep && ep->afkv2; }
static int dcp_ib_shutdown(dcp_iboot_if_t *ib) { (void)ib; call("ib"); return 0; }
static int dcp_shutdown(dcp_dev_t *d, bool sleep) { (void)d; call(sleep ? "sleep" : "quiesce"); return 0; }
static int dcp_hand_on(dcp_dev_t *d)
{
    call("hand-on");
    running = true;
    handed_on = !d->endpoint_fails;
    return d->endpoint_fails ? -1 : 0;
}
static bool dcp_handed_on_live(void) { return handed_on; }
static bool dcp_left_running(void) { return running; }
"""

MAIN = r"""
static void reset(dcp_dev_t *d, dcp_iboot_if_t *ib, bool dptx, bool external)
{
    dcp = d;
    iboot = ib;
    display_is_dptx = dptx;
    display_is_external = external;
    running = handed_on = false;
    calls[0] = 0;
}
int main(void)
{
    dcp_dptx_if_t v2 = {true}, v1 = {false};
    dcp_dev_t d2 = {&v2, false}, d1 = {&v1, false}, internal = {NULL, false}, stuck = {&v2, true};
    dcp_iboot_if_t ib;

    /* AFKv2 dptx-port: iBoot interface stopped, then handed on running; no sleep. */
    reset(&d2, &ib, true, true);
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == 0);
    assert(!strcmp(calls, "ib hand-on ") && !iboot);
    /* The Linux boundary asks again: nothing more to do, and still no failure. */
    calls[0] = 0;
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == 0 && !calls[0]);

    /* The pre-AFKv2 format (13.5 firmware) keeps the sleep. */
    reset(&d1, &ib, true, true);
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == 0 && !strcmp(calls, "ib sleep "));

    /* An internal panel without a dptx-port keeps the quiesce. */
    reset(&internal, &ib, false, false);
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == 0 && !strcmp(calls, "ib quiesce "));

    /* An endpoint that does not stop fails the hand-on, then and at the next boundary. */
    reset(&stuck, &ib, true, true);
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == -1 && !strcmp(calls, "ib hand-on "));
    assert(display_shutdown(DCP_SLEEP_IF_EXTERNAL) == -1);
    puts("ok");
    return 0;
}
"""


def test_afkv2_dptx_dcp_is_handed_on_running(tmp_path):
    disp = (SOURCE / "src/display.c").read_text()
    source = HARNESS + "".join(function(disp, signature) for signature in (
        "static bool display_dptx_hands_on(",
        "int display_shutdown(dcp_shutdown_mode mode)",
    )) + MAIN
    exe = tmp_path / "hand-on"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-switch",
                    "-x", "c", "-", "-o", str(exe)], input=source, text=True, check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    assert out.count("handing the DCP on running") == 2
    assert out.endswith("ok\n")
