/* SPDX-License-Identifier: MIT */

/*
 * DCP AFK/EPIC ring trace under the virtualized SPTM (SPTM_VIRT_DCP_AFK, debug only).
 *
 * The display coprocessors' AFK endpoints (0x20-0x3f) carry their EPIC traffic in a ring pair in
 * shared memory, not in the mailbox words. A pass-through stage-2 hook on each DCP's ASC mailbox
 * page follows the endpoints the host selects (sptm_virt_dcp_afk_eps, bit n = endpoint 0x20 + n):
 *
 *   - the coprocessor's GETBUF asks for the buffer, the AP's GETBUF_ACK gives its DVA, and the
 *     coprocessor's INIT_TX/INIT_RX place the AP's transmit and receive rings in it (offset and
 *     size in 64-byte units; each ring has three header blocks, bufsz, rptr and wptr, then data);
 *   - at every message on such an endpoint, either way, both rings' write pointers are read, and
 *     every queue entry between the last pointer seen and the new one (its 16-byte header and
 *     its bytes, the EPIC/AFKv2 header and payload) is copied into the auractl ring;
 *   - an AFKv2 command or response with an out-of-band descriptor also has its two buffers
 *     copied, as they are when the entry passes.
 * The ring layout, the message types and the entry rules are the public ones of the Linux DCP
 * driver (drivers/gpu/drm/apple/afk.c, afk.h, afkv2-codec.h).
 *
 * Ring records (struct s2r_rec; write = the AP sent it), tags in the pc field's low word:
 *   DAFM a mailbox message on a followed endpoint: pc |= dcp << 32 | i2a << 36 | ep << 40,
 *        val = message word 0, pa = the guest PC (low 48 bits)
 *   DAFR a ring placed: pc |= dcp << 32 | ring << 36 | ep << 40 (ring 0 AP transmit, 1 receive),
 *        val = bufsz | stride << 32, pa = the ring header's DVA
 *   DAFH a copy: pc |= seq << 32, val = dcp | ring << 4 | kind << 5 | ep << 8 | bytes << 16 |
 *        full << 40 (kind 0 a queue entry, 1 an OOB rx buffer, 2 an OOB tx buffer; bytes copied,
 *        full = its size, both capped at 2^24 - 1), pa = the entry's offset in the ring's data, or
 *        the OOB buffer's DVA
 *   DAFD its 8-byte words: pc |= seq << 32, pa = byte offset from the copy's start (an OOB copy
 *        starts at its DVA rounded down to 8), val = the bytes
 *   DAFX a problem: pc |= dcp << 32 | code << 36 | ep << 40, val = detail (1 DVA not mapped,
 *        2 ring pointer out of range, 3 bad entry magic, 4 entry budget used up, 5 bad ring header,
 *        6 a DART table outside guest RAM and the DART's pt-regions: val = its PA, 7 a DVA's
 *        page outside guest RAM: val = its PA)
 *   DAFN a DCP traced, written at setup: pc |= dcp << 32, val = its ADT name's first 8 bytes,
 *        pa = its mailbox page
 *
 * The guest's buffers are found through the DCP's DART (/arm-io/dart-<dcp>, T8110 layout), read
 * live, the IOVA being the DVA's low 36 bits (dart.c's rule; the DT's DMA range on T8112 starts
 * at 0x8_0000_0000). The DART's tables may sit in its ADT pt-region-<sid> windows, which iBoot
 * reserves above the guest's RAM (J473 27.2: dart-dcp 0x9ffedc000, guest RAM ends 0x9c2000000);
 * they are read there too. Buffer bytes are only read inside guest RAM, through a non-cacheable
 * window, like the SEP trace: the DCP is not coherent.
 *
 * Only the XNU side is seen (stage 2 is armed at each core's XNU entry), and a ring set up before
 * that (GETBUF/INIT before the hook) is not followed: its mailbox words are still recorded.
 */

#include "../build/build_cfg.h"

#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "sptm_virt.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

