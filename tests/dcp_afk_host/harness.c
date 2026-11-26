/* SPDX-License-Identifier: MIT */
/*
 * Host run of src/sptm_virt_dcp_afk.c (tests/run-dcp-afk-trace.sh): a fake ADT with two DCPs and
 * their T8110 DARTs (plus nodes the setup must refuse), fake guest RAM holding the DART tables and
 * an AFK buffer, and a scripted AFK session driven through the tracer's own mailbox hook. The
 * records it appends are written as an auractl dump (AURATRC1) for tools/auractl/dcp_afk_decode.py.
 */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <unistd.h>

#include "src/sptm_virt_dcp_afk.c"

#define RAM_BASE 0x10000000UL
#define RAM_SIZE (64UL << 20)
#define GUEST    (48UL << 20) /* guest RAM; above it the DARTs' pt-region, as iBoot's on J473 */
#define PTR_BASE (RAM_BASE + GUEST)
#define PG       0x4000UL

struct boot_args cur_boot_args = {.phys_base = RAM_BASE};
u64 mem_size_actual = RAM_BASE + RAM_SIZE; /* aura's DRAM map from 0 (phys_base & ~4G) */

u64 shim_clock;
u64 sptm_virt_flags = SPTM_VIRT_DCP_AFK;
u64 sptm_virt_s2r_nrec = 1 << 16;
void *adt = (void *)1;

static int ram_fd;
static u64 ram_next = RAM_BASE, ptr_next = PTR_BASE;
static struct {
    u64 t, meta, val, pc;
} recs[1 << 16];
static u32 nrec;

/* ---- the platform */
static u64 page_alloc(void)
{
    u64 p = ram_next;
    ram_next += PG;
    memset((void *)p, 0, PG);
    return p;
}
static u64 ptr_alloc(void)
{
    u64 p = ptr_next;
    ptr_next += PG;
    memset((void *)p, 0, PG);
    return p;
}

void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool write)
{
    recs[nrec].t = ++shim_clock;
    recs[nrec].meta = (pa & GENMASK(47, 0)) | 3UL << 48 | (u64)write << 50;
    recs[nrec].val = val;
    recs[nrec].pc = tag;
    nrec++;
}
bool sptm_virt_s2r_ring_ensure(void)
{
    return true;
}
void sptm_virt_ctl_guest_ram(u64 *base, u64 *size)
{
    *base = RAM_BASE;
    *size = GUEST;
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
} hooks[8];
static int nhooks;
static u64 prehooked; /* a page another tracer has, its stage-2 leaf invalid */

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    (void)size;
    hooks[nhooks].page = from;
    hooks[nhooks++].hook = hook;
    return 0;
}
u64 hv_pt_walk(u64 addr)
{
    return addr == prehooked ? 0 : (addr | PTE_VALID);
}

static u64 win_mapped;
int mmu_add_mapping(u64 from, u64 to, size_t size, u8 attr, u64 perms)
{
    (void)attr, (void)perms;
    if (to < RAM_BASE || to + size > RAM_BASE + RAM_SIZE) {
        fprintf(stderr, "window onto 0x%lx outside RAM\n", to);
        exit(2);
    }
    if (win_mapped) {
        fprintf(stderr, "window mapped twice without an unmap\n");
        exit(2);
    }
    if (mmap((void *)from, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, ram_fd,
             to - RAM_BASE) == MAP_FAILED) {
        perror("mmap window");
        exit(2);
    }
    win_mapped = to;
    return 0;
}
int mmu_rm_mapping(u64 from, size_t size)
{
    munmap((void *)from, size);
    win_mapped = 0;
    return 0;
}

