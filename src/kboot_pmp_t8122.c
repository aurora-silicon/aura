/* SPDX-License-Identifier: MIT */

#include <stdarg.h>

#include "kboot_pmp_t8122.h"
#include "adt.h"
#include "exception.h"
#include "malloc.h"
#include "pmgr.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "vsprintf.h"

#include "libfdt/libfdt.h"

/*
 * T8122 (M3 MacBook Air): what a PMP start and the display clock need, as this Mac has it, for
 * Linux-side inspection. Two /chosen nodes. adt,* properties are raw ADT bytes, sram-uuid is
 * SRAM bytes in memory order, and every other value is big-endian cells. They are not device
 * bindings.
 *
 * aurora,t8122-pmp-facts
 *   pmp/, iop-pmp-nub/  the ADT properties of /arm-io/pmp, every iop-pmp-nub property name,
 *                       and the iop-pmp-nub properties /chosen/aurora,t8122-pmp does not carry
 *                       as apple,tunable-*
 *   power-*             the PMGR power states the /arm-io/pmp clock-gates resolve to
 *   sram-*              the image UUID at SRAM +0x214, and whether it is the iop-pmp-nub uuid
 *   bootargs-*          the boot-argument block at the offset and size given at SRAM
 *                       +0x22c/+0x230: every record's key and size, and the values of the
 *                       BDID, DVID and DCAP records only; read only if the UUID matched
 *   ptd-*               the PTD status, request and acknowledge words, at the offsets the
 *                       iop-pmp-nub ptd-range table gives (each id once, with a nonzero
 *                       count), read only if an ADT reg of an /arm-io node covers each and
 *                       that node's own PMGR power states are actually on
 *   skipped             why a reading was not taken
 *
 * aurora,t8122-clock-facts
 *   pmgr/               every PMGR property name and size, and its clocks, perf-regs,
 *                       power-domains and voltage-states* tables
 *   dcp/, disp0/, dcpext/, dispext0/
 *                       their property names, clock-ids, clock-gates and power-gates
 *   No clock register is read and no rate is derived.
 *
 * Nothing is written to the hardware. SRAM is read with aligned 32-bit loads and the PTD with
 * aligned 64-bit loads, under an exception guard, and only after every PMGR power state of the
 * /arm-io/pmp clock-gates read as actually on. The PTD update page is never accessed. After a
 * guarded fault, or once the node cannot be published, nothing more is read. Only on a J613
 * with chosen.aurora,t8122-dcp=1; the caller selects T8122. Each node is built on a copy of the
 * FDT and published only if complete. Failure is non-fatal to kboot, and an existing node is
 * never replaced.
 */

#define FACTS_PMP    "aurora,t8122-pmp-facts"
#define FACTS_CLOCK  "aurora,t8122-clock-facts"
#define VALUES_NODE  "aurora,t8122-pmp"
#define FACTS_SCHEMA 1
#define LOG          "PMP: T8122: "

#define MAX_PROPERTY_BYTES 32768u
#define MAX_CLOCK_BYTES    65536u
#define MAX_GATES          8
#define MAX_RECORDS        64
#define MAX_REGS           32
#define MAX_PS             16
#define MAX_GATE_DEPTH     3

#define PMGR_AUTO_ENABLE BIT(28)
#define PMGR_PS_ACTUAL   GENMASK(7, 4)
#define PMGR_PS_TARGET   GENMASK(3, 0)

#define SRAM_UUID          0x214
#define SRAM_BOOTARGS_OFF  0x22c
#define SRAM_BOOTARGS_SIZE 0x230
#define BOOTARGS_MIN       8
#define BOOTARGS_MAX       4096

#define PTD_UPDATE_OFFSET 0x10000
#define PTD_RANGE_SIZE    32
#define PTD_ID_STATUS     2
#define PTD_ID_REQUEST    10
#define PTD_ID_ACK        11

struct facts {
    void *dt;
    int chosen;       /* /chosen; its offset does not move as its own subtree grows */
    const char *name; /* the /chosen subnode being built */
    int err;          /* the first libfdt error */
    bool hw_stop;     /* a guarded read faulted: no more hardware reads */
    u32 bytes;        /* ADT bytes copied, against the node's limit */
};

