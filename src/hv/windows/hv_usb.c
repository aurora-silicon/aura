/* SPDX-License-Identifier: MIT */

/*
 * D4 carrier glue: usb-drd1 (and optionally usb-drd3) xHCI passthrough for the J804 Windows
 * guest.  See hv_usb.h for the contract and windows-m6/D4-USB-DESIGN.md for the reasoning.
 * Written on this branch.  Register facts are cited, not copied: the xHCI capability and
 * operational layout from the public xHCI 1.2 specification (sections 5.3-5.6, 7.0); the
 * dwc3 global offsets and the xHCI window from Linux drivers/usb/dwc3/core.h (GPL-2.0, cited);
 * the Gen3 DART registers from Linux drivers/iommu/apple-dart.c (T8110 layout) and our own
 * linux-j804 patches apple-dart-t8152-gen3-{ias,tlbi,irqdump}.py.  The table format lives in
 * hv_usb_dart.c.
 */

#include "hv_usb.h"
#include "adt.h"
#include "dart.h"
#include "heapblock.h"
#include "hv.h"
#include "hv_aic_alias.h"
#include "hv_t8152.h"
#include "hv_usb_dart.h"
#include "j873.h"
#include "malloc.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "windows_compat.h"

#define J804_BOARD_ID 0x22

/* usb-drd0, the proxy's CDC link: never touched, whatever an ADT or a caller says. */
#define DRD0_XHCI  0x402430000UL
#define DRD0_DART0 0x403020000UL
#define DRD0_DART1 0x403040000UL

/* Gen3 DART (T8110 layout; apple-dart.c, aura dart.c). */
#define DART_PARAMS1         0x000
#define DART_PARAMS3         0x008
#define DART_PARAMS4         0x00c
#define DART_ERROR           0x100
#define DART_PROTECT         0x200
#define DART_PROTECT_LOCK    0x208
#define DART_ENABLE_STREAMS  0xc00
#define DART_DISABLE_STREAMS 0xc20
#define DART_TCR(sid)        (0x1000 + 4 * (sid))
#define DART_TTBR(sid)       (0x1400 + 4 * (sid))
#define DART_EXCEPTION       0x4000 /* per-SID bitmap, W1C (irqdump patch) */
#define DART_EXC_RECORD(sid) (0x8000 + 0x40 * (sid))
#define DART_ROUTE           0x4604
#define DART_PAGE_LOG2       14
/*
 * PARAMS3 of the Gen3 USB DART: PA width [29:24] 42, VA width [23:16] 42, version [15:8] 3.0
 * (field layout from Linux apple-dart.c DART_T8110_PARAMS3_*; the 42-bit walk from our
 * linux-j804 apple-dart-t8152-gen3-ias.py), i.e. 0x2a2a0300.
 */
#define DART_PARAMS3_USB     ((42u << 24) | (42u << 16) | (3u << 8))
#define DART_MIN_REGS        (DART_ROUTE + 4)
#define DART_RECORD_REGS     (DART_EXC_RECORD(16))

/* xHCI 1.2 capability and operational registers (section 5.3, 5.4). */
#define XHCI_CAPLENGTH  0x00
#define XHCI_HCSPARAMS1 0x04
#define XHCI_HCCPARAMS1 0x10
#define XHCI_DBOFF      0x14
#define XHCI_RTSOFF     0x18
#define XHCI_USBCMD     0x00
#define XHCI_USBSTS     0x04
#define XHCI_PORTSC(n)  (0x400 + 0x10 * ((n) - 1))
#define USBCMD_RS       BIT(0)
#define USBSTS_HCH      BIT(0)
#define USBSTS_CNR      BIT(11)
#define XECP_ID_DBC     10
#define XECP_MAX        32
#define HALT_US         20000

/* dwc3 globals (core.h), read and logged; the guest cannot reach them. */
#define DWC3_GCTL              0xc110
#define DWC3_GCTL_PRTCAPDIR    0x3000u
#define DWC3_GCTL_HOST         0x1000u
#define DWC3_GUSB2PHYCFG0      0xc200
#define DWC3_GUSB2PHYCFG_SUSP  BIT(6)
#define DWC3_GUSB3PIPECTL0     0xc2c0
#define DWC3_GUSB3PIPECTL_SUSP BIT(17)
#define UTMI_EVENTS            0x20 /* evt block: nonzero once the eUSB2 PHY clocks (j873.c) */

static const struct {
    u32 off;
    const char *name;
} dwc_regs[] = {
    {0xc100, "GSBUSCFG0"}, {0xc104, "GSBUSCFG1"},    {DWC3_GCTL, "GCTL"},
    {0xc11c, "GUCTL1"},    {0xc12c, "GUCTL"},        {DWC3_GUSB2PHYCFG0, "GUSB2PHYCFG0"},
    {DWC3_GUSB3PIPECTL0, "GUSB3PIPECTL0"},
};
#define N_DWC (sizeof(dwc_regs) / sizeof(dwc_regs[0]))
enum { DWC_GCTL = 2, DWC_USB2PHY = 5, DWC_USB3PIPE = 6 }; /* indices into dwc_regs */

