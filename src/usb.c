/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include "usb.h"
#include "adt.h"
#include "boot_options.h"
#include "dart.h"
#include "i2c.h"
#include "iodev.h"
#include "malloc.h"
#include "pmgr.h"
#include "sptm_virt.h"
#include "string.h"
#include "tps6598x.h"
#include "tunables.h"
#include "types.h"
#include "usb_atc_regs.h"
#include "usb_cdc.h"
#include "usb_cdc_atc.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "utils.h"
#include "vsprintf.h"

struct usb_drd_regs {
    uintptr_t drd_regs;
    uintptr_t drd_regs_unk3;
    uintptr_t atc;
};

#if USB_IODEV_COUNT > 100
#error "USB_IODEV_COUNT is limited to 100 to prevent overflow in ADT path names"
#endif

#ifdef USE_DEBUG_USB
#define FIRST_USB_IODEV 1
#else
#define FIRST_USB_IODEV 0
#endif

// length of the format string is is used as buffer size
// limits the USB instance numbers to reasonable 2 digits
#define FMT_DART_PATH        "/arm-io/dart-usb%u"
#define FMT_DART_MAPPER_PATH "/arm-io/dart-usb%u/mapper-usb%u"
#define FMT_ATC_PATH         "/arm-io/atc-phy%u"
#define FMT_DRD_PATH         "/arm-io/usb-drd%u"
// HPM_PATH string is at most
// "/arm-io/i2cX" (12) + "/" + hpmBusManagerX (14) + "/" + "hpmX" (4) + '\0'
#define MAX_HPM_PATH_LEN 40

static tps6598x_irq_state_t tps6598x_irq_state[USB_IODEV_COUNT];
static bool usb_is_initialized = false;

static dart_dev_t *usb_dart_init(u32 idx)
{
    int mapper_offset;
    char path[sizeof(FMT_DART_MAPPER_PATH)];

    snprintf(path, sizeof(path), FMT_DART_MAPPER_PATH, idx, idx);
    mapper_offset = adt_path_offset(adt, path);
    if (mapper_offset < 0 && chip_id == T8140 && idx == 0) {
        snprintf(path, sizeof(path), "/arm-io/dart-usb/mapper-usb");
        mapper_offset = adt_path_offset(adt, path);
    }
    if (mapper_offset < 0) {
        // Device not present
        return NULL;
    }

    u32 dart_idx;
    if (ADT_GETPROP(adt, mapper_offset, "reg", &dart_idx) < 0) {
        printf("usb: Error getting DART %s device index/\n", path);
        return NULL;
    }

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    if (chip_id == T8140 && idx == 0 && adt_path_offset(adt, path) < 0)
        snprintf(path, sizeof(path), "/arm-io/dart-usb");
    return dart_init_adt(path, 1, dart_idx, false);
}

static int usb_drd_adt_offset(u32 idx, char drd_path[sizeof(FMT_DRD_PATH)], int *adt_drd_path)
{
    snprintf(drd_path, sizeof(FMT_DRD_PATH), FMT_DRD_PATH, idx);
    int offset = adt_path_offset_trace(adt, drd_path, adt_drd_path);
    if (offset < 0 && chip_id == T8140 && idx == 0) {
        snprintf(drd_path, sizeof(FMT_DRD_PATH), "/arm-io/usb-drd");
        offset = adt_path_offset_trace(adt, drd_path, adt_drd_path);
    }
    return offset;
}

/* T6050 has a different PIPE clock field. Never run the older automatic
 * gadget initialization on it, including when the explicit setup is refused. */
bool usb_cdc_deferred_usb2(void)
{
    static int deferred = -1;
    if (!adt)
        return false;
    /* This is also queried on every proxy poll; the firmware ADT is fixed. */
    if (deferred < 0) {
        int node = adt_path_offset(adt, "/arm-io/atc-phy0");
        /*
         * The J714c (M5 Max) enumerates through the generic gadget path on every port, as the
         * omacom m1n1 Stage 1 does there; the explicit port-0 setup never comes up on it.
         */
        deferred = node >= 0 && adt_is_compatible(adt, node, "atc-phy,t6050") &&
                   !adt_is_compatible(adt, 0, "J714cAP");
    }
    return deferred;
}

