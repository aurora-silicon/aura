/* SPDX-License-Identifier: MIT */
/*
 * Stand-ins shared by the hv_gpu.c host drivers (windows_gpu_glue_host.c for G0,
 * windows_gpu_probe_host.c for G1): the stage-2 policy and HV map calls, which record what the
 * glue asks for; the synthetic TEXT where the ADT says iBoot left it; and, for G1, a model of the
 * GPU PSD blocks and of the forwarded registers that records every hardware access.  Nothing in
 * here is a real address or value: the tests pass synthetic ones.
 */
#ifndef WINDOWS_GPU_GLUE_STUBS_H
#define WINDOWS_GPU_GLUE_STUBS_H

#include <assert.h>
#include <inttypes.h>

#include "windows_gpu_adt_standin.h"

#include "gpu_t8152_admit.h"
#include "utils.h"
#include "hv.h"
#include "hv_gpu.h"
#include "xnuboot.h"

#include "glue_profile.inc"
#include "windows_gpu_sha256.h"

u32 board_id = 0x22, chip_id = 0x8152;
struct boot_args cur_boot_args;

const struct gpu_profile *gpu_t8152_j804_profiles(void)
{
    return profiles_j804;
}

const struct gpu_tlv *gpu_t8152_tlvs(u32 *count)
{
    *count = ARRAY_SIZE(tlvs);
    return tlvs;
}

void aura_sha256(const void *data, size_t size, u8 digest[32])
{
    host_sha256(data, size, digest);
}

/* ---- stand-ins that record ------------------------------------------------------------------ */
static bool armed = true, ram_set = true, entered;
static u64 ram_base, ram_top;
static int fw_allow_calls, fw_allow_fail_at = -1, deny_calls, allow_calls, map_ro_calls;
static int hook_calls, unmap_calls, clean_calls, rollback_calls;
static bool allow_fail, map_fail;
static unsigned int live_exceptions; /* the stage-2 allow table as hv_t8152.c would hold it */
static void *clean_addr;
static size_t clean_len;
static struct {
    u64 pa, size;
    bool ro;
    u64 lo, hi;
} fw[16];
static struct {
    u64 from, size;
    hv_hook_t *fn;
} hooks[4];
static struct {
    u64 from, size;
} denies[4];
static u64 allow_ipa, allow_pa, allow_size, map_from, map_to, map_size;
static bool allow_ro;

bool hv_t8152_policy_armed(void)
{
    return armed;
}

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    if (!ram_set)
        return false;
    *base = ram_base;
    *size = ram_top - ram_base;
    return true;
}

int hv_t8152_gpu_fw_allow(u64 pa, u64 size, bool read_only, u64 dram_lo, u64 dram_hi,
                          const char *name)
{
    (void)name;
    if (fw_allow_calls == fw_allow_fail_at) {
        fw_allow_calls++;
        return -1;
    }
    fw[fw_allow_calls].pa = pa;
    fw[fw_allow_calls].size = size;
    fw[fw_allow_calls].ro = read_only;
    fw[fw_allow_calls].lo = dram_lo;
    fw[fw_allow_calls].hi = dram_hi;
    fw_allow_calls++;
    live_exceptions++;
    return 0;
}

int hv_t8152_gpu_deny(u64 start, u64 end, const char *name)
{
    (void)name;
    denies[deny_calls].from = start;
    denies[deny_calls].size = end - start;
    deny_calls++;
    return 0;
}

int hv_t8152_allow(u64 ipa, u64 pa, u64 size, bool read_only, const char *name)
{
    (void)name;
    allow_calls++;
    if (allow_fail)
        return -1;
    live_exceptions++;
    allow_ipa = ipa;
    allow_pa = pa;
    allow_size = size;
    allow_ro = read_only;
    return 0;
}

unsigned int hv_t8152_allow_mark(void)
{
    return live_exceptions;
}

void hv_t8152_allow_rollback(unsigned int mark, const char *why)
{
    assert(why && mark <= live_exceptions);
    rollback_calls++;
    live_exceptions = mark;
}

static struct {
    u64 from, size;
} unmaps[32];

int hv_unmap(u64 from, u64 size)
{
    if (unmap_calls < 32) {
        unmaps[unmap_calls].from = from;
        unmaps[unmap_calls].size = size;
    }
    unmap_calls++;
    return 0;
}

void dc_civac_range(void *addr, size_t length)
{
    clean_calls++;
    clean_addr = addr;
    clean_len = length;
}

