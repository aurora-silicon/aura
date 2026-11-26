/* SPDX-License-Identifier: MIT */
/*
 * Host run of src/sptm_virt_mmio.c (tests/run-mmio-trace.sh): a fake ADT (a DP PHY, a crossbar,
 * an ATC PHY with sub-page and shared ranges, a display block holding a coprocessor and a DART,
 * a node whose page another tracer owns, an oversized range, a coprocessor named directly),
 * fake device registers in this process's memory, and scripted guest accesses through the
 * tracer's own hook, each checked for passthrough. The records are written as an auractl dump
 * (AURATRC1) for tools/auractl/mmio_decode.py.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>

#include "src/sptm_virt_mmio.c"

#define B  0x50000000UL /* the fake register space */
#define BZ 0x2000000UL
#define PG 0x4000UL

u64 shim_clock;
int shim_cpu;
u64 sptm_virt_flags = SPTM_VIRT_MMIO;
u64 sptm_virt_s2r_nrec = 1 << 16;
volatile u64 sptm_virt_s2r_head;
void *adt = (void *)1;

static struct {
    u64 t, meta, val, pc;
} recs[1 << 16];

volatile u64 *sptm_virt_s2r_rec_ext(u64 t, u64 meta_hi, u64 pa, int width, bool write, bool marker,
                                    u64 val, u64 pc)
{
    u64 i = sptm_virt_s2r_head++;
    recs[i].t = t ? t : ++shim_clock;
    recs[i].meta = (pa & GENMASK(47, 0)) | (u64)(width & 3) << 48 | (u64)write << 50 |
                   (u64)(shim_cpu & 15) << 51 | (u64)marker << 55 | meta_hi;
    recs[i].val = val;
    recs[i].pc = pc;
    return &recs[i].val;
}
bool sptm_virt_s2r_ring_ensure(void)
{
    return true;
}
bool sptm_virt_rg_owns(u64 page)
{
    (void)page;
    return false;
}
void sptm_virt_mmio_rw(u64 addr, u64 *val, bool write, int width)
{
    if (write)
        memcpy((void *)addr, val, 1 << width);
    else {
        *val = 0;
        memcpy(val, (void *)addr, 1 << width);
    }
}

static struct {
    u64 page;
    hv_hook_t *hook;
} hooks[512];
static int nhooks;
static const u64 prehooked = B + 0x300000; /* i2c0's page: another tracer has it */

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    if (size != PG || (from & (PG - 1))) {
        fprintf(stderr, "hook of 0x%lx size 0x%lx\n", from, size);
        exit(2);
    }
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].page == from) {
            fprintf(stderr, "page 0x%lx hooked twice\n", from);
            exit(2);
        }
    hooks[nhooks].page = from;
    hooks[nhooks++].hook = hook;
    return 0;
}
u64 hv_pt_walk(u64 addr)
{
    return addr == prehooked ? 0 : (addr | PTE_VALID);
}

/* ---- the ADT: /arm-io and its children */
static struct node {
    const char *name, *compat;
    int nreg;
    u64 reg[6][2];
} nodes[] = {
    {"arm-io", "", 0, {{0}}},
    {"dptx-phy", "dptx-phy,t8112", 2, {{B + 0x40000, 0xc000}, {B, 0x4000}}},
    {"atc0-dpxbar", "atc-dpxbar,t8112", 1, {{B + 0x100000, 0x4000}}},
    {"atc-phy0",
     "atc-phy,t8112",
     5,
     {{B + 0x200000, 0x4000},
      {B + 0x204000, 0x200},
      {B + 0x204200, 0x200},
      {B + 0x100000, 0x4000},
      {B + 0x1000000, 0x1000000}}},
    {"disp0", "disp0,t8112", 2, {{B + 0x400000, 0x8000}, {B + 0x800000, 0x200000}}},
    {"dcp", "iop,ascwrap-v4", 1, {{B + 0x880000, 0x10000}}},
    {"dart-disp0", "dart,t8110", 1, {{B + 0x8c0000, 0x4000}}},
    {"i2c0", "i2c,t8101", 1, {{B + 0x300000, 0x4000}}},
    {"dispext0", "dispext0,t8112", 2, {{B + 0xc00000, 0x200000}, {B + 0x404000, 0x4000}}},
};
#define NNODES (int)(sizeof(nodes) / sizeof(nodes[0]))