struct usb_t6050_regs {
    u64 phy, phy_size;
    u64 digital;
    u64 pipe, pipe_size;
    u64 dwc;
};

/* This API explicitly selects USB0/HPM0. Prove that the ADT associates all
 * three with the same external port, rather than adding a board-ID table. */
static int usb_t6050_get_regs(struct usb_t6050_regs *regs)
{
    int phy_path[8], drd_path[8], dart_path[8];
    int phy = adt_path_offset_trace(adt, "/arm-io/atc-phy0", phy_path);
    int drd = adt_path_offset_trace(adt, "/arm-io/usb-drd0", drd_path);
    int hpm = adt_path_offset(adt, "/arm-io/nub-spmi-a0/hpm0");
    int dart = adt_path_offset_trace(adt, "/arm-io/dart-usb0", dart_path);
    int mapper = adt_path_offset(adt, "/arm-io/dart-usb0/mapper-usb0");
    u32 phy_port, drd_port, hpm_port, port_type, sid;
    u64 digital_size, dwc_size, dart_base, dart_size;

    if (phy < 0 || drd < 0 || hpm < 0 || dart < 0 || mapper < 0 ||
        !adt_is_compatible(adt, phy, "atc-phy,t6050") ||
        !adt_is_compatible(adt, drd, "usb-drd,t6050") ||
        !adt_is_compatible(adt, dart, "dart,t8110") ||
        adt_get_property(adt, drd, "usb-device-disable") ||
        ADT_GETPROP(adt, phy, "port-number", &phy_port) < 0 ||
        ADT_GETPROP(adt, drd, "port-number", &drd_port) < 0 ||
        ADT_GETPROP(adt, hpm, "port-number", &hpm_port) < 0 || !phy_port || phy_port != drd_port ||
        phy_port != hpm_port || ADT_GETPROP(adt, phy, "port-type", &port_type) < 0 ||
        port_type != 2 || ADT_GETPROP(adt, mapper, "reg", &sid) < 0 || sid >= 256 ||
        adt_get_reg(adt, phy_path, "reg", 0, &regs->phy, &regs->phy_size) < 0 ||
        adt_get_reg(adt, phy_path, "reg", 1, &regs->digital, &digital_size) < 0 ||
        adt_get_reg(adt, drd_path, "reg", 0, &regs->dwc, &dwc_size) < 0 ||
        adt_get_reg(adt, drd_path, "reg", 3, &regs->pipe, &regs->pipe_size) < 0 ||
        adt_get_reg(adt, dart_path, "reg", 1, &dart_base, &dart_size) < 0 ||
        regs->phy_size < 0x20 || digital_size < 0x24 || dwc_size < DWC3_DSTS + 4 ||
        regs->pipe_size < 0x48 || dart_size < 0x1800 ||
        tunables_validate_compact("/arm-io/atc-phy0", "tunable_USB2PHY_DFLT", regs->phy_size) ||
        tunables_validate_compact("/arm-io/atc-phy0", "tunable_USB2PHY_DEV", regs->phy_size))
        return -1;

    return 0;
}

/* Public Aurora Linux c24e75ca9d99fd43d890daea8fae9e0e2eb2ae2a:
 * drivers/phy/apple/usb-t6050/apple_t6050_usb_phy.h (USB2 sequence),
 * apple_t6050_usb3_pipe.h (cold dummy), atc-t6050-combo.h (reset).
 * Only USB2 is needed: no ACIO firmware, high-speed lanes or internal PHY3.
 * Device-role values come from this machine's ADT, not the host-role table. */

/* Older loaders leave the PIPE on the dummy data producer with the clock field
 * as the domain came up (0x22, see apple_t6050_usb3_pipe_cold_dummy()). */
#define T6050_MUX_OLD_LOADER_RELEASED PIPEHANDLER_MUX_T8142(USB4, DUMMY)

