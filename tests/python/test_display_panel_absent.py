# SPDX-License-Identifier: MIT
"""A Mac with no internal panel skips the display status wait.

display_init() and display_internal_panel_absent() are compiled verbatim from src/display.c with
an ADT stub. The panel-less M1 Pro (J316s) has /arm-io/disp0 dot-pitch = 0 and no /product
raw-panel-serial-number; its DCP never reports a display and display_configure() waited the full
2 s. The M2 (J413) has dot-pitch 224 and a panel serial. Only both signs together skip the
modeset, an external display is never skipped, and anything unexpected keeps the old path.
"""
from pathlib import Path
import os
import re
import subprocess

SOURCE = Path(os.environ.get("DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


def function(text, signature):
    start = text.index(signature)
    end, depth = text.index("{", start) + 1, 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef unsigned long u64;
#define UNUSED(x) (void)(x)
#define FB_DEPTH_FLAG_RETINA 0x10000
%(chips)s
enum fw_version { V15_0B1 = 1, V_UNKNOWN = 2 };
#define FW_MAX V_UNKNOWN

static u32 chip_id, board_id;
static bool is_mac = true;
static bool display_is_external, display_needs_power_cycle;
static struct { struct { u64 base, width, height, depth; } video; } cur_boot_args;
static struct { enum fw_version version; } os_firmware = {V15_0B1};
static int dummy_adt;
static void *adt = &dummy_adt;
static unsigned configures;

enum { NODE_PRODUCT = 1, NODE_DISP0 = 2 };
static bool has_product, has_dot_pitch, has_serial, external;
static u32 dot_pitch, dot_pitch_len;
static const char serial[] = "panel";

static bool adt_is_compatible(void *tree, int node, const char *compat)
{
    (void)tree; (void)node; (void)compat;
    return false;
}
static int adt_path_offset(void *tree, const char *path)
{
    (void)tree;
    if (!strcmp(path, "/product"))
        return has_product ? NODE_PRODUCT : -1;
    if (!strcmp(path, "/arm-io/disp0"))
        return NODE_DISP0;
    return -1;
}
static int adt_getprop_copy(void *tree, int node, const char *name, void *out, size_t len)
{
    (void)tree; (void)node; (void)name;
    memset(out, 0, len);
    return -1;
}
#define ADT_GETPROP(adt, node, name, val) adt_getprop_copy(adt, node, name, (val), sizeof(*(val)))
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree;
    if (node == NODE_DISP0 && !strcmp(name, "external"))
        return external ? (const void *)serial : NULL;
    if (node == NODE_DISP0 && !strcmp(name, "dot-pitch") && has_dot_pitch) {
        if (len)
            *len = dot_pitch_len;
        return &dot_pitch;
    }
    if (node == NODE_PRODUCT && !strcmp(name, "raw-panel-serial-number") && has_serial) {
        if (len)
            *len = sizeof(serial);
        return serial;
    }
    return NULL;
}
static int display_configure(const char *config) { (void)config; configures++; return 0; }
static int display_init_t8152(void) { return 0; }
static void fb_clear_direct(void) {}
static bool firmware_sfw_in_range(enum fw_version lo, enum fw_version hi) { (void)lo; (void)hi; return false; }

%(panel)s

%(display_init)s

/* iBoot's dummy framebuffer, which made display_init() start the DCP and wait for a panel. */
static unsigned boot(bool product, bool pitch, u32 pitch_value, u32 pitch_len, bool panel_serial,
                     bool is_external)
{
    chip_id = T6000;
    cur_boot_args.video.base = 0x10000000;
    cur_boot_args.video.width = 640;
    cur_boot_args.video.height = 1136;
    cur_boot_args.video.depth = 32;
    has_product = product;
    has_dot_pitch = pitch;
    dot_pitch = pitch_value;
    dot_pitch_len = pitch_len;
    has_serial = panel_serial;
    external = is_external;
    configures = 0;
    assert(display_init() == 0);
    return configures;
}

int main(void)
{
    /* The panel-less M1 Pro: no modeset, so no 2 s status wait. */
    assert(boot(true, true, 0, 4, false, false) == 0);
    /* The M2 with its panel, and every half-sign, keep the modeset. */
    assert(boot(true, true, 224, 4, true, false) == 1);
    assert(boot(true, true, 224, 4, false, false) == 1);
    assert(boot(true, true, 0, 4, true, false) == 1);
    /* No dot-pitch, a malformed one, or no /product node: assume a panel. */
    assert(boot(true, false, 0, 4, false, false) == 1);
    assert(boot(true, true, 0, 8, false, false) == 1);
    assert(boot(false, true, 0, 4, false, false) == 1);
    /* An external display node is configured whatever its dot-pitch says. */
    assert(boot(true, true, 0, 4, false, true) == 1);
    /* A firmware framebuffer on a Mac that has its panel is left alone, as before. */
    chip_id = T6000;
    cur_boot_args.video.width = 3024;
    cur_boot_args.video.height = 1964;
    cur_boot_args.video.depth = 30 | FB_DEPTH_FLAG_RETINA;
    has_product = has_dot_pitch = has_serial = true;
    dot_pitch = 254;
    dot_pitch_len = 4;
    external = false;
    configures = 0;
    assert(display_init() == 0 && configures == 0);
    puts("PASS panel-less display skip");
    return 0;
}
'''


def test_panel_less_mac_skips_the_status_wait(tmp_path):
    display = (SOURCE / "src/display.c").read_text()
    soc = (SOURCE / "src/soc.h").read_text()
    chips = "\n".join(re.findall(r"^#define (?:T8140|T8132|T8152|T6000|T6040|S5L8960X)\s+0x[0-9a-fA-F]+\s*$",
                                 soc, re.M))
    panel = function(display, "static bool display_internal_panel_absent(")
    init = function(display, "int display_init(void)")
    # The skip comes after the T8140/T8132 early return and before any modeset path.
    skip = init.index("display_internal_panel_absent(node, product)")
    assert init.index("if (chip_id == T8140 || chip_id == T8132 ||") < skip
    assert skip < init.index("Dummy framebuffer found") < init.index("External display found")
    code = tmp_path / "panel-absent.c"
    code.write_text(HARNESS % {"chips": chips, "panel": panel, "display_init": init})
    binary = tmp_path / "panel-absent"
    compiled = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra",
                               "-Werror", "-Wno-unused-function", str(code), "-o", str(binary)],
                              capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS panel-less display skip" in result.stdout
