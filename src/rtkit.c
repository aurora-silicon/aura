/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../config.h"

#include "rtkit.h"
#include "adt.h"
#include "asc.h"
#include "dart.h"
#include "iova.h"
#include "malloc.h"
#include "sart.h"
#include "soc.h"
#include "string.h"
#include "types.h"
#include "utils.h"

#define rtkit_printf(...)                                                                          \
    do {                                                                                           \
        debug_printf("rtkit(%s): ", rtk->name);                                                    \
        debug_printf(__VA_ARGS__);                                                                 \
    } while (0)

#define RTKIT_EP_MGMT     0
#define RTKIT_EP_CRASHLOG 1
#define RTKIT_EP_SYSLOG   2
#define RTKIT_EP_DEBUG    3
#define RTKIT_EP_IOREPORT 4
#define RTKIT_EP_OSLOG    8

#define MGMT_TYPE GENMASK(59, 52)

#define MGMT_PWR_STATE GENMASK(15, 0)

#define MSG_BUFFER_REQUEST      1
#define MSG_BUFFER_REQUEST_SIZE GENMASK(51, 44)
#define MSG_BUFFER_REQUEST_IOVA GENMASK(41, 0)

#define MSG_SYSLOG_INIT           8
#define MSG_SYSLOG_INIT_ENTRYSIZE GENMASK(39, 24)
#define MSG_SYSLOG_INIT_COUNT     GENMASK(15, 0)
#define MSG_SYSLOG_LOG            5
#define MSG_SYSLOG_LOG_INDEX      GENMASK(7, 0)

#define MSG_OSLOG_INIT 0x10
#define MSG_OSLOG_ACK  0x30

#define OSLOG_TYPE                GENMASK(63, 56)
#define OSLOG_TYPE_BUFFER_REQUEST 1
#define OSLOG_SIZE                GENMASK(55, 36)
#define OSLOG_IOVA                GENMASK(35, 0)

#define MGMT_MSG_HELLO        1
#define MGMT_MSG_HELLO_ACK    2
#define MGMT_MSG_HELLO_MINVER GENMASK(15, 0)
#define MGMT_MSG_HELLO_MAXVER GENMASK(31, 16)

#define MGMT_MSG_IOP_PWR_STATE     6
#define MGMT_MSG_IOP_PWR_STATE_ACK 7

#define MGMT_MSG_EPMAP        8
#define MGMT_MSG_EPMAP_DONE   BIT(51)
#define MGMT_MSG_EPMAP_BASE   GENMASK(34, 32)
#define MGMT_MSG_EPMAP_BITMAP GENMASK(31, 0)

#define MGMT_MSG_EPMAP_REPLY      8
#define MGMT_MSG_EPMAP_REPLY_DONE BIT(51)
#define MGMT_MSG_EPMAP_REPLY_MORE BIT(0)

#define MGMT_MSG_AP_PWR_STATE     0xb
#define MGMT_MSG_AP_PWR_STATE_ACK 0xb

#define MGMT_MSG_START_EP      5
#define MGMT_MSG_START_EP_IDX  GENMASK(39, 32)
#define MGMT_MSG_START_EP_FLAG BIT(1)

#define RTKIT_MIN_VERSION 11
#define RTKIT_MAX_VERSION 12

#define IOVA_MASK GENMASK(35, 0)

/* Bound on an IOP power state change, for a firmware that has stopped answering. */
#define RTKIT_IOP_POWER_TIMEOUT 5000000

enum rtkit_power_state {
    RTKIT_POWER_OFF = 0x00,
    RTKIT_POWER_SLEEP = 0x01,
    RTKIT_POWER_QUIESCED = 0x10,
    RTKIT_POWER_ON = 0x20,
    RTKIT_POWER_INIT = 0x220,
};

struct rtkit_dev {
    char *name;

    asc_dev_t *asc;
    dart_dev_t *dart;
    iova_domain_t *dart_iovad;
    sart_dev_t *sart;
    bool sram;

    u64 dva_base;
    u32 timeout_usec;
    struct rtkit_buffer oslog_bfr;

    enum rtkit_power_state iop_power;
    enum rtkit_power_state ap_power;

    struct rtkit_buffer syslog_bfr;
    struct rtkit_buffer crashlog_bfr;
    struct rtkit_buffer ioreport_bfr;