static dwc3_dev_t *usb_t6050_init(void (*set_step)(unsigned step))
{
    struct usb_t6050_regs regs;
    if (usb_t6050_get_regs(&regs)) {
        printf("CDC USB2: unsupported T6050 ADT layout\n");
        return NULL;
    }

    set_step(2);
    if (pmgr_adt_power_enable("/arm-io/atc-phy0") < 0 ||
        pmgr_adt_power_enable("/arm-io/dart-usb0") < 0 ||
        pmgr_adt_power_enable("/arm-io/usb-drd0") < 0)
        return NULL;

    u64 pipe = regs.pipe;
    u32 aon = read32(pipe + PIPEHANDLER_AON_GEN);
    u32 mux = read32(pipe + PIPEHANDLER_MUX_CTRL) & PIPEHANDLER_MUX_CTRL_T8142_MASK;
    if (dart_adt_locked("/arm-io/dart-usb0", 1)) {
        printf("CDC USB2: T6050 USB DART is locked; leaving it alone\n");
        return NULL;
    }
    /* An in-place raw chainload retires Stage 1's gadget before this stage
     * starts. Older loaders leave its dummy PIPE released (0x22). Accept
     * only that fully stopped device, never a live or native PIPE producer. */
    if ((aon & PIPEHANDLER_AON_GEN_DWC3_RESET_N) || mux == T6050_MUX_OLD_LOADER_RELEASED) {
        u32 gctl = read32(regs.dwc + DWC3_GCTL);
        bool retired =
            (mux == T6050_MUX_OLD_LOADER_RELEASED || mux == PIPEHANDLER_MUX_T8142(DUMMY, DUMMY)) &&
            (gctl & DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG)) ==
                DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE) &&
            !(read32(regs.dwc + DWC3_DCTL) & DWC3_DCTL_RUN_STOP) &&
            (read32(regs.dwc + DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) &&
            !read32(regs.dwc + DWC3_DEVTEN) && !read32(regs.dwc + DWC3_DALEPENA);
        if (!retired) {
            printf("CDC USB2: T6050 PIPE consumer is not retired (aon=%x mux=%x)\n", aon, mux);
            return NULL;
        }
        set32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
        clear32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
        if (poll32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N, 0, 1000))
            return NULL;
        udelay(10);
        printf("CDC USB2: retired Stage 1 gadget held in reset\n");
    } else if (mux != PIPEHANDLER_MUX_T8142(OFF, USB3) &&
               mux != PIPEHANDLER_MUX_T8142(OFF, DUMMY) &&
               mux != PIPEHANDLER_MUX_T8142(DUMMY, DUMMY)) {
        printf("CDC USB2: unknown T6050 PIPE producer (aon=%x mux=%x)\n", aon, mux);
        return NULL;
    }
    set32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    mask32(pipe + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_CLK_T8142,
           FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK_T8142, PIPEHANDLER_MUX_CTRL_CLK_OFF));
    udelay(10);
    mask32(pipe + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_DATA_T8142,
           FIELD_PREP(PIPEHANDLER_MUX_CTRL_DATA_T8142, PIPEHANDLER_MUX_CTRL_DATA_DUMMY));
    udelay(10);
    mask32(pipe + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_CLK_T8142,
           FIELD_PREP(PIPEHANDLER_MUX_CTRL_CLK_T8142, PIPEHANDLER_MUX_CTRL_CLK_DUMMY));
    udelay(10);
    if ((read32(pipe + PIPEHANDLER_MUX_CTRL) & PIPEHANDLER_MUX_CTRL_T8142_MASK) !=
        PIPEHANDLER_MUX_T8142(DUMMY, DUMMY))
        return NULL;

    set_step(3);
    if (tunables_apply_compact_addr("/arm-io/atc-phy0", "tunable_USB2PHY_DFLT", regs.phy,
                                    regs.phy_size) ||
        tunables_apply_compact_addr("/arm-io/atc-phy0", "tunable_USB2PHY_DEV", regs.phy,
                                    regs.phy_size))
        return NULL;
    set32(regs.phy + USB2PHY_SIG, USB2PHY_SIG_VBUS_FORCE);
    udelay(10000);
    clear32(regs.phy + USB2PHY_CTL, USB2PHY_CTL_SIDDQ);
    udelay(10);
    clear32(regs.phy + USB2PHY_CTL, USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET);
    set32(regs.digital + ATCPHY_EVT_USB2_CTL,
          ATCPHY_EVT_USB2_CTL_EVT_EN | ATCPHY_EVT_USB2_CTL_LOAD_CNT);
    set32(regs.phy + USB2PHY_CTL, USB2PHY_CTL_APB_RESET_N);
    clear32(regs.phy + USB2PHY_MISCTUNE,
            USB2PHY_MISCTUNE_APBCLK_GATE_OFF | USB2PHY_MISCTUNE_REFCLK_GATE_OFF);
    udelay(30);
    udelay(5000);
    mask32(regs.phy + USB2PHY_USBCTL, USB2PHY_USBCTL_USB_MODE, USB2PHY_USBCTL_RUN);

    set32(pipe + PIPEHANDLER_NONSELECTED_OVERRIDE, PIPEHANDLER_DUMMY_PHY_EN);
    clear32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN);
    set32(pipe + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    if (poll32(pipe + PIPEHANDLER_AON_GEN,
               PIPEHANDLER_AON_GEN_DWC3_FORCE_CLAMP_EN | PIPEHANDLER_AON_GEN_DWC3_RESET_N,
               PIPEHANDLER_AON_GEN_DWC3_RESET_N, 1000) ||
        (read32(regs.phy + USB2PHY_USBCTL) & USB2PHY_USBCTL_USB_MODE) != USB2PHY_USBCTL_RUN ||
        (read32(regs.phy + USB2PHY_CTL) & USB2PHY_CTL_MASK) != USB2PHY_CTL_APB_RESET_N) {
        printf("CDC USB2: T6050 reset/clock setup did not latch\n");
        return NULL;
    }

    set_step(4);
    dart_dev_t *dart = usb_dart_init(0); /* ADT mapper SID, t8110 on T6050 */
    if (!dart)
        return NULL;
    dwc3_dev_t *dwc = usb_dwc3_init(regs.dwc, dart);
    if (!dwc)
        return NULL;
    printf("CDC USB2: T6050 controller initialized, PIPE=%x; link unverified\n",
           read32(pipe + PIPEHANDLER_MUX_CTRL));
    return dwc;
}

