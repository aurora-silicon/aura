/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

/*
 * Useful references:
 * - TI KeyStone II Architecture Universal Serial Bus 3.0 (USB 3.0) User's Guide
 *   Literature Number: SPRUHJ7A, https://www.ti.com/lit/ug/spruhj7a/spruhj7a.pdf
 * - https://www.beyondlogic.org/usbnutshell/usb1.shtml
 */

#include "../build/build_cfg.h"
#include "../build/build_tag.h"

#include "adt.h"
#include "dart.h"
#include "malloc.h"
#include "memory.h"
#include "ringbuffer.h"
#include "string.h"
#include "types.h"
#include "usb_cdc.h"
#include "usb_cdc_atc.h"
#include "usb_cdc_bulk.h"
#include "usb_cdc_ss_desc.h"
#include "usb_cdc_ss_regs.h"
#include "usb_cdc_state.h"
#include "usb_dwc3.h"
#include "usb_dwc3_regs.h"
#include "usb_types.h"
#include "utils.h"

#define MAX_ENDPOINTS     16
#define CDC_BUFFER_SIZE   SZ_1M
#define CDC_IO_TIMEOUT_MS 3000
/* A closed or reset pipe ends every blocking CDC loop, in both engines. */
#define CDC_LOOP_READY(dev, pipe) ((dev)->pipe[(pipe)].ready)

#define usb_debug_printf(fmt, ...) debug_printf("usb-dwc3@%lx: " fmt, dev->regs, ##__VA_ARGS__)

#define STRING_DESCRIPTOR_LANGUAGES    0
#define STRING_DESCRIPTOR_MANUFACTURER 1
#define STRING_DESCRIPTOR_PRODUCT      2
#define STRING_DESCRIPTOR_SERIAL       3

#if USB_DWC3_SUPERSPEED
#define DWC3_EP0_MAX_DESC_LEN   510
#define CDC_BULK_PACKET_SIZE    1024
#define CDC_CONTROL_PACKET_SIZE 512
#else
#define DWC3_EP0_MAX_DESC_LEN   62
#define CDC_BULK_PACKET_SIZE    512
#define CDC_CONTROL_PACKET_SIZE 64
#endif

#define CDC_DEVICE_CLASS 0x02

#define CDC_USB_VID 0x1209
#define CDC_USB_PID 0x316d

#define CDC_INTERFACE_CLASS         0x02
#define CDC_INTERFACE_CLASS_DATA    0x0a
#define CDC_INTERFACE_SUBCLASS_ACM  0x02
#define CDC_INTERFACE_PROTOCOL_NONE 0x00
#define CDC_INTERFACE_PROTOCOL_AT   0x01

#define DWC3_SCRATCHPAD_SIZE SZ_16K
#define TRB_BUFFER_SIZE      SZ_16K
#if USB_DWC3_SUPERSPEED
#define XFER_BUFFER_BYTES_PER_EP CDC_BULK_CHAIN_BYTES
#define XFER_SIZE                CDC_BULK_CHAIN_BYTES
#define XFER_BUFFER_SIZE         (XFER_BUFFER_BYTES_PER_EP * MAX_ENDPOINTS)
#else
#define XFER_BUFFER_SIZE         (SZ_16K * MAX_ENDPOINTS * 2)
#define XFER_BUFFER_BYTES_PER_EP (XFER_BUFFER_SIZE / MAX_ENDPOINTS)
#define XFER_SIZE                SZ_16K
#endif
#define PAD_BUFFER_SIZE SZ_16K

#define TRBS_PER_EP (TRB_BUFFER_SIZE / (MAX_ENDPOINTS * sizeof(struct dwc3_trb)))

#define SCRATCHPAD_IOVA   0xbeef0000
#define EVENT_BUFFER_IOVA 0xdead0000
#define XFER_BUFFER_IOVA  0xbabe0000
#define TRB_BUFFER_IOVA   0xf00d0000

/* these map to the control endpoint 0x00/0x80 */
#define USB_LEP_CTRL_OUT 0
#define USB_LEP_CTRL_IN  1

/* maps to interrupt endpoint 0x81 */
#define USB_LEP_CDC_INTR_IN 3

/* these map to physical endpoints 0x02 and 0x82 */
#define USB_LEP_CDC_BULK_OUT 4
#define USB_LEP_CDC_BULK_IN  5

/* maps to interrupt endpoint 0x83 */
#define USB_LEP_CDC_INTR_IN_2 7

/* these map to physical endpoints 0x04 and 0x84 */
#define USB_LEP_CDC_BULK_OUT_2 8
#define USB_LEP_CDC_BULK_IN_2  9

/* content doesn't matter at all, this is the setting linux writes by default */
static const u8 cdc_default_line_coding[] = {0x80, 0x25, 0x00, 0x00, 0x00, 0x00, 0x08};

enum ep0_state {
    USB_DWC3_EP0_STATE_IDLE,
    USB_DWC3_EP0_STATE_SETUP_HANDLE,
    USB_DWC3_EP0_STATE_DATA_SEND,
    USB_DWC3_EP0_STATE_DATA_RECV,
    USB_DWC3_EP0_STATE_DATA_SEND_DONE,
    USB_DWC3_EP0_STATE_DATA_RECV_DONE,
    USB_DWC3_EP0_STATE_DATA_RECV_STATUS,
    USB_DWC3_EP0_STATE_DATA_RECV_STATUS_DONE,
    USB_DWC3_EP0_STATE_DATA_SEND_STATUS,
    USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE
};

typedef struct dwc3_dev {
    /* USB DRD */
    uintptr_t regs;
    dart_dev_t *dart;

    enum ep0_state ep0_state;
    bool ep0_three_stage; /* the control transfer has a data stage (wLength != 0) */
    const void *ep0_buffer;
    u32 ep0_buffer_len;
    void *ep0_read_buffer;
    u32 ep0_read_buffer_len;

    void *evtbuffer;
    u32 evt_buffer_offset;
    bool failed;
    bool dma_unsafe;
    /* usb_dwc3_shutdown() is retiring transfers: only their completions are handled */
    bool shutting_down;
#if USB_DWC3_SUPERSPEED
    bool reset_in_progress;
    bool ep0_progressed_during_reset;
    bool configured_during_reset;
    bool cdc_reconfigure;
    bool cdc_ever_configured;
#endif

    void *scratchpad;
    void *xferbuffer;
    struct dwc3_trb *trbs;

    struct {
        bool xfer_in_progress;
        /* a shutdown ENDTRANSFER waits for its command completion */
        bool end_cmd_pending;
        bool zlp_pending;
        bool stalled;
        u8 resource_index;
        u8 trb_count;
        u8 completed_trbs;
        u32 transfer_length;

        void *xfer_buffer;
        uintptr_t xfer_buffer_iova;

        struct dwc3_trb *trb;
        uintptr_t trb_iova;
    } endpoints[MAX_ENDPOINTS];

    struct {
        ringbuffer_t *host2device;
        ringbuffer_t *device2host;
        u8 ep_intr;
        u8 ep_in;
        u8 ep_out;
        bool ready;
        /* USB ACM CDC serial */
        u8 cdc_line_coding[7];
    } pipe[CDC_ACM_PIPE_MAX];

#if USB_DWC3_SUPERSPEED
    bool primary_was_open;
    bool primary_dtr_pending;
    struct usb_cdc_recovery recovery;
#endif

} dwc3_dev_t;

static const struct usb_string_descriptor str_manufacturer =
    make_usb_string_descriptor("Aurora Silicon");
static const struct usb_string_descriptor str_product =
    make_usb_string_descriptor("aura uartproxy " BUILD_TAG);
static const struct usb_string_descriptor str_serial_dummy = make_usb_string_descriptor("P-0");
static const struct usb_string_descriptor *str_serial;

static const struct usb_string_descriptor_languages str_langs = {
    .bLength = sizeof(str_langs) + 2,
    .bDescriptorType = USB_STRING_DESCRIPTOR,
    .wLANGID = {USB_LANGID_EN_US},
};

struct cdc_dev_desc {
    const struct usb_configuration_descriptor configuration;
    const struct usb_interface_descriptor interface_management;
    const struct cdc_union_functional_descriptor cdc_union_func;
    const struct usb_endpoint_descriptor endpoint_notification;
    const struct usb_interface_descriptor interface_data;
    const struct usb_endpoint_descriptor endpoint_data_in;
    const struct usb_endpoint_descriptor endpoint_data_out;
    const struct usb_interface_descriptor sec_interface_management;
    const struct cdc_union_functional_descriptor sec_cdc_union_func;
    const struct usb_endpoint_descriptor sec_endpoint_notification;
    const struct usb_interface_descriptor sec_interface_data;
    const struct usb_endpoint_descriptor sec_endpoint_data_in;
    const struct usb_endpoint_descriptor sec_endpoint_data_out;
} PACKED;

static const struct usb_device_descriptor usb_cdc_device_descriptor = {
    .bLength = sizeof(struct usb_device_descriptor),
    .bDescriptorType = USB_DEVICE_DESCRIPTOR,
    .bcdUSB = 0x0200,
    .bDeviceClass = CDC_DEVICE_CLASS,
    .bDeviceSubClass = 0, // unused
    .bDeviceProtocol = 0, // unused
    .bMaxPacketSize0 = 64,
    .idVendor = CDC_USB_VID,
    .idProduct = CDC_USB_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = STRING_DESCRIPTOR_MANUFACTURER,
    .iProduct = STRING_DESCRIPTOR_PRODUCT,
    .iSerialNumber = STRING_DESCRIPTOR_SERIAL,
    .bNumConfigurations = 1,
};

#if USB_DWC3_SUPERSPEED
static struct usb_device_descriptor usb_cdc_ss_device_descriptor;
static u8 usb_cdc_ss_configuration[sizeof(struct cdc_dev_desc) + CDC_SS_COMPANION_BYTES];
static size_t usb_cdc_ss_configuration_len;
#endif