/* The streams the dwc3 uses (DT iommus = <&usb1_dart0 0>, <&usb1_dart0 1>, <&usb1_dart1 1>). */
static const struct {
    u8 inst, sid;
} streams[] = {{0, 0}, {0, 1}, {1, 1}};
#define N_STREAMS (sizeof(streams) / sizeof(streams[0]))
static const u32 inst_streams[2] = {BIT(0) | BIT(1), BIT(1)}; /* never Linux's 0xffffffff */

struct usb_port {
    int num;
    u32 bit;
    const char *drd_path, *dart_path;
    u32 irq, gsiv;
    /* probe */
    const char *name;
    u64 xhci, evt, dart[2], dart_size[2];
    u32 route[2], dart_irq, op, nports, aic_cfg;
    u32 dwc[N_DWC];
    /* commit */
    bool streams_on, allowed, mapped, owned;
    bool attached, storm;
    /* report */
    u64 irqs, irqs_seen, irq_tick, report_tick;
    u32 usbsts, portsc[2], exc_seen[2];
    /* fatal report: CNTPCT of the last tick read of the controller and of the last line */
    u64 read_at, irq_at;
};

static struct usb_port ports[] = {
    {.num = 1,
     .bit = HV_USB_PORT_DRD1,
     .drd_path = "/arm-io/usb-drd1",
     .dart_path = "/arm-io/dart-usb1",
     .irq = 1692,
     .gsiv = HV_USB_GSIV_DRD1},
    {.num = 3,
     .bit = HV_USB_PORT_DRD3,
     .drd_path = "/arm-io/usb-drd3",
     .dart_path = "/arm-io/dart-usb3",
     .irq = 1834,
     .gsiv = HV_USB_GSIV_DRD3},
};
#define N_PORTS (sizeof(ports) / sizeof(ports[0]))

static bool usb_enabled;
/* An ATTACH reached the commit (tables, deny entry) this carrier session: no second one. */
static bool session_used;
static u32 attached_mask;
/* The DART identity window of the current attach, [dma_base, dma_top); 0 when detached. */
static u64 dma_base, dma_top;
static u64 *tables;
static u32 n_tables;
static u64 root_pa;
static u32 ttbr_word;
static DECLARE_SPINLOCK(usb_lock);
/* The TLBI aperture is shared by every Gen3 DART; used here only at attach and teardown. */
static DECLARE_SPINLOCK(usb_tlbi_lock);

void hv_usb_enable(void)
{
    usb_enabled = true;
}

bool hv_usb_enabled(void)
{
    return usb_enabled;
}

static struct usb_port *port_by_num(u64 num)
{
    for (unsigned int i = 0; i < N_PORTS; i++)
        if ((u64)ports[i].num == num)
            return &ports[i];
    return NULL;
}

static bool never_touch(u64 addr)
{
    return addr == DRD0_XHCI || addr == DRD0_DART0 || addr == DRD0_DART1;
}

static int adt_first_irq(const char *path, u32 *irq)
{
    int offsets[8];
    int node = adt_path_offset_trace(adt, path, offsets);
    u32 len;
    if (node < 0)
        return -1;
    const u32 *v = adt_getprop(adt, node, "interrupts", &len);
    if (!v || len < sizeof(u32))
        return -1;
    memcpy(irq, v, sizeof(*irq));
    return 0;
}

/* ---- probe: read only ------------------------------------------------------------------ */

static int probe_xhci(struct usb_port *p)
{
    u64 x = p->xhci;
    u32 cap = read32(x + XHCI_CAPLENGTH);
    u32 caplen = cap & 0xff;
    u32 hcs1 = read32(x + XHCI_HCSPARAMS1);
    u32 hcc1 = read32(x + XHCI_HCCPARAMS1);
    u32 dboff = read32(x + XHCI_DBOFF) & ~3u;
    u32 rtsoff = read32(x + XHCI_RTSOFF) & ~0x1fu;
    u32 slots = hcs1 & 0xff, intrs = (hcs1 >> 8) & 0x7ff, nports = hcs1 >> 24;

    if (caplen < 0x20 || !nports || caplen + 0x400 + 0x10 * nports > HV_USB_XHCI_SIZE ||
        rtsoff + 0x20 + 0x20 * intrs > HV_USB_XHCI_SIZE ||
        dboff + 4 * (slots + 1) > HV_USB_XHCI_SIZE) {
        printf("HV: USB %s: xHCI layout outside the 0x8000 window (CAPLENGTH 0x%x, HCSPARAMS1 "
               "0x%x, RTSOFF 0x%x, DBOFF 0x%x); refusing\n",
               p->name, caplen, hcs1, rtsoff, dboff);
        return HV_USB_E_XHCI;
    }
    u32 xecp = (hcc1 >> 16) << 2, n = 0;
    while (xecp) {
        if (xecp + 0x10 > HV_USB_XHCI_SIZE || ++n > XECP_MAX) {
            printf("HV: USB %s: extended capability at 0x%x (#%u) outside the window or the "
                   "chain is too long; refusing\n",
                   p->name, xecp, n);
            return HV_USB_E_XHCI;
        }
        u32 word = read32(x + xecp);
        if ((word & 0xff) == XECP_ID_DBC) {
            printf("HV: USB %s: xHCI Debug Capability at 0x%x; refusing\n", p->name, xecp);
            return HV_USB_E_XHCI;
        }
        u32 next = (word >> 8) & 0xff;
        xecp = next ? xecp + (next << 2) : 0;
    }
    u32 cmd = read32(x + caplen + XHCI_USBCMD), sts = read32(x + caplen + XHCI_USBSTS);
    if ((cmd & USBCMD_RS) || !(sts & USBSTS_HCH) || (sts & USBSTS_CNR)) {
        printf("HV: USB %s: xHCI is not halted and ready (USBCMD 0x%x USBSTS 0x%x); refusing\n",
               p->name, cmd, sts);
        return HV_USB_E_XHCI;
    }
    p->op = caplen;
    p->nports = nports;
    printf("HV: USB %s: xHCI 0x%lx HCIVERSION 0x%x HCSPARAMS1 0x%x (%u slots, %u interrupters, "
           "%u ports) HCCPARAMS1 0x%x RTSOFF 0x%x DBOFF 0x%x, %u extended capabilities, halted\n",
           p->name, x, cap >> 16, hcs1, slots, intrs, nports, hcc1, rtsoff, dboff, n);
    return 0;
}