/* Host: endpoints followed, bit n = 0x20 + n (default 0x2b); bytes copied per entry/buffer. */
u32 sptm_virt_dcp_afk_eps = BIT(0x2b - 0x20);
u32 sptm_virt_dcp_afk_maxb = 4096;

#ifdef AURACTL

#define DAFK_WIN  0xb200000000UL /* aura VA of the one-page window onto a guest page */
#define DAFK_PG   0x4000UL
#define DAFK_NDCP 4
#define DAFK_NEP  32
#define DAFK_EP0  0x20

#define ASC_MBOX_OFF 0x8000
#define A2I_SEND0    0x800
#define I2A_RECV0    0x830

#define T8110_TCR        0x1000
#define T8110_TTBR       0x1400
#define T8110_TCR_XLATE  BIT(0)
#define T8110_TCR_BYPASS BIT(1)
#define T8110_TCR_REMAP  BIT(7)
#define DART_IOVA_MASK   GENMASK(35, 0)

/* afk.c */
#define RBEP_GETBUF     0x89
#define RBEP_GETBUF_ACK 0xa1
#define RBEP_INIT_TX    0x8a
#define RBEP_INIT_RX    0x8b
#define RBEP_SHUTDOWN   0xc0
#define QE_MAGIC        0x20504f49
#define QE_SIZE         16

#define TAG_M     0x4d464144UL /* "DAFM" */
#define TAG_R     0x52464144UL /* "DAFR" */
#define TAG_H     0x48464144UL /* "DAFH" */
#define TAG_D     0x44464144UL /* "DAFD" */
#define TAG_X     0x58464144UL /* "DAFX" */
#define TAG_N     0x4e464144UL /* "DAFN" */
#define DAFK_NPTR 4            /* pt-region windows kept per DART */

struct dafk_ring {
    u64 hdr;   /* DVA of the ring's header */
    u32 bufsz; /* data bytes */
    u32 stride;
    u32 last; /* the write pointer copied up to */
    bool ready;
};

struct dafk_ep {
    u64 dva; /* the buffer the AP gave (GETBUF_ACK) */
    u32 size;
    u32 want; /* GETBUF's size, until the ACK */
    struct dafk_ring r[2];
};

struct dafk_dcp {
    char name[16];
    u64 mbox, dart;
    u64 m0[2]; /* word 0 of a pair not yet complete: 0 A2I, 1 I2A */
    bool have_m0[2];
    int sid;               /* the stream that last translated, or -1 */
    u64 ptr[DAFK_NPTR][2]; /* the DART's ADT pt-region-<sid> windows [start, end) */
    int nptr;
    u64 bad_table; /* the last table PA refused, said once */
    u64 bad_page;  /* the last buffer page refused, said once */
    struct dafk_ep ep[DAFK_NEP];
};

static struct dafk_dcp *dafk; /* on the heap, as sptm_virt_sep.c's state */
static int dafk_n;
static u64 dafk_ram_base, dafk_ram_end;
static u64 dafk_mapped;
static u32 dafk_seq;
static u32 dafk_complaints;
static DECLARE_SPINLOCK(dafk_lock);

/* the page translation of the copy in progress */
static struct dafk_dcp *xl_dcp;
static u64 xl_page = ~0UL, xl_pa;

AURA_XTEXT static void dafk_unmap(void)
{
    if (!dafk_mapped)
        return;
    mmu_rm_mapping(DAFK_WIN, DAFK_PG);
    sysop("dsb ish");
    sysop("tlbi alle2is");
    sysop("dsb ish");
    sysop("isb");
    dafk_mapped = 0;
}

AURA_XTEXT static void dafk_problem(struct dafk_dcp *d, u8 ep, u32 code, u64 detail);

/* A DART table page the walk may read: in guest RAM or one of the DART's pt-regions. */
AURA_XTEXT static bool dafk_table_ok(struct dafk_dcp *d, u64 pa)
{
    if (pa >= dafk_ram_base && pa < dafk_ram_end)
        return true;
    for (int i = 0; i < d->nptr; i++)
        if (pa >= d->ptr[i][0] && pa < d->ptr[i][1])
            return true;
    if (d->bad_table != pa) {
        d->bad_table = pa;
        dafk_problem(d, 0, 6, pa);
    }
    return false;
}