    u32 syslog_cnt, syslog_size;

    bool crashed;
    /* The last boot attempt got no HELLO after the wakeup. */
    bool no_hello;
};

struct syslog_log {
    u32 hdr;
    u32 unk;
    char context[24];
    char msg[];
};

struct crashlog_hdr {
    u32 type;
    u32 ver;
    u32 total_size;
    u32 flags;
    u8 _padding[16];
};

struct crashlog_entry {
    u32 type;
    u32 _padding;
    u32 flags;
    u32 len;
    u8 payload[];
};

rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *asc, dart_dev_t *dart,
                        iova_domain_t *dart_iovad, sart_dev_t *sart, bool sram)
{
    if (dart && sart) {
        printf("rtkit: Cannot use both SART and DART simultaneously\n");
        return NULL;
    }

    if (dart && !dart_iovad) {
        printf("rtkit: if DART is used iovad is already required\n");
        return NULL;
    }

    if (sram && (dart || sart)) {
        printf("rtkit: cannot use SRAM with DART or SART \n");
        return NULL;
    }

    rtkit_dev_t *rtk = calloc(1, sizeof(*rtk));
    if (!rtk)
        return NULL;

    size_t name_len = strlen(name);
    rtk->name = calloc(name_len + 1, 1);
    if (!rtk->name)
        goto out_free_rtk;
    strcpy(rtk->name, name);

    rtk->asc = asc;
    rtk->dart = dart;
    rtk->dart_iovad = dart_iovad;
    rtk->sart = sart;
    rtk->sram = sram;
    rtk->iop_power = RTKIT_POWER_OFF;
    rtk->ap_power = RTKIT_POWER_OFF;
    rtk->dva_base = 0;

    int iop_node = asc_get_iop_node(asc);
    ADT_GETPROP(adt, iop_node, "asc-dram-mask", &rtk->dva_base);

    return rtk;

out_free_rtk:
    free(rtk);
    return NULL;
}

void rtkit_free(rtkit_dev_t *rtk)
{
    rtkit_free_buffer(rtk, &rtk->syslog_bfr);
    rtkit_free_buffer(rtk, &rtk->crashlog_bfr);
    rtkit_free_buffer(rtk, &rtk->ioreport_bfr);
    rtkit_free_buffer(rtk, &rtk->oslog_bfr);
    free(rtk->name);
    free(rtk);
}

bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg)
{
    struct asc_message asc_msg;

    asc_msg.msg0 = msg->msg;
    asc_msg.msg1 = msg->ep;

    return asc_send(rtk->asc, &asc_msg);
}

/* A failed handshake can already have granted host storage to firmware. */
bool rtkit_has_owned_buffers(const rtkit_dev_t *rtk)
{
    return rtk && (rtk->syslog_bfr.owned || rtk->crashlog_bfr.owned || rtk->ioreport_bfr.owned ||
                   rtk->oslog_bfr.owned);
}

bool rtkit_map(rtkit_dev_t *rtk, void *phys, size_t sz, u64 *dva)
{
    sz = ALIGN_UP(sz, 16384);

    if (rtk->sart) {
        if (!sart_add_allowed_region(rtk->sart, phys, sz)) {
            rtkit_printf("sart_add_allowed_region failed (size 0x%lx)\n", sz);
            return false;
        }
        *dva = (u64)phys;
        return true;
    } else if (rtk->dart) {
        u64 iova = iova_alloc(rtk->dart_iovad, sz);
        if (!iova) {
            rtkit_printf("failed to alloc iova (size 0x%lx)\n", sz);
            return false;
        }

        if (dart_map(rtk->dart, iova, phys, sz) < 0) {
            rtkit_printf("failed to DART map (size 0x%lx)\n", sz);
            iova_free(rtk->dart_iovad, iova, sz);
            return false;
        }

        *dva = iova | rtk->dva_base;
        return true;
    } else {
        rtkit_printf("TODO: implement no IOMMU buffers\n");
        return false;
    }
}

