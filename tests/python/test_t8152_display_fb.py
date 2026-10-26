# SPDX-License-Identifier: MIT
"""T8152 display framebuffer ownership and the J873g second display.

Builds one host unit from production sources (T8152_DISPLAY_SOURCE/src): the
display.c state block (globals, configs, second-display state) and its real
display_start_dcp, display_map_fb, mode parse/choose, display_swap,
display_configure, display_swap_state and display_configure_second. The iBoot
types come from dcp_iboot.h.

Host models:
- Each DCP has two DARTs (disp, dcp) that record every mapping. dart_unmap()
  must match a live mapping exactly; dart_find_iova() returns the first gap.
- iova_alloc()/iova_free() track live allocations; a free must match one.
- The iBoot calls are scripted: one connected sink with a mode list and one
  32 bpp color mode. SET_SURFACE records the scanout DVA and requires it to be
  mapped on both DARTs of the current DCP.
- dcp_init() hands out a fresh DCP per call; the first one already maps the
  boot framebuffer (found by dart_search), later ones do not.
- display_shutdown(), fb_reinit() and the ADT identity are recorded leaves.
"""
from pathlib import Path
import os
import re
import subprocess

import pytest

SOURCE = Path(os.environ.get("T8152_DISPLAY_SOURCE", Path(__file__).resolve().parents[2]))


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
    ibh = (SOURCE / "src/dcp_iboot.h").read_text()
    dcph = (SOURCE / "src/dcp.h").read_text()
    disp = (SOURCE / "src/display.c").read_text()
    types = ibh[ibh.index("enum DCPEOTF {"):ibh.index("dcp_iboot_if_t *dcp_ib_init")]
    config = dcph[dcph.index("typedef struct {"):dcph.index("} display_config_t;")] + "} display_config_t;\n"
    state = disp[disp.index("static dcp_dev_t *dcp;"):disp.index("#define USE_DCPEXT 1")]
    display = "".join((
        define(disp, "DISPLAY_STATUS_DELAY"), define(disp, "DISPLAY_STATUS_RETRIES"),
        define(disp, "COMPARE"), define(disp, "abs"),
        state,
        LEAVES,
        function(disp, "u64 display_mode_fb_size("),
        function(disp, "static void display_choose_timing_mode("),
        function(disp, "static void display_choose_color_mode("),
        function(disp, "static bool display_t6030_hands_off("),
        function(disp, "static uintptr_t display_map_fb("),
        function(disp, "int display_start_dcp("),
        block(disp, disp.index("struct display_options {")) + ";\n",
        function(disp, "int display_parse_mode("),
        function(disp, "static int display_swap("),
        function(disp, "int display_configure("),
        function(disp, "static void display_swap_state("),
        function(disp, "int display_configure_second("),
    ))
    return PREFIX + types + config + display + CASES


PREFIX = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32;
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
static u32 chip_id = T8152, board_id = 0x24;
static bool is_j873g = true;

/* A DART that records mappings. */
struct mapping { u64 dva, pa, size; bool live; };
struct fake_dart { struct mapping maps[16]; };
typedef struct fake_dart dart_dev_t;
typedef struct { dart_dev_t *dart_disp, *dart_dcp; void *iovad_dcp, *dptx_ep; } dcp_dev_t;
typedef void afk_epic_ep_t;
typedef struct { int id; } dcp_iboot_if_t;
struct boot_video { u64 base, display, stride, width, height, depth; };
static struct { u64 revision; struct boot_video video; } cur_boot_args;
typedef enum _dcp_shutdown_mode { DCP_QUIESCED = 0, DCP_SLEEP_IF_EXTERNAL = 1, DCP_SLEEP = 2 } dcp_shutdown_mode;
'''

LEAVES = r'''
/* ---- host models ---- */
static u8 boot_args_copy[sizeof(cur_boot_args)];
static u64 boot_args_addr = (u64)boot_args_copy;
static void *adt;
#define BOOT_DVA 0x10004000UL
static u64 vram_pa, vram_size;
static int dcps_started, shutdowns, fb_reinits, allocations, hpd_pokes;
static dcp_dev_t dcps[4], *shut_down[4];
static struct fake_dart darts[8];
static dcp_iboot_if_t iboots[4];