static int probe_dart(struct usb_port *p, unsigned int inst)
{
    u64 d = p->dart[inst];
    u32 params1 = read32(d + DART_PARAMS1), params3 = read32(d + DART_PARAMS3);
    u32 params4 = read32(d + DART_PARAMS4);
    u32 protect = read32(d + DART_PROTECT), lock = read32(d + DART_PROTECT_LOCK);
    u32 enabled = read32(d + DART_ENABLE_STREAMS);

    if (((params1 >> 24) & 0xf) != DART_PAGE_LOG2 || params3 != DART_PARAMS3_USB ||
        (params4 & 0x1ff) < 2) {
        printf("HV: USB %s: DART 0x%lx PARAMS1 0x%x PARAMS3 0x%x PARAMS4 0x%x is not the 16 KiB, "
               "42-bit Gen3 USB DART; refusing\n",
               p->name, d, params1, params3, params4);
        return HV_USB_E_DART;
    }
    if (protect || lock || enabled) {
        printf("HV: USB %s: DART 0x%lx PROTECT 0x%x LOCK 0x%x ENABLE_STREAMS 0x%x: not idle and "
               "unprotected; refusing\n",
               p->name, d, protect, lock, enabled);
        return HV_USB_E_DART;
    }
    for (unsigned int i = 0; i < N_STREAMS; i++) {
        if (streams[i].inst != inst)
            continue;
        u32 sid = streams[i].sid;
        u32 tcr = read32(d + DART_TCR(sid)), ttbr = read32(d + DART_TTBR(sid));
        /* Idle: no valid root, translation off or plain (aura's quiesced value is 0x1). */
        if ((ttbr & 1) || (tcr != 0 && tcr != USB_DART_TCR_IDLE)) {
            printf("HV: USB %s: DART 0x%lx SID %u TCR 0x%x TTBR 0x%x is not an idle stream; "
                   "refusing\n",
                   p->name, d, sid, tcr, ttbr);
            return HV_USB_E_DART;
        }
    }
    p->route[inst] = read32(d + DART_ROUTE) & 0xf;
    return 0;
}

