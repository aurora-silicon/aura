/* SPDX-License-Identifier: MIT */

/*
 * The virtualized SPTM's hooks on power-management registers.
 *
 * aura's debug link (DockChannel, and the ATC0 USB port the CDC proxy uses) is hidden from XNU,
 * but XNU's PMGR driver still lowers the link's power domains: a hook on those PS words keeps them
 * on while macOS runs.
 *
 * MMIO addresses are T8140's, as is everything in sptm_virt*.c.
 */

#include "sptm_virt.h"
#include "adt.h"
#include "exception.h"
#include "hv.h"
#include "pmgr.h"
#include "sptm_virt_priv.h"
#include "string.h"
#include "types.h"
#include "utils.h"

u32 sptm_virt_s2r_pages; /* pages hooked */

static struct {
    u64 page;
    const char *what;
    bool seen;
} s2r_pages[160];

/*
 * The debug link's power domains: the PS words of the ATC0 USB controller, its DART and PHY, and
 * all their ancestors. A write that lowers one is not performed, and reads then report what XNU
 * asked for, so its polls complete.
 */
#define S2R_KEEP_MAX 32
static u64 s2r_keep[S2R_KEEP_MAX];
static u32 s2r_keep_shadow[S2R_KEEP_MAX];
static bool s2r_keep_low[S2R_KEEP_MAX], s2r_keep_told[S2R_KEEP_MAX];
static int s2r_nkeep;
/* With the proxy on CDC the rule holds from setup: XNU lowers three of these domains about a
 * second into its boot. */
static bool s2r_keep_always;
static const char *const s2r_link_paths[] = {"/arm-io/usb-drd", "/arm-io/dart-usb",
                                             "/arm-io/atc-phy0"};

static const char *s2r_page_name(u64 page, bool *first)
{
    for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
        if (s2r_pages[i].page == page) {
            *first = !s2r_pages[i].seen;
            s2r_pages[i].seen = true;
            return s2r_pages[i].what;
        }
    *first = false;
    return "?";
}

/* A 16-byte access (a trapped LDP/STP: width 4 and two values) as one LDP/STP, as the guest issued
 * it. The ASC mailboxes take their messages as such a pair. */
static void s2r_rw128(u64 addr, u64 *val, bool write)
{
    if (write)
        __asm__ volatile("stp %0, %1, [%2]" ::"r"(val[0]), "r"(val[1]), "r"(addr) : "memory");
    else
        __asm__ volatile("ldp %0, %1, [%2]" : "=r"(val[0]), "=r"(val[1]) : "r"(addr) : "memory");
}

static int s2r_keep_idx(u64 addr)
{
    for (int i = 0; i < s2r_nkeep; i++)
        if (s2r_keep[i] == (addr & ~3UL))
            return i;
    return -1;
}

static bool s2r_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    int k = width == 2 && s2r_keep_always ? s2r_keep_idx(addr) : -1;

    /* The link's domains: XNU's lowering writes are shadowed, and reads then see them. */
    if (k >= 0 && write && (*val & 0xf) < (read32(addr) & 0xf)) {
        s2r_keep_shadow[k] = *val;
        s2r_keep_low[k] = true;
        if (!s2r_keep_told[k]) {
            s2r_keep_told[k] = true;
            printf("SPTM-virt[s2r]: kept debug-link domain on: "
                   "XNU wrote 0x%lx -> 0x%lx (pc 0x%lx)\n",
                   *val, addr, ctx->elr);
        }
        return true;
    }
    if (k >= 0 && !write && s2r_keep_low[k]) {
        u32 tgt = s2r_keep_shadow[k] & 0xf;
        *val = (read32(addr) & ~0xffUL) | tgt | (tgt << 4);
        return true;
    }
    if (k >= 0 && write) {
        /* Not lowering it. */
        s2r_keep_low[k] = !(*val & 0xf);
        if (s2r_keep_low[k])
            s2r_keep_shadow[k] = *val;
    }

    bool first;
    const char *what = s2r_page_name(addr & ~0x3fffUL, &first);
    if (width == 4) {
        s2r_rw128(addr, val, write);
        return true;
    }
    if (width > 4) {
        printf("SPTM-virt[s2r]: %u-byte access at 0x%lx (pc 0x%lx) not supported\n", 1u << width,
               addr, ctx->elr);
        return false;
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    if (first)
        printf("SPTM-virt[s2r]: first access to %s page 0x%lx: %s 0x%lx <- 0x%lx pc 0x%lx\n", what,
               addr & ~0x3fffUL, write ? "wr" : "rd", addr, *val, ctx->elr);
    return true;
}

static void s2r_hook_range(const char *what, u64 base, u64 size)
{
    for (u64 page = base & ~0x3fffUL; page < base + size; page += 0x4000) {
        if (sptm_virt_rg_owns(page))
            continue; /* the reset guard records into the ring itself */
        bool dup = false;
        for (u32 i = 0; i < sptm_virt_s2r_pages; i++)
            dup |= s2r_pages[i].page == page;
        if (dup)
            continue; /* already hooked under a more specific name */
        if (sptm_virt_s2r_pages >= ARRAY_SIZE(s2r_pages)) {
            printf("SPTM-virt[s2r]: page table full at %s 0x%lx\n", what, page);
            return;
        }
        s2r_pages[sptm_virt_s2r_pages].page = page;
        s2r_pages[sptm_virt_s2r_pages].what = what;
        sptm_virt_s2r_pages++;
        hv_map_hook(page, s2r_hook, 0x4000);
    }
}

static void s2r_keep_setup(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(s2r_link_paths); i++) {
        u64 regs[S2R_KEEP_MAX];
        int n = pmgr_adt_collect_psregs(s2r_link_paths[i], regs, S2R_KEEP_MAX);
        if (n < 0)
            printf("SPTM-virt[s2r]: cannot list the power domains of %s\n", s2r_link_paths[i]);
        for (int j = 0; j < n; j++) {
            if (s2r_keep_idx(regs[j]) >= 0)
                continue;
            if (s2r_nkeep >= S2R_KEEP_MAX) {
                printf("SPTM-virt[s2r]: too many debug-link domains; 0x%lx not kept\n", regs[j]);
                continue;
            }
            s2r_keep[s2r_nkeep++] = regs[j];
            s2r_hook_range("pmgr-debugpath", regs[j], 4);
        }
    }
    printf("SPTM-virt[s2r]: keeping %d debug-link PMGR domains on %s:", s2r_nkeep,
           s2r_keep_always ? "from boot (proxy on CDC)" : "across sleep");
    for (int i = 0; i < s2r_nkeep; i++)
        printf(" %d=0x%lx", i, s2r_keep[i]);
    printf("\n");
}

/* cdc: the proxy runs over the DWC3 CDC link, whose power domains are kept on. */
void sptm_virt_s2r_setup(bool cdc)
{
    s2r_keep_always = cdc;
    if (cdc)
        s2r_keep_setup();
}