static const struct cdc_dev_desc cdc_configuration_descriptor = {
    .configuration =
        {
            .bLength = sizeof(cdc_configuration_descriptor.configuration),
            .bDescriptorType = USB_CONFIGURATION_DESCRIPTOR,
            .wTotalLength = sizeof(cdc_configuration_descriptor),
            .bNumInterfaces = 4,
            .bConfigurationValue = 1,
            .iConfiguration = 0,
            .bmAttributes = USB_CONFIGURATION_ATTRIBUTE_RES1 | USB_CONFIGURATION_SELF_POWERED,
            .bMaxPower = 250,

        },
    .interface_management =
        {
            .bLength = sizeof(cdc_configuration_descriptor.interface_management),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 0,
            .bAlternateSetting = 0,
            .bNumEndpoints = 1,
            .bInterfaceClass = CDC_INTERFACE_CLASS,
            .bInterfaceSubClass = CDC_INTERFACE_SUBCLASS_ACM,
            .bInterfaceProtocol = CDC_INTERFACE_PROTOCOL_NONE,
            .iInterface = 0,

        },
    .cdc_union_func =
        {
            .bFunctionLength = sizeof(cdc_configuration_descriptor.cdc_union_func),
            .bDescriptorType = USB_CDC_INTERFACE_FUNCTIONAL_DESCRIPTOR,
            .bDescriptorSubtype = USB_CDC_UNION_SUBTYPE,
            .bControlInterface = 0,
            .bDataInterface = 1,
        },
    /*
     * we never use this endpoint, but it should exist and always be idle.
     * it needs to exist in the descriptor though to make hosts correctly recognize
     * us as a ACM CDC device.
     */
    .endpoint_notification =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_notification),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(1),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_INTERRUPT,
            .wMaxPacketSize = 64,
            .bInterval = 10,

        },
    .interface_data =
        {
            .bLength = sizeof(cdc_configuration_descriptor.interface_data),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 1,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = CDC_INTERFACE_CLASS_DATA,
            .bInterfaceSubClass = 0, // unused
            .bInterfaceProtocol = 0, // unused
            .iInterface = 0,
        },
    .endpoint_data_in =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_data_in),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_OUT(2),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
    .endpoint_data_out =
        {
            .bLength = sizeof(cdc_configuration_descriptor.endpoint_data_out),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(2),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },

    /*
     * CDC ACM interface for virtual uart
     */

    .sec_interface_management =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_interface_management),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 2,
            .bAlternateSetting = 0,
            .bNumEndpoints = 1,
            .bInterfaceClass = CDC_INTERFACE_CLASS,
            .bInterfaceSubClass = CDC_INTERFACE_SUBCLASS_ACM,
            .bInterfaceProtocol = CDC_INTERFACE_PROTOCOL_NONE,
            .iInterface = 0,

        },
    .sec_cdc_union_func =
        {
            .bFunctionLength = sizeof(cdc_configuration_descriptor.sec_cdc_union_func),
            .bDescriptorType = USB_CDC_INTERFACE_FUNCTIONAL_DESCRIPTOR,
            .bDescriptorSubtype = USB_CDC_UNION_SUBTYPE,
            .bControlInterface = 2,
            .bDataInterface = 3,
        },
    /*
     * we never use this endpoint, but it should exist and always be idle.
     * it needs to exist in the descriptor though to make hosts correctly recognize
     * us as a ACM CDC device.
     */
    .sec_endpoint_notification =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_notification),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(3),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_INTERRUPT,
            .wMaxPacketSize = 64,
            .bInterval = 10,

        },
    .sec_interface_data =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_interface_data),
            .bDescriptorType = USB_INTERFACE_DESCRIPTOR,
            .bInterfaceNumber = 3,
            .bAlternateSetting = 0,
            .bNumEndpoints = 2,
            .bInterfaceClass = CDC_INTERFACE_CLASS_DATA,
            .bInterfaceSubClass = 0, // unused
            .bInterfaceProtocol = 0, // unused
            .iInterface = 0,
        },
    .sec_endpoint_data_in =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_data_in),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_OUT(4),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
    .sec_endpoint_data_out =
        {
            .bLength = sizeof(cdc_configuration_descriptor.sec_endpoint_data_out),
            .bDescriptorType = USB_ENDPOINT_DESCRIPTOR,
            .bEndpointAddress = USB_ENDPOINT_ADDR_IN(4),
            .bmAttributes = USB_ENDPOINT_ATTR_TYPE_BULK,
            .wMaxPacketSize = 512,
            .bInterval = 10,
        },
};

static const struct usb_device_qualifier_descriptor usb_cdc_device_qualifier_descriptor = {
    .bLength = sizeof(struct usb_device_qualifier_descriptor),
    .bDescriptorType = USB_DEVICE_QUALIFIER_DESCRIPTOR,
    .bcdUSB = 0x0200,
    .bDeviceClass = CDC_DEVICE_CLASS,
    .bDeviceSubClass = 0, // unused
    .bDeviceProtocol = 0, // unused
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 0,
};

static const char *devt_names[] = {
    "DisconnEvt", "USBRst",   "ConnectDone", "ULStChng", "WkUpEvt",      "Reserved",       "EOPF",
    "SOF",        "Reserved", "ErrticErr",   "CmdCmplt", "EvntOverflow", "VndrDevTstRcved"};
static const char *depvt_names[] = {
    "Reserved",
    "XferComplete",
    "XferInProgress",
    "XferNotReady",
    "RxTxFifoEvt (IN->Underrun, OUT->Overrun)",
    "Reserved",
    "StreamEvt",
    "EPCmdCmplt",
};

static const char *ep0_state_names[] = {
    "STATE_IDLE",
    "STATE_SETUP_HANDLE",
    "STATE_DATA_SEND",
    "STATE_DATA_RECV",
    "STATE_DATA_SEND_DONE",
    "STATE_DATA_RECV_DONE",
    "STATE_DATA_RECV_STATUS",
    "STATE_DATA_RECV_STATUS_DONE",
    "STATE_DATA_SEND_STATUS",
    "STATE_DATA_SEND_STATUS_DONE",
};

#if USB_DWC3_SUPERSPEED
static void usb_dwc3_fail_session(dwc3_dev_t *dev);
#endif

static u8 ep_to_num(u8 epno)
{
    return (epno << 1) | (epno >> 7);
}

static int usb_dwc3_command(dwc3_dev_t *dev, u32 command, u32 par)
{
    write32(dev->regs + DWC3_DGCMDPAR, par);
    write32(dev->regs + DWC3_DGCMD, command | DWC3_DGCMD_CMDACT);

    if (poll32(dev->regs + DWC3_DGCMD, DWC3_DGCMD_CMDACT, 0, 1000)) {
        usb_debug_printf("timeout while waiting for DWC3_DGCMD_CMDACT to clear.\n");
        return -1;
    }

    return DWC3_DGCMD_STATUS(read32(dev->regs + DWC3_DGCMD));
}

static int usb_dwc3_ep_command(dwc3_dev_t *dev, u8 ep, u32 command, u32 par0, u32 par1, u32 par2)
{
    write32(dev->regs + DWC3_DEPCMDPAR0(ep), par0);
    write32(dev->regs + DWC3_DEPCMDPAR1(ep), par1);
    write32(dev->regs + DWC3_DEPCMDPAR2(ep), par2);
    write32(dev->regs + DWC3_DEPCMD(ep), command | DWC3_DEPCMD_CMDACT);

    if (poll32(dev->regs + DWC3_DEPCMD(ep), DWC3_DEPCMD_CMDACT, 0, 1000)) {
        usb_debug_printf("timeout while waiting for DWC3_DEPCMD_CMDACT to clear.\n");
        return -1;
    }

    return DWC3_DEPCMD_STATUS(read32(dev->regs + DWC3_DEPCMD(ep)));
}

static int usb_dwc3_end_transfer(dwc3_dev_t *dev, u8 ep)
{
    /* An IOC wait recursively dispatches EP0 events while a SETUP request
     * is still being handled. Linux uses a non-IOC ENDTRANSFER and allows
     * the controller 1 ms to retire DMA before reusing the TRB. */
    u32 phy = read32(dev->regs + DWC3_GUSB2PHYCFG(0));
    if (phy & DWC3_GUSB2PHYCFG_SUSPHY)
        clear32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_SUSPHY);
    write32(dev->regs + DWC3_DEPCMDPAR0(ep), 0);
    write32(dev->regs + DWC3_DEPCMDPAR1(ep), 0);
    write32(dev->regs + DWC3_DEPCMDPAR2(ep), 0);
    write32(dev->regs + DWC3_DEPCMD(ep), DWC3_DEPCMD_ENDTRANSFER |
                                             DWC3_DEPCMD_PARAM(dev->endpoints[ep].resource_index) |
                                             DWC3_DEPCMD_CMDACT);
    mdelay(1);
    if (phy & DWC3_GUSB2PHYCFG_SUSPHY)
        set32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_SUSPHY);
    return 0;
}

/* The databook allows ENDTRANSFER up to 100 ms; the controller usually answers in microseconds. */
#define USB_DWC3_RETIRE_TIMEOUT_US 100000

/*
 * Shutdown retires each active transfer before its memory is released: a
 * forced ENDTRANSFER (no wait for the host) with an interrupt on completion,
 * then its command-complete event, bounded. Unlike usb_dwc3_end_transfer(),
 * this may wait on events: during shutdown usb_dwc3_handle_event() handles
 * only completions, so no EP0 request is re-entered. A transfer that
 * completes while another one is retired needs no ENDTRANSFER of its own.
 */
static int usb_dwc3_retire_transfer(dwc3_dev_t *dev, u8 ep)
{
    dev->endpoints[ep].end_cmd_pending = true;
    if (usb_dwc3_ep_command(dev, ep,
                            DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_CMDIOC |
                                DWC3_DEPCMD_HIPRI_FORCERM |
                                DWC3_DEPCMD_PARAM(dev->endpoints[ep].resource_index),
                            0, 0, 0)) {
        dev->endpoints[ep].end_cmd_pending = false;
        return -1;
    }
    u64 deadline = timeout_calculate(USB_DWC3_RETIRE_TIMEOUT_US);
    while (dev->endpoints[ep].end_cmd_pending && !dev->failed && !timeout_expired(deadline))
        usb_dwc3_handle_events(dev);
    if (dev->failed || dev->endpoints[ep].end_cmd_pending) {
        dev->endpoints[ep].end_cmd_pending = false;
        return -1;
    }
    dev->endpoints[ep].xfer_in_progress = false;
    return 0;
}

static int usb_dwc3_ep_configure(dwc3_dev_t *dev, u8 ep, u8 type, u32 max_packet_len)
{
    u32 param0, param1;

    param0 = DWC3_DEPCFG_EP_TYPE(type) | DWC3_DEPCFG_MAX_PACKET_SIZE(max_packet_len);
    if (type != DWC3_DEPCMD_TYPE_CONTROL)
        param0 |= DWC3_DEPCFG_FIFO_NUMBER(ep);

    param1 =
        DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN | DWC3_DEPCFG_EP_NUMBER(ep);
#if USB_DWC3_SUPERSPEED
    if (type == DWC3_DEPCMD_TYPE_BULK)
        param1 |= DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
#endif

    if (usb_dwc3_ep_command(dev, ep, DWC3_DEPCMD_SETEPCONFIG, param0, param1, 0)) {
        usb_debug_printf("cannot issue DWC3_DEPCMD_SETEPCONFIG for EP %d.\n", ep);
        return -1;
    }

    if (usb_dwc3_ep_command(dev, ep, DWC3_DEPCMD_SETTRANSFRESOURCE, 1, 0, 0)) {
        usb_debug_printf("cannot issue DWC3_DEPCMD_SETTRANSFRESOURCE EP %d.\n", ep);
        return -1;
    }

    return 0;
}

static int usb_dwc3_ep_start_transfer(dwc3_dev_t *dev, u8 ep, uintptr_t trb_iova)
{
    if (dev->failed)
        return -1;
    if (dev->endpoints[ep].xfer_in_progress) {
        usb_debug_printf(
            "Tried to start a transfer for ep 0x%02x while another transfer is ongoing.\n", ep);
        return -1;
    }

    dma_wmb();
    int ret =
        usb_dwc3_ep_command(dev, ep, DWC3_DEPCMD_STARTTRANSFER, trb_iova >> 32, (u32)trb_iova, 0);
    if (ret) {
        usb_debug_printf("cannot issue DWC3_DEPCMD_STARTTRANSFER for EP %d: %d.\n", ep, ret);
#if USB_DWC3_SUPERSPEED
        usb_dwc3_fail_session(dev);
#endif
        return ret;
    }

    dev->endpoints[ep].xfer_in_progress = true;
    dev->endpoints[ep].resource_index =
        DWC3_DEPCMD_GET_RSC_IDX(read32(dev->regs + DWC3_DEPCMD(ep)));
    return 0;
}

static uintptr_t usb_dwc3_init_trb(dwc3_dev_t *dev, u8 ep, struct dwc3_trb **trb)
{
    struct dwc3_trb *next_trb = dev->endpoints[ep].trb;
    uintptr_t next_iova = dev->endpoints[ep].trb_iova;
#if USB_DWC3_SUPERSPEED
    if (ep == USB_LEP_CTRL_IN) {
        next_trb = dev->endpoints[USB_LEP_CTRL_OUT].trb;
        next_iova = dev->endpoints[USB_LEP_CTRL_OUT].trb_iova;
    }
#endif

    if (trb)
        *trb = next_trb;

    next_trb->ctrl = DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_LST;
#if USB_DWC3_SUPERSPEED
    next_trb->ctrl |= DWC3_TRB_CTRL_IOC;
#endif
    next_trb->size = DWC3_TRB_SIZE_LENGTH(0);
    next_trb->bph = 0;
    next_trb->bpl = dev->endpoints[ep].xfer_buffer_iova;

    return next_iova;
}