static int probe(struct usb_port *p)
{
    struct j804_usb_port st;
    int offsets[8];
    u64 base, size;

    if (j804_usb_port_state(p->num, &st) || !st.up) {
        printf("HV: USB usb-drd%d is not in host mode as aura's j804_usb_prep() leaves it (AON, "
               "PSD, gate, GCTL.PRTCAPDIR); refusing\n",
               p->num);
        return HV_USB_E_PORT_UP;
    }
    p->name = st.name;
    p->xhci = st.dwc;
    p->evt = st.evt;
    p->dart[0] = st.dart0;
    p->dart[1] = st.dart1;
    if (never_touch(p->xhci) || never_touch(p->dart[0]) || never_touch(p->dart[1])) {
        printf("HV: USB %s: an address is usb-drd0's (the proxy's link); refusing\n", p->name);
        return HV_USB_E_PORTS;
    }
    if (!read32(p->evt + UTMI_EVENTS)) {
        printf("HV: USB %s: no UTMI clock events; the PHY is not running; refusing\n", p->name);
        return HV_USB_E_PORT_UP;
    }

    /* The ADT must describe the same controller, line and DARTs. */
    u32 irq;
    if (adt_path_offset_trace(adt, p->drd_path, offsets) < 0 ||
        adt_get_reg(adt, offsets, "reg", 0, &base, &size) < 0 || base != p->xhci ||
        size < HV_USB_XHCI_SIZE || adt_first_irq(p->drd_path, &irq) || irq != p->irq) {
        printf("HV: USB %s: ADT %s is not the controller at 0x%lx with line %u; refusing\n",
               p->name, p->drd_path, p->xhci, p->irq);
        return HV_USB_E_ADT;
    }
    int node = adt_path_offset_trace(adt, p->dart_path, offsets);
    if (node < 0 || !adt_is_compatible(adt, node, "dart,gen3") ||
        adt_first_irq(p->dart_path, &p->dart_irq)) {
        printf("HV: USB %s: ADT %s is not a Gen3 DART with an interrupt; refusing\n", p->name,
               p->dart_path);
        return HV_USB_E_ADT;
    }
    for (unsigned int inst = 0; inst < 2; inst++) {
        if (adt_get_reg(adt, offsets, "reg", inst, &base, &size) < 0 || base != p->dart[inst] ||
            size < DART_MIN_REGS) {
            printf("HV: USB %s: ADT %s reg %u is not the DART at 0x%lx; refusing\n", p->name,
                   p->dart_path, inst, p->dart[inst]);
            return HV_USB_E_ADT;
        }
        p->dart_size[inst] = size;
    }
    int ret;
    if ((ret = probe_dart(p, 0)) || (ret = probe_dart(p, 1)) || (ret = probe_xhci(p)))
        return ret;

    for (unsigned int i = 0; i < N_DWC; i++)
        p->dwc[i] = read32(p->xhci + dwc_regs[i].off);
    if ((p->dwc[DWC_GCTL] & DWC3_GCTL_PRTCAPDIR) != DWC3_GCTL_HOST) {
        printf("HV: USB %s: GCTL 0x%x is not in host mode; refusing\n", p->name,
               p->dwc[DWC_GCTL]);
        return HV_USB_E_PORT_UP;
    }
    p->aic_cfg = aic_irq_config(p->irq);
    printf("HV: USB %s: dwc3 GSBUSCFG0 0x%x GSBUSCFG1 0x%x GCTL 0x%x GUCTL1 0x%x GUCTL 0x%x "
           "GUSB2PHYCFG0 0x%x GUSB3PIPECTL0 0x%x (Linux host: GCTL 0x30c11006, GUCTL 0x0a416802, "
           "GUCTL1 0x81001908; logged, not applied); AIC %u cfg 0x%x, DART line %u, TLBI routes "
           "%u/%u\n",
           p->name, p->dwc[0], p->dwc[1], p->dwc[2], p->dwc[3], p->dwc[4], p->dwc[5], p->dwc[6],
           p->irq, p->aic_cfg, p->dart_irq, p->route[0], p->route[1]);
    return 0;
}

/* ---- tables ------------------------------------------------------------------------------ */

static void free_tables(void)
{
    if (tables)
        free(tables);
    tables = NULL;
    n_tables = 0;
    root_pa = 0;
}

static int build_tables(u64 start, u64 end)
{
    u32 n;
    if (usb_dart_tables_needed(start, end, &n)) {
        printf("HV: USB: guest RAM [0x%lx, 0x%lx) cannot be mapped (more than %u tables, or not "
               "16 KiB aligned below 2^42); refusing\n",
               start, end, USB_DART_MAX_TABLES);
        return HV_USB_E_TABLES;
    }
    u64 bytes = (u64)n * USB_DART_PAGE;
    u64 limit = heapblock_limit(), high = heapblock_high_water();
    if (!limit || limit < high || limit - high < bytes + 2 * USB_DART_PAGE + (1UL << 20)) {
        printf("HV: USB: %lu KiB of DART tables do not fit below aura's heap limit (high water "
               "0x%lx, limit 0x%lx); refusing\n",
               bytes >> 10, high, limit);
        return HV_USB_E_TABLES;
    }
    u64 *t = memalign(USB_DART_PAGE, bytes);
    if (!t) {
        printf("HV: USB: cannot allocate %lu KiB of DART tables; refusing\n", bytes >> 10);
        return HV_USB_E_TABLES;
    }
    u64 pa = (u64)t, root;
    if (usb_dart_build_identity(start, end, t, pa, n, &root) ||
        usb_dart_self_check(t, pa, n, root, start, end) || usb_dart_ttbr(root, &ttbr_word)) {
        printf("HV: USB: the DART table self check failed at 0x%lx; refusing\n", pa);
        free(t);
        return HV_USB_E_TABLES;
    }
    if (hv_t8152_reserve_carrier(pa, pa + bytes, "USB DART tables (D4)")) {
        free(t);
        return HV_USB_E_TABLES;
    }
    dc_cvac_range(t, bytes);
    sysop("dsb sy");
    tables = t;
    n_tables = n;
    root_pa = root;
    printf("HV: USB: DART identity map of [0x%lx, 0x%lx): %u tables (%lu KiB) at 0x%lx, TTBR "
           "0x%x; everything else faults\n",
           start, end, n, bytes >> 10, pa, ttbr_word);
    return 0;
}

/* ---- commit and quiesce ------------------------------------------------------------------- */

static int tlbi_stream(const struct usb_port *p, unsigned int i, bool locked)
{
    if (locked)
        spin_lock(&usb_tlbi_lock);
    int ret = dart_gen3_tlbi_adt(p->dart_path, streams[i].inst, streams[i].sid);
    if (locked)
        spin_unlock(&usb_tlbi_lock);
    return ret;
}

/*
 * Halt the controller, take its streams down and, unless this is the reset path, unmap and deny
 * the window again.  Works on a partly committed port.
 */