static struct mapping *find_map(dart_dev_t *d, u64 dva)
{
    for (int i = 0; i < 16; i++)
        if (d->maps[i].live && d->maps[i].dva == dva)
            return &d->maps[i];
    return NULL;
}
static int live_maps(dart_dev_t *d)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += d->maps[i].live;
    return n;
}
static void add_map(dart_dev_t *d, u64 dva, u64 pa, u64 size)
{
    for (int i = 0; i < 16; i++) {
        struct mapping *m = &d->maps[i];
        if (m->live)
            assert(dva + size <= m->dva || m->dva + m->size <= dva);
    }
    for (int i = 0; i < 16; i++)
        if (!d->maps[i].live) {
            d->maps[i] = (struct mapping){dva, pa, size, true};
            return;
        }
    assert(!"mapping table full");
}
static int dart_map(dart_dev_t *d, u64 iova, void *pa, size_t size)
{
    add_map(d, iova, (u64)pa, size);
    return 0;
}
static void dart_unmap(dart_dev_t *d, u64 iova, size_t size)
{
    struct mapping *m = find_map(d, iova);
    assert(m && m->size == size && "unmap of a range that is not mapped");
    m->live = false;
}
static u64 dart_vm_base(dart_dev_t *d) { (void)d; return 0x10000000; }
static u64 dart_find_iova(dart_dev_t *d, s64 start, size_t size)
{
    u64 dva = start;
    for (bool moved = true; moved;) {
        moved = false;
        for (int i = 0; i < 16; i++) {
            struct mapping *m = &d->maps[i];
            if (m->live && dva < m->dva + m->size && m->dva < dva + size) {
                dva = ALIGN_UP(m->dva + m->size, SZ_16K);
                moved = true;
            }
        }
    }
    return dva;
}
static u64 dart_search(dart_dev_t *d, void *pa)
{
    for (int i = 0; i < 16; i++)
        if (d->maps[i].live && d->maps[i].pa == (u64)pa)
            return d->maps[i].dva;
    return 0;
}

/* DCP IOVA allocator: high window, live allocations tracked. */
static struct { u64 dva, size; bool live; } iovas[16];
static u64 iova_next = 0x80000000UL;
static int live_iovas(void)
{
    int n = 0;
    for (int i = 0; i < 16; i++)
        n += iovas[i].live;
    return n;
}
static bool iova_live(u64 dva)
{
    for (int i = 0; i < 16; i++)
        if (iovas[i].live && iovas[i].dva == dva)
            return true;
    return false;
}
static u64 iova_alloc(void *d, size_t size)
{
    (void)d;
    for (int i = 0; i < 16; i++)
        if (!iovas[i].live) {
            iovas[i].dva = iova_next;
            iovas[i].size = size;
            iovas[i].live = true;
            iova_next += ALIGN_UP(size, SZ_32M);
            return iovas[i].dva;
        }
    assert(!"iova table full");
    return 0;
}
static void iova_free(void *d, u64 dva, size_t size)
{
    (void)d;
    for (int i = 0; i < 16; i++)
        if (iovas[i].live && iovas[i].dva == dva) {
            assert(iovas[i].size == size);
            iovas[i].live = false;
            return;
        }
    assert(!"free of an IOVA that is not allocated");
}

/* DCP lifecycle */
static bool has_dcp_left_running;
static bool dcp_left_running(void) { return has_dcp_left_running; }
static const display_config_t *display_get_config(void) { has_dcp = true; return t8152_config; }
static dcp_dev_t *dcp_init(const display_config_t *cfg)
{
    (void)cfg;
    assert(dcps_started < 4);
    dcp_dev_t *d = &dcps[dcps_started];
    d->dart_disp = &darts[2 * dcps_started];
    d->dart_dcp = &darts[2 * dcps_started + 1];
    /* iBoot maps the boot framebuffer for the first DCP only. */
    if (!dcps_started) {
        add_map(d->dart_disp, BOOT_DVA, vram_pa, vram_size);
        add_map(d->dart_dcp, BOOT_DVA, vram_pa, vram_size);
    }
    dcps_started++;
    return d;
}
static void dcp_shutdown(dcp_dev_t *d, bool sleep) { (void)d; (void)sleep; assert(!"DCP start failed"); }
static dcp_iboot_if_t *dcp_ib_init(dcp_dev_t *d) { iboots[d - dcps].id = d - dcps; return &iboots[d - dcps]; }
static int display_get_vram(u64 *pa, u64 *size) { *pa = vram_pa; *size = vram_size; return 0; }
static int display_shutdown(dcp_shutdown_mode mode)
{
    assert(mode == DCP_QUIESCED && shutdowns < 4);
    shut_down[shutdowns++] = dcp;
    iboot = NULL;
    return 0;
}
static int dcp_connect_dptx(dcp_dev_t *d) { (void)d; return 0; }
static int dcp_dptx_hpd(void *ep, u32 port, bool hpd) { (void)ep; (void)port; assert(hpd); hpd_pokes++; return 0; }
static int dcp_work(dcp_dev_t *d) { (void)d; return 0; }
static void mdelay(unsigned ms) { (void)ms; }
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_msecs(u64 t) { return t; }
static void mmu_map_framebuffer(u64 a, u64 s) { (void)a; (void)s; }
static int adt_path_offset(void *a, const char *p) { (void)a; (void)p; return -1; }
static int adt_setprop(void *a, int n, const char *p, void *v, size_t s) { (void)a; (void)n; (void)p; (void)v; (void)s; assert(0); return -1; }
static bool adt_is_compatible(void *a, int n, const char *c) { (void)a; assert(!n && !strcmp(c, "J873gAP")); return is_j873g; }
static void fb_reinit(void) { fb_reinits++; }
static u64 top_of_memory_alloc(size_t s)
{
    allocations++;
    void *p = aligned_alloc(SZ_16K, ALIGN_UP(s, SZ_16K));
    assert(p);
    return (u64)p;
}

