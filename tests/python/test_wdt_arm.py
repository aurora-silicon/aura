"""Exercise the production watchdog arming, disable and reboot paths."""
from pathlib import Path
import subprocess


def test_wdt_arm_seconds(tmp_path):
    repo = Path(__file__).resolve().parents[2]
    source = (repo / 'src/wdt.c').read_text()
    body = source[source.index('#define WDT_COUNT'):]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define BIT(x) (1u << (x))
#define T8140 0x8140
#define T6040 0x6040
#define WDT_TRIAL_SECONDS 178
#define WDT_BOOT_SECONDS 178
#define ADT_GETPROP(a,n,k,v) (*(v)=adt_version,0)
#define sysop(x) ((void)0)
struct wdt_guard {
    bool borrowed;
    u64 base;
    u32 ctl, count, alarm;
};
static u32 chip_id = T6040, adt_version = 2, regs[12], secondary_value, reconfig_value;
static u64 primary = 0x1000, primary_size = 0x4000, secondary_size = 4, reconfig_size = 4;
static bool has_simple_reconfig, has_reg3 = true;
static unsigned writes, irq_status_clears, cache_cleans;
static bool bad_readback, bad_reconfig_readback;
static int adt_dummy;
static void *adt = &adt_dummy;
static void dc_cvac_range(const void *p, size_t n)
{ (void)p; (void)n; cache_cleans++; }
static int adt_path_offset_trace(void *a, const char *p, int *trace)
{ (void)a; (void)p; (void)trace; return 1; }
static const void *adt_getprop(void *a, int node, const char *key, u32 *len)
{
    (void)a; (void)node; (void)len;
    if (!strcmp(key, "simple-reconfig-wdog-support"))
        return has_simple_reconfig ? (const void *)1 : NULL;
    return NULL;
}
static int adt_get_reg(void *a, int *p, const char *key, int i, u64 *base, u64 *size)
{
    (void)a; (void)p; (void)key;
    if (i == 0) {
        *base = primary;
        if (size) *size = primary_size;
        return 0;
    }
    if (i == 2) {
        *base = 0x2000;
        if (size) *size = secondary_size;
        return 0;
    }
    if (i == 3) {
        if (!has_reg3) return -1;
        *base = 0x3000;
        if (size) *size = reconfig_size;
        return 0;
    }
    return -1;
}
static u32 read32(u64 addr)
{
    if (addr == 0x2000) return secondary_value;
    if (addr == 0x3000) return bad_reconfig_readback ? 0 : reconfig_value;
    assert(addr >= primary && addr < primary + 0x30);
    if (bad_readback && (addr == primary + 0x14 || addr == primary + 0x28)) return 0;
    return regs[(addr - primary) / 4];
}
static void write32(u64 addr, u32 value)
{
    if (addr == 0x2000) {
        secondary_value = value;
        writes++;
        return;
    }
    if (addr == 0x3000) {
        reconfig_value = value;
        writes++;
        return;
    }
    assert(addr >= primary && addr < primary + 0x30);
    if (addr == primary + 0x2c && value == 2u)
        irq_status_clears++;
    regs[(addr - primary) / 4] = value;
    writes++;
}
''' + body + r'''
int main(void)
{
    /* wdt-version 2: WD1 RESET_EN path unchanged */
    assert(wdt_arm_seconds(30) == 0);
    assert(regs[WDT_ALARM/4] == 720000000);
    assert(regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(writes == 4 && wdt_base == primary && wdt_reconfig == 0);
    chip_id = T8140;
    assert(wdt_arm_seconds(178) == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U);
    writes = 0;
    assert(wdt_arm_seconds(0) == -1 && writes == 0);
    assert(wdt_arm_seconds(179) == -1 && writes == 0);
    chip_id = 0x8103;
    assert(wdt_arm_seconds(30) == 0 && writes == 4);

    /* wdt-version 3 WITHOUT simple-reconfig-wdog-support refuses */
    writes = 0;
    adt_version = 3;
    has_simple_reconfig = false;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);

    /* wdt-version 3 WITH simple-reconfig-wdog-support + reg[3]: WD2 bark-IRQ + reg[3]=0x10 */
    has_simple_reconfig = true;
    reconfig_value = 0x000000a5u; /* unrelated bits preserved, bits [5:4] -> 01 */
    writes = 0;
    irq_status_clears = 0;
    cache_cleans = 0;
    u64 out_pri = 0, out_rec = 0;
    assert(wdt_simple_reconfig_reg(&out_pri, &out_rec) && out_pri == primary && out_rec == 0x3000);
    assert(wdt_arm_seconds(30) == 0);
    assert(regs[WDT_WD2_BITE/4] == 0xffffffffu);
    assert(regs[WDT_WD2_BARK/4] == 720000000u);
    assert(regs[WDT_WD2_CTL/4] == 1 && regs[WDT_WD2_COUNT/4] == 0);
    assert(reconfig_value == 0x00000095u);
    assert(irq_status_clears == 1 && cache_cleans >= 2);
    assert(wdt_reconfig == 0x3000);

    /* wdt_disable clears WD1, WD2, and reg[2] */
    regs[WDT_CTL/4] = 4;
    secondary_value = 1;
    wdt_disable();
    assert(regs[WDT_CTL/4] == 0 && regs[WDT_WD2_CTL/4] == 0 && secondary_value == 0);

    /* wdt_reboot uses WD2 + reg[3] even on an early fault before wdt_disable() */
    wdt_base = 0;
    wdt_reconfig = 0;
    reconfig_value = 0;
    wdt_reboot();
    assert(regs[WDT_WD2_BARK/4] == 0x100000u && regs[WDT_WD2_BITE/4] == 0xffffffffu);
    assert(regs[WDT_WD2_CTL/4] == 1 && reconfig_value == 0x10u);

    /* Missing reg[3], bad reg[3] size, or short reg[0] refuses on simple-reconfig */
    writes = 0;
    has_reg3 = false;
    assert(!wdt_simple_reconfig_reg(&out_pri, &out_rec));
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    has_reg3 = true;
    reconfig_size = 8;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    reconfig_size = 4;
    primary_size = 0x20;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    primary_size = 0x4000;
    /* Read-back failure clears WD2_CTL and restores reg[3] so nothing is left changed */
    reconfig_value = 0x000000a5u;
    bad_readback = true;
    assert(wdt_arm_seconds(30) == -1 && regs[WDT_WD2_CTL/4] == 0 && reconfig_value == 0x000000a5u);
    bad_readback = false;
    bad_reconfig_readback = true;
    assert(wdt_arm_seconds(30) == -1 && regs[WDT_WD2_CTL/4] == 0);
    bad_reconfig_readback = false;

    /* Trial arms WD2 on v3 simple-reconfig after disarming iBoot's */
    secondary_value = 1;
    writes = 0;
    assert(wdt_arm_trial() == 0);
    assert(regs[WDT_WD2_BARK/4] == 4272000000U && regs[WDT_WD2_CTL/4] == 1);
    assert((reconfig_value & 0x30u) == 0x10u && secondary_value == 0);

    /* wdt_s2r_guard_supported preserves simple-reconfig channel on v2 if ADT advertises it */
    adt_version = 2;
    has_simple_reconfig = true;
    wdt_reconfig = 0;
    assert(wdt_s2r_guard_supported() && wdt_reconfig == 0x3000);
    has_simple_reconfig = false;
    assert(wdt_s2r_guard_supported() && wdt_reconfig == 0);

    /* Back to v2 checks */
    adt_version = 2;
    has_simple_reconfig = false;
    assert(!wdt_simple_reconfig_reg(&out_pri, &out_rec));
    primary_size = 0x1f;
    writes = 0;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    primary_size = 0x4000;
    secondary_size = 8;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_size = 4;
    secondary_value = 1;
    assert(wdt_arm_seconds(30) == -1 && writes == 0);
    secondary_value = 0;
    bad_readback = true;
    assert(wdt_arm_seconds(30) == -1 && regs[WDT_CTL/4] == 0);
    bad_readback = false;

    /* Trial arms WD1 on v2 */
    chip_id = 0x6000;
    secondary_value = 1;
    writes = 0;
    assert(wdt_arm_trial() == 0);
    assert(regs[WDT_ALARM/4] == 4272000000U && regs[WDT_CTL/4] == 4 && regs[WDT_COUNT/4] == 0);
    assert(secondary_value == 0 && wdt_base == primary && wdt_reconfig == 0);

    /* Early reboot on v2 uses WD1 */
    wdt_base = 0;
    wdt_reboot();
    assert(regs[WDT_ALARM/4] == 0x100000u && regs[WDT_CTL/4] == 4);
    return 0;
}
'''
    binary = tmp_path / 'wdt-arm'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined',
                    '-x', 'c', '-', '-o', str(binary)], input=harness, text=True, check=True)
    subprocess.run([str(binary)], check=True)


def test_dt_set_wdt_reconfig(tmp_path):
    """kboot publishes ADT reg[3] onto 1-reg watchdog DT nodes when simple-reconfig is present."""
    from fdt_harness import LIBFDT, LIBFDT_SOURCES, PRELUDE, definition
    from host_compiler import host_cc

    repo = Path(__file__).resolve().parents[2]
    kboot = (repo / 'src/kboot.c').read_text()
    helper = definition(kboot, 'static bool dt_ranges_identity_covers(int parent, u64 addr, u64 size)')
    func = definition(kboot, 'static void dt_set_wdt_reconfig(void)')
    prepare = definition(kboot, 'int kboot_prepare_dt(void *fdt)')
    assert 'dt_set_wdt_reconfig();' in prepare
    assert prepare.index('dt_set_trial_watchdog();') < prepare.index('dt_set_wdt_reconfig();')

    code = PRELUDE + r'''
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static bool adt_has_reconfig;
static u64 adt_primary = 0x3882b0000ULL;
static u64 adt_reconfig = 0x38823002cULL;
static bool wdt_simple_reconfig_reg(u64 *primary_out, u64 *reconfig_out)
{
    if (!adt_has_reconfig)
        return false;
    *primary_out = adt_primary;
    *reconfig_out = adt_reconfig;
    return true;
}
''' + helper + func + r'''
static int make_soc(void)
{
    fresh();
    int root = fdt_path_offset(tree, "/");
    assert(!fdt_setprop_u32(tree, root, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, root, "#size-cells", 2));
    int soc = node_at("/", "soc");
    assert(!fdt_setprop_u32(tree, soc, "#address-cells", 2));
    assert(!fdt_setprop_u32(tree, soc, "#size-cells", 2));
    assert(!fdt_setprop_empty(tree, soc, "ranges"));
    return soc;
}

