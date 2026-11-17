/* SPDX-License-Identifier: MIT */

/*
 * D5 GPT checks.  See gpt_check.h for the scope; written from UEFI 2.10 section 5 only.
 */

#include "gpt_check.h"
#include "ahci_model.h" /* ahci_crc32: the zlib CRC-32 GPT uses (UEFI 2.10 5.3.2) */

/* EFI_GUID in its on-disk encoding: Data1-3 little-endian, Data4 as bytes (UEFI 2.10 A). */
#define GUID(d1, d2, d3, a, b, c, d, e, f, g, h)                                                   \
    {(u8)(d1),                                                                                     \
     (u8)((d1) >> 8),                                                                              \
     (u8)((d1) >> 16),                                                                             \
     (u8)((d1) >> 24),                                                                             \
     (u8)(d2),                                                                                     \
     (u8)((d2) >> 8),                                                                              \
     (u8)(d3),                                                                                     \
     (u8)((d3) >> 8),                                                                              \
     a,                                                                                            \
     b,                                                                                            \
     c,                                                                                            \
     d,                                                                                            \
     e,                                                                                            \
     f,                                                                                            \
     g,                                                                                            \
     h}

/*
 * Public type GUIDs (UEFI 2.10 table 5.7, the Discoverable Partitions Specification, Apple's
 * published APFS/HFS+/boot types, Microsoft's documented basic data and reserved types).  A
 * partition of one of these belongs to firmware, macOS, Linux or a normal Windows install and is
 * never the carrier's disk; the Windows partition carries its own private type GUID.
 */
static const u8 denied_types[][16] = {
    GUID(0xc12a7328, 0xf81f, 0x11d2, 0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b), /* ESP */
    GUID(0x024dee41, 0x33e7, 0x11d3, 0x9d, 0x69, 0x00, 0x08, 0xc7, 0x81, 0xf3, 0x9f), /* MBR */
    GUID(0x7c3457ef, 0x0000, 0x11aa, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac), /* APFS */
    GUID(0x48465300, 0x0000, 0x11aa, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac), /* HFS+ */
    GUID(0x426f6f74, 0x0000, 0x11aa, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac), /* Boot */
    GUID(0x69646961, 0x6700, 0x11aa, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac), /* iSC */
    GUID(0x52637672, 0x7900, 0x11aa, 0xaa, 0x11, 0x00, 0x30, 0x65, 0x43, 0xec, 0xac), /* RecOS */
    GUID(0x0fc63daf, 0x8483, 0x4772, 0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4), /* Linux */
    GUID(0xb921b045, 0x1df0, 0x41c3, 0xaf, 0x44, 0x4c, 0x6f, 0x28, 0x0d, 0x3f, 0xae), /* root64 */
    GUID(0x933ac7e1, 0x2eb4, 0x4f13, 0xb8, 0x44, 0x0e, 0x14, 0xe2, 0xae, 0xf9, 0x15), /* home */
    GUID(0x0657fd6d, 0xa4ab, 0x43c4, 0x84, 0xe5, 0x09, 0x33, 0xc8, 0x4b, 0x4f, 0x4f), /* swap */
    GUID(0xe6d6d379, 0xf507, 0x44c2, 0xa2, 0x3c, 0x23, 0x8f, 0x2a, 0x3d, 0xf9, 0x28), /* LVM */
    GUID(0xa19d880f, 0x05fc, 0x4d3b, 0xa0, 0x06, 0x74, 0x3f, 0x0f, 0x84, 0x91, 0x1e), /* RAID */
    GUID(0xca7d7ccb, 0x63ed, 0x4c53, 0x86, 0x1c, 0x17, 0x42, 0x53, 0x60, 0x59, 0xcc), /* LUKS */
    GUID(0xbc13c2ff, 0x59e6, 0x4262, 0xa3, 0x52, 0xb2, 0x75, 0xfd, 0x6f, 0x71, 0x72), /* XBOOTLDR */
    GUID(0xebd0a0a2, 0xb9e5, 0x4433, 0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7), /* MS data */
    GUID(0xe3c9e316, 0x0b5c, 0x4db8, 0x81, 0x7d, 0xf9, 0x2d, 0xf0, 0x02, 0x15, 0xae), /* MSR */
    GUID(0xde94bba4, 0x06d1, 0x4d40, 0xa1, 0x6a, 0xbf, 0xd5, 0x01, 0x79, 0xd6, 0xac), /* WinRE */
};

