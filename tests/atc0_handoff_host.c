/* SPDX-License-Identifier: MIT */
/*
 * Host run of aura's kboot_setup_atc() (src/kboot_atc.c, copied next to this file) on a real
 * ADT capture and a kernel DTB, as aura runs it at kboot.
 *
 *   t6050-atc-host <adt> <in.dtb> <out.dtb> [chip] [slack]
 *
 * chip: SoC id in hex (default 6050). slack: free bytes in the DT buffer (default 65536).
 * The ADT reader follows aura's (rust/src/adt.rs): 36-byte property headers, 4-byte padding,
 * reg translation through each parent's ranges. Any MMIO access aborts.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "kboot_atc.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#undef malloc
bool host_fail_malloc;
void *host_malloc(size_t size)
{
    return host_fail_malloc ? NULL : malloc(size);
}

u32 board_id, chip_id;
void *adt;
static u32 adt_size;

#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c);                   \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

void host_mmio(u64 addr)
{
    fprintf(stderr, "unexpected MMIO access at 0x%llx\n", (unsigned long long)addr);
    abort();
}

#undef printf
int debug_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("log: ");
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

static u32 rd32(u32 off)
{
    u32 v;
    CHECK(off + 4 <= adt_size);
    memcpy(&v, (u8 *)adt + off, 4);
    return v;
}

static u32 prop_next(u32 p)
{
    u32 sz = rd32(p + 32);
    CHECK(!(sz & 0x80000000));
    return p + 36 + ((sz + 3) & ~3u);
}

static u32 props_end(u32 node)
{
    u32 p = node + 8;
    for (u32 i = 0; i < rd32(node); i++)
        p = prop_next(p);
    return p;
}

static u32 subtree_end(u32 node)
{
    u32 p = props_end(node);
    for (u32 i = 0; i < rd32(node + 4); i++)
        p = subtree_end(p);
    return p;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *lenp)
{
    (void)a;
    CHECK(node >= 0);
    u32 p = node + 8;
    for (u32 i = 0; i < rd32(node); i++) {
        const char *pn = (const char *)adt + p;
        if (strnlen(pn, 32) == strlen(name) && !memcmp(pn, name, strlen(name))) {
            if (lenp)
                *lenp = rd32(p + 32);
            return (u8 *)adt + p + 36;
        }
        p = prop_next(p);
    }
    return NULL;
}

bool adt_is_compatible_at(const void *a, int node, const char *compat, size_t index)
{
    u32 len = 0;
    const char *c = adt_getprop(a, node, "compatible", &len);
    size_t i = 0;

    while (c && len) {
        size_t n = strnlen(c, len);
        if (i++ == index)
            return n == strlen(compat) && !memcmp(c, compat, n);
        if (n + 1 > len)
            break;
        c += n + 1;
        len -= n + 1;
    }
    return false;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    for (size_t i = 0; i < 8; i++)
        if (adt_is_compatible_at(a, node, compat, i))
            return true;
    return false;
}

static int child_named(int node, const char *name, size_t n)
{
    u32 c = props_end(node);
    for (u32 i = 0; i < rd32(node + 4); i++) {
        u32 len = 0;
        const char *cn = adt_getprop(adt, c, "name", &len);
        if (cn && strnlen(cn, len) == n && !memcmp(cn, name, n))
            return c;
        c = subtree_end(c);
    }
    return -1;
}

int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    int node = 0, d = 0;

    CHECK(path[0] == '/');
    for (const char *p = path + 1; *p;) {
        const char *e = strchr(p, '/');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        node = child_named(node, p, n);
        if (node < 0)
            return -1;
        CHECK(d < 8);
        if (offsets)
            offsets[d] = node;
        d++;
        p += n + (e ? 1 : 0);
    }
    if (offsets && d < 8)
        offsets[d] = 0;
    return node;
}

int adt_path_offset(const void *a, const char *path)
{
    return adt_path_offset_trace(a, path, NULL);
}

static u64 cells(const u8 *p, u32 n)
{
    u64 v = 0;
    for (u32 i = 0; i < n; i++) {
        u32 x;
        memcpy(&x, p + 4 * i, 4);
        v |= (u64)x << (32 * i);
    }
    return v;
}

static int ncells(int node, const char *name, u32 *v)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);
    if (!p || len != 4)
        return -1;
    memcpy(v, p, 4);
    return 0;
}

int adt_get_reg(const void *a, int *path, const char *pname, int idx, u64 *paddr, u64 *psize)
{
    (void)a;
    int n = 0;
    u32 ac, sc, len = 0;

    if (!path[0])
        return -1;
    while (n < 8 && path[n])
        n++;
    int cur = n - 1, node = path[cur], parent = cur ? path[cur - 1] : 0;
    if (ncells(parent, "#address-cells", &ac) || ncells(parent, "#size-cells", &sc) || ac < 1 ||
        ac > 2 || sc > 2)
        return -1;
    const u8 *reg = adt_getprop(adt, node, pname, &len);
    if (!reg || !len || len < (u32)(idx + 1) * (ac + sc) * 4)
        return -1;
    u64 addr = cells(reg + idx * (ac + sc) * 4, ac);
    u64 size = cells(reg + idx * (ac + sc) * 4 + ac * 4, sc);
    while (parent) {
        cur--;
        node = parent;
        parent = cur > 0 ? path[cur - 1] : 0;
        const u8 *ranges = adt_getprop(adt, node, "ranges", &len);
        if (!ranges)
            break;
        u32 pac;
        if (ncells(parent, "#address-cells", &pac) || pac < 1 || pac > 2)
            return -1;
        u32 entry = 4 * (pac + ac + sc);
        for (u32 o = 0; o + entry <= len; o += entry) {
            u64 ca = cells(ranges + o, ac), pa = cells(ranges + o + 4 * ac, pac);
            u64 cs = cells(ranges + o + 4 * (ac + pac), sc);
            if (addr >= ca && addr + size <= ca + cs) {
                addr = addr - ca + pa;
                break;
            }
        }
        if (ncells(parent, "#size-cells", &sc))
            return -1;
        ac = pac;
    }
    if (paddr)
        *paddr = addr;
    if (psize)
        *psize = size;
    return 0;
}

static void *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    CHECK(f);
    CHECK(!fseek(f, 0, SEEK_END));
    long n = ftell(f);
    CHECK(n > 0);
    rewind(f);
    void *buf = malloc(n);
    CHECK(buf && fread(buf, 1, n, f) == (size_t)n);
    fclose(f);
    *len = n;
    return buf;
}

int main(int argc, char **argv)
{
    size_t alen, dlen;

    if (argc < 4) {
        fprintf(stderr,
                "usage: %s <adt> <in.dtb> <out.dtb> [chip-hex] [slack] [allow|deny|alloc-fail] "
                "[expected-ret]\n",
                argv[0]);
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    adt = slurp(argv[1], &alen);
    adt_size = alen;
    chip_id = argc > 4 ? strtoul(argv[4], NULL, 16) : 0x6050;
    size_t slack = argc > 5 ? strtoul(argv[5], NULL, 0) : 65536;
    CHECK(subtree_end(0) <= adt_size);

    void *in = slurp(argv[2], &dlen);
    int cap = fdt_totalsize(in) + slack;
    void *dt = calloc(1, cap);
    CHECK(dt && !fdt_open_into(in, dt, cap));

    int ret = kboot_setup_atc(dt);
    printf("kboot_setup_atc: %d\n", ret);
    CHECK(!ret);
    void *before = malloc(cap);
    CHECK(before);
    memcpy(before, dt, cap);
    char *before_path;
    CHECK(asprintf(&before_path, "%s.before", argv[3]) >= 0);
    FILE *before_file = fopen(before_path, "wb");
    CHECK(before_file && fwrite(before, 1, cap, before_file) == (size_t)cap);
    fclose(before_file);
    free(before_path);
    bool allow = argc > 6 && strcmp(argv[6], "deny");
    host_fail_malloc = argc > 6 && !strcmp(argv[6], "alloc-fail");
    ret = kboot_handoff_atc0(dt, cap, allow);
    int expected = argc > 7 ? strtol(argv[7], NULL, 0) : 0;
    printf("kboot_handoff_atc0: %d (expected %d)\n", ret, expected);
    CHECK(ret == expected);
    int handoff = fdt_path_offset(dt, "/firmware/atc0-handoff");
    bool expected_activation =
        chip_id == 0x6050 && !fdt_node_check_compatible(dt, 0, "apple,j714s") && handoff >= 0 &&
        !fdt_node_check_compatible(dt, handoff, "apple,j714s-atc0-handoff") &&
        fdt_getprop(dt, handoff, "apple,devices", NULL);
    if (!allow || ret || !expected_activation)
        CHECK(!memcmp(before, dt, cap));
    free(before);
    int phy0 = fdt_path_offset(dt, "/soc@200000000/phy@2a3000000");
    if (allow && !ret && phy0 >= 0 && fdt_getprop(dt, phy0, "apple,guest-port-reserved", NULL)) {
        before = malloc(cap);
        CHECK(before);
        memcpy(before, dt, cap);
        CHECK(kboot_handoff_atc0(dt, cap, true) < 0);
        CHECK(!memcmp(before, dt, cap));
        free(before);
    }
    CHECK(!fdt_check_header(dt));
    CHECK(!fdt_pack(dt));

    FILE *f = fopen(argv[3], "wb");
    CHECK(f && fwrite(dt, 1, fdt_totalsize(dt), f) == fdt_totalsize(dt));
    fclose(f);
    free(dt);
    free(in);
    free(adt);
    fflush(stdout);
    return 0;
}
