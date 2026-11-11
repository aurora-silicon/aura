/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "adt.h"
#include "dapf.h"
#include "dart.h"
#include "firmware.h"
#include "isp.h"
#include "pmgr.h"
#include "soc.h"
#include "utils.h"
#include "xnuboot.h"

#define ISP_ASC_VERSION 0x1800000

#define ISP_VER_T8103 0xb0090
#define ISP_VER_T6000 0xb3091
#define ISP_VER_T8112 0xc1090
#define ISP_VER_T6020 0xc3091
#define ISP_VER_T8122 0xf1001
#define ISP_VER_T603X 0xf3001
#define ISP_VER_T8140 0x110000

// Global PMGR PS slots from the T8140 ADT. These are virtual device entries,
// so the generic PMGR recursion enables their real parents but skips these slots.
static const struct {
    const char *name;
    u32 offset;
} isp_t8140_domains[] = {
    {"ISP_CPU", 0x4000},
    {"ISP_CPU_CORE0", 0x4008},
    {"ISP_CPU_CORE1", 0x4010},
    {"ISP_FE", 0x4018},
};

// Candidate heap limit; requires hardware validation.
#define ISP_T8140_HEAP_TOP 0x2200000
#define ISP_T8140_SEG_END  0x21ec000

/*
 * T8142 (M5, J813) has the Neo's ISP17a: the same global PMGR slot offsets
 * and DART layout at another base. Its ADT segment-ranges end at 0x16ec000;
 * the heap top is the next 2 MiB boundary, as the T8140 one is. Candidate,
 * like T8140's.
 */
#define ISP_T8142_HEAP_TOP 0x1800000
#define ISP_T8142_SEG_END  0x16ec000

#define PMGR_PS_TARGET GENMASK(3, 0)

// PMGR offset to enable to get the version info to work
#define ISP_PMGR_T8103 0x4018
#define ISP_PMGR_T6000 0x8
#define ISP_PMGR_T6020 0x4008
#define ISP_PMGR_T6031 0x4030

static bool isp_initialized = false;
static u64 heap_phys, heap_iova, heap_size;

int isp_get_heap(u64 *phys, u64 *iova, u64 *size)
{
    if (!isp_initialized)
        return -1;

    *phys = heap_phys;
    *iova = heap_iova | isp_iova_base();
    *size = heap_size;
    return 0;
}

u64 isp_iova_base(void)
{
    switch (chip_id) {
        case 0x6020 ... 0x6fff:
        case 0x8122:
            return 0x10000000000;
        default:
            return 0;
    }
}

static int isp_firmware_end(const struct adt_segment_ranges *segments, u32 length, u64 *end)
{
    if (!segments || length < 2 * sizeof(*segments) || length % sizeof(*segments))
        return -1;
    *end = 0;
    for (u32 i = 0; i < length / sizeof(*segments); i++) {
        if (!segments[i].size || segments[i].iova > UINT64_MAX - segments[i].size)
            return -1;
        u64 next = segments[i].iova + segments[i].size;
        if (next > *end)
            *end = next;
    }
    return 0;
}

