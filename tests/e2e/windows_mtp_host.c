/* SPDX-License-Identifier: MIT */
/* Production MTP lifecycle replay; modeled ADT, MMIO and subordinate devices.
 * Failure model: docs/windows-mtp-handoff.md. No physical addresses are accessed. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TYPES_H
#define UTILS_H
#define HV_H
#define PMGR_H
#define ASC_H
#define DART_H
#define DAPF_H
#define IOVA_H
#define RTKIT_H
#define MEMORY_H
#define HEAPBLOCK_H
#define XNUBOOT_H
#define __EXCEPTION_H__
typedef uint8_t u8; typedef uint32_t u32; typedef uint64_t u64;
#define BIT(x) (1ULL << (x))
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define T8142 0x8142
#define SZ_16K 16384
#define SZ_32M (32ULL << 20)
#define USEC_PER_SEC 1000000
enum exc_guard_t { GUARD_OFF, GUARD_SKIP, GUARD_MARK, GUARD_RETURN, GUARD_SILENT=0x100 };
static enum exc_guard_t exc_guard;
static int exc_count;
static u32 chip_id=T8142, board_id=0x24;
static void *adt;
static struct { u64 phys_base, mem_size; void *devtree; u64 virt_base, devtree_size, top_of_kernel_data; }
    cur_boot_args={0x10000000000ULL, 0x400000000ULL, (void *)0x10001000000ULL, 0x10000000000ULL,
                   0x80000, 0x10010000000ULL};
typedef struct { unsigned marker; } asc_dev_t;
typedef struct { unsigned marker; } dart_dev_t;
typedef struct { unsigned marker; } iova_domain_t;
typedef struct { unsigned marker; } rtkit_dev_t;
/* Modeled snapshot boundary; production RTKit is replayed separately. */
struct rtkit_rejection { unsigned reason; u64 msg0, msg1; };
struct rtkit_notification_counts { u64 app, debug, oslog_type3; };
static asc_dev_t asc_obj; static dart_dev_t dart_obj;
static iova_domain_t iova_obj; static rtkit_dev_t rtkit_obj;
static const char *scenario;
static bool is(const char *name) { return !strcmp(scenario, name); }
static unsigned reads, writes, starts, stops, maps, grants, polls, power_checks, translations, aborts;
static bool running, mapped, pool, sram, early, cleaned, prepared_power;
static u64 clock_us;
static u32 tcr, ttbr, enable, tlbi;
static unsigned char staging[0x200000];
static u64 heapblock_high_water(void) { return is("heap-overlap") ? 0x10020004000ULL : 0x10018000000ULL; }
static bool hv_windows_mtp_enabled(void) { return !is("disabled"); }
static void hv_windows_abort_guest(void) { ++aborts; }
static u64 hv_host_counter(void) { return clock_us += 10000; }
#define mrs(reg) 1000000ULL
static void model_sysop(const char *op) { assert(!strcmp(op,"dsb sy")); }
#define sysop(op) model_sysop(op)
static u64 timeout_calculate(u32 delay) { return clock_us + delay; }
static bool timeout_expired(u64 deadline) { clock_us += 10000; return clock_us >= deadline; }
static u32 read32(u64 address) {
    assert(prepared_power); ++reads;
    if (is("mmio-fault") && reads == 1) ++exc_count;
    if (address == 0x394b3402cULL) {
        if (is("readiness-mmio")) ++exc_count;
        return is("no-init") ? 0 : 12;
    }
    assert(address >= 0x394800000ULL && address < 0x39480c000ULL);
    switch (address - 0x394800000ULL) {
    case 0x200: return is("locked");
    case 0xc00: return enable;
    case 0x1000: return tcr;
    case 0x1400: return ttbr;
    case 0x80: return tlbi;
    default: assert(!"unexpected register read"); return 0;
    }
}
static void write32(u64 address, u32 value) {
    ++writes;
    if (address == 0x394801000ULL) tcr = is("stop-fail") ? 1 : value;
    else if (address == 0x394800c20ULL) { assert(value == 1); enable = 0; }
    else if (address == 0x394800080ULL) { assert(value == 0x100); tlbi = 0; }
    else assert(!"unexpected register write");
}
static int poll32(u64 address, u32 mask, u32 target, u32 timeout) {
    assert(timeout == 100); return (read32(address) & mask) != target;
}
static int pmgr_adt_power_enable(const char *path) {
    (void)path; ++power_checks; assert(!reads && !writes);
    return is("power-fail") ? -1 : 0;
}
static int pmgr_adt_is_powered(const char *path) {
    (void)path; return is("power-inactive") ? 0 : 1;
}
static asc_dev_t *asc_init(const char *path) {
    assert(!strcmp(path,"/arm-io/mtp")); prepared_power = true;
    return is("asc-allocation") ? NULL : &asc_obj;
}
static bool asc_cpu_running(asc_dev_t *asc) { assert(asc == &asc_obj); return running; }
static void asc_cpu_stop(asc_dev_t *asc) { assert(asc == &asc_obj); ++stops; running=false; }
static int dapf_init(const char *path, int index) {
    assert(!strcmp(path,"/arm-io/dart-mtp") && index == 1);
    return is("dapf") ? -1 : 0;
}
static dart_dev_t *dart_init_adt(const char *path, int index, int sid, bool keep) {
    assert(!strcmp(path,"/arm-io/dart-mtp") && !index && !sid && !keep);
    if (is("dart-allocation")) return NULL;
    tcr=1; ttbr=1; enable=1; return &dart_obj;
}
static bool dart_is_locked(dart_dev_t *dart) { assert(dart == &dart_obj); return false; }
static iova_domain_t *iovad_init(u64 base, u64 limit) {
    assert(base == SZ_32M && limit == SZ_32M+0x10000000ULL);
    return is("iova-allocation") ? NULL : &iova_obj;
}
static void *model_memset(void *p, int value, size_t size) {
    assert(p == (void *)0x10020000000ULL && value == 0 && size == sizeof(staging));
    return memset(staging, value, size);
}
static void dc_cvac_range(void *p, size_t size) {
    assert(p == (void *)0x10020000000ULL && size == sizeof(staging));
    for (size_t i=0; i<size; ++i) assert(!staging[i]);
    cleaned=true;
}
static int dart_map(dart_dev_t *dart, uintptr_t dva, void *p, size_t size) {
    assert(dart == &dart_obj && dva == 0x1800000 && p == (void *)0x10020000000ULL && size == 0x100000 && cleaned);
    mapped=!is("dart-map"); return mapped ? 0 : -1;
}
static void *dart_translate_silent(dart_dev_t *dart, uintptr_t dva) {
    assert(dart == &dart_obj && mapped && dva >= 0x1800000 && dva < 0x1900000 && !(dva & 16383));
    ++translations;
    if (is("translation") && dva == 0x18fc000) return NULL;
    return (void *)(0x10020000000ULL + dva - 0x1800000);
}
static rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, dart_dev_t *dart, iova_domain_t *domain, void *sart, bool wait) {
    assert(!strcmp(name,"mtp-handoff") && asc == &asc_obj && dart == &dart_obj && domain == &iova_obj && !sart && !wait && translations == 64);
    return is("rtkit-allocation") ? NULL : &rtkit_obj;
}
static bool rtkit_set_phys_window(rtkit_dev_t *rtk, u64 base, size_t size) {
    assert(rtk == &rtkit_obj && base == 0x394c00000ULL && size == 0x100000);
    sram=true; return !is("sram-window");
}
static bool rtkit_set_buffer_pool(rtkit_dev_t *rtk, u64 base, size_t size) {
    assert(rtk == &rtkit_obj && base == 0x10020100000ULL && size == 0x100000 && sram);
    ++grants; pool=true; return !is("pool-map");
}
static bool rtkit_set_early_ap_power(rtkit_dev_t *rtk, bool value) {
    assert(rtk == &rtkit_obj && value && pool); early=true; return true;
}
static bool rtkit_boot_timed(rtkit_dev_t *rtk, u32 timeout) {
    assert(rtk == &rtkit_obj && early && timeout == 3000000); ++starts; running=true;
    return !is("boot-timeout");
}
static int rtkit_service_quiet(rtkit_dev_t *rtk, int count) {
    assert(rtk == &rtkit_obj && count == 4); ++polls;
    if (is("service-mmio")) ++exc_count;
    return is("service-fail") || (is("runtime-fail") && maps) ? -1 : 0;
}
static bool __attribute__((unused)) rtkit_get_first_rejection(const rtkit_dev_t *rtk, struct rtkit_rejection *out) {
    assert(rtk == &rtkit_obj);
    *out = (struct rtkit_rejection){3, 0x123456789abcdef0ULL, 2};
    return true;
}
static bool __attribute__((unused)) rtkit_get_notification_counts(const rtkit_dev_t *rtk, struct rtkit_notification_counts *out) {
    assert(rtk == &rtkit_obj);
    *out = (struct rtkit_notification_counts){2, 1, 3};
    return true;
}
static int hv_map_hw(u64 ipa, u64 pa, u64 size) {
    assert(ipa == 0x1800000 && pa == 0x10020000000ULL && size == 0x100000 && pool);
    ++maps; return is("stage2-map") || (is("final-map") && maps == 2) ? -1 : 0;
}
#include "src/adt.h"
static const char *paths[]={"/","/chosen","/arm-io/mtp","/arm-io/dart-mtp","/arm-io/dart-mtp/mapper-mtp","/arm-io/dockchannel-mtp","/arm-io/dockchannel-mtp/mtp-transport","/chosen/memory-map"};
int adt_path_offset(const void *tree, const char *path) {
    (void)tree; for(unsigned i=0; i<ARRAY_SIZE(paths); ++i) if(!strcmp(paths[i],path)) return i; return -1;
}
int adt_path_offset_trace(const void *tree, const char *path, int *out) { return out[0]=adt_path_offset(tree,path); }
int adt_first_child_offset(const void *tree, int node) { (void)tree; (void)node; return -1; }
bool adt_is_compatible(const void *tree, int node, const char *value) {
    (void)tree; if(is("compatible")) return false;
    return (node==2 && !strcmp(value,"iop,ascwrap-v6")) || (node==3 && !strcmp(value,"dart,t8110")) || (node==4 && !strcmp(value,"iommu-mapper"));
}
const void *adt_getprop(const void *tree, int node, const char *name, u32 *length) {
    (void)tree; static u32 value; static u64 segments[12]; static u64 allocation[2];
    if(length) *length=4;
    if(node==0 && !strcmp(name,"model")) { if(length) *length=8; return is("model") ? "Mac00,0" : "Mac17,3"; }
    if(node==1 && !strcmp(name,"chip-id")) value=chip_id;
    else if(node==1 && !strcmp(name,"board-id")) value=board_id;
    else if(node==4 && !strcmp(name,"reg")) value=is("sid") ? 1 : 0;
    else if(node==4 && !strcmp(name,"AAPL,phandle")) value=239;
    else if((node==2 || node==6) && !strcmp(name,"iommu-parent")) value=is("mapper") ? 238 : 239;
    else if(node==2 && !strcmp(name,"segment-ranges")) {
        u64 data[]={0x394c00000ULL,0x1000000,0x394c00000ULL,0x300054000ULL,
                    0x394c54000ULL,0x1054000,0x394c54000ULL,0x20006c000ULL,
                    0x100054f4000ULL,0x10c0000,0x100054f4000ULL,0xa00003000ULL};
        memcpy(segments,data,sizeof(data));
        if(is("segment-overflow")) segments[8]=segments[10]=UINT64_MAX-1;
        if(is("segment-alias")) segments[9]=0x1000000;
        if(length) *length=is("segment-size") ? 95 : sizeof(segments); return segments;
    } else if(!strcmp(name,"clock-gates")) {
        if(node!=2) return NULL;
        value=1; if(length) *length=(is("power-fail") || is("power-inactive")) ? 4 : is("gates-size") ? 3 : 0;
    } else if(node==7 && !strcmp(name,"SEPFW") && is("firmware-overlap")) {
        allocation[0]=0x10020004000ULL; allocation[1]=0x4000; if(length) *length=16; return allocation;
    } else return NULL;
    return &value;
}
int adt_get_reg(const void *tree, int *path, const char *name, int index, u64 *base, u64 *size) {
    (void)tree; assert(!strcmp(name,"reg"));
    if(path[0]==2 && index==0) { *base=0x394600000ULL; *size=0x88000; }
    else if(path[0]==3 && index==0) { *base=0x394800000ULL; *size=0xc000; }
    else if(path[0]==3 && index==1) { *base=0x394810000ULL; *size=0x4000; }
    else if(path[0]==5 && index==1) { *base=0x394b14000ULL; *size=0x1000; }
    else if(path[0]==5 && index==2) { *base=0x394b30000ULL; *size=0x1000; }
    else if(path[0]==5 && index==3) { *base=0x394b34000ULL; *size=is("data-reg") ? 0x20 : 0x1000; }
    else return -1;
    return 0;
}
#define memset model_memset
#include "src/hv/windows/mtp_handoff.c"
#undef memset
int main(int argc, char **argv) {
    assert(argc==2); scenario=argv[1]; memset(staging,0xaa,sizeof(staging));
    if(is("chip")) chip_id=0x8112;
    if(is("board")) board_id=0x25;
    if(is("ram-outside")) cur_boot_args.mem_size=0x1000000;
    if(is("adt-overlap")) cur_boot_args.devtree=(void *)0x10020000000ULL;
    if(is("running")) running=true;
    if(is("active-root")) ttbr=1;
    if(is("active-stream")) enable=1;
    if(is("bypass")) tcr=2;
    if(is("tlbi-busy")) tlbi=BIT(31);
    bool result=hv_windows_mtp_prepare();
    if(is("disabled")) {
        assert(result && !reads && !writes && !maps && !starts);
        hv_windows_mtp_poll(); hv_windows_mtp_report(); assert(hv_windows_mtp_stop());
    } else if(is("success") || is("runtime-fail") || is("stop-fail") || is("final-map")) {
        assert(result && maps==1 && starts==1 && translations==64 && grants==1 && exc_guard==GUARD_OFF);
        assert(!hv_windows_mtp_prepare());
        if(is("final-map")) assert(!hv_windows_mtp_map_guest());
        else assert(hv_windows_mtp_map_guest());
        unsigned before=polls; hv_windows_mtp_poll(); assert(polls==before+1); hv_windows_mtp_report();
        assert(aborts == is("runtime-fail"));
        bool stopped=hv_windows_mtp_stop();
        assert(stopped == !(is("stop-fail") || is("runtime-fail")));
        assert(stops==1 && !running && !enable);
        before=polls; hv_windows_mtp_poll(); assert(polls==before);
    } else {
        assert(!result && exc_guard==GUARD_OFF && !mtp.ready);
        unsigned before=reads+writes+starts; assert(!hv_windows_mtp_prepare()); assert(before==reads+writes+starts);
        if(mtp.owned) assert(stops==1 && !running && !enable && !tcr);
        else assert(!writes && !starts && !maps);
    }
    assert(clock_us <= 3100000);
    printf("PASS %s reads=%u writes=%u starts=%u stops=%u polls=%u maps=%u\n",scenario,reads,writes,starts,stops,polls,maps);
}
