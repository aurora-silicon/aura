# SPDX-License-Identifier: MIT
"""Pin the T6050 GP link-speed rule: the ADT's speed for a bridge, the target stage D derives from it and
the root port's LNKCAP2, and the max-link-speed values the handoff puts in the tree (a hint whose failed
write never stops the handoff)."""

from pathlib import Path
import subprocess

from host_compiler import host_cc


def extract(source, signature):
    start = source.index(signature)
    end = source.index("{", start) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def test_t6050_link_speed(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    pcie = (repo / "src/pcie.c").read_text()
    t6050 = (repo / "src/pcie_t6050.c").read_text()
    functions = "\n".join([
        extract(pcie, "int pcie_adt_link_speed("),
        extract(t6050, "static int target_link_speed("),
        extract(t6050, "static void t6050_max_link_speed("),
        extract(t6050, "static void t6050_publish_speeds("),
    ])
    code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(n) (1UL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_GET(f, v) (((v) & (f)) / ((f) & -(f)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min(a, b) (((a) < (b)) ? (a) : (b))
#define FDT_ERR_NOTFOUND 1
#define T6050_GP_PORTS 4
#define PCI_EXP_LNKCAP2_SLS GENMASK(7, 1)
#define FAIL(...) do { printf("STOP: " __VA_ARGS__); return -1; } while (0)
#define ADT_GETPROP(a, n, p, v) adt_getprop_copy(n, p, v, sizeof(*(v)))
static void *adt;
static const char apcie_path[] = "/arm-io/apcie0";

/* The ADT model: bridge N is node 10 + N, its first child 20 + N; a child speed of 0 means absent, and the
 * bridge's maximum-link-speed is present when set nonzero or when mls_zero[N] marks a present 0 */
static u32 mls[4], child_target[4], child_expected[4];
static bool mls_zero[4];
static bool bridge[4], child[4];
static int adt_path_offset(void *a, const char *path)
{
    for (int p = 0; p < 4; p++) {
        char want[40];
        snprintf(want, sizeof(want), "%s/pci-bridge%d", apcie_path, p);
        if (!strcmp(path, want))
            return bridge[p] ? 10 + p : -1;
    }
    return -1;
}
static int adt_first_child_offset(void *a, int node) { return child[node - 10] ? node + 10 : -1; }
static int adt_getprop_copy(int node, const char *prop, void *out, size_t len)
{
    u32 v = 0;
    assert(len == 4);
    if (node >= 10 && node < 14 && !strcmp(prop, "maximum-link-speed")) {
        if (!mls[node - 10] && !mls_zero[node - 10])
            return -1;
        v = mls[node - 10];
        memcpy(out, &v, 4);
        return 4;
    }
    else if (node >= 20 && node < 24 && !strcmp(prop, "target-link-speed"))
        v = child_target[node - 20];
    else if (node >= 20 && node < 24 && !strcmp(prop, "expected-link-speed"))
        v = child_expected[node - 20];
    if (!v)
        return -1;
    memcpy(out, &v, 4);
    return 4;
}

/* The DT model: root port node N is N, or absent; one write may fail */
static bool dt_node[4];
static u32 published[4];
static int fail_port = -1;
static int t6050_port_node(void *dt, int port) { return dt_node[port] ? port : -FDT_ERR_NOTFOUND; }
static int fdt_setprop_u32(void *dt, int node, const char *name, u32 val)
{
    assert(!strcmp(name, "max-link-speed"));
    if (node == fail_port)
        return -1;
    published[node] = val;
    return 0;
}

struct t6050_port { int port; };
struct port_regs { const struct t6050_port *p; char bridge[40]; u32 speed; bool up; };
static const struct t6050_port ports[] = {{2}, {3}, {0}};
static struct port_regs pr[ARRAY_SIZE(ports)];

''' + functions + r'''

static void reset(void)
{
    memset(mls, 0, sizeof(mls)); memset(child_target, 0, sizeof(child_target));
    memset(child_expected, 0, sizeof(child_expected)); memset(bridge, 0, sizeof(bridge));
    memset(child, 0, sizeof(child)); memset(dt_node, 0, sizeof(dt_node)); memset(mls_zero, 0, sizeof(mls_zero));
    memset(published, 0, sizeof(published)); memset(pr, 0, sizeof(pr)); fail_port = -1;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        pr[i].p = &ports[i];
        snprintf(pr[i].bridge, sizeof(pr[i].bridge), "%s/pci-bridge%d", apcie_path, ports[i].port);
    }
}

int main(void)
{
    const u32 gen5 = 0x3e, gen4 = 0x1e; /* LNKCAP2: 2.5-32 GT/s, 2.5-16 GT/s */
    struct port_regs *p2 = &pr[0];

    /* target_link_speed() */
    reset(); bridge[2] = true;
    assert(target_link_speed(p2, gen5) == 5);          /* no maximum-link-speed: the root port's fastest */
    mls[2] = 3; assert(target_link_speed(p2, gen5) == 3);
    mls[2] = 1; child[2] = true; child_expected[2] = 2;
    assert(target_link_speed(p2, gen5) == 2);          /* 1: the child's expected-link-speed */
    child_target[2] = 4; assert(target_link_speed(p2, gen5) == 4); /* target-link-speed first */
    child[2] = false; assert(target_link_speed(p2, gen5) == 1);    /* 1 with no child stays 1 */
    mls[2] = 5; assert(target_link_speed(p2, gen4) == 4); /* above the root port's fastest: lowered */
    bridge[2] = false; assert(target_link_speed(p2, gen5) == -1); /* no bridge */
    bridge[2] = true; mls[2] = 0; assert(target_link_speed(p2, 0) == -1); /* no supported speeds */
    assert(target_link_speed(p2, 0x3c) == -1);         /* 2.5 GT/s not supported */
    mls_zero[2] = true; assert(target_link_speed(p2, gen5) == -1); /* a present 0: refused, not "no limit" */

    /* t6050_publish_speeds(): J775D-like, ports 2, 3, 0 up (targets 5, 3, 3), port 1 down with ADT 1/2 */
    reset();
    for (int p = 0; p < 4; p++) bridge[p] = dt_node[p] = true;
    mls[0] = mls[3] = 3; mls[1] = 1; child[1] = true; child_expected[1] = 2;
    pr[0].up = pr[1].up = pr[2].up = true; pr[0].speed = 5; pr[1].speed = 3; pr[2].speed = 3;
    t6050_publish_speeds(0);
    assert(published[0] == 3 && published[1] == 2 && published[2] == 4 && published[3] == 3);

    /* no port 1 node in the tree: nothing for it, the rest as before */
    memset(published, 0, sizeof(published)); dt_node[1] = false;
    t6050_publish_speeds(0);
    assert(!published[1] && published[0] == 3 && published[2] == 4 && published[3] == 3);

    /* a failed write is only logged: the other ports are still published */
    memset(published, 0, sizeof(published)); dt_node[1] = true; fail_port = 2;
    t6050_publish_speeds(0);
    assert(!published[2] && published[0] == 3 && published[1] == 2 && published[3] == 3);

    /* a down port with no ADT speed, or no bridge, gets none */
    reset(); dt_node[2] = true; bridge[2] = true;
    t6050_publish_speeds(0);
    assert(!published[2]);
    bridge[2] = false; t6050_publish_speeds(0); assert(!published[2]);
    bridge[2] = true; mls_zero[2] = true; t6050_publish_speeds(0); assert(!published[2]); /* a present 0 */
    puts("ok");
    return 0;
}
'''
    src = tmp_path / "link_speed.c"
    exe = tmp_path / "link_speed"
    src.write_text(code)
    subprocess.run(host_cc() + ["-std=gnu11", "-Wall", "-Werror", "-o", str(exe), str(src)], check=True)
    assert subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()[-1] == "ok"
