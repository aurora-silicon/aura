/* SPDX-License-Identifier: MIT */
/* Based on public aurora-silicon/m1n1 src/mtp_handoff.c at
 * 69534d4899e20108d2c7a085565a7097509fa17f. J813-only, explicit and one-shot.
 * Windows owns DockChannel; Aura retains only the RTKit mailbox and DART.
 */
#include "mtp_handoff.h"
#include "adt.h"
#include "asc.h"
#include "dapf.h"
#include "dart.h"
#include "exception.h"
#include "heapblock.h"
#include "hv.h"
#include "iova.h"
#include "memory.h"
#include "pmgr.h"
#include "rtkit.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#define MTP_PATH         "/arm-io/mtp"
#define DART_PATH        "/arm-io/dart-mtp"
#define DOCK_PATH        "/arm-io/dockchannel-mtp"
#define MTP_STAGE_PA     0x10020000000ULL
#define MTP_STAGE_DVA    0x1800000ULL
#define MTP_STAGE_SIZE   0x100000ULL
#define MTP_RESERVE_SIZE 0x200000ULL
#define MTP_SRAM         0x394c00000ULL
#define MTP_SRAM_SIZE    0x100000ULL
#define MTP_DATA         0x394b34000ULL
#define MTP_TIMEOUT      (3 * USEC_PER_SEC)
#define DART_PROTECT     0x200
#define DART_ENABLE      0xc00
#define DART_DISABLE     0xc20
#define DART_TCR         0x1000
#define DART_TTBR        0x1400
#define DART_TLBI        0x80

static struct {
    asc_dev_t *asc;
    dart_dev_t *dart;
    iova_domain_t *iovad;
    rtkit_dev_t *rtkit;
    u64 dart_base;
    bool attempted, owned, ready, failed, stopped;
    u32 busy;
    u64 next_poll;
} mtp;

static bool prop32(int node, const char *name, u32 *value)
{
    u32 size;
    const void *p = adt_getprop(adt, node, name, &size);
    if (!p || size != sizeof(*value))
        return false;
    memcpy(value, p, sizeof(*value));
    return true;
}

static bool reg(const char *path, int index, u64 *base, u64 *size)
{
    int nodes[8];
    return adt_path_offset_trace(adt, path, nodes) >= 0 &&
           adt_get_reg(adt, nodes, "reg", index, base, size) == 0 && *size &&
           *base <= UINT64_MAX - *size;
}

static bool overlaps(u64 base, u64 size, u64 other, u64 length)
{
    return size && length && base < other + length && other < base + size;
}

static bool memory_valid(void)
{
    u64 end = MTP_STAGE_PA + MTP_RESERVE_SIZE;
    if (cur_boot_args.mem_size < MTP_RESERVE_SIZE ||
        cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size ||
        MTP_STAGE_PA < cur_boot_args.phys_base ||
        end > cur_boot_args.phys_base + cur_boot_args.mem_size ||
        heapblock_high_water() > MTP_STAGE_PA)
        return false;
    u64 adt_pa =
        (uintptr_t)cur_boot_args.devtree - cur_boot_args.virt_base + cur_boot_args.phys_base;
    if (adt_pa > UINT64_MAX - cur_boot_args.devtree_size ||
        overlaps(MTP_STAGE_PA, MTP_RESERVE_SIZE, adt_pa, cur_boot_args.devtree_size))
        return false;
    int mm = adt_path_offset(adt, "/chosen/memory-map");
    if (mm < 0)
        return false;
    const char *names[] = {"SEPFW", "TrustCache", "preoslog"};
    for (unsigned i = 0; i < ARRAY_SIZE(names); i++) {
        u32 length;
        const void *p = adt_getprop(adt, mm, names[i], &length);
        if (!p)
            continue;
        u64 range[2];
        if (length != sizeof(range))
            return false;
        memcpy(range, p, sizeof(range));
        if (range[0] > UINT64_MAX - range[1] ||
            overlaps(MTP_STAGE_PA, MTP_RESERVE_SIZE, range[0], range[1]))
            return false;
    }
    return true;
}

