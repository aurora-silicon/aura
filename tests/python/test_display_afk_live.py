# SPDX-License-Identifier: MIT
"""The live AFK endpoints a display hand-off reports, and dcp.afk-live= between stages.

Builds display.c's display_hands_on_live(), display_live_endpoints(), display_set_afk_live()
and display_afk_var() with their state against a host model of the first and second
display's DCP, each with an AFK context whose started endpoints are a given mask.
"""
from pathlib import Path
import re
import subprocess

SOURCE = Path(__file__).resolve().parents[2]


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


HARNESS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define T8140 0x8140
#define T8132 0x8132
#define T8152 0x8152
static u32 chip_id = T8152;
typedef struct { u32 live; } afk_epic_t;
typedef struct { afk_epic_t *afk; } dcp_dev_t;
typedef struct { const char *dcp; } display_config_t;
static u32 afk_epic_live_endpoints(const afk_epic_t *afk) { return afk ? afk->live : 0; }
static dcp_dev_t *dcp;
static const display_config_t *t8152_config;
static struct { dcp_dev_t *dcp; const display_config_t *cfg; } display_other;
'''

MAIN = r'''
int main(void)
{
    static const display_config_t hdmi = {"/arm-io/dcp"}, ext = {"/arm-io/dcpext0"};
    afk_epic_t a = {0x419}, b = {0x418};
    dcp_dev_t d = {&a}, e = {&b};

    /* Nothing started, nothing inherited: nothing to report or pass on. */
    assert(!display_live_endpoints("/arm-io/dcp") && !display_afk_var());

    /* This stage's sessions: the main DCP, and dcpext0 for the second display. */
    dcp = &d;
    t8152_config = &hdmi;
    display_other.dcp = &e;
    display_other.cfg = &ext;
    assert(display_live_endpoints("/arm-io/dcp") == 0x419);
    assert(display_live_endpoints("/arm-io/dcpext0") == 0x418);
    assert(!display_live_endpoints("/arm-io/dcpext1"));
    assert(!strcmp(display_afk_var(), "dcp.afk-live=0419,0418"));
    /* Handed on live here too: the mask decides. */
    chip_id = T8132;
    assert(!strcmp(display_afk_var(), "dcp.afk-live=0419,0418"));
    /* display_shutdown() stops these before the next stage runs: nothing is passed on. */
    chip_id = 0x8103;
    assert(!display_afk_var());
    chip_id = T8152;

    /* A later stage that has not adopted the earlier stage's display reports none of it. */
    dcp = NULL;
    display_other.dcp = NULL;
    display_other.cfg = NULL;
    assert(display_set_afk_live("0418,0000"));
    assert(!display_live_endpoints("/arm-io/dcp") && !display_afk_var());
    /* It adopted the framebuffer and has no session of its own. */
    display_afk_adopted = true;
    assert(display_live_endpoints("/arm-io/dcp") == 0x418);
    assert(!display_live_endpoints("/arm-io/dcpext0"));
    assert(!strcmp(display_afk_var(), "dcp.afk-live=0418,0000"));
    assert(display_set_afk_live("00Ab,FfFf"));
    assert(display_live_endpoints("/arm-io/dcp") == 0xab);
    assert(display_live_endpoints("/arm-io/dcpext0") == 0xffff);

    /* A malformed value is refused and changes nothing. */
    static const char *const bad[] = {"",          "418,0",      "0418",      "0418,",
                                      "0418;0000", "0418,0000 ", "0418,00000", "0x18,0000",
                                      "041g,0000"};
    for (size_t i = 0; i < ARRAY_SIZE(bad); i++)
        assert(!display_set_afk_live(bad[i]));
    assert(display_live_endpoints("/arm-io/dcp") == 0xab);
    assert(display_live_endpoints("/arm-io/dcpext0") == 0xffff);

    /* A session this stage starts replaces the earlier one on that controller only. */
    display_afk_restart("/arm-io/dcp");
    assert(!display_live_endpoints("/arm-io/dcp"));
    assert(display_live_endpoints("/arm-io/dcpext0") == 0xffff);
    assert(!strcmp(display_afk_var(), "dcp.afk-live=0000,ffff"));
    display_afk_restart("/arm-io/dcpext0");
    assert(!display_afk_var());
    puts("ok");
    return 0;
}
'''


def test_live_endpoints_are_reported_and_passed_on(tmp_path):
    disp = (SOURCE / "src/display.c").read_text()
    state = disp[disp.index("/* The controllers whose live AFK endpoints"):
                 disp.index("/* This stage starts a session of its own")]
    source = HARNESS + function(disp, "static bool display_hands_on_live(") + state + "".join(
        function(disp, signature) for signature in (
            "static void display_afk_restart(",
            "u32 display_live_endpoints(",
            "bool display_set_afk_live(",
            "char *display_afk_var(",
        )) + MAIN
    exe = tmp_path / "afk_live"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o",
                    str(exe)], input=source, text=True, check=True)
    assert subprocess.run([str(exe)], capture_output=True, text=True,
                          check=True).stdout == "ok\n"


KBOOT_HARNESS = r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)
static void *adt, *dt;
static u32 live_dcp, live_ext;
static bool ext_in_adt, ext_in_dt;
static int writes, written_node;
static u32 written;
static u32 display_live_endpoints(const char *path)
{
    return !strcmp(path, "/arm-io/dcp") ? live_dcp : !strcmp(path, "/arm-io/dcpext0") ? live_ext : 0;
}
static int adt_path_offset_trace(void *a, const char *path, int *trace)
{
    (void)a;
    trace[0] = !strcmp(path, "/arm-io/dcp") ? 1 : 2;
    return trace[0] == 2 && !ext_in_adt ? -1 : 0;
}
static int adt_get_reg(void *a, int *trace, const char *name, int index, u64 *reg, u64 *size)
{
    (void)a, (void)name, (void)index, (void)size;
    *reg = trace[0] == 1 ? 0x286e00000 : 0x32ae00000;
    return 0;
}
static int dt_t8152_dcp_node(u64 coproc)
{
    return coproc == 0x286e00000 ? 10 : ext_in_dt ? 20 : -1;
}
static int fdt_setprop_u32(void *fdt, int node, const char *name, u32 value)
{
    (void)fdt;
    assert(!strcmp(name, "apple,afk-active-endpoints"));
    writes++;
    written_node = node;
    written = value;
    return 0;
}
"""

KBOOT_MAIN = r"""
int main(void)
{
    /* dcpext0 bits on a Mac without dcpext0 in the ADT: logged, the main DCP still reported. */
    live_dcp = 0x418;
    live_ext = 0x10;
    assert(dt_set_live_afk_t8152() == 0 && writes == 1 && written_node == 10 && written == 0x418);
    /* In the ADT but without a device tree node: the same. */
    ext_in_adt = true;
    writes = 0;
    assert(dt_set_live_afk_t8152() == 0 && writes == 1 && written_node == 10);
    /* Both present: both reported. */
    ext_in_dt = true;
    writes = 0;
    assert(dt_set_live_afk_t8152() == 0 && writes == 2 && written_node == 20 && written == 0x10);
    /* Nothing live: nothing written. */
    live_dcp = live_ext = 0;
    writes = 0;
    assert(dt_set_live_afk_t8152() == 0 && !writes);
    puts("ok");
    return 0;
}
"""


def test_a_missing_controller_is_skipped_not_fatal(tmp_path):
    kboot = (SOURCE / "src/kboot.c").read_text()
    source = KBOOT_HARNESS + function(kboot, "static int dt_set_live_afk_t8152(") + KBOOT_MAIN
    exe = tmp_path / "afk_kboot"
    subprocess.run(["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o",
                    str(exe)], input=source, text=True, check=True)
    out = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout
    assert out.count("no /arm-io/dcpext0 in the ADT; its AFK endpoints 0x10 are not reported") == 1
    assert out.count("no node for /arm-io/dcpext0; its AFK endpoints 0x10 are not reported") == 1
    assert out.endswith("ok\n")