/* ---- FDT writes, into the copy; the first error sticks ---- */

static int facts_node(struct facts *f, const char *sub)
{
    int node;

    if (f->err)
        return -1;
    node = fdt_subnode_offset(f->dt, f->chosen, f->name);
    if (node >= 0 && sub)
        node = fdt_subnode_offset(f->dt, node, sub);
    if (node < 0)
        f->err = node;
    return node;
}

static void facts_set(struct facts *f, const char *sub, const char *name, const void *val, int len)
{
    int node = facts_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_setprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void facts_append(struct facts *f, const char *sub, const char *name, const void *val,
                         int len)
{
    int node = facts_node(f, sub);
    int ret;

    if (node < 0)
        return;
    ret = fdt_appendprop(f->dt, node, name, val, len);
    if (ret)
        f->err = ret;
}

static void facts_u32(struct facts *f, const char *sub, const char *name, u32 v)
{
    fdt32_t c = cpu_to_fdt32(v);
    facts_set(f, sub, name, &c, sizeof(c));
}

static void facts_append_u32(struct facts *f, const char *sub, const char *name, u32 v)
{
    fdt32_t c = cpu_to_fdt32(v);
    facts_append(f, sub, name, &c, sizeof(c));
}

static void facts_append_u64(struct facts *f, const char *sub, const char *name, u64 v)
{
    fdt64_t c = cpu_to_fdt64(v);
    facts_append(f, sub, name, &c, sizeof(c));
}

static void facts_str(struct facts *f, const char *sub, const char *name, const char *s)
{
    facts_set(f, sub, name, s, strlen(s) + 1);
}

static void facts_append_str(struct facts *f, const char *sub, const char *name, const char *s)
{
    facts_append(f, sub, name, s, strlen(s) + 1);
}

static void __attribute__((format(printf, 2, 3))) facts_skip(struct facts *f, const char *fmt, ...)
{
    char buf[160];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf(LOG "%s\n", buf);
    facts_append_str(f, NULL, "skipped", buf);
}

static void facts_subnode(struct facts *f, const char *sub, const char *adt_path)
{
    int node = facts_node(f, NULL);
    int ret;

    if (node < 0)
        return;
    ret = fdt_add_subnode(f->dt, node, sub);
    if (ret < 0) {
        f->err = ret;
        return;
    }
    facts_str(f, sub, "adt-path", adt_path);
}

/* The ADT property name, NUL-terminated (the ADT field is 32 bytes) */
static void prop_name(const struct adt_property *prop, char *name)
{
    size_t n = strnlen(prop->name, sizeof(prop->name));
    memcpy(name, prop->name, n);
    name[n] = '\0';
}

/* Copies one ADT property as adt,<name>, within the node's byte limit */
static void facts_copy(struct facts *f, const char *sub, const struct adt_property *prop, u32 limit)
{
    char name[sizeof(prop->name) + 1], fname[sizeof(name) + 4];

    prop_name(prop, name);
    if (prop->size > MAX_PROPERTY_BYTES || prop->size > limit - min(f->bytes, limit)) {
        facts_skip(f, "%s %s: %u bytes, over the size limit", sub, name, prop->size);
        return;
    }
    snprintf(fname, sizeof(fname), "adt,%s", name);
    facts_set(f, sub, fname, prop->value, prop->size);
    f->bytes += prop->size;
}

/* ---- guarded MMIO reads ---- */

static int guard_begin(void)
{
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    return before;
}

/*
 * Returns -1 if a synchronous abort or an SError was taken since guard_begin(), and then stops
 * every later hardware read
 */
static int guard_end(struct facts *f, int before)
{
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    if (exc_count == before)
        return 0;
    f->hw_stop = true;
    return -1;
}

/* Whether hardware may still be read: no fault so far and the node can still be published */
static bool hw_ok(struct facts *f)
{
    return !f->hw_stop && !f->err;
}

/* ADT values read only at their exact size, never copied into a smaller buffer */
static int adt_u32_exact(int node, const char *name, u32 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);

    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

static int adt_u64_exact(int node, const char *name, u64 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);

    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

/* Whether every property name of an ADT node is printable and NUL-terminated within its field */
static bool adt_names_ok(int node)
{
    ADT_FOREACH_PROPERTY(adt, node, prop)
    {
        size_t n = strnlen(prop->name, sizeof(prop->name));
        if (n == sizeof(prop->name))
            return false;
        for (size_t i = 0; i < n; i++)
            if (prop->name[i] < 0x20 || prop->name[i] > 0x7e)
                return false;
    }
    return true;
}

/* A power state is on only if it is actually on; a target with automatic power management is not */
static bool ps_on(u32 ps)
{
    return FIELD_GET(PMGR_PS_ACTUAL, ps) == PMGR_PS_ACTIVE;
}

static u32 le32(const u8 *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

static u64 le64(const u8 *p)
{
    return le32(p) | ((u64)le32(p + 4) << 32);
}

/* ---- the switch and the board ---- */

static bool facts_wanted(void *dt)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "aurora,t8122-dcp", &len);

    return opt && len == 2 && opt[0] == '1' && opt[1] == '\0' &&
           !fdt_node_check_compatible(dt, 0, "apple,t8122") &&
           (!fdt_node_check_compatible(dt, 0, "apple,j613") ||
            !fdt_node_check_compatible(dt, 0, "apple,j615"));
}