AURA_XTEXT static u64 dafk_walk(struct dafk_dcp *d, u32 sid, u64 iova)
{
    u32 tcr = read32(d->dart + T8110_TCR + 4 * sid);
    u32 ttbr = read32(d->dart + T8110_TTBR + 4 * sid);
    /* a bypass stream would hand back the IOVA itself: not the DCP's view */
    if (!(tcr & T8110_TCR_XLATE) || (tcr & (T8110_TCR_BYPASS | T8110_TCR_REMAP)) || !(ttbr & 1))
        return 0;
    u64 l1 = (u64)((ttbr >> 2) & GENMASK(27, 0)) << 14;
    if (!dafk_table_ok(d, l1))
        return 0;
    u64 pte = ((const volatile u64 *)l1)[(iova >> 25) & 0x7ff];
    if (!(pte & 1))
        return 0;
    u64 l2 = ((pte >> 10) & GENMASK(29, 0)) << 14;
    if (!dafk_table_ok(d, l2))
        return 0;
    pte = ((const volatile u64 *)l2)[(iova >> 14) & 0x7ff];
    if (!(pte & 1))
        return 0;
    return (((pte >> 10) & GENMASK(29, 0)) << 14) | (iova & (DAFK_PG - 1));
}

/* One DVA through the DCP's live DART tables, the stream that worked last first. */
AURA_XTEXT static u64 dafk_translate(struct dafk_dcp *d, u64 dva)
{
    u64 iova = dva & DART_IOVA_MASK, pa;
    if (d->sid >= 0 && (pa = dafk_walk(d, d->sid, iova)))
        return pa;
    for (u32 sid = 0; sid < 16; sid++)
        if ((int)sid != d->sid && (pa = dafk_walk(d, sid, iova))) {
            d->sid = sid;
            return pa;
        }
    return 0;
}

AURA_XTEXT static void dafk_problem(struct dafk_dcp *d, u8 ep, u32 code, u64 detail)
{
    sptm_virt_s2r_rec_raw(0, detail,
                          TAG_X | (u64)(d - dafk) << 32 | (u64)code << 36 | (u64)ep << 40, false);
    if (dafk_complaints++ < 16)
        printf("SPTM-virt[dcp-afk]: %s ep 0x%x: problem %u (0x%lx)\n", d->name, ep, code, detail);
}

/* The guest word at dva (8-aligned), through a non-cacheable window; false if not mapped. */
AURA_XTEXT static bool dafk_word(struct dafk_dcp *d, u64 dva, u64 *v)
{
    u64 page = dva & ~(DAFK_PG - 1);
    if (xl_dcp != d || page != xl_page) {
        xl_pa = dafk_translate(d, page);
        xl_page = page;
        xl_dcp = d;
    }
    if (!xl_pa)
        return false;
    if (xl_pa < dafk_ram_base || xl_pa >= dafk_ram_end) {
        if (d->bad_page != xl_pa) {
            d->bad_page = xl_pa;
            dafk_problem(d, 0, 7, xl_pa);
        }
        return false;
    }
    u64 pa = xl_pa + (dva & (DAFK_PG - 1));
    if ((pa & ~(DAFK_PG - 1)) != dafk_mapped) {
        dafk_unmap();
        mmu_add_mapping(DAFK_WIN, pa & ~(DAFK_PG - 1), DAFK_PG, MAIR_IDX_NORMAL_NC, PERM_RW);
        dafk_mapped = pa & ~(DAFK_PG - 1);
    }
    *v = *(const volatile u64 *)(DAFK_WIN + (pa & (DAFK_PG - 1)));
    return true;
}

AURA_XTEXT static bool dafk_word32(struct dafk_dcp *d, u64 dva, u32 *v)
{
    u64 w;
    if (!dafk_word(d, dva & ~7UL, &w))
        return false;
    *v = w >> (8 * (dva & 4));
    return true;
}

