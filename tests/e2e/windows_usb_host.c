/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * D4 host replay of the carrier glue src/hv/windows/hv_usb.c, compiled unchanged (with
 * hv_usb_dart.c) against stub headers from tests/python/test_windows_t8152_usb.py, ASan +
 * UBSan.  The hardware is a fake register file: every write, AIC mask, TLBI, stage-2 call and
 * alias change is recorded in order, so the test can check that each refusal (F1-F5) writes
 * nothing, that the success path writes exactly what the design says in order, that F7 and a
 * refused stage-2 map tear down what was done, and what teardown, the reset-path stop, the
 * storm guard and the 1 Hz report do, that ATTACH runs once per carrier session and never
 * after the guest started, and that DETACH is refused while a started guest holds the port.
 * Register bases are the repo's j873.c constants; the guest RAM window and the ADT's DART and
 * secondary interrupt numbers are synthetic.
 *
 * Output: "EV ..." lines for events, one JSON line per case.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "adt.h"
#include "hv_usb.h"
#include "hv_usb_dart.h"
#include "j873.h"
#include "utils.h"

/* ---- the fake machine -------------------------------------------------------------------- */

#define XHCI1 0x40a430000UL
#define EVT1  0x40a448000UL
#define DART1 0x40b020000UL
#define DART2 0x40b040000UL
#define XHCI3 0x412430000UL
#define EVT3  0x412448000UL
#define DART3 0x413020000UL
#define DART4 0x413040000UL
#define RAM_LO 0x10100000000UL /* synthetic: 64 MiB of guest RAM */
#define RAM_SZ (64UL << 20)

u32 chip_id = 0x8152, board_id = 0x22;
u64 stub_cntpct = 1000, stub_cntfrq = 24000000;

struct reg {
    u64 addr;
    u32 val;
};
static struct reg regs[1024];
static int nregs;
static int writes; /* device register writes (MMIO, AIC, TLBI) since the last mark */
static u64 tcr_bad_dart; /* this DART instance keeps TCR bit 3 clear (F7) */
static bool tcr_ignores_four_level, tlbi_fails, allow_fails, map_fails, reserve_fails;
static bool port_up[4] = {false, true, false, true};
static u64 heap_limit = 0x200000000UL, heap_high = 0x100000000UL;
static bool ram_set = true;
static bool guest_entered;

void hv_usb_host_new_session(void);