static void check_reg(int node, int expected_entries, const u64 *expected)
{
    int len = 0;
    const void *p = fdt_getprop(dt, node, "reg", &len);
    assert(p && len == expected_entries * 16);
    fdt64_t got[4];
    memcpy(got, p, (size_t)len);
    for (int i = 0; i < expected_entries * 2; i++)
        assert(fdt64_to_cpu(got[i]) == expected[i]);
}

int main(void)
{
    /* 1. T8142-style 1-reg node gets reg[3] + reg-names = "wdt", "reconfig". */
    make_soc();
    int wdt = node_at("/soc", "watchdog@3882b0000");
    static const char compat_t8142[] = "apple,t8142-wdt\0apple,t8103-wdt";
    assert(!fdt_setprop(tree, wdt, "compatible", compat_t8142, sizeof(compat_t8142)));
    reg(wdt, adt_primary, 0x4000);
    adt_has_reconfig = true;
    dt_set_wdt_reconfig();

    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    const u64 want2[4] = {adt_primary, 0x4000, adt_reconfig, 4};
    const u64 want1[2] = {adt_primary, 0x4000};
    check_reg(wdt, 2, want2);
    assert(fdt_stringlist_count(tree, wdt, "reg-names") == 2);
    assert(fdt_stringlist_search(tree, wdt, "reg-names", "wdt") == 0);
    assert(fdt_stringlist_search(tree, wdt, "reg-names", "reconfig") == 1);

    /* 2. Idempotent on a second call (already has "reconfig", like T6050). */
    dt_set_wdt_reconfig();
    check_reg(wdt, 2, want2);
    assert(fdt_stringlist_count(tree, wdt, "reg-names") == 2);

    /* 3. Existing 1-entry reg-names ("primary") is preserved and "reconfig" appended. */
    make_soc();
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,wdt"));
    reg(wdt, adt_primary, 0x4000);
    assert(!fdt_setprop_string(tree, wdt, "reg-names", "primary"));
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    check_reg(wdt, 2, want2);
    assert(fdt_stringlist_search(tree, wdt, "reg-names", "primary") == 0);
    assert(fdt_stringlist_search(tree, wdt, "reg-names", "reconfig") == 1);

    /* 4. Node with both compatibles + sibling node after it: edited once, sibling intact. */
    make_soc();
    wdt = node_at("/soc", "watchdog@3882b0000");
    static const char compat_both[] = "apple,t8103-wdt\0apple,wdt";
    assert(!fdt_setprop(tree, wdt, "compatible", compat_both, sizeof(compat_both)));
    reg(wdt, adt_primary, 0x4000);
    int sib = node_at("/soc", "watchdog@23d2b0000");
    assert(!fdt_setprop(tree, sib, "compatible", compat_both, sizeof(compat_both)));
    reg(sib, 0x23d2b0000ULL, 0x4000);
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    sib = fdt_path_offset(tree, "/soc/watchdog@23d2b0000");
    check_reg(wdt, 2, want2);
    assert(fdt_stringlist_count(tree, wdt, "reg-names") == 2);
    const u64 want_sib[2] = {0x23d2b0000ULL, 0x4000};
    check_reg(sib, 1, want_sib);
    assert(!fdt_getprop(tree, sib, "reg-names", NULL));

    /* 5. Explicit identity ranges (T6050 die0 shape) covering both addresses publishes. */
    int soc = make_soc();
    fdt64_t id_ranges[3] = {
        cpu_to_fdt64(0x200000000ULL),
        cpu_to_fdt64(0x200000000ULL),
        cpu_to_fdt64(0x400000000ULL),
    };
    assert(!fdt_setprop(tree, soc, "ranges", id_ranges, sizeof(id_ranges)));
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    check_reg(wdt, 2, want2);

    /* 6. Non-identity ranges or ranges missing reconfig skips safely. */
    soc = make_soc();
    fdt64_t xlat_ranges[3] = {
        cpu_to_fdt64(0x200000000ULL),
        cpu_to_fdt64(0x300000000ULL),
        cpu_to_fdt64(0x400000000ULL),
    };
    assert(!fdt_setprop(tree, soc, "ranges", xlat_ranges, sizeof(xlat_ranges)));
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    check_reg(wdt, 1, want1);
    assert(!fdt_getprop(tree, wdt, "reg-names", NULL));

    soc = make_soc();
    fdt64_t short_ranges[3] = {
        cpu_to_fdt64(adt_primary),
        cpu_to_fdt64(adt_primary),
        cpu_to_fdt64(0x4000ULL),
    };
    assert(!fdt_setprop(tree, soc, "ranges", short_ranges, sizeof(short_ranges)));
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    check_reg(wdt, 1, want1);
    assert(!fdt_getprop(tree, wdt, "reg-names", NULL));

    /* 7. #cells != 2 or 2-entry reg without "reconfig" skips safely. */
    soc = make_soc();
    assert(!fdt_setprop_u32(tree, soc, "#address-cells", 1));
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    check_reg(wdt, 1, want1);

    make_soc();
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    fdt64_t two_regs[4] = {
        cpu_to_fdt64(adt_primary), cpu_to_fdt64(0x4000),
        cpu_to_fdt64(0x3882c0000ULL), cpu_to_fdt64(0x4000),
    };
    assert(!fdt_setprop(tree, wdt, "reg", two_regs, sizeof(two_regs)));
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tree, "/soc/watchdog@3882b0000");
    const u64 want_two_other[4] = {adt_primary, 0x4000, 0x3882c0000ULL, 0x4000};
    check_reg(wdt, 2, want_two_other);
    assert(!fdt_getprop(tree, wdt, "reg-names", NULL));

    /* 8. Out-of-space on reg-names rolls back reg cleanly to its original 1-entry value. */
    make_soc();
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    assert(!fdt_pack(tree));
    u32 packed = fdt_totalsize(tree);
    /* Allow room for the 16-byte reg append, but not for reg-names (+28 bytes + string). */
    static _Alignas(8) unsigned char tight[4096];
    assert(packed + 20 < sizeof(tight));
    assert(!fdt_open_into(tree, tight, (int)(packed + 20)));
    dt = tight;
    dt_set_wdt_reconfig();
    wdt = fdt_path_offset(tight, "/soc/watchdog@3882b0000");
    check_reg(wdt, 1, want1);
    assert(!fdt_getprop(tight, wdt, "reg-names", NULL));
    dt = tree;

    /* 9. Non-simple-reconfig ADT leaves DT untouched. */
    adt_has_reconfig = false;
    make_soc();
    wdt = node_at("/soc", "watchdog@3882b0000");
    assert(!fdt_setprop_string(tree, wdt, "compatible", "apple,t8103-wdt"));
    reg(wdt, adt_primary, 0x4000);
    dt_set_wdt_reconfig();
    check_reg(wdt, 1, want1);
    assert(!fdt_getprop(tree, wdt, "reg-names", NULL));

    printf("dt_set_wdt_reconfig ok\n");
    return 0;
}
'''
    src = tmp_path / 'dt-wdt-reconfig.c'
    src.write_text(code)
    binary = tmp_path / 'dt-wdt-reconfig'
    cmd = host_cc() + [
        '-std=gnu11', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-fsanitize=undefined', '-fno-sanitize-recover=all',
        '-I', str(LIBFDT), str(src),
        *[str(LIBFDT / f) for f in (*LIBFDT_SOURCES, 'fdt_addresses.c')],
        '-o', str(binary),
    ]
    built = subprocess.run(cmd, capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    res = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
    assert res.returncode == 0, res.stdout + res.stderr
    assert 'dt_set_wdt_reconfig ok' in res.stdout
