# SPDX-License-Identifier: MIT
"""The RTBuddySeg firmware logs are named no-map from the ADT, on any chip.

The functions under test are extracted from src/kboot.c and built against the in-tree libfdt.
The ADT is a small fake laid out as a J713's (T8132) that answers the calls the extracted code
makes. Nothing touches hardware.
"""
from fdt_harness import PRELUDE, REPO, definition, run


def test_rtbuddyseg_logs_named_from_the_adt(tmp_path):
    source = (REPO / "src/kboot.c").read_text()
    header = (REPO / "src/adt.h").read_text()
    layout_start = header.index("struct adt_segment_ranges {")
    layout = header[layout_start:header.index("} PACKED;", layout_start) + len("} PACKED;")]
    foreach_start = header.index("#define ADT_FOREACH_CHILD(adt, node)")
    foreach = header[foreach_start:header.index("\n\n", foreach_start)]
    retained_start = source.index("static struct {\n    u64 base, size;\n} retained_rsv[4];")
    retained = source[retained_start:source.index("\n}\n", retained_start) + 3]
    named_start = source.index("#define FW_LOG_NAMED_MAX")
    named_record = source[named_start:source.index(";\n", source.index("}", named_start)) + 2]
    named_record += source[source.index("static unsigned fw_log_named_count"):].split("\n", 1)[0]
    for signature in ("static void dt_reserve_rtbuddyseg_logs(", "static int dt_fw_log_check(",
                      "static void dt_reserve_fw_logs_of(", "static int dt_reserve_retained_extent(",
                      "static bool dt_fw_log_ours(", "static int dt_fw_log_merge(",
                      "static u32 adt_count_fw_logs("):
        assert "chip_id" not in definition(source, signature)
    code = PRELUDE + r'''
#include <stddef.h>
#include <stdlib.h>
#define PACKED __attribute__((packed))
#define SZ_16K 0x4000UL
#define ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define ALIGN_UP(x, a) ALIGN_DOWN((x) + (a) - 1, a)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
''' + layout + "\n" + foreach + r'''
/* The last message, to check the counts the summary gives, and all of them */
static char last[256], logbuf[4096];
#define printf(...)                                                                               \
    (snprintf(last, sizeof(last), __VA_ARGS__),                                                    \
     strncat(logbuf, last, sizeof(logbuf) - strlen(logbuf) - 1))
#define ADT_GETPROP_ARRAY(adt, node, name, arr) adt_getprop_copy(adt, node, name, (arr), sizeof(arr))
static void *adt;
static struct { u64 phys_base, mem_size, top_of_kernel_data; } cur_boot_args;
static u64 relocated_from, image_base, image_end, high_water, window, window_size;
#define _base ((char *)(uintptr_t)image_base)
#define _end ((char *)(uintptr_t)image_end)
static u64 heapblock_high_water(void) { return high_water; }
static bool in_exec_window(u64 start, u64 size)
{
    return window_size && start < window + window_size && start + size > window;
}
#define dt_reserve_outside_exec_window(start, size) fdt_add_mem_rsv(dt, start, size)

/* The ADT: /chosen/memory-map (1), /arm-io (2) and its children, each with one nub */
struct fake {
    const char *name;
    int parent;
    struct adt_segment_ranges seg[264];
    int nseg;
    const char *names;
};
static struct fake nodes[24];
static int nnodes;
static u64 rtbuddy[2];
static bool has_rtbuddy, has_map;
static int add(const char *name, int parent)
{
    nodes[nnodes] = (struct fake){.name = name, .parent = parent};
    return nnodes++;
}
static void seg(int n, u64 phys, u32 size, u32 flags)
{
    nodes[n].seg[nodes[n].nseg++] = (struct adt_segment_ranges){phys, 0, phys, size, flags};
}
/* An owner and its nub, both repeating the same segments */
static void owner(const char *name, const char *nub, const char *names, u64 log, u32 size)
{
    int pair[2] = {add(name, 2), 0};
    pair[1] = add(nub, pair[0]);
    for (int i = 0; i < 2; i++) {
        seg(pair[i], 0x10000000000UL, 0x4000, 0);
        seg(pair[i], log, size, 0xa);
        nodes[pair[i]].names = names;
    }
}
static int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    if (!strcmp(path, "/chosen/memory-map"))
        return has_map ? 1 : -1;
    return !strcmp(path, "/arm-io") ? 2 : -1;
}
static int adt_get_child_count(const void *a, int node)
{
    (void)a;
    int n = 0;
    for (int i = 3; i < nnodes; i++)
        n += nodes[i].parent == node;
    return n;
}
static int adt_next_child(int node, int after)
{
    for (int i = after + 1; i < nnodes; i++)
        if (nodes[i].parent == node)
            return i;
    return -1;
}
static int adt_first_child_offset(const void *a, int node) { (void)a; return adt_next_child(node, 2); }
static int adt_next_sibling_offset(const void *a, int node)
{
    (void)a;
    return adt_next_child(nodes[node].parent, node);
}
static const char *adt_get_name(const void *a, int node) { (void)a; return nodes[node].name; }
static const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (node == 1 && !strcmp(name, "RTBuddySeg") && has_rtbuddy) {
        if (len)
            *len = sizeof(rtbuddy);
        return rtbuddy;
    }
    if (node < 3)
        return NULL;
    if (!strcmp(name, "segment-ranges") && nodes[node].nseg) {
        *len = nodes[node].nseg * sizeof(struct adt_segment_ranges);
        return nodes[node].seg;
    }
    if (!strcmp(name, "segment-names") && nodes[node].names) {
        *len = strlen(nodes[node].names) + 1;
        return nodes[node].names;
    }
    return NULL;
}
static int adt_getprop_copy(const void *a, int node, const char *name, void *out, size_t size)
{
    u32 len = 0;
    const void *v = adt_getprop(a, node, name, &len);
    if (!v || len != size)
        return -1;
    memcpy(out, v, size);
    return len;
}
''' + definition(source, "static int dt_get_or_add_reserved_mem(") + retained + \
        definition(source, "static bool fw_log_overlaps(") + \
        named_record + "\n" + \
        definition(source, "static bool dt_fw_log_ours(") + \
        definition(source, "static int dt_fw_log_check(") + \
        definition(source, "static bool fw_log_inside(") + \
        definition(source, "static int dt_fw_log_merge(") + \
        definition(source, "static bool adt_segment_named(") + \
        definition(source, "static u32 adt_count_fw_logs(") + \
        definition(source, "static void dt_reserve_fw_logs_of(") + \
        definition(source, "static void dt_reserve_rtbuddyseg_logs(") + r'''
#define RAM 0x10000000000UL
#define AOP 0x10004600000UL
#define MTP 0x1000462c000UL
#define DCP 0x10004594000UL
/* The J713's (T8132) layout: no T6050 anywhere. */
static void reset(void)
{
    fresh();
    node_at("/", "reserved-memory");
    memset(nodes, 0, sizeof(nodes));
    nnodes = 3;
    has_map = has_rtbuddy = true;
    rtbuddy[0] = DCP;
    rtbuddy[1] = 0x9c000;
    owner("aop", "iop-aop-nub", "__TEXT;__OS_LOG", AOP, 0x2a000);
    owner("mtp", "iop-mtp-nub", "__TEXT;__OS_LOG", MTP, 0x3000);
    owner("dcp", "iop-dcp-nub", "__TEXT;__OS_LOG;unknown", DCP, 0x24000);
    cur_boot_args.phys_base = RAM;
    cur_boot_args.mem_size = 0x200000000UL;
    image_base = RAM + 0x5000000;
    image_end = image_base + 0x400000;
    cur_boot_args.top_of_kernel_data = image_base;
    high_water = image_end;
    relocated_from = window = window_size = 0;
    retained_rsv_count = 0;
    logbuf[0] = 0;
    /* The image/high-water envelope retained from the boot objects below aura */
    assert(!dt_reserve_retained_extent(RAM, image_base - RAM));
}
/* The os-log node at @base, or -1; it must be no-map and of @size */
static int named(u64 base, u64 size)
{
    char name[32];
    snprintf(name, sizeof(name), "os-log@%lx", (unsigned long)base);
    int node = fdt_subnode_offset(tree, fdt_path_offset(tree, "/reserved-memory"), name);
    if (node < 0)
        return -1;
    int len;
    const fdt64_t *r = fdt_getprop(tree, node, "reg", &len);
    assert(r && len == 16 && fdt64_ld(&r[0]) == base && fdt64_ld(&r[1]) == size);
    assert(fdt_getprop(tree, node, "no-map", NULL));
    assert(!fdt_node_check_compatible(tree, node, "apple,os-log"));
    return node;
}
static void summary(int named, int held, int left)
{
    char want[96];
    snprintf(want, sizeof(want), "%d named no-map, %d already reserved, %d left", named, held,
             left);
    if (!strstr(last, want))
        fprintf(stderr, "summary: %s", last);
    assert(strstr(last, want));
}
static int count(void)
{
    int n = 0, node;
    fdt_for_each_subnode(node, tree, fdt_path_offset(tree, "/reserved-memory"))
        n++;
    return n;
}
int main(void)
{
    int cases = 0;
    /* Each log once (owner and nub repeat it), rounded to 16K, inside the envelope. */
    reset();
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0 && named(DCP, 0x24000) >= 0);
    assert(count() == 3);
    summary(3, 3, 0); cases++;
    /* A log a display handoff already reserved stays as it is. */
    reset();
    int held = node_at("/reserved-memory", "dcp-oslog@10004594000");
    reg(held, DCP, 0x24000);
    assert(!fdt_setprop_empty(tree, held, "no-map"));
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) < 0 && named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0);
    assert(count() == 3);
    summary(2, 4, 0); cases++;
    /* One straddling a reserved-memory node, one meeting another memory reservation and one in
     * the OS's execute window are left as they were; the others are still named. */
    reset();
    held = node_at("/reserved-memory", "fw@10004610000");
    reg(held, AOP + 0x10000, 0x10000);
    assert(!fdt_add_mem_rsv(tree, MTP, 0x1000));
    window = DCP;
    window_size = 0x4000;
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP, 0x2c000) < 0 && named(MTP, 0x4000) < 0 && named(DCP, 0x24000) < 0);
    assert(count() == 1);
    summary(0, 0, 6); cases++;
    /* The one in the execute window says so, apart from the live boot allocations. */
    assert(strstr(logbuf, "firmware log 0x10004594000+0x24000 lies in the OS's execute window"));
    assert(!strstr(logbuf, "0x10004594000+0x24000 overlaps a live boot allocation"));
    /* A log on aura's live allocations is left alone. */
    reset();
    image_base = AOP;
    image_end = AOP + 0x4000;
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP, 0x2c000) < 0 && named(MTP, 0x4000) >= 0 && count() == 2);
    summary(2, 2, 2); cases++;
    /* A log outside RTBuddySeg, or a 0xa segment of another name, is not named. */
    reset();
    rtbuddy[1] = MTP - DCP;
    nodes[4].names = nodes[3].names = "__TEXT;__DATA";
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) >= 0 && named(AOP, 0x2c000) < 0 && named(MTP, 0x4000) < 0);
    assert(count() == 1);
    summary(1, 1, 2); cases++;
    /* A log that meets no memory reservation at all lies in RAM the OS would take: named. */
    reset();
    fresh();
    node_at("/", "reserved-memory");
    retained_rsv_count = 0;
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0 && named(DCP, 0x24000) >= 0);
    assert(fdt_num_mem_rsv(tree) == 0 && count() == 3);
    summary(3, 3, 0); cases++;
    /* A disabled node already named os-log@<base> is neither rewritten nor removed. */
    reset();
    held = node_at("/reserved-memory", "os-log@10004600000");
    reg(held, AOP, 0x2c000);
    assert(!fdt_setprop_string(tree, held, "status", "disabled"));
    dt_reserve_rtbuddyseg_logs();
    held = fdt_subnode_offset(tree, fdt_path_offset(tree, "/reserved-memory"),
                              "os-log@10004600000");
    assert(held >= 0 && !strcmp(fdt_getprop(tree, held, "status", NULL), "disabled"));
    assert(!fdt_getprop(tree, held, "no-map", NULL) && !fdt_getprop(tree, held, "compatible", NULL));
    assert(named(MTP, 0x4000) >= 0 && named(DCP, 0x24000) >= 0 && count() == 3);
    summary(2, 2, 2); cases++;
    /* A reusable node gives its pages to the OS: a log inside one is refused, the node kept. */
    reset();
    held = node_at("/reserved-memory", "cma@10004590000");
    reg(held, DCP - 0x4000, 0x30000);
    assert(!fdt_setprop_empty(tree, held, "reusable"));
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) < 0 && named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0);
    assert(strstr(logbuf, "0x10004594000+0x24000 lies in reusable reserved-memory/cma@"));
    assert(count() == 3);
    summary(2, 2, 2); cases++;
    /* A node that is neither no-map nor reusable keeps its pages from the OS: it holds the log. */
    reset();
    held = node_at("/reserved-memory", "fw@10004594000");
    reg(held, DCP, 0x24000);
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) < 0 && count() == 3);
    summary(2, 4, 0); cases++;
    /* Linux reserves only an available node ("okay", "ok" or no status): a log inside a
     * "reserved" one is still named. */
    reset();
    held = node_at("/reserved-memory", "fw@10004594000");
    reg(held, DCP, 0x24000);
    assert(!fdt_setprop_string(tree, held, "status", "reserved"));
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) >= 0 && named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0);
    assert(count() == 4);
    summary(3, 3, 0); cases++;
    /* Linux reserves only a node's first reg entry: a log inside a later one is still named. */
    reset();
    held = node_at("/reserved-memory", "fw@10100000000");
    fdt64_t two[4] = {cpu_to_fdt64(RAM + 0x100000000UL), cpu_to_fdt64(0x4000), cpu_to_fdt64(DCP),
                      cpu_to_fdt64(0x24000)};
    assert(!fdt_setprop(tree, held, "reg", two, sizeof(two)));
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) >= 0 && named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0);
    assert(count() == 4);
    summary(3, 3, 0); cases++;
    /* A log sharing the last 16K page of one named before it grows that node over both. */
    reset();
    owner("sio", "iop-sio-nub", "__TEXT;__OS_LOG", DCP + 0x23000, 0x2000);
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x28000) >= 0 && named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0);
    assert(count() == 3);
    summary(4, 4, 0); cases++;
    /* One sharing the first page of a later base: the node takes the lower base and its name. */
    reset();
    owner("sio", "iop-sio-nub", "__TEXT;__OS_LOG", AOP - 0x1000, 0x2000);
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP - 0x4000, 0x30000) >= 0 && named(AOP, 0x2c000) < 0);
    assert(named(MTP, 0x4000) >= 0 && named(DCP, 0x24000) >= 0 && count() == 3);
    summary(4, 4, 0); cases++;
    /* One bridging two named logs joins all three into the lower node. */
    reset();
    owner("pmp", "iop-pmp-nub", "__TEXT;__OS_LOG", DCP + 0x2c000, 0x4000);
    owner("sep", "iop-sep-nub", "__TEXT;__OS_LOG", DCP + 0x34000, 0x4000);
    owner("sio", "iop-sio-nub", "__TEXT;__OS_LOG", DCP + 0x2f000, 0x6000);
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP + 0x2c000, 0xc000) >= 0 && named(DCP + 0x34000, 0x4000) < 0);
    assert(named(DCP, 0x24000) >= 0 && count() == 4);
    summary(6, 6, 0); cases++;
    /* The rest of a page-sharing log meets another reservation: it is refused, the node kept. */
    reset();
    owner("sio", "iop-sio-nub", "__TEXT;__OS_LOG", DCP + 0x23000, 0x2000);
    assert(!fdt_add_mem_rsv(tree, DCP + 0x24000, 0x4000));
    dt_reserve_rtbuddyseg_logs();
    assert(named(DCP, 0x24000) >= 0 && count() == 3);
    summary(3, 3, 2); cases++;
    /* The lower name is taken by a node the scan passed over: refused, nothing renamed. */
    reset();
    owner("sio", "iop-sio-nub", "__TEXT;__OS_LOG", AOP - 0x1000, 0x2000);
    held = node_at("/reserved-memory", "os-log@100045fc000");
    assert(!fdt_setprop_string(tree, held, "status", "disabled"));
    dt_reserve_rtbuddyseg_logs();
    assert(named(AOP, 0x2c000) >= 0 && count() == 4);
    held = fdt_subnode_offset(tree, fdt_path_offset(tree, "/reserved-memory"),
                              "os-log@100045fc000");
    assert(held >= 0 && !fdt_getprop(tree, held, "reg", NULL));
    summary(3, 3, 2); cases++;
    /* Each of 36 logs on its own 16K page is named, and the 33rd one still grows over the
     * page-sharing log after it: the record holds more than 32 nodes. */
    reset();
    rtbuddy[1] = 0x300000;
    static char many_names[40 * 9];
    many_names[0] = 0;
    int many = add("many", 2);
    for (int i = 0; i < 37; i++) {
        strncat(many_names, i ? ";__OS_LOG" : "__OS_LOG",
                sizeof(many_names) - strlen(many_names) - 1);
        if (i < 36)
            seg(many, DCP + 0x100000 + i * 0x8000UL, 0x1000, 0xa);
    }
    seg(many, DCP + 0x100000 + 32 * 0x8000UL + 0x3000, 0x2000, 0xa);
    nodes[many].names = many_names;
    dt_reserve_rtbuddyseg_logs();
    for (int i = 0; i < 36; i++)
        assert(named(DCP + 0x100000 + i * 0x8000UL, i == 32 ? 0x8000 : 0x4000) >= 0);
    assert(named(AOP, 0x2c000) >= 0 && named(MTP, 0x4000) >= 0 && named(DCP, 0x24000) >= 0);
    assert(count() == 3 + 36);
    summary(3 + 37, 3, 0); cases++;
    /* More __OS_LOG segments than the record holds (both copies of the three owners, and
     * 251 in one 16K page): the tree is not touched. With one fewer the first is named and
     * holds the rest. */
    static unsigned char before[sizeof(tree)];
    static char more_names[260 * 9];
    for (int extra = 251; extra >= 250; extra--) {
        reset();
        rtbuddy[1] = 0x1000000;
        more_names[0] = 0;
        int more = add("more", 2);
        for (int i = 0; i < extra; i++) {
            strncat(more_names, i ? ";__OS_LOG" : "__OS_LOG",
                    sizeof(more_names) - strlen(more_names) - 1);
            seg(more, DCP + 0x100000 + i * 0x40UL, 0x40, 0xa);
        }
        nodes[more].names = more_names;
        memcpy(before, tree, sizeof(tree));
        dt_reserve_rtbuddyseg_logs();
        if (extra == 251) {
            assert(!memcmp(before, tree, sizeof(tree)));
            assert(strstr(logbuf, "257 firmware log segments exceed the record of 256"));
        } else {
            assert(named(DCP + 0x100000, 0x4000) >= 0 && count() == 4);
            summary(4, 3 + 249, 0);
        }
        cases++;
    }
    /* No RTBuddySeg, no memory map, or one outside RAM: the tree is not touched. */
    for (int i = 0; i < 3; i++) {
        reset();
        if (i == 0)
            has_rtbuddy = false;
        else if (i == 1)
            has_map = false;
        else
            rtbuddy[0] = RAM + cur_boot_args.mem_size;
        memcpy(before, tree, sizeof(tree));
        dt_reserve_rtbuddyseg_logs();
        assert(!memcmp(before, tree, sizeof(tree)));
        cases++;
    }
    fprintf(stdout, "PASS rtbuddyseg logs %d cases\n", cases);
    return 0;
}
'''
    run(tmp_path, "rtbuddyseg", code, "PASS rtbuddyseg logs 22 cases")


def test_rtbuddyseg_pass_runs_on_every_preparation_path():
    # T8152 prepares its tree on its own path; both name the logs before the memory node is set.
    source = (REPO / "src/kboot.c").read_text()
    t8152 = definition(source, "static int dt_prepare_t8152(void)")
    generic = source[source.index("int kboot_prepare_dt(void *fdt)"):]
    generic = generic[generic.index("return dt_prepare_t8152();"):]
    for path in (t8152, generic):
        assert path.index("dt_reserve_rtbuddyseg_logs();") < path.index("dt_set_memory()")