static bool usb_dwc3_trb_available(dwc3_dev_t *dev, u8 ep)
{
    if (dev->failed || dev->endpoints[ep].xfer_in_progress)
        return false;
#if USB_DWC3_SUPERSPEED
    /* Both EP0 directions use the OUT TRB slot on this controller. */
    if (ep <= USB_LEP_CTRL_IN && (dev->endpoints[USB_LEP_CTRL_OUT].xfer_in_progress ||
                                  dev->endpoints[USB_LEP_CTRL_IN].xfer_in_progress))
        return false;
#endif
    return true;
}

static int usb_dwc3_run_data_trb(dwc3_dev_t *dev, u8 ep, u32 data_len)
{
    if (!usb_dwc3_trb_available(dev, ep))
        return -1;
    struct dwc3_trb *trb;
    uintptr_t trb_iova = usb_dwc3_init_trb(dev, ep, &trb);

    trb->ctrl |= DWC3_TRBCTL_CONTROL_DATA;
    trb->size = DWC3_TRB_SIZE_LENGTH(data_len);

    return usb_dwc3_ep_start_transfer(dev, ep, trb_iova);
}

static int usb_dwc3_start_setup_phase(dwc3_dev_t *dev)
{
    if (!usb_dwc3_trb_available(dev, USB_LEP_CTRL_OUT))
        return -1;
    struct dwc3_trb *trb;
    uintptr_t trb_iova = usb_dwc3_init_trb(dev, USB_LEP_CTRL_OUT, &trb);

    trb->ctrl |= DWC3_TRBCTL_CONTROL_SETUP;
    trb->size = DWC3_TRB_SIZE_LENGTH(sizeof(union usb_setup_packet));
    return usb_dwc3_ep_start_transfer(dev, USB_LEP_CTRL_OUT, trb_iova);
}

static int usb_dwc3_start_status_phase(dwc3_dev_t *dev, u8 ep)
{
    if (!usb_dwc3_trb_available(dev, ep))
        return -1;
    struct dwc3_trb *trb;
    uintptr_t trb_iova = usb_dwc3_init_trb(dev, ep, &trb);

    /* The status stage of a transfer with a data stage is CONTROL_STATUS3, at every speed;
     * CONTROL_STATUS2 is only for a two-stage transfer. */
    trb->ctrl |= dev->ep0_three_stage ? DWC3_TRBCTL_CONTROL_STATUS3 : DWC3_TRBCTL_CONTROL_STATUS2;
    trb->size = DWC3_TRB_SIZE_LENGTH(0);
#if USB_DWC3_SUPERSPEED
    trb->bpl = trb_iova;
    trb->bph = trb_iova >> 32;
#endif

    return usb_dwc3_ep_start_transfer(dev, ep, trb_iova);
}

static int usb_dwc3_ep0_start_data_send_phase(dwc3_dev_t *dev)
{
    if (dev->ep0_buffer_len > XFER_BUFFER_BYTES_PER_EP) {
        usb_debug_printf("Cannot xfer more than %d bytes but was requested to xfer %d on ep 1\n",
                         XFER_BUFFER_BYTES_PER_EP, dev->ep0_buffer_len);
        return -1;
    }

    memset(dev->endpoints[USB_LEP_CTRL_IN].xfer_buffer, 0, CDC_CONTROL_PACKET_SIZE);
    memcpy(dev->endpoints[USB_LEP_CTRL_IN].xfer_buffer, dev->ep0_buffer, dev->ep0_buffer_len);

    return usb_dwc3_run_data_trb(dev, USB_LEP_CTRL_IN, dev->ep0_buffer_len);
}

static int usb_dwc3_ep0_start_data_recv_phase(dwc3_dev_t *dev)
{
    if (dev->ep0_buffer_len > XFER_BUFFER_BYTES_PER_EP) {
        usb_debug_printf("Cannot xfer more than %d bytes but was requested to xfer %d on ep 0\n",
                         XFER_BUFFER_BYTES_PER_EP, dev->ep0_buffer_len);
        return -1;
    }

    memset(dev->endpoints[USB_LEP_CTRL_OUT].xfer_buffer, 0, CDC_CONTROL_PACKET_SIZE);

    return usb_dwc3_run_data_trb(dev, USB_LEP_CTRL_OUT, CDC_CONTROL_PACKET_SIZE);
}

static void usb_dwc3_ep_set_stall(dwc3_dev_t *dev, u8 ep, u8 stall)
{
    if (ep >= MAX_ENDPOINTS || (!stall && ep <= USB_LEP_CTRL_IN && !dev->endpoints[ep].stalled))
        return;
    if (!usb_dwc3_ep_command(dev, ep, stall ? DWC3_DEPCMD_SETSTALL : DWC3_DEPCMD_CLEARSTALL, 0, 0,
                             0))
        dev->endpoints[ep].stalled = stall;
}

#if USB_DWC3_SUPERSPEED
static void usb_dwc3_fail_session(dwc3_dev_t *dev)
{
    dev->failed = true;
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
        dev->pipe[i].ready = false;
    write32(dev->regs + DWC3_DEVTEN, 0);
    clear32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST);
    if (poll32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST, 0, 250000)) {
        dev->dma_unsafe = true;
        usb_debug_printf("controller reset failed; preserving DMA mappings\n");
    }
}
#endif

static void usb_dwc3_close_pipe(dwc3_dev_t *dev, int pipe)
{
    dev->pipe[pipe].ready = false;
#if USB_DWC3_SUPERSPEED
    if (pipe == CDC_ACM_PIPE_0)
        dev->primary_dtr_pending = false;
    dev->pipe[pipe].host2device->read = dev->pipe[pipe].host2device->write;
    dev->pipe[pipe].device2host->read = dev->pipe[pipe].device2host->write;

    u8 endpoints[] = {dev->pipe[pipe].ep_in, dev->pipe[pipe].ep_out};
    for (size_t i = 0; i < ARRAY_SIZE(endpoints); i++) {
        u8 ep = endpoints[i];
        if (dev->endpoints[ep].xfer_in_progress) {
            int status = usb_dwc3_end_transfer(dev, ep);
            if (!usb_cdc_dma_may_release(true, status, false)) {
                usb_dwc3_fail_session(dev);
                return;
            }
        }
        dev->endpoints[ep].xfer_in_progress = false;
        dev->endpoints[ep].resource_index = 0;
        dev->endpoints[ep].zlp_pending = false;
        dev->endpoints[ep].trb_count = 0;
        dev->endpoints[ep].completed_trbs = 0;
        dev->endpoints[ep].transfer_length = 0;
    }
#endif
}

#if USB_DWC3_SUPERSPEED
static int usb_dwc3_configure_cdc_endpoints(dwc3_dev_t *dev)
{
    /* DEPSTARTCFG(2) discards the old non-control transfer resources. The
     * databook requires a fresh configuration for each SET_CONFIGURATION. */
    if (usb_dwc3_ep_command(dev, USB_LEP_CTRL_OUT, DWC3_DEPCMD_DEPSTARTCFG | DWC3_DEPCMD_PARAM(2),
                            0, 0, 0))
        return -1;

    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        if (usb_dwc3_ep_configure(dev, dev->pipe[i].ep_intr, DWC3_DEPCMD_TYPE_INTR, 64) ||
            usb_dwc3_ep_configure(dev, dev->pipe[i].ep_in, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE) ||
            usb_dwc3_ep_configure(dev, dev->pipe[i].ep_out, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE))
            return -1;
    }
    return 0;
}
#endif

static void usb_build_serial(void)
{
    if (str_serial)
        return;

#if USB_DWC3_SUPERSPEED
    str_serial = &str_serial_dummy;
    return;
#endif

    const char *serial = adt_getprop(adt, 0, "serial-number", NULL);
    if (!serial || !serial[0]) {
        str_serial = &str_serial_dummy;
        return;
    }

    size_t len = strlen(serial);
    size_t size = sizeof(struct usb_string_descriptor) + 2 * len;

    struct usb_string_descriptor *desc = calloc(1, size);
    memset(desc, 0, size);
    desc->bLength = size;
    desc->bDescriptorType = USB_STRING_DESCRIPTOR;
    for (size_t i = 0; i < len; i++)
        desc->bString[i] = serial[i];

    str_serial = desc;
}

static void usb_cdc_get_string_descriptor(u32 index, const void **descriptor, u16 *descriptor_len)
{
    switch (index) {
        case STRING_DESCRIPTOR_LANGUAGES:
            *descriptor = &str_langs;
            *descriptor_len = str_langs.bLength;
            break;
        case STRING_DESCRIPTOR_MANUFACTURER:
            *descriptor = &str_manufacturer;
            *descriptor_len = str_manufacturer.bLength;
            break;
        case STRING_DESCRIPTOR_PRODUCT:
            *descriptor = &str_product;
            *descriptor_len = str_product.bLength;
            break;
        case STRING_DESCRIPTOR_SERIAL:
            usb_build_serial();
            *descriptor = str_serial;
            *descriptor_len = str_serial->bLength;
            break;
        default:
            *descriptor = NULL;
            *descriptor_len = 0;
    }

    // FIXME: handle descriptors larger than maxPacketSize for EP 0
    // limit the descriptor length to stay below EP 0's maxPacketSize of 64
    *descriptor_len = min(*descriptor_len, DWC3_EP0_MAX_DESC_LEN);
}

static int
usb_dwc3_handle_ep0_get_descriptor(dwc3_dev_t *dev,
                                   const struct usb_setup_packet_get_descriptor *get_descriptor)
{
    const void *descriptor = NULL;
    u16 descriptor_len = 0;

    switch (get_descriptor->type) {
        case USB_DEVICE_DESCRIPTOR:
#if USB_DWC3_SUPERSPEED
            descriptor = &usb_cdc_ss_device_descriptor;
            descriptor_len = usb_cdc_ss_device_descriptor.bLength;
#else
            descriptor = &usb_cdc_device_descriptor;
            descriptor_len = usb_cdc_device_descriptor.bLength;
#endif
            break;
        case USB_CONFIGURATION_DESCRIPTOR:
#if USB_DWC3_SUPERSPEED
            descriptor = usb_cdc_ss_configuration;
            descriptor_len = usb_cdc_ss_configuration_len;
#else
            descriptor = &cdc_configuration_descriptor;
            descriptor_len = cdc_configuration_descriptor.configuration.wTotalLength;
#endif
            break;
#if USB_DWC3_SUPERSPEED
        case 0x0f:
            descriptor = usb_cdc_ss_bos(NULL);
            descriptor_len = CDC_SS_BOS_BYTES;
            break;
#endif
        case USB_STRING_DESCRIPTOR:
            usb_cdc_get_string_descriptor(get_descriptor->index, &descriptor, &descriptor_len);
            break;
        case USB_DEVICE_QUALIFIER_DESCRIPTOR:
            descriptor = &usb_cdc_device_qualifier_descriptor;
            descriptor_len = usb_cdc_device_qualifier_descriptor.bLength;
            break;
        default:
            usb_debug_printf("Unknown descriptor type: %d\n", get_descriptor->type);
            break;
    }

    if (descriptor) {
        dev->ep0_buffer = descriptor;
        dev->ep0_buffer_len = min(get_descriptor->wLength, descriptor_len);
        return 0;
    } else {
        return -1;
    }
}

static void usb_dwc3_ep0_handle_standard_device(dwc3_dev_t *dev,
                                                const union usb_setup_packet *setup)
{
    switch (setup->raw.bRequest) {
        case USB_REQUEST_SET_ADDRESS:
            mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_DEVADDR_MASK,
                   DWC3_DCFG_DEVADDR(setup->set_address.address));
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
            break;

        case USB_REQUEST_SET_CONFIGURATION:
            switch (setup->set_configuration.configuration) {
                case 0:
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_OUT));
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_IN));
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_INTR_IN));
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_OUT_2));
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_IN_2));
                    clear32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_INTR_IN_2));
                    dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
                    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
                        usb_dwc3_close_pipe(dev, i);
#if USB_DWC3_SUPERSPEED
                    dev->cdc_reconfigure = true;
#endif
                    break;
                case 1:
#if USB_DWC3_SUPERSPEED
                    if (dev->cdc_reconfigure) {
                        write32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CTRL_OUT) |
                                                               DWC3_DALEPENA_EP(USB_LEP_CTRL_IN));
                        for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
                            usb_dwc3_close_pipe(dev, i);
                        if (dev->failed || usb_dwc3_configure_cdc_endpoints(dev)) {
                            usb_dwc3_fail_session(dev);
                            return;
                        }
                        dev->cdc_reconfigure = false;
                        if (dev->reset_in_progress)
                            dev->configured_during_reset = true;
                    }
#endif
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_OUT));
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_IN));
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_INTR_IN));
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_OUT_2));
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_BULK_IN_2));
                    set32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(USB_LEP_CDC_INTR_IN_2));
#if USB_DWC3_SUPERSPEED
                    dev->cdc_ever_configured = true;
#endif
                    dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
                    break;
                default:
                    usb_dwc3_ep_set_stall(dev, 0, 1);
                    dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
                    break;
            }
            break;

        case USB_REQUEST_GET_DESCRIPTOR:
            if (usb_dwc3_handle_ep0_get_descriptor(dev, &setup->get_descriptor) < 0) {
                usb_dwc3_ep_set_stall(dev, 0, 1);
                dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            } else {
                dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND;
            }
            break;

        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0001; // self-powered
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND;
            break;
        }

        default:
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            usb_debug_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc3_ep0_handle_standard_interface(dwc3_dev_t *dev,
                                                   const union usb_setup_packet *setup)
{
    switch (setup->raw.bRequest) {
        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0000; // reserved
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND;
            break;
        }
        default:
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            usb_debug_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc3_ep0_handle_standard_endpoint(dwc3_dev_t *dev,
                                                  const union usb_setup_packet *setup)
{
    switch (setup->raw.bRequest) {
        case USB_REQUEST_GET_STATUS: {
            static const u16 device_status = 0x0000; // reserved
            dev->ep0_buffer = &device_status;
            dev->ep0_buffer_len = 2;
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND;
            break;
        }
        case USB_REQUEST_CLEAR_FEATURE: {
            switch (setup->feature.wFeatureSelector) {
                case USB_FEATURE_ENDPOINT_HALT:
                    usb_debug_printf("Host cleared EP 0x%x stall\n", setup->feature.wEndpoint);
                    if ((setup->feature.wEndpoint & 0xff70) ||
                        ep_to_num(setup->feature.wEndpoint) >= MAX_ENDPOINTS) {
                        usb_dwc3_ep_set_stall(dev, 0, 1);
                        dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
                        break;
                    }
                    usb_dwc3_ep_set_stall(dev, ep_to_num(setup->feature.wEndpoint), 0);
                    dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
                    break;
                default:
                    usb_dwc3_ep_set_stall(dev, 0, 1);
                    dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
                    usb_debug_printf("unsupported CLEAR FEATURE: 0x%x\n",
                                     setup->feature.wFeatureSelector);
                    break;
            }
            break;
        }
        default:
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            usb_debug_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc3_ep0_handle_standard(dwc3_dev_t *dev, const union usb_setup_packet *setup)
{
    switch (setup->raw.bmRequestType & USB_REQUEST_TYPE_RECIPIENT_MASK) {
        case USB_REQUEST_TYPE_RECIPIENT_DEVICE:
            usb_dwc3_ep0_handle_standard_device(dev, setup);
            break;

        case USB_REQUEST_TYPE_RECIPIENT_INTERFACE:
            usb_dwc3_ep0_handle_standard_interface(dev, setup);
            break;

        case USB_REQUEST_TYPE_RECIPIENT_ENDPOINT:
            usb_dwc3_ep0_handle_standard_endpoint(dev, setup);
            break;

        default:
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            usb_debug_printf("unimplemented request recipient\n");
    }
}

static void usb_dwc3_ep0_handle_class(dwc3_dev_t *dev, const union usb_setup_packet *setup)
{
    int pipe = setup->raw.wIndex / 2;
    if (pipe >= CDC_ACM_PIPE_MAX) {
        usb_dwc3_ep_set_stall(dev, USB_LEP_CTRL_OUT, 1);
        dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
        return;
    }

    switch (setup->raw.bRequest) {
        case USB_REQUEST_CDC_GET_LINE_CODING:
            dev->ep0_buffer_len = min(setup->raw.wLength, sizeof(dev->pipe[pipe].cdc_line_coding));
            dev->ep0_buffer = dev->pipe[pipe].cdc_line_coding;
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND;
            break;

        case USB_REQUEST_CDC_SET_CTRL_LINE_STATE:
            usb_dwc3_close_pipe(dev, pipe);
            if (dev->failed)
                return;
            if (setup->raw.wValue & 1) { // DTR
                usb_debug_printf("ACM device opened\n");
                dev->pipe[pipe].ready = true;
                if (pipe == CDC_ACM_PIPE_0) {
#if USB_DWC3_SUPERSPEED
                    dev->primary_dtr_pending = true;
#endif
                }
            } else {
                usb_debug_printf("ACM device closed\n");
            }
            /*
             * The status TRB of a request without a data stage may only be started once the
             * controller reports XferNotReady for the status stage, like SET_ADDRESS and
             * SET_CONFIGURATION above. Starting it from the SETUP completion races the host's
             * status token. When the TRB wins on T8132, the host still sees the request
             * complete, but the controller reports XferNotReady instead of XferComplete, keeps
             * the TRB and raises no further event on any endpoint.
             */
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
            break;

        case USB_REQUEST_CDC_SET_LINE_CODING:
            dev->ep0_read_buffer = dev->pipe[pipe].cdc_line_coding;
            dev->ep0_read_buffer_len =
                min(setup->raw.wLength, sizeof(dev->pipe[pipe].cdc_line_coding));
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_RECV;
            break;

        default:
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            usb_debug_printf("unsupported SETUP packet\n");
    }
}

static void usb_dwc3_ep0_handle_setup(dwc3_dev_t *dev)
{
    const union usb_setup_packet *setup = dev->endpoints[0].xfer_buffer;
    dev->ep0_three_stage = !!setup->raw.wLength;

    switch (setup->raw.bmRequestType & USB_REQUEST_TYPE_MASK) {
        case USB_REQUEST_TYPE_STANDARD:
            usb_dwc3_ep0_handle_standard(dev, setup);
            break;
        case USB_REQUEST_TYPE_CLASS:
            usb_dwc3_ep0_handle_class(dev, setup);
            break;
        default:
            usb_debug_printf("unsupported request type\n");
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
    }
}

static void usb_dwc3_ep0_handle_xfer_done(dwc3_dev_t *dev, const struct dwc3_event_depevt event)
{
    switch (dev->ep0_state) {
        case USB_DWC3_EP0_STATE_SETUP_HANDLE:
            usb_dwc3_ep0_handle_setup(dev);
            break;

        case USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE:
#if USB_DWC3_SUPERSPEED
            if (dev->primary_dtr_pending) {
                dev->primary_dtr_pending = false;
                dev->primary_was_open = true;
                usb_cdc_recovery_connected(&dev->recovery);
                usb_cdc_primary_opened();
            }
#endif
            /* fall through */
        case USB_DWC3_EP0_STATE_DATA_RECV_STATUS_DONE:
            dev->ep0_state = usb_dwc3_start_setup_phase(dev) ? USB_DWC3_EP0_STATE_IDLE
                                                             : USB_DWC3_EP0_STATE_SETUP_HANDLE;
            break;

        case USB_DWC3_EP0_STATE_DATA_SEND_DONE:
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_RECV_STATUS;
            break;

        case USB_DWC3_EP0_STATE_DATA_RECV_DONE:
            memcpy(dev->ep0_read_buffer, dev->endpoints[event.endpoint_number].xfer_buffer,
                   dev->ep0_read_buffer_len);
            dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_STATUS;
            break;

        case USB_DWC3_EP0_STATE_IDLE:
        default:
            usb_debug_printf("invalid state in usb_dwc3_ep0_handle_xfer_done: %d, %s\n",
                             dev->ep0_state, ep0_state_names[dev->ep0_state]);
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
    }
}

static void usb_dwc3_ep0_handle_xfer_not_ready(dwc3_dev_t *dev,
                                               const struct dwc3_event_depevt event)
{
    switch (dev->ep0_state) {
        case USB_DWC3_EP0_STATE_IDLE:
            if (!usb_dwc3_start_setup_phase(dev))
                dev->ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE;
            break;

        case USB_DWC3_EP0_STATE_DATA_SEND:
            if (usb_dwc3_ep0_start_data_send_phase(dev)) {
                usb_debug_printf("cannot start xtrl xfer data phase for EP 1.\n");
                dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            } else {
                dev->ep0_state = USB_DWC3_EP0_STATE_DATA_SEND_DONE;
            }
            break;

        case USB_DWC3_EP0_STATE_DATA_RECV:
            if (usb_dwc3_ep0_start_data_recv_phase(dev)) {
                usb_debug_printf("cannot start xtrl xfer data phase for EP 0.\n");
                dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
            } else {
                dev->ep0_state = USB_DWC3_EP0_STATE_DATA_RECV_DONE;
            }
            break;

        case USB_DWC3_EP0_STATE_DATA_RECV_STATUS:
            dev->ep0_state = usb_dwc3_start_status_phase(dev, USB_LEP_CTRL_OUT)
                                 ? USB_DWC3_EP0_STATE_IDLE
                                 : USB_DWC3_EP0_STATE_DATA_RECV_STATUS_DONE;
            break;

        case USB_DWC3_EP0_STATE_DATA_SEND_STATUS:
            dev->ep0_state = usb_dwc3_start_status_phase(dev, USB_LEP_CTRL_IN)
                                 ? USB_DWC3_EP0_STATE_IDLE
                                 : USB_DWC3_EP0_STATE_DATA_SEND_STATUS_DONE;
            break;

        default:
            usb_debug_printf(
                "invalid state in usb_dwc3_ep0_handle_xfer_not_ready: %d, %s for ep %d (%x)\n",
                dev->ep0_state, ep0_state_names[dev->ep0_state], event.endpoint_number,
                event.endpoint_event);
            usb_dwc3_ep_set_stall(dev, 0, 1);
            dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
    }
}

static ringbuffer_t *usb_dwc3_cdc_get_ringbuffer(dwc3_dev_t *dev, u8 endpoint_number)
{
    switch (endpoint_number) {
        case USB_LEP_CDC_BULK_IN:
            return dev->pipe[CDC_ACM_PIPE_0].device2host;
        case USB_LEP_CDC_BULK_OUT:
            return dev->pipe[CDC_ACM_PIPE_0].host2device;
        case USB_LEP_CDC_BULK_IN_2:
            return dev->pipe[CDC_ACM_PIPE_1].device2host;
        case USB_LEP_CDC_BULK_OUT_2:
            return dev->pipe[CDC_ACM_PIPE_1].host2device;
        default:
            return NULL;
    }
}

static int usb_dwc3_start_bulk_transfer(dwc3_dev_t *dev, u8 ep, u32 length)
{
    if (!usb_dwc3_trb_available(dev, ep))
        return -1;
#if USB_DWC3_SUPERSPEED
    u32 segments[CDC_BULK_MAX_TRBS];
    size_t count = usb_cdc_bulk_plan(length, segments, ARRAY_SIZE(segments));
    if (!count || count > TRBS_PER_EP)
        return -1;

    u32 offset = 0;
    for (size_t i = 0; i < count; i++) {
        struct dwc3_trb *trb = &dev->endpoints[ep].trb[i];
        uintptr_t buffer = dev->endpoints[ep].xfer_buffer_iova + offset;
        trb->bpl = buffer;
        trb->bph = buffer >> 32;
        trb->size = DWC3_TRB_SIZE_LENGTH(segments[i]);
        bool out = ep == USB_LEP_CDC_BULK_OUT || ep == USB_LEP_CDC_BULK_OUT_2;
        trb->ctrl = DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRBCTL_NORMAL |
                    (out ? DWC3_TRB_CTRL_CSP | DWC3_TRB_CTRL_IOC : 0) |
                    (i + 1 == count ? DWC3_TRB_CTRL_LST | DWC3_TRB_CTRL_IOC : DWC3_TRB_CTRL_CHN);
        offset += segments[i];
    }
    dev->endpoints[ep].trb_count = count;
#else
    struct dwc3_trb *trb;
    usb_dwc3_init_trb(dev, ep, &trb);
    trb->ctrl |= DWC3_TRBCTL_NORMAL;
    trb->size = DWC3_TRB_SIZE_LENGTH(length);
    dev->endpoints[ep].trb_count = 1;
#endif
    dev->endpoints[ep].transfer_length = length;
    dev->endpoints[ep].completed_trbs = 0;
    if (usb_dwc3_ep_start_transfer(dev, ep, dev->endpoints[ep].trb_iova)) {
        dev->endpoints[ep].trb_count = 0;
        return -1;
    }
    return 0;
}

static void usb_dwc3_cdc_start_bulk_out_xfer(dwc3_dev_t *dev, u8 endpoint_number)
{
    if (!usb_dwc3_trb_available(dev, endpoint_number))
        return;
    if (dev->endpoints[endpoint_number].xfer_in_progress)
        return;

    if ((endpoint_number == USB_LEP_CDC_BULK_OUT && !dev->pipe[0].ready) ||
        (endpoint_number == USB_LEP_CDC_BULK_OUT_2 && !dev->pipe[1].ready))
        return;

    ringbuffer_t *host2device = usb_dwc3_cdc_get_ringbuffer(dev, endpoint_number);
    if (!host2device)
        return;

    if (ringbuffer_get_free(host2device) < XFER_SIZE)
        return;

#if !USB_DWC3_SUPERSPEED
    memset(dev->endpoints[endpoint_number].xfer_buffer, 0xaa, XFER_SIZE);
#endif
    usb_dwc3_start_bulk_transfer(dev, endpoint_number, XFER_SIZE);
}

static void usb_dwc3_cdc_start_bulk_in_xfer(dwc3_dev_t *dev, u8 endpoint_number)
{
    if (!usb_dwc3_trb_available(dev, endpoint_number))
        return;
    if (dev->endpoints[endpoint_number].xfer_in_progress)
        return;

    if ((endpoint_number == USB_LEP_CDC_BULK_IN && !dev->pipe[0].ready) ||
        (endpoint_number == USB_LEP_CDC_BULK_IN_2 && !dev->pipe[1].ready))
        return;

    ringbuffer_t *device2host = usb_dwc3_cdc_get_ringbuffer(dev, endpoint_number);
    if (!device2host)
        return;

    size_t previous_read = device2host->read;
    size_t len =
        ringbuffer_read(dev->endpoints[endpoint_number].xfer_buffer, XFER_SIZE, device2host);

    if (!len && !dev->endpoints[endpoint_number].zlp_pending)
        return;

    if (usb_dwc3_start_bulk_transfer(dev, endpoint_number, len)) {
        device2host->read = previous_read;
        return;
    }
    dev->endpoints[endpoint_number].zlp_pending = len && (len % CDC_BULK_PACKET_SIZE) == 0;
}

static bool usb_dwc3_cdc_copy_bulk_out_progress(dwc3_dev_t *dev, u8 ep)
{
    ringbuffer_t *host2device = usb_dwc3_cdc_get_ringbuffer(dev, ep);
    if (!host2device)
        return false;
    dma_rmb();
#if !USB_DWC3_SUPERSPEED
    u32 remaining = dev->endpoints[ep].trb->size & DWC3_TRB_SIZE_MASK;
    if (remaining > XFER_SIZE)
        return false;
    size_t bytes = XFER_SIZE - remaining;
    return bytes <= ringbuffer_get_free(host2device) &&
           ringbuffer_write(dev->endpoints[ep].xfer_buffer, bytes, host2device) == bytes;
#else
    /* A short OUT packet can leave a gap before the next chained TRB. */
    for (unsigned i = dev->endpoints[ep].completed_trbs; i < dev->endpoints[ep].trb_count; i++) {
        struct dwc3_trb *trb = &dev->endpoints[ep].trb[i];
        u32 expected = min((u32)CDC_BULK_TRB_BYTES,
                           dev->endpoints[ep].transfer_length - i * CDC_BULK_TRB_BYTES);
        u32 remaining = trb->size & DWC3_TRB_SIZE_MASK;
        int actual =
            usb_cdc_bulk_retired_bytes(expected, remaining, DWC3_TRB_SIZE_TRBSTS(trb->size),
                                       !!(trb->ctrl & DWC3_TRB_CTRL_HWO));
        if (actual == -1)
            break;
        if (actual < 0) {
            usb_debug_printf("bad bulk completion on EP %u, TRB %u\n", ep, i);
            usb_dwc3_close_pipe(dev, ep == USB_LEP_CDC_BULK_OUT ? 0 : 1);
            return false;
        }
        const u8 *source = (const u8 *)dev->endpoints[ep].xfer_buffer + i * CDC_BULK_TRB_BYTES;
        size_t bytes = actual;
        if (bytes > ringbuffer_get_free(host2device) ||
            ringbuffer_write(source, bytes, host2device) != bytes) {
            usb_dwc3_close_pipe(dev, ep == USB_LEP_CDC_BULK_OUT ? 0 : 1);
            return false;
        }
        dev->endpoints[ep].completed_trbs = i + 1;
    }
    return true;
#endif
}

static void usb_dwc3_handle_event_ep(dwc3_dev_t *dev, const struct dwc3_event_depevt event)
{
    if (event.endpoint_number >= MAX_ENDPOINTS)
        return;
#if USB_DWC3_SUPERSPEED
    if (dev->reset_in_progress && event.endpoint_number <= USB_LEP_CTRL_IN &&
        event.endpoint_event != DWC3_DEPEVT_EPCMDCMPLT)
        dev->ep0_progressed_during_reset = true;
#endif
    if (event.endpoint_event == DWC3_DEPEVT_EPCMDCMPLT) {
        return;
    }

#if USB_DWC3_SUPERSPEED
    /* ENDTRANSFER polls events recursively. Do not restart a retired CDC
     * transfer from an old completion or NRDY while reset is in progress. */
    if (dev->reset_in_progress && event.endpoint_number > USB_LEP_CTRL_IN)
        return;
#endif

#if USB_DWC3_SUPERSPEED
    if (event.endpoint_event == DWC3_DEPEVT_XFERINPROGRESS) {
        u8 ep = event.endpoint_number;
        if (!dev->endpoints[ep].xfer_in_progress)
            return;
        if (event.status & DEPEVT_STATUS_BUSERR) {
            if (ep <= USB_LEP_CTRL_IN) {
                usb_dwc3_fail_session(dev);
                return;
            }
            if (ep == USB_LEP_CDC_BULK_OUT || ep == USB_LEP_CDC_BULK_IN)
                usb_dwc3_close_pipe(dev, 0);
            else if (ep == USB_LEP_CDC_BULK_OUT_2 || ep == USB_LEP_CDC_BULK_IN_2)
                usb_dwc3_close_pipe(dev, 1);
            return;
        }
        if (ep == USB_LEP_CDC_BULK_OUT || ep == USB_LEP_CDC_BULK_OUT_2)
            usb_dwc3_cdc_copy_bulk_out_progress(dev, ep);
        return;
    }
#endif

    if (event.endpoint_event == DWC3_DEPEVT_XFERCOMPLETE) {
        if (!dev->endpoints[event.endpoint_number].xfer_in_progress)
            return;
#if USB_DWC3_SUPERSPEED
        if (event.status & DEPEVT_STATUS_BUSERR) {
            u8 ep = event.endpoint_number;
            if (ep <= USB_LEP_CTRL_IN) {
                usb_dwc3_fail_session(dev);
                return;
            }
            if (ep == USB_LEP_CDC_BULK_OUT || ep == USB_LEP_CDC_BULK_IN)
                usb_dwc3_close_pipe(dev, 0);
            else if (ep == USB_LEP_CDC_BULK_OUT_2 || ep == USB_LEP_CDC_BULK_IN_2)
                usb_dwc3_close_pipe(dev, 1);
            return;
        }
#endif
        dev->endpoints[event.endpoint_number].xfer_in_progress = false;
        dev->endpoints[event.endpoint_number].resource_index = 0;

        switch (event.endpoint_number) {
            case USB_LEP_CTRL_IN:
            case USB_LEP_CTRL_OUT:
                return usb_dwc3_ep0_handle_xfer_done(dev, event);
            case USB_LEP_CDC_INTR_IN: // [[fallthrough]]
            case USB_LEP_CDC_INTR_IN_2:
                return;
            case USB_LEP_CDC_BULK_IN: // [[fallthrough]]
            case USB_LEP_CDC_BULK_IN_2:
#if USB_DWC3_SUPERSPEED
                return usb_dwc3_cdc_start_bulk_in_xfer(dev, event.endpoint_number);
#else
                return;
#endif
            case USB_LEP_CDC_BULK_OUT: // [[fallthrough]]
            case USB_LEP_CDC_BULK_OUT_2:
                if (usb_dwc3_cdc_copy_bulk_out_progress(dev, event.endpoint_number)) {
#if USB_DWC3_SUPERSPEED
                    return usb_dwc3_cdc_start_bulk_out_xfer(dev, event.endpoint_number);
#endif
                }
                return;
        }
    } else if (event.endpoint_event == DWC3_DEPEVT_XFERNOTREADY) {
        /*
         * this might be a bug, we sometimes get spurious events like these here.
         * ignoring them works just fine though
         */
        if (dev->endpoints[event.endpoint_number].xfer_in_progress)
            return;
#if USB_DWC3_SUPERSPEED
        /* EP0 is one control pipe; an NRDY on the other direction is stale. */
        if (event.endpoint_number <= USB_LEP_CTRL_IN &&
            (dev->endpoints[USB_LEP_CTRL_OUT].xfer_in_progress ||
             dev->endpoints[USB_LEP_CTRL_IN].xfer_in_progress))
            return;
#endif

        switch (event.endpoint_number) {
            case USB_LEP_CTRL_IN:
            case USB_LEP_CTRL_OUT:
                return usb_dwc3_ep0_handle_xfer_not_ready(dev, event);
            case USB_LEP_CDC_INTR_IN: // [[fallthrough]]
            case USB_LEP_CDC_INTR_IN_2:
                return;
            case USB_LEP_CDC_BULK_IN: // [[fallthrough]]
            case USB_LEP_CDC_BULK_IN_2:
                return usb_dwc3_cdc_start_bulk_in_xfer(dev, event.endpoint_number);
            case USB_LEP_CDC_BULK_OUT: // [[fallthrough]]
            case USB_LEP_CDC_BULK_OUT_2:
                return usb_dwc3_cdc_start_bulk_out_xfer(dev, event.endpoint_number);
        }
    }

    const char *name = event.endpoint_event < ARRAY_SIZE(depvt_names)
                           ? depvt_names[event.endpoint_event]
                           : "Unknown";
    usb_debug_printf("unhandled EP %02x event: %s (0x%02x) (%d)\n", event.endpoint_number, name,
                     event.endpoint_event, dev->endpoints[event.endpoint_number].xfer_in_progress);
    usb_dwc3_ep_set_stall(dev, event.endpoint_number, 1);
}

static void usb_dwc3_handle_event_usbrst(dwc3_dev_t *dev, bool rearm_setup)
{
#if USB_DWC3_SUPERSPEED
    if (dev->reset_in_progress)
        return;
    dev->reset_in_progress = true;
    dev->ep0_progressed_during_reset = false;
    dev->configured_during_reset = false;
    dev->cdc_reconfigure = dev->cdc_ever_configured;
    /* These writes must precede ENDTRANSFER's recursive event polling: a
     * nested SET_ADDRESS or SET_CONFIGURATION belongs to the new bus state. */
    mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_DEVADDR_MASK, DWC3_DCFG_DEVADDR(0));
    write32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(0) | DWC3_DALEPENA_EP(1));
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        dev->pipe[i].ready = false;
        dev->pipe[i].host2device->read = dev->pipe[i].host2device->write;
        dev->pipe[i].device2host->read = dev->pipe[i].device2host->write;
    }
    dev->primary_dtr_pending = false;

    for (int i = 2; i < MAX_ENDPOINTS; ++i) {
        if (dev->endpoints[i].xfer_in_progress) {
            int status = usb_dwc3_end_transfer(dev, i);
            if (!usb_cdc_dma_may_release(true, status, false)) {
                usb_debug_printf("ENDTRANSFER failed for EP %d on reset\n", i);
                dev->reset_in_progress = false;
                usb_dwc3_fail_session(dev);
                return;
            }
        }
        /* A nested SET_CONFIGURATION has already replaced the old endpoint
         * resources. Do not clear its new transfer state or DMA buffers. */
        if (dev->configured_during_reset)
            break;
        dev->endpoints[i].xfer_in_progress = false;
        dev->endpoints[i].resource_index = 0;
        dev->endpoints[i].zlp_pending = false;
        dev->endpoints[i].trb_count = 0;
        dev->endpoints[i].completed_trbs = 0;
        dev->endpoints[i].transfer_length = 0;
        memset(dev->endpoints[i].xfer_buffer, 0, XFER_BUFFER_BYTES_PER_EP);
        memset(dev->endpoints[i].trb, 0, TRBS_PER_EP * sizeof(struct dwc3_trb));
        if (dev->endpoints[i].stalled) {
            if (rearm_setup)
                usb_dwc3_ep_set_stall(dev, i, 0);
            else
                dev->endpoints[i].stalled = false;
        }
    }

    /* A live SETUP TRB is already the required post-reset EP0 state. Never
     * clear it or its buffer after a nested event has advanced control. */
    if (!dev->ep0_progressed_during_reset && dev->ep0_state != USB_DWC3_EP0_STATE_SETUP_HANDLE) {
        bool data_phase = dev->ep0_state == USB_DWC3_EP0_STATE_DATA_SEND_DONE ||
                          dev->ep0_state == USB_DWC3_EP0_STATE_DATA_RECV_DONE;
        for (int i = 0; i <= USB_LEP_CTRL_IN; i++) {
            if (data_phase && dev->endpoints[i].xfer_in_progress && usb_dwc3_end_transfer(dev, i)) {
                dev->reset_in_progress = false;
                usb_dwc3_fail_session(dev);
                return;
            }
            if (dev->ep0_progressed_during_reset)
                break;
            dev->endpoints[i].xfer_in_progress = false;
            dev->endpoints[i].resource_index = 0;
        }
    }
    if (!dev->ep0_progressed_during_reset && !dev->endpoints[USB_LEP_CTRL_OUT].xfer_in_progress &&
        !dev->endpoints[USB_LEP_CTRL_IN].xfer_in_progress) {
        dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
        if (rearm_setup && !usb_dwc3_start_setup_phase(dev))
            dev->ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE;
    }
    dev->reset_in_progress = false;
#else
    /* The databook requires ENDTRANSFER for every active transfer on a bus
     * reset. Clearing only the software flags left the controller owning the
     * old EP0 TRB: the new SETUP could not be armed and its completion was
     * then dropped, so EP0 never answered SET_ADDRESS again. This
     * ENDTRANSFER does not poll events, so no nested request can run here. */
    mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_DEVADDR_MASK, DWC3_DCFG_DEVADDR(0));
    write32(dev->regs + DWC3_DALEPENA, DWC3_DALEPENA_EP(0) | DWC3_DALEPENA_EP(1));
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        dev->pipe[i].ready = false;
        dev->pipe[i].host2device->read = dev->pipe[i].host2device->write;
        dev->pipe[i].device2host->read = dev->pipe[i].device2host->write;
    }

    for (int i = 2; i < MAX_ENDPOINTS; i++) {
        if (dev->endpoints[i].xfer_in_progress)
            usb_dwc3_end_transfer(dev, i);
        dev->endpoints[i].xfer_in_progress = false;
        dev->endpoints[i].resource_index = 0;
        dev->endpoints[i].zlp_pending = false;
        dev->endpoints[i].trb_count = 0;
        dev->endpoints[i].completed_trbs = 0;
        dev->endpoints[i].transfer_length = 0;
        memset(dev->endpoints[i].xfer_buffer, 0, XFER_BUFFER_BYTES_PER_EP);
        memset(dev->endpoints[i].trb, 0, TRBS_PER_EP * sizeof(struct dwc3_trb));
        /* CLEARSTALL also restarts the data sequence at DATA0. */
        usb_dwc3_ep_set_stall(dev, i, 0);
    }

    /* An armed SETUP TRB is already the post-reset EP0 state, as in Linux's
     * dwc3. Any other control transfer belongs to the old session. */
    bool setup_armed = dev->ep0_state == USB_DWC3_EP0_STATE_SETUP_HANDLE &&
                       dev->endpoints[USB_LEP_CTRL_OUT].xfer_in_progress &&
                       !dev->endpoints[USB_LEP_CTRL_IN].xfer_in_progress;
    if (!setup_armed) {
        for (int i = USB_LEP_CTRL_OUT; i <= USB_LEP_CTRL_IN; i++) {
            if (dev->endpoints[i].xfer_in_progress)
                usb_dwc3_end_transfer(dev, i);
            dev->endpoints[i].xfer_in_progress = false;
            dev->endpoints[i].resource_index = 0;
        }
        dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
        if (rearm_setup && !usb_dwc3_start_setup_phase(dev))
            dev->ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE;
    }
    usb_dwc3_ep_set_stall(dev, USB_LEP_CTRL_IN, 0);