#define HDR_SIG          0x00
#define HDR_REVISION     0x08
#define HDR_SIZE         0x0c
#define HDR_CRC          0x10
#define HDR_RESERVED     0x14
#define HDR_MY_LBA       0x18
#define HDR_ALT_LBA      0x20
#define HDR_FIRST_USABLE 0x28
#define HDR_LAST_USABLE  0x30
#define HDR_DISK_GUID    0x38
#define HDR_ENTRY_LBA    0x48
#define HDR_ENTRY_COUNT  0x50
#define HDR_ENTRY_SIZE   0x54
#define HDR_ENTRY_CRC    0x58
#define HDR_MIN_SIZE     92

#define ENT_TYPE  0x00
#define ENT_GUID  0x10
#define ENT_FIRST 0x20
#define ENT_LAST  0x28
#define ENT_NAME  0x38

static u32 rd32(const u8 *p)
{
    return (u32)p[0] | (u32)p[1] << 8 | (u32)p[2] << 16 | (u32)p[3] << 24;
}

static u64 rd64(const u8 *p)
{
    return (u64)rd32(p) | (u64)rd32(p + 4) << 32;
}

static bool same(const u8 *a, const u8 *b, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (a[i] != b[i])
            return false;
    return true;
}

static bool zero(const u8 *a, u32 n)
{
    for (u32 i = 0; i < n; i++)
        if (a[i])
            return false;
    return true;
}

bool gpt_type_denied(const u8 type[16])
{
    for (u32 i = 0; i < sizeof(denied_types) / sizeof(denied_types[0]); i++)
        if (same(type, denied_types[i], 16))
            return true;
    return false;
}

u32 gpt_header_crc(const u8 *hdr, u32 size)
{
    static const u8 zero4[4] = {0, 0, 0, 0};
    u32 crc = ahci_crc32(0, hdr, HDR_CRC);
    crc = ahci_crc32(crc, zero4, 4);
    return ahci_crc32(crc, hdr + HDR_CRC + 4, size - HDR_CRC - 4);
}

struct hdr {
    u64 my, alt, first_usable, last_usable, entry_lba;
    u32 count, esize, entry_crc;
    u8 disk_guid[16];
};

/* UEFI 2.10 5.3.2: the checks a header must pass on its own, at `my` in a `block`-byte disk. */
static const char *header_ok(const u8 *h, u32 block, u64 my, struct hdr *o)
{
    if (!same(h + HDR_SIG, (const u8 *)"EFI PART", 8))
        return "no GPT signature";
    if (rd32(h + HDR_REVISION) != 0x00010000)
        return "GPT revision is not 1.0";
    u32 size = rd32(h + HDR_SIZE);
    if (size < HDR_MIN_SIZE || size > block)
        return "GPT header size out of range";
    if (gpt_header_crc(h, size) != rd32(h + HDR_CRC))
        return "GPT header CRC mismatch";
    if (rd32(h + HDR_RESERVED))
        return "GPT header reserved field is not zero";
    o->my = rd64(h + HDR_MY_LBA);
    o->alt = rd64(h + HDR_ALT_LBA);
    o->first_usable = rd64(h + HDR_FIRST_USABLE);
    o->last_usable = rd64(h + HDR_LAST_USABLE);
    o->entry_lba = rd64(h + HDR_ENTRY_LBA);
    o->count = rd32(h + HDR_ENTRY_COUNT);
    o->esize = rd32(h + HDR_ENTRY_SIZE);
    o->entry_crc = rd32(h + HDR_ENTRY_CRC);
    for (u32 i = 0; i < 16; i++)
        o->disk_guid[i] = h[HDR_DISK_GUID + i];
    if (o->my != my)
        return "GPT header MyLBA is not where it was read";
    if (o->first_usable > o->last_usable)
        return "GPT usable range is empty";
    /* 128 x 2^n bytes (UEFI 2.10 table 5.5), at most one block here. */
    if (o->esize < 128 || o->esize > block || (o->esize & (o->esize - 1)))
        return "GPT entry size is not 128 x 2^n (up to one block)";
    if (!o->count || o->count > GPT_MAX_ENTRIES)
        return "GPT entry count is 0 or above 1024";
    if (zero(o->disk_guid, 16))
        return "GPT disk GUID is zero";
    return 0;
}