/* ---- aurora,t8122-pmp-facts ---- */

/* Every /arm-io/pmp property, and the iop-pmp-nub properties the values node does not carry */
static void pmp_adt(struct facts *f, int pmp, int nub)
{
    facts_subnode(f, "pmp", "/arm-io/pmp");
    ADT_FOREACH_PROPERTY(adt, pmp, prop)
    {
        facts_copy(f, "pmp", prop, ~0u);
    }

    facts_subnode(f, "iop-pmp-nub", "/arm-io/pmp/iop-pmp-nub");
    ADT_FOREACH_PROPERTY(adt, nub, prop)
    {
        char name[sizeof(prop->name) + 1], tunable[sizeof(name) + 16];
        int len = -1;
        int values; /* looked up each time: nodes move as properties are added */

        prop_name(prop, name);
        facts_append_str(f, "iop-pmp-nub", "adt-property-names", name);
        snprintf(tunable, sizeof(tunable), "apple,tunable-%s", name);
        values = fdt_subnode_offset(f->dt, f->chosen, VALUES_NODE);
        if (values >= 0 && fdt_getprop(f->dt, values, tunable, &len) && (u32)len == prop->size &&
            !memcmp(fdt_getprop(f->dt, values, tunable, NULL), prop->value, len))
            continue;
        facts_copy(f, "iop-pmp-nub", prop, ~0u);
    }
}

/*
 * Reads the PMGR power states of the /arm-io/pmp clock-gates. Returns whether all are actually
 * on. Nothing is read unless the whole clock-gates property is well-formed, at most MAX_GATES
 * entries, and every entry resolves to a device with a power state register of its own.
 */
static bool pmp_power(struct facts *f, int pmp)
{
    u64 addr[MAX_GATES];
    u32 val[MAX_GATES], len = 0, n;
    char names[MAX_GATES][17];
    bool all_on = true;

    if (!adt_getprop(adt, pmp, "clock-gates", &len) || !len || len % 4 || len / 4 > MAX_GATES) {
        facts_skip(f,
                   "power: /arm-io/pmp clock-gates absent, malformed or over %d entries (%u "
                   "bytes); SRAM and PTD not read",
                   MAX_GATES, len);
        return false;
    }
    n = len / 4;
    for (u32 i = 0; i < n; i++) {
        struct pmgr_gate g;
        if (pmgr_adt_gate("/arm-io/pmp", "clock-gates", i, &g) < 0 || !g.found || g.virt ||
            !g.psreg) {
            facts_skip(f,
                       "power: /arm-io/pmp clock-gates[%u] has no PMGR power state register; "
                       "SRAM and PTD not read",
                       i);
            return false;
        }
        addr[i] = g.psreg;
        size_t nl = strnlen(g.name, 16); /* up to 16 chars, not always NUL-terminated */
        memcpy(names[i], g.name, nl);
        names[i][nl] = '\0';
    }

    if (!hw_ok(f))
        return false;
    int g = guard_begin();
    for (u32 i = 0; i < n; i++)
        val[i] = read32(addr[i]);
    if (guard_end(f, g)) {
        facts_skip(f, "power: reading the PMGR power states faulted; SRAM and PTD not read");
        return false;
    }

    for (u32 i = 0; i < n; i++) {
        bool on = ps_on(val[i]);
        printf(LOG "power state %s 0x%lx = 0x%x: actual 0x%lx, target 0x%lx, auto %s; %s\n",
               names[i], addr[i], val[i], FIELD_GET(PMGR_PS_ACTUAL, val[i]),
               FIELD_GET(PMGR_PS_TARGET, val[i]), (val[i] & PMGR_AUTO_ENABLE) ? "on" : "off",
               on ? "on" : "NOT on");
        facts_append_str(f, NULL, "power-names", names[i]);
        facts_append_u64(f, NULL, "power-state-regs", addr[i]);
        facts_append_u32(f, NULL, "power-states", val[i]);
        all_on &= on;
    }
    facts_u32(f, NULL, "power-on", all_on);
    if (!all_on)
        facts_skip(f, "power: a PMP power state is not actually on; SRAM and PTD not read");
    return all_on;
}

