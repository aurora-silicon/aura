/* SPDX-License-Identifier: MIT */

/*
 * The GPU/PMP record under the virtualized SPTM (src/sptm_virt_gfxrec.h has the buffer's layout).
 *
 * The GPU trace's hooks (sptm_virt_gpu.c) and the debug link's PMGR hook (sptm_virt_s2r.c) hand
 * every access they have performed to sptm_virt_gfxrec_note. From the first InitData doorbell
 * (the GPU hook's named write) until window_ms after it, the ones below each become one record:
 *   - a mailbox message on gfx-asc, gfx1-asc or the PMP's ASC, either way: a write of A2I_SEND
 *     (+0x8800, its endpoint word at +0x8808) or a read of I2A_RECV (+0x8830, +0x8838), as one
 *     128-bit pair or as two 64-bit halves (two records, which the host pairs per core);
 *   - a write to sgx +0xd60000..+0xd60067 (the host block and its generation), +0x1000104 or
 *     +0x1000108;
 *   - a write over the power-state word of a PMGR device named GFX, AGX or SGX.
 * Nothing in here touches the access: the hook performed it as the guest asked before the call.
 *
 * Lock-free: a record's slot is claimed with one atomic add, its time is written last, so a slot
 * with a time is complete. The buffer never wraps; the slot past the last ends the record
 * (GFXREC_DONE_FULL) and claimed - cap counts what was lost. The window's end is exact in time:
 * the first access at or after t_end ends it (GFXREC_DONE_TIME). The MMIO ring stops there too, so
 * the host can read the bring-up before the doorbell from it at the desktop.
 */

#ifdef GFXREC_HOST
#include "gfxrec_host.h"
#else
#include "sptm_virt_gfxrec.h"
#include "string.h"
#include "utils.h"
#endif

u32 sptm_virt_gfxrec_ms;
u32 sptm_virt_gfxrec_max = 262144;
struct gfxrec_hdr *sptm_virt_gfxrec_buf;
u64 sptm_virt_gfxrec_ring_stop;
u64 sptm_virt_gfxrec_blk_pa, sptm_virt_gfxrec_blk_len;
u32 sptm_virt_gfxrec_blk_max = 131072;
struct gfxblk_hdr *sptm_virt_gfxrec_blk;

static u32 gfxrec_triggered;

/* The host's reader (proxyclient/aura/hvgfxrec.py) has these sizes too. */
_Static_assert(sizeof(struct gfxrec_hdr) == 512, "gfxrec_hdr");
_Static_assert(sizeof(struct gfxrec_rec) == 32, "gfxrec_rec");
_Static_assert(sizeof(struct gfxblk_hdr) == 128, "gfxblk_hdr");
_Static_assert(sizeof(struct gfxblk_rec) == 32, "gfxblk_rec");

/* The sgx writes taken: {offset from sgx reg 0, bytes}. */
static const u32 gfxrec_sgx_w[][2] = {
    {0xd60000, 0x68},
    {0x1000104, 4},
    {0x1000108, 4},
};

static struct gfxrec_rec *gfxrec_recs(struct gfxrec_hdr *h)
{
    return (struct gfxrec_rec *)((u8 *)h + sizeof(*h));
}

void sptm_virt_gfxrec_init(void *buf, u64 bytes, u64 cntfrq, u32 window_ms)
{
    sptm_virt_gfxrec_buf = NULL;
    sptm_virt_gfxrec_ring_stop = 0;
    gfxrec_triggered = 0;
    if (!buf || bytes < sizeof(struct gfxrec_hdr) + sizeof(struct gfxrec_rec) || !window_ms)
        return;
    struct gfxrec_hdr *h = buf;
    memset(h, 0, sizeof(*h));
    h->magic = GFXREC_MAGIC;
    h->version = GFXREC_VERSION;
    h->hdr_bytes = sizeof(*h);
    h->rec_bytes = sizeof(struct gfxrec_rec);
    h->cap = (bytes - sizeof(*h)) / sizeof(struct gfxrec_rec);
    h->cntfrq = cntfrq;
    h->window_ms = window_ms;
    for (u32 i = 0; i < ARRAY_SIZE(gfxrec_sgx_w) && i < GFXREC_SGX_W; i++) {
        h->sgx_w[i][0] = gfxrec_sgx_w[i][0];
        h->sgx_w[i][1] = gfxrec_sgx_w[i][1];
    }
    h->state = GFXREC_ARMED;
    sptm_virt_gfxrec_buf = h;
}