#endif
}

static void usb_dwc3_handle_event_connect_done(dwc3_dev_t *dev)
{
#if USB_DWC3_SUPERSPEED
    usb_cdc_recovery_connected(&dev->recovery);
#endif
    u32 speed = read32(dev->regs + DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;

#if USB_DWC3_SUPERSPEED
    if (speed != DWC3_DSTS_SUPERSPEED) {
        usb_debug_printf("CDC Gen1 link mode mismatch: DSTS=%x\n", speed);
        clear32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);
        usb_cdc_link_failed();
        return;
    }
#else
    if (speed != DWC3_DSTS_HIGHSPEED) {
        usb_debug_printf(
            "WARNING: we only support high speed right now but %02x was requested in DSTS\n",
            speed);
    }
#endif

    if (!dev->endpoints[USB_LEP_CTRL_OUT].xfer_in_progress &&
        !dev->endpoints[USB_LEP_CTRL_IN].xfer_in_progress && !usb_dwc3_start_setup_phase(dev))
        dev->ep0_state = USB_DWC3_EP0_STATE_SETUP_HANDLE;
}

static void usb_dwc3_handle_event_dev(dwc3_dev_t *dev, const struct dwc3_event_devt event)
{
    const char *name = event.type < ARRAY_SIZE(devt_names) ? devt_names[event.type] : "Unknown";
    usb_debug_printf("device event: %s (0x%02x)\n", name, event.type);
    switch (event.type) {
        case DWC3_DEVT_USBRST:
            usb_dwc3_handle_event_usbrst(dev, true);
            break;
        case DWC3_DEVT_DISCONN:
            usb_dwc3_handle_event_usbrst(dev, true);
#if USB_DWC3_SUPERSPEED
            if (dev->primary_was_open)
                usb_cdc_recovery_arm(&dev->recovery, ticks_to_msecs(get_ticks()));
#endif
            break;
        case DWC3_DEVT_CONNECTDONE:
            usb_dwc3_handle_event_connect_done(dev);
            break;
        default:
            usb_debug_printf("unhandled device event: %s (0x%02x)\n", name, event.type);
    }
}