bool rtkit_unmap(rtkit_dev_t *rtk, u64 dva, size_t sz)
{
    if (rtk->sart) {
        if (!sart_remove_allowed_region(rtk->sart, (void *)dva, sz)) {
            rtkit_printf("sart_remove_allowed_region failed (size 0x%lx)\n", sz);
            return false;
        }
        return true;
    } else if (rtk->dart) {
        dva &= ~rtk->dva_base;
        dart_unmap(rtk->dart, dva & IOVA_MASK, sz);
        iova_free(rtk->dart_iovad, dva, sz);
        return true;
    } else {
        rtkit_printf("TODO: implement no IOMMU buffers\n");
        return false;
    }
}

bool rtkit_alloc_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr, size_t sz)
{
    if (!sz || sz > (size_t)-1 - (SZ_16K - 1))
        return false;

    size_t alloc_sz = ALIGN_UP(sz, SZ_16K);
    bfr->bfr = memalign(SZ_16K, alloc_sz);
    if (!bfr->bfr) {
        rtkit_printf("unable to allocate %zu buffer\n", sz);
        return false;
    }

    memset(bfr->bfr, 0, alloc_sz);
    bfr->sz = alloc_sz;
    if (!rtkit_map(rtk, bfr->bfr, alloc_sz, &bfr->dva))
        goto error;

    bfr->owned = true;
    return true;

error:
    free(bfr->bfr);
    memset(bfr, 0, sizeof(*bfr));
    return false;
}

bool rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr)
{
    if (!bfr->bfr || !bfr->owned) {
        memset(bfr, 0, sizeof(*bfr));
        return true;
    }

    if (!rtkit_unmap(rtk, bfr->dva, bfr->sz))
        return false;

    free(bfr->bfr);
    memset(bfr, 0, sizeof(*bfr));
    return true;
}

static bool rtkit_handle_buffer_request(rtkit_dev_t *rtk, struct rtkit_message *msg,
                                        struct rtkit_buffer *bfr)
{
    size_t n_4kpages = FIELD_GET(MSG_BUFFER_REQUEST_SIZE, msg->msg);
    size_t sz = n_4kpages << 12;
    u64 addr = FIELD_GET(MSG_BUFFER_REQUEST_IOVA, msg->msg);

    if (rtk->sram) {
        if (!addr) {
            rtkit_printf("SRAM buffers needs to be provided by the IOP\n");
            return false;
        }
        bfr->dva = addr;
        bfr->bfr = (void *)addr;
        bfr->sz = sz;
        bfr->owned = false;
        return true;
    } else if (addr) {
        if (!rtk->dart)
            return false;
        bfr->dva = addr & ~rtk->dva_base;
        bfr->sz = sz;
        bfr->owned = false;
        bfr->bfr = dart_translate(rtk->dart, bfr->dva & IOVA_MASK);
        if (!bfr->bfr) {
            rtkit_printf("failed to translate pre-allocated buffer (ep 0x%x)\n", msg->ep);
            return false;
        }
        return true;

    } else {
        if (!rtkit_alloc_buffer(rtk, bfr, sz)) {
            rtkit_printf("unable to allocate buffer\n");
            return false;
        }
    }

    struct asc_message reply;
    reply.msg1 = msg->ep;
    reply.msg0 = FIELD_PREP(MGMT_TYPE, MSG_BUFFER_REQUEST);
    reply.msg0 |= FIELD_PREP(MSG_BUFFER_REQUEST_SIZE, n_4kpages);
    reply.msg0 |= FIELD_PREP(MSG_BUFFER_REQUEST_IOVA, bfr->dva | rtk->dva_base);

    if (!asc_send(rtk->asc, &reply)) {
        rtkit_printf("unable to send buffer reply\n");
        return false;
    }

    return true;
}