int isp_init(void)
{
    /* The T6050 kernel owns power-up. Its handoff only reserves memory. */
    if (chip_id == T6050)
        return 0;

    int err = -1;
    /* T8142 takes the T8140 (ISP17a) path throughout. */
    bool t8142 = chip_id == T8142;
    bool t8140 = chip_id == T8140 || t8142;
    bool powered = false;
    unsigned int local_powered = 0;
    unsigned int global_attempted = 0;
    u8 global_actual[ARRAY_SIZE(isp_t8140_domains)] = {0};
    u8 global_target[ARRAY_SIZE(isp_t8140_domains)] = {0};
    struct pmgr_saved_modes adt_modes = {0};
    u64 global_base = 0;
    u32 ver_rev = 0;
    u64 selected_top = 0;
    u64 selected_iova = 0;
    u64 selected_size = 0;
    u64 selected_phys = 0;
    const char *reason = "initialization failed";

    const char *isp_path = "/arm-io/isp";
    const char *dart_path = "/arm-io/dart-isp";

    int adt_path[8], adt_isp_path[8];
    int isp_node = adt_path_offset_trace(adt, isp_path, adt_isp_path);
    int node = adt_path_offset_trace(adt, dart_path, adt_path);
    if (node < 0 || isp_node < 0) {
        isp_path = "/arm-io/isp0";
        dart_path = "/arm-io/dart-isp0";
        isp_node = adt_path_offset_trace(adt, isp_path, adt_isp_path);
        node = adt_path_offset_trace(adt, dart_path, adt_path);
    }
    isp_initialized = false;
    if (node < 0 || isp_node < 0)
        return 0;

    if (!t8140) {
        u32 segments_len = 0;
        const struct adt_segment_ranges *segments =
            adt_getprop(adt, isp_node, "segment-ranges", &segments_len);
        u64 firmware_end;
        if (isp_firmware_end(segments, segments_len, &firmware_end)) {
            printf("isp: invalid firmware segment list\n");
            return -1;
        }
    }

    powered = !t8140;
    reason = "ISP ADT power gates failed";
    if ((t8140 ? pmgr_adt_power_enable_traced_saved(isp_path, &adt_modes)
               : pmgr_adt_power_enable(isp_path)) < 0)
        goto out;
    powered = true;

    u64 isp_base, isp_size;
    u64 pmgr_base = 0;
    reason = "invalid ISP aperture";
    if (adt_get_reg(adt, adt_isp_path, "reg", 0, &isp_base, &isp_size) ||
        isp_size < ISP_ASC_VERSION + sizeof(u32) || isp_base > UINT64_MAX - isp_size)
        goto out;

    if (t8140) {
        u64 global_size, local_base, local_size;
        reason = "invalid T8140 PMGR apertures";
        err = -1;
        if (adt_get_reg(adt, adt_isp_path, "reg", 1, &global_base, &global_size) ||
            adt_get_reg(adt, adt_isp_path, "reg", 2, &local_base, &local_size) ||
            global_size < 0x18000 || local_size != 0x4000 ||
            global_base > UINT64_MAX - global_size || local_base > UINT64_MAX - local_size ||
            local_base < global_base + global_size)
            goto out;
        printf("isp: global PMGR 0x%lx..0x%lx; separate local PMGR 0x%lx..0x%lx\n", global_base,
               global_base + global_size, local_base, local_base + local_size);

        // The ADT's ISP_CPU, CORE0, CORE1 and FE slots belong to the global PMGR.
        // Start with CPU: the old sequence started at CORE0 and timed out there.
        // The separate local aperture has no ADT-described PS slot for these devices.
        for (unsigned int i = 0; i < ARRAY_SIZE(isp_t8140_domains); i++) {
            reason = "global ISP PMGR power failed";
            global_attempted++;
            uintptr_t addr = global_base + isp_t8140_domains[i].offset;
            u32 original = read32(addr);
            global_actual[i] = (original >> 4) & 0xf;
            global_target[i] = original & 0xf;
            int ret = pmgr_set_mode(addr, PMGR_PS_ACTIVE);
            printf("isp: %s power on at 0x%lx: 0x%x%s\n", isp_t8140_domains[i].name, addr,
                   read32(addr), ret ? " failed" : "");
            /*
             * J813: the ISP_CPU_CORE1 slot ignores writes, its target reads
             * back 0 whatever is written, also from the OS. Boot without it.
             */
            if (ret && t8142 && isp_t8140_domains[i].offset == 0x4010 &&
                !(read32(addr) & PMGR_PS_TARGET)) {
                printf("isp: %s ignores power requests on T8142, skipping it\n",
                       isp_t8140_domains[i].name);
                continue;
            }
            if (ret)
                goto out;
        }
    } else {
        err = adt_get_reg(adt, adt_isp_path, "reg", 1, &pmgr_base, NULL);
        if (err)
            goto out;
    }
    err = -1;

    u32 pmgr_off;
    switch (chip_id) {
        case T8103:
        case T8112:
        case T8122:
            pmgr_off = ISP_PMGR_T8103;
            break;
        case T6000 ... T6002:
            pmgr_off = ISP_PMGR_T6000;
            break;
        case T6020 ... T6022:
        case T6030:
            pmgr_off = ISP_PMGR_T6020;
            break;
        case T6031 ... T6034:
            pmgr_off = ISP_PMGR_T6031;
            break;
        case T8140:
        case T8142:
            pmgr_off = 0;
            break;
        default:
            reason = "unsupported SoC";
            goto out;
    }

    if (!t8140) {
        reason = "local PMGR power failed";
        local_powered = 1;
        if (pmgr_set_mode(pmgr_base + pmgr_off, PMGR_PS_ACTIVE))
            goto out;
    }

    ver_rev = read32(isp_base + ISP_ASC_VERSION);
    printf("isp: Version 0x%x\n", ver_rev);
    if (t8140 && !t8142 && ver_rev != ISP_VER_T8140) {
        reason = "unexpected T8140 revision";
        goto out;
    }

    if (t8142) {
        /* The J813 revision is not known yet; it is printed above. */
        /*
         * 27.0's ISP firmware moves the __TEXT/__DATA split but still ends at ISP_T8142_SEG_END
         * (J813 ADTs of both), which the segment check below still verifies.
         */
        if (os_firmware.version != V26_6 && os_firmware.version != V26_6_2 &&
            os_firmware.version != V27_0) {
            reason = "unsupported T8142 firmware";
            goto out;
        }
        selected_top = ISP_T8142_HEAP_TOP;
        goto heap;
    }

    /* TODO: confirm versions */
    switch (ver_rev) {
        case ISP_VER_T8103:
        case ISP_VER_T8112:
            switch (os_firmware.version) {
                case V12_3 ... V12_4:
                    selected_top = 0x1800000;
                    break;
                case V13_5:
                    selected_top = 0x1000000;
                    break;
                default:
                    reason = "unsupported firmware";
                    goto out;
            }
            break;
        case ISP_VER_T6000:
            switch (os_firmware.version) {
                case V12_3:
                    selected_top = 0xe00000;
                    break;
                case V13_5:
                case V13_6_1:
                    selected_top = 0xf00000;
                    break;
                default:
                    reason = "unsupported firmware";
                    goto out;
            }
            break;
        case ISP_VER_T6020:
            switch (os_firmware.version) {
                case V13_5:
                case V13_6_1:
                    selected_top = 0xf00000;
                    break;
                default:
                    reason = "unsupported firmware";
                    goto out;
            }
            break;
        case ISP_VER_T603X:
        case ISP_VER_T8122:
            switch (os_firmware.version) {
                case V14_7:
                    selected_top = 0x1000000;
                    break;
                case V26_6_2:
                    /* Only T8122 has a qualified heap on this firmware. */
                    if (ver_rev != ISP_VER_T8122) {
                        reason = "unsupported firmware";
                        goto out;
                    }
                    selected_top = 0x1000000;
                    break;
                default:
                    reason = "unsupported firmware";
                    goto out;
            }
            break;
        case ISP_VER_T8140:
            if (!t8140 || os_firmware.version != V26_6_2) {
                reason = "unsupported T8140 firmware or SoC";
                goto out;
            }
            selected_top = ISP_T8140_HEAP_TOP;
            break;
        default:
            reason = "unknown revision";
            goto out;
    }

heap:;
    const struct adt_segment_ranges *seg;
    u32 segments_len = 0;

    seg = adt_getprop(adt, isp_node, "segment-ranges", &segments_len);
    reason = "invalid segment-ranges length";
    if (!seg || !segments_len || segments_len % sizeof(*seg))
        goto out;
    unsigned int count = segments_len / sizeof(*seg);

    reason = "segment end overflow";
    u64 end = 0;
    for (unsigned int i = 0; i < count; i++) {
        if (seg[i].iova > UINT64_MAX - seg[i].size)
            goto out;
        if (seg[i].iova + seg[i].size > end)
            end = seg[i].iova + seg[i].size;
    }
    reason = "segment alignment overflow";
    if (end > UINT64_MAX - (SZ_16K - 1))
        goto out;
    selected_iova = ALIGN_UP(end, SZ_16K);
    reason = "unmeasured T8140 segment end";
    if (t8140 && selected_iova != (t8142 ? ISP_T8142_SEG_END : ISP_T8140_SEG_END))
        goto out;
    reason = "heap top at or below segment end";
    if (selected_iova >= selected_top)
        goto out;
    selected_size = selected_top - selected_iova;
    if (t8140) {
        reason = "ISP DAPF initialization failed";
        if (dapf_init(dart_path, 5) < 0)
            goto out;
    }
    reason = "heap allocation failed";
    if (selected_size > SIZE_MAX - (SZ_16K - 1) || cur_boot_args.mem_size <= SZ_16K ||
        ALIGN_UP(selected_size, SZ_16K) > cur_boot_args.mem_size - SZ_16K)
        goto out;
    selected_phys = top_of_memory_alloc(selected_size);
    if (!selected_phys)
        goto out;

    printf("isp: Code: 0x%lx..0x%lx (0x%x @ 0x%lx)\n", seg[0].iova, seg[0].iova + seg[0].size,
           seg[0].size, seg[0].phys);
    if (count > 1)
        printf("isp: Data: 0x%lx..0x%lx (0x%x @ 0x%lx)\n", seg[1].iova, seg[1].iova + seg[1].size,
               seg[1].size, seg[1].phys);
    printf("isp: Heap: 0x%lx..0x%lx (0x%lx @ 0x%lx)\n", selected_iova, selected_top, selected_size,
           selected_phys);

    heap_iova = selected_iova;
    heap_size = selected_size;
    heap_phys = selected_phys;
    isp_initialized = true;
    if (t8140)
        return 0; // Resident H17 ISP stays powered for the Linux handoff.
    err = 0;

out:
    if (err)
        printf("isp: disabled: revision 0x%x firmware %s aligned end 0x%lx: %s\n", ver_rev,
               os_firmware.string ? os_firmware.string : "unknown", selected_iova, reason);
    while (global_attempted) {
        unsigned int i = --global_attempted;
        uintptr_t addr = global_base + isp_t8140_domains[i].offset;
        int ret = pmgr_set_mode(addr, global_target[i]);
        printf("isp: %s restore at 0x%lx from %x/%x: 0x%x%s\n", isp_t8140_domains[i].name, addr,
               global_actual[i], global_target[i], read32(addr), ret ? " failed" : "");
    }
    if (local_powered)
        pmgr_set_mode(pmgr_base + pmgr_off, PMGR_PS_PWRGATE);
    if (powered)
        t8140 ? pmgr_restore_modes(&adt_modes) : pmgr_adt_power_disable(isp_path);
    return err;
}