static bool resources_valid(void)
{
    u32 length, value, phandle;
    const char *model = adt_getprop(adt, 0, "model", &length);
    int chosen = adt_path_offset(adt, "/chosen");
    int node = adt_path_offset(adt, MTP_PATH);
    int dart = adt_path_offset(adt, DART_PATH);
    int mapper = adt_path_offset(adt, DART_PATH "/mapper-mtp");
    int transport = adt_path_offset(adt, DOCK_PATH "/mtp-transport");
    if (chip_id != T8142 || board_id != 0x24 || !model || length != sizeof("Mac17,3") ||
        memcmp(model, "Mac17,3", length) || chosen < 0 || node < 0 || dart < 0 || mapper < 0 ||
        transport < 0 || !prop32(chosen, "chip-id", &value) || value != T8142 ||
        !prop32(chosen, "board-id", &value) || value != 0x24 ||
        !adt_is_compatible(adt, dart, "dart,t8110") ||
        !adt_is_compatible(adt, node, "iop,ascwrap-v6") ||
        !adt_is_compatible(adt, mapper, "iommu-mapper") || !prop32(mapper, "reg", &value) ||
        value != 0 || !prop32(mapper, "AAPL,phandle", &phandle) ||
        !prop32(node, "iommu-parent", &value) || value != phandle ||
        !prop32(transport, "iommu-parent", &value) || value != phandle)
        return false;
    u64 base, size;
    if (!reg(DOCK_PATH, 1, &base, &size) || base != 0x394b14000ULL || size < 0x1000 ||
        !reg(DOCK_PATH, 2, &base, &size) || base != 0x394b30000ULL || size < 0x1000 ||
        !reg(DOCK_PATH, 3, &base, &size) || base != MTP_DATA || size != 0x1000 ||
        !reg(DART_PATH, 0, &mtp.dart_base, &size) || mtp.dart_base != 0x394800000ULL ||
        size != 0xc000 || !reg(DART_PATH, 1, &base, &size) || base != 0x394810000ULL ||
        size != 0x4000 || !reg(MTP_PATH, 0, &base, &size) || base != 0x394600000ULL ||
        size != 0x88000)
        return false;
    /* The public pin documents two live firmware segments in this SRAM.
     * Reject a changed/malformed carveout instead of only printing a warning. */
    const u8 *segments = adt_getprop(adt, node, "segment-ranges", &length);
    if (!segments) {
        node = adt_first_child_offset(adt, node);
        if (node >= 0)
            segments = adt_getprop(adt, node, "segment-ranges", &length);
    }
    if (!segments || !length || length % 32 || length > 16 * 32)
        return false;
    u64 last_iova = 0;
    bool found_sram = false;
    for (u32 offset = 0; offset < length; offset += 32) {
        u64 physical, iova, remap;
        u32 bytes;
        memcpy(&physical, segments + offset, 8);
        memcpy(&iova, segments + offset + 8, 8);
        memcpy(&remap, segments + offset + 16, 8);
        memcpy(&bytes, segments + offset + 24, 4);
        if (!bytes || remap != physical || iova < last_iova || iova > UINT64_MAX - bytes ||
            physical > UINT64_MAX - bytes || (!offset && physical != MTP_SRAM))
            return false;
        if (physical >= MTP_SRAM && physical - MTP_SRAM < MTP_SRAM_SIZE) {
            if (bytes > MTP_SRAM_SIZE - (physical - MTP_SRAM))
                return false;
            found_sram = true;
        } else {
            /* The captured J813 tree also has a small IOP metadata segment
             * backed by iBoot-owned DRAM. It is not part of the SRAM window. */
            if (physical < cur_boot_args.phys_base ||
                physical + bytes > cur_boot_args.top_of_kernel_data ||
                overlaps(MTP_STAGE_PA, MTP_RESERVE_SIZE, physical, bytes))
                return false;
        }
        last_iova = iova + bytes;
    }
    return found_sram && memory_valid();
}

static bool power(const char *path)
{
    int node = adt_path_offset(adt, path);
    u32 length;
    const void *gates = adt_getprop(adt, node, "clock-gates", &length);
    return !gates || !length ||
           (!(length % 4) && pmgr_adt_power_enable(path) >= 0 && pmgr_adt_is_powered(path) == 1);
}