/* During shutdown: the ENDTRANSFER completions, and transfers that completed on their own. */
static void usb_dwc3_handle_shutdown_event(dwc3_dev_t *dev, const union dwc3_event event)
{
    const struct dwc3_event_depevt ep = event.depevt;

    if (event.type.is_devspec || ep.endpoint_number >= MAX_ENDPOINTS)
        return;
    if (ep.endpoint_event == DWC3_DEPEVT_EPCMDCMPLT &&
        ((ep.parameters >> 8) & 0xf) == DWC3_DEPCMD_ENDTRANSFER)
        dev->endpoints[ep.endpoint_number].end_cmd_pending = false;
    else if (ep.endpoint_event == DWC3_DEPEVT_XFERCOMPLETE && !(ep.status & DEPEVT_STATUS_BUSERR))
        dev->endpoints[ep.endpoint_number].xfer_in_progress = false;
}

static void usb_dwc3_handle_event(dwc3_dev_t *dev, const union dwc3_event event)
{
    if (dev->shutting_down)
        usb_dwc3_handle_shutdown_event(dev, event);
    else if (!event.type.is_devspec)
        usb_dwc3_handle_event_ep(dev, event.depevt);
    else if (event.type.type == DWC3_EVENT_TYPE_DEV)
        usb_dwc3_handle_event_dev(dev, event.devt);
    else
        usb_debug_printf("unknown event %08x\n", event.raw);
}

#if USB_DWC3_SUPERSPEED
static int usb_dwc3_reannounce(dwc3_dev_t *dev)
{
    if (dev->failed)
        return -1;
    clear32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);
    if (poll32(dev->regs + DWC3_DSTS, DWC3_DSTS_DEVCTRLHLT, DWC3_DSTS_DEVCTRLHLT, 250000))
        return -1;
    mdelay(20);

    u32 stale = read32(dev->regs + DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
    if (stale)
        write32(dev->regs + DWC3_GEVNTCOUNT(0), stale);
    (void)read32(dev->regs + DWC3_GEVNTCOUNT(0));
    dev->evt_buffer_offset = 0;
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST);
    if (poll32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST, 0, 250000)) {
        usb_dwc3_fail_session(dev);
        return -1;
    }
    /* A successful controller reset retires all DMA before state is cleared. */
    for (int i = 0; i < MAX_ENDPOINTS; i++) {
        if (usb_cdc_dma_may_release(dev->endpoints[i].xfer_in_progress, -1, true))
            dev->endpoints[i].xfer_in_progress = false;
    }
    usb_dwc3_handle_event_usbrst(dev, false);
    clear32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_SUSPHY);
    clear32(dev->regs + DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_SUSPHY);
    if (usb_cdc_atc_switch_pipe())
        return -1;

    mask32(dev->regs + DWC3_GCTL, DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG),
           DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
    int nump =
        usb_cdc_nump(read32(dev->regs + DWC3_GHWPARAMS0), read32(dev->regs + DWC3_GHWPARAMS7));
    if (nump < 1)
        return -1;
    clear32(dev->regs + DWC3_GRXTHRCFG, CDC_GRXTHRCFG_PKTCNTSEL);
    mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_SPEED_MASK | CDC_DCFG_NUMP_MASK,
           DWC3_DCFG_SUPERSPEED | CDC_DCFG_NUMP(nump));

    if (usb_dwc3_command(dev, DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO, SCRATCHPAD_IOVA) ||
        usb_dwc3_command(dev, DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI, 0))
        return -1;

    write32(dev->regs + DWC3_GEVNTADRLO(0), EVENT_BUFFER_IOVA);
    write32(dev->regs + DWC3_GEVNTADRHI(0), 0);
    write32(dev->regs + DWC3_GEVNTSIZ(0), DWC3_EVENT_BUFFERS_SIZE);
    write32(dev->regs + DWC3_GEVNTCOUNT(0), 0);
    write32(dev->regs + DWC3_DEVTEN,
            DWC3_DEVTEN_DISCONNEVTEN | DWC3_DEVTEN_USBRSTEN | DWC3_DEVTEN_CONNECTDONEEN);

    if (usb_dwc3_ep_command(dev, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0) ||
        usb_dwc3_ep_configure(dev, USB_LEP_CTRL_OUT, DWC3_DEPCMD_TYPE_CONTROL,
                              CDC_CONTROL_PACKET_SIZE) ||
        usb_dwc3_ep_configure(dev, USB_LEP_CTRL_IN, DWC3_DEPCMD_TYPE_CONTROL,
                              CDC_CONTROL_PACKET_SIZE))
        return -1;
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        if (usb_dwc3_ep_configure(dev, dev->pipe[i].ep_intr, DWC3_DEPCMD_TYPE_INTR, 64) ||
            usb_dwc3_ep_configure(dev, dev->pipe[i].ep_in, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE) ||
            usb_dwc3_ep_configure(dev, dev->pipe[i].ep_out, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE))
            return -1;
    }
    dev->cdc_reconfigure = false;
    dev->cdc_ever_configured = false;
    dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;
    write32(dev->regs + DWC3_DALEPENA,
            DWC3_DALEPENA_EP(USB_LEP_CTRL_OUT) | DWC3_DALEPENA_EP(USB_LEP_CTRL_IN));
    clear32(dev->regs + DWC3_DCTL, DWC3_DCTL_KEEP_CONNECT);
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);
    if (poll32(dev->regs + DWC3_DSTS, DWC3_DSTS_DEVCTRLHLT, 0, 250000))
        return -1;
    return 0;
}