static bool rtkit_handle_oslog_request(rtkit_dev_t *rtk, struct rtkit_message *msg)
{
    size_t sz = FIELD_GET(OSLOG_SIZE, msg->msg);
    u64 addr = FIELD_GET(OSLOG_IOVA, msg->msg) << 12;
    struct rtkit_buffer *bfr = &rtk->oslog_bfr;

    if (bfr->bfr) {
        rtkit_printf("duplicate oslog buffer request %lx\n", msg->msg);
        return false;
    }

    if (addr) {
        if (rtk->sram && chip_id == T8152 && sz) {
            /* As for SRAM buffers, the address is physical and the IOP owns
             * it: the T8152 SMC pre-allocates its oslog inside an
             * SPTM-protected window and only needs the address echoed back. */
            bfr->dva = addr;
            bfr->bfr = (void *)addr;
            bfr->sz = sz;
            bfr->owned = false;
            rtkit_printf("pre-allocated oslog buffer (phys %#lx, size %#zx)\n", addr, sz);
            return true;
        }
        if (rtk->dart && sz && addr <= UINT64_MAX - sz) {
            u64 dva = addr & ~rtk->dva_base;
            u64 page = ALIGN_DOWN(dva, SZ_16K);
            u64 end = ALIGN_UP(dva + sz, SZ_16K);
            void *phys = dart_translate(rtk->dart, page & IOVA_MASK);
            if (!phys)
                return false;
            /* The RTKit buffer API requires one contiguous host mapping. */
            for (u64 next = page + SZ_16K; next < end; next += SZ_16K) {
                if (dart_translate(rtk->dart, next & IOVA_MASK) != phys + (next - page)) {
                    rtkit_printf("oslog DART buffer is not contiguous\n");
                    return false;
                }
            }
            bfr->dva = dva;
            bfr->bfr = phys + (dva - page);
            bfr->sz = sz;
            rtkit_printf("pre-allocated oslog buffer (dva %#lx, phys %p, size %#zx)\n", dva,
                         bfr->bfr, sz);
            return true;
        }
        rtkit_printf("oslog buffer request outside mapped DART (%#lx, %#zx)\n", addr, sz);
        return false;
    }

    if (!sz) {
        rtkit_printf("empty oslog buffer request %lx\n", msg->msg);
        return false;
    }

    if (!rtkit_alloc_buffer(rtk, bfr, sz)) {
        rtkit_printf("unable to allocate oslog buffer\n");
        return false;
    }

    struct asc_message reply;
    reply.msg1 = RTKIT_EP_OSLOG;
    reply.msg0 = FIELD_PREP(OSLOG_TYPE, OSLOG_TYPE_BUFFER_REQUEST);
    reply.msg0 |= FIELD_PREP(OSLOG_SIZE, sz);
    reply.msg0 |= FIELD_PREP(OSLOG_IOVA, bfr->dva >> 12);
    if (!asc_send(rtk->asc, &reply)) {
        rtkit_printf("unable to send oslog buffer reply\n");
        rtkit_free_buffer(rtk, bfr);
        return false;
    }

    rtkit_printf("oslog buffer (dva %#lx, phys %p, size %#zx)\n", bfr->dva, bfr->bfr, sz);
    return true;
}

static void rtkit_crashed(rtkit_dev_t *rtk)
{
    struct crashlog_hdr *hdr = rtk->crashlog_bfr.bfr;
    rtk->crashed = true;

    rtkit_printf("IOP crashed!\n");

    if (hdr->type != 'CLHE') {
        rtkit_printf("bad crashlog header 0x%x @ %p\n", hdr->type, hdr);
        return;
    }

    struct crashlog_entry *p = (void *)(hdr + 1);

    rtkit_printf("== CRASH INFO ==\n");
    while (p->type != 'CLHE') {
        switch (p->type) {
            case 'Cstr':
                rtkit_printf("  Message %d: %s\n", p->payload[0], &p->payload[4]);
                break;
            default:
                rtkit_printf("  0x%x\n", p->type);
                break;
        }
        p = ((void *)p) + p->len;
    }
}

bool rtkit_can_recv(rtkit_dev_t *rtk)
{
    if (rtk->crashed)
        return false;

    return asc_can_recv(rtk->asc);
}