static void quiesce_port(struct usb_port *p, bool emergency)
{
    __atomic_store_n(&p->attached, false, __ATOMIC_RELEASE);
    aic_set_mask(p->irq, true);
    if (p->op) {
        u64 op = p->xhci + p->op;
        p->usbsts = read32(op + XHCI_USBSTS);
        for (u32 n = 1; n <= 2 && n <= p->nports; n++)
            p->portsc[n - 1] = read32(op + XHCI_PORTSC(n));
        u32 cmd = read32(op + XHCI_USBCMD);
        if (cmd & USBCMD_RS)
            write32(op + XHCI_USBCMD, cmd & ~USBCMD_RS);
        if (poll32(op + XHCI_USBSTS, USBSTS_HCH, USBSTS_HCH, HALT_US))
            printf("HV: USB %s: xHCI did not halt within 20 ms (USBSTS 0x%x)\n", p->name,
                   read32(op + XHCI_USBSTS));
    }
    if (p->streams_on) {
        for (unsigned int i = 0; i < N_STREAMS; i++) {
            u64 d = p->dart[streams[i].inst];
            u32 sid = streams[i].sid;
            write32(d + DART_DISABLE_STREAMS, BIT(sid));
            write32(d + DART_TTBR(sid), 0);
            write32(d + DART_TCR(sid), USB_DART_TCR_IDLE);
        }
        for (unsigned int i = 0; i < N_STREAMS; i++)
            if (tlbi_stream(p, i, !emergency))
                printf("HV: USB %s: TLBI of DART %u SID %u failed\n", p->name, streams[i].inst,
                       streams[i].sid);
        p->streams_on = false;
    }
    if (emergency)
        return;
    if (p->mapped && hv_unmap(p->xhci, HV_USB_XHCI_SIZE) < 0)
        printf("HV: USB %s: stage-2 unmap of the xHCI failed\n", p->name);
    p->mapped = false;
    if (p->allowed)
        hv_t8152_usb_revoke(p->num);
    p->allowed = false;
}

static int commit(struct usb_port *p)
{
    /* SUSPHY: aura clears both on its setup path only; a suspended PHY would never wake. */
    if (p->dwc[DWC_USB2PHY] & DWC3_GUSB2PHYCFG_SUSP) {
        mask32(p->xhci + DWC3_GUSB2PHYCFG0, DWC3_GUSB2PHYCFG_SUSP, 0);
        p->dwc[DWC_USB2PHY] = read32(p->xhci + DWC3_GUSB2PHYCFG0);
        printf("HV: USB %s: cleared GUSB2PHYCFG0.SUSPHY (now 0x%x)\n", p->name,
               p->dwc[DWC_USB2PHY]);
    }
    if (p->dwc[DWC_USB3PIPE] & DWC3_GUSB3PIPECTL_SUSP) {
        mask32(p->xhci + DWC3_GUSB3PIPECTL0, DWC3_GUSB3PIPECTL_SUSP, 0);
        p->dwc[DWC_USB3PIPE] = read32(p->xhci + DWC3_GUSB3PIPECTL0);
        printf("HV: USB %s: cleared GUSB3PIPECTL0.SUSPHY (now 0x%x)\n", p->name,
               p->dwc[DWC_USB3PIPE]);
    }
    /* Both lines stay masked until the guest enables the published SPI (the DART's never). */
    aic_set_mask(p->irq, true);
    aic_set_mask(p->dart_irq, true);

    p->streams_on = true;
    for (unsigned int i = 0; i < N_STREAMS; i++) {
        u64 d = p->dart[streams[i].inst];
        write32(d + DART_TTBR(streams[i].sid), ttbr_word);
        write32(d + DART_TCR(streams[i].sid), USB_DART_TCR_ON);
    }
    for (unsigned int i = 0; i < N_STREAMS; i++) {
        if (tlbi_stream(p, i, true)) {
            printf("HV: USB %s: TLBI of DART %u SID %u failed; tearing down\n", p->name,
                   streams[i].inst, streams[i].sid);
            return HV_USB_E_COMMIT;
        }
    }
    for (unsigned int inst = 0; inst < 2; inst++)
        write32(p->dart[inst] + DART_ENABLE_STREAMS, inst_streams[inst]);

    for (unsigned int inst = 0; inst < 2; inst++) {
        u32 en = read32(p->dart[inst] + DART_ENABLE_STREAMS);
        bool ok = en == inst_streams[inst];
        for (unsigned int i = 0; i < N_STREAMS; i++) {
            if (streams[i].inst != inst)
                continue;
            u32 sid = streams[i].sid;
            u32 tcr = read32(p->dart[inst] + DART_TCR(sid));
            u32 ttbr = read32(p->dart[inst] + DART_TTBR(sid));
            ok = ok && tcr == USB_DART_TCR_ON && ttbr == ttbr_word;
            printf("HV: USB %s: DART %u (0x%lx) SID %u TCR 0x%x TTBR 0x%x ENABLE 0x%x\n",
                   p->name, inst, p->dart[inst], sid, tcr, ttbr, en);
        }
        if (!ok) {
            printf("HV: USB %s: DART %u did not take the four-level identity map (F7; the "
                   "three-level fallback is a separate experiment); tearing down\n",
                   p->name, inst);
            return HV_USB_E_COMMIT;
        }
    }

    if (hv_t8152_usb_allow(p->num, p->xhci))
        return HV_USB_E_COMMIT;
    p->allowed = true;
    if (hv_map_hw(p->xhci, p->xhci, HV_USB_XHCI_SIZE) < 0) {
        printf("HV: USB %s: stage-2 map of the xHCI refused; tearing down\n", p->name);
        return HV_USB_E_COMMIT;
    }
    p->mapped = true;
    return 0;
}