/* ---- the ADT: /arm-io and its children */
static struct node {
    const char *name, *compat;
    u64 reg, size;
} nodes[] = {
    {"arm-io", "", 0, 0},
    {"sep", "iop-sep,ascwrap-v4", 0x100000000UL, 0x10000},
    {"dcp", "iop,ascwrap-v4", 0, 0x6c000},
    {"dart-dcp", "dart,t8110", 0, 0x4000},
    {"dcp-exclave-mailbox", "iop,exclave-mailbox", 0x200000000UL, 0x4000},
    {"dcpext", "iop,ascwrap-v4", 0, 0x6c000},
    {"dart-dcpext", "dart,t8110", 0, 0x4000},
    {"dcpext0", "iop,ascwrap-v4", 0, 0x6c000},
    {"dart-dcpext0", "dart,t8110", 0, 0x4000},
    {"dcpext1", "iop,ascwrap-v4", 0x300000000UL, 0x6c000},
    {"dart-dcpext1", "dart,t6000", 0x300100000UL, 0x4000},
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
    if (idx || !nodes[path[0]].reg)
        return -1;
    *pa = nodes[path[0]].reg;
    *size = nodes[path[0]].size;
    return 0;
}
bool adt_is_compatible(const void *a, int n, const char *c)
{
    (void)a;
    return !strcmp(nodes[n].compat, c);
}
const void *adt_getprop(const void *a, int n, const char *name, u32 *lenp)
{
    (void)a;
    /* dart-dcp: its tables' pt-region-5 above guest RAM; dart-dcpext: one outside DRAM */
    static u64 pr5[2] = {PTR_BASE, PTR_BASE + (16UL << 20)};
    static u64 pr0[2] = {0x7000000000UL, 0x7000100000UL};
    if (!strcmp(name, "pt-region-5") && n == 3) {
        *lenp = 16;
        return pr5;
    }
    if (!strcmp(name, "pt-region-0") && n == 6) {
        *lenp = 16;
        return pr0;
    }
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
    (void)a;
    (void)n;
    return 1;
}
int adt_next_sibling_offset(const void *a, int n)
{
    (void)a;
    return n + 1;
}

/* ---- a T8110 DART in RAM */
struct fdart {
    u64 regs;    /* the register page */
    bool in_ptr; /* tables in the pt-region */
    u64 l1;
    u64 l2[2048];
};

static void dart_init(struct fdart *d, int sid)
{
    d->l1 = d->in_ptr ? ptr_alloc() : page_alloc();
    *(u32 *)(d->regs + 0x1000 + 4 * sid) = 1; /* translate */
    *(u32 *)(d->regs + 0x1400 + 4 * sid) = (u32)((d->l1 >> 14) << 2) | 1;
    *(u32 *)(d->regs + 0x1000 + 4 * 15) = 2; /* bypass: must not be taken for the DCP's view */
}

static void dart_map(struct fdart *d, u64 dva, u64 pa)
{
    u64 i = (dva >> 25) & 0x7ff, j = (dva >> 14) & 0x7ff;
    if (!d->l2[i]) {
        d->l2[i] = d->in_ptr ? ptr_alloc() : page_alloc();
        ((u64 *)d->l1)[i] = ((d->l2[i] >> 14) << 10) | 1;
    }
    ((u64 *)d->l2[i])[j] = ((pa >> 14) << 10) | 1;
}

/* the PA behind a DVA (test side) */
static u8 *dva_ptr(struct fdart *d, u64 dva)
{
    u64 l2 = d->l2[(dva >> 25) & 0x7ff];
    u64 pte = ((u64 *)l2)[(dva >> 14) & 0x7ff];
    return (u8 *)((((pte >> 10) & GENMASK(29, 0)) << 14) | (dva & (PG - 1)));
}

static void put(struct fdart *d, u64 dva, const void *src, size_t n)
{
    for (size_t k = 0; k < n; k++)
        *dva_ptr(d, dva + k) = ((const u8 *)src)[k];
}
static void put32(struct fdart *d, u64 dva, u32 v)
{
    put(d, dva, &v, 4);
}

/* ---- the mailbox, through the tracer's hook */
static struct exc_info ctx = {.elr = 0xfffffe0007654321UL};

static hv_hook_t *hook_of(u64 mbox)
{
    for (int i = 0; i < nhooks; i++)
        if (hooks[i].page == mbox)
            return hooks[i].hook;
    fprintf(stderr, "mailbox 0x%lx not hooked\n", mbox);
    exit(2);
}

static void a2i(u64 mbox, u8 ep, u64 m0, bool split)
{
    u64 v[2] = {m0, ep};
    if (split) {
        hook_of(mbox)(&ctx, mbox + 0x800, &v[0], true, 3);
        hook_of(mbox)(&ctx, mbox + 0x808, &v[1], true, 3);
    } else {
        hook_of(mbox)(&ctx, mbox + 0x800, v, true, 4);
    }
    if (*(u64 *)(mbox + 0x800) != m0 || *(u64 *)(mbox + 0x808) != ep) {
        fprintf(stderr, "A2I write not passed through\n");
        exit(1);
    }
}

static void i2a(u64 mbox, u8 ep, u64 m0, bool split)
{
    u64 v[2] = {0, 0};
    *(u64 *)(mbox + 0x830) = m0;
    *(u64 *)(mbox + 0x838) = ep;
    if (split) {
        hook_of(mbox)(&ctx, mbox + 0x830, &v[0], false, 3);
        hook_of(mbox)(&ctx, mbox + 0x838, &v[1], false, 3);
    } else {
        hook_of(mbox)(&ctx, mbox + 0x830, v, false, 4);
    }
    if (v[0] != m0 || v[1] != ep) {
        fprintf(stderr, "I2A read not passed through\n");
        exit(1);
    }
}