int rtkit_recv(rtkit_dev_t *rtk, struct rtkit_message *msg)
{
    struct asc_message asc_msg;
    bool ok = true;

    if (rtk->crashed)
        return -1;

    while (asc_recv(rtk->asc, &asc_msg)) {
        if (asc_msg.msg1 >= 0x100) {
            rtkit_printf("WARNING: received message for invalid endpoint %x >= 0x100\n",
                         asc_msg.msg1);
            continue;
        }

        msg->msg = asc_msg.msg0;
        msg->ep = (u8)asc_msg.msg1;

        /* if this is an app message we can just forward it to the caller */
        if (msg->ep >= 0x20)
            return 1;

        u32 msgtype = FIELD_GET(MGMT_TYPE, msg->msg);
        switch (msg->ep) {
            case RTKIT_EP_MGMT:
                switch (msgtype) {
                    case MGMT_MSG_IOP_PWR_STATE_ACK:
                        rtk->iop_power = FIELD_GET(MGMT_PWR_STATE, msg->msg);
                        break;
                    case MGMT_MSG_AP_PWR_STATE_ACK:
                        rtk->ap_power = FIELD_GET(MGMT_PWR_STATE, msg->msg);
                        break;
                    default:
                        rtkit_printf("unknown management message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_SYSLOG:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->syslog_bfr);
                        break;
                    case MSG_SYSLOG_INIT:
                        rtk->syslog_cnt = FIELD_GET(MSG_SYSLOG_INIT_COUNT, msg->msg);
                        rtk->syslog_size = FIELD_GET(MSG_SYSLOG_INIT_ENTRYSIZE, msg->msg);
                        break;
                    case MSG_SYSLOG_LOG:
#ifdef RTKIT_SYSLOG
                    {
                        u64 index = FIELD_GET(MSG_SYSLOG_LOG_INDEX, msg->msg);
                        u64 stride = rtk->syslog_size + sizeof(struct syslog_log);
                        struct syslog_log *log = rtk->syslog_bfr.bfr + stride * index;
                        rtkit_printf("syslog: [%s]%s", log->context, log->msg);
                        if (log->msg[strlen(log->msg) - 1] != '\n')
                            printf("\n");
                    }
#endif
                        if (!asc_send(rtk->asc, &asc_msg))
                            rtkit_printf("failed to ack syslog\n");
                        break;
                    default:
                        rtkit_printf("unknown syslog message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_CRASHLOG:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        if (!rtk->crashlog_bfr.bfr) {
                            ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->crashlog_bfr);
                        } else {
                            rtkit_crashed(rtk);
                            return -1;
                        }
                        break;
                    default:
                        rtkit_printf("unknown crashlog message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_IOREPORT:
                switch (msgtype) {
                    case MSG_BUFFER_REQUEST:
                        ok = ok && rtkit_handle_buffer_request(rtk, msg, &rtk->ioreport_bfr);
                        break;
                    /* unknown but must be ACKed */
                    case 0x8:
                    case 0xc:
                        if (!rtkit_send(rtk, msg))
                            rtkit_printf("unable to ACK unknown ioreport message\n");
                        break;
                    default:
                        rtkit_printf("unknown ioreport message %x\n", msgtype);
                }
                break;
            case RTKIT_EP_OSLOG:
                if (asc_is_v8(rtk->asc) &&
                    FIELD_GET(OSLOG_TYPE, msg->msg) == OSLOG_TYPE_BUFFER_REQUEST) {
                    ok = ok && rtkit_handle_oslog_request(rtk, msg);
                    break;
                }
                rtkit_printf("unknown oslog message %lx\n", msg->msg);
                break;
            default:
                rtkit_printf("message to unknown system endpoint 0x%02x: %lx\n", msg->ep, msg->msg);
        }

        if (!ok) {
            rtkit_printf("failed to handle system message 0x%02x: %lx\n", msg->ep, msg->msg);
            return -1;
        }
    }

    return 0;
}

bool rtkit_start_ep(rtkit_dev_t *rtk, u8 ep)
{
    struct asc_message msg;

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_START_EP);
    msg.msg0 |= MGMT_MSG_START_EP_FLAG;
    msg.msg0 |= FIELD_PREP(MGMT_MSG_START_EP_IDX, ep);
    msg.msg1 = RTKIT_EP_MGMT;

    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("unable to start endpoint 0x%02x\n", ep);
        return false;
    }

    return true;
}

static bool rtkit_wait_for_power(rtkit_dev_t *rtk, enum rtkit_power_state *state,
                                 enum rtkit_power_state target, u32 timeout_usec, const char *name)
{
    u64 timeout = timeout_usec ? timeout_calculate(timeout_usec) : 0;