/* The uuid string of iop-pmp-nub as 16 bytes in string order. Returns -1 if malformed. */
static int adt_uuid(int nub, u8 *out)
{
    static const u8 at[16] = {0, 2, 4, 6, 9, 11, 14, 16, 19, 21, 24, 26, 28, 30, 32, 34};
    u32 len = 0;
    const char *s = adt_getprop(adt, nub, "uuid", &len);

    if (!s || len < 36 || len > 37 || (len == 37 && s[36]) || s[8] != '-' || s[13] != '-' ||
        s[18] != '-' || s[23] != '-')
        return -1;
    for (int i = 0; i < 16; i++) {
        u8 b = 0;
        for (int k = 0; k < 2; k++) {
            char c = s[at[i] + k];
            u8 d;
            if (c >= '0' && c <= '9')
                d = c - '0';
            else if (c >= 'a' && c <= 'f')
                d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F')
                d = c - 'A' + 10;
            else
                return -1;
            b = (b << 4) | d;
        }
        out[i] = b;
    }
    return 0;
}

/* 16 bytes in string order as the canonical upper-case UUID text */
static void uuid_text(const u8 *b, char *out)
{
    static const char hex[] = "0123456789ABCDEF";
    char *p = out;

    for (int i = 0; i < 16; i++) {
        if (i == 4 || i == 6 || i == 8 || i == 10)
            *p++ = '-';
        *p++ = hex[b[i] >> 4];
        *p++ = hex[b[i] & 0xf];
    }
    *p = '\0';
}

static void print_fourcc(u32 key, char *out)
{
    for (int i = 0; i < 4; i++) {
        char c = (key >> (24 - 8 * i)) & 0xff;
        out[i] = (c >= 0x20 && c < 0x7f) ? c : '.';
    }
    out[4] = '\0';
}

/* The records whose values are passed on; of any other record only its key and size */
static const u32 bootargs_values[] = {0x42444944 /* BDID */, 0x44564944 /* DVID */,
                                      0x44434150 /* DCAP */};

/*
 * The records of the packed boot-argument block: every key and size, and the value of a BDID,
 * DVID or DCAP record of 1..8 bytes. Returns whether the records end exactly at its end.
 */
static bool bootargs_records(struct facts *f, const u8 *b, u32 size)
{
    u32 pos = 0, n = 0;

    while (pos < size) {
        char cc[5];
        u32 key, len;
        bool pass = false;

        if (size - pos < 8 || n >= MAX_RECORDS)
            return false;
        key = le32(b + pos);
        len = le32(b + pos + 4);
        if (len > size - pos - 8)
            return false;
        facts_append_u32(f, NULL, "bootargs-records", key);
        facts_append_u32(f, NULL, "bootargs-records", len);
        print_fourcc(key, cc);
        for (size_t i = 0; i < ARRAY_SIZE(bootargs_values); i++)
            pass |= key == bootargs_values[i];
        if (pass && len >= 1 && len <= 8) {
            u8 v[8] = {0};
            memcpy(v, b + pos + 8, len);
            facts_append_u32(f, NULL, "bootargs-values", key);
            facts_append_u64(f, NULL, "bootargs-values", le64(v));
            printf(LOG "boot argument %s: %u bytes, 0x%lx\n", cc, len, le64(v));
        } else {
            printf(LOG "boot argument %s: %u bytes\n", cc, len);
        }
        pos += 8 + len;
        n++;
    }
    return true;
}