static int find(const char *path)
{
    if (!strcmp(path, "/arm-io"))
        return 0;
    if (strncmp(path, "/arm-io/", 8))
        return -1;
    for (int i = 1; i < NNODES; i++)
        if (!strcmp(nodes[i].name, path + 8))
            return i;
    return -1;
}
int adt_path_offset(const void *a, const char *path)
{
    (void)a;
    return find(path);
}
int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    int n = find(path);
    if (n >= 0)
        offsets[0] = n;
    return n;
}
int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *pa, u64 *size)
{
    (void)a, (void)prop;
    if (idx < 0 || idx >= nodes[path[0]].nreg)
        return -1;
    *pa = nodes[path[0]].reg[idx][0];
    *size = nodes[path[0]].reg[idx][1];
    return 0;
}
const void *adt_getprop(const void *a, int n, const char *name, u32 *lenp)
{
    (void)a;
    if (strcmp(name, "compatible"))
        return NULL;
    *lenp = strlen(nodes[n].compat) + 1;
    return nodes[n].compat;
}
const char *adt_get_name(const void *a, int n)
{
    (void)a;
    return nodes[n].name;
}
int adt_get_child_count(const void *a, int n)
{
    (void)a;
    return n ? 0 : NNODES - 1;
}
int adt_first_child_offset(const void *a, int n)
{
    (void)a, (void)n;
    return 1;
}
int adt_next_sibling_offset(const void *a, int n)
{
    (void)a;
    return n + 1;
}

/* ---- guest accesses through the hook, checked against the registers */
static struct exc_info ctx = {.elr = 0xfffffe0000a01000UL};

static hv_hook_t *hook_of(u64 addr)
{
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].page == (addr & ~(PG - 1)))
            return hooks[i].hook;
    fprintf(stderr, "0x%lx is not hooked\n", addr);
    exit(2);
}

static u64 rd(u64 addr, int width)
{
    u64 v[4] = {0}, want = 0;
    memcpy(&want, (void *)addr, 1 << width);
    if (!hook_of(addr)(&ctx, addr, v, false, width) || v[0] != want) {
        fprintf(stderr, "read of 0x%lx: 0x%lx, the register holds 0x%lx\n", addr, v[0], want);
        exit(2);
    }
    return v[0];
}

static void wr(u64 addr, int width, u64 val)
{
    u64 v[4] = {val}, got = 0;
    if (!hook_of(addr)(&ctx, addr, v, true, width)) {
        fprintf(stderr, "write of 0x%lx refused\n", addr);
        exit(2);
    }
    memcpy(&got, (void *)addr, 1 << width);
    if (got != (width == 3 ? val : val & ((1UL << (8 << width)) - 1))) {
        fprintf(stderr, "write of 0x%lx: register holds 0x%lx, not 0x%lx\n", addr, got, val);
        exit(2);
    }
}