/*
 * The ADT marks ports that must never run as a USB device with usb-device-disable, for
 * example the J773 front ports, whose hub prohibits device mode. Leave those alone.
 *
 * The T8140 CDC handoff (usb_cdc_link_start) is the exception: J700's ATC0 usb-drd carries
 * the property too, yet that port is the debug port. The handoff has already cold-reset its
 * HPM and taken the carrier over, and explicitly asks for device mode on it, so it bypasses
 * the property. Every other bring-up still honours it.
 */
static bool usb_cdc_device_override;

static bool usb_device_mode_disabled(u32 idx)
{
    if (usb_cdc_device_override && chip_id == T8140 && idx == 0)
        return false;

    int adt_drd_path[8];
    char drd_path[sizeof(FMT_DRD_PATH)];
    int node = usb_drd_adt_offset(idx, drd_path, adt_drd_path);

    return node >= 0 && adt_get_property(adt, node, "usb-device-disable");
}

static int usb_drd_get_regs(u32 idx, struct usb_drd_regs *regs)
{
    int adt_drd_path[8];
    int adt_drd_offset;
    int adt_phy_path[8];
    int adt_phy_offset;
    char phy_path[sizeof(FMT_ATC_PATH)];
    char drd_path[sizeof(FMT_DRD_PATH)];

    adt_drd_offset = usb_drd_adt_offset(idx, drd_path, adt_drd_path);
    if (adt_drd_offset < 0) {
        // Nonexistent device
        return -1;
    }

    snprintf(phy_path, sizeof(phy_path), FMT_ATC_PATH, idx);
    adt_phy_offset = adt_path_offset_trace(adt, phy_path, adt_phy_path);
    if (adt_phy_offset < 0) {
        printf("usb: Error getting phy node %s\n", phy_path);
        return -1;
    }

    if (adt_get_reg(adt, adt_phy_path, "reg", 0, &regs->atc, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", phy_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 0, &regs->drd_regs, NULL) < 0) {
        printf("usb: Error getting reg with index 0 for %s.\n", drd_path);
        return -1;
    }
    if (adt_get_reg(adt, adt_drd_path, "reg", 3, &regs->drd_regs_unk3, NULL) < 0) {
        printf("usb: Error getting reg with index 3 for %s.\n", drd_path);
        return -1;
    }

    return 0;
}

int usb_phy_bringup(u32 idx)
{
    char path[24];

    if (idx >= USB_IODEV_COUNT)
        return -1;

    if (usb_cdc_deferred_usb2())
        return -1;

    if (usb_device_mode_disabled(idx)) {
        printf("usb: usb-drd%u has usb-device-disable, leaving it alone\n", idx);
        return -1;
    }

    struct usb_drd_regs usb_regs;
    if (usb_drd_get_regs(idx, &usb_regs) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_ATC_PATH, idx);
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DART_PATH, idx);
    if (chip_id == T8140 && idx == 0)
        snprintf(path, sizeof(path), "/arm-io/dart-usb");
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    snprintf(path, sizeof(path), FMT_DRD_PATH, idx);
    if (chip_id == T8140 && idx == 0)
        snprintf(path, sizeof(path), "/arm-io/usb-drd");
    if (pmgr_adt_power_enable(path) < 0)
        return -1;

    if (chip_id == T8140 && idx == 0)
        return usb_cdc_atc_power_on(usb_regs.drd_regs_unk3);

    write32(usb_regs.atc + USB2PHY_SIG, 0x01c1000f);
    write32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_RESET | USB2PHY_CTL_PORT_RESET);
    clear32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_RESET);
    clear32(usb_regs.atc + USB2PHY_CTL, USB2PHY_CTL_PORT_RESET);
    write32(usb_regs.atc + USB2PHY_MISCTUNE, 0x008c0813);
    write32(usb_regs.atc + USB2PHY_USBCTL, USB2PHY_USBCTL_RUN);

    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_MUX_CTRL, PIPEHANDLER_MUX_CTRL_DUMMY);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_AON_GEN, PIPEHANDLER_AON_GEN_DWC3_RESET_N);
    write32(usb_regs.drd_regs_unk3 + PIPEHANDLER_NONSELECTED_OVERRIDE, 0x9332);

    return 0;
}