void sptm_virt_gfxrec_source(u32 src, u64 base, u64 size)
{
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    if (h && src < GFXREC_SRCS) {
        h->base[src] = base;
        h->size[src] = size;
    }
}

void sptm_virt_gfxrec_add_ps(u64 pa, const char *name, bool trapped)
{
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    if (!h || h->n_ps >= GFXREC_PS_MAX)
        return;
    for (u32 k = 0; k < h->n_ps; k++)
        if (h->ps[k] == pa)
            return;
    u32 k = h->n_ps++;
    h->ps[k] = pa;
    for (u32 i = 0; i < sizeof(h->ps_name[k]) - 1 && name && name[i]; i++)
        h->ps_name[k][i] = name[i];
    if (trapped)
        h->ps_trapped |= 1U << k;
}

bool sptm_virt_gfxrec_trigger(u64 t, u64 val, u64 hi, u64 pc, u32 cpu)
{
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    if (!h)
        return false;
    if (__atomic_exchange_n(&gfxrec_triggered, 1, __ATOMIC_SEQ_CST))
        return false;
    u64 t_end = t + h->window_ms * (h->cntfrq / 1000);
    h->t_start = t;
    h->t_end = t_end;
    h->trig_val = val;
    h->trig_hi = hi;
    h->trig_pc = pc;
    h->trig_cpu = cpu;
    __atomic_store_n(&sptm_virt_gfxrec_ring_stop, t_end, __ATOMIC_RELAXED);
    __atomic_store_n(&h->state, GFXREC_ON, __ATOMIC_RELEASE);
    return true;
}

/* Is this access one the record takes? src is its window. */
static bool gfxrec_takes(struct gfxrec_hdr *h, u32 src, u64 addr, bool write, int width)
{
    u64 off = addr - h->base[src];

    if (src <= GFXREC_SRC_PMP_ASC)
        return write ? off >= GFXREC_A2I && off < GFXREC_A2I + 16
                     : off >= GFXREC_I2A && off < GFXREC_I2A + 16;
    if (src == GFXREC_SRC_SGX && write) {
        for (u32 i = 0; i < GFXREC_SGX_W && h->sgx_w[i][1]; i++)
            if (off >= h->sgx_w[i][0] && off < h->sgx_w[i][0] + h->sgx_w[i][1])
                return true;
    }
    (void)width;
    return false;
}

/* The window an access is in: the ASCs before sgx, whose window holds gfx-asc's. */
static int gfxrec_src(struct gfxrec_hdr *h, u64 addr, int width)
{
    for (u32 s = 0; s < GFXREC_SRCS; s++) {
        if (h->size[s] && addr >= h->base[s] && addr - h->base[s] < h->size[s])
            return s;
    }
    u64 bytes = 1UL << width;
    for (u32 k = 0; k < h->n_ps; k++) {
        if (addr < h->ps[k] + 4 && addr + bytes > h->ps[k])
            return GFXREC_SRC_PMGR;
    }
    return -1;
}