static void pair(u64 addr, bool write, u64 a, u64 b)
{
    u64 v[4] = {a, b};
    if (!hook_of(addr)(&ctx, addr, v, write, 4)) {
        fprintf(stderr, "pair at 0x%lx refused\n", addr);
        exit(2);
    }
    if (((u64 *)addr)[0] != v[0] || ((u64 *)addr)[1] != v[1] ||
        (write && (v[0] != a || v[1] != b))) {
        fprintf(stderr, "pair at 0x%lx not passed through\n", addr);
        exit(2);
    }
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <dump.trc>\n", argv[0]);
        return 2;
    }
    if (mmap((void *)B, BZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == MAP_FAILED) {
        perror("mmap");
        return 2;
    }
    snprintf(sptm_virt_mmio_nodes, sizeof(sptm_virt_mmio_nodes), "%s",
             "dptx-phy,atc0-dpxbar,atc-phy0/0-3,disp0/1,nosuch,i2c0,dcp,dart-disp0,dispext0,"
             "bad/x");
    sptm_virt_mmio_max = 50;
    sptm_virt_mmio_setup();

    /* the coprocessor's pages and the DART's are not disp0's; the DART's is its own */
    for (int i = 0; i < nhooks; i++)
        if ((hooks[i].page >= B + 0x880000 && hooks[i].page < B + 0x890000)) {
            fprintf(stderr, "coprocessor page 0x%lx hooked\n", hooks[i].page);
            return 2;
        }
    if (mmt_page_node(B + 0x8c0000) != 5 || mmt_page_node(B + 0x100000) != 1 ||
        mmt_page_node(B + 0x204000) != 2) {
        fprintf(stderr, "pages attributed to the wrong nodes\n");
        return 2;
    }

    shim_cpu = 1;
    wr(B + 0x40010, 2, 0x11223344);
    wr(B + 0x40800, 2, 0x1); /* page offset 0x800: not a SEP mailbox word */
    shim_cpu = 3;
    rd(B + 0x40010, 2);
    shim_cpu = 1;
    *(u32 *)(B + 0x4) = 1;
    for (int i = 0; i < 5; i++)
        rd(B + 0x4, 2); /* a poll: one record and 4 repeats */
    *(u32 *)(B + 0x4) = 3;
    rd(B + 0x4, 2);
    wr(B + 0x8, 2, 5);
    wr(B + 0x8, 2, 5); /* the same write again: both recorded */
    *(u32 *)(B + 0xc) = 6;
    rd(B + 0xc, 2);
    wr(B + 0xc, 2, 6); /* a write of what was just read: not a repeat */
    *(u8 *)(B + 0x204010) = 0x5a;
    rd(B + 0x204010, 0);
    wr(B + 0x204012, 1, 0xbeef);
    *(u64 *)(B + 0x204208) = 0x123456789abcdef0UL;
    rd(B + 0x204208, 3);
    *(u32 *)(B + 0x205000) = 0x77;
    rd(B + 0x205000, 2); /* in atc-phy0's page, outside its ranges */
    pair(B + 0x100020, true, 0x1111111122222222UL, 0x3333333344444444UL);
    ((u64 *)(B + 0x100030))[0] = 0x5555555566666666UL;
    ((u64 *)(B + 0x100030))[1] = 0x7777777788888888UL;
    pair(B + 0x100030, false, 0, 0);
    *(u32 *)(B + 0x8c0000) = 0xd;
    rd(B + 0x8c0000, 2);
    for (int i = 0; i < 60; i++) /* a flood: 50 kept, 10 dropped */
        wr(B + 0x800000 + 4 * i, 2, i);
    wr(B + 0x40014, 2, 0xabc); /* other nodes still recorded */

    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 2;
    }
    struct {
        char magic[8] __attribute__((nonstring));
        u32 ver, rsz;
        u64 frq, first, count, head, lost;
        char build[48];
    } h = {"AURATRC1", 1, 32, 1000, 0, sptm_virt_s2r_head, sptm_virt_s2r_head, 0, "mmio-host"};
    fwrite(&h, sizeof(h), 1, f);
    fwrite(recs, 32, sptm_virt_s2r_head, f);
    fclose(f);
    printf("%lu records, dropped %lu\n", (unsigned long)sptm_virt_s2r_head,
           (unsigned long)sptm_virt_mmio_dropped);
    return sptm_virt_mmio_dropped == 10 ? 0 : 1;
}
