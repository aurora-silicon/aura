# SPDX-License-Identifier: MIT
"""Build and run src/kboot_display_routes.c on the host against an ADT model and a DT.

The ADT model is a list of nodes: {"path": "/arm-io/x", "props": {name: bytes}, "regs": [cpu
addresses]}. Paths give the tree; "regs" stand in for adt_get_reg()'s translated addresses.
The DT is either built by the C snippet `dt_setup` (a body that fills `dt`) or loaded from a
DTB file. `checks` is C run after the routes are written.
"""

from pathlib import Path
import struct
import subprocess

from host_compiler import host_cc

ROOT = Path(__file__).resolve().parents[2]


def u32(*values):
    return b"".join(struct.pack("<I", v) for v in values)


def _cbytes(data):
    return "{" + ",".join(str(b) for b in data) + "}" if data else "{0}"


def adt_model_c(nodes):
    """C tables and the ADT API over them. Node 0 is the root; offsets are index + 1."""
    paths = ["/"] + [n["path"] for n in nodes]
    index = {p: i for i, p in enumerate(paths)}
    parent = [-1]
    for p in paths[1:]:
        up = p.rsplit("/", 1)[0] or "/"
        assert up in index, f"{p}: no parent {up}"
        parent.append(index[up])
    children = {i: [] for i in range(len(paths))}
    for i, up in enumerate(parent):
        if up >= 0:
            children[up].append(i)
    props, regs = [], []
    for i, n in enumerate([{"props": {}, "regs": []}] + list(nodes)):
        for name, value in n.get("props", {}).items():
            props.append((i, name, value))
        for k, a in enumerate(n.get("regs", [])):
            regs.append((i, k, a))
    out = ["#define ADT_N %d" % len(paths)]
    out.append("static const char *adt_names[] = {%s};" % ",".join(
        '"%s"' % (p.rsplit("/", 1)[1] if p != "/" else "") for p in paths))
    out.append("static const char *adt_paths[] = {%s};" % ",".join('"%s"' % p for p in paths))
    nxt = [-1] * len(paths)
    first = [-1] * len(paths)
    for i, kids in children.items():
        if kids:
            first[i] = kids[0]
            for a, b in zip(kids, kids[1:]):
                nxt[a] = b
    out.append("static const int adt_first[] = {%s};" % ",".join(map(str, first)))
    out.append("static const int adt_next[] = {%s};" % ",".join(map(str, nxt)))
    out.append("static const int adt_nkids[] = {%s};" % ",".join(
        str(len(children[i])) for i in range(len(paths))))
    out.append("struct prop { int node; const char *name; unsigned len; unsigned char v[64]; };")
    out.append("static const struct prop adt_props[] = {%s};" % ",".join(
        '{%d,"%s",%d,%s}' % (i, name, len(v), _cbytes(v)) for i, name, v in props) +
        "\n#define ADT_NPROPS %d" % len(props))
    out.append("struct reg { int node, idx; unsigned long long a; };")
    out.append("static const struct reg adt_regs[] = {%s};" % (",".join(
        "{%d,%d,0x%xULL}" % r for r in regs) or "{-1,0,0}") +
        "\n#define ADT_NREGS %d" % len(regs))
    out.append(r'''
void *adt = (void *)1;
static int adt_get_child_count(const void *a, int n) { return adt_nkids[n - 1]; }
static int adt_first_child_offset(const void *a, int n) { return adt_first[n - 1] + 1; }
static int adt_next_sibling_offset(const void *a, int n) { return adt_next[n - 1] + 1; }
static const char *adt_get_name(const void *a, int n) { return adt_names[n - 1]; }
static const void *adt_getprop(const void *a, int n, const char *name, u32 *lenp)
{
    for (int i = 0; i < ADT_NPROPS; i++)
        if (adt_props[i].node == n - 1 && !strcmp(adt_props[i].name, name)) {
            if (lenp)
                *lenp = adt_props[i].len;
            return adt_props[i].v;
        }
    return NULL;
}
static int adt_getprop_copy(const void *a, int n, const char *name, void *out, size_t len)
{
    u32 l;
    const void *v = adt_getprop(a, n, name, &l);
    if (!v || l != len)
        return -1;
    memcpy(out, v, len);
    return len;
}
#define ADT_GETPROP(adt, nodeoffset, name, val)                                                    \
    adt_getprop_copy(adt, nodeoffset, name, (val), sizeof(*(val)))
#define ADT_FOREACH_CHILD(adt, node)                                                               \
    for (int _child_count = adt_get_child_count(adt, node); _child_count; _child_count = 0)        \
        for (node = adt_first_child_offset(adt, node); _child_count--;                             \
             node = adt_next_sibling_offset(adt, node))
static int adt_path_offset(const void *a, const char *path)
{
    for (int i = 0; i < ADT_N; i++)
        if (!strcmp(adt_paths[i], path))
            return i + 1;
    return -1;
}
static int adt_path_offset_trace(const void *a, const char *path, int *trace)
{
    int n = adt_path_offset(a, path);
    trace[0] = n;
    trace[1] = 0;
    return n;
}
static int adt_get_reg(const void *a, int *trace, const char *prop, int idx, u64 *addr, u64 *size)
{
    for (int i = 0; i < ADT_NREGS; i++)
        if (adt_regs[i].node == trace[0] - 1 && adt_regs[i].idx == idx) {
            *addr = adt_regs[i].a;
            return 0;
        }
    return -1;
}
''')
    return "\n".join(out)