/* Copy [dva, dva + len) into the ring as one DAFH and its DAFD words. */
AURA_XTEXT static void dafk_copy(struct dafk_dcp *d, u8 ep, int ring, int kind, u64 pa_field,
                                 u64 dva, u64 len, u64 full, bool a2i)
{
    u64 start = dva & ~7UL, end = ALIGN_UP(dva + len, 8);
    u32 seq = ++dafk_seq;
    u64 cap = GENMASK(23, 0);
    sptm_virt_s2r_rec_raw(pa_field,
                          (u64)(d - dafk) | (u64)ring << 4 | (u64)kind << 5 | (u64)ep << 8 |
                              min(len, cap) << 16 | min(full, cap) << 40,
                          TAG_H | (u64)seq << 32, a2i);
    for (u64 a = start; a < end; a += 8) {
        u64 v = 0;
        if (!dafk_word(d, a, &v)) {
            dafk_problem(d, ep, 1, a);
            return;
        }
        sptm_virt_s2r_rec_raw(a - start, v, TAG_D | (u64)seq << 32, a2i);
    }
}

/* An AFKv2 entry's OOB descriptor (afkv2-codec.h): copy the buffers it names. */
AURA_XTEXT static void dafk_oob(struct dafk_dcp *d, u8 ep, int ring, u64 body, u32 size, bool a2i)
{
    u32 w0, w16, len;
    /* 24-byte frame header, then the 8-byte command and the 24-byte OOB part; only a command
     * (category 1) or a response (2): a report's payload is not a command word */
    if (size < 24 + 8 + 24 || !dafk_word32(d, body + 4, &len) || len < 16 + 8 + 24 ||
        len > size - 8 || !dafk_word32(d, body + 16, &w16) || ((w16 >> 8) & 0xff) < 1 ||
        ((w16 >> 8) & 0xff) > 2 || !dafk_word32(d, body + 24, &w0) || (w0 & 0xfe) || (w0 >> 16) ||
        !(w0 & 1))
        return;
    u64 rx, tx, sizes;
    if (!dafk_word(d, body + 32, &rx) || !dafk_word(d, body + 40, &tx) ||
        !dafk_word(d, body + 48, &sizes))
        return;
    u32 rxs = sizes, txs = sizes >> 32;
    if (rx && rxs)
        dafk_copy(d, ep, ring, 1, rx & GENMASK(47, 0), rx,
                  min((u64)rxs, (u64)sptm_virt_dcp_afk_maxb), rxs, a2i);
    if (tx && txs)
        dafk_copy(d, ep, ring, 2, tx & GENMASK(47, 0), tx,
                  min((u64)txs, (u64)sptm_virt_dcp_afk_maxb), txs, a2i);
}

/* Copy the entries the ring's producer added since the last look (afk.c's afk_recv rules). */
AURA_XTEXT static void dafk_scan(struct dafk_dcp *d, u8 ep, int ring)
{
    struct dafk_ring *r = &d->ep[ep - DAFK_EP0].r[ring];
    u32 w;
    if (!r->ready)
        return;
    if (!dafk_word32(d, r->hdr + 2 * r->stride, &w)) {
        dafk_problem(d, ep, 1, r->hdr + 2 * r->stride);
        return;
    }
    if (w == r->last)
        return;
    if (w >= r->bufsz || w % r->stride) {
        dafk_problem(d, ep, 2, (u64)ring << 32 | w);
        r->last = w < r->bufsz ? ALIGN_DOWN(w, r->stride) : 0;
        return;
    }
    u64 data = r->hdr + 3 * r->stride;
    u32 p = r->last;
    bool a2i = ring == 0;
    for (u32 n = 0; p != w; n++) {
        u32 magic, size, hdr[2];
        if (n > r->bufsz / r->stride) {
            dafk_problem(d, ep, 4, (u64)ring << 32 | p);
            break;
        }
        if (!dafk_word32(d, data + p, &magic) || !dafk_word32(d, data + p + 4, &size) ||
            magic != QE_MAGIC) {
            dafk_problem(d, ep, 3, (u64)ring << 32 | p);
            break;
        }
        /* not room for it before the end: the producer put it at 0, a dummy header here */
        if (p > r->bufsz - QE_SIZE || size > r->bufsz - p - QE_SIZE) {
            p = 0;
            if (!dafk_word32(d, data, &magic) || !dafk_word32(d, data + 4, &size) ||
                magic != QE_MAGIC || size > r->bufsz - QE_SIZE) {
                dafk_problem(d, ep, 3, (u64)ring << 32 | 0);
                break;
            }
        }
        dafk_copy(d, ep, ring, 0, p, data + p, QE_SIZE + min(size, sptm_virt_dcp_afk_maxb),
                  QE_SIZE + size, a2i);
        /* channel 0, type 0: the AFKv2/compact framing */
        if (dafk_word32(d, data + p + 8, &hdr[0]) && dafk_word32(d, data + p + 12, &hdr[1]) &&
            !hdr[0] && !hdr[1])
            dafk_oob(d, ep, ring, data + p + QE_SIZE, size, a2i);
        p = ALIGN_UP(p + QE_SIZE + size, r->stride);
        if (p >= r->bufsz)
            p = 0;
    }
    r->last = w;
}

