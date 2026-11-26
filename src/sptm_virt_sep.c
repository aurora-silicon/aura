/* SPDX-License-Identifier: MIT */

/*
 * SEP mailbox trace under the virtualized SPTM (SPTM_VIRT_SEP, debug only).
 *
 * A pass-through stage-2 hook on the SEP's ASC mailbox page records every message the guest
 * sends and every reply it takes, into the record ring auractl reads (sptm_virt_s2r.c). The
 * control endpoint's OOL messages (types 2-5: inbound/outbound address and size, param = the
 * endpoint) tell which shared buffers each endpoint uses; on each send to and reply from an
 * endpoint that has them, both buffers are copied into the ring as raw records, through a
 * non-cacheable window, so the copy is what the SEP itself reads and writes.
 *
 * Ring records (struct s2r_rec): the mailbox words as ordinary accesses (pa = the register);
 * a buffer copy as one header, pc = 'SEPH' | seq << 32, val = ep | buf << 8 (0 in, 1 out) |
 * dir << 9 (1 = reply) | words << 16, pa = the buffer's IOVA; then its words, pc = 'SEPD' |
 * seq << 32, pa = byte offset, val = the 8 bytes. Trailing zero words are not copied.
 *
 * The guest's buffers are found through dart-sep's live tables (T8110 layout), read at each
 * copy, so tables the guest builds after entry are followed.
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

#ifdef AURACTL /* the guest's RAM range and the ring reader are auractl's */

#define SEP_WIN  0xb100000000UL /* aura VA of the one-page window onto a guest page */
#define SEP_PG   0x4000UL
#define SEP_MAXB (128UL << 10) /* bytes copied per buffer at most */

#define ASC_MBOX_OFF  0x8000
#define A2I_SEND0     0x800
#define I2A_RECV0     0x830
#define T8110_TCR     0x1000
#define T8110_TTBR    0x1400
#define T8110_PROTECT 0x100

u32 sptm_virt_sep_maxb = SEP_MAXB; /* set by the host before entry (AURA_SEP_MAXB) */

static u64 sep_mbox, sep_dart;
static u64 sep_ram_base, sep_ram_end;
static u64 sep_mapped;
static u32 sep_seq;
static u64 last_page = ~0UL, last_pa; /* sep_word's translation of one page */
static u32 sep_unmapped;
static DECLARE_SPINLOCK(sep_lock);
/* On the heap: as .bss it would move aura's stacks (see sptm_virt_gpu.c). */
static struct sep_ool {
    u64 iova[2];
    u32 size[2];
} *sep_ool;

AURA_XTEXT static void sep_unmap(void)
{
    if (!sep_mapped)
        return;
    mmu_rm_mapping(SEP_WIN, SEP_PG);
    sysop("dsb ish");
    sysop("tlbi alle2is");
    sysop("dsb ish");
    sysop("isb");
    sep_mapped = 0;
}

/* One IOVA through dart-sep's live tables, any stream that translates; 0 = not mapped. */
AURA_XTEXT static u64 sep_translate(u64 iova)
{
    for (u32 sid = 0; sid < 16; sid++) {
        u32 tcr = read32(sep_dart + T8110_TCR + 4 * sid);
        u32 ttbr = read32(sep_dart + T8110_TTBR + 4 * sid);
        if ((tcr & 3) == 2) /* bypass */
            return iova;
        if (!(tcr & 1) || !(ttbr & 1))
            continue;
        u64 l1 = (u64)((ttbr >> 2) & GENMASK(27, 0)) << 14;
        if (l1 < sep_ram_base || l1 >= sep_ram_end)
            continue;
        u64 i = (iova >> 25) & 0x7ff, j = (iova >> 14) & 0x7ff;
        u64 pte = ((const volatile u64 *)l1)[i];
        if (!(pte & 1))
            continue;
        u64 l2 = ((pte >> 10) & GENMASK(29, 0)) << 14;
        if (l2 < sep_ram_base || l2 >= sep_ram_end)
            continue;
        pte = ((const volatile u64 *)l2)[j];
        if (!(pte & 1))
            continue;
        return (((pte >> 10) & GENMASK(29, 0)) << 14) | (iova & (SEP_PG - 1));
    }
    return 0;
}

AURA_XTEXT static const u64 *sep_map(u64 pa)
{
    u64 page = pa & ~(SEP_PG - 1);
    if (page != sep_mapped) {
        sep_unmap();
        mmu_add_mapping(SEP_WIN, page, SEP_PG, MAIR_IDX_NORMAL_NC, PERM_RW);
        sep_mapped = page;
    }
    return (const u64 *)(SEP_WIN + (pa & (SEP_PG - 1)));
}

/* The guest word at iova, through a non-cacheable window; false if it is not mapped. */
AURA_XTEXT static bool sep_word(u64 iova, u64 *v)
{
    u64 page = iova & ~(SEP_PG - 1);
    if (page != last_page) {
        last_pa = sep_translate(page);
        last_page = page;
    }
    u64 pa = last_pa ? last_pa + (iova & (SEP_PG - 1)) : 0;
    if (!pa || pa < sep_ram_base || pa >= sep_ram_end) {
        if (sep_unmapped++ < 8)
            printf("SPTM-virt[sep]: IOVA 0x%lx not mapped (pa 0x%lx)\n", iova, pa);
        return false;
    }
    *v = *sep_map(pa);
    return true;
}