PRELUDE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libfdt.h"
typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
#define max(a, b) (((a) > (b)) ? (a) : (b))
#define AURA_XTEXT
'''


def path_exact():
    """kboot.c's exact-name path lookup, which the route code uses."""
    kboot = (ROOT / "src/kboot.c").read_text()
    start = kboot.index("int kboot_fdt_path_exact(")
    return kboot[start:kboot.index("\n}\n", start) + 3]


def routes_source():
    source = path_exact() + (ROOT / "src/kboot_display_routes.c").read_text()
    header = (ROOT / "src/kboot_display_routes.h").read_text()
    strip = lambda text: "\n".join(l for l in text.splitlines()
                                   if not l.startswith("#include")) + "\n"
    return strip(header), strip(source)


def route_walk_source():
    """The headless handoff's route walk (kboot_display_handoff.c), which enables what the
    written routes need."""
    handoff = (ROOT / "src/kboot_display_handoff.c").read_text()
    start = handoff.index("enum { T6050_ROUTE_DONE")
    end = handoff.index("\n}\n", handoff.index("static T6050_TEXT int t6050_route(void")) + 3
    return ("#define T6050_TEXT\n#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))\n"
            "#define dlog(fmt, ...) printf(\"DCP: \" fmt, ##__VA_ARGS__)\n" +
            handoff[start:end])


def run(tmp_path, nodes, main_body, dt_setup="", dtb=None, name="routes", walk=False):
    header, source = routes_source()
    if walk:
        source += route_walk_source()
    lib = ROOT / "src/libfdt"
    program = PRELUDE + adt_model_c(nodes) + header + source + r'''
static unsigned char blob[1 << 20];
static void *dt = blob;
static int node(const char *path)
{
    int n = kboot_fdt_path_exact(dt, path);
    if (n < 0)
        fprintf(stderr, "no node %s\n", path);
    assert(n >= 0);
    return n;
}
static int add(const char *parent, const char *name)
{
    int n = fdt_add_subnode(dt, node(parent), name);
    assert(n >= 0);
    return n;
}
static const fdt32_t *cells(const char *path, const char *prop, int count)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node(path), prop, &len);
    if (!v || len != 4 * count)
        fprintf(stderr, "%s %s: %d bytes, not %d\n", path, prop, v ? len : -1, 4 * count);
    assert(v && len == 4 * count);
    return v;
}
static u32 cell(const char *path, const char *prop, int i, int count)
{
    return fdt32_ld(&cells(path, prop, count)[i]);
}
static int remote(const char *path)
{
    return fdt_node_offset_by_phandle(dt, cell(path, "remote-endpoint", 0, 1));
}
static void load(const char *file)
{
    FILE *f = fopen(file, "rb");
    assert(f);
    size_t n = fread(blob, 1, sizeof(blob), f);
    fclose(f);
    assert(n > 0 && fdt_check_header(blob) == 0);
    assert(fdt_open_into(blob, blob, sizeof(blob)) == 0);
}
static void setup(void)
{
''' + dt_setup + r'''
}
int main(int argc, char **argv)
{
    if (argc > 1)
        load(argv[1]);
    else
        setup();
''' + main_body + r'''
    return 0;
}
'''
    binary = tmp_path / name
    (tmp_path / (name + ".c")).write_text(program)
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
         "-Wno-unused-parameter", "-Wno-unused-function", "-I", str(lib),
         str(tmp_path / (name + ".c")),
         *map(str, (lib / f for f in (
             "fdt.c", "fdt_ro.c", "fdt_rw.c", "fdt_sw.c", "fdt_wip.c",
             "fdt_empty_tree.c", "fdt_strerror.c", "fdt_addresses.c", "fdt_check.c"))),
         "-o", str(binary)],
        check=True,
    )
    result = subprocess.run([str(binary)] + ([str(dtb)] if dtb else []), check=True,
                            capture_output=True, text=True)
    return result.stdout
