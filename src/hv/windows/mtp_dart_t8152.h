/* SPDX-License-Identifier: MIT */

#ifndef MTP_DART_T8152_H
#define MTP_DART_T8152_H

#include "types.h"

/*
 * I1: static translations for the T8152 MTP in dart-sbr stream 4, pure over a memory accessor.
 *
 * iBoot configures and locks the stream (TCR 9, a root table at TTBR) and leaves the root
 * empty; the stream is shared with live coprocessors.  This writer adds only its own entries
 * under that root, the way our Linux driver measured to work on every boot
 * (linux-j804/patches/t8152-mtp-hid.py, windows-m6/INPUT-I1I2-BUILD.md section 1):
 *
 *   3 levels, 16 KiB tables of 2048 entries, indices (dva >> 36), (dva >> 25), (dva >> 14),
 *   each & 0x7ff;
 *   a table entry: (pa >> 14) in bits 37:10, valid (bit 0);
 *   a leaf: the same plus SP_END (bits 51:40) = 0xfff.
 *
 * It never touches a DART register (src/dart.c only knows 2-level Gen3 streams, and its init
 * writes registers).  It refuses to follow a valid entry to a table it did not create, and to
 * overwrite a valid leaf.  Each mapping takes the next DVAs, then leaves one 16 KiB page
 * unmapped as a guard.  No TLB invalidation: only entries that were invalid are written, before
 * the MTP is told the addresses (the Linux driver does none either).
 */

#define MTP_DART_PAGE       0x4000ull
#define MTP_DART_ENTRIES    2048u
#define MTP_DART_MAX_TABLES 16u
#define MTP_DART_VALID      1ull
#define MTP_DART_OFFSET     0x3ffffffc00ull     /* bits 37:10 */
#define MTP_DART_SP_END     0xfff0000000000ull /* bits 51:40 */
#define MTP_DART_PA_MAX     (1ull << 42)        /* 28 bits of 16 KiB pages */

struct mtp_dart_mem {
    void *ctx;
    u64 (*read64)(void *ctx, u64 pa);
    void (*write64)(void *ctx, u64 pa, u64 value);
};

struct mtp_dart {
    struct mtp_dart_mem mem;
    u64 root;
    u64 next_dva, dva_end;
    u64 pool[MTP_DART_MAX_TABLES]; /* zeroed 16 KiB pages the caller provides */
    u32 pool_count, used;
    u32 writes;
};

enum mtp_dart_result {
    MTP_DART_OK = 0,
    MTP_DART_E_ARGS = -1,    /* unaligned, empty, past the DVA window, or a PA out of range */
    MTP_DART_E_TABLES = -2,  /* more tables needed than were provided */
    MTP_DART_E_FOREIGN = -3, /* a valid entry leads to a table this writer did not create */
    MTP_DART_E_MAPPED = -4,  /* a leaf is already valid */
};

/*
 * `root` is the stream's root table; [dva_base, dva_end) the DVAs this writer may hand out;
 * `pool` holds `count` (<= 16) zeroed, 16 KiB aligned table pages.
 */
void mtp_dart_init(struct mtp_dart *d, const struct mtp_dart_mem *mem, u64 root, u64 dva_base,
                   u64 dva_end, const u64 *pool, u32 count);

/*
 * Map [pa, pa + size) at the next free DVAs (size rounded up to 16 KiB).  All checks are made
 * before the first write, so a refusal writes nothing.  *dva gets the first DVA.
 */
int mtp_dart_map(struct mtp_dart *d, u64 pa, u64 size, u64 *dva);

/* Walk this writer's own tables: the PA behind `dva`, or false. */
bool mtp_dart_lookup(const struct mtp_dart *d, u64 dva, u64 *pa);

u64 mtp_dart_table_pte(u64 pa);
u64 mtp_dart_leaf_pte(u64 pa);

#endif
