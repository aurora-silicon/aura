/* SPDX-License-Identifier: MIT */

#ifndef HV_USB_DART_H
#define HV_USB_DART_H

#include "types.h"

/*
 * D4: the identity map the T8152 (J804) carrier gives the usb-drd1/usb-drd3 Gen3 DARTs, so that
 * the guest's xHCI can DMA into exactly the guest RAM window and nowhere else.  Pure: no
 * hardware, no allocation, no globals; host-tested unchanged (tests/e2e/windows_usb_dart_host.c
 * against an independent Python model in tests/python/test_windows_t8152_usb.py).
 *
 * Format, as Linux runs these DARTs on the M6 (TCR 0x9 = TRANSLATE_ENABLE | FOUR_LEVEL, ias 42;
 * windows-m6/D4-USB-DESIGN.md section 4).  Facts taken from, and cited to, our own Linux patches
 * (linux-j804 patches/apple-dart-t8152-gen3-ias.py, -tlbi.py) and the Asahi/upstream GPL-2.0
 * drivers/iommu/io-pgtable-dart.c (APPLE_DART2_* masks, dart_alloc_pgtable) and apple-dart.c
 * (T8110 registers); no code is copied from either:
 *
 *   - 16 KiB tables of 2048 u64 entries; TTBR -> L1 -> L2 -> leaf.  Linux's "4 levels" counts
 *     the TTBR selection as a level.  L1 is indexed by DVA[46:36] (only [41:36] decode at ias
 *     42), L2 by DVA[35:25], the leaf table by DVA[24:14].
 *   - table descriptor: ((next_pa >> 4) & GENMASK(37, 10)) | VALID;
 *   - leaf: SUBPAGE_START [63:52] = 0, SUBPAGE_END [51:40] = 0xfff, ((pa >> 4) & GENMASK(37,
 *     10)), NO_READ (bit 3), NO_WRITE (bit 2) and NO_CACHE (bit 1) clear, VALID.  Bit 1 clear
 *     keeps the DMA coherent.  Example: page 0x10100000000 -> 0x000fff1010000001.
 *   - TTBR: VALID | ((root_pa >> 14) << 2), the T8110 TTBR_ADDR field [29:2].
 *   - TLBI (Gen3): a 128-bit store of {lo, 0} to the dart-tlbi-aperture slot, then
 *     "tlbi vale1os, xzr"; lo = BIT(60) | route << 56 | sid << 48 (aura dart.c
 *     dart_gen3_tlb_invalidate, which the carrier calls; the encoder here is for the tests).
 *
 * The arena is n_tables consecutive 16 KiB tables: [0] the L1 root, then one L2 table per
 * 64 GiB block the window touches, then one leaf table per 32 MiB block, all in DVA order.
 * Every entry for an address outside [start, end) is 0, so everything else (the AHCI store,
 * the framebuffer shadow, carrier memory, the tables themselves, MMIO) faults in the DART.
 */

#define USB_DART_PAGE        0x4000UL
#define USB_DART_PTES        2048u
#define USB_DART_IAS         42
#define USB_DART_MAX_TABLES  640u /* 10 MiB: covers 17.5 GiB (563 tables) with room */
#define USB_DART_L1_SHIFT    36
#define USB_DART_L2_SHIFT    25
#define USB_DART_LEAF_SHIFT  14
#define USB_DART_PTE_VALID   0x1UL
#define USB_DART_PTE_FLAGS   0xeUL                 /* NO_READ | NO_WRITE | NO_CACHE */
#define USB_DART_PTE_ADDR    0x0000003ffffffc00UL  /* GENMASK(37, 10) */
#define USB_DART_PTE_SP_END  0x000fff0000000000UL  /* GENMASK(51, 40) */
#define USB_DART_PTE_SP_STRT 0xfff0000000000000UL  /* GENMASK(63, 52) */

#define USB_DART_TCR_TRANSLATE  0x1u
#define USB_DART_TCR_FOUR_LEVEL 0x8u
#define USB_DART_TCR_ON         (USB_DART_TCR_TRANSLATE | USB_DART_TCR_FOUR_LEVEL) /* 0x9 */
#define USB_DART_TCR_IDLE       USB_DART_TCR_TRANSLATE /* aura's quiesced stream */

enum usb_dart_err {
    USB_DART_OK = 0,
    USB_DART_EINVAL = -1, /* unaligned, empty, above 2^42 */
    USB_DART_ETOOBIG = -2, /* more than USB_DART_MAX_TABLES */
    USB_DART_ESPACE = -3, /* arena too small, or not 16 KiB aligned / below 2^42 */
    USB_DART_EFAULT = -4, /* walk: no valid translation */
};

/* Tables needed for [start, end): 1 + 64 GiB blocks touched + 32 MiB blocks touched. */
int usb_dart_tables_needed(u64 start, u64 end, u32 *n_tables);

/*
 * Fill the arena (n_tables * 16 KiB at CPU pointer arena, physical address arena_pa) with the
 * identity map of [start, end).  The arena is zeroed first.  *root_pa is the L1 table.
 */
int usb_dart_build_identity(u64 start, u64 end, u64 *arena, u64 arena_pa, u32 n_tables,
                            u64 *root_pa);

/*
 * Translate dva through the arena as the DART would; table pointers outside the arena and
 * leaves with any NO_* bit or a partial subpage range count as faults.
 */
int usb_dart_walk(const u64 *arena, u64 arena_pa, u32 n_tables, u64 root_pa, u64 dva,
                  u64 *pa_out);

/*
 * The attach-time spot check: the first and last page, both neighbours of the first 32 MiB
 * boundary inside the window, and (must fault) start - 16 KiB, end and the arena itself.
 * Returns the number of disagreements (0 = good).
 */
int usb_dart_self_check(const u64 *arena, u64 arena_pa, u32 n_tables, u64 root_pa, u64 start,
                        u64 end);

u64 usb_dart_leaf_pte(u64 pa);
u64 usb_dart_table_pte(u64 table_pa);
/* T8110 TTBR word for a root table; USB_DART_EINVAL if unaligned or at/above 2^42. */
int usb_dart_ttbr(u64 root_pa, u32 *ttbr);
/* Low word of the Gen3 TLBI aperture command for one stream (high word 0). */
u64 usb_dart_tlbi_lo(u32 route, u32 sid);

#endif
