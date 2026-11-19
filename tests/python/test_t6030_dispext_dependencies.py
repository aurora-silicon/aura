# SPDX-License-Identifier: MIT
"""A display-DART request must first publish its firmware-memory prerequisite."""

from pathlib import Path
from test_siliconforest_handoff import run_c

ROOT = Path(__file__).resolve().parents[2]


def test_dispext_request_includes_firmware_memory(tmp_path):
    source = (ROOT / "src/kboot_display_handoff.c").read_text()

    def function(signature):
        start = source.index(signature)
        return source[start:source.index("\n}", start) + 2] + "\n"

    run_c(tmp_path, "t6030-external-dependencies", r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include "libfdt.h"
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
struct dcpext6_inst { bool optional; const char *alias; };
static const struct dcpext6_inst dcpext6_insts[] = {
    {false, "dcpext0"}, {true, "dcpext1"},
};
static unsigned char blob[8192];
static void *dt = blob;
static int reject_memory, calls[4], ncalls;

static bool dcpext6_present(void *tree, const struct dcpext6_inst *inst)
{
    return fdt_get_alias(tree, inst->alias) != NULL;
}
/* Reservation/validation boundaries: failed memory publication leaves no marker. */
static void dcpext6_handoff(void *tree, const struct dcpext6_inst *inst)
{
    int index = inst - dcpext6_insts;
    assert(ncalls < 4);
    calls[ncalls++] = 2 * index;
    if (index != reject_memory)
        assert(!fdt_setprop_u32(tree, fdt_path_offset(tree, inst->alias),
                               "memory-ready", 1));
}
static void dispext6_handoff(void *tree, const struct dcpext6_inst *inst)
{
    int index = inst - dcpext6_insts;
    assert(ncalls < 4);
    calls[ncalls++] = 2 * index + 1;
    int node = fdt_path_offset(tree, inst->alias);
    if (fdt_getprop(tree, node, "memory-ready", NULL))
        assert(!fdt_setprop_u32(tree, node, "display-ready", 1));
}
''' + function("static bool dcpext6_flag(") +
        function("static void dcpext6_handoffs(") + r'''
static void world(const char *memory, const char *display, bool ext1, int reject)
{
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    if (memory) assert(!fdt_setprop_string(dt, chosen, "aurora,t6030-dcpext", memory));
    if (display) assert(!fdt_setprop_string(dt, chosen, "aurora,t6030-dispext", display));
    assert(fdt_add_subnode(dt, 0, "dcpext0") >= 0);
    if (ext1) assert(fdt_add_subnode(dt, 0, "dcpext1") >= 0);
    int aliases = fdt_add_subnode(dt, 0, "aliases");
    assert(aliases >= 0);
    assert(!fdt_setprop_string(dt, aliases, "dcpext0", "/dcpext0"));
    if (ext1) assert(!fdt_setprop_string(dt, aliases, "dcpext1", "/dcpext1"));
    reject_memory = reject;
    ncalls = 0;
    dcpext6_handoffs(dt);
}
static bool has(const char *alias, const char *property)
{
    int node = fdt_path_offset(dt, alias);
    return node >= 0 && fdt_getprop(dt, node, property, NULL);
}
int main(void)
{
    world(NULL, NULL, true, -1);
    assert(ncalls == 0);
    world("0", "01", true, -1);
    assert(ncalls == 0);
    world("1", NULL, true, -1);
    assert(ncalls == 2 && calls[0] == 0 && calls[1] == 2);
    assert(has("dcpext0", "memory-ready") && has("dcpext1", "memory-ready"));
    assert(!has("dcpext0", "display-ready") && !has("dcpext1", "display-ready"));
    world(NULL, "1", true, -1); /* J514S DT: only the display flag is present. */
    assert(has("dcpext0", "display-ready") && has("dcpext1", "display-ready"));
    assert(ncalls == 4);
    for (int i = 0; i < 4; i++) assert(calls[i] == i);
    world("0", "1", false, -1);
    assert(ncalls == 2 && calls[0] == 0 && calls[1] == 1);
    assert(has("dcpext0", "display-ready") && !has("dcpext1", "display-ready"));
    world("1", "1", true, -1);
    assert(ncalls == 4 && has("dcpext1", "display-ready"));
    world(NULL, "1", true, 0);
    assert(!has("dcpext0", "memory-ready") && !has("dcpext0", "display-ready"));
    assert(has("dcpext1", "display-ready"));
    world(NULL, "1", true, 1);
    assert(has("dcpext0", "display-ready") && !has("dcpext1", "display-ready"));
    return 0;
}
''')
