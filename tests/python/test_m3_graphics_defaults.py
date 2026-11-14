# SPDX-License-Identifier: MIT
"""M3 graphics on by default: dt_set_m3_defaults() against real libfdt root compatibles."""
import re
from pathlib import Path

from test_siliconforest_handoff import definition, run_c

ROOT = Path(__file__).resolve().parents[2]


def between(source, start, end_marker):
    a = source.index(start)
    b = source.index(end_marker, a) + len(end_marker)
    return source[a:b]


def harness():
    source = (ROOT / "src/kboot.c").read_text()
    tables = between(source, "struct m3_default_board {", '#define M3_DEFAULTS_SWITCH "asahi,m3-defaults"\n')
    lookup = between(source, "static const char *kboot_chosen_param(const char *name)\n{", "\n}\n")
    drop = between(source, "static void dt_drop_m3_defaults(void)\n{", "\n}\n")
    drop_graphics = between(source, "static void dt_drop_m3_graphics(", "\n}\n")
    setter = between(source, "int kboot_set_chosen(const char *name, const char *value)\n{", "\n}\n")
    deleter = between(source, "void kboot_delete_chosen(const char *name)\n{", "\n}\n")
    return (r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
#define MAX_CHOSEN_PARAMS 16
static char *chosen_params[MAX_CHOSEN_PARAMS][2];
static bool chosen_is_default[MAX_CHOSEN_PARAMS];
static bool chosen_lost;
static unsigned char blob[16384];
static void *dt = blob;
/* allocation fault injection: the n-th calloc from here on fails */
static int calloc_fail_in = -1;
static void *test_calloc(size_t n, size_t size)
{
    if (calloc_fail_in == 0) { calloc_fail_in = -1; return NULL; }
    if (calloc_fail_in > 0) calloc_fail_in--;
    return calloc(n, size);
}
#define calloc test_calloc
void kboot_delete_chosen(const char *name);
''' + setter + "\n" + deleter + "\n" + tables + lookup + "\n" + drop + "\n" + drop_graphics + "\n" +
        definition(source, "dt_set_m3_defaults") + r'''
/* the same write as dt_set_chosen(): value plus its NUL */
static void write_chosen(void)
{
    int node = fdt_path_offset(dt, "/chosen");
    assert(node >= 0);
    for (int i = 0; i < MAX_CHOSEN_PARAMS && chosen_params[i][0]; i++)
        assert(!fdt_setprop(dt, node, chosen_params[i][0], chosen_params[i][1],
                            (int)strlen(chosen_params[i][1]) + 1));
}
/* a fresh device tree, as each kboot_prepare_dt() opens one; chosen_params persist */
static void tree(const char *compat, int compat_len)
{
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    assert(!fdt_setprop(dt, 0, "compatible", compat, compat_len));
    assert(fdt_add_subnode(dt, 0, "chosen") >= 0);
}
static void reset(const char *compat, int compat_len)
{
    dt_drop_m3_defaults();
    chosen_lost = false;     /* a new boot */
    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        free(chosen_params[i][0]); free(chosen_params[i][1]);
        chosen_params[i][0] = chosen_params[i][1] = NULL;
        chosen_is_default[i] = false;
    }
    tree(compat, compat_len);
}
#define RESET(c) reset(c, sizeof(c))
#define TREE(c) tree(c, sizeof(c))
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define J613 "apple,j613\0apple,t8122\0apple,arm-platform"
#define J615 "apple,j615\0apple,t8122\0apple,arm-platform"
#define J504 "apple,j504\0apple,t8122\0apple,arm-platform"
#define J433 "apple,j433\0apple,t8122\0apple,arm-platform"
#define J514S "apple,j514s\0apple,t6030\0apple,arm-platform"
#define J516S "apple,j516s\0apple,t6030\0apple,arm-platform"
#define J516C "apple,j516c\0apple,t6031\0apple,arm-platform"
#define J514C "apple,j514c\0apple,t6031\0apple,arm-platform"
#define J514M "apple,j514m\0apple,t6034\0apple,arm-platform"
#define J516M "apple,j516m\0apple,t6034\0apple,arm-platform"
#define J293 "apple,j293\0apple,t8103\0apple,arm-platform"
static int entries(void)
{
    int n = 0;
    while (n < MAX_CHOSEN_PARAMS && chosen_params[n][0]) n++;
    return n;
}
static int defaults(void)
{
    int n = 0;
    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) n += chosen_is_default[i];
    return n;
}
static int slot(const char *name)
{
    for (int i = 0; i < MAX_CHOSEN_PARAMS && chosen_params[i][0]; i++)
        if (!strcmp(chosen_params[i][0], name)) return i;
    return -1;
}
/* what dcp6_armed()/g15_optin() accept: exactly the two bytes "1\0" */
static int armed(const char *name)
{
    int len = 0, node = fdt_path_offset(dt, "/chosen");
    const char *v = node >= 0 ? fdt_getprop(dt, node, name, &len) : NULL;
    return v && len == 2 && !strcmp(v, "1");
}
static int present(const char *name)
{
    int node = fdt_path_offset(dt, "/chosen");
    return fdt_getprop(dt, node, name, NULL) != NULL;
}
static int run(void) { int r = dt_set_m3_defaults(); write_chosen(); return r; }
static void fill(int n)
{
    for (int i = 0; i < n; i++) {
        char name[32]; snprintf(name, sizeof(name), "test,filler-%d", i);
        assert(kboot_set_chosen(name, "x") >= 0);
    }
}
static const char *air[] = {"asahi,t8122-dcp", "asahi,t8122-gpu", "asahi,t8122-gpu-power-standin",
                            "asahi,t8122-gpu-fuse-leakage"};