    while (*state != target) {
        struct rtkit_message rtk_msg;
        int ret = rtkit_recv(rtk, &rtk_msg);
        if (ret == 1)
            rtkit_printf("unexpected message to non-system endpoint 0x%02x "
                         "while waiting for %s power: %lx\n",
                         rtk_msg.ep, name, rtk_msg.msg);
        else if (ret < 0)
            return false;

        if (timeout && timeout_expired(timeout)) {
            rtkit_printf("timed out waiting for %s power state %#x\n", name, target);
            return false;
        }
    }

    return true;
}

static bool rtkit_boot_internal(rtkit_dev_t *rtk, u32 timeout_usec)
{
    struct asc_message msg;

    rtk->no_hello = false;

    /* boot the IOP if it isn't already */
    asc_cpu_start(rtk->asc);
    /* can be sent unconditionally to wake up a possibly sleeping IOP */
    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) |
               FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_INIT);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("unable to send wakeup message\n");
        return false;
    }

    if (!asc_recv_timeout(rtk->asc, &msg, USEC_PER_SEC)) {
        rtkit_printf("did not receive HELLO\n");
        rtk->no_hello = true;
        return false;
    }

    if (msg.msg1 != RTKIT_EP_MGMT) {
        rtkit_printf("expected HELLO but got message for EP 0x%x", msg.msg1);
        return false;
    }

    u32 msgtype;
    msgtype = FIELD_GET(MGMT_TYPE, msg.msg0);
    if (msgtype != MGMT_MSG_HELLO) {
        rtkit_printf("expected HELLO but got message with type 0x%02x", msgtype);

        return false;
    }

    u32 min_ver, max_ver, want_ver;
    min_ver = FIELD_GET(MGMT_MSG_HELLO_MINVER, msg.msg0);
    max_ver = FIELD_GET(MGMT_MSG_HELLO_MAXVER, msg.msg0);
    want_ver = min(RTKIT_MAX_VERSION, max_ver);

    if (min_ver > RTKIT_MAX_VERSION || max_ver < RTKIT_MIN_VERSION) {
        rtkit_printf("supported versions [%d,%d] must overlap versions [%d,%d]\n",
                     RTKIT_MIN_VERSION, RTKIT_MAX_VERSION, min_ver, max_ver);
        return false;
    }

    rtkit_printf("booting with version %d\n", want_ver);

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_HELLO_ACK);
    msg.msg0 |= FIELD_PREP(MGMT_MSG_HELLO_MINVER, want_ver);
    msg.msg0 |= FIELD_PREP(MGMT_MSG_HELLO_MAXVER, want_ver);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("couldn't send HELLO ack\n");
        return false;
    }

    bool has_crashlog = false;
    bool has_debug = false;
    bool has_ioreport = false;
    bool has_syslog = false;
    bool has_oslog = false;
    bool got_epmap = false;
    while (!got_epmap) {
        if (!asc_recv_timeout(rtk->asc, &msg, USEC_PER_SEC)) {
            rtkit_printf("couldn't receive message while waiting for endpoint map\n");
            return false;
        }

        if (msg.msg1 != RTKIT_EP_MGMT) {
            rtkit_printf("expected management message while waiting for endpoint map but got "
                         "message for endpoint 0x%x\n",
                         msg.msg1);
            return false;
        }

        msgtype = FIELD_GET(MGMT_TYPE, msg.msg0);
        if (msgtype != MGMT_MSG_EPMAP) {
            rtkit_printf("expected endpoint map message but got 0x%x instead\n", msgtype);
            return false;
        }

        u32 bitmap = FIELD_GET(MGMT_MSG_EPMAP_BITMAP, msg.msg0);
        u32 base = FIELD_GET(MGMT_MSG_EPMAP_BASE, msg.msg0);
        for (unsigned int i = 0; i < 32; i++) {
            if (bitmap & (1U << i)) {
                u8 ep_idx = 32 * base + i;

                if (ep_idx >= 0x20)
                    continue;
                switch (ep_idx) {
                    case RTKIT_EP_CRASHLOG:
                        has_crashlog = true;
                        break;
                    case RTKIT_EP_DEBUG:
                        has_debug = true;
                        break;
                    case RTKIT_EP_IOREPORT:
                        has_ioreport = true;
                        break;
                    case RTKIT_EP_SYSLOG:
                        has_syslog = true;
                        break;
                    case RTKIT_EP_OSLOG:
                        has_oslog = true;
                    case RTKIT_EP_MGMT:
                        break;
                    default:
                        rtkit_printf("unknown system endpoint 0x%02x\n", ep_idx);
                }
            }
        }

        if (msg.msg0 & MGMT_MSG_EPMAP_DONE)
            got_epmap = true;

        msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_EPMAP_REPLY);
        msg.msg0 |= FIELD_PREP(MGMT_MSG_EPMAP_BASE, base);
        if (got_epmap)
            msg.msg0 |= MGMT_MSG_EPMAP_REPLY_DONE;
        else
            msg.msg0 |= MGMT_MSG_EPMAP_REPLY_MORE;

        msg.msg1 = RTKIT_EP_MGMT;

        if (!asc_send(rtk->asc, &msg)) {
            rtkit_printf("couldn't reply to endpoint map\n");
            return false;
        }
    }

    /* start all required system endpoints */
    if (has_debug && !rtkit_start_ep(rtk, RTKIT_EP_DEBUG))
        return false;
    if (has_crashlog && !rtkit_start_ep(rtk, RTKIT_EP_CRASHLOG))
        return false;
    if (has_syslog && !rtkit_start_ep(rtk, RTKIT_EP_SYSLOG))
        return false;
    if (has_ioreport && !rtkit_start_ep(rtk, RTKIT_EP_IOREPORT))
        return false;
    if (has_oslog && !rtkit_start_ep(rtk, RTKIT_EP_OSLOG))
        return false;

    if (!rtkit_wait_for_power(rtk, &rtk->iop_power, RTKIT_POWER_ON, timeout_usec, "IOP"))
        return false;

    /* this enables syslog */
    msg.msg0 =
        FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) | FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_ON);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("unable to send AP power message\n");
        return false;
    }

    if (timeout_usec &&
        !rtkit_wait_for_power(rtk, &rtk->ap_power, RTKIT_POWER_ON, timeout_usec, "AP"))
        return false;

    return true;
}

