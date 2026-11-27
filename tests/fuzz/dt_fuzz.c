/* SPDX-License-Identifier: MIT */
/*
 * libFuzzer target for the code that takes a payload's device tree and turns it and the ADT into
 * the tree aura hands the kernel: payload.c's load_fdt() and payload_profile_rank(), which accept
 * the tree, src/devicetree.c (ranges and address translation), kboot.c's SMBIOS node and
 * dt_disable_missing_devs() (load_fdt() and the kboot.c code copied out by
 * tests/fuzz/src_copy.py), src/kboot_audio.c, with aura's ADT reader (rust/src/adt.rs) and libfdt.
 *
 * Input: a little-endian u32 ADT length, a flags byte, the ADT, then the DTB. The ADT stands for
 * what iBoot hands over, the DTB for the payload's tree; both are untrusted here. Everything the
 * builders write must leave a tree that still passes fdt_check_full().
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "devicetree.h"
#include "kboot_audio.h"
#include "types.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#undef printf

void *adt;
static u32 adt_size;
static u8 flags;
static volatile u64 sink;

u32 adt_get_size(void)
{
    return adt_size;
}

int fuzz_log(const char *fmt, ...)
{
    static char line[512];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    return n;
}

u64 host_sysreg(const char *name)
{
    /* ID_AA64PFR1_EL1.SME, the only register the copied code reads */
    if (strstr(name, "ID_AA64PFR1"))
        return (flags & 1) ? (1UL << 24) : 0;
    return 0;
}

/* The speaker ID straps: every GPIO register reads as zero and writes are dropped */
u32 read32(u64 addr)
{
    sink += addr;
    return 0;
}

void write32(u64 addr, u32 data)
{
    sink += addr + data;
}

void udelay(u32 us)
{
    (void)us;
}

extern bool fuzz_os_26_6_2;
void *fuzz_load_fdt(void *p, size_t size, void **selected);
int fuzz_set_smbios(void *dt);
int fuzz_disable_missing_devs(void *dt, const char *adt_prefix, const char *dt_prefix, int max_devs,
                              int regnum);

#define check(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #cond);               \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

/* Room for what the builders add, as kboot leaves DT_EDIT_MARGIN */
#define DT_MARGIN (64 * 1024)

/*
 * dt_get_address() walks up from a node with fdt_parent_offset(), which scans the tree from its
 * start, so a pass over every node of a real tree takes seconds. Take the nodes aura asks about,
 * the root's and its buses' children (two levels down), and a bounded number of them.
 */
#define TRANSLATE_DEPTH 2
#define TRANSLATE_NODES 96

static void translate(void *dt)
{
    int depth = 0, count = 0;

    for (int node = fdt_next_node(dt, -1, &depth); node >= 0 && count < TRANSLATE_NODES;
         node = fdt_next_node(dt, node, &depth)) {
        if (depth > TRANSLATE_DEPTH)
            continue;
        count++;
        sink += dt_get_address(dt, node);

        if (!fdt_getprop(dt, node, "ranges", NULL))
            continue;
        struct dt_ranges_tbl ranges[DT_MAX_RANGES] = {0};
        dt_parse_ranges(dt, node, ranges);
        int child;
        fdt_for_each_subnode(child, dt, node)
        {
            u64 addr;
            if (dt_reg_address(dt, node, child, &addr))
                sink += dt_translate_address(ranges, addr);
        }
    }
}

static void stage(void *dt, bool valid, const char *what, int ret)
{
    sink += ret;
    if (valid && fdt_check_full(dt, fdt_totalsize(dt))) {
        fprintf(stderr, "%s left an invalid tree\n", what);
        abort();
    }
}

int LLVMFuzzerTestOneInput(const u8 *data, size_t size)
{
    u32 alen;

    if (size < 5)
        return 0;
    memcpy(&alen, data, 4);
    flags = data[4];
    data += 5;
    size -= 5;
    if (alen > size || alen > SZ_1M || size - alen > SZ_1M)
        return 0;

    adt = malloc(alen ? alen : 1);
    memcpy(adt, data, alen);
    adt_size = alen;

    /*
     * As payload.c does, take a tree that starts with the FDT magic (load_fdt(): with or without
     * the size a payload record gives) and open it into a bigger buffer (kboot_prepare_dt()). A
     * tree load_fdt() accepts but does not select (its compatible is not the Mac's) goes on too.
     * The payload holds as much as the header says, here the rest of the input.
     */
    size_t dlen = size - alen;
    void *fdt = malloc(dlen ? dlen : 1);
    memcpy(fdt, data + alen, dlen);
    void *dt = NULL, *selected;

    if (adt_check_header(adt) || dlen < sizeof(struct fdt_header) || fdt_magic(fdt) != FDT_MAGIC ||
        fdt_totalsize(fdt) > dlen)
        goto out;
    fuzz_os_26_6_2 = flags & 16;
    if (!fuzz_load_fdt(fdt, (flags & 32) ? dlen : 0, &selected))
        goto out;
    sink += selected == fdt;
    size_t bufsize = fdt_totalsize(fdt) + DT_MARGIN;
    dt = malloc(bufsize);
    if (fdt_open_into(fdt, dt, bufsize) < 0)
        goto out;
    bool valid = !fdt_check_full(dt, fdt_totalsize(dt));

    translate(dt);
    stage(dt, valid, "dt_set_smbios", fuzz_set_smbios(dt));
    if (flags & 2)
        stage(dt, valid, "dt_set_smbios (again)", fuzz_set_smbios(dt));
    stage(dt, valid, "kboot_audio_amp_config", kboot_audio_amp_config(dt, adt));
    stage(dt, valid, "kboot_audio_speaker_id", kboot_audio_speaker_id(dt, adt));
    stage(dt, valid, "usb-drd", fuzz_disable_missing_devs(dt, "usb-drd", "usb@", 8, 0));
    stage(dt, valid, "acio", fuzz_disable_missing_devs(dt, "acio", "cio@", 8, (flags >> 2) & 3));
    stage(dt, valid, "i2c", fuzz_disable_missing_devs(dt, "i2c", "i2c@", 16, 0));
    translate(dt);

out:
    free(dt);
    free(fdt);
    free(adt);
    adt = NULL;
    return 0;
}