static void ev(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("EV ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static struct reg *find(u64 addr)
{
    for (int i = 0; i < nregs; i++)
        if (regs[i].addr == addr)
            return &regs[i];
    return NULL;
}

static void set(u64 addr, u32 val)
{
    struct reg *r = find(addr);
    if (!r) {
        if (nregs >= (int)(sizeof(regs) / sizeof(regs[0])))
            abort();
        r = &regs[nregs++];
        r->addr = addr;
    }
    r->val = val;
}

static u32 get(u64 addr)
{
    struct reg *r = find(addr);
    return r ? r->val : 0;
}

u32 read32(u64 addr)
{
    return get(addr);
}

static bool in_dart(u64 addr, u64 *base)
{
    const u64 darts[] = {DART1, DART2, DART3, DART4};
    for (unsigned int i = 0; i < 4; i++)
        if (addr >= darts[i] && addr < darts[i] + 0x20000) {
            *base = darts[i];
            return true;
        }
    return false;
}

void write32(u64 addr, u32 val)
{
    u64 d;
    writes++;
    ev("W 0x%lx 0x%x", addr, val);
    if (in_dart(addr, &d)) {
        u64 off = addr - d;
        if (off == 0xc00) {
            set(addr, get(addr) | val);
            return;
        }
        if (off == 0xc20) {
            set(d + 0xc00, get(d + 0xc00) & ~val);
            return;
        }
        if (off == 0x4000 || off == 0x100) { /* exception bitmap, ERROR: W1C */
            set(addr, get(addr) & ~val);
            return;
        }
        if (off >= 0x1000 && off < 0x1400 && (tcr_ignores_four_level || d == tcr_bad_dart)) {
            set(addr, val & ~8u);
            return;
        }
    }
    if ((addr == XHCI1 + 0x20 || addr == XHCI3 + 0x20) && !(val & 1))
        set(addr + 4, get(addr + 4) | 1); /* RS = 0: the controller halts (HCH) */
    set(addr, val);
}

void mask32(u64 addr, u32 clear, u32 setbits)
{
    write32(addr, (read32(addr) & ~clear) | setbits);
}

int poll32(u64 addr, u32 mask, u32 target, u32 timeout)
{
    (void)timeout;
    return (read32(addr) & mask) == target ? 0 : -1;
}

void aic_set_mask(int irq, bool masked)
{
    writes++;
    ev("AIC %d %s", irq, masked ? "mask" : "unmask");
}

u32 aic_irq_config(int irq)
{
    return 0x1000 | (u32)irq;
}

int dart_gen3_tlbi_adt(const char *path, int instance, int device)
{
    writes++;
    ev("TLBI %s %d %d", path, instance, device);
    return tlbi_fails ? -1 : 0;
}

void hv_aic_alias_t8152_usb(u32 mask)
{
    ev("ALIAS 0x%x", mask);
}

int hv_map_hw(u64 from, u64 to, u64 size)
{
    ev("MAP 0x%lx 0x%lx 0x%lx", from, to, size);
    return map_fails ? -1 : 0;
}

int hv_unmap(u64 from, u64 size)
{
    ev("UNMAP 0x%lx 0x%lx", from, size);
    return 0;
}

bool hv_windows_guest_entered(void)
{
    return guest_entered;
}

bool hv_t8152_guest_ram(u64 *base, u64 *size)
{
    *base = RAM_LO;
    *size = RAM_SZ;
    return ram_set;
}

int hv_t8152_usb_allow(int port, u64 xhci)
{
    ev("ALLOW %d 0x%lx", port, xhci);
    return allow_fails ? -1 : 0;
}

void hv_t8152_usb_revoke(int port)
{
    ev("REVOKE %d", port);
}

int hv_t8152_reserve_carrier(u64 start, u64 end, const char *name)
{
    ev("RESERVE 0x%lx %s", end - start, name);
    return reserve_fails ? -1 : 0;
}

u64 hv_t8152_unowned_irqs(void)
{
    return 7;
}

uintptr_t heapblock_limit(void)
{
    return heap_limit;
}

uintptr_t heapblock_high_water(void)
{
    return heap_high;
}

/* A pool in the binary's data, so that table "physical" addresses stay below 2^42. */
static u8 pool[64 * 16384] __attribute__((aligned(16384)));
static size_t pool_used;
static int live_allocs;
void *stub_memalign(size_t align, size_t size)
{
    size_t at = (pool_used + align - 1) & ~(align - 1);
    if (at + size > sizeof(pool))
        return NULL;
    pool_used = at + size;
    live_allocs++;
    ev("ALLOC 0x%zx", size);
    return pool + at;
}

void stub_free(void *ptr)
{
    if (!(ptr >= (void *)pool && ptr < (void *)(pool + sizeof(pool))))
        abort();
    live_allocs--;
    ev("FREE");
    if (!live_allocs)
        pool_used = 0;
}

void dc_cvac_range(void *addr, size_t length)
{
    (void)addr;
    (void)length;
}

int j804_usb_port_state(int port, struct j804_usb_port *out)
{
    if (port != 1 && port != 3)
        return -1;
    out->dwc = port == 1 ? XHCI1 : XHCI3;
    out->evt = port == 1 ? EVT1 : EVT3;
    out->dart0 = port == 1 ? DART1 : DART3;
    out->dart1 = port == 1 ? DART2 : DART4;
    out->name = port == 1 ? "usb-drd1" : "usb-drd3";
    out->up = port_up[port];
    return 0;
}

/* Fake ADT: four nodes. */
enum { N_DRD1 = 1, N_DART1, N_DRD3, N_DART3, N_APERTURE };
void *adt = (void *)1;
/* 1692/1834 are the carrier's contract lines; the rest are synthetic. */
static u32 drd1_irqs[] = {1692, 1700}, drd3_irqs[] = {1834}, dart1_irqs[] = {1600},
           dart3_irqs[] = {1800};
static u64 drd1_reg0 = XHCI1;

int adt_path_offset_trace(const void *a, const char *path, int *offsets)
{
    (void)a;
    int node = !strcmp(path, "/arm-io/usb-drd1")    ? N_DRD1
               : !strcmp(path, "/arm-io/dart-usb1") ? N_DART1
               : !strcmp(path, "/arm-io/usb-drd3")  ? N_DRD3
               : !strcmp(path, "/arm-io/dart-usb3") ? N_DART3
                                                    : -1;
    if (node < 0)
        return -1;
    offsets[0] = 0;
    offsets[1] = node;
    offsets[2] = 0;
    return node;
}

int adt_get_reg(const void *a, int *path, const char *prop, int idx, u64 *addr, u64 *size)
{
    (void)a;
    (void)prop;
    switch (path[1]) {
        case N_DRD1:
            if (idx)
                return -1;
            *addr = drd1_reg0;
            *size = 0x11800;
            return 0;
        case N_DRD3:
            if (idx)
                return -1;
            *addr = XHCI3;
            *size = 0x11800;
            return 0;
        case N_DART1:
        case N_DART3:
            if (idx > 1)
                return -1;
            *addr = path[1] == N_DART1 ? (idx ? DART2 : DART1) : (idx ? DART4 : DART3);
            *size = 0x20000;
            return 0;
    }
    return -1;
}

const void *adt_getprop(const void *a, int node, const char *name, u32 *len)
{
    (void)a;
    if (strcmp(name, "interrupts"))
        return NULL;
    switch (node) {
        case N_DRD1:
            *len = sizeof(drd1_irqs);
            return drd1_irqs;
        case N_DRD3:
            *len = sizeof(drd3_irqs);
            return drd3_irqs;
        case N_DART1:
            *len = sizeof(dart1_irqs);
            return dart1_irqs;
        case N_DART3:
            *len = sizeof(dart3_irqs);
            return dart3_irqs;
    }
    return NULL;
}

bool adt_is_compatible(const void *a, int node, const char *compat)
{
    (void)a;
    return (node == N_DART1 || node == N_DART3) && !strcmp(compat, "dart,gen3");
}

/* ---- machine states ---------------------------------------------------------------------- */

static void reset_machine(void)
{
    nregs = 0;
    tcr_bad_dart = 0;
    tcr_ignores_four_level = tlbi_fails = allow_fails = map_fails = reserve_fails = false;
    port_up[1] = port_up[3] = true;
    heap_limit = 0x200000000UL;
    heap_high = 0x100000000UL;
    ram_set = true;
    guest_entered = false;
    hv_usb_host_new_session(); /* each scenario is a fresh carrier session */
    drd1_reg0 = XHCI1;
    drd1_irqs[0] = 1692;
    const u64 x[] = {XHCI1, XHCI3}, e[] = {EVT1, EVT3};
    for (int i = 0; i < 2; i++) {
        set(e[i] + 0x20, 0x55);
        /* xHCI 1.2 capability block, the layout dwc3 shows in Linux's debugfs */
        set(x[i] + 0x00, 0x01100020);
        set(x[i] + 0x04, 0x0200047f);
        set(x[i] + 0x08, 0x1c0000f1);
        set(x[i] + 0x10, 0x0238ffcd);
        set(x[i] + 0x14, 0x4e0);
        set(x[i] + 0x18, 0x440);
        set(x[i] + 0x8e0, 0x01000801); /* USBLEGSUP, next +0x20 */
        set(x[i] + 0x900, 0x02000402); /* protocol USB 2.0, next +0x10 */
        set(x[i] + 0x910, 0x03100002); /* protocol USB 3.1, last */
        set(x[i] + 0x20, 0);           /* USBCMD */
        set(x[i] + 0x24, 0x1);         /* USBSTS: HCH */
        set(x[i] + 0x420, 0x2a0);
        set(x[i] + 0x430, 0x2a0);
        set(x[i] + 0xc100, 0x1);
        set(x[i] + 0xc104, 0x6f00);
        set(x[i] + 0xc110, 0x30c11006);
        set(x[i] + 0xc11c, 0x81001908);
        set(x[i] + 0xc12c, 0x0a416802);
        set(x[i] + 0xc200, 0x00102500);
        set(x[i] + 0xc2c0, 0x01001102);
    }
    const u64 d[] = {DART1, DART2, DART3, DART4};
    for (int i = 0; i < 4; i++) {
        set(d[i] + 0x000, 0x0e000000);
        set(d[i] + 0x008, (42u << 24) | (42u << 16) | (3u << 8)); /* PA/VA 42, v3.0 */
        set(d[i] + 0x00c, 0x10);
        set(d[i] + 0x1000, 1);
        set(d[i] + 0x1004, 1);
        set(d[i] + 0x4604, 2);
    }
}

static int attach(u64 mask)
{
    writes = 0;
    return (int)(s64)hv_usb_proxy(HV_USB_OP_ATTACH, mask, 0);
}

static int failures;
#define CHECK(cond)                                                                            \
    do {                                                                                       \
        if (!(cond)) {                                                                         \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);           \
            failures++;                                                                        \
        }                                                                                      \
    } while (0)

static void refuse(const char *name, u64 mask, int want)
{
    printf("CASE %s\n", name);
    int ret = attach(mask);
    CHECK(ret == want);
    CHECK(writes == 0);
    CHECK(live_allocs == 0);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == 0);
    printf("{\"case\":\"%s\",\"ret\":%d,\"writes\":%d,\"result\":\"%s\"}\n", name, ret, writes,
           ret == want && !writes ? "passed" : "failed");
}