static void usb_dwc3_maybe_reannounce(dwc3_dev_t *dev)
{
    if (dev->failed || dev->shutting_down)
        return;
    if (!usb_cdc_recovery_due(&dev->recovery, ticks_to_msecs(get_ticks())))
        return;
    int result = usb_dwc3_reannounce(dev);
    usb_cdc_recovery_attempted(&dev->recovery, ticks_to_msecs(get_ticks()));
    usb_debug_printf("CDC re-advertise attempt %u: %s\n", dev->recovery.attempts,
                     result ? "failed" : "awaiting ConnectDone");
    if (!dev->recovery.pending)
        usb_debug_printf("CDC re-advertise exhausted; VDM recovery available\n");
}
#endif

void usb_dwc3_handle_events(dwc3_dev_t *dev)
{
    if (!dev || dev->failed)
        return;

    u32 count = read32(dev->regs + DWC3_GEVNTCOUNT(0)) & DWC3_GEVNTCOUNT_MASK;
    if (!count) {
#if USB_DWC3_SUPERSPEED
        usb_dwc3_maybe_reannounce(dev);
#endif
        return;
    }

    if (count > DWC3_EVENT_BUFFERS_SIZE || count % sizeof(union dwc3_event)) {
        usb_debug_printf("invalid event count %u\n", count);
        write32(dev->regs + DWC3_GEVNTCOUNT(0), count);
        return;
    }

    dma_rmb();

    const union dwc3_event *evtbuffer = dev->evtbuffer;
    union dwc3_event pending[DWC3_EVENT_MAX_NUM];
    u32 n_events = count / sizeof(union dwc3_event);
    for (u32 i = 0; i < n_events; ++i) {
        pending[i] = evtbuffer[dev->evt_buffer_offset];
        dev->evt_buffer_offset =
            (dev->evt_buffer_offset + 1) % (DWC3_EVENT_BUFFERS_SIZE / sizeof(union dwc3_event));
    }

    write32(dev->regs + DWC3_GEVNTCOUNT(0), count);
    (void)read32(dev->regs + DWC3_GEVNTCOUNT(0));
    dma_rmb();

#if USB_DWC3_SUPERSPEED
    /* Service control and reset events before bulk completions in this batch. */
    for (u32 i = 0; i < n_events; i++) {
        if (!dev->failed &&
            (pending[i].type.is_devspec || pending[i].depevt.endpoint_number <= USB_LEP_CTRL_IN))
            usb_dwc3_handle_event(dev, pending[i]);
    }
    for (u32 i = 0; i < n_events; i++) {
        if (!dev->failed && !pending[i].type.is_devspec &&
            pending[i].depevt.endpoint_number > USB_LEP_CTRL_IN)
            usb_dwc3_handle_event(dev, pending[i]);
    }
#else
    for (u32 i = 0; i < n_events && !dev->failed; i++)
        usb_dwc3_handle_event(dev, pending[i]);
#endif
#if USB_DWC3_SUPERSPEED
    usb_dwc3_maybe_reannounce(dev);
#endif
}

dwc3_dev_t *usb_dwc3_init(uintptr_t regs, dart_dev_t *dart)
{
#if USB_DWC3_SUPERSPEED
    usb_cdc_ss_device_descriptor = usb_cdc_device_descriptor;
    usb_cdc_ss_device_descriptor.bcdUSB = 0x0310;
    usb_cdc_ss_device_descriptor.bMaxPacketSize0 = 9;
    usb_cdc_ss_configuration_len = usb_cdc_ss_config(
        (const u8 *)&cdc_configuration_descriptor, sizeof(cdc_configuration_descriptor),
        usb_cdc_ss_configuration, sizeof(usb_cdc_ss_configuration));
    if (!usb_cdc_ss_configuration_len)
        return NULL;
#endif
    /* sanity check */
    u32 snpsid = read32(regs + DWC3_GSNPSID);
    if ((snpsid & DWC3_GSNPSID_MASK) != 0x33310000) {
        debug_printf("no DWC3 core found at 0x%lx: %08x\n", regs, snpsid);
        return NULL;
    }

    dwc3_dev_t *dev = calloc(1, sizeof(*dev));
    if (!dev)
        return NULL;

    memset(dev, 0, sizeof(*dev));
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
        memcpy(dev->pipe[i].cdc_line_coding, cdc_default_line_coding,
               sizeof(cdc_default_line_coding));

    dev->regs = regs;
    dev->dart = dart;

    /* allocate and map dma buffers */
    dev->evtbuffer = memalign(SZ_16K, max(DWC3_EVENT_BUFFERS_SIZE, SZ_16K));
    if (!dev->evtbuffer)
        goto error;

    dev->scratchpad = memalign(SZ_16K, max(DWC3_SCRATCHPAD_SIZE, SZ_16K));
    if (!dev->scratchpad)
        goto error;

    dev->trbs = memalign(SZ_16K, TRB_BUFFER_SIZE);
    if (!dev->trbs)
        goto error;

    dev->xferbuffer = memalign(SZ_16K, XFER_BUFFER_SIZE);
    if (!dev->xferbuffer)
        goto error;

    memset(dev->evtbuffer, 0xaa, max(DWC3_EVENT_BUFFERS_SIZE, SZ_16K));
    memset(dev->scratchpad, 0, max(DWC3_SCRATCHPAD_SIZE, SZ_16K));
    memset(dev->xferbuffer, 0, XFER_BUFFER_SIZE);
    memset(dev->trbs, 0, TRB_BUFFER_SIZE);

    if (dart_map(dev->dart, EVENT_BUFFER_IOVA, dev->evtbuffer,
                 max(DWC3_EVENT_BUFFERS_SIZE, SZ_16K)))
        goto error;
    if (dart_map(dev->dart, SCRATCHPAD_IOVA, dev->scratchpad, max(DWC3_SCRATCHPAD_SIZE, SZ_16K)))
        goto error;
    if (dart_map(dev->dart, TRB_BUFFER_IOVA, dev->trbs, TRB_BUFFER_SIZE))
        goto error;
    if (dart_map(dev->dart, XFER_BUFFER_IOVA, dev->xferbuffer, XFER_BUFFER_SIZE))
        goto error;

    /* prepare endpoint buffers */
    for (int i = 0; i < MAX_ENDPOINTS; ++i) {
        u32 xferbuffer_offset = i * XFER_BUFFER_BYTES_PER_EP;
        dev->endpoints[i].xfer_buffer = dev->xferbuffer + xferbuffer_offset;
        dev->endpoints[i].xfer_buffer_iova = XFER_BUFFER_IOVA + xferbuffer_offset;

        u32 trb_offset = i * TRBS_PER_EP;
        dev->endpoints[i].trb = &dev->trbs[i * TRBS_PER_EP];
        dev->endpoints[i].trb_iova = TRB_BUFFER_IOVA + trb_offset * sizeof(struct dwc3_trb);
    }

    /* reset the device side of the controller */
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST);
    if (poll32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST, 0, 1000)) {
        usb_debug_printf("timeout while waiting for DWC3_DCTL_CSFTRST to clear.\n");
        goto error;
    }

    /* soft reset the core and phy */
    set32(dev->regs + DWC3_GCTL, DWC3_GCTL_CORESOFTRESET);
    set32(dev->regs + DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_PHYSOFTRST);
    set32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_PHYSOFTRST);
    usb_cdc_wait_us(100000);
    clear32(dev->regs + DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_PHYSOFTRST);
    clear32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_PHYSOFTRST);
    usb_cdc_wait_us(100000);
    clear32(dev->regs + DWC3_GCTL, DWC3_GCTL_CORESOFTRESET);
    usb_cdc_wait_us(100000);

#if USB_DWC3_SUPERSPEED
    clear32(dev->regs + DWC3_GUSB2PHYCFG(0), DWC3_GUSB2PHYCFG_SUSPHY);
    clear32(dev->regs + DWC3_GUSB3PIPECTL(0), DWC3_GUSB3PIPECTL_SUSPHY);

    int nump =
        usb_cdc_nump(read32(dev->regs + DWC3_GHWPARAMS0), read32(dev->regs + DWC3_GHWPARAMS7));
    if (nump < 1) {
        usb_debug_printf("invalid DWC3 receive RAM parameters\n");
        goto error;
    }
    clear32(dev->regs + DWC3_GRXTHRCFG, CDC_GRXTHRCFG_PKTCNTSEL);
    mask32(dev->regs + DWC3_DCFG, CDC_DCFG_NUMP_MASK, CDC_DCFG_NUMP(nump));
    usb_debug_printf("NUMP=%d\n", nump);
#endif

    /* disable unused features */
    clear32(dev->regs + DWC3_GCTL, DWC3_GCTL_SCALEDOWN_MASK | DWC3_GCTL_DISSCRAMBLE);

    /* switch to device-only mode */
    mask32(dev->regs + DWC3_GCTL, DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_OTG),
           DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));

    /* Advertise only the speed selected by this build. */
#if USB_DWC3_SUPERSPEED
    mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_SPEED_MASK, DWC3_DCFG_SUPERSPEED);
#else
    mask32(dev->regs + DWC3_DCFG, DWC3_DCFG_SPEED_MASK, DWC3_DCFG_HIGHSPEED);