/* Copy one of ep's buffers into the ring; caller holds sep_lock. */
AURA_XTEXT static void sep_copy(u8 ep, int buf, bool reply)
{
    u64 iova = sep_ool[ep].iova[buf];
    u64 size = min((u64)sep_ool[ep].size[buf], (u64)sptm_virt_sep_maxb) & ~7UL;
    if (!iova || !size)
        return;

    /* the last non-zero word: buffers are mostly empty. Tables re-read per copy. */
    u64 words = 0, v;
    last_page = ~0UL; /* the guest may have remapped since the last copy */
    for (u64 off = 0; off < size; off += 8)
        if (sep_word(iova + off, &v) && v)
            words = off / 8 + 1;
    u32 seq = ++sep_seq;
    sptm_virt_s2r_rec_raw(iova, ep | (u64)buf << 8 | (u64)reply << 9 | words << 16,
                          0x48504553UL | (u64)seq << 32, reply);
    for (u64 w = 0; w < words; w++) {
        v = 0;
        sep_word(iova + w * 8, &v);
        sptm_virt_s2r_rec_raw(w * 8, v, 0x44504553UL | (u64)seq << 32, reply);
    }
}

AURA_XTEXT static void sep_msg(struct exc_info *ctx, u64 addr, u64 msg0, bool reply)
{
    u8 ep = msg0 & 0xff, ty = (msg0 >> 16) & 0xff, param = (msg0 >> 24) & 0xff;
    u32 data = msg0 >> 32;

    spin_lock(&sep_lock);
    sptm_virt_s2r_note(ctx, addr, msg0, !reply, 3);
    if (!reply && ep == 0 && ty >= 2 && ty <= 5) {
        int buf = (ty == 3 || ty == 5); /* 2/4 inbound, 3/5 outbound */
        if (ty <= 3)
            sep_ool[param].iova[buf] = (u64)data << 12;
        else
            sep_ool[param].size[buf] = data;
    } else if (ep && ep < 0xfd) {
        sep_copy(ep, 0, reply);
        sep_copy(ep, 1, reply);
    }
    spin_unlock(&sep_lock);
}

AURA_XTEXT static bool sep_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    u64 off = addr - sep_mbox;

    if (width == 4) {
        if (write)
            __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
        else
            __asm__ volatile("ldp %0, %1, [%2]"
                             : "=r"(val[0]), "=r"(val[1])
                             : "r"(addr)
                             : "memory");
    } else if (width > 4) {
        return false;
    } else {
        sptm_virt_mmio_rw(addr, val, write, width);
    }

    /* After the access: a send has happened, a reply has been taken. */
    if (write && off == A2I_SEND0 && width >= 3)
        sep_msg(ctx, addr, val[0], false);
    else if (!write && off == I2A_RECV0 && width >= 3)
        sep_msg(ctx, addr, val[0], true);
    return true;
}

void sptm_virt_sep_setup(void)
{
    if (!(sptm_virt_flags & SPTM_VIRT_SEP))
        return;
    int p[8];
    u64 pa, sz;
    if (adt_path_offset_trace(adt, "/arm-io/sep", p) < 0 ||
        adt_get_reg(adt, p, "reg", 0, &pa, &sz) < 0) {
        printf("SPTM-virt[sep]: no /arm-io/sep reg 0; SEP trace off\n");
        return;
    }
    sep_mbox = pa + ASC_MBOX_OFF;
    int dnode = adt_path_offset_trace(adt, "/arm-io/dart-sep", p);
    if (dnode < 0 || adt_get_reg(adt, p, "reg", 0, &pa, &sz) < 0 ||
        !adt_is_compatible(adt, dnode, "dart,t8110")) {
        printf("SPTM-virt[sep]: no T8110 /arm-io/dart-sep; SEP trace off\n");
        return;
    }
    sep_dart = pa;
    u64 size = 0;
    sptm_virt_ctl_guest_ram(&sep_ram_base, &size);
    sep_ram_end = sep_ram_base + size;
    sep_ool = calloc(256, sizeof(*sep_ool));
    if (!size || !sep_ool || !sptm_virt_s2r_ring_ensure()) {
        printf("SPTM-virt[sep]: no guest RAM range or no ring; SEP trace off\n");
        return;
    }
    /* the window's tables now, so a hook never allocates */
    mmu_add_mapping(SEP_WIN, sep_ram_base, SEP_PG, MAIR_IDX_NORMAL_NC, PERM_RW);
    sep_mapped = sep_ram_base;
    sep_unmap();
    if (hv_map_hook(sep_mbox, sep_hook, 0x4000) < 0) {
        printf("SPTM-virt[sep]: cannot hook the mailbox 0x%lx\n", sep_mbox);
        return;
    }
    printf("SPTM-virt[sep]: hooked mailbox 0x%lx, dart-sep 0x%lx, guest RAM [0x%lx, 0x%lx), "
           "%u bytes per buffer, ring %lu records\n",
           sep_mbox, sep_dart, sep_ram_base, sep_ram_end, sptm_virt_sep_maxb,
           (unsigned long)sptm_virt_s2r_nrec);
}
#else
void sptm_virt_sep_setup(void)
{
    if (sptm_virt_flags & SPTM_VIRT_SEP)
        printf("SPTM-virt[sep]: SEP trace needs an AURACTL=1 build\n");
}
#endif