/* G1 firmware-region maps (RW and RO) after the handoff page's RO map. */
static struct {
    u64 from, to, size;
    bool ro;
} fwmaps[32];
static int fwmap_calls, fwmap_fail_at = -1;

static int fwmap(u64 from, u64 to, u64 size, bool ro)
{
    if (fwmap_calls == fwmap_fail_at) {
        fwmap_calls++;
        return -1;
    }
    if (fwmap_calls < 32)
        fwmaps[fwmap_calls].from = from, fwmaps[fwmap_calls].to = to,
        fwmaps[fwmap_calls].size = size, fwmaps[fwmap_calls].ro = ro;
    fwmap_calls++;
    return 0;
}

int hv_map_hw_ro(u64 from, u64 to, u64 size)
{
    if (from != (u64)hv_gpu_host_handoff())
        return fwmap(from, to, size, true); /* G1: a DATA region */
    map_ro_calls++;
    map_from = from;
    map_to = to;
    map_size = size;
    return map_fail ? -1 : 0;
}

int hv_map_hw(u64 from, u64 to, u64 size)
{
    return fwmap(from, to, size, false);
}

static int reserve_calls;
static u64 reserve_start, reserve_end;

int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name)
{
    assert(name);
    reserve_calls++;
    reserve_start = start;
    reserve_end = end;
    return 0;
}

int hv_map_hook(u64 from, hv_hook_t *hook, u64 size)
{
    assert(hook && hook_calls < 4);
    hooks[hook_calls].from = from;
    hooks[hook_calls].size = size;
    hooks[hook_calls].fn = hook;
    hook_calls++;
    return 0;
}

bool hv_windows_guest_entered(void)
{
    return entered;
}

/* Every 16 KiB page handed out stays readable for the rest of the run: a carrier reload keeps
 * the old heap, so a boot marker page from an earlier "session" can still be read. */
static struct {
    void *p;
    size_t size;
} pages[64];
static int npages;

void *memalign(size_t align, size_t size)
{
    void *p = aligned_alloc(align, size);
    if (p && npages < 64) {
        pages[npages].p = p;
        pages[npages].size = size;
        npages++;
    }
    return p;
}

bool hv_gpu_host_marker_read(u64 pa, void *out, u64 size)
{
    for (int i = 0; i < npages; i++)
        if ((u64)pages[i].p == pa && size <= pages[i].size) {
            memcpy(out, pages[i].p, size);
            return true;
        }
    return false;
}

/* ---- synthetic TEXT where the ADT says iBoot left it ---------------------------------------- */
static struct {
    u64 phys, size;
    u8 *data;
} texts[2];

bool hv_gpu_host_read_text(u64 phys, void *buf, u64 size)
{
    for (int i = 0; i < 2; i++)
        if (phys == texts[i].phys && size <= texts[i].size) {
            memcpy(buf, texts[i].data, size);
            return true;
        }
    return false;
}

static u8 *slurp(const char *path, u64 *size)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    *size = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(*size);
    assert(fread(buf, 1, *size, f) == *size);
    fclose(f);
    return buf;
}

static s64 op(u64 o, u64 a1, u64 a2)
{
    return (s64)hv_gpu_proxy(o, a1, a2);
}

static void handoff_ok(u32 want_state)
{
    const u8 *page = hv_gpu_host_handoff();
    u32 magic, total, crc, st;
    assert(page);
    memcpy(&magic, page, 4);
    memcpy(&total, page + GPU_HO_TOTAL, 4);
    memcpy(&crc, page + GPU_HO_CRC, 4);
    memcpy(&st, page + GPU_HO_STATE, 4);
    assert(magic == GPU_HANDOFF_MAGIC && total > GPU_HANDOFF_HEADER && total <= GPU_HANDOFF_SIZE);
    u8 *copy = malloc(total);
    memcpy(copy, page, total);
    memset(copy + GPU_HO_CRC, 0, 4);
    assert(gpu_t8152_crc32(copy, total) == crc);
    free(copy);
    assert(st == want_state);
}

static void reset_stubs(void)
{
    hv_gpu_host_new_session();
    armed = ram_set = true;
    entered = false;
    chip_id = 0x8152;
    board_id = 0x22;
    fw_allow_calls = deny_calls = allow_calls = map_ro_calls = hook_calls = 0;
    unmap_calls = clean_calls = rollback_calls = 0;
    fwmap_calls = reserve_calls = 0;
    fwmap_fail_at = -1;
    fw_allow_fail_at = -1;
    allow_fail = map_fail = false;
    live_exceptions = 0;
    clean_addr = NULL;
    clean_len = 0;
}


#endif