/* INIT_TX/INIT_RX: the ring's place in the buffer; its header holds bufsz (afk_init_rxtx). */
AURA_XTEXT static void dafk_place(struct dafk_dcp *d, u8 ep, int ring, u64 msg)
{
    struct dafk_ep *e = &d->ep[ep - DAFK_EP0];
    struct dafk_ring *r = &e->r[ring];
    u32 base = ((msg >> 32) & 0xffff) << 6, size = ((msg >> 16) & 0xffff) << 6, bufsz, w;
    memset(r, 0, sizeof(*r));
    if (!e->dva || base >= e->size || size > e->size - base ||
        !dafk_word32(d, e->dva + base, &bufsz)) {
        dafk_problem(d, ep, 5, msg);
        return;
    }
    u32 header = size > bufsz ? size - bufsz : 0, block = header / 3;
    if (!header || header % 3 || block < 0x40 || block % 0x40 || bufsz < block || bufsz % block) {
        dafk_problem(d, ep, 5, (u64)size << 32 | bufsz);
        return;
    }
    r->hdr = e->dva + base;
    r->bufsz = bufsz;
    r->stride = block;
    r->ready = true;
    r->last = dafk_word32(d, r->hdr + 2 * block, &w) && w < bufsz && !(w % block) ? w : 0;
    sptm_virt_s2r_rec_raw(r->hdr & GENMASK(47, 0), bufsz | (u64)block << 32,
                          TAG_R | (u64)(d - dafk) << 32 | (u64)ring << 36 | (u64)ep << 40, false);
}

AURA_XTEXT static void dafk_msg(struct exc_info *ctx, struct dafk_dcp *d, u64 m0, u64 m1, int i2a)
{
    u8 ep = m1 & 0xff;
    if (ep < DAFK_EP0 || ep >= DAFK_EP0 + DAFK_NEP || !(sptm_virt_dcp_afk_eps & BIT(ep - DAFK_EP0)))
        return;
    struct dafk_ep *e = &d->ep[ep - DAFK_EP0];
    u32 type = m0 >> 48;

    sptm_virt_s2r_rec_raw(ctx ? ctx->elr & GENMASK(47, 0) : 0, m0,
                          TAG_M | (u64)(d - dafk) << 32 | (u64)i2a << 36 | (u64)ep << 40, !i2a);
    xl_dcp = NULL; /* the guest may have remapped since the last message */
    if (i2a && type == RBEP_GETBUF) {
        e->want = ((m0 >> 16) & 0xffff) << 6;
    } else if (!i2a && type == RBEP_GETBUF_ACK) {
        u32 want = e->want;
        memset(e, 0, sizeof(*e));
        e->dva = m0 & GENMASK(47, 0);
        e->size = want;
    } else if (i2a && (type == RBEP_INIT_TX || type == RBEP_INIT_RX)) {
        dafk_place(d, ep, type == RBEP_INIT_RX, m0);
    } else {
        /* anything else may follow a producer's step: both rings, the AP's first */
        dafk_scan(d, ep, 0);
        dafk_scan(d, ep, 1);
        if (!i2a && type == RBEP_SHUTDOWN)
            e->r[0].ready = e->r[1].ready = false;
    }
}