static void reset_port_state(struct usb_port *p)
{
    p->op = p->nports = 0;
    p->streams_on = p->allowed = p->mapped = p->owned = false;
    p->attached = p->storm = false;
    p->irqs = p->irqs_seen = p->irq_tick = p->report_tick = 0;
    p->read_at = p->irq_at = 0;
    p->usbsts = 0;
    p->portsc[0] = p->portsc[1] = 0;
    p->exc_seen[0] = p->exc_seen[1] = 0;
}

static int attach(u64 mask)
{
    if (!usb_enabled || chip_id != T8152 || board_id != J804_BOARD_ID) {
        printf("HV: USB passthrough refused: not configured (BIT(9)) or not J804\n");
        return HV_USB_E_STATE;
    }
    if (!mask || (mask & ~(u64)HV_USB_PORTS)) {
        printf("HV: USB passthrough refused: port mask 0x%lx (bit 1 usb-drd1, bit 3 usb-drd3; "
               "usb-drd0 is the proxy's link and never passed through)\n",
               mask);
        return HV_USB_E_PORTS;
    }
    if (hv_windows_guest_entered()) {
        printf("HV: USB passthrough refused: the guest session has started (ATTACH belongs "
               "before hv_start)\n");
        return HV_USB_E_STATE;
    }
    if (attached_mask || tables || session_used) {
        printf("HV: USB passthrough refused: already attached this session (reload the "
               "carrier)\n");
        return HV_USB_E_STATE;
    }
    u64 ram_base, ram_size;
    if (!hv_t8152_guest_ram(&ram_base, &ram_size)) {
        printf("HV: USB passthrough refused: no guest RAM window yet\n");
        return HV_USB_E_STATE;
    }

    int ret = 0;
    for (unsigned int i = 0; i < N_PORTS && !ret; i++) {
        reset_port_state(&ports[i]);
        if (mask & ports[i].bit)
            ret = probe(&ports[i]);
    }
    if (ret)
        return ret;
    /* From here on the session owns a deny entry and DART state: one ATTACH per session. */
    session_used = true;
    if ((ret = build_tables(ram_base, ram_base + ram_size)))
        return ret;

    u32 done = 0;
    for (unsigned int i = 0; i < N_PORTS && !ret; i++) {
        struct usb_port *p = &ports[i];
        if (!(mask & p->bit))
            continue;
        ret = commit(p);
        done |= p->bit;
    }
    if (ret) {
        for (unsigned int i = 0; i < N_PORTS; i++)
            if (done & ports[i].bit)
                quiesce_port(&ports[i], false);
        free_tables();
        return ret;
    }

    u64 now = mrs(CNTPCT_EL0);
    for (unsigned int i = 0; i < N_PORTS; i++) {
        struct usb_port *p = &ports[i];
        if (!(mask & p->bit))
            continue;
        p->irq_tick = p->report_tick = now;
        p->owned = true;
        __atomic_store_n(&p->attached, true, __ATOMIC_RELEASE);
    }
    hv_aic_alias_t8152_usb((u32)mask);
    dma_base = ram_base;
    dma_top = ram_base + ram_size;
    __atomic_store_n(&attached_mask, (u32)mask, __ATOMIC_RELEASE);
    for (unsigned int i = 0; i < N_PORTS; i++)
        if (mask & ports[i].bit)
            printf("HV: USB %s attached: xHCI [0x%lx, 0x%lx) to the guest, AIC %u -> SPI %u "
                   "(level, masked until the guest enables it), DART streams 0:0 0:1 1:1 on "
                   "guest RAM [0x%lx, 0x%lx)\n",
                   ports[i].name, ports[i].xhci, ports[i].xhci + HV_USB_XHCI_SIZE, ports[i].irq,
                   ports[i].gsiv, ram_base, ram_base + ram_size);
    return 0;
}

void hv_usb_teardown(const char *why)
{
    spin_lock(&usb_lock);
    u32 mask = __atomic_exchange_n(&attached_mask, 0, __ATOMIC_ACQ_REL);
    if (mask) {
        printf("HV: USB: tearing down (%s)\n", why);
        hv_aic_alias_t8152_usb(0);
        for (unsigned int i = 0; i < N_PORTS; i++) {
            struct usb_port *p = &ports[i];
            if (!(mask & p->bit))
                continue;
            p->irqs_seen = __atomic_load_n(&p->irqs, __ATOMIC_RELAXED);
            p->owned = false;
            quiesce_port(p, false);
            printf("HV: USB %s: stopped; USBSTS 0x%x PORTSC 0x%x/0x%x, %lu interrupts, DART "
                   "exceptions 0x%x/0x%x; streams disabled, xHCI denied again\n",
                   p->name, p->usbsts, p->portsc[0], p->portsc[1], p->irqs_seen,
                   p->exc_seen[0], p->exc_seen[1]);
        }
    }
    free_tables();
    dma_base = dma_top = 0;
    spin_unlock(&usb_lock);
}