static void pmp_sram(struct facts *f, int *pmp_path, int nub)
{
    u64 base = 0, size = 0, region_base = 0, region_size = 0;
    u8 uuid[16], want[16];
    u32 w[4], off, sz;

    if (adt_get_reg(adt, pmp_path, "reg", 2, &base, &size)) {
        facts_skip(f, "sram: /arm-io/pmp has no reg[2]");
        return;
    }
    facts_append_u64(f, NULL, "sram-reg", base);
    facts_append_u64(f, NULL, "sram-reg", size);
    if (adt_u64_exact(nub, "region-base", &region_base) ||
        adt_u64_exact(nub, "region-size", &region_size) || region_base != base ||
        region_size != size) {
        facts_skip(f, "sram: /arm-io/pmp reg[2] is not the iop-pmp-nub region; not read");
        return;
    }
    if ((base & 3) || size < SRAM_BOOTARGS_SIZE + 4) {
        facts_skip(f, "sram: region 0x%lx+0x%lx too small or unaligned; not read", base, size);
        return;
    }

    if (!hw_ok(f))
        return;
    int g = guard_begin();
    for (int i = 0; i < 4; i++)
        w[i] = read32(base + SRAM_UUID + 4 * i);
    off = read32(base + SRAM_BOOTARGS_OFF);
    sz = read32(base + SRAM_BOOTARGS_SIZE);
    if (guard_end(f, g)) {
        facts_skip(f, "sram: reading the image UUID faulted");
        return;
    }
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 4; k++)
            uuid[4 * i + k] = w[i] >> (8 * k);
    facts_set(f, NULL, "sram-uuid", uuid, sizeof(uuid));

    bool match = !adt_uuid(nub, want) && !memcmp(uuid, want, sizeof(uuid));
    facts_u32(f, NULL, "sram-uuid-matches-adt", match);
    char text[37];
    uuid_text(uuid, text);
    printf(LOG "SRAM image UUID %s, %s the iop-pmp-nub uuid\n", text, match ? "equal to" : "NOT");

    facts_u32(f, NULL, "bootargs-offset", off);
    facts_u32(f, NULL, "bootargs-size", sz);
    bool in_bounds = sz >= BOOTARGS_MIN && sz <= BOOTARGS_MAX && !(off & 3) && off <= size &&
                     ALIGN_UP(sz, 4) <= size - off;
    facts_u32(f, NULL, "bootargs-in-bounds", in_bounds);
    printf(LOG "boot arguments at SRAM +0x%x, 0x%x bytes: %s\n", off, sz,
           in_bounds ? "in bounds" : "OUT OF BOUNDS");
    if (!match) {
        facts_skip(f, "bootargs: the SRAM image is not the iop-pmp-nub uuid; not read");
        return;
    }
    if (!in_bounds) {
        facts_skip(f, "bootargs: offset 0x%x size 0x%x out of bounds; not read", off, sz);
        return;
    }

    u32 words = ALIGN_UP(sz, 4) / 4;
    u32 buf[BOOTARGS_MAX / 4];
    if (!hw_ok(f))
        return;
    g = guard_begin();
    for (u32 i = 0; i < words; i++)
        buf[i] = read32(base + off + 4 * i);
    if (guard_end(f, g)) {
        facts_skip(f, "bootargs: reading the block faulted");
        return;
    }
    bool parsed = bootargs_records(f, (const u8 *)buf, sz);
    facts_u32(f, NULL, "bootargs-parsed", parsed);
    if (!parsed)
        printf(LOG "boot arguments do not parse as packed records to the end of the block\n");
}

/*
 * For each of n words at addr[i] (8 bytes each), the /arm-io node whose ADT reg range covers it,
 * in one pass over /arm-io. A node with a malformed property name is not used. Returns how many
 * words are covered.
 */