void sptm_virt_gfxrec_note(u64 addr, const u64 *val, bool write, int width, u32 cpu, u64 t)
{
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;

    if (!h || __atomic_load_n(&h->state, __ATOMIC_ACQUIRE) != GFXREC_ON)
        return;
    if (width < 0 || width > 4)
        return;
    int src = gfxrec_src(h, addr, width);
    if (src < 0 || (src != GFXREC_SRC_PMGR && !gfxrec_takes(h, src, addr, write, width)))
        return;
    if (src == GFXREC_SRC_PMGR && !write)
        return;
    if (t >= h->t_end) {
        u32 on = GFXREC_ON;
        if (__atomic_compare_exchange_n(&h->state, &on, GFXREC_DONE_TIME, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
            h->t_done = t;
        return;
    }
    u64 i = __atomic_fetch_add(&h->claimed, 1, __ATOMIC_RELAXED);
    if (i >= h->cap) {
        u32 on = GFXREC_ON;
        if (__atomic_compare_exchange_n(&h->state, &on, GFXREC_DONE_FULL, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
            h->t_done = t;
        return;
    }
    struct gfxrec_rec *r = &gfxrec_recs(h)[i];
    r->val = val[0];
    r->hi = width == 4 ? val[1] : 0;
    r->meta = (addr & 0xffffffffffffUL) | ((u64)src << 48) | ((u64) !!write << 52) |
              ((u64)(width & 7) << 53) | ((u64)(cpu & 15) << 56);
    __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);
}

/* ---- the MMIO_PROTECTED block ------------------------------------------------------------- */

static struct gfxblk_rec *gfxblk_recs(struct gfxblk_hdr *b)
{
    return (struct gfxblk_rec *)((u8 *)b + sizeof(*b));
}

void sptm_virt_gfxrec_blk_init(void *buf, u64 bytes, u64 cntfrq, u64 base, u64 size,
                               const char *node)
{
    sptm_virt_gfxrec_blk = NULL;
    if (!buf || bytes < sizeof(struct gfxblk_hdr) + 2 * sizeof(struct gfxblk_rec) || !size)
        return;
    struct gfxblk_hdr *b = buf;
    memset(b, 0, sizeof(*b));
    b->magic = GFXBLK_MAGIC;
    b->version = GFXREC_VERSION;
    b->hdr_bytes = sizeof(*b);
    b->rec_bytes = sizeof(struct gfxblk_rec);
    b->cap = (bytes - sizeof(*b)) / sizeof(struct gfxblk_rec);
    b->base = base;
    b->size = size;
    b->cntfrq = cntfrq;
    for (u32 i = 0; i < sizeof(b->node) - 1 && node && node[i]; i++)
        b->node[i] = node[i];
    b->state = GFXREC_ON;
    sptm_virt_gfxrec_blk = b;
}

void sptm_virt_gfxrec_blk_note(u64 addr, const u64 *val, bool write, int width, u32 cpu, u64 t,
                               u64 pc, bool gl)
{
    struct gfxblk_hdr *b = sptm_virt_gfxrec_blk;

    if (!b || __atomic_load_n(&b->state, __ATOMIC_ACQUIRE) != GFXREC_ON)
        return;
    if (addr - b->base >= b->size || width < 0 || width > 4)
        return;
    /* The mailbox record's window end, once its doorbell set one. */
    struct gfxrec_hdr *h = sptm_virt_gfxrec_buf;
    u64 end = h && __atomic_load_n(&h->state, __ATOMIC_ACQUIRE) >= GFXREC_ON ? h->t_end : 0;
    if (end && t >= end) {
        u32 on = GFXREC_ON;
        if (__atomic_compare_exchange_n(&b->state, &on, GFXREC_DONE_TIME, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
            b->t_done = t;
        return;
    }
    u32 n = width == 4 ? 2 : 1;
    u64 i = __atomic_fetch_add(&b->claimed, n, __ATOMIC_RELAXED);
    if (i + n > b->cap) {
        u32 on = GFXREC_ON;
        if (__atomic_compare_exchange_n(&b->state, &on, GFXREC_DONE_FULL, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_RELAXED))
            b->t_done = t;
        return;
    }
    if (!i)
        b->t_first = t;
    for (u32 k = 0; k < n; k++) {
        struct gfxblk_rec *r = &gfxblk_recs(b)[i + k];
        r->val = val[k];
        r->pc = pc;
        r->meta = ((addr + 8 * k) & 0xffffffffffffUL) | ((u64) !!write << 52) |
                  ((u64)(n == 2 ? 3 : width & 7) << 53) | ((u64)(cpu & 15) << 56) |
                  ((u64) !!gl << 60) | ((u64)(n == 2) << 61);
        __atomic_store_n(&r->t, t, __ATOMIC_RELEASE);
    }
}