/* ---- AFK entries */
static size_t qe(u8 *out, const void *body, u32 size, u32 channel, u32 type)
{
    u32 h[4] = {0x20504f49, size, channel, type};
    memcpy(out, h, 16);
    memcpy(out + 16, body, size);
    return 16 + size;
}

static size_t v2(u8 *out, u8 seq, u16 intf, u8 cat, u8 type, u64 token, u16 flags, const void *pl,
                 u32 n)
{
    u32 len = n + 16;
    memset(out, 0, 24);
    out[0] = seq;
    memcpy(out + 2, &intf, 2);
    memcpy(out + 4, &len, 4);
    memcpy(out + 8, &token, 8);
    out[16] = type;
    out[17] = cat;
    memcpy(out + 18, &flags, 2);
    memcpy(out + 24, pl, n);
    return 24 + n;
}

#define BUF 0x800104000UL

static void session(u64 mbox, struct fdart *d, u8 ep, bool split, int dcpno)
{
    u8 body[0x4000], e[0x4000];
    size_t n;

    /* buffer 0x8000: two pages, not contiguous in RAM */
    u64 p0 = page_alloc();
    page_alloc();
    u64 p1 = page_alloc();
    dart_map(d, BUF, p0);
    dart_map(d, BUF + PG, p1);
    put32(d, BUF, 0x3f40);      /* TX ring at 0: 0xc0 of header, 0x3f40 of data */
    put32(d, BUF + PG, 0x3f40); /* RX ring at 0x4000 */
    const u64 tx = BUF + 0xc0, rx = BUF + PG + 0xc0;

    i2a(mbox, ep, 0x89UL << 48 | 0x200 << 16 | 7, split);
    a2i(mbox, ep, 0xa1UL << 48 | BUF, split);
    i2a(mbox, ep, 0x8aUL << 48 | 0UL << 32 | 0x100 << 16 | 7, split);
    i2a(mbox, ep, 0x8bUL << 48 | 0x100UL << 32 | 0x100 << 16 | 7, split);

    /* DCP -> AP: the announce of interface 3 */
    /* (the name in its first 32 bytes; 0x80 of payload: the entry takes 0xc0 of the ring) */
    char name[0x80] = {0};
    strncpy(name, dcpno ? "dispext0:dcpdptx-hdcp-interface:1" : "disp0:dcpdptx-hdcp-interface:0",
            31);
    n = v2(body, 1, 3, 0, 0x11, 0, 1, name, sizeof(name));
    n = qe(e, body, n, 0, 0);
    put(d, rx, e, n);
    put32(d, BUF + PG + 0x80, 0xc0); /* RX wptr */
    i2a(mbox, ep, 0x85UL << 48 | 0xc0, split);

    /* AP -> DCP: an OOB command; its buffers in their own pages, tx at an unaligned DVA */
    u64 rxb = page_alloc(), txb = page_alloc();
    dart_map(d, 0x800200000UL, rxb);
    dart_map(d, 0x800300000UL, txb);
    for (int k = 0; k < 0x40; k++)
        ((u8 *)rxb)[k] = 0xc0 + k;
    memcpy((u8 *)txb + 4, "\x01\x02\x03\x04\x05", 5);
    u8 cmd[34] = {1, 0x12, 0, 0, 0x40, 0, 0, 0};
    u64 a = 0x800200000UL, b = 0x800300004UL;
    u32 sz[2] = {0x40, 5};
    memcpy(cmd + 8, &a, 8);
    memcpy(cmd + 16, &b, 8);
    memcpy(cmd + 24, sz, 8);
    cmd[32] = 0xaa, cmd[33] = 0xbb;
    n = v2(body, 9, 3, 1, 0xc0, 0x77, 0, cmd, sizeof(cmd));
    n = qe(e, body, n, 0, 0);
    put(d, tx, e, n);
    put32(d, BUF + 0x80, 0x80); /* TX wptr */
    a2i(mbox, ep, 0xa2UL << 48 | 0x80, split);

    if (dcpno == 0)
        return;

    /* an entry that fills the ring to 0x3f00 (copied up to the byte budget), then one of 0x58
     * bytes that does not fit in the 0x40 left: a dummy header at 0x3f00 and the entry at 0,
     * ending at 0x80, before the consumer's 0xc0; one notification for both */
    memset(body, 0x5a, sizeof(body));
    n = qe(e, body, 0x3f00 - 0xc0 - 16, 0, 0);
    put(d, rx + 0xc0, e, n);
    u8 small[0x20];
    memset(small, 0x33, sizeof(small));
    n = v2(body, 2, 3, 2, 0xc0, 0x77, 0, small, sizeof(small));
    n = qe(e, body, n, 0, 0);
    put(d, rx + 0x3f00, e, 16);
    put(d, rx, e, n);
    put32(d, BUF + PG + 0x80, 0x80);
    i2a(mbox, ep, 0x85UL << 48 | 0x80, split);

    /* an endpoint not followed: nothing recorded */
    i2a(mbox, 0x37, 0x1234, split);
    /* a bad entry magic, then a write pointer off the block size */
    put32(d, tx + 0x80, 0);
    put32(d, BUF + 0x80, 0xc0);
    a2i(mbox, ep, 0xa2UL << 48 | 0xc0, split);
    put32(d, BUF + PG + 0x80, 0x41);
    i2a(mbox, ep, 0x85UL << 48 | 0x41, split);
    /* another access on the page: passed through, no record */
    u64 v = 0xabcd;
    hook_of(mbox)(&ctx, mbox + 0x110, &v, true, 2);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.trc>\n", argv[0]);
        return 2;
    }
    ram_fd = memfd_create("ram", 0);
    if (ram_fd < 0 || ftruncate(ram_fd, RAM_SIZE) ||
        mmap((void *)RAM_BASE, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, ram_fd,
             0) == MAP_FAILED) {
        perror("ram");
        return 2;
    }
    /* register pages and mailbox pages: MMIO, outside RAM */
    static struct fdart dart[3];
    u64 mmio = (u64)mmap(NULL, 8 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mmio = ALIGN_UP(mmio, PG);
    for (int i = 0; i < 3; i++)
        dart[i].regs = mmio + i * PG;
    nodes[3].reg = dart[0].regs;
    nodes[6].reg = dart[1].regs;
    nodes[8].reg = dart[2].regs;
    nodes[2].reg = mmio + 3 * PG - 0x8000; /* dcp's mailbox page at +0x8000 */
    nodes[5].reg = mmio + 4 * PG - 0x8000; /* dcpext */
    nodes[7].reg = mmio + 5 * PG - 0x8000; /* dcpext0: its page already hooked */
    prehooked = mmio + 5 * PG;
    dart[0].in_ptr = true;
    dart_init(&dart[0], 5);
    dart_init(&dart[1], 0);

    sptm_virt_dcp_afk_setup();
    if (dafk_n != 2 || strcmp(dafk[0].name, "dcp") || strcmp(dafk[1].name, "dcpext") ||
        nhooks != 2) {
        fprintf(stderr, "setup: %d DCPs, %d hooks\n", dafk_n, nhooks);
        return 1;
    }
    session(dafk[1].mbox, &dart[1], 0x2b, false, 1);
    session(dafk[0].mbox, &dart[0], 0x2b, true, 0);
    if (dafk[0].nptr != 1 || dafk[1].nptr != 0) {
        fprintf(stderr, "pt-regions: dcp %d, dcpext %d\n", dafk[0].nptr, dafk[1].nptr);
        return 1;
    }
    /* without its pt-region the dcp's tables are refused, said once as problem 6 */
    dafk[0].nptr = 0;
    u32 before = nrec;
    if (dafk_translate(&dafk[0], BUF) || dafk_translate(&dafk[0], BUF + PG) || nrec != before + 1 ||
        recs[before].pc != (TAG_X | 6UL << 36) || recs[before].val != dart[0].l1) {
        fprintf(stderr, "a table outside guest RAM and the pt-regions was walked\n");
        return 1;
    }
    /* a DVA whose page is outside guest RAM: not read, said as problem 7 */
    dafk[0].nptr = 1;
    dart_map(&dart[0], 0x800400000UL, PTR_BASE + (8UL << 20));
    xl_dcp = NULL;
    u64 w;
    before = nrec;
    if (dafk_word(&dafk[0], 0x800400000UL, &w) || nrec != before + 1 ||
        recs[before].pc != (TAG_X | 7UL << 36)) {
        fprintf(stderr, "a page outside guest RAM was read\n");
        return 1;
    }

    FILE *f = fopen(argv[1], "wb");
    struct {
        char magic[8] __attribute__((nonstring));
        u32 ver, rsz;
        u64 frq, first, count, head, lost;
        char build[48];
    } h = {"AURATRC1", 1, 32, 1000, 0, nrec, nrec, 0, "dcp-afk-host"};
    fwrite(&h, sizeof(h), 1, f);
    fwrite(recs, 32, nrec, f);
    fclose(f);
    printf("%u records\n", nrec);
    return 0;
}