static const char *pro[] = {"asahi,t6030-dcp", "asahi,t6030-gpu"};
static const char *max[] = {"asahi,t6031-dcp", "asahi,t6031-gpu"};
static const char *never[] = {"asahi,t8122-dcpext", "asahi,t6030-dcpext", "asahi,t6030-dispext",
                              "asahi,t8122-gpu-asc-preflight",
                              "asahi,t8122-gpu-handoff-diag", "asahi,m3-defaults"};
static int listed(const char *name, const char **list, int n)
{
    for (int i = 0; i < n; i++) if (!strcmp(name, list[i])) return 1;
    return 0;
}
/* want armed; every other default switch of every board, and the never-defaults, absent */
static void expect_only(const char **want, int n_want)
{
    const char **lists[] = {air, pro, max};
    int sizes[] = {N(air), N(pro), N(max)};
    for (int i = 0; i < n_want; i++) assert(armed(want[i]));
    for (int l = 0; l < 3; l++)
        for (int i = 0; i < sizes[l]; i++)
            if (!listed(lists[l][i], want, n_want)) assert(!present(lists[l][i]));
    for (int i = 0; i < N(never); i++) assert(!present(never[i]));
}
#define expect_none() expect_only(NULL, 0)
#define UNTOUCHED(c) do { RESET(c); assert(!run()); expect_none(); assert(entries() == 0); } while (0)
int main(void)
{
    /* the five boards get their own recipe and nothing else */
    RESET(J613); assert(!run()); expect_only(air, 4);
    RESET(J615); assert(!run()); expect_only(air, 4);
    RESET(J504); assert(!run()); expect_only(air, 4);
    RESET(J516S); assert(!run()); expect_only(pro, 2);
    RESET(J514S); assert(!run()); expect_only(pro, 2);
    RESET(J516C); assert(!run()); expect_only(max, 2);
    assert(entries() == 2 && defaults() == 2 && chosen_is_default[0] && chosen_is_default[1]);
    assert(slot("asahi,t6031-dcp") == 0 && slot("asahi,t6031-gpu") == 1);

    /* the J514C, the T6034 Maxes, a Max board name on another SoC or none, unknown M3 boards, a
       board without its SoC, M1/M2: untouched */
    UNTOUCHED(J514C);
    UNTOUCHED(J514M);
    UNTOUCHED(J433);
    UNTOUCHED("apple,j504\0apple,arm-platform");
    UNTOUCHED("apple,j504\0apple,t6030\0apple,arm-platform");
    UNTOUCHED(J516M);
    UNTOUCHED("apple,j516c\0apple,t6034\0apple,arm-platform");
    UNTOUCHED("apple,j516c\0apple,arm-platform");
    UNTOUCHED("apple,j516s\0apple,t6031\0apple,arm-platform");
    UNTOUCHED("apple,j999\0apple,t6031\0apple,arm-platform");
    UNTOUCHED("apple,j999\0apple,t6030\0apple,arm-platform");
    UNTOUCHED("apple,j999\0apple,t8122\0apple,arm-platform");
    UNTOUCHED("apple,j613\0apple,arm-platform");
    UNTOUCHED("apple,j516s\0apple,t8122\0apple,arm-platform");
    UNTOUCHED(J293);
    UNTOUCHED("apple,j413\0apple,t8112\0apple,arm-platform");

    /* an explicit config value wins: =0 keeps that switch off, the others still default on */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t8122-gpu", "0") >= 0);
    assert(!run());
    assert(armed("asahi,t8122-dcp") && !armed("asahi,t8122-gpu") && present("asahi,t8122-gpu"));
    assert(armed("asahi,t8122-gpu-power-standin") && armed("asahi,t8122-gpu-fuse-leakage"));
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t6030-dcp", "0") >= 0);
    assert(kboot_set_chosen("asahi,t6030-dcpext", "1") >= 0);   /* its own line, kept as is */
    assert(!run());
    assert(!armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu") && armed("asahi,t6030-dcpext"));
    /* an explicit =1 stays one entry */
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t6030-gpu", "1") >= 0);
    assert(!run());
    assert(entries() == 2 && armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu"));

    /* the 14-inch M3 Pro (J514S): its two defaults, and an owner's =0 keeps each one off */
    RESET(J514S);
    assert(kboot_set_chosen("asahi,t6030-gpu", "0") >= 0);
    assert(!run());
    assert(armed("asahi,t6030-dcp") && present("asahi,t6030-gpu") && !armed("asahi,t6030-gpu"));
    assert(entries() == 2 && !chosen_is_default[slot("asahi,t6030-gpu")] && defaults() == 1);
    RESET(J514S);
    assert(kboot_set_chosen("asahi,t6030-dcp", "0") >= 0);
    assert(!run());
    assert(present("asahi,t6030-dcp") && !armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu"));
    assert(entries() == 2 && defaults() == 1);
    RESET(J514S);
    assert(kboot_set_chosen("asahi,t6030-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6030-gpu", "0") >= 0);       /* the later line wins */
    assert(!run());
    assert(armed("asahi,t6030-dcp") && present("asahi,t6030-gpu") && !armed("asahi,t6030-gpu"));
    RESET(J514S);
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(!run());
    assert(!present("asahi,t6030-dcp") && !present("asahi,t6030-gpu") && entries() == 1);

    /* the J516C: each switch stands alone, as on the Pro. An owner's =0 keeps that one off (the
       GPU =0 an earlier installer wrote included) while the other still defaults on, and any owner
       value stays the owner's */
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);
    assert(!run());
    assert(present("asahi,t6031-gpu") && !armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp"));
    assert(entries() == 2 && defaults() == 1 && !chosen_is_default[slot("asahi,t6031-gpu")]);
    assert(!strcmp(chosen_params[slot("asahi,t6031-gpu")][1], "0"));
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);       /* the later line wins */
    assert(!run());
    assert(present("asahi,t6031-gpu") && !armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp"));
    assert(entries() == 2 && defaults() == 1);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp") && entries() == 2 && defaults() == 1);
    assert(!chosen_is_default[slot("asahi,t6031-gpu")] && chosen_is_default[slot("asahi,t6031-dcp")]);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "yes") >= 0);     /* kept; no reader takes it as 1 */
    assert(!run());
    assert(present("asahi,t6031-gpu") && !armed("asahi,t6031-gpu") && entries() == 2);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-dcp", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6031-dcp") && armed("asahi,t6031-gpu") && entries() == 2);
    assert(!chosen_is_default[slot("asahi,t6031-dcp")] && chosen_is_default[slot("asahi,t6031-gpu")]);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-dcp", "0") >= 0);
    assert(!run());
    assert(present("asahi,t6031-dcp") && !armed("asahi,t6031-dcp") && armed("asahi,t6031-gpu"));
    assert(entries() == 2 && defaults() == 1 && !chosen_is_default[slot("asahi,t6031-dcp")]);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-dcp", "0") >= 0);
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);
    assert(!run());
    assert(!armed("asahi,t6031-dcp") && !armed("asahi,t6031-gpu") && entries() == 2 && defaults() == 0);

    /* chosen.asahi,m3-defaults=0 (or anything but 1) adds no default and keeps explicit lines,
       an owner's =1 included */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(kboot_set_chosen("asahi,t8122-dcp", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t8122-dcp") && !present("asahi,t8122-gpu"));
    assert(!present("asahi,t8122-gpu-power-standin") && !present("asahi,t8122-gpu-fuse-leakage"));
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,m3-defaults", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu"));
    RESET(J516C);
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp"));
    assert(entries() == 1 && defaults() == 0);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,m3-defaults", "off") >= 0);
    assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp") && entries() == 1);
    RESET(J516C);                                    /* the owner's =1 stays; no display default */
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6031-gpu") && !present("asahi,t6031-dcp") && entries() == 2);
    assert(defaults() == 0);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6031-dcp", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp") && entries() == 3 && defaults() == 0);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,m3-defaults", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp") && entries() == 3 && defaults() == 2);

    /* a short chosen table: all of the recipe or none of it, nothing overwritten */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    fill(MAX_CHOSEN_PARAMS - 3);
    assert(!run());
    for (int i = 0; i < 4; i++) assert(!present(air[i]));
    assert(entries() == MAX_CHOSEN_PARAMS - 3 && !strcmp(chosen_params[0][1], "x"));
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");      /* the Pro's two still fit */
    fill(MAX_CHOSEN_PARAMS - 2);
    assert(!run()); assert(armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu"));
    RESET(J516C);                                                 /* the Max's two in the last entries */
    fill(MAX_CHOSEN_PARAMS - 2);
    assert(!run());
    assert(armed("asahi,t6031-dcp") && armed("asahi,t6031-gpu") && entries() == MAX_CHOSEN_PARAMS);
    assert(chosen_is_default[MAX_CHOSEN_PARAMS - 2] && chosen_is_default[MAX_CHOSEN_PARAMS - 1]);
    assert(defaults() == 2);
    RESET(J516C);                                                 /* not one of them into one entry */
    fill(MAX_CHOSEN_PARAMS - 1);
    assert(!run());
    assert(!present("asahi,t6031-dcp") && !present("asahi,t6031-gpu"));
    assert(entries() == MAX_CHOSEN_PARAMS - 1 && defaults() == 0 && !chosen_lost);
    for (int i = 0; i < MAX_CHOSEN_PARAMS - 1; i++) assert(!strcmp(chosen_params[i][1], "x"));
    RESET(J516C);                                     /* one entry is enough beside an owner's line */
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);
    fill(MAX_CHOSEN_PARAMS - 2);
    assert(!run());
    assert(armed("asahi,t6031-dcp") && present("asahi,t6031-gpu") && !armed("asahi,t6031-gpu"));
    assert(entries() == MAX_CHOSEN_PARAMS && defaults() == 1 && chosen_is_default[MAX_CHOSEN_PARAMS - 1]);
    RESET(J516C);                                                 /* and not into a full table */
    fill(MAX_CHOSEN_PARAMS);
    assert(!run());
    assert(!present("asahi,t6031-dcp") && !present("asahi,t6031-gpu"));
    assert(entries() == MAX_CHOSEN_PARAMS && defaults() == 0);
    assert(!chosen_lost);                                         /* every config line was kept */
    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) assert(!strcmp(chosen_params[i][1], "x"));

    /* a second preparation (proxy re-prepare) changes nothing */
    RESET("apple,j615\0apple,t8122\0apple,arm-platform");
    assert(!run()); TREE("apple,j615\0apple,t8122\0apple,arm-platform"); assert(!run());
    assert(entries() == 4);
    for (int i = 0; i < 4; i++) assert(armed(air[i]));
    RESET(J516C);
    assert(!run()); TREE(J516C); assert(!run()); TREE(J516C); assert(!run());
    assert(entries() == 2 && defaults() == 2 && armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp"));

    /* the master switch set after a first preparation: the earlier defaults go */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(!run());
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    TREE("apple,j613\0apple,t8122\0apple,arm-platform"); assert(!run());
    for (int i = 0; i < 4; i++) assert(!present(air[i]));
    assert(entries() == 1);
    RESET(J516C);
    assert(!run());
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    TREE(J516C); assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp"));
    assert(entries() == 1 && defaults() == 0);

    /* another device tree in the same process: no default carries over to another board */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(!run());
    TREE("apple,j516s\0apple,t6030\0apple,arm-platform"); assert(!run());
    for (int i = 0; i < 4; i++) assert(!present(air[i]));
    assert(armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu") && entries() == 2);
    TREE(J516C); assert(!run());
    expect_only(max, 2); assert(entries() == 2);
    TREE(J514C); assert(!run());
    expect_none(); assert(entries() == 0);

    /* an owner's later value for a defaulted switch is theirs and survives */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(!run());
    assert(kboot_set_chosen("asahi,t8122-gpu", "0") >= 0);
    TREE("apple,j613\0apple,t8122\0apple,arm-platform"); assert(!run());
    assert(present("asahi,t8122-gpu") && !armed("asahi,t8122-gpu"));
    assert(armed("asahi,t8122-dcp") && armed("asahi,t8122-gpu-power-standin") &&
           armed("asahi,t8122-gpu-fuse-leakage") && entries() == 4);

    /* generated-entry flags: a default deleted from the middle moves the owner's later line down
       without its flag, the owner's later value takes a default over, and switching boards in one
       process carries no default from one board to the next */
    RESET(J516C);
    assert(kboot_set_chosen("test,first", "1") >= 0);
    assert(!run());                                       /* test,first, then the defaults */
    assert(kboot_set_chosen("test,later", "1") >= 0);     /* a line after them, as from the proxy */
    assert(entries() == 4 && chosen_is_default[1] && chosen_is_default[2] && !chosen_is_default[3]);
    TREE(J516C); assert(!run());                          /* the defaults dropped, then added last */
    assert(entries() == 4 && slot("test,later") == 1 && !chosen_is_default[1]);
    assert(slot("asahi,t6031-dcp") == 2 && slot("asahi,t6031-gpu") == 3);
    assert(chosen_is_default[2] && chosen_is_default[3] && defaults() == 2);
    TREE(J516S); assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp"));
    assert(armed("asahi,t6030-dcp") && armed("asahi,t6030-gpu"));
    assert(entries() == 4 && !chosen_is_default[0] && !chosen_is_default[1] && defaults() == 2);
    TREE(J613); assert(!run());
    expect_only(air, 4); assert(entries() == 6 && defaults() == 4);
    TREE(J514C); assert(!run());
    expect_none(); assert(entries() == 2 && defaults() == 0);
    TREE(J516M); assert(!run());
    expect_none(); assert(entries() == 2 && defaults() == 0);
    TREE(J516C); assert(!run());
    expect_only(max, 2); assert(entries() == 4 && defaults() == 2);
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);   /* the owner takes the GPU over */
    assert(defaults() == 1 && slot("asahi,t6031-gpu") == 3 && !chosen_is_default[3]);
    TREE(J516C); assert(!run());                          /* the display default moves after it */
    assert(present("asahi,t6031-gpu") && !armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp"));
    assert(entries() == 4 && slot("asahi,t6031-gpu") == 2 && slot("asahi,t6031-dcp") == 3);
    assert(chosen_is_default[3] && defaults() == 1);
    TREE(J516S); assert(!run());                          /* an owner's T6031 line stays on a Pro */
    assert(present("asahi,t6031-gpu") && !present("asahi,t6031-dcp") && armed("asahi,t6030-gpu"));
    assert(entries() == 5 && defaults() == 2 && !chosen_is_default[slot("asahi,t6031-gpu")]);
    assert(!strcmp(chosen_params[0][0], "test,first") && !strcmp(chosen_params[1][0], "test,later"));

    /* allocation failure part way through: no partial recipe, no crash */
    for (int fail = 0; fail < 8; fail++) {
        RESET("apple,j613\0apple,t8122\0apple,arm-platform");
        calloc_fail_in = fail;
        assert(!run());
        calloc_fail_in = -1;
        int on = 0;
        for (int i = 0; i < 4; i++) on += present(air[i]);
        assert(on == 0 || on == 4);
        assert(entries() == on);
        for (int i = 0; i < entries(); i++) assert(chosen_params[i][0] && chosen_params[i][1]);
    }
    for (int fail = 0; fail < 6; fail++) {
        RESET(J516C);
        assert(kboot_set_chosen("test,owner", "1") >= 0);
        calloc_fail_in = fail;
        assert(!run());
        calloc_fail_in = -1;
        int on = present("asahi,t6031-dcp") + present("asahi,t6031-gpu");
        assert(on == 0 || on == 2);                      /* both defaults or neither */
        assert(entries() == 1 + on && armed("test,owner") && !chosen_is_default[0]);
        assert(defaults() == on && (!on || (armed("asahi,t6031-dcp") && armed("asahi,t6031-gpu"))));
        assert(on || chosen_lost);                       /* a default that failed marks the loss */
        assert(on || fail < 4);                          /* each of the four allocations was hit */
        TREE(J516C); assert(!run());                     /* and a loss stays: graphics stay off */
        assert(present("asahi,t6031-dcp") + present("asahi,t6031-gpu") == on);
        assert(entries() == 1 + on);
    }
    /* kboot_set_chosen itself: a failed allocation leaves the table as it was */
    RESET("apple,j293\0apple,t8103\0apple,arm-platform");
    calloc_fail_in = 1;                                  /* the name, after the value */
    assert(kboot_set_chosen("test,x", "1") < 0);
    assert(entries() == 0 && chosen_lost);
    RESET("apple,j293\0apple,t8103\0apple,arm-platform");
    calloc_fail_in = 0;                                  /* the value */
    assert(kboot_set_chosen("test,x", "1") < 0);
    assert(entries() == 0 && chosen_lost);

    /* a failed overwrite keeps an owner's =0 (name, value and ownership), and the defaults stay off */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t8122-gpu", "0") >= 0);
    assert(!run());
    calloc_fail_in = 0;
    assert(kboot_set_chosen("asahi,t8122-gpu", "0") < 0);
    assert(entries() == 4 && !strcmp(chosen_params[0][0], "asahi,t8122-gpu") &&
           !strcmp(chosen_params[0][1], "0") && !chosen_is_default[0]);
    TREE("apple,j613\0apple,t8122\0apple,arm-platform"); assert(!run());
    for (int i = 0; i < 4; i++) assert(!present(air[i]));     /* fail closed: no graphics at all */
    assert(entries() == 0);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);
    calloc_fail_in = 0;
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") < 0);
    assert(!strcmp(chosen_params[0][1], "0") && !chosen_is_default[0] && chosen_lost);
    assert(!run());
    expect_none(); assert(entries() == 0);

    /* an owner's =0 that could not be stored at all: no default goes in */
    static const char *zero[] = {"asahi,t8122-gpu", "asahi,t6030-gpu", "asahi,t6031-gpu"};
    for (int b = 0; b < 3; b++) {
        if (b == 2) RESET(J516C);
        else if (b) RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
        else RESET("apple,j613\0apple,t8122\0apple,arm-platform");
        calloc_fail_in = 0;
        assert(kboot_set_chosen(zero[b], "0") < 0);
        assert(!run());
        expect_none(); assert(entries() == 0);
    }
    /* nor when a config line was lost to a full table */
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
    fill(MAX_CHOSEN_PARAMS);
    assert(kboot_set_chosen("asahi,t6030-gpu", "0") < 0 && chosen_lost);
    assert(!run());
    expect_none();
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    fill(MAX_CHOSEN_PARAMS - 1);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);      /* the same entry: no loss */
    assert(!chosen_lost);
    assert(kboot_set_chosen("asahi,t6031-gpu-later", "0") < 0 && chosen_lost);
    assert(!run());
    expect_none(); assert(entries() == MAX_CHOSEN_PARAMS - 1);

    /* the Air's display line at anything but 1 keeps the default GPU switches off */
    for (int b = 0; b < 2; b++) {
        if (b) RESET("apple,j615\0apple,t8122\0apple,arm-platform");
        else RESET("apple,j613\0apple,t8122\0apple,arm-platform");
        assert(kboot_set_chosen("asahi,t8122-dcp", "0") >= 0);
        assert(!run());
        assert(present("asahi,t8122-dcp") && !armed("asahi,t8122-dcp"));
        for (int i = 1; i < 4; i++) assert(!present(air[i]));
        assert(entries() == 1);
    }
    /* an owner's own GPU line beside it stays, with no default power switches added */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t8122-dcp", "0") >= 0);
    assert(kboot_set_chosen("asahi,t8122-gpu", "1") >= 0);
    assert(!run());
    assert(armed("asahi,t8122-gpu") && !present("asahi,t8122-gpu-power-standin"));
    assert(!present("asahi,t8122-gpu-fuse-leakage") && entries() == 2);
    /* the display line changed after a first preparation: =0 takes the GPU defaults away, =1 restores them */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(!run()); assert(entries() == 4);
    assert(kboot_set_chosen("asahi,t8122-dcp", "0") >= 0);
    TREE("apple,j613\0apple,t8122\0apple,arm-platform"); assert(!run());
    for (int i = 1; i < 4; i++) assert(!present(air[i]));
    assert(entries() == 1);
    assert(kboot_set_chosen("asahi,t8122-dcp", "1") >= 0);
    TREE("apple,j613\0apple,t8122\0apple,arm-platform"); assert(!run());
    for (int i = 0; i < 4; i++) assert(armed(air[i]));
    assert(entries() == 4);
    /* the Pro keeps its GPU default beside a display line at 0 (tested above): no such rule there */

    /* after a lost line an owner's earlier =1 no longer stands on the five boards: every graphics
       line of the board goes, other lines stay, and the preparation still succeeds */
    RESET("apple,j613\0apple,t8122\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t8122-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t8122-dcpext", "1") >= 0);
    assert(kboot_set_chosen("asahi,j615-25g83-experimental", "1") >= 0);
    assert(kboot_set_chosen("test,other", "1") >= 0);
    chosen_lost = true;
    assert(!run());
    for (int i = 0; i < 4; i++) assert(!present(air[i]));
    assert(!present("asahi,t8122-dcpext") && !present("asahi,j615-25g83-experimental"));
    assert(present("test,other") && entries() == 1);
    RESET("apple,j516s\0apple,t6030\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t6030-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6030-dispext", "1") >= 0);
    chosen_lost = true;
    assert(!run());
    for (int i = 0; i < 2; i++) assert(!present(pro[i]));
    assert(!present("asahi,t6030-dispext") && entries() == 0);
    RESET(J516C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6031-dcp", "1") >= 0);
    assert(kboot_set_chosen("asahi,efi-system-partition", "X") >= 0);
    assert(kboot_set_chosen("asahi,t6030-gpu", "1") >= 0);      /* another SoC's line: not this board's */
    chosen_lost = true;
    assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp"));
    assert(present("asahi,efi-system-partition") && present("asahi,t6030-gpu") && entries() == 2);
    RESET(J516C);                                         /* the master switch does not keep them */
    assert(kboot_set_chosen("asahi,m3-defaults", "0") >= 0);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    chosen_lost = true;
    assert(!run());
    assert(!present("asahi,t6031-gpu") && entries() == 1);
    RESET(J516C);                                         /* nothing to drop: no default either */
    chosen_lost = true;
    assert(!run()); assert(entries() == 0);
    TREE(J516C); assert(!run()); assert(entries() == 0 && chosen_lost);
    RESET(J516C);                                         /* a loss after a first preparation */
    assert(!run()); assert(armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp"));
    chosen_lost = true;
    TREE(J516C); assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp"));
    assert(entries() == 0 && defaults() == 0);
    RESET(J516C);                                         /* an owner's =0 and the other default */
    assert(kboot_set_chosen("asahi,t6031-gpu", "0") >= 0);
    assert(!run()); assert(armed("asahi,t6031-dcp"));
    chosen_lost = true;
    TREE(J516C); assert(!run());
    assert(!present("asahi,t6031-gpu") && !present("asahi,t6031-dcp") && entries() == 0);
    /* other boards keep their lines: the rule is only for the five with defaults */
    RESET(J514C);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    assert(kboot_set_chosen("asahi,t6031-dcp", "1") >= 0);
    chosen_lost = true;
    assert(!run());
    assert(armed("asahi,t6031-gpu") && armed("asahi,t6031-dcp") && entries() == 2);
    RESET(J516M);
    assert(kboot_set_chosen("asahi,t6031-gpu", "1") >= 0);
    chosen_lost = true;
    assert(!run());
    assert(armed("asahi,t6031-gpu") && entries() == 1);
    RESET("apple,j293\0apple,t8103\0apple,arm-platform");
    assert(kboot_set_chosen("asahi,t8122-gpu", "1") >= 0);   /* a line no M1 reads: kept as is */
    chosen_lost = true;
    assert(!run());
    assert(armed("asahi,t8122-gpu") && entries() == 1);
    puts("ok");
    return 0;
}
''')


def test_m3_graphics_defaults(tmp_path):
    run_c(tmp_path, "m3-graphics-defaults", harness())


def test_defaults_run_before_chosen_is_written():
    source = (ROOT / "src/kboot.c").read_text()
    prep = source[source.index("int kboot_prepare_dt(void *fdt)\n{"):]
    assert prep.index("dt_set_m3_defaults()") < prep.index("dt_set_chosen()")
    # and before every reader of the switches
    for reader in ("dt_set_display()", "dt_set_gpu(dt)", "dt_set_pmp_t6030()", "dt_set_pmp_t8122()",
                   "dt_set_pmp_t6031()"):
        assert prep.index("dt_set_m3_defaults()") < prep.index(reader)


def test_the_j516c_defaults_are_the_switches_the_t6031_entries_read():
    """The defaults are put where the T6031 display handoff and dt_set_gpu_t6031() look, the GPU's
    ahead of its image read and probe"""
    kboot = (ROOT / "src/kboot.c").read_text()
    gpu = (ROOT / "src/kboot_gpu.c").read_text()
    handoff = (ROOT / "src/kboot_display_handoff.c").read_text()
    assert 'm3_max_boards[] = {"apple,j516c", NULL};' in kboot
    assert 'm3_max_switches[] = {"asahi,t6031-dcp", "asahi,t6031-gpu", NULL};' in kboot
    display = between(handoff, "static const struct dcp_soc dcp_soc_t6031 = {", "};")
    assert '.optin = "asahi,t6031-dcp",' in display and ".board_gate = true," in display
    assert 'dcp_boards_t6031[] = {"apple,j514c", "apple,j516c", NULL};' in handoff
    assert '{"apple,t6031", m3_max_boards, m3_max_switches, false, m3_max_graphics},' in kboot
    soc = between(gpu, "static const struct g15_soc g15_soc_t6031 = {", "};")
    assert '.optin = "asahi,t6031-gpu",' in soc
    entry = definition(gpu, "dt_set_gpu_t6031")
    order = ["t6031_board(dt)", "g15_optin(dt, g15_soc_t6031.optin)", "dt_t6031_gpu_image(dt)",
             "t6031_gpu_probe(dt)", "dt_set_gpu_g15(dt, &g15_soc_t6031)"]
    assert [entry.index(s) for s in order] == sorted(entry.index(s) for s in order)


def test_the_defaults_path_only_edits_the_chosen_table():
    """No reboot, persistence or second-boot step: the helpers only read the device tree's root
    compatible and edit the chosen table"""
    source = (ROOT / "src/kboot.c").read_text()
    allowed = {"dt_drop_m3_defaults", "dt_drop_m3_graphics", "kboot_chosen_param",
               "kboot_set_chosen", "kboot_delete_chosen", "fdt_node_check_compatible", "printf",
               "strcmp", "strncmp", "strlen", "sizeof", "while", "for", "if"}
    for name, signature in (("dt_set_m3_defaults", "static int dt_set_m3_defaults(void)\n{"),
                            ("dt_drop_m3_defaults", "static void dt_drop_m3_defaults(void)\n{"),
                            ("dt_drop_m3_graphics", "static void dt_drop_m3_graphics(")):
        body = between(source, signature, "\n}\n")
        body = body[body.index("{"):]
        body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)      # comments
        body = re.sub(r'"(?:\\.|[^"\\])*"', '""', body)        # strings
        calls = set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", body))
        assert "kboot_delete_chosen" in calls or "dt_drop_m3_defaults" in calls, name
        assert calls <= allowed, (name, calls - allowed)