/* iBoot: one connected sink with a scripted mode list, one 32 bpp color mode. */
static dcp_timing_mode_t modes[4];
static int nmodes, set_mode_failures;
static dcp_color_mode_t color = {.valid = 1, .bpp = 32};
static u64 scanout;
static dcp_dev_t *scanout_dcp;
static int dcp_ib_set_power(dcp_iboot_if_t *ib, bool on) { (void)ib; (void)on; return 0; }
static int dcp_ib_get_hpd(dcp_iboot_if_t *ib, int *timing_cnt, int *color_cnt)
{
    (void)ib;
    *timing_cnt = nmodes;
    *color_cnt = 1;
    return 1;
}
static int dcp_ib_get_timing_modes(dcp_iboot_if_t *ib, dcp_timing_mode_t **m) { (void)ib; *m = modes; return nmodes; }
static int dcp_ib_get_color_modes(dcp_iboot_if_t *ib, dcp_color_mode_t **m) { (void)ib; *m = &color; return 1; }
static int dcp_ib_set_mode(dcp_iboot_if_t *ib, dcp_timing_mode_t *t, dcp_color_mode_t *c)
{
    (void)ib; (void)t; (void)c;
    if (set_mode_failures) {
        set_mode_failures--;
        return -1;
    }
    return 0;
}
static int dcp_ib_set_surface(dcp_iboot_if_t *ib, dcp_layer_t *layer)
{
    assert(ib == iboot);
    u64 dva = layer->planes[0].addr;
    struct mapping *a = find_map(dcp->dart_disp, dva), *b = find_map(dcp->dart_dcp, dva);
    assert(a && b && "scanout DVA is not mapped on both DARTs");
    assert(a->size >= (u64)layer->planes[0].stride * layer->height);
    scanout = dva;
    scanout_dcp = dcp;
    return 0;
}
'''

CASES = r'''
static void mode(int i, u32 w, u32 h)
{
    modes[i] = (dcp_timing_mode_t){.valid = 1, .width = w, .height = h, .fps = 60 << 16};
}
/* A 640x1136 boot framebuffer (the stage before), too small for 1280x720. */
static void boot(u32 chip)
{
    chip_id = chip;
    vram_size = ALIGN_UP(640 * 1136 * 4, SZ_16K);
    vram_pa = top_of_memory_alloc(vram_size);
    allocations = 0;
    cur_boot_args.video = (struct boot_video){
        .base = vram_pa, .stride = 640 * 4, .width = 640, .height = 1136, .depth = 32};
    nmodes = 2;
    mode(0, 1280, 720);
    mode(1, 1920, 1080);
}
/* The scanout DVA is the framebuffer DVA, mapped on both DARTs, and nothing
 * else is left mapped or allocated. */
static void assert_scanout_owned(int maps)
{
    assert(scanout == fb_dva && scanout_dcp == dcp);
    assert(find_map(dcp->dart_disp, fb_dva) && find_map(dcp->dart_dcp, fb_dva));
    assert(find_map(dcp->dart_disp, fb_dva)->size == fb_size);
    assert(live_maps(dcp->dart_disp) == maps && live_maps(dcp->dart_dcp) == maps);
}

static void t8152_growth_keeps_scanout(void)
{
    boot(T8152);
    assert(display_configure("1280x720@60") == 1);
    /* The new framebuffer took over its temporary DVA: still mapped and
     * allocated; the boot framebuffer's DVA is gone. */
    assert(fb_dva != BOOT_DVA && iova_live(fb_dva) && live_iovas() == 1);
    assert(!find_map(dcp->dart_disp, BOOT_DVA) && !find_map(dcp->dart_dcp, BOOT_DVA));
    assert(cur_boot_args.video.base != vram_pa && cur_boot_args.video.width == 1280);
    assert_scanout_owned(1);
    u64 grown = fb_dva;

    /* A later modeset that fits swaps the same framebuffer again. */
    scanout = 0;
    assert(display_configure("1280x720@60") == 1);
    assert(fb_dva == grown && live_iovas() == 1);
    assert_scanout_owned(1);

    /* Growing again releases the previous allocated DVA. */
    assert(display_configure("1920x1080@60") == 1);
    assert(fb_dva != grown && !iova_live(grown) && iova_live(fb_dva) && live_iovas() == 1);
    assert(!find_map(dcp->dart_disp, grown) && !find_map(dcp->dart_dcp, grown));
    assert(cur_boot_args.video.width == 1920);
    assert_scanout_owned(1);
}

