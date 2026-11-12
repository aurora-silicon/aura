# SPDX-License-Identifier: MIT
"""Enable a T8140 video encoder, its DARTs and their whole power-domain chains in a real FDT.

The J700 DT describes both encoders and their power states disabled; aura enables the one the
ADT describes. The power-state chain above an encoder is deep (encoder 0's me1 state sits eight
providers from the top on integration DTs since the T8140 fabric power states), so the walk
must reach the top of any chain, enable every provider on it, refuse cycles and say why it
stopped. Failure modes covered:

  1. a chain deeper than an arbitrary small limit is left disabled (the J700 encoder 0 case);
  2. a provider shared by two consumers (a diamond, both DARTs and the encoder) is walked badly;
  3. a node with several providers, or providers with power-domain cells, loses one of them;
  4. a cycle recurses forever or is enabled anyway;
  5. a chain past the sanity bound is enabled, or refused without a log line;
  6. a dangling provider phandle is enabled through, or refused without a log line;
  7. the other encoder (ave1, Dj's unit type) or the wrong encoder is enabled.
"""

from pathlib import Path
import subprocess

from host_compiler import host_cc


def region(source, start_marker, end_marker):
    start = source.index(start_marker)
    return source[start:source.index(end_marker, start)]


HARNESS = r'''
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "libfdt.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
static void *dt, *adt;
static char logbuf[8192];
static size_t loglen;
static int log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(logbuf + loglen, sizeof(logbuf) - loglen, fmt, ap);
    va_end(ap);
    if (n > 0)
        loglen += (size_t)n < sizeof(logbuf) - loglen ? (size_t)n : sizeof(logbuf) - loglen - 1;
    return n;
}
#define printf log_printf

/* The ADT has one encoder node, at the address this scenario puts it. */
static const char *adt_encoder = "/arm-io/ave";
static u64 adt_encoder_addr = 0x285100000ULL;

static int adt_path_offset_trace(void *tree, const char *path, int *trace)
{
    (void)tree;
    if (strcmp(path, adt_encoder))
        return -1;
    trace[0] = 1;
    return 1;
}

static int adt_get_reg(void *tree, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)tree; (void)prop; (void)size;
    if (path[0] != 1 || idx)
        return -1;
    *addr = adt_encoder_addr;
    return 0;
}
''' + "@REGION@" + r'''

static unsigned char blob[65536];
static u32 next_phandle = 1;

static int node(const char *name)
{
    char path[64];
    snprintf(path, sizeof(path), "/%s", name);
    int n = fdt_path_offset(dt, path);
    assert(n >= 0);
    return n;
}

static bool okay(const char *name)
{
    int len;
    const char *s = fdt_getprop(dt, node(name), "status", &len);
    return !s || (len == 5 && !memcmp(s, "okay", 5));
}

/* A disabled node with a phandle; cells < 0 means no #power-domain-cells. */
static u32 add(const char *name, int cells)
{
    assert(fdt_add_subnode(dt, 0, name) >= 0);
    u32 ph = next_phandle++;
    assert(!fdt_setprop_u32(dt, node(name), "phandle", ph));
    assert(!fdt_setprop_string(dt, node(name), "status", "disabled"));
    if (cells >= 0)
        assert(!fdt_setprop_u32(dt, node(name), "#power-domain-cells", cells));
    return ph;
}

static void set_cells(const char *name, const char *prop, const u32 *cells, int n)
{
    fdt32_t v[16];
    assert(n <= 16);
    for (int i = 0; i < n; i++)
        v[i] = cpu_to_fdt32(cells[i]);
    assert(!fdt_setprop(dt, node(name), prop, v, n * sizeof(v[0])));
}

static void pd(const char *name, u32 provider)
{
    set_cells(name, "power-domains", &provider, 1);
}

static void reset(void)
{
    dt = blob;
    assert(!fdt_create_empty_tree(dt, sizeof(blob)));
    next_phandle = 1;
    loglen = 0;
    logbuf[0] = 0;
}

static void encoder(const char *name, u64 addr, u32 dart0, u32 dart1, u32 power)
{
    u32 ph = add(name, -1);
    (void)ph;
    assert(!fdt_setprop_string(dt, node(name), "compatible", "apple,t8140-ave"));
    u32 reg[] = {addr >> 32, (u32)addr, 0, 0x4000};
    set_cells(name, "reg", reg, 4);
    u32 iommus[] = {dart0, 1, dart1, 1, dart0, 2};
    set_cells(name, "iommus", iommus, 6);
    pd(name, power);
}

static u32 dart(const char *name, u32 power)
{
    u32 ph = add(name, -1);
    assert(!fdt_setprop_u32(dt, node(name), "#iommu-cells", 1));
    pd(name, power);
    return ph;
}

/*
 * The J700 integration DT shape: encoder 0's me1 power state is eight providers below the
 * top of its chain, and both its DARTs hang off the same chain. Encoder 1 has its own chain.
 */
static u32 ave0, ave1;
static void j700(void)
{
    reset();
    u32 p198 = add("ps_198", 0);
    assert(!fdt_setprop_string(dt, node("ps_198"), "status", "okay"));
    u32 p1e8 = add("ps_1e8", 0); pd("ps_1e8", p198);
    u32 p218 = add("ps_218", 0); pd("ps_218", p1e8);
    u32 p240 = add("ps_240", 0); pd("ps_240", p218);
    u32 dma = add("ps_venc0_dma", 0); pd("ps_venc0_dma", p240);
    u32 me0 = add("ps_venc0_me0", 0); pd("ps_venc0_me0", dma);
    u32 me1 = add("ps_venc0_me1", 0); pd("ps_venc0_me1", me0);
    u32 sys = add("ps_venc0_sys", 0); pd("ps_venc0_sys", p240);
    u32 d0 = dart("dart_ave0", sys), d1 = dart("dart_ave0_1", sys);
    encoder("ave0", 0x285100000ULL, d0, d1, me1);
    ave0 = fdt_get_phandle(dt, node("ave0"));

    u32 v1 = add("ps_venc1_sys", 0); pd("ps_venc1_sys", p218);
    u32 v1me = add("ps_venc1_me", 0); pd("ps_venc1_me", v1);
    u32 e0 = dart("dart_ave1", v1), e1 = dart("dart_ave1_1", v1);
    encoder("ave1", 0x289100000ULL, e0, e1, v1me);
    ave1 = fdt_get_phandle(dt, node("ave1"));
}

static const char *const enc0_chain[] = {
    "ave0", "dart_ave0", "dart_ave0_1", "ps_venc0_me1", "ps_venc0_me0", "ps_venc0_dma",
    "ps_venc0_sys", "ps_240", "ps_218", "ps_1e8", "ps_198",
};
static const char *const enc1_only[] = {"ave1", "dart_ave1", "dart_ave1_1", "ps_venc1_sys",
                                        "ps_venc1_me"};

int main(void)
{
    /* 1+2: J700 encoder 0, chain eight providers deep, DARTs share it. */
    j700();
    assert(!strcmp(dt_t8140_ave_adt_path(node("ave0")), "/arm-io/ave"));
    assert(dt_t8140_ave_adt_path(node("ave1")) == NULL);
    assert(!dt_t8140_ave_enable(ave0));
    for (size_t i = 0; i < ARRAY_SIZE(enc0_chain); i++)
        if (!okay(enc0_chain[i])) {
            fprintf(stderr, "FAIL: %s not enabled\nlog:\n%s\n", enc0_chain[i], logbuf);
            return 1;
        }
    for (size_t i = 0; i < ARRAY_SIZE(enc1_only); i++)
        assert(!okay(enc1_only[i]));
    assert(okay("ps_218")); /* shared with encoder 1, enabled for encoder 0 */
    puts("PASS: J700 encoder 0, 8-deep power-state chain enabled, encoder 1 untouched");

    /* 7: encoder 1 (ave1): the ADT names it at its own address; encoder 0 stays off. */
    j700();
    adt_encoder = "/arm-io/ave1";
    adt_encoder_addr = 0x289100000ULL;
    assert(dt_t8140_ave_adt_path(node("ave0")) == NULL);
    assert(!strcmp(dt_t8140_ave_adt_path(node("ave1")), "/arm-io/ave1"));
    assert(!dt_t8140_ave_enable(ave1));
    for (size_t i = 0; i < ARRAY_SIZE(enc1_only); i++)
        assert(okay(enc1_only[i]));
    assert(okay("ps_218") && okay("ps_1e8") && okay("ps_198"));
    const char *const enc0_only[] = {"ave0", "dart_ave0", "dart_ave0_1", "ps_venc0_me1",
                                     "ps_venc0_me0", "ps_venc0_dma", "ps_venc0_sys", "ps_240"};
    for (size_t i = 0; i < ARRAY_SIZE(enc0_only); i++)
        assert(!okay(enc0_only[i]));
    adt_encoder = "/arm-io/ave";
    adt_encoder_addr = 0x285100000ULL;
    puts("PASS: encoder 1 (ave1) chain enabled, encoder 0 untouched");

    /* 3: two providers on one node, one of them with a power-domain cell. */
    reset();
    u32 top = add("top", 0);
    u32 a = add("a", 1); pd("a", top);
    u32 b = add("b", 0); pd("b", top);
    u32 dd = dart("dart", b);
    encoder("ave0", 0x285100000ULL, dd, dd, a);
    u32 two[] = {a, 7, b};
    set_cells("ave0", "power-domains", two, 3);
    assert(!dt_t8140_ave_enable(fdt_get_phandle(dt, node("ave0"))));
    assert(okay("ave0") && okay("a") && okay("b") && okay("top") && okay("dart"));
    puts("PASS: several providers and power-domain cells");

    /* 4: a cycle is refused, nothing on it is enabled, and the log says so. */
    reset();
    u32 c1 = add("c1", 0), c2 = add("c2", 0), c3 = add("c3", 0);
    pd("c1", c2); pd("c2", c3); pd("c3", c1);
    dd = dart("dart", c1);
    encoder("ave0", 0x285100000ULL, dd, dd, c1);
    assert(dt_t8140_ave_enable(fdt_get_phandle(dt, node("ave0"))) < 0);
    assert(!okay("c1") && !okay("c2") && !okay("c3") && !okay("ave0"));
    assert(strstr(logbuf, "cycle"));
    puts("PASS: power-domain cycle refused and logged");

    /* A self-reference is a cycle too. */
    reset();
    c1 = add("c1", 0);
    pd("c1", c1);
    dd = dart("dart", c1);
    encoder("ave0", 0x285100000ULL, dd, dd, c1);
    assert(dt_t8140_ave_enable(fdt_get_phandle(dt, node("ave0"))) < 0);
    assert(!okay("c1") && strstr(logbuf, "cycle"));
    puts("PASS: self-referencing power domain refused and logged");

    /* 5: 30 deep is fine; past the sanity bound is refused with a log line. */
    for (int run = 0; run < 2; run++) {
        int depth = run ? 200 : 30, pass = !run;
        reset();
        u32 prev = 0;
        char name[16];
        for (int i = 0; i < depth; i++) {
            snprintf(name, sizeof(name), "p%d", i);
            u32 ph = add(name, 0);
            if (prev)
                pd(name, prev);
            prev = ph;
        }
        dd = dart("dart", prev);
        encoder("ave0", 0x285100000ULL, dd, dd, prev);
        int ret = dt_t8140_ave_enable(fdt_get_phandle(dt, node("ave0")));
        if (pass) {
            assert(!ret && okay("ave0") && okay("p0") && okay("p29"));
        } else {
            assert(ret < 0 && !okay("ave0") && !okay("p0"));
            assert(strstr(logbuf, "deeper than"));
        }
    }
    puts("PASS: 30-deep chain enabled, 200-deep refused at the sanity bound and logged");

    /* 6: a dangling provider phandle is refused and logged. */
    reset();
    u32 p = add("p", 0);
    pd("p", 0xdead);
    dd = dart("dart", p);
    encoder("ave0", 0x285100000ULL, dd, dd, p);
    assert(dt_t8140_ave_enable(fdt_get_phandle(dt, node("ave0"))) < 0);
    assert(!okay("p") && !okay("ave0") && strstr(logbuf, "0xdead"));
    puts("PASS: dangling power-domain phandle refused and logged");
    return 0;
}
'''


def test_t8140_ave_enable_walk(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / "src/kboot.c").read_text()
    code = HARNESS.replace("@REGION@", region(
        source, "static const char *dt_t8140_ave_adt_path(",
        "/*\n * The node is found again by its compatible"))
    harness = tmp_path / "t8140-ave-enable.c"
    harness.write_text(code)
    binary = tmp_path / "t8140-ave-enable"
    lib = repo / "src/libfdt"
    files = ["fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_wip.c", "fdt_sw.c", "fdt_empty_tree.c"]
    subprocess.run([
        *host_cc(), "-std=gnu11", "-Wall", "-Werror", "-Wno-sign-compare",
        "-Wno-unused-function", "-I", str(lib), str(harness),
        *[str(lib / f) for f in files], "-o", str(binary),
    ], check=True)
    run = subprocess.run([str(binary)], capture_output=True, text=True)
    assert run.returncode == 0, run.stdout + run.stderr
    assert run.stdout.count("PASS:") == 7, run.stdout