dwc3_dev_t *usb_iodev_bringup(u32 idx)
{
    if (usb_cdc_deferred_usb2())
        return NULL;
    if (usb_device_mode_disabled(idx))
        return NULL;

    dart_dev_t *usb_dart = usb_dart_init(idx);
    if (!usb_dart)
        return NULL;

    struct usb_drd_regs usb_reg;
    if (usb_drd_get_regs(idx, &usb_reg) < 0)
        return NULL;

    return usb_dwc3_init_mode(usb_reg.drd_regs, usb_dart, chip_id == T8140);
}

#define USB_IODEV_WRAPPER(name, pipe)                                                              \
    static ssize_t usb_##name##_can_read(void *dev)                                                \
    {                                                                                              \
        return usb_dwc3_can_read(dev, pipe);                                                       \
    }                                                                                              \
                                                                                                   \
    static bool usb_##name##_can_write(void *dev)                                                  \
    {                                                                                              \
        return usb_dwc3_can_write(dev, pipe);                                                      \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_read(void *dev, void *buf, size_t count)                           \
    {                                                                                              \
        return usb_dwc3_read(dev, pipe, buf, count);                                               \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_write(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_write(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static ssize_t usb_##name##_queue(void *dev, const void *buf, size_t count)                    \
    {                                                                                              \
        return usb_dwc3_queue(dev, pipe, buf, count);                                              \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_handle_events(void *dev)                                              \
    {                                                                                              \
        usb_dwc3_handle_events(dev);                                                               \
    }                                                                                              \
                                                                                                   \
    static void usb_##name##_flush(void *dev)                                                      \
    {                                                                                              \
        usb_dwc3_flush(dev, pipe);                                                                 \
    }

USB_IODEV_WRAPPER(0, CDC_ACM_PIPE_0)
USB_IODEV_WRAPPER(1, CDC_ACM_PIPE_1)

struct iodev_ops iodev_usb_ops = {
    .can_read = usb_0_can_read,
    .can_write = usb_0_can_write,
    .read = usb_0_read,
    .write = usb_0_write,
    .queue = usb_0_queue,
    .flush = usb_0_flush,
    .handle_events = usb_0_handle_events,
};

static struct iodev_ops iodev_usb_sec_ops = {
    .can_read = usb_1_can_read,
    .can_write = usb_1_can_write,
    .read = usb_1_read,
    .write = usb_1_write,
    .queue = usb_1_queue,
    .flush = usb_1_flush,
    .handle_events = usb_1_handle_events,
};

struct iodev iodev_usb_vuart = {
    .ops = &iodev_usb_sec_ops,
    .usage = 0,
    .lock = SPINLOCK_INIT,
};

static int hpm_idx(char *hpm_path)
{
    size_t len = strlen(hpm_path);
    if (len < 4)
        return -1;
    if (memcmp(hpm_path + len - 4, "hpm", 3))
        return -1; // unexpected hpm node name
    u8 idx = hpm_path[len - 1] - '0';
    if (idx > 9)
        return -2; // unexpected hpm index
    return idx;
}

static bool usb_init_match(char *hpm_path, void *unused)
{
    (void)unused;
    int idx = hpm_idx(hpm_path);
    if (idx < FIRST_USB_IODEV)
        return false;
    if (idx >= USB_IODEV_COUNT)
        return false;
    return true;
}

static int usb_init_one(char *hpm_path, tps6598x_dev_t *tps, void *unused)
{
    (void)unused;
    int idx = hpm_idx(hpm_path);

    if (tps6598x_powerup(tps) < 0)
        printf("usb: tps6598x_powerup failed for %s.\n", hpm_path);

    if (tps6598x_disable_irqs(tps, &tps6598x_irq_state[idx]))
        printf("usb: unable to disable IRQ masks for %s.\n", hpm_path);

    return 0;
}

void usb_init(void)
{
    /* Preserve the debug carrier; normal USB initialization can retire it. */
    if (aura_debug_enabled())
        return;
    if (usb_cdc_deferred_usb2())
        return;
    if (usb_is_initialized)
        return;

    /* This PHY requires a deferred carrier transition; normal boots never
     * enter this fallback path before handing over to the OS. */
    if (chip_id == T8140) {
        if (!usb_cdc_ready() && !usb_cdc_owns_carrier() && usb_cdc_schedule(0, 0, BIT(1) | BIT(2)))
            printf("usb: unable to schedule proxy transport\n");
        return;
    }

    /*
     * A7-A11 uses a custom internal otg controller with the peripheral part
     * being dwc2.
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0) {
        /* We do not support the custom controller and dwc2 (yet). */
        return;
    }

    tps6598x_foreach_hpm(usb_init_match, usb_init_one, NULL);

    for (int idx = 0; idx < USB_IODEV_COUNT; ++idx)
        usb_phy_bringup(idx); /* Fails on missing devices, just continue */

    usb_is_initialized = true;
}

static bool usb_hpm_restore_irqs_match(char *hpm_path, void *state)
{
    int idx = hpm_idx(hpm_path);
    if (idx < FIRST_USB_IODEV || idx >= USB_IODEV_COUNT)
        return false;

    bool force = *(bool *)state;
    if (iodev_get_usage(IODEV_USB0 + idx) && !force)
        return false;

    if (!tps6598x_irq_state[idx].valid)
        return false;

    return true;
}

static int usb_hpm_restore_irqs_one(char *hpm_path, tps6598x_dev_t *tps, void *unused)
{
    (void)unused;
    int idx = hpm_idx(hpm_path);

    if (tps6598x_restore_irqs(tps, &tps6598x_irq_state[idx]))
        printf("usb: unable to restore IRQ masks for %s\n", hpm_path);

    return 0;
}

void usb_hpm_restore_irqs(bool force)
{
    /*
     * Do not try to restore irqs on A7-A11 which don't use i2c
     */
    if (adt_path_offset(adt, "/arm-io/otgphyctrl") > 0 &&
        adt_path_offset(adt, "/arm-io/usb-complex") > 0)
        return;

    tps6598x_foreach_hpm(usb_hpm_restore_irqs_match, usb_hpm_restore_irqs_one, &force);
}

void usb_iodev_init(void)
{
    /* Preserve the debug carrier; normal USB initialization can retire it. */
    if (aura_debug_enabled())
        return;
    if (usb_cdc_deferred_usb2())
        return;
    if (chip_id == T8140)
        return;

    for (int i = FIRST_USB_IODEV; i < USB_IODEV_COUNT; i++) {
        dwc3_dev_t *opaque;
        struct iodev *usb_iodev;

        opaque = usb_iodev_bringup(i);
        if (!opaque)
            continue;

        usb_iodev = memalign(SPINLOCK_ALIGN, sizeof(*usb_iodev));
        if (!usb_iodev)
            continue;

        usb_iodev->ops = &iodev_usb_ops;
        usb_iodev->opaque = opaque;
        usb_iodev->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
        spin_init(&usb_iodev->lock);

        iodev_register_device(IODEV_USB0 + i, usb_iodev);
        printf("USB%d: initialized at %p\n", i, opaque);
    }
}

/* False when a controller may still reach its DMA buffers (they are retained, see usb_dwc3.h). */
bool usb_iodev_shutdown(void)
{
    bool stopped = true;

    for (int i = FIRST_USB_IODEV; i < USB_IODEV_COUNT; i++) {
        struct iodev *usb_iodev = iodev_unregister_device(IODEV_USB0 + i);
        if (!usb_iodev)
            continue;

        printf("USB%d: shutdown\n", i);
        if (!usb_dwc3_shutdown(usb_iodev->opaque)) {
            printf("USB%d: controller not stopped, its DMA buffers are retained\n", i);
            stopped = false;
        }
        free(usb_iodev);
    }
    return stopped;
}

void usb_iodev_vuart_setup(iodev_id_t iodev)
{
    if (iodev < IODEV_USB0 || iodev >= IODEV_USB0 + USB_IODEV_COUNT)
        return;

    iodev_usb_vuart.opaque = iodev_get_opaque(iodev);
}

/*
 * The waits of the CDC link's bring-up. Under the virtualized SPTM the link can come back on a
 * running macOS, where the bring-up runs on a coroutine and these give the core back to the guest
 * instead of spinning.
 */
void usb_cdc_wait_us(u32 us)
{
    if (!sptm_virt_s2r_usb_yield(us) && us)
        udelay(us);
}

int usb_cdc_poll32(u64 addr, u32 mask, u32 target, u32 timeout_us)
{
    if (!sptm_virt_s2r_usb_yieldable())
        return poll32(addr, mask, target, timeout_us);
    u64 end = mrs(CNTPCT_EL0) + (u64)timeout_us * mrs(CNTFRQ_EL0) / 1000000;
    while ((read32(addr) & mask) != target) {
        if (mrs(CNTPCT_EL0) >= end)
            return -1;
        usb_cdc_wait_us(0);
    }
    return 0;
}

int usb_cdc_link_start(void (*set_step)(unsigned step), bool force_swapped)
{
    if (chip_id == T8142) {
        /* Normal J813 startup already initialized USB0. Do not reinitialize
         * its live controller or DART during the carrier transition. */
        return !force_swapped && board_id == 0x24 && iodev_get_opaque(IODEV_USB0) ? 0 : -1;
    }
    if (chip_id == T8152 || usb_cdc_deferred_usb2()) {
        if (force_swapped || iodev_get_opaque(IODEV_USB0))
            return -1;
        struct iodev *device = memalign(SPINLOCK_ALIGN, sizeof(*device));
        if (!device)
            return -1;
        dwc3_dev_t *dwc =
            usb_cdc_deferred_usb2() ? usb_t6050_init(set_step) : usb_t8152_init(set_step);
        if (!dwc) {
            free(device);
            return -1;
        }
        device->ops = &iodev_usb_ops;
        device->opaque = dwc;
        device->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
        spin_init(&device->lock);
        iodev_register_device(IODEV_USB0, device);
        return 0;
    }
    if (chip_id != T8140 || iodev_get_usage(IODEV_USB0))
        return -1;
    set_step(3);
    usb_cdc_atc_force_swapped(force_swapped);
    usb_cdc_device_override = true;
    int phy = usb_phy_bringup(0);
    if (phy) {
        usb_cdc_device_override = false;
        return -1;
    }

    set_step(4);
    dwc3_dev_t *dwc = usb_iodev_bringup(0);
    usb_cdc_device_override = false;
    if (!dwc)
        return -1;

    struct iodev *device = memalign(SPINLOCK_ALIGN, sizeof(*device));
    if (!device) {
        usb_dwc3_shutdown(dwc);
        return -1;
    }
    device->ops = &iodev_usb_ops;
    device->opaque = dwc;
    device->usage = USAGE_CONSOLE | USAGE_UARTPROXY;
    spin_init(&device->lock);
    iodev_register_device(IODEV_USB0, device);

    set_step(5);
    if (usb_cdc_atc_switch_pipe()) {
        iodev_unregister_device(IODEV_USB0);
        usb_dwc3_shutdown(dwc);
        free(device);
        return -1;
    }
    return 0;
}