static void other_chip_growth_remaps(void)
{
    /* Elsewhere the framebuffer moves back to its original DVA and the
     * temporary one is released, as before. */
    boot(T8103);
    t8152_config = &display_config_j873_hdmi;
    assert(display_configure("1280x720@60") == 1);
    assert(fb_dva == BOOT_DVA && !live_iovas());
    assert_scanout_owned(1);
    /* The HPD poke is T8152 only, also on a DPTX route. */
    assert(display_is_dptx && !hpd_pokes);
}

static void second_display_failure_restores_first(void)
{
    boot(T8152);
    assert(display_configure("1280x720@60") == 1);
    dcp_dev_t *first = dcp;
    dcp_iboot_if_t *first_iboot = iboot;
    u64 first_dva = fb_dva, first_size = fb_size;
    struct boot_video first_video = cur_boot_args.video;
    int first_maps = live_maps(first->dart_disp);
    fb_reinits = allocations = 0;

    /* Both SET_MODE attempts fail on the second display. */
    set_mode_failures = 2;
    assert(display_configure_second("max") < 0);
    assert(allocations == 1 && shutdowns == 1 && shut_down[0] == &dcps[1]);
    assert(dcp == first && iboot == first_iboot && fb_dva == first_dva && fb_size == first_size);
    assert(!memcmp(&cur_boot_args.video, &first_video, sizeof(first_video)));
    assert(!memcmp(boot_args_copy + offsetof(__typeof__(cur_boot_args), video), &first_video,
                   sizeof(first_video)));
    assert(fb_reinits == 1 && live_maps(first->dart_disp) == first_maps);
    assert(!display_second_video.base);

    /* A retry reuses the framebuffer allocated for the failed attempt. */
    u64 second_fb = display_other.video.base;
    assert(second_fb);
    assert(display_configure_second("max") == 1);
    assert(allocations == 1 && display_second_video.base == second_fb);
    assert(display_second_video.width == 1920 && scanout_dcp == &dcps[2]);
    assert(dcp == first && fb_dva == first_dva && fb_size == first_size);
    assert(!memcmp(&cur_boot_args.video, &first_video, sizeof(first_video)));
    /* The second display has its own framebuffer DVA, the first keeps its own. */
    assert(display_other.fb_dva && find_map(dcps[2].dart_disp, display_other.fb_dva));
    assert(find_map(first->dart_disp, first_dva) && find_map(first->dart_dcp, first_dva));
}

static void second_display_identity(void)
{
    boot(T8152);
    is_j873g = false;
    assert(display_configure_second("max") == -1);
    is_j873g = true;
    board_id = 0x23;
    assert(display_configure_second("max") == -1);
    board_id = 0x24;
    chip_id = T8103;
    assert(display_configure_second("max") == -1);
    assert(!allocations && !dcps_started && !fb_reinits && !display_other.video.base);
}

static const struct { const char *name; void (*run)(void); } cases[] = {
    {"t8152-growth-keeps-scanout", t8152_growth_keeps_scanout},
    {"other-chip-growth-remaps", other_chip_growth_remaps},
    {"second-display-failure-restores-first", second_display_failure_restores_first},
    {"second-display-identity", second_display_identity},
};

int main(int argc, char **argv)
{
    assert(argc == 2);
    if (!strcmp(argv[1], "--list")) {
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
            printf("%s\n", cases[i].name);
        return 0;
    }
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(argv[1], cases[i].name))
            continue;
        cases[i].run();
        printf("PASS %s\n", cases[i].name);
        return 0;
    }
    fprintf(stderr, "unknown case %s\n", argv[1]);
    return 2;
}
'''

CASES_LIST = ("t8152-growth-keeps-scanout", "other-chip-growth-remaps",
              "second-display-failure-restores-first", "second-display-identity")


@pytest.fixture(scope="module")
def binary(tmp_path_factory):
    work = tmp_path_factory.mktemp("t8152-display-fb")
    code = work / "unit.c"
    code.write_text(unit())
    program = work / "unit"
    result = subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra",
                             "-Werror", "-Wno-unused-function", "-fsanitize=address,undefined",
                             "-fno-sanitize-recover=undefined", str(code), "-o", str(program)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr[-6000:]
    listed = subprocess.run([str(program), "--list"], capture_output=True, text=True, check=True)
    assert tuple(listed.stdout.split()) == CASES_LIST
    return program


@pytest.mark.parametrize("case", CASES_LIST)
def test_case(binary, case):
    result = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0 and ("PASS " + case) in result.stdout, \
        result.stdout[-3000:] + result.stderr[-3000:]
