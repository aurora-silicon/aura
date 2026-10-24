# SPDX-License-Identifier: MIT
"""T8103 display power cycle: iBoot setter replies and the display-status decision.

Builds one host unit from production sources (DISPLAY_RETRY_SOURCE/src): the
dcp_iboot.c structures and command bodies (dcp_ib_cmd and the setters/getters)
and display.c's display_configure() with its real parse/choose/T6030 helpers.
Only the AFK transport, DCP start/work, delays and the framebuffer-remap leaves
are host models; the remap leaves are unreachable in these cases.

The transport models what the J293 capture of cc02405 showed: an AFKEPV1 iBoot
endpoint whose setters answer with transport status alone (0 reply bytes) and
whose GET_HPD reports a connected panel. The timing-mode query is the first step
after the display-status decision; the fake records it and fails it, so a case
either reaches the modeset path or stops before it.

Direct command cases call the real dcp_ib_cmd() and setters with scripted
replies. An empty SET_POWER reply is the runtime observation from the J293
capture; empty SET_MODE and SET_SURFACE replies are admitted by the iBoot command
contract (no reply payload), not yet observed on hardware.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

SOURCE = Path(os.environ.get("DISPLAY_RETRY_SOURCE", Path(__file__).resolve().parents[2]))


def block(text, start):
    end, depth = text.index("{", start) + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def function(text, signature):
    match = re.search(r"^" + re.escape(signature), text, re.M)
    assert match, signature
    return block(text, match.start()) + "\n"


def define(text, name):
    match = re.search(r"^#define " + re.escape(name) + r"\b(?:.*\\\n)*.*$", text, re.M)
    assert match, name
    return match.group(0) + "\n"


def unit():
    ib = (SOURCE / "src/dcp_iboot.c").read_text()
    ibh = (SOURCE / "src/dcp_iboot.h").read_text()
    disp = (SOURCE / "src/display.c").read_text()
    types = ibh[ibh.index("enum DCPEOTF {"):ibh.index("dcp_iboot_if_t *dcp_ib_init")]
    structures = ib[ib.index("#define TXBUF_LEN"):ib.index("void dcp_ib_service_init")]
    commands = ib[ib.index("static int dcp_ib_cmd"):ib.index("int dcp_ib_swap_begin")]
    display = "".join((
        define(disp, "DISPLAY_STATUS_DELAY"), define(disp, "DISPLAY_STATUS_RETRIES"),
        define(disp, "COMPARE"), define(disp, "abs"),
        HOST_LEAVES,
        block(disp, disp.index("struct display_options {")) + ";\n",
        function(disp, "u64 display_mode_fb_size("),
        function(disp, "static void display_choose_timing_mode("),
        function(disp, "static void display_choose_color_mode("),
        function(disp, "static bool display_t6030_hands_off("),
        function(disp, "int display_parse_mode("),
        function(disp, "int display_configure("),
    ))
    return PREFIX + types + structures + TRANSPORT + commands + display + CASES


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
/* Match the freestanding aarch64-linux-gnu LP64 aliases. Darwin's native
 * uint64_t is unsigned long long, unlike the target's unsigned long. */
typedef unsigned long u64;
typedef long s64;
_Static_assert(sizeof(u64) == 8 && sizeof(s64) == 8, "target LP64 integer sizes");
#define PACKED __attribute__((packed))
#define ALIGN_UP(x, a) (((x) + ((a) - 1)) & ~((a) - 1))
#define SZ_16K (1 << 14)
#define SZ_32M (1 << 25)
#define T6030 0x6030
#define T8103 0x8103
#define T8152 0x8152
#define DART_PTR_ERR (1ULL << 63)
#define DART_IS_ERR(v) (((v) >> 63) & 1)
#define FB_DEPTH_MASK 0xff
#define FB_DEPTH_FLAG_RETINA 0x10000
static u32 chip_id = T8103;
typedef struct { void *dart_disp, *dart_dcp, *iovad_dcp, *dptx_ep; } dcp_dev_t;
typedef void afk_epic_ep_t;
typedef struct dcp_iboot_if dcp_iboot_if_t;
'''

TRANSPORT = r'''
/* Host AFKEPV1 transport. Setters reply with setter_bytes; GET_HPD follows hpd_script. */
static int setter_bytes, hpd_empty, hpd_script[32], hpd_calls, setter_calls, timing_requested;
/* Direct command cases: any operation replies with direct_bytes and header length
 * direct_len, and the transport returns direct_error. */
static bool direct;
static int direct_error;
static size_t direct_bytes, direct_len;
static bool afk_epic_is_v2(afk_epic_ep_t *epic) { (void)epic; return false; }
static int afk_epic_command(afk_epic_ep_t *epic, int channel, int type, void *tx, size_t txsize,
                            void *rx, size_t *rxsize)
{
    (void)epic; (void)channel; (void)txsize;
    assert(type == 0xc0 && rx && rxsize);
    u32 op = ((u32 *)tx)[0];
    u8 *reply = rx;
    memset(reply, 0, *rxsize);
    if (direct) {
        ((u32 *)reply)[0] = op;
        ((u32 *)reply)[1] = direct_len;
        *rxsize = direct_bytes;
        return direct_error;
    }
    if (op == 1 || op == 2 || op == 6) {
        setter_calls++;
        ((u32 *)reply)[0] = op;
        ((u32 *)reply)[1] = setter_bytes;
        *rxsize = setter_bytes;
        return 0;
    }
    if (op == 3) {
        int state = hpd_script[hpd_calls < 32 ? hpd_calls : 31];
        hpd_calls++;
        if (state < 0)
            return -5;
        if (hpd_empty) {
            *rxsize = 0;
            return 0;
        }
        ((u32 *)reply)[0] = op;
        ((u32 *)reply)[1] = 8 + 12;
        reply[8] = state;
        ((u32 *)(reply + 8))[1] = 1;
        ((u32 *)(reply + 8))[2] = 1;
        *rxsize = 20;
        return 0;
    }
    if (op == 4) {
        timing_requested++;
        return -5;
    }
    assert(!"unexpected iBoot operation");
    return -5;
}
'''

HOST_LEAVES = r'''
static dcp_dev_t dcp_storage, *dcp = &dcp_storage;
static struct dcp_iboot_if iboot_storage, *iboot = &iboot_storage;
static u64 fb_dva, fb_size;
static bool display_is_external, display_is_dptx, display_needs_power_cycle;
static struct { struct { u64 base, stride, width, height, depth; } video; } cur_boot_args;
static u64 boot_args_addr;
static void *adt;
static unsigned delays;
static int display_start_dcp(void) { return 0; }
static int dcp_connect_dptx(dcp_dev_t *d) { (void)d; assert(!"no DPTX on the internal panel"); return -1; }
static int dcp_dptx_hpd(void *dptx, u32 port, bool hpd) { (void)dptx; (void)port; (void)hpd; assert(!"no DPTX on the internal panel"); return -1; }
static int dcp_work(dcp_dev_t *d) { (void)d; return 0; }
static void mdelay(unsigned ms) { (void)ms; delays++; }
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_msecs(u64 t) { return t; }
/* Framebuffer remap leaves follow the timing query, which every case fails. */
static uintptr_t display_map_fb(uintptr_t iova, u64 pa, u64 size) { (void)iova; (void)pa; (void)size; assert(0); return 0; }
static int display_swap(u64 iova, u32 stride, u32 w, u32 h) { (void)iova; (void)stride; (void)w; (void)h; assert(0); return -1; }
static u64 top_of_memory_alloc(size_t s) { (void)s; assert(0); return 0; }
static u64 iova_alloc(void *d, size_t s) { (void)d; (void)s; assert(0); return 0; }
static void iova_free(void *d, u64 a, size_t s) { (void)d; (void)a; (void)s; assert(0); }
static void dart_unmap(void *d, u64 a, size_t s) { (void)d; (void)a; (void)s; assert(0); }
static void mmu_map_framebuffer(u64 a, u64 s) { (void)a; (void)s; assert(0); }
static int adt_path_offset(void *a, const char *p) { (void)a; (void)p; assert(0); return -1; }
static int adt_setprop(void *a, int n, const char *p, void *v, size_t s) { (void)a; (void)n; (void)p; (void)v; (void)s; assert(0); return -1; }
static void fb_reinit(void) { assert(0); }
'''

CASES = r'''
static int command(int op, size_t bytes, size_t len, int error)
{
    direct = true;
    direct_bytes = bytes;
    direct_len = len;
    direct_error = error;
    iboot_storage = (struct dcp_iboot_if){.channel = 7};
    int ret = dcp_ib_cmd(&iboot_storage, op, 0);
    direct = false;
    return ret;
}
static void setup(int setter_reply, int first, int rest)
{
    setter_bytes = setter_reply;
    hpd_empty = 0;
    hpd_calls = setter_calls = timing_requested = 0;
    delays = 0;
    hpd_script[0] = first;
    for (int i = 1; i < 32; i++)
        hpd_script[i] = rest;
    display_is_external = display_is_dptx = false;
    display_needs_power_cycle = true;
    iboot_storage = (struct dcp_iboot_if){.channel = 7};
}
int main(int argc, char **argv)
{
    assert(argc == 2);
    const char *c = argv[1];
    int ret;
    if (!strcmp(c, "observed-empty-setter-replies")) {
        /* The capture: both power-cycle setters answer with 0 bytes, panel connected. */
        setup(0, 1, 1);
        ret = display_configure(NULL);
        assert(setter_calls == 3 && hpd_calls == 1);
        assert(timing_requested == 1 && ret == -1);
    } else if (!strcmp(c, "transient-status-error")) {
        /* One failed status query, then a connected panel, with well-formed setter replies. */
        setup(8, -1, 1);
        ret = display_configure(NULL);
        assert(hpd_calls == 2 && timing_requested == 1 && ret == -1);
    } else if (!strcmp(c, "persistent-status-error")) {
        /* Every status query fails: no modeset, as before. */
        setup(8, -1, -1);
        ret = display_configure(NULL);
        assert(ret == 0 && hpd_calls == 20 && !timing_requested);
    } else if (!strcmp(c, "not-connected")) {
        setup(8, 0, 0);
        ret = display_configure(NULL);
        assert(ret == 0 && hpd_calls == 20 && !timing_requested);
    } else if (!strcmp(c, "malformed-setter-reply")) {
        /* A partial setter reply stays an error and never reaches the modeset. */
        setup(4, 1, 1);
        ret = display_configure(NULL);
        assert(ret <= 0 && !timing_requested);
    } else if (!strcmp(c, "empty-status-reply")) {
        /* Non-setters keep the full reply checks: an empty GET_HPD reply is not a status. */
        setup(8, 1, 1);
        hpd_empty = 1;
        ret = display_configure(NULL);
        assert(ret == 0 && hpd_calls == 20 && !timing_requested);
    } else if (!strcmp(c, "direct-empty-power-observed")) {
        /* Runtime observation: J293 AFKEPV1 SET_POWER answered with zero bytes. */
        direct = true; direct_bytes = 0; direct_len = 0; direct_error = 0;
        iboot_storage = (struct dcp_iboot_if){.channel = 7};
        assert(dcp_ib_set_power(&iboot_storage, false) == 0);
        assert(dcp_ib_set_power(&iboot_storage, true) == 0);
        direct = false;
    } else if (!strcmp(c, "direct-empty-mode-surface-contract")) {
        /* Contract-based: SET_MODE and SET_SURFACE carry no reply payload either. */
        dcp_timing_mode_t t = {.valid = 1, .width = 2560, .height = 1600, .fps = 60 << 16};
        dcp_color_mode_t m = {.valid = 1, .bpp = 30};
        dcp_layer_t layer = {.width = 2560, .height = 1600};
        direct = true; direct_bytes = 0; direct_len = 0; direct_error = 0;
        iboot_storage = (struct dcp_iboot_if){.channel = 7};
        assert(dcp_ib_set_mode(&iboot_storage, &t, &m) == 0);
        assert(dcp_ib_set_surface(&iboot_storage, &layer) == 0);
        direct = false;
    } else if (!strcmp(c, "direct-empty-nonsetters-refused")) {
        /* Getters and SWAP_BEGIN need their reply; an empty one is refused. */
        assert(command(IBOOT_GET_HPD, 0, 0, 0) < 0);
        assert(command(IBOOT_GET_TIMING_MODES, 0, 0, 0) < 0);
        assert(command(IBOOT_GET_COLOR_MODES, 0, 0, 0) < 0);
        assert(command(IBOOT_SWAP_BEGIN, 0, 0, 0) < 0);
    } else if (!strcmp(c, "direct-transport-error-propagated")) {
        /* A transport failure is returned as is, even with a zero-length reply. */
        assert(command(IBOOT_SET_POWER, 0, 0, -37) == -37);
        assert(command(IBOOT_SET_MODE, 0, 0, -37) == -37);
        assert(command(IBOOT_SET_SURFACE, 0, 0, -37) == -37);
        assert(command(IBOOT_GET_HPD, 0, 0, -37) == -37);
    } else if (!strcmp(c, "direct-malformed-setter-refused")) {
        /* A non-empty setter reply must still be a well-formed header. */
        static const int setters[] = {IBOOT_SET_POWER, IBOOT_SET_MODE, IBOOT_SET_SURFACE};
        for (unsigned i = 0; i < 3; i++) {
            assert(command(setters[i], 4, 4, 0) < 0);   /* shorter than the header */
            assert(command(setters[i], 8, 7, 0) < 0);   /* header length below the header */
            assert(command(setters[i], 8, 9, 0) < 0);   /* header length past the reply */
            assert(command(setters[i], 8, 8, 0) == 0);  /* well-formed empty payload */
        }
    } else if (!strcmp(c, "control")) {
        setup(8, 1, 1);
        ret = display_configure(NULL);
        assert(setter_calls == 3 && hpd_calls == 1 && timing_requested == 1 && ret == -1);
    } else {
        fprintf(stderr, "unknown case %s\n", c);
        return 2;
    }
    printf("PASS %s\n", c);
    return 0;
}
'''

CASES_LIST = ("observed-empty-setter-replies", "transient-status-error", "persistent-status-error",
              "not-connected", "malformed-setter-reply", "empty-status-reply", "control",
              "direct-empty-power-observed", "direct-empty-mode-surface-contract",
              "direct-empty-nonsetters-refused", "direct-transport-error-propagated",
              "direct-malformed-setter-refused")


@pytest.fixture(scope="module")
def binary(tmp_path_factory):
    work = tmp_path_factory.mktemp("iboot-power-status")
    code = work / "unit.c"
    code.write_text(unit())
    program = work / "unit"
    result = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                             "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined",
                             str(code), "-o", str(program)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    return program


@pytest.mark.parametrize("case", CASES_LIST)
def test_case(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and ("PASS " + case) in result.stdout, \
        result.stdout[-3000:] + result.stderr[-3000:]
