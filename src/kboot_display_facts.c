/* SPDX-License-Identifier: MIT */

#include <stdarg.h>

#include "kboot_display_facts.h"
#include "adt.h"
#include "malloc.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"

#include "libfdt/libfdt.h"

/*
 * T6030 (M3 Pro): the ADT's description of the external display blocks and the Type-C PHYs, for
 * Linux-side inspection, as /chosen/aurora,t6030-display-facts. It is not a device binding and no
 * driver reads it. adt,* properties are raw ADT bytes (little-endian cells); translated-reg (an
 * address and a size per reg entry, translated through the bus ranges) and adt-property-sizes are
 * big-endian cells.
 *
 *   atc-phy0 .. atc-phy3   compatible, reg, interrupts, every property name and size, and the
 *                          value of each property whose name mentions DP, HDMI or AUX
 *                          (up to 256 bytes; larger ones by name and size only)
 *   dcpext0, dcpext1, dispext0, dispext1, dart-dcpext0, dart-dcpext1, dart-dispext0,
 *   dart-dispext1          compatible, reg, interrupts
 *   any /arm-io node whose name contains xbar or crossbar
 *                          as the PHYs, with every value up to 64 bytes
 *   any /arm-io node whose name contains hdmi (dp2hdmi-gpio0)
 *                          every property name and size, and every value up to 256 bytes
 *   any /arm-io node whose name contains dptx
 *                          compatible, reg, interrupts, every property name and size
 *   skipped                what was absent or over a limit
 *
 * ADT memory only: no device register is read or written. Bounded: at most four levels below
 * /arm-io and 4096 nodes are visited, eight nodes per kind found by name, 512 property names per
 * node and 16 translated reg entries per node. Every write is charged, before it is made, with
 * what it adds to the FDT (node and property headers, padding and name strings), and the writes
 * stop at 40 KiB; with `skipped` (at most 32 lines) and the node itself, the export never takes
 * more than 46 KiB of the 64 KiB kboot_prepare_dt() adds for it. The node is built on a copy of
 * the FDT and published only if complete; an existing node is never replaced, and a failure
 * leaves the FDT as it was without stopping the boot.
 *
 * The ADT is read here, not through adt.h: a lookup by name there stops the boot if any earlier
 * property of the node has a name it cannot read, and this export looks at many more nodes than
 * the boot needs. Every node and property header is checked to lie inside the ADT before it is
 * read, and every value before it is used or copied. A malformed entry ends the search at that
 * point and is reported in `skipped`; what was found before it is still published.
 */

#define FACTS_NODE   "aurora,t6030-display-facts"
#define FACTS_SCHEMA 1
#define LOG          "ADT facts: T6030: "

#define BUDGET       (40 * 1024)
#define MAX_NAMES    512
#define MAX_REGS     16
#define MAX_FOUND    8
#define MAX_SKIPS    32
#define WALK_DEPTH   4
#define WALK_NODES   4096
#define BASIC_MAX    1024 /* compatible, reg, interrupts */
#define ADT_NAME_LEN 32   /* the ADT name field */
#define PATH_LEN     176  /* "/arm-io" and four names of up to 31 characters, with separators */
#define CHAIN_LEN    (WALK_DEPTH + 2) /* the root, /arm-io and four levels below it */

/* FDT structure padding */
#define TAGALIGN(x) (((x) + FDT_TAGSIZE - 1) & ~(u32)(FDT_TAGSIZE - 1))

/* The ADT's binary layout and the limits the ADT library applies */
#define ADT_NODE_HDR  8  /* property count, child count */
#define ADT_PROP_HDR  36 /* name, size */
#define ADT_MAX_PROPS 2048
#define ADT_MAX_KIDS  2048
#define ADT_NEST      16         /* levels followed below a node to find where its subtree ends */
#define ADT_MAX_SIZE  (64 << 20) /* larger is not an ADT, and keeps offsets far from overflow */

/* Which property values a node exports, besides compatible, reg and interrupts */
enum values {
    V_NONE,   /* none */
    V_DP,     /* names mentioning DP, HDMI or AUX, up to 256 bytes */
    V_SMALL,  /* every value up to 64 bytes */
    V_ALL,    /* every value up to 256 bytes */
    V_NOLIST, /* none, and no property names */
};

/* Nodes found by name below /arm-io */
enum kind { K_XBAR, K_HDMI, K_DPTX, K_NR };