void hv_usb_emergency_stop(void)
{
    u32 mask = __atomic_exchange_n(&attached_mask, 0, __ATOMIC_ACQ_REL);
    if (!mask)
        return;
    hv_aic_alias_t8152_usb(0);
    for (unsigned int i = 0; i < N_PORTS; i++)
        if (mask & ports[i].bit)
            quiesce_port(&ports[i], true);
    printf("HV: USB: controller halted and DART streams disabled before the reset\n");
}

void hv_usb_note_irq(u32 physical)
{
    for (unsigned int i = 0; i < N_PORTS; i++)
        if (ports[i].irq == physical) {
            __atomic_fetch_add(&ports[i].irqs, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&ports[i].irq_at, mrs(CNTPCT_EL0), __ATOMIC_RELAXED);
        }
}

/* ---- report ------------------------------------------------------------------------------- */

static void report_dart(struct usb_port *p, unsigned int inst)
{
    u64 d = p->dart[inst];
    u32 exc = read32(d + DART_EXCEPTION);
    u32 err = read32(d + DART_ERROR);
    if (!exc && !err)
        return;
    p->exc_seen[inst] |= exc;
    printf("HV: USB %s: DART %u exception bitmap 0x%x ERROR 0x%x\n", p->name, inst, exc, err);
    for (u32 sid = 0; sid < 16; sid++) {
        if (!(exc & BIT(sid)) || p->dart_size[inst] < DART_RECORD_REGS)
            continue;
        u64 r = d + DART_EXC_RECORD(sid);
        printf("HV: USB %s: DART %u SID %u record 0x%x 0x%x 0x%x 0x%x (raw; Gen3 meaning "
               "unknown)\n",
               p->name, inst, sid, read32(r), read32(r + 4), read32(r + 8), read32(r + 12));
    }
    /* Acknowledge both, as linux-j804 apple-dart-t8152-gen3-irqdump.py does: bitmap, ERROR. */
    if (exc)
        write32(d + DART_EXCEPTION, exc);
    if (err)
        write32(d + DART_ERROR, err);
    sysop("dsb sy");
}

static void storm_guard(struct usb_port *p, u64 now, u64 freq)
{
    u64 irqs = __atomic_load_n(&p->irqs, __ATOMIC_RELAXED);
    u64 elapsed = now - p->irq_tick;
    if (!freq || elapsed < freq)
        return;
    u64 delta = irqs - p->irqs_seen;
    p->irqs_seen = irqs;
    p->irq_tick = now;
    if (p->storm || delta / (elapsed / freq) <= HV_USB_STORM_PER_SEC)
        return;
    /* F10: the level never drops.  Withdraw the line; the guest keeps running without it. */
    p->storm = true;
    p->owned = false;
    u32 owned = 0;
    for (unsigned int i = 0; i < N_PORTS; i++)
        if (ports[i].owned)
            owned |= ports[i].bit;
    hv_aic_alias_t8152_usb(owned);
    aic_set_mask(p->irq, true);
    printf("HV: USB %s: %lu interrupts in %lu ms: the line never drops; AIC %u masked and SPI "
           "%u withdrawn for the rest of the session\n",
           p->name, delta, elapsed / (freq / 1000 ? freq / 1000 : 1), p->irq, p->gsiv);
}

void hv_usb_report(void)
{
    if (!__atomic_load_n(&attached_mask, __ATOMIC_ACQUIRE))
        return;
    u64 now = mrs(CNTPCT_EL0), freq = mrs(CNTFRQ_EL0);
    for (unsigned int i = 0; i < N_PORTS; i++) {
        struct usb_port *p = &ports[i];
        if (!__atomic_load_n(&p->attached, __ATOMIC_ACQUIRE))
            continue;
        u64 op = p->xhci + p->op;
        u32 sts = read32(op + XHCI_USBSTS);
        u32 sc[2] = {0, 0};
        for (u32 n = 1; n <= 2 && n <= p->nports; n++)
            sc[n - 1] = read32(op + XHCI_PORTSC(n));
        report_dart(p, 0);
        report_dart(p, 1);
        for (unsigned int r = 0; r < N_DWC; r++) {
            u32 v = read32(p->xhci + dwc_regs[r].off);
            if (v != p->dwc[r]) {
                printf("HV: USB %s: dwc3 %s 0x%x -> 0x%x (F14, logged only)\n", p->name,
                       dwc_regs[r].name, p->dwc[r], v);
                p->dwc[r] = v;
            }
        }
        storm_guard(p, now, freq);
        p->read_at = mrs(CNTPCT_EL0);
        bool changed = sts != p->usbsts || sc[0] != p->portsc[0] || sc[1] != p->portsc[1];
        bool periodic = freq && now - p->report_tick >= 10 * freq;
        p->usbsts = sts;
        p->portsc[0] = sc[0];
        p->portsc[1] = sc[1];
        if (changed || periodic) {
            p->report_tick = now;
            printf("HV: USB %s: usbsts 0x%x portsc 0x%x/0x%x irq %lu dart exc 0x%x/0x%x "
                   "unowned %lu%s\n",
                   p->name, sts, sc[0], sc[1], __atomic_load_n(&p->irqs, __ATOMIC_RELAXED),
                   p->exc_seen[0], p->exc_seen[1], hv_t8152_unowned_irqs(),
                   p->storm ? " STORM (line withdrawn)" : "");
        }
    }
}

