# SPDX-License-Identifier: MIT
"""T8140 and T8132 never reach the display DART view.

display_init() is compiled verbatim from src/display.c with host stubs for
everything it calls. On T8140 and T8132 it must return before the dummy
framebuffer path, so neither display_configure() nor any DCP or DART work runs,
even with the 640x1136 framebuffer that makes every other chip start the DCP.
display_dart_tables_inherited() must then answer false without looking at a
DART. A T6000 run is the positive control.
"""
from pathlib import Path
import os
import re
import subprocess

SOURCE = Path(os.environ.get("DART_DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


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
typedef unsigned long u64; /* as in aura: printed as long */
#define UNUSED(x) (void)(x)
#define FB_DEPTH_FLAG_RETINA 0x10000
%(chips)s
enum fw_version { V15_0B1 = 1, V_UNKNOWN = 2 };
#define FW_MAX V_UNKNOWN
typedef struct { const char *dcp, *dcp_dart, *disp_dart; } display_config_t;

static u32 chip_id, board_id;
static bool is_mac = true;
static bool display_is_external, display_needs_power_cycle;
static struct { struct { u64 base, width, height, depth; } video; } cur_boot_args;
static struct { enum fw_version version; } os_firmware = {V15_0B1};
static int dummy_adt;
static void *adt = &dummy_adt;
static unsigned configures, t8152_inits, dart_queries, config_queries, adt_queries;

static bool adt_is_compatible(void *tree, int node, const char *compat)
{
    (void)tree; (void)node; (void)compat;
    adt_queries++;
    return false;
}
static int adt_path_offset(void *tree, const char *path) { (void)tree; (void)path; adt_queries++; return 1; }
static int adt_getprop_copy(void *tree, int node, const char *name, void *out, size_t len)
{
    (void)tree; (void)node; (void)name;
    memset(out, 0, len);
    return -1;
}
#define ADT_GETPROP(adt, node, name, val) adt_getprop_copy(adt, node, name, (val), sizeof(*(val)))
static const void *adt_getprop(void *tree, int node, const char *name, u32 *len)
{
    (void)tree; (void)node; (void)name; (void)len;
    return NULL;
}
static int display_configure(const char *config) { (void)config; configures++; return 0; }
static int display_init_t8152(void) { t8152_inits++; return 0; }
static void fb_clear_direct(void) {}
static bool firmware_sfw_in_range(enum fw_version lo, enum fw_version hi) { (void)lo; (void)hi; return false; }
static const display_config_t m1 = {"/arm-io/dcp", "/arm-io/dart-dcp", "/arm-io/dart-disp0"};
static const display_config_t *display_get_config(void) { config_queries++; return &m1; }
static bool dart_locked = true;
static bool dart_adt_locked(const char *path, int instance) { (void)path; (void)instance; dart_queries++; return dart_locked; }

%(display_init)s

%(inherited)s

static void boot(u32 chip)
{
    chip_id = chip;
    cur_boot_args.video.base = 0x10000000;
    cur_boot_args.video.width = 640;
    cur_boot_args.video.height = 1136;
    cur_boot_args.video.depth = 30;
    configures = t8152_inits = dart_queries = config_queries = adt_queries = 0;
}

int main(void)
{
    static const u32 inherited_only[] = {T8140, T8132};
    for (unsigned i = 0; i < 2; i++) {
        boot(inherited_only[i]);
        assert(display_init() == 0);
        assert(!configures && !t8152_inits);
        assert(!display_dart_tables_inherited());
        assert(!dart_queries && !config_queries);
    }
    /* Positive control: the same dummy framebuffer starts the DCP on T6000,
     * and the inherited-table question goes to its display DARTs. */
    boot(T6000);
    assert(display_init() == 0 && configures == 1);
    assert(display_dart_tables_inherited() && dart_queries == 1 && config_queries == 1);
    dart_locked = false;
    dart_queries = 0;
    assert(!display_dart_tables_inherited() && dart_queries == 2);
    puts("PASS inherited-only display chips");
    return 0;
}
'''


def test_t8140_t8132_return_before_the_dart_path(tmp_path):
    display = (SOURCE / "src/display.c").read_text()
    soc = (SOURCE / "src/soc.h").read_text()
    chips = "\n".join(re.findall(r"^#define (?:T8140|T8132|T8152|T6000|T6040|T6041|S5L8960X)\s+0x[0-9a-fA-F]+\s*$",
                                 soc, re.M))
    panel = function(display, "static bool display_internal_panel_absent(")
    init = function(display, "int display_init(void)")
    inherited = function(display, "bool display_dart_tables_inherited(void)")
    # The early return the PR cites, ahead of the dummy framebuffer path.
    early = init.index("if (chip_id == T8140 || chip_id == T8132 ||")
    assert early < init.index("Dummy framebuffer found") and "return 0;" in init[early:init.index("}", early)]
    line = display[:display.index(init)].count("\n") + init[:early].count("\n") + 1
    print(f"display_init() returns early for T8140/T8132 at src/display.c:{line}")
    code = tmp_path / "inherited-only.c"
    code.write_text(HARNESS % {"chips": chips, "display_init": panel + "\n" + init,
                               "inherited": inherited})
    binary = tmp_path / "inherited-only"
    compiled = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-Wall", "-Wextra",
                               "-Werror", "-Wno-unused-function", str(code), "-o",
                               str(binary)], capture_output=True, text=True)
    assert compiled.returncode == 0, compiled.stdout + compiled.stderr
    result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "PASS inherited-only display chips" in result.stdout