static int adt_reg_cover(const u64 *addr, int n, int *node, int *reg)
{
    int path[8] = {0};
    int arm_io = adt_path_offset_trace(adt, "/arm-io", path);
    int child, covered = 0;

    for (int i = 0; i < n; i++)
        node[i] = -1;
    if (arm_io < 0 || path[0] != arm_io || path[1])
        return 0;
    child = arm_io;
    ADT_FOREACH_CHILD(adt, child)
    {
        if (!adt_names_ok(child))
            continue;
        path[1] = child;
        path[2] = 0;
        for (int r = 0; r < MAX_REGS && covered < n; r++) {
            u64 base, size;
            if (adt_get_reg(adt, path, "reg", r, &base, &size))
                break;
            if (base + size < base || size < 8)
                continue;
            for (int i = 0; i < n; i++) {
                if (node[i] < 0 && addr[i] >= base && addr[i] - base <= size - 8) {
                    node[i] = child;
                    reg[i] = r;
                    covered++;
                }
            }
        }
        if (covered == n)
            break;
    }
    return covered;
}

/* Adds g's power state register, or, for a virtual device, those of its nearest real parents */
static int ps_add(u64 *ps, int *n, const struct pmgr_gate *g, int depth)
{
    if (!g->found)
        return -1;
    if (!g->virt) {
        if (!g->psreg)
            return -1;
        for (int i = 0; i < *n; i++)
            if (ps[i] == g->psreg)
                return 0;
        if (*n >= MAX_PS)
            return -1;
        ps[(*n)++] = g->psreg;
        return 0;
    }
    if (depth >= MAX_GATE_DEPTH || (!g->parent[0] && !g->parent[1]))
        return -1;
    for (int k = 0; k < 2; k++) {
        struct pmgr_gate p;
        if (!g->parent[k])
            continue;
        if (pmgr_device_gate(g->die, g->parent[k], &p) < 0 || ps_add(ps, n, &p, depth + 1))
            return -1;
    }
    return 0;
}

/*
 * Whether the power states an ADT node's clock-gates resolve to (a virtual device by its
 * nearest real parents) all read as actually on. A node without clock-gates has none to check.
 */
static bool node_power_on(struct facts *f, const char *path, int node)
{
    u64 ps[MAX_PS];
    u32 val[MAX_PS], len = 0;
    int n = 0;

    if (!adt_getprop(adt, node, "clock-gates", &len))
        return true;
    if (!len || len % 4 || len / 4 > MAX_GATES) {
        facts_skip(f, "ptd: %s clock-gates malformed or over %d entries; not read", path,
                   MAX_GATES);
        return false;
    }
    for (u32 i = 0; i < len / 4; i++) {
        struct pmgr_gate g;
        if (pmgr_adt_gate(path, "clock-gates", i, &g) < 0 || ps_add(ps, &n, &g, 0)) {
            facts_skip(f, "ptd: %s clock-gates[%u] does not resolve to PMGR power states; not read",
                       path, i);
            return false;
        }
    }
    if (!hw_ok(f))
        return false;
    int g = guard_begin();
    for (int i = 0; i < n; i++)
        val[i] = read32(ps[i]);
    if (guard_end(f, g)) {
        facts_skip(f, "ptd: reading the %s power states faulted; not read", path);
        return false;
    }
    for (int i = 0; i < n; i++) {
        facts_append_u64(f, NULL, "ptd-power-state-regs", ps[i]);
        facts_append_u32(f, NULL, "ptd-power-states", val[i]);
        if (!ps_on(val[i])) {
            facts_skip(f, "ptd: %s power state 0x%lx = 0x%x is not actually on; not read", path,
                       ps[i], val[i]);
            return false;
        }
    }
    return true;
}