struct dfacts {
    void *dt;
    int chosen; /* /chosen; its offset does not move as its own subtree grows */
    int err;    /* the first libfdt error */
    u32 bytes;  /* what the charged writes added to the FDT, at most, against BUDGET */
    bool over;  /* BUDGET reached (reported once): nothing more is written but skipped lines */
    bool stop;  /* the search below /arm-io ended */
    int skips;
    int visited;
    int found[K_NR];
};

/* ---- FDT writes, into the copy; the first error sticks ---- */

static int f_node(struct dfacts *f, const char *sub)
{
    int node;

    if (f->err)
        return -1;
    node = fdt_subnode_offset(f->dt, f->chosen, FACTS_NODE);
    if (node >= 0 && sub)
        node = fdt_subnode_offset(f->dt, node, sub);
    if (node < 0)
        f->err = node;
    return node;
}

static void f_set(struct dfacts *f, const char *sub, const char *name, const void *val, int len)
{
    int node = f_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_setprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void f_append(struct dfacts *f, const char *sub, const char *name, const void *val, int len)
{
    int node = f_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_appendprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void __attribute__((format(printf, 2, 3))) f_skip(struct dfacts *f, const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    if (f->skips >= MAX_SKIPS)
        return;
    f->skips++;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf(LOG "%s\n", buf);
    f_append(f, NULL, "skipped", buf, strlen(buf) + 1);
}

/* Takes n bytes of the budget, or reports (once) that it is used up */
static bool f_charge(struct dfacts *f, u32 n)
{
    if (f->over || n > BUDGET - f->bytes) {
        if (!f->over)
            f_skip(f, "device tree budget of %u bytes reached; later entries left out", BUDGET);
        f->over = true;
        return false;
    }
    f->bytes += n;
    return true;
}

/* At most what a subnode adds to the FDT: its begin and end tags and its padded name */
static u32 node_cost(const char *sub)
{
    return 2 * FDT_TAGSIZE + TAGALIGN(strlen(sub) + 1);
}

/*
 * At most what writing len bytes as property `name` adds to the FDT: appended to an existing
 * property, the growth of its padded value; otherwise a property header, the padded value and the
 * name in the string table.
 */
static u32 prop_cost(struct dfacts *f, const char *sub, const char *name, u32 len, bool append)
{
    int node = f_node(f, sub), old = 0;

    if (node >= 0 && append && fdt_getprop(f->dt, node, name, &old) && old >= 0)
        return TAGALIGN((u32)old + len) - TAGALIGN((u32)old);
    return sizeof(struct fdt_property) + TAGALIGN(len) + strlen(name) + 1;
}

/* Sets (or appends) a property if the budget has room for it */
static bool f_put(struct dfacts *f, const char *sub, const char *name, const void *val, u32 len,
                  bool append)
{
    if (f->err || !f_charge(f, prop_cost(f, sub, name, len, append)))
        return false;
    if (append)
        f_append(f, sub, name, val, len);
    else
        f_set(f, sub, name, val, len);
    return !f->err;
}

static bool f_str(struct dfacts *f, const char *sub, const char *name, const char *s)
{
    return f_put(f, sub, name, s, strlen(s) + 1, false);
}

/* ---- ADT names ---- */

/*
 * An ADT node or property name, NUL-terminated, if it is 1..31 printable characters terminated
 * within its field. Returns false otherwise.
 */
static bool name_ok(const char *s, size_t field, char *out)
{
    size_t n = strnlen(s, field);

    if (!n || n >= ADT_NAME_LEN || n == field)
        return false;
    for (size_t i = 0; i < n; i++)
        if (s[i] < 0x21 || s[i] > 0x7e)
            return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

/* Whether a name is also usable in a device tree node name (`node`) or property name */
static bool fdt_name_ok(const char *s, bool node)
{
    for (; *s; s++) {
        char c = *s;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == ',' || c == '.' || c == '_' || c == '+' || c == '-' || (!node && c == '#')))
            return false;
    }
    return true;
}

/* ---- the ADT, read with bounds ---- */

static u32 adt_bytes; /* the size of the ADT */

struct anode {
    u32 off;    /* the node header */
    u32 nprops; /* 1..ADT_MAX_PROPS */
    u32 nkids;  /* 0..ADT_MAX_KIDS */
    u32 kids;   /* where its properties end: its first child, if it has one */
};

struct aprop {
    const char *name; /* the 32-byte name field, not necessarily terminated */
    const u8 *value;
    u32 size;
    u32 next; /* where the next property starts */
};

static u32 a_u32(u32 off)
{
    u32 v;

    memcpy(&v, (const u8 *)adt + off, sizeof(v));
    return v;
}

/* The property at off, if its header and its value lie inside the ADT */
static bool a_prop(u32 off, struct aprop *p)
{
    if (off % 4 || off > adt_bytes || adt_bytes - off < ADT_PROP_HDR)
        return false;
    p->size = a_u32(off + ADT_NAME_LEN);
    if (p->size > adt_bytes - off - ADT_PROP_HDR)
        return false;
    p->name = (const char *)adt + off;
    p->value = (const u8 *)adt + off + ADT_PROP_HDR;
    p->next = off + ADT_PROP_HDR + ((p->size + 3) & ~3u);
    return true;
}

/* The node at off, if its header and all of its properties lie inside the ADT */
static bool a_node(u32 off, struct anode *n)
{
    struct aprop p;

    if (off % 4 || off > adt_bytes || adt_bytes - off < ADT_NODE_HDR)
        return false;
    n->off = off;
    n->nprops = a_u32(off);
    n->nkids = a_u32(off + 4);
    if (!n->nprops || n->nprops > ADT_MAX_PROPS || n->nkids > ADT_MAX_KIDS)
        return false;
    off += ADT_NODE_HDR;
    for (u32 i = 0; i < n->nprops; i++, off = p.next)
        if (!a_prop(off, &p))
            return false;
    n->kids = off;
    return true;
}

/* The first property of n named exactly `name`, or NULL */
static const u8 *a_getprop(const struct anode *n, const char *name, u32 *len)
{
    size_t nl = strlen(name);
    struct aprop p;
    u32 off = n->off + ADT_NODE_HDR;

    for (u32 i = 0; i < n->nprops && a_prop(off, &p); i++, off = p.next)
        if (strnlen(p.name, ADT_NAME_LEN) == nl && !memcmp(p.name, name, nl)) {
            *len = p.size;
            return p.value;
        }
    return NULL;
}

/* Where the subtree of n ends (where its next sibling starts), if it is well formed */
static bool a_end(const struct anode *n, u32 *end)
{
    u32 left[ADT_NEST];
    u32 off = n->kids;
    int d = 0;

    left[0] = n->nkids;
    while (d >= 0) {
        struct anode c;

        if (!left[d]) {
            d--;
            continue;
        }
        left[d]--;
        if (!a_node(off, &c))
            return false;
        off = c.kids; /* every node takes at least one property: off only grows */
        if (c.nkids) {
            if (++d >= ADT_NEST)
                return false;
            left[d] = c.nkids;
        }
    }
    *end = off;
    return true;
}

/* The children of a node, in order; a child's end is only looked for when the next is asked */
struct akids {
    u32 left;
    u32 off;
    struct anode prev;
    bool started;
    bool bad; /* malformed: the children from here on cannot be found */
};

static void a_kids(const struct anode *parent, struct akids *it)
{
    memset(it, 0, sizeof(*it));
    it->left = parent->nkids;
    it->off = parent->kids;
}

static bool a_next(struct akids *it, struct anode *c)
{
    if (!it->left || it->bad)
        return false;
    if ((it->started && !a_end(&it->prev, &it->off)) || !a_node(it->off, c)) {
        it->bad = true;
        return false;
    }
    it->left--;
    it->prev = *c;
    it->started = true;
    return true;
}

/* The name of n, if it is 1..31 printable characters */
static bool a_name(const struct anode *n, char *out)
{
    u32 len = 0;
    const u8 *s = a_getprop(n, "name", &len);

    return s && len && name_ok((const char *)s, min(len, (u32)ADT_NAME_LEN), out);
}

static u64 a_cells(const u8 *p, u32 n)
{
    u64 v = 0;

    for (u32 i = 0; i < n; i++) {
        u32 x;
        memcpy(&x, p + 4 * i, sizeof(x));
        v |= (u64)x << (32 * i);
    }
    return v;
}

static bool a_ncells(const struct anode *n, const char *name, u32 *v)
{
    u32 len = 0;
    const u8 *p = a_getprop(n, name, &len);

    if (!p || len != sizeof(*v))
        return false;
    memcpy(v, p, sizeof(*v));
    return true;
}

/*
 * Entry idx of the reg of chain[depth - 1], translated through the ranges of its ancestors
 * (chain[0] is the root), as the ADT library's adt_get_reg() translates it: the cells are the
 * parent's #address-cells and #size-cells, and a bus without ranges ends the translation.
 */
static bool a_reg(const struct anode *chain, int depth, u32 idx, u64 *paddr, u64 *psize)
{
    int cur = depth - 1;
    u32 ac, sc, len = 0;

    if (cur < 1 || !a_ncells(&chain[cur - 1], "#address-cells", &ac) ||
        !a_ncells(&chain[cur - 1], "#size-cells", &sc) || ac < 1 || ac > 2 || sc > 2)
        return false;
    const u8 *reg = a_getprop(&chain[cur], "reg", &len);
    if (!reg || !len || len < (idx + 1) * (ac + sc) * 4)
        return false;
    reg += idx * (ac + sc) * 4;
    u64 addr = a_cells(reg, ac), size = a_cells(reg + 4 * ac, sc);

    while (cur > 1) {
        u32 pac;

        cur--;
        const u8 *ranges = a_getprop(&chain[cur], "ranges", &len);
        if (!ranges)
            break;
        if (!a_ncells(&chain[cur - 1], "#address-cells", &pac) || pac < 1 || pac > 2 || sc > 2)
            return false;
        u32 entry = 4 * (ac + pac + sc);
        for (u32 o = 0; entry <= len - o; o += entry) {
            u64 ca = a_cells(ranges + o, ac), pa = a_cells(ranges + o + 4 * ac, pac);
            u64 cs = a_cells(ranges + o + 4 * (ac + pac), sc);
            if (addr >= ca && size <= cs && addr - ca <= cs - size) {
                addr = addr - ca + pa;
                break;
            }
        }
        if (!a_ncells(&chain[cur - 1], "#size-cells", &sc))
            return false;
        ac = pac;
    }
    *paddr = addr;
    *psize = size;
    return true;
}

static bool contains(const char *s, const char *word)
{
    size_t n = strlen(word);

    for (; *s; s++) {
        size_t i = 0;
        while (i < n && s[i] && (s[i] | 0x20) == word[i])
            i++;
        if (i == n)
            return true;
    }
    return false;
}

static bool value_wanted(enum values v, const char *name, u32 size)
{
    if (!strcmp(name, "compatible") || !strcmp(name, "reg") || !strcmp(name, "interrupts"))
        return false; /* copied already */
    switch (v) {
        case V_DP:
            return size <= 256 &&
                   (contains(name, "dp") || contains(name, "hdmi") || contains(name, "aux"));
        case V_SMALL:
            return size <= 64;
        case V_ALL:
            return size <= 256;
        default:
            return false;
    }
}

/* ---- one node ---- */

/* adt,<name> = the ADT bytes, if they are at most max bytes */
static void put_value(struct dfacts *f, const char *sub, const char *name, const u8 *val, u32 len,
                      u32 max)
{
    char fname[ADT_NAME_LEN + 4];

    if (len > max) {
        f_skip(f, "%s %s: %u bytes, over %u", sub, name, len, max);
        return;
    }
    snprintf(fname, sizeof(fname), "adt,%s", name);
    f_put(f, sub, fname, val, len, false);
}

static void copy_prop(struct dfacts *f, const char *sub, const struct anode *node, const char *name,
                      u32 max)
{
    u32 len = 0;
    const u8 *p = a_getprop(node, name, &len);

    if (p)
        put_value(f, sub, name, p, len, max);
}

/* chain[0..depth - 1]: the root down to the node, which is at path */
static void export_node(struct dfacts *f, const char *sub, const char *path,
                        const struct anode *chain, int depth, enum values v)
{
    const struct anode *node = &chain[depth - 1];
    u32 len = 0;
    int fdt;

    if (f->err)
        return;
    fdt = fdt_subnode_offset(f->dt, f_node(f, NULL), sub);
    if (fdt >= 0) {
        f_skip(f, "%s: a node named %s is exported already", path, sub);
        return;
    }
    /* The node and its adt-path, charged together */
    if (!f_charge(f, node_cost(sub) + prop_cost(f, NULL, "adt-path", strlen(path) + 1, false)))
        return;
    fdt = fdt_add_subnode(f->dt, f_node(f, NULL), sub);
    if (fdt < 0) {
        if (!f->err)
            f->err = fdt;
        return;
    }
    f_set(f, sub, "adt-path", path, strlen(path) + 1);

    copy_prop(f, sub, node, "compatible", BASIC_MAX);
    copy_prop(f, sub, node, "reg", BASIC_MAX);
    copy_prop(f, sub, node, "interrupts", BASIC_MAX);
    if (a_getprop(node, "reg", &len)) {
        for (u32 r = 0; r < MAX_REGS; r++) {
            u64 addr, size;
            fdt64_t cells[2];
            if (!a_reg(chain, depth, r, &addr, &size))
                break;
            cells[0] = cpu_to_fdt64(addr);
            cells[1] = cpu_to_fdt64(size);
            if (!f_put(f, sub, "translated-reg", cells, sizeof(cells), true))
                break;
        }
    }
    if (v == V_NOLIST)
        return;

    struct aprop p;
    u32 off = node->off + ADT_NODE_HDR;
    for (u32 i = 0; i < node->nprops && a_prop(off, &p); i++, off = p.next) {
        char name[ADT_NAME_LEN];
        fdt32_t size = cpu_to_fdt32(p.size);

        if (f->err || f->over)
            break;
        if (i >= MAX_NAMES) {
            f_skip(f, "%s: over %d properties, the rest left out", path, MAX_NAMES);
            break;
        }
        if (!name_ok(p.name, ADT_NAME_LEN, name)) {
            f_skip(f, "%s: property %u has an unusable name", path, i);
            continue;
        }
        /* The two lists stay the same length: both are charged before either is written */
        if (!f_charge(f, prop_cost(f, sub, "adt-property-names", strlen(name) + 1, true) +
                             prop_cost(f, sub, "adt-property-sizes", sizeof(size), true)))
            break;
        f_append(f, sub, "adt-property-names", name, strlen(name) + 1);
        f_append(f, sub, "adt-property-sizes", &size, sizeof(size));
        if (value_wanted(v, name, p.size) && fdt_name_ok(name, false))
            put_value(f, sub, name, p.value, p.size, 256);
    }
}

/* ---- nodes found by name below /arm-io ---- */

static const struct {
    const char *words[2];
    const char *what;
    enum values v;
} kinds[K_NR] = {
    [K_XBAR] = {{"xbar", "crossbar"}, "crossbar", V_SMALL},
    [K_HDMI] = {{"hdmi", NULL}, "hdmi", V_ALL},
    [K_DPTX] = {{"dptx", NULL}, "dptx", V_NONE},
};

static void visit(struct dfacts *f, const struct anode *chain, int depth, const char *path,
                  const char *name)
{
    for (int k = 0; k < K_NR; k++) {
        bool match = false;
        for (int w = 0; w < 2; w++)
            match |= kinds[k].words[w] && contains(name, kinds[k].words[w]);
        if (!match)
            continue;
        if (f->found[k] >= MAX_FOUND) {
            if (f->found[k]++ == MAX_FOUND)
                f_skip(f, "over %d %s nodes; %s and later ones left out", MAX_FOUND, kinds[k].what,
                       path);
            return;
        }
        f->found[k]++;
        export_node(f, name, path, chain, depth, kinds[k].v);
        return;
    }
}

/*
 * Visits the children of chain[depth - 1] (at `path`) and their subtrees, down to WALK_DEPTH
 * levels below /arm-io (chain[1]); each child is put in chain[depth]. The end of a child's
 * subtree is only looked for while another child is left.
 */
static void walk(struct dfacts *f, struct anode *chain, int depth, char *path, size_t len)
{
    struct akids it;

    a_kids(&chain[depth - 1], &it);
    while (!f->err && !f->stop && a_next(&it, &chain[depth])) {
        char name[ADT_NAME_LEN];

        if (++f->visited > WALK_NODES) {
            f_skip(f, "over %d ADT nodes; the search stopped", WALK_NODES);
            f->stop = true;
            return;
        }
        if (!a_name(&chain[depth], name) || !fdt_name_ok(name, true))
            continue;
        size_t n = strlen(name);
        if (len + 1 + n + 1 > PATH_LEN)
            continue;
        path[len] = '/';
        memcpy(path + len + 1, name, n + 1);
        visit(f, chain, depth + 1, path, name);
        if (depth - 1 < WALK_DEPTH)
            walk(f, chain, depth + 1, path, len + 1 + n);
        path[len] = '\0';
    }
    if (it.bad && !f->stop) {
        f_skip(f, "%s: malformed ADT entry among its children; the search stopped", path);
        f->stop = true;
    }
}

/* ---- the node ---- */

static const char *const phys[] = {"atc-phy0", "atc-phy1", "atc-phy2", "atc-phy3"};

static const char *const ext[] = {
    "dcpext0",      "dcpext1",      "dispext0",      "dispext1",
    "dart-dcpext0", "dart-dcpext1", "dart-dispext0", "dart-dispext1",
};

#define NFIXED (ARRAY_SIZE(phys) + ARRAY_SIZE(ext))

static const char *fixed_name(size_t i)
{
    return i < ARRAY_SIZE(phys) ? phys[i] : ext[i - ARRAY_SIZE(phys)];
}

/* The first child of parent named `name`; *bad if a malformed entry came first */
static bool a_child(const struct anode *parent, const char *name, struct anode *out, bool *bad)
{
    struct akids it;
    char n[ADT_NAME_LEN];

    a_kids(parent, &it);
    while (a_next(&it, out))
        if (a_name(out, n) && !strcmp(n, name))
            return true;
    *bad = it.bad;
    return false;
}

static void build(struct dfacts *f)
{
    fdt32_t schema = cpu_to_fdt32(FACTS_SCHEMA);
    struct anode chain[CHAIN_LEN], found[NFIXED];
    bool have[NFIXED] = {false};
    char path[PATH_LEN];

    f_put(f, NULL, "schema-version", &schema, sizeof(schema), false);
    f_str(f, NULL, "encoding", "adt-raw-little-endian");

    bool bad = false, root = a_node(0, &chain[0]);
    bool arm_io = root && a_child(&chain[0], "arm-io", &chain[1], &bad);
    if (!root)
        f_skip(f, "the ADT root is malformed; nothing read");
    else if (bad)
        f_skip(f, "/: malformed ADT entry among its children; later ones not read");
    if (arm_io) {
        /* One pass over the children of /arm-io: the first node of each name */
        struct akids it;
        struct anode c;
        char name[ADT_NAME_LEN];

        a_kids(&chain[1], &it);
        while (a_next(&it, &c)) {
            if (!a_name(&c, name))
                continue;
            for (size_t i = 0; i < NFIXED; i++)
                if (!have[i] && !strcmp(name, fixed_name(i))) {
                    found[i] = c;
                    have[i] = true;
                }
        }
        if (it.bad)
            f_skip(f, "/arm-io: malformed ADT entry among its children; later ones not read");
    }
    for (size_t i = 0; i < NFIXED; i++) {
        snprintf(path, sizeof(path), "/arm-io/%s", fixed_name(i));
        if (!have[i]) {
            f_skip(f, "%s absent", path);
            continue;
        }
        chain[2] = found[i];
        export_node(f, fixed_name(i), path, chain, 3, i < ARRAY_SIZE(phys) ? V_DP : V_NOLIST);
    }

    if (!arm_io) {
        f_skip(f, "/arm-io absent");
        return;
    }
    strcpy(path, "/arm-io");
    walk(f, chain, 2, path, strlen(path));
    for (int k = 0; k < K_NR; k++)
        if (!f->found[k])
            f_skip(f, "no /arm-io node named *%s*", kinds[k].what);
}

void dt_display_facts_t6030(void *dt)
{
    struct dfacts f = {0};
    int chosen = fdt_path_offset(dt, "/chosen");
    size_t size = fdt_totalsize(dt);
    void *copy;

    if (chosen < 0 || fdt_subnode_offset(dt, chosen, FACTS_NODE) >= 0)
        return;
    adt_bytes = adt_get_size();
    if (!adt || adt_bytes < ADT_NODE_HDR || adt_bytes > ADT_MAX_SIZE) {
        printf(LOG "/chosen/" FACTS_NODE " not published: ADT size %u not usable\n", adt_bytes);
        return;
    }
    copy = malloc(size);
    if (!copy) {
        printf(LOG "/chosen/" FACTS_NODE " not published: no memory\n");
        return;
    }
    memcpy(copy, dt, size);

    f.dt = copy;
    f.chosen = chosen; /* the same offset in the copy */
    int node = fdt_add_subnode(copy, chosen, FACTS_NODE);
    if (node < 0)
        f.err = node;
    else
        build(&f);

    if (!f.err) {
        memcpy(dt, copy, size);
        printf(LOG "/chosen/" FACTS_NODE " published (%u bytes of device tree, budget %u)\n",
               f.bytes, BUDGET);
    } else {
        printf(LOG "/chosen/" FACTS_NODE " not published: %s\n", fdt_strerror(f.err));
    }
    free(copy);
}
