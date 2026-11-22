/* SPDX-License-Identifier: GPL-2.0-only */

#ifndef GPT_CHECK_H
#define GPT_CHECK_H

#include "types.h"

/*
 * D5: which GPT partition of the internal NVMe the carrier's AHCI disk may use, and whether
 * that partition holds a disk image the AHCI disk can present.
 *
 * Clean room, written only from UEFI Specification 2.10 section 5 ("GUID Partition Table
 * (GPT) Disk Layout": 5.2.3 protective MBR, 5.3.2 GPT header, 5.3.3 partition entry array) and
 * the public partition type GUIDs listed in gpt_check.c.  Pure logic: no I/O except through
 * the read callback, no printing, no allocation; every refusal returns a reason string.
 *
 * A partition is never chosen by number.  The carrier is given five values from the owner's
 * private configuration (disk GUID, unique partition GUID, type GUID, name, first/last LBA) and
 * accepts exactly one entry that matches all of them, in a table whose primary and backup copies
 * agree, that overlaps nothing, starts 1 MiB aligned and whose type is not one of the well-known
 * types another OS on the machine owns.
 */

#define GPT_NAME_UNITS   36   /* UTF-16 code units in an entry's name (UEFI 2.10 table 5.6) */
#define GPT_MAX_ENTRIES  1024 /* this reader's cap; a 128-entry table is the common case */
#define GPT_MAX_BLOCK    4096
#define GPT_ALIGN_BYTES  (1UL << 20)
#define GPT_INNER_SECTOR 512

struct gpt_want {
    u8 disk_guid[16]; /* on-disk byte order (the mixed-endian EFI_GUID encoding) */
    u8 part_guid[16];
    u8 type_guid[16];
    u16 name[GPT_NAME_UNITS]; /* zero padded */
    u64 first, last;          /* LBAs, inclusive */
};

struct gpt_found {
    u64 first, last;
    u32 index;   /* entry index, for logs only */
    u32 entries; /* non-empty entries in the table */
    u64 first_usable, last_usable, alternate;
};

/* 4 KiB (or `block`-byte) blocks; count is at most 64.  false = I/O error. */
typedef bool (*gpt_read_fn)(void *opaque, u64 lba, u32 count, void *buf);

/*
 * The outer table of a disk with `nsze` blocks of `block` bytes (512..4096).  `scratch` holds
 * the entry array (scratch_bytes >= the array, and a multiple of `block`, at least 2 blocks).
 * 0 with *found filled, or -1 with *why set.
 */
int gpt_check_outer(gpt_read_fn read, void *opaque, u64 nsze, u32 block, u8 *scratch,
                    u64 scratch_bytes, const struct gpt_want *want, struct gpt_found *found,
                    const char **why);

/*
 * The inner disk: the first 4096 bytes of the partition, which the AHCI disk presents as
 * 512-byte sectors 0..7.  Sector 0 must be a protective MBR and sector 1 a valid GPT header with
 * 512-byte LBAs whose disk (AlternateLBA + 1 sectors) fits in `part_bytes`.  *backup_at_end says
 * whether the backup header is at the partition's last sector (false: sgdisk -e not run yet).
 * 0, or -1 with *why.
 */
int gpt_check_inner(const u8 *first4k, u64 part_bytes, bool *backup_at_end, u64 *inner_bytes,
                    const char **why);

/* True if `type` is one of the public type GUIDs the carrier never presents to Windows. */
bool gpt_type_denied(const u8 type[16]);

/* The GPT header CRC-32 (zlib) over `size` bytes with the CRC field taken as zero. */
u32 gpt_header_crc(const u8 *hdr, u32 size);

#endif