static u64 array_blocks(const struct hdr *h, u32 block)
{
    return ((u64)h->count * h->esize + block - 1) / block;
}

/* Read the entry array of `h` into scratch and check its CRC. */
static const char *read_array(gpt_read_fn read, void *opaque, u32 block, const struct hdr *h,
                              u8 *scratch, u64 scratch_bytes)
{
    u64 blocks = array_blocks(h, block);
    if (blocks * block > scratch_bytes)
        return "GPT entry array larger than this reader's buffer";
    for (u64 done = 0; done < blocks;) {
        u32 n = blocks - done > 64 ? 64 : (u32)(blocks - done);
        if (!read(opaque, h->entry_lba + done, n, scratch + done * block))
            return "read error in the GPT entry array";
        done += n;
    }
    if (ahci_crc32(0, scratch, (u64)h->count * h->esize) != h->entry_crc)
        return "GPT entry array CRC mismatch";
    return 0;
}

static bool overlap(u64 a0, u64 a1, u64 b0, u64 b1)
{
    return a0 <= b1 && b0 <= a1;
}

int gpt_check_outer(gpt_read_fn read, void *opaque, u64 nsze, u32 block, u8 *scratch,
                    u64 scratch_bytes, const struct gpt_want *want, struct gpt_found *found,
                    const char **why)
{
    struct hdr p, b;
    const char *err;

#define REFUSE(msg)                                                                                \
    do {                                                                                           \
        *why = (msg);                                                                              \
        return -1;                                                                                 \
    } while (0)

    if (block < 512 || block > GPT_MAX_BLOCK || (block & (block - 1)) || nsze < 6 ||
        scratch_bytes < 2 * (u64)block || scratch_bytes % block)
        REFUSE("bad disk geometry for the GPT reader");

    /* Primary header (LBA 1) and its array. */
    if (!read(opaque, 1, 1, scratch))
        REFUSE("read error at LBA 1");
    if ((err = header_ok(scratch, block, 1, &p)))
        REFUSE(err);
    if (p.alt != nsze - 1)
        REFUSE("primary GPT AlternateLBA is not the last LBA of the namespace");
    if (p.last_usable >= p.alt)
        REFUSE("GPT LastUsableLBA reaches the backup header");
    if (p.entry_lba < 2 || p.entry_lba + array_blocks(&p, block) > p.first_usable)
        REFUSE("primary GPT entry array is not between the header and FirstUsableLBA");

    /* Backup header (last LBA): same table, mirrored. */
    if (!read(opaque, nsze - 1, 1, scratch))
        REFUSE("read error at the backup GPT header");
    if ((err = header_ok(scratch, block, nsze - 1, &b)))
        REFUSE("backup GPT header invalid (signature, CRC, MyLBA or fields)");
    if (b.alt != 1 || !same(b.disk_guid, p.disk_guid, 16) || b.first_usable != p.first_usable ||
        b.last_usable != p.last_usable || b.count != p.count || b.esize != p.esize ||
        b.entry_crc != p.entry_crc)
        REFUSE("backup GPT header disagrees with the primary (someone is editing the disk?)");
    if (b.entry_lba <= b.last_usable || b.entry_lba + array_blocks(&b, block) > nsze - 1)
        REFUSE("backup GPT entry array is not between LastUsableLBA and the backup header");
    if ((err = read_array(read, opaque, block, &b, scratch, scratch_bytes)))
        REFUSE("backup GPT entry array invalid");
    if ((err = read_array(read, opaque, block, &p, scratch, scratch_bytes)))
        REFUSE(err);

    if (!same(p.disk_guid, want->disk_guid, 16))
        REFUSE("GPT disk GUID is not the configured one (another disk or machine?)");
    if (gpt_type_denied(want->type_guid))
        REFUSE("the configured type GUID is a well-known type another OS owns");

    /* The entries: exactly one match on the unique GUID, then everything else about it. */
    s64 idx = -1;
    u32 used = 0;
    for (u32 i = 0; i < p.count; i++) {
        const u8 *e = scratch + (u64)i * p.esize;
        if (zero(e + ENT_TYPE, 16))
            continue;
        used++;
        u64 f = rd64(e + ENT_FIRST), l = rd64(e + ENT_LAST);
        if (f > l || f < p.first_usable || l > p.last_usable)
            REFUSE("a GPT entry lies outside the usable range or ends before it starts");
        for (u32 j = 0; j < i; j++) {
            const u8 *o = scratch + (u64)j * p.esize;
            if (zero(o + ENT_TYPE, 16))
                continue;
            if (overlap(f, l, rd64(o + ENT_FIRST), rd64(o + ENT_LAST)))
                REFUSE("two GPT entries overlap");
            if (same(e + ENT_GUID, o + ENT_GUID, 16))
                REFUSE("two GPT entries share a unique partition GUID");
        }
        if (same(e + ENT_GUID, want->part_guid, 16))
            idx = i;
    }
    if (idx < 0)
        REFUSE("no GPT entry has the configured unique partition GUID");
    const u8 *e = scratch + (u64)idx * p.esize;
    if (!same(e + ENT_TYPE, want->type_guid, 16))
        REFUSE("the partition's type GUID is not the configured one");
    for (u32 k = 0; k < GPT_NAME_UNITS; k++)
        if ((u16)(e[ENT_NAME + 2 * k] | e[ENT_NAME + 2 * k + 1] << 8) != want->name[k])
            REFUSE("the partition's name is not the configured one");
    for (u32 i = 0; i < p.count; i++) {
        const u8 *o = scratch + (u64)i * p.esize;
        if (i == idx || zero(o + ENT_TYPE, 16))
            continue;
        if (same(o + ENT_NAME, e + ENT_NAME, GPT_NAME_UNITS * 2))
            REFUSE("another GPT entry has the same name");
    }
    u64 f = rd64(e + ENT_FIRST), l = rd64(e + ENT_LAST);
    if (f != want->first || l != want->last)
        REFUSE("the partition's first/last LBA are not the configured ones (moved?)");
    if ((f * block) % GPT_ALIGN_BYTES)
        REFUSE("the partition does not start on a 1 MiB boundary");
    if (gpt_type_denied(e + ENT_TYPE))
        REFUSE("the partition's type is a well-known type another OS owns");

    found->first = f;
    found->last = l;
    found->index = (u32)idx;
    found->entries = used;
    found->first_usable = p.first_usable;
    found->last_usable = p.last_usable;
    found->alternate = p.alt;
    return 0;
#undef REFUSE
}

int gpt_check_inner(const u8 *d, u64 part_bytes, bool *backup_at_end, u64 *inner_bytes,
                    const char **why)
{
    struct hdr h;
    const char *err;

    if (d[510] != 0x55 || d[511] != 0xaa) {
        *why = "inner disk: no MBR signature in sector 0 (not a disk image)";
        return -1;
    }
    bool protective = false;
    for (u32 i = 0; i < 4; i++)
        if (d[446 + 16 * i + 4] == 0xee)
            protective = true;
    if (!protective) {
        *why = "inner disk: sector 0 is not a protective MBR (type 0xEE)";
        return -1;
    }
    if ((err = header_ok(d + GPT_INNER_SECTOR, GPT_INNER_SECTOR, 1, &h))) {
        *why = "inner disk: sector 1 is not a valid 512-byte-sector GPT header";
        return -1;
    }
    u64 sectors = part_bytes / GPT_INNER_SECTOR;
    if (h.alt < 2 || h.last_usable >= h.alt || h.alt >= sectors) {
        *why = "inner disk: its GPT describes a disk larger than the partition";
        return -1;
    }
    *backup_at_end = h.alt == sectors - 1;
    *inner_bytes = (h.alt + 1) * GPT_INNER_SECTOR;
    return 0;
}