static void pmp_ptd(struct facts *f, int *pmp_path, int pmp, int nub, bool power_on)
{
    static const u32 ids[3] = {PTD_ID_STATUS, PTD_ID_REQUEST, PTD_ID_ACK};
    static const char *const what[3] = {"status", "request", "ack"};
    u64 upd = 0, upd_size = 0, ap, addr[3], val[3];
    u32 idx, off[3], len = 0, found = 0;
    int cover[3], reg[3];
    const u8 *table;

    if (adt_u32_exact(pmp, "ptd-update-reg-index", &idx) &&
        adt_u32_exact(nub, "ptd-update-reg-index", &idx)) {
        facts_skip(f, "ptd: no 4-byte ptd-update-reg-index; not read");
        return;
    }
    if (idx >= MAX_REGS || adt_get_reg(adt, pmp_path, "reg", idx, &upd, &upd_size) ||
        upd < PTD_UPDATE_OFFSET) {
        facts_skip(f, "ptd: /arm-io/pmp reg[%u] unusable; not read", idx);
        return;
    }
    ap = upd - PTD_UPDATE_OFFSET;
    facts_append_u64(f, NULL, "ptd-update-reg", upd);
    facts_append_u64(f, NULL, "ptd-update-reg", upd_size);

    table = adt_getprop(adt, nub, "ptd-range", &len);
    if (!table || !len || len % PTD_RANGE_SIZE) {
        facts_skip(f, "ptd: ptd-range missing or not 32-byte entries; not read");
        return;
    }
    for (u32 pos = 0; pos < len; pos += PTD_RANGE_SIZE) {
        u32 id = le32(table + pos);
        u64 o = (u64)le32(table + pos + 4) * 16;
        u32 count = le32(table + pos + 8);
        for (int i = 0; i < 3; i++) {
            const char *bad = NULL;
            if (id != ids[i])
                continue;
            if (found & BIT(i))
                bad = "appears twice";
            else if (!count)
                bad = "has count 0";
            else if (o >= PTD_UPDATE_OFFSET || (o & 7))
                bad = "is out of range";
            if (bad) {
                facts_skip(f, "ptd: ptd-range id %u (%s) %s; not read", id, what[i], bad);
                return;
            }
            off[i] = o;
            found |= BIT(i);
        }
    }
    for (int i = 0; i < 3; i++) {
        if (!(found & BIT(i))) {
            facts_skip(f, "ptd: ptd-range has no id %u (%s); not read", ids[i], what[i]);
            return;
        }
    }
    for (int i = 0; i < 3; i++) {
        facts_append_u32(f, NULL, "ptd-offsets", off[i]);
        addr[i] = ap + off[i];
    }
    facts_append_u64(f, NULL, "ptd-aperture", ap);

    adt_reg_cover(addr, 3, cover, reg);
    for (int i = 0; i < 3; i++) {
        char who[64];
        if (cover[i] < 0) {
            facts_skip(f, "ptd: no /arm-io reg covers the %s word at 0x%lx; not read", what[i],
                       addr[i]);
            return;
        }
        snprintf(who, sizeof(who), "/arm-io/%s reg[%d]", adt_get_name(adt, cover[i]), reg[i]);
        facts_append_str(f, NULL, "ptd-covered-by", who);
    }
    if (!power_on)
        return;
    for (int i = 0; i < 3; i++) {
        char path[48];
        bool seen = cover[i] == pmp; /* its power states were checked already */
        for (int k = 0; k < i; k++)
            seen |= cover[k] == cover[i];
        if (seen)
            continue;
        snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, cover[i]));
        if (!node_power_on(f, path, cover[i]))
            return;
    }

    if (!hw_ok(f))
        return;
    int g = guard_begin();
    for (int i = 0; i < 3; i++)
        val[i] = read64(addr[i]);
    if (guard_end(f, g)) {
        facts_skip(f, "ptd: reading the PTD words faulted");
        return;
    }
    facts_append_u64(f, NULL, "ptd-status", val[0]);
    facts_append_u64(f, NULL, "ptd-request", val[1]);
    facts_append_u64(f, NULL, "ptd-ack", val[2]);
    printf(LOG "PTD status 0x%lx request 0x%lx ack 0x%lx (at 0x%lx +0x%x +0x%x +0x%x)\n", val[0],
           val[1], val[2], ap, off[0], off[1], off[2]);
}