bool rtkit_boot(rtkit_dev_t *rtk)
{
    return rtkit_boot_internal(rtk, 0);
}

/* Boot the IOP, or adopt the session of one a previous stage left running. Such
 * an IOP already completed HELLO/EPMAP and does not repeat them, so the only
 * accepted evidence is a CPU that was running before the wakeup and no HELLO
 * in reply to it; any other boot failure stays a failure. The caller must
 * still confirm the adopted session on its own endpoint. */
bool rtkit_boot_or_adopt(rtkit_dev_t *rtk, bool *adopted)
{
    bool running = asc_cpu_running(rtk->asc);

    *adopted = false;
    if (rtkit_boot(rtk))
        return true;
    if (!running || !rtk->no_hello)
        return false;

    rtkit_printf("running IOP sent no HELLO, adopting its session\n");
    *adopted = true;
    return true;
}

bool rtkit_boot_timed(rtkit_dev_t *rtk, u32 timeout_usec)
{
    if (!timeout_usec) {
        rtkit_printf("timed boot requires a nonzero timeout\n");
        return false;
    }

    rtk->timeout_usec = timeout_usec;
    return rtkit_boot_internal(rtk, timeout_usec);
}

u32 rtkit_get_timeout(rtkit_dev_t *rtk)
{
    return rtk->timeout_usec;
}

static bool rtkit_quiesce_ap_power(rtkit_dev_t *rtk)
{
    struct asc_message msg;

    /* AP power should always go to QUIESCED, otherwise rebooting doesn't work */
    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_AP_PWR_STATE) |
               FIELD_PREP(MGMT_PWR_STATE, RTKIT_POWER_QUIESCED);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("unable to send shutdown message\n");
        return false;
    }

    u64 timeout = timeout_calculate(rtk->timeout_usec ? rtk->timeout_usec : 1000000);
    while (rtk->ap_power != RTKIT_POWER_QUIESCED) {
        if (timeout_expired(timeout)) {
            rtkit_printf("AP power transition timed out\n");
            return false;
        }
        struct rtkit_message rtk_msg;
        int ret = rtkit_recv(rtk, &rtk_msg);

        if (ret > 0) {
            rtkit_printf("unexpected message to non-system endpoint 0x%02x during shutdown: %lx\n",
                         rtk_msg.ep, rtk_msg.msg);
            continue;
        } else if (ret < 0) {
            rtkit_printf("IOP died during shutdown\n");
            return false;
        }
    }

    return true;
}