bool hv_windows_mtp_map_guest(void)
{
    if (!hv_windows_mtp_enabled())
        return true;
    if (!__atomic_load_n(&mtp.ready, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&mtp.failed, __ATOMIC_ACQUIRE) ||
        hv_map_hw(MTP_STAGE_DVA, MTP_STAGE_PA, MTP_STAGE_SIZE) < 0)
        return false;
    printf("mtp-handoff: stage2 IPA %#lx -> %#lx/+%#lx\n", (unsigned long)MTP_STAGE_DVA,
           (unsigned long)MTP_STAGE_PA, (unsigned long)MTP_STAGE_SIZE);
    return true;
}

bool hv_windows_mtp_stop(void)
{
    if (!mtp.owned)
        return true;
    if (mtp.stopped)
        return !__atomic_load_n(&mtp.failed, __ATOMIC_ACQUIRE);
    __atomic_store_n(&mtp.ready, false, __ATOMIC_RELEASE);
    /* All guest CPUs must be drained before this call. Never free backing
     * allocations in this single-session resident, even on a clean stop. */
    if (__atomic_load_n(&mtp.busy, __ATOMIC_ACQUIRE))
        return false;
    int before = exc_count;
    enum exc_guard_t saved = exc_guard;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    asc_cpu_stop(mtp.asc);
    write32(mtp.dart_base + DART_TCR, 0);
    write32(mtp.dart_base + DART_DISABLE, 1);
    write32(mtp.dart_base + DART_TLBI, 1 << 8);
    bool failed = poll32(mtp.dart_base + DART_TLBI, BIT(31), 0, 100) || asc_cpu_running(mtp.asc) ||
                  read32(mtp.dart_base + DART_TCR) != 0;
    sysop("dsb sy");
    exc_guard = saved;
    if (failed || exc_count != before)
        __atomic_store_n(&mtp.failed, true, __ATOMIC_RELEASE);
    mtp.stopped = !failed && exc_count == before;
    return !__atomic_load_n(&mtp.failed, __ATOMIC_ACQUIRE);
}