#endif

    /* setup scratchpad at SCRATCHPAD_IOVA */
    if (usb_dwc3_command(dev, DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO, SCRATCHPAD_IOVA)) {
        usb_debug_printf("DWC3_DGCMD_SET_SCRATCHPAD_ADDR_LO failed.");
        goto error;
    }
    if (usb_dwc3_command(dev, DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI, 0)) {
        usb_debug_printf("DWC3_DGCMD_SET_SCRATCHPAD_ADDR_HI failed.");
        goto error;
    }

    /* setup a single event buffer at EVENT_BUFFER_IOVA */
    write32(dev->regs + DWC3_GEVNTADRLO(0), EVENT_BUFFER_IOVA);
    write32(dev->regs + DWC3_GEVNTADRHI(0), 0);
    write32(dev->regs + DWC3_GEVNTSIZ(0), DWC3_EVENT_BUFFERS_SIZE);
    write32(dev->regs + DWC3_GEVNTCOUNT(0), 0);

    /* enable connect, disconnect and reset events */
    write32(dev->regs + DWC3_DEVTEN,
            DWC3_DEVTEN_DISCONNEVTEN | DWC3_DEVTEN_USBRSTEN | DWC3_DEVTEN_CONNECTDONEEN);

    if (usb_dwc3_ep_command(dev, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0)) {
        usb_debug_printf("cannot issue initial DWC3_DEPCMD_DEPSTARTCFG.\n");
        goto error;
    }

    /* prepare control endpoint 0 IN and OUT */
    if (usb_dwc3_ep_configure(dev, USB_LEP_CTRL_OUT, DWC3_DEPCMD_TYPE_CONTROL,
                              CDC_CONTROL_PACKET_SIZE))
        goto error;
    if (usb_dwc3_ep_configure(dev, USB_LEP_CTRL_IN, DWC3_DEPCMD_TYPE_CONTROL,
                              CDC_CONTROL_PACKET_SIZE))
        goto error;

    /* prepare CDC ACM interfaces */

    dev->pipe[CDC_ACM_PIPE_0].ep_intr = USB_LEP_CDC_INTR_IN;
    dev->pipe[CDC_ACM_PIPE_0].ep_in = USB_LEP_CDC_BULK_IN;
    dev->pipe[CDC_ACM_PIPE_0].ep_out = USB_LEP_CDC_BULK_OUT;

    dev->pipe[CDC_ACM_PIPE_1].ep_intr = USB_LEP_CDC_INTR_IN_2;
    dev->pipe[CDC_ACM_PIPE_1].ep_in = USB_LEP_CDC_BULK_IN_2;
    dev->pipe[CDC_ACM_PIPE_1].ep_out = USB_LEP_CDC_BULK_OUT_2;

    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        dev->pipe[i].host2device = ringbuffer_alloc(CDC_BUFFER_SIZE);
        if (!dev->pipe[i].host2device)
            goto error;
        dev->pipe[i].device2host = ringbuffer_alloc(CDC_BUFFER_SIZE);
        if (!dev->pipe[i].device2host)
            goto error;

        /* Prepare CDC resources for the first enumeration. USB bus resets
         * reconfigure them on the next SET_CONFIGURATION request. */
        if (usb_dwc3_ep_configure(dev, dev->pipe[i].ep_intr, DWC3_DEPCMD_TYPE_INTR, 64))
            goto error;

        /* prepare BULK endpoints so that we don't have to reconfigure this device later */
        if (usb_dwc3_ep_configure(dev, dev->pipe[i].ep_in, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE))
            goto error;
        if (usb_dwc3_ep_configure(dev, dev->pipe[i].ep_out, DWC3_DEPCMD_TYPE_BULK,
                                  CDC_BULK_PACKET_SIZE))
            goto error;
    }

    /* prepare first control transfer */
    dev->ep0_state = USB_DWC3_EP0_STATE_IDLE;

    /* only enable control endpoints for now */
    write32(dev->regs + DWC3_DALEPENA,
            DWC3_DALEPENA_EP(USB_LEP_CTRL_IN) | DWC3_DALEPENA_EP(USB_LEP_CTRL_OUT));

    /* and finally kick the device controller to go live! */
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);

    return dev;

error:
    usb_dwc3_shutdown(dev);
    return NULL;
}

/* Hand the memory the controller and its DART may still reach to the next stage as reserved. */
static void usb_dwc3_retain(dwc3_dev_t *dev)
{
    if (!usb_dwc3_retain_dma((u64)dev->evtbuffer, max(DWC3_EVENT_BUFFERS_SIZE, SZ_16K)) ||
        !usb_dwc3_retain_dma((u64)dev->scratchpad, max(DWC3_SCRATCHPAD_SIZE, SZ_16K)) ||
        !usb_dwc3_retain_dma((u64)dev->trbs, TRB_BUFFER_SIZE) ||
        !usb_dwc3_retain_dma((u64)dev->xferbuffer, XFER_BUFFER_SIZE) ||
        !usb_dwc3_retain_dart(dev->dart))
        usb_debug_printf("too many retained DMA ranges to describe\n");
}

/*
 * Returns false when the controller may still reach its DMA buffers: they
 * then stay allocated, the device stays retained and usb_dwc3_retain()
 * records them for the handoff (kboot reserves them in the kernel's tree).
 */
bool usb_dwc3_shutdown(dwc3_dev_t *dev)
{
    if (!dev)
        return true;
    /* A controller reset already failed: record its memory, touch nothing else. */
    if (dev->dma_unsafe) {
        usb_dwc3_retain(dev);
        return false;
    }
    dev->shutting_down = true;
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++)
        dev->pipe[i].ready = false;

    /*
     * Retire every active transfer, EP0 included. A failed session already
     * reset the controller, which retired its DMA.
     */
    int retired = 0;
    for (int i = 0; i < MAX_ENDPOINTS && !dev->failed && !dev->dma_unsafe; ++i) {
        if (!dev->endpoints[i].xfer_in_progress)
            continue;
        if (usb_dwc3_retire_transfer(dev, i)) {
            dev->dma_unsafe = true;
            printf("usb-dwc3@%lx: EP %d transfer not retired at shutdown\n", dev->regs, i);
        } else {
            retired++;
        }
    }
    if (retired)
        printf("usb-dwc3@%lx: %d transfer(s) retired at shutdown\n", dev->regs, retired);

    /* disable events and all endpoints and stop the device controller */
    write32(dev->regs + DWC3_DEVTEN, 0);
    write32(dev->regs + DWC3_DALEPENA, 0);
    clear32(dev->regs + DWC3_DCTL, DWC3_DCTL_RUN_STOP);

    /* wait until the controller is shut down */
    if (poll32(dev->regs + DWC3_DSTS, DWC3_DSTS_DEVCTRLHLT, DWC3_DSTS_DEVCTRLHLT, 1000)) {
        /* A controller that did not halt may still be moving data. */
        dev->dma_unsafe = true;
        usb_debug_printf("timeout while waiting for DWC3_DSTS_DEVCTRLHLT during shutdown.\n");
    }

    /* reset the device side of the controller just to be safe */
    set32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST);
    if (poll32(dev->regs + DWC3_DCTL, DWC3_DCTL_CSFTRST, 0, 1000)) {
        dev->dma_unsafe = true;
        usb_debug_printf("controller reset failed during shutdown; preserving DMA mappings\n");
    }
    if (dev->dma_unsafe) {
        usb_dwc3_retain(dev);
        return false;
    }

    /* Free the buffers only once the DART confirmed it no longer maps them. */
    if (!dart_unmap_checked(dev->dart, TRB_BUFFER_IOVA, TRB_BUFFER_SIZE) ||
        !dart_unmap_checked(dev->dart, XFER_BUFFER_IOVA, XFER_BUFFER_SIZE) ||
        !dart_unmap_checked(dev->dart, SCRATCHPAD_IOVA, max(DWC3_SCRATCHPAD_SIZE, SZ_16K)) ||
        !dart_unmap_checked(dev->dart, EVENT_BUFFER_IOVA, max(DWC3_EVENT_BUFFERS_SIZE, SZ_16K)) ||
        !dart_shutdown_checked(dev->dart)) {
        dev->dma_unsafe = true;
        usb_debug_printf("DART unmap failed during shutdown; preserving DMA buffers\n");
        usb_dwc3_retain(dev);
        return false;
    }

    free(dev->evtbuffer);
    free(dev->scratchpad);
    free(dev->xferbuffer);
    free(dev->trbs);
    for (int i = 0; i < CDC_ACM_PIPE_MAX; i++) {
        ringbuffer_free(dev->pipe[i].device2host);
        ringbuffer_free(dev->pipe[i].host2device);
    }

    free(dev);
    return true;
}

u8 usb_dwc3_getbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

    u8 ep = dev->pipe[pipe].ep_out;

    u8 c;
    while (ringbuffer_read(&c, 1, host2device) < 1) {
#if USB_DWC3_SUPERSPEED
        if (!dev->pipe[pipe].ready)
            return 0;
#endif
        usb_dwc3_handle_events(dev);
        usb_dwc3_cdc_start_bulk_out_xfer(dev, ep);
    }
    return c;
}

void usb_dwc3_putbyte(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, u8 byte)
{
    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return;

    u8 ep = dev->pipe[pipe].ep_in;

    while (ringbuffer_write(&byte, 1, device2host) < 1) {
#if USB_DWC3_SUPERSPEED
        if (!dev->pipe[pipe].ready)
            return;
#endif
        usb_dwc3_handle_events(dev);
        usb_dwc3_cdc_start_bulk_in_xfer(dev, ep);
    }
}

size_t usb_dwc3_queue(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    const u8 *p = buf;
    size_t wrote, sent = 0;

    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return 0;

    u8 ep = dev->pipe[pipe].ep_in;
#if USB_DWC3_SUPERSPEED
    u64 last_progress = get_ticks();
#endif

    while (count && CDC_LOOP_READY(dev, pipe)) {
        wrote = ringbuffer_write(p, count, device2host);
        if (wrote) {
#if USB_DWC3_SUPERSPEED
            last_progress = get_ticks();
#endif
        }
        count -= wrote;
        p += wrote;
        sent += wrote;
        if (count) {
#if USB_DWC3_SUPERSPEED
            if (ticks_to_msecs(get_ticks() - last_progress) >= CDC_IO_TIMEOUT_MS)
                break;
#endif
            usb_dwc3_handle_events(dev);
            usb_dwc3_cdc_start_bulk_in_xfer(dev, ep);
        }
    }

    return sent;
}

size_t usb_dwc3_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, const void *buf, size_t count)
{
    if (!dev)
        return 0;

    u8 ep = dev->pipe[pipe].ep_in;
    size_t ret = usb_dwc3_queue(dev, pipe, buf, count);

    usb_dwc3_cdc_start_bulk_in_xfer(dev, ep);

    return ret;
}

size_t usb_dwc3_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe, void *buf, size_t count)
{
    u8 *p = buf;
    size_t read, recvd = 0;

    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

    u8 ep = dev->pipe[pipe].ep_out;
#if USB_DWC3_SUPERSPEED
    u64 last_progress = get_ticks();
#endif

    while (count && CDC_LOOP_READY(dev, pipe)) {
        read = ringbuffer_read(p, count, host2device);
        if (read) {
#if USB_DWC3_SUPERSPEED
            last_progress = get_ticks();
#endif
        }
        count -= read;
        p += read;
        recvd += read;
#if USB_DWC3_SUPERSPEED
        if (count && recvd && ticks_to_msecs(get_ticks() - last_progress) >= CDC_IO_TIMEOUT_MS)
            break;
#endif
        usb_dwc3_handle_events(dev);
        usb_dwc3_cdc_start_bulk_out_xfer(dev, ep);
    }

    return recvd;
}

ssize_t usb_dwc3_can_read(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev || !dev->pipe[pipe].ready)
        return 0;

    ringbuffer_t *host2device = dev->pipe[pipe].host2device;
    if (!host2device)
        return 0;

#if !USB_DWC3_SUPERSPEED
    /* High-speed OUT is otherwise re-armed only by XferNotReady or a read in
     * progress; an idle proxy loop must not depend on that one event. */
    usb_dwc3_cdc_start_bulk_out_xfer(dev, dev->pipe[pipe].ep_out);
#endif
    return ringbuffer_get_used(host2device);
}

bool usb_dwc3_can_write(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    (void)pipe;
    if (!dev)
        return false;

    return dev->pipe[pipe].ready;
}

void usb_dwc3_flush(dwc3_dev_t *dev, cdc_acm_pipe_id_t pipe)
{
    if (!dev || !dev->pipe[pipe].ready)
        return;

    ringbuffer_t *device2host = dev->pipe[pipe].device2host;
    if (!device2host)
        return;

    u8 ep = dev->pipe[pipe].ep_in;
    /* After the host restores KIS, USB2 may retain an open pipe with TX
     * pending. Console flushing must not block a guarded RAM handoff. */
    bool bounded = USB_DWC3_SUPERSPEED || chip_id == T8122 || chip_id == T8142 ||
                   chip_id == T8152 || chip_id == T6050;
    u64 start = get_ticks();

    while (CDC_LOOP_READY(dev, pipe) &&
           (ringbuffer_get_used(device2host) != 0 || dev->endpoints[ep].xfer_in_progress)) {
        if (bounded && ticks_to_msecs(get_ticks() - start) >= 3000)
            return;
        usb_dwc3_handle_events(dev);
#if !USB_DWC3_SUPERSPEED
        usb_dwc3_cdc_start_bulk_in_xfer(dev, ep);
#endif
    }
}