AURA_XTEXT static bool dafk_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    struct dafk_dcp *d = NULL;
    for (int i = 0; i < dafk_n; i++)
        if (dafk[i].mbox == (addr & ~(DAFK_PG - 1)))
            d = &dafk[i];
    if (!d || width > 4)
        return false;
    if (width == 4) {
        if (write)
            __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
        else
            __asm__ volatile("ldp %0, %1, [%2]"
                             : "=r"(val[0]), "=r"(val[1])
                             : "r"(addr)
                             : "memory");
    } else {
        sptm_virt_mmio_rw(addr, val, write, width);
    }

    /* After the access: a send has happened (A2I), a message has been taken (I2A). */
    u64 off = addr & (DAFK_PG - 1);
    int dir = off >= I2A_RECV0 ? 1 : 0;
    u64 base = dir ? I2A_RECV0 : A2I_SEND0;
    if (write != !dir || (off & ~8UL) != base || width < 3)
        return true;
    spin_lock(&dafk_lock);
    if (width == 4 && off == base) {
        d->have_m0[dir] = false;
        dafk_msg(ctx, d, val[0], val[1], dir);
    } else if (width == 3 && off == base) {
        d->m0[dir] = val[0];
        d->have_m0[dir] = true;
    } else if (width == 3 && off == base + 8 && d->have_m0[dir]) {
        d->have_m0[dir] = false;
        dafk_msg(ctx, d, d->m0[dir], val[0], dir);
    }
    spin_unlock(&dafk_lock);
    return true;
}

AURA_XTEXT static bool dafk_compat(int node, const char *prefix)
{
    u32 len = 0;
    const char *c = adt_getprop(adt, node, "compatible", &len);
    return c && len > strlen(prefix) && !strncmp(c, prefix, strlen(prefix));
}

/* A DCP by its ADT node name: its mailbox page and its T8110 DART /arm-io/dart-<name>. */
AURA_XTEXT static void dafk_add(const char *name)
{
    char path[48];
    int p[8];
    u64 pa, sz, dpa, dsz;

    if (dafk_n >= DAFK_NDCP)
        return;
    snprintf(path, sizeof(path), "/arm-io/%s", name);
    if (adt_path_offset_trace(adt, path, p) < 0 || adt_get_reg(adt, p, "reg", 0, &pa, &sz) < 0)
        return;
    snprintf(path, sizeof(path), "/arm-io/dart-%s", name);
    int dnode = adt_path_offset_trace(adt, path, p);
    if (dnode < 0 || adt_get_reg(adt, p, "reg", 0, &dpa, &dsz) < 0 ||
        !adt_is_compatible(adt, dnode, "dart,t8110") || dsz < T8110_TTBR + 4 * 16) {
        printf("SPTM-virt[dcp-afk]: %s: no T8110 %s; not traced\n", name, path);
        return;
    }
    u64 mbox = pa + ASC_MBOX_OFF;
    if (sptm_virt_rg_owns(mbox) || !(hv_pt_walk(mbox) & PTE_VALID)) {
        printf("SPTM-virt[dcp-afk]: %s: mailbox 0x%lx already hooked (S2R COPROC?); not traced\n",
               name, mbox);
        return;
    }
    struct dafk_dcp *d = &dafk[dafk_n];
    memset(d, 0, sizeof(*d));
    strncpy(d->name, name, sizeof(d->name) - 1);
    d->mbox = mbox;
    d->dart = dpa;
    d->sid = -1;
    for (int sid = 0; sid < 16 && d->nptr < DAFK_NPTR; sid++) {
        u32 len = 0;
        snprintf(path, sizeof(path), "pt-region-%d", sid);
        const u8 *pr = adt_getprop(adt, dnode, path, &len);
        u64 lo, hi;
        if (!pr || len != 16)
            continue;
        memcpy(&lo, pr, 8);
        memcpy(&hi, pr + 8, 8);
        /* only inside aura's own map of DRAM (memory.c), which the walk reads directly */
        u64 dram = ALIGN_DOWN(cur_boot_args.phys_base, BIT(32));
        if (hi <= lo || lo < dram || hi > dram + mem_size_actual) {
            printf("SPTM-virt[dcp-afk]: %s: pt-region-%d [0x%lx, 0x%lx) outside DRAM; ignored\n",
                   name, sid, lo, hi);
            continue;
        }
        d->ptr[d->nptr][0] = lo;
        d->ptr[d->nptr][1] = hi;
        d->nptr++;
        printf("SPTM-virt[dcp-afk]: %s: DART tables may sit in pt-region-%d [0x%lx, 0x%lx)\n", name,
               sid, lo, hi);
    }
    dafk_n++; /* before the hook: its lookup must find it */
    if (hv_map_hook(mbox, dafk_hook, DAFK_PG) < 0) {
        dafk_n--;
        printf("SPTM-virt[dcp-afk]: %s: cannot hook the mailbox 0x%lx\n", name, mbox);
        return;
    }
    u64 tag = 0;
    memcpy(&tag, d->name, min(strlen(d->name), sizeof(tag)));
    sptm_virt_s2r_rec_raw(mbox, tag, TAG_N | (u64)(dafk_n - 1) << 32, false);
    printf("SPTM-virt[dcp-afk]: %d %s: hooked mailbox 0x%lx, dart 0x%lx\n", dafk_n - 1, name, mbox,
           dpa);
}