bool hv_windows_mtp_prepare(void)
{
    if (!hv_windows_mtp_enabled())
        return true;
    if (mtp.attempted)
        return false;
    mtp.attempted = true;
    printf("mtp-handoff: phase1 resource and memory admission\n");
    if (!resources_valid()) {
        printf("mtp-handoff: resource or reserved-memory admission failed\n");
        return false;
    }
    int before = exc_count;
    enum exc_guard_t saved = exc_guard;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    printf("mtp-handoff: phase2 power admission\n");
    if (!power(MTP_PATH) || !power(DART_PATH) || !power(DOCK_PATH) || exc_count != before)
        goto fail;
    printf("mtp-handoff: phase3 inherited ASC/DART state\n");
    mtp.asc = asc_init(MTP_PATH);
    if (!mtp.asc || asc_cpu_running(mtp.asc) || exc_count != before ||
        read32(mtp.dart_base + DART_PROTECT) & 1 || read32(mtp.dart_base + DART_TTBR) & 1 ||
        read32(mtp.dart_base + DART_ENABLE) & 1 || (read32(mtp.dart_base + DART_TCR) & ~1U) ||
        (read32(mtp.dart_base + DART_TLBI) & BIT(31)) || exc_count != before) {
        printf("mtp-handoff: inherited ASC/DART is active, locked or unreadable\n");
        goto fail;
    }
    mtp.owned = true;
    printf("mtp-handoff: phase4 DAPF and DART setup\n");
    if (dapf_init(DART_PATH, 1) < 0 || !power(DART_PATH) || exc_count != before)
        goto fail;
    mtp.dart = dart_init_adt(DART_PATH, 0, 0, false);
    if (!mtp.dart || dart_is_locked(mtp.dart) || exc_count != before ||
        read32(mtp.dart_base + DART_TCR) != 1 || !(read32(mtp.dart_base + DART_TTBR) & 1) ||
        (read32(mtp.dart_base + DART_TLBI) & BIT(31)))
        goto fail;
    mtp.iovad = iovad_init(SZ_32M, SZ_32M + 0x10000000ULL);
    if (!mtp.iovad)
        goto fail;
    printf("mtp-handoff: phase5 reserved staging and DMA mapping\n");
    memset((void *)MTP_STAGE_PA, 0, MTP_RESERVE_SIZE);
    dc_cvac_range((void *)MTP_STAGE_PA, MTP_RESERVE_SIZE);
    if (dart_map(mtp.dart, MTP_STAGE_DVA, (void *)MTP_STAGE_PA, MTP_STAGE_SIZE) < 0 ||
        exc_count != before || (read32(mtp.dart_base + DART_TLBI) & BIT(31)))
        goto fail;
    for (u64 offset = 0; offset < MTP_STAGE_SIZE; offset += SZ_16K)
        if ((u64)dart_translate_silent(mtp.dart, MTP_STAGE_DVA + offset) != MTP_STAGE_PA + offset)
            goto fail;
    printf("mtp-handoff: phase6 fixed-pool RTKit boot\n");
    mtp.rtkit = rtkit_init("mtp-handoff", mtp.asc, mtp.dart, mtp.iovad, NULL, false);
    if (!mtp.rtkit || !rtkit_set_phys_window(mtp.rtkit, MTP_SRAM, MTP_SRAM_SIZE) ||
        !rtkit_set_buffer_pool(mtp.rtkit, MTP_STAGE_PA + MTP_STAGE_SIZE, MTP_STAGE_SIZE) ||
        !rtkit_set_early_ap_power(mtp.rtkit, true) || !rtkit_boot_timed(mtp.rtkit, MTP_TIMEOUT) ||
        exc_count != before)
        goto fail;
    u64 deadline = timeout_calculate(MTP_TIMEOUT);
    u32 count = 0;
    printf("mtp-handoff: phase7 non-consuming DockChannel readiness\n");
    do {
        if (rtkit_service_quiet(mtp.rtkit, 4) < 0 || exc_count != before)
            goto fail;
        count = read32(MTP_DATA + 0x2c); /* RX_COUNT only; never consume INIT. */
        if (count)
            break;
    } while (!timeout_expired(deadline));
    if (!count || exc_count != before)
        goto fail;
    __atomic_store_n(&mtp.ready, true, __ATOMIC_RELEASE);
    if (!hv_windows_mtp_map_guest() || exc_count != before)
        goto fail;
    exc_guard = saved;
    printf("mtp-handoff: J813 ready, preserved DockChannel RX=%u, DART SID0, pool %#lx/+%#lx\n",
           count, (unsigned long)(MTP_STAGE_PA + MTP_STAGE_SIZE), (unsigned long)MTP_STAGE_SIZE);
    return true;
fail:
    exc_guard = saved;
    hv_windows_mtp_stop();
    __atomic_store_n(&mtp.failed, true, __ATOMIC_RELEASE);
    printf("mtp-handoff: failed; allocations retained, fresh Aura required\n");
    return false;
}

void hv_windows_mtp_poll(void)
{
    if (!__atomic_load_n(&mtp.ready, __ATOMIC_ACQUIRE) || !mtp.rtkit ||
        __atomic_exchange_n(&mtp.busy, 1, __ATOMIC_ACQUIRE))
        return;
    u64 now = hv_host_counter();
    if (now >= mtp.next_poll) {
        mtp.next_poll = now + mrs(CNTFRQ_EL0) / 100;
        if (rtkit_service_quiet(mtp.rtkit, 4) < 0) {
            __atomic_store_n(&mtp.ready, false, __ATOMIC_RELEASE);
            __atomic_store_n(&mtp.failed, true, __ATOMIC_RELEASE);
        }
    }
    __atomic_store_n(&mtp.busy, 0, __ATOMIC_RELEASE);
}

void hv_windows_mtp_report(void)
{
    if (__atomic_load_n(&mtp.failed, __ATOMIC_ACQUIRE) && hv_windows_mtp_enabled()) {
        printf("mtp-handoff: RTKit service failed; aborting guest\n");
        hv_windows_abort_guest();
    }
}