static u64 fatal_age_ms(u64 now, u64 then, u64 freq)
{
    if (!then || then > now || freq < 1000)
        return ~0UL;
    return (now - then) / (freq / 1000);
}

void hv_usb_fatal_report(void)
{
    u64 now = mrs(CNTPCT_EL0), freq = mrs(CNTFRQ_EL0);
    for (unsigned int i = 0; i < N_PORTS; i++) {
        struct usb_port *p = &ports[i];
        if (!p->owned && !p->attached && !p->streams_on)
            continue;
        printf("HV: fatal: USB %s: attached %d, last usbsts 0x%x portsc 0x%x/0x%x, %lu "
               "interrupts, DART exceptions 0x%x/0x%x%s\n",
               p->name ? p->name : "?", (int)p->attached, p->usbsts, p->portsc[0], p->portsc[1],
               __atomic_load_n(&p->irqs, __ATOMIC_RELAXED), p->exc_seen[0], p->exc_seen[1],
               p->storm ? ", storm" : "");
        u64 rd = fatal_age_ms(now, p->read_at, freq);
        u64 irq = fatal_age_ms(now, __atomic_load_n(&p->irq_at, __ATOMIC_RELAXED), freq);
        printf("HV: fatal: USB %s: xHCI [0x%lx, 0x%lx) %s; carrier last read the controller "
               "(USBSTS, PORTSC, DART, dwc3) ",
               p->name ? p->name : "?", p->xhci, p->xhci + HV_USB_XHCI_SIZE,
               p->mapped ? "mapped to the guest (Device-nGnRnE passthrough)" : "not mapped");
        if (rd == ~0UL)
            printf("never");
        else
            printf("%lu ms ago", rd);
        printf("; last line ");
        if (irq == ~0UL)
            printf("never\n");
        else
            printf("%lu ms ago\n", irq);
    }
}

static u64 status(u64 num, u64 index)
{
    if (index == HV_USB_S_ATTACHED)
        return __atomic_load_n(&attached_mask, __ATOMIC_ACQUIRE);
    if (index == HV_USB_S_UNOWNED)
        return hv_t8152_unowned_irqs();
    if (index == HV_USB_S_DMA_BASE || index == HV_USB_S_DMA_TOP) {
        spin_lock(&usb_lock);
        u64 v = !attached_mask ? HV_USB_ERROR : index == HV_USB_S_DMA_BASE ? dma_base : dma_top;
        spin_unlock(&usb_lock);
        return v;
    }
    struct usb_port *p = port_by_num(num);
    if (!p)
        return HV_USB_ERROR;
    bool live = __atomic_load_n(&p->attached, __ATOMIC_ACQUIRE);
    u64 op = p->xhci + p->op;
    switch (index) {
        case HV_USB_S_USBSTS:
            return live ? read32(op + XHCI_USBSTS) : p->usbsts;
        case HV_USB_S_PORTSC1:
            return live && p->nports >= 1 ? read32(op + XHCI_PORTSC(1)) : p->portsc[0];
        case HV_USB_S_PORTSC2:
            return live && p->nports >= 2 ? read32(op + XHCI_PORTSC(2)) : p->portsc[1];
        case HV_USB_S_IRQS:
            return __atomic_load_n(&p->irqs, __ATOMIC_RELAXED);
        case HV_USB_S_DART0_EXC:
            return p->exc_seen[0];
        case HV_USB_S_DART1_EXC:
            return p->exc_seen[1];
        case HV_USB_S_GCTL:
            return p->dwc[DWC_GCTL];
        case HV_USB_S_STORM:
            return p->storm;
        default:
            return HV_USB_ERROR;
    }
}

u64 hv_usb_proxy(u64 op, u64 a1, u64 a2)
{
    int ret;
    switch (op) {
        case HV_USB_OP_ATTACH:
            spin_lock(&usb_lock);
            ret = attach(a1);
            spin_unlock(&usb_lock);
            return (u64)(s64)ret;
        case HV_USB_OP_STATUS:
            return status(a1, a2);
        case HV_USB_OP_DETACH:
            /*
             * Once the guest session has started, only hv_start()'s own exit (and the reset
             * path) tears USB down: a DETACH from the in-session proxy would halt the
             * controller and free the tables under running vCPUs.
             */
            if (hv_windows_guest_entered() && __atomic_load_n(&attached_mask, __ATOMIC_ACQUIRE)) {
                printf("HV: USB: DETACH refused while the guest session owns the controller; "
                       "it is torn down when the session ends\n");
                return HV_USB_ERROR;
            }
            hv_usb_teardown("launcher DETACH");
            return 0;
        default:
            return HV_USB_ERROR;
    }
}

#ifdef HV_USB_HOST_TEST
/* tests/e2e/windows_usb_host.c: a fresh carrier session between scenarios. */
void hv_usb_host_new_session(void)
{
    session_used = false;
}
#endif