AURA_XTEXT void sptm_virt_dcp_afk_setup(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_DCP_AFK))
        return;
    u64 size = 0;
    sptm_virt_ctl_guest_ram(&dafk_ram_base, &size);
    dafk_ram_end = dafk_ram_base + size;
    dafk = calloc(DAFK_NDCP, sizeof(*dafk));
    if (!size || !dafk || !sptm_virt_s2r_ring_ensure()) {
        printf("SPTM-virt[dcp-afk]: no guest RAM range or no ring; DCP AFK trace off\n");
        return;
    }
    if (!sptm_virt_dcp_afk_maxb)
        sptm_virt_dcp_afk_maxb = 4096;
    if (sptm_virt_dcp_afk_maxb > GENMASK(23, 0)) /* the DAFH byte count's width */
        sptm_virt_dcp_afk_maxb = GENMASK(23, 0);
    /* the window's tables now, so a hook never allocates */
    mmu_add_mapping(DAFK_WIN, dafk_ram_base, DAFK_PG, MAIR_IDX_NORMAL_NC, PERM_RW);
    dafk_mapped = dafk_ram_base;
    dafk_unmap();

    int bus = adt_path_offset(adt, "/arm-io");
    if (bus >= 0)
        ADT_FOREACH_CHILD(adt, bus)
        {
            const char *name = adt_get_name(adt, bus);
            if (!strncmp(name, "dcp", 3) && dafk_compat(bus, "iop,ascwrap"))
                dafk_add(name);
        }
    printf("SPTM-virt[dcp-afk]: %d DCPs, endpoints mask 0x%x (bit n = 0x%x + n), %u bytes per "
           "entry/buffer, guest RAM [0x%lx, 0x%lx), ring %lu records\n",
           dafk_n, sptm_virt_dcp_afk_eps, DAFK_EP0, sptm_virt_dcp_afk_maxb, dafk_ram_base,
           dafk_ram_end, (unsigned long)sptm_virt_s2r_nrec);
}
#else
void sptm_virt_dcp_afk_setup(void)
{
    if (sptm_virt_flags & SPTM_VIRT_DCP_AFK)
        printf("SPTM-virt[dcp-afk]: DCP AFK trace needs an AURACTL=1 build\n");
}
#endif