static void build_pmp_facts(struct facts *f)
{
    int pmp_path[8] = {0};
    int pmp = adt_path_offset_trace(adt, "/arm-io/pmp", pmp_path);
    int nub = adt_path_offset(adt, "/arm-io/pmp/iop-pmp-nub");

    facts_u32(f, NULL, "schema-version", FACTS_SCHEMA);
    facts_str(f, NULL, "encoding", "adt-raw-little-endian");
    if (pmp < 0 || nub < 0) {
        facts_skip(f, "adt: /arm-io/pmp or its iop-pmp-nub absent");
        return;
    }
    pmp_adt(f, pmp, nub);
    if (f->err)
        return;
    bool on = pmp_power(f, pmp);
    if (on)
        pmp_sram(f, pmp_path, nub);
    pmp_ptd(f, pmp_path, pmp, nub, on);
}

/* ---- aurora,t8122-clock-facts ---- */

static const char *const pmgr_tables[] = {"clocks", "perf-regs", "power-domains"};

static const char *const display_nodes[][2] = {
    {"dcp", "/arm-io/dcp"},
    {"disp0", "/arm-io/disp0"},
    {"dcpext", "/arm-io/dcpext"},
    {"dispext0", "/arm-io/dispext0"},
};

static const char *const display_props[] = {"clock-ids", "clock-gates", "power-gates"};

static void build_clock_facts(struct facts *f)
{
    int pmgr = adt_path_offset(adt, "/arm-io/pmgr");

    facts_u32(f, NULL, "schema-version", FACTS_SCHEMA);
    facts_str(f, NULL, "encoding", "adt-raw-little-endian");

    if (pmgr < 0) {
        facts_skip(f, "clock: /arm-io/pmgr absent");
    } else {
        facts_subnode(f, "pmgr", "/arm-io/pmgr");
        ADT_FOREACH_PROPERTY(adt, pmgr, prop)
        {
            char name[sizeof(prop->name) + 1];
            bool table = false;

            prop_name(prop, name);
            facts_append_str(f, "pmgr", "adt-property-names", name);
            facts_append_u32(f, "pmgr", "adt-property-sizes", prop->size);
            for (size_t i = 0; i < ARRAY_SIZE(pmgr_tables); i++)
                table |= !strcmp(name, pmgr_tables[i]);
            table |= !strncmp(name, "voltage-states", strlen("voltage-states"));
            if (table)
                facts_copy(f, "pmgr", prop, MAX_CLOCK_BYTES);
        }
    }

    for (size_t n = 0; n < ARRAY_SIZE(display_nodes); n++) {
        const char *sub = display_nodes[n][0], *path = display_nodes[n][1];
        int node = adt_path_offset(adt, path);

        if (node < 0) {
            facts_skip(f, "clock: %s absent", path);
            continue;
        }
        facts_subnode(f, sub, path);
        ADT_FOREACH_PROPERTY(adt, node, prop)
        {
            char name[sizeof(prop->name) + 1];

            prop_name(prop, name);
            facts_append_str(f, sub, "adt-property-names", name);
            for (size_t i = 0; i < ARRAY_SIZE(display_props); i++)
                if (!strcmp(name, display_props[i]))
                    facts_copy(f, sub, prop, MAX_CLOCK_BYTES);
        }
    }
}

/* ---- publishing ---- */

static void publish(void *dt, const char *name, void (*build)(struct facts *))
{
    struct facts f = {0};
    int chosen = fdt_path_offset(dt, "/chosen");
    size_t size = fdt_totalsize(dt);
    void *copy;

    if (chosen < 0 || fdt_subnode_offset(dt, chosen, name) >= 0)
        return;
    copy = malloc(size);
    if (!copy) {
        printf(LOG "%s not published: no memory\n", name);
        return;
    }
    memcpy(copy, dt, size);

    f.dt = copy;
    f.chosen = chosen; /* the same offset in the copy */
    f.name = name;
    int node = fdt_add_subnode(copy, chosen, name);
    if (node < 0)
        f.err = node;
    else
        build(&f);

    if (!f.err) {
        memcpy(dt, copy, size);
        printf(LOG "/chosen/%s published (%u ADT bytes)\n", name, f.bytes);
    } else {
        printf(LOG "/chosen/%s not published: %s\n", name, fdt_strerror(f.err));
    }
    free(copy);
}

void dt_pmp_t8122_facts(void *dt)
{
    if (!facts_wanted(dt))
        return;
    publish(dt, FACTS_PMP, build_pmp_facts);
    publish(dt, FACTS_CLOCK, build_clock_facts);
}