static bool rtkit_switch_power_state(rtkit_dev_t *rtk, enum rtkit_power_state target)
{
    struct asc_message msg;

    if (rtk->crashed)
        return false;

    if (!rtkit_quiesce_ap_power(rtk))
        return false;

    msg.msg0 = FIELD_PREP(MGMT_TYPE, MGMT_MSG_IOP_PWR_STATE) | FIELD_PREP(MGMT_PWR_STATE, target);
    msg.msg1 = RTKIT_EP_MGMT;
    if (!asc_send(rtk->asc, &msg)) {
        rtkit_printf("unable to send shutdown message\n");
        return false;
    }

    /*
     * The IOP finishes its own work before it acknowledges a sleep: the M1-M3 ANS completes its
     * background GC bitmap rebuild and saves its context first, which its log times at up to
     * about 0.55 s on the M2 and leaves the whole handshake as close as 0.77 s to a one second
     * limit. Allow it the same five seconds as an NVMe normal shutdown.
     */
    u64 timeout =
        timeout_calculate(rtk->timeout_usec ? rtk->timeout_usec : RTKIT_IOP_POWER_TIMEOUT);
    while (rtk->iop_power != target) {
        if (timeout_expired(timeout)) {
            rtkit_printf("IOP power transition timed out\n");
            return false;
        }
        struct rtkit_message rtk_msg;
        int ret = rtkit_recv(rtk, &rtk_msg);

        if (ret > 0) {
            rtkit_printf("unexpected message to non-system endpoint 0x%02x during shutdown: %lx\n",
                         rtk_msg.ep, rtk_msg.msg);
            continue;
        } else if (ret < 0) {
            rtkit_printf("IOP died during shutdown\n");
            return false;
        }
    }

    return true;
}

/*
 * Stop the IOP sending syslog, ioreport and oslog traffic to the AP while the
 * IOP itself keeps running, for a next stage that uses the coprocessor without
 * servicing its mailbox.
 */
bool rtkit_quiesce_ap(rtkit_dev_t *rtk)
{
    struct rtkit_message msg;
    int ret;

    if (rtk->crashed)
        return false;

    while ((ret = rtkit_recv(rtk, &msg)) > 0)
        rtkit_printf("dropping message to non-system endpoint 0x%02x before AP quiesce: %lx\n",
                     msg.ep, msg.msg);
    if (ret < 0 || !rtkit_quiesce_ap_power(rtk))
        return false;

    /* Acknowledge anything the IOP sent along with the transition. */
    u64 timeout = timeout_calculate(50000);
    while (!timeout_expired(timeout)) {
        ret = rtkit_recv(rtk, &msg);
        if (ret < 0)
            return false;
        if (ret > 0)
            rtkit_printf("dropping message to non-system endpoint 0x%02x after AP quiesce: %lx\n",
                         msg.ep, msg.msg);
    }

    return true;
}

bool rtkit_quiesce(rtkit_dev_t *rtk)
{
    return rtkit_switch_power_state(rtk, RTKIT_POWER_QUIESCED);
}

bool rtkit_sleep(rtkit_dev_t *rtk)
{
    if (!rtkit_switch_power_state(rtk, RTKIT_POWER_SLEEP))
        return false;

    asc_cpu_stop(rtk->asc);
    return true;
}

static void rtkit_drain(rtkit_dev_t *rtk)
{
    struct asc_message msg;

    for (int i = 0; i < 64 && asc_recv(rtk->asc, &msg); i++)
        rtkit_printf("dropping late message 0x%02x: %lx\n", msg.msg1, msg.msg0);
}

/*
 * Stop an IOP that did not complete a power transition. Its CPU is left with RUN set and its
 * acknowledgement may still arrive, so a later owner would find a running coprocessor and stale
 * mailbox traffic instead of a fresh boot. Stop the CPU and drop what it sent, so that a reset
 * after this leaves the same state as a completed sleep.
 */
void rtkit_stop(rtkit_dev_t *rtk)
{
    rtkit_drain(rtk);
    asc_cpu_stop(rtk->asc);
    udelay(1000);
    rtkit_drain(rtk);
    rtk->crashed = true;
}