int main(void)
{
    /* Not configured: BIT(9) not given to hv_windows_configure. */
    reset_machine();
    refuse("not_configured", HV_USB_PORT_DRD1, HV_USB_E_STATE);
    hv_usb_enable();

    refuse("drd0", BIT(0), HV_USB_E_PORTS);
    refuse("drd0_and_drd1", BIT(0) | HV_USB_PORT_DRD1, HV_USB_E_PORTS);
    refuse("drd2", BIT(2), HV_USB_E_PORTS);
    refuse("empty", 0, HV_USB_E_PORTS);
    refuse("high_bits", HV_USB_PORT_DRD1 | (1UL << 40), HV_USB_E_PORTS);
    board_id = 0x24;
    refuse("not_j804", HV_USB_PORT_DRD1, HV_USB_E_STATE);
    board_id = 0x22;
    ram_set = false;
    refuse("no_guest_ram", HV_USB_PORT_DRD1, HV_USB_E_STATE);
    ram_set = true;

    /* F1 */
    port_up[1] = false;
    refuse("f1_not_up", HV_USB_PORT_DRD1, HV_USB_E_PORT_UP);
    reset_machine();
    set(EVT1 + 0x20, 0);
    refuse("f1_no_utmi", HV_USB_PORT_DRD1, HV_USB_E_PORT_UP);
    reset_machine();
    set(XHCI1 + 0xc110, 0x30c12006);
    refuse("f1_gctl_device", HV_USB_PORT_DRD1, HV_USB_E_PORT_UP);
    /* ADT disagreements */
    reset_machine();
    drd1_reg0 = XHCI3;
    refuse("adt_wrong_controller", HV_USB_PORT_DRD1, HV_USB_E_ADT);
    reset_machine();
    drd1_irqs[0] = 1701;
    refuse("adt_wrong_line", HV_USB_PORT_DRD1, HV_USB_E_ADT);
    /* The guest session has started: ATTACH from the in-session proxy writes nothing. */
    reset_machine();
    guest_entered = true;
    refuse("guest_entered", HV_USB_PORT_DRD1, HV_USB_E_STATE);
    /* F2 */
    reset_machine();
    set(DART1 + 0x200, 1);
    refuse("f2_protected", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART2 + 0x208, 1);
    refuse("f2_locked", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART2 + 0xc00, 0x2);
    refuse("f2_streams_live", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART1 + 0x1404, 0x1);
    refuse("f2_ttbr_valid", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART1 + 0x1000, 0x9);
    refuse("f2_tcr_live", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART1 + 0x1000, 0x2); /* bypass */
    refuse("f2_tcr_bypass", HV_USB_PORT_DRD1, HV_USB_E_DART);
    /* F3 */
    reset_machine();
    set(DART1 + 0x000, 0x0c000000);
    refuse("f3_page_size", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART2 + 0x008, 0x24240300);
    refuse("f3_params3", HV_USB_PORT_DRD1, HV_USB_E_DART);
    reset_machine();
    set(DART1 + 0x00c, 1);
    refuse("f3_one_sid", HV_USB_PORT_DRD1, HV_USB_E_DART);
    /* F4 */
    reset_machine();
    set(XHCI1 + 0x20, 1);
    refuse("f4_running", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x24, 0);
    refuse("f4_not_halted", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x24, 0x801);
    refuse("f4_cnr", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x910, 0x0000000a);
    refuse("f4_dbc", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x18, 0x7fc0); /* 4 interrupters end at 0x8060 */
    refuse("f4_runtime_outside", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x14, 0x7f00);
    refuse("f4_doorbells_outside", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x910, 0x00000102); /* next +4, forever */
    for (u64 o = 0x914; o < 0x914 + 4 * 40; o += 4)
        set(XHCI1 + o, 0x00000102);
    refuse("f4_chain_loop", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    reset_machine();
    set(XHCI1 + 0x10, 0x1ffdffcd); /* xECP 0x7ff4: the capability runs past the window */
    refuse("f4_cap_outside", HV_USB_PORT_DRD1, HV_USB_E_XHCI);
    /* drd3 fails probe after drd1 passed: still nothing written */
    reset_machine();
    set(DART3 + 0x200, 1);
    refuse("drd3_probe_fails_after_drd1", HV_USB_PORTS, HV_USB_E_DART);
    /* F5 */
    reset_machine();
    heap_limit = heap_high + 0x10000;
    refuse("f5_no_heap_room", HV_USB_PORT_DRD1, HV_USB_E_TABLES);
    reset_machine();
    reserve_fails = true;
    refuse("f5_placement", HV_USB_PORT_DRD1, HV_USB_E_TABLES);

    /* F7: TCR 0x9 does not stick -> torn down, tables freed, no stage-2 change. */
    reset_machine();
    tcr_ignores_four_level = true;
    printf("CASE f7_readback\n");
    int ret = attach(HV_USB_PORT_DRD1);
    CHECK(ret == HV_USB_E_COMMIT && live_allocs == 0);
    CHECK(get(DART1 + 0xc00) == 0 && get(DART2 + 0xc00) == 0 && get(DART1 + 0x1400) == 0);
    printf("{\"case\":\"f7_readback\",\"ret\":%d,\"result\":\"%s\"}\n", ret,
           ret == HV_USB_E_COMMIT ? "passed" : "failed");
    /* TLBI failure and a refused stage-2 map: torn down as well. */
    reset_machine();
    tlbi_fails = true;
    printf("CASE tlbi_fails\n");
    ret = attach(HV_USB_PORT_DRD1);
    CHECK(ret == HV_USB_E_COMMIT && live_allocs == 0);
    printf("{\"case\":\"tlbi_fails\",\"ret\":%d}\n", ret);
    reset_machine();
    allow_fails = true;
    printf("CASE allow_fails\n");
    ret = attach(HV_USB_PORT_DRD1);
    CHECK(ret == HV_USB_E_COMMIT && live_allocs == 0 && get(DART1 + 0xc00) == 0);
    printf("{\"case\":\"allow_fails\",\"ret\":%d}\n", ret);
    reset_machine();
    map_fails = true;
    printf("CASE map_fails\n");
    ret = attach(HV_USB_PORT_DRD1);
    CHECK(ret == HV_USB_E_COMMIT && live_allocs == 0 && get(DART2 + 0xc00) == 0);
    printf("{\"case\":\"map_fails\",\"ret\":%d}\n", ret);

    /* Success, drd1 only, with GUSB2PHYCFG.SUSPHY set. */
    reset_machine();
    set(XHCI1 + 0xc200, 0x00102540);
    printf("CASE success\n");
    ret = attach(HV_USB_PORT_DRD1);
    CHECK(ret == 0 && live_allocs == 1);
    CHECK(get(DART1 + 0xc00) == 0x3 && get(DART2 + 0xc00) == 0x2);
    CHECK(get(DART1 + 0x1000) == 9 && get(DART1 + 0x1004) == 9 && get(DART2 + 0x1004) == 9);
    CHECK(get(DART2 + 0x1000) == 1); /* instance 1 SID 0 untouched */
    CHECK(get(XHCI1 + 0xc200) == 0x00102500 && get(XHCI1 + 0xc2c0) == 0x01001102);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == HV_USB_PORT_DRD1);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_USBSTS) == 1);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_PORTSC1) == 0x2a0);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_GCTL) == 0x30c11006);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_UNOWNED) == 7);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 2, HV_USB_S_USBSTS) == HV_USB_ERROR);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, 99) == HV_USB_ERROR);
    CHECK(hv_usb_proxy(9, 0, 0) == HV_USB_ERROR);
    /* The DART window for the launcher's aura,windows-usb-v1 handshake. */
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_BASE) == RAM_LO);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_TOP) == RAM_LO + RAM_SZ);
    printf("{\"case\":\"success\",\"ret\":%d,\"writes\":%d,\"dma_base\":%lu,"
           "\"dma_top\":%lu}\n",
           ret, writes, hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_BASE),
           hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_TOP));
    printf("CASE again\n");
    CHECK(attach(HV_USB_PORT_DRD1) == HV_USB_E_STATE && writes == 0);

    /* The guest runs.  A DETACH from the in-session proxy is refused and changes nothing. */
    guest_entered = true;
    printf("CASE detach_in_session\n");
    writes = 0;
    CHECK(hv_usb_proxy(HV_USB_OP_DETACH, 0, 0) == HV_USB_ERROR);
    CHECK(writes == 0 && live_allocs == 1);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == HV_USB_PORT_DRD1);
    CHECK(attach(HV_USB_PORT_DRD1) == HV_USB_E_STATE && writes == 0);
    printf("{\"case\":\"detach_in_session\",\"writes\":%d}\n", writes);

    /* Interrupts, a port change, a DART fault, a dwc3 drift, then a storm. */
    printf("CASE report\n");
    set(XHCI1 + 0x20, 1); /* the guest started the controller */
    set(XHCI1 + 0x24, 0x8);
    set(XHCI1 + 0x420, 0x206e1);
    for (int i = 0; i < 1000; i++)
        hv_usb_note_irq(1692);
    hv_usb_note_irq(1834); /* not attached: counted on drd3, never reported */
    set(DART2 + 0x4000, 0x2);
    set(DART2 + 0x100, 0x80000001); /* ERROR latched with the exception */
    set(DART1 + 0x100, 0x4);        /* ERROR alone on the other instance */
    set(DART2 + 0x8040, 0xdead0001);
    set(XHCI1 + 0xc110, 0x30c11007);
    stub_cntpct += stub_cntfrq;
    hv_usb_report();
    CHECK(get(DART2 + 0x4000) == 0); /* W1C */
    CHECK(get(DART2 + 0x100) == 0 && get(DART1 + 0x100) == 0); /* ERROR acknowledged too */
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_IRQS) == 1000);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_DART1_EXC) == 0x2);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_STORM) == 0);
    printf("CASE storm\n");
    for (int i = 0; i < 2 * (int)HV_USB_STORM_PER_SEC + 10; i++)
        hv_usb_note_irq(1692);
    stub_cntpct += stub_cntfrq;
    hv_usb_report();
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_STORM) == 1);
    hv_usb_fatal_report();

    printf("CASE teardown\n");
    writes = 0;
    hv_usb_teardown("guest SYSTEM_OFF");
    CHECK(live_allocs == 0);
    CHECK(get(DART1 + 0xc00) == 0 && get(DART2 + 0xc00) == 0);
    CHECK(get(DART1 + 0x1400) == 0 && get(DART1 + 0x1404) == 0 && get(DART2 + 0x1404) == 0);
    CHECK(get(DART1 + 0x1000) == 1 && get(DART1 + 0x1004) == 1 && get(DART2 + 0x1004) == 1);
    CHECK((get(XHCI1 + 0x20) & 1) == 0 && (get(XHCI1 + 0x24) & 1) == 1);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == 0);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_PORTSC1) == 0x206e1); /* recorded */
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 1, HV_USB_S_IRQS) == 2 * HV_USB_STORM_PER_SEC + 1010);
    int before = writes;
    hv_usb_teardown("again");
    hv_usb_emergency_stop();
    CHECK(writes == before); /* idempotent */
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_BASE) == HV_USB_ERROR);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_DMA_TOP) == HV_USB_ERROR);
    /* After the session ended (entered stays set) DETACH is an idempotent no-op again. */
    CHECK(hv_usb_proxy(HV_USB_OP_DETACH, 0, 0) == 0 && writes == before);
    printf("{\"case\":\"teardown\",\"writes\":%d}\n", writes);

    /* Same carrier session, nothing attached any more: a second ATTACH is still refused. */
    guest_entered = false;
    refuse("again_after_teardown", HV_USB_PORT_DRD1, HV_USB_E_STATE);

    /* Both ports attach and detach together. */
    reset_machine();
    printf("CASE both_ports\n");
    ret = attach(HV_USB_PORTS);
    CHECK(ret == 0 && live_allocs == 1);
    CHECK(get(DART3 + 0xc00) == 0x3 && get(DART4 + 0xc00) == 0x2);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == HV_USB_PORTS);
    CHECK(hv_usb_proxy(HV_USB_OP_DETACH, 0, 0) == 0);
    CHECK(live_allocs == 0 && get(DART3 + 0xc00) == 0 && get(DART1 + 0xc00) == 0);
    printf("{\"case\":\"both_ports\",\"ret\":%d}\n", ret);

    /* Both ports; drd3's second DART refuses the four-level walk: drd1 is torn down too. */
    reset_machine();
    printf("CASE both_ports_commit_fails\n");
    tcr_bad_dart = DART4;
    ret = attach(HV_USB_PORTS);
    CHECK(ret == HV_USB_E_COMMIT && live_allocs == 0);
    CHECK(get(DART1 + 0xc00) == 0 && get(DART2 + 0xc00) == 0 && get(DART3 + 0xc00) == 0);
    CHECK(hv_usb_proxy(HV_USB_OP_STATUS, 0, HV_USB_S_ATTACHED) == 0);
    printf("{\"case\":\"both_ports_commit_fails\",\"ret\":%d}\n", ret);

    /* The reset path: no stage-2 or heap work, but the controller halts and streams drop. */
    reset_machine();
    printf("CASE emergency\n");
    CHECK(attach(HV_USB_PORT_DRD1) == 0);
    set(XHCI1 + 0x20, 1);
    set(XHCI1 + 0x24, 0);
    hv_usb_emergency_stop();
    CHECK(get(DART1 + 0xc00) == 0 && get(DART2 + 0xc00) == 0 && (get(XHCI1 + 0x24) & 1));
    CHECK(live_allocs == 1); /* no free on the reset path */
    hv_usb_teardown("after the emergency stop");
    printf("{\"case\":\"emergency\"}\n");

    printf("{\"case\":\"done\",\"result\":\"%s\",\"failures\":%d}\n",
           failures ? "failed" : "passed", failures);
    return failures ? 1 : 0;
}
