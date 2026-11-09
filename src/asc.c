/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "adt.h"
#include "asc.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#define ASC_CPU_CONTROL        0x44
#define ASC_CPU_CONTROL_START  0x10
#define ASC_CPU_STATUS         0x48
#define ASC_CPU_STATUS_STOPPED BIT(1)
#define ASC_CPU_STATUS_IDLE    BIT(5)

#define ASC_MBOX_CONTROL_ENABLE   BIT(0)
#define ASC_MBOX_CONTROL_FULL     BIT(16)
#define ASC_MBOX_CONTROL_EMPTY    BIT(17)
#define ASC_MBOX_CONTROL_OVERFLOW BIT(18)
#define ASC_MBOX_CONTROL_COUNT    GENMASK(23, 20)

#define ASC_MBOX_A2I_CONTROL 0x110
#define ASC_MBOX_A2I_SEND0   0x800
#define ASC_MBOX_A2I_SEND1   0x808
#define ASC_MBOX_A2I_RECV0   0x810
#define ASC_MBOX_A2I_RECV1   0x818

#define ASC_MBOX_I2A_CONTROL 0x114
#define ASC_MBOX_I2A_SEND0   0x820
#define ASC_MBOX_I2A_SEND1   0x828
#define ASC_MBOX_I2A_RECV0   0x830
#define ASC_MBOX_I2A_RECV1   0x838

#define ASC_V8_MBOX_SEND   0x10
#define ASC_V8_MBOX_STATUS 0x20
#define ASC_V8_MBOX_RX     GENMASK(3, 0)
#define ASC_V8_MBOX_TX     GENMASK(11, 8)

struct asc_dev {
    uintptr_t cpu_base;
    uintptr_t base;
    int iop_node;
    bool postoffice;
};

/* Firmware ADTs use both integer and NUL-terminated boolean properties. */
static int asc_get_bool(int node, const char *name)
{
    u32 len;
    const void *prop = adt_getprop(adt, node, name, &len);
    if (!prop)
        return 0;
    if (len == 4) {
        u32 value;
        memcpy(&value, prop, sizeof(value));
        return !!value;
    }
    if (len == 5 && !memcmp(prop, "true", 5))
        return 1;
    if (len == 6 && !memcmp(prop, "false", 6))
        return 0;
    return -1;
}

asc_dev_t *asc_init(const char *path)
{
    int asc_path[8];
    int node = adt_path_offset_trace(adt, path, asc_path);
    if (node < 0) {
        printf("asc: Error getting ASC node %s\n", path);
        return NULL;
    }

    u64 base;
    if (adt_get_reg(adt, asc_path, "reg", 0, &base, NULL) < 0) {
        printf("asc: Error getting ASC %s base address.\n", path);
        return NULL;
    }

    asc_dev_t *asc = calloc(1, sizeof(*asc));
    if (!asc)
        return NULL;

    asc->iop_node = adt_first_child_offset(adt, node);
    asc->cpu_base = base;
    asc->base = base + 0x8000;

    if (adt_is_compatible(adt, node, "iop,ascwrap-v8")) {
        u64 msgbox, size;
        u32 mailbox = 0;
        u32 mailbox_len;
        const void *mailbox_prop = adt_getprop(adt, node, "msgbox-mailbox-num", &mailbox_len);
        /* All three optional mailbox properties default to zero when
         * absent. The J873 SMC omits them and uses mailbox 0,
         * aperture 1 in the non-alias bank. Reject malformed present data. */
        if (mailbox_prop) {
            if (mailbox_len != sizeof(mailbox)) {
                printf("asc: malformed ASCWrap-v8 mailbox number for %s\n", path);
                free(asc);
                return NULL;
            }
            memcpy(&mailbox, mailbox_prop, sizeof(mailbox));
        }
        int postoffice = asc_get_bool(node, "msgbox-using-postoffice");
        int using_alias = asc_get_bool(node, "msgbox-using-alias");
        if (postoffice < 0 || using_alias < 0 ||
            adt_get_reg(adt, asc_path, "reg", 2, &msgbox, &size) < 0) {
            printf("asc: invalid ASCWrap-v8 mailbox metadata for %s\n", path);
            free(asc);
            return NULL;
        }

        /* 26A428 AppleMessageBoxMailbox: two 128-byte apertures per mailbox.
         * The using-alias property selects the first 64 KiB register bank. */
        u64 offset = (2ULL * mailbox + !postoffice) * 0x80;
        if (!using_alias)
            offset += 0x10000;
        if (size < ASC_V8_MBOX_STATUS + 4 || offset > size - ASC_V8_MBOX_STATUS - 4) {
            printf("asc: ASCWrap-v8 mailbox lies outside %s reg[2]\n", path);
            free(asc);
            return NULL;
        }
        asc->base = msgbox + offset;
        asc->postoffice = true;
        printf("asc: %s ASCWrap-v8 mailbox %u at %lx\n", path, mailbox, asc->base);
    }

    // clear32(base + ASC_CPU_CONTROL, ASC_CPU_CONTROL_START);
    return asc;
}

void asc_free(asc_dev_t *asc)
{
    free(asc);
}

int asc_get_iop_node(asc_dev_t *asc)
{
    return asc->iop_node;
}

bool asc_is_v8(asc_dev_t *asc)
{
    return asc->postoffice;
}

void asc_cpu_start(asc_dev_t *asc)
{
    set32(asc->cpu_base + ASC_CPU_CONTROL, ASC_CPU_CONTROL_START);
}

void asc_cpu_stop(asc_dev_t *asc)
{
    clear32(asc->cpu_base + ASC_CPU_CONTROL, ASC_CPU_CONTROL_START);
}

bool asc_cpu_running(asc_dev_t *asc)
{
    return read32(asc->cpu_base + ASC_CPU_CONTROL) & ASC_CPU_CONTROL_START;
}

/* The mailbox's receive side (I2A) on or off, as macOS sets it around an IOP's power loss. */
void asc_i2a_enable(asc_dev_t *asc, bool on)
{
    if (on)
        set32(asc->base + ASC_MBOX_I2A_CONTROL, ASC_MBOX_CONTROL_ENABLE);
    else
        clear32(asc->base + ASC_MBOX_I2A_CONTROL, ASC_MBOX_CONTROL_ENABLE);
}

/* Stopped in its idle loop, as an IOP that acknowledged an idle request reads. */
bool asc_cpu_idle(asc_dev_t *asc)
{
    u32 status = read32(asc->cpu_base + ASC_CPU_STATUS);

    return (status & ASC_CPU_STATUS_STOPPED) && (status & ASC_CPU_STATUS_IDLE);
}

static bool asc_mailbox_empty(u32 control)
{
    return (control & (ASC_MBOX_CONTROL_ENABLE | ASC_MBOX_CONTROL_EMPTY)) ==
               (ASC_MBOX_CONTROL_ENABLE | ASC_MBOX_CONTROL_EMPTY) &&
           !(control &
             (ASC_MBOX_CONTROL_FULL | ASC_MBOX_CONTROL_OVERFLOW | ASC_MBOX_CONTROL_COUNT)) &&
           ((control >> 12) & 0xf) == ((control >> 8) & 0xf);
}

/*
 * The internal DCP is handed over running. The external DCP is handed over
 * only in the state Linux adopts: started, but stopped in its idle loop, as it
 * is left while no display is attached. Every other state is refused.
 */
static bool asc_inherited_cpu_state(u32 control, u32 status, enum asc_inherited_state state)
{
    if (!(control & ASC_CPU_CONTROL_START))
        return false;
    if (state == ASC_INHERITED_IDLE)
        return (status & ASC_CPU_STATUS_STOPPED) && (status & ASC_CPU_STATUS_IDLE);
    return !(status & ASC_CPU_STATUS_STOPPED);
}

bool asc_validate_inherited_dcp(asc_dev_t *asc, const char *name, enum asc_inherited_state state)
{
    u32 control = read32(asc->cpu_base + ASC_CPU_CONTROL);
    u32 status = read32(asc->cpu_base + ASC_CPU_STATUS);
    printf("asc: %s inherited CPU control=0x%x status=0x%x\n", name, control, status);
    if (!asc_inherited_cpu_state(control, status, state)) {
        printf("asc: %s inherited CPU is not %s\n", name,
               state == ASC_INHERITED_IDLE ? "started and idle" : "running");
        return false;
    }

    u32 a2i = 0, i2a = 0;
    u64 timeout = timeout_calculate(200000);
    bool empty;
    do {
        a2i = read32(asc->base + ASC_MBOX_A2I_CONTROL);
        i2a = read32(asc->base + ASC_MBOX_I2A_CONTROL);
        empty = asc_mailbox_empty(a2i) && asc_mailbox_empty(i2a);
    } while (!empty && !timeout_expired(timeout));
    printf("asc: %s inherited mailboxes A2I=0x%x I2A=0x%x (%s)\n", name, a2i, i2a,
           empty ? "healthy and empty" : "timeout or unhealthy");
    if (!empty)
        return false;

    control = read32(asc->cpu_base + ASC_CPU_CONTROL);
    status = read32(asc->cpu_base + ASC_CPU_STATUS);
    printf("asc: %s inherited CPU recheck control=0x%x status=0x%x\n", name, control, status);
    if (!asc_inherited_cpu_state(control, status, state)) {
        printf("asc: %s inherited CPU changed state during mailbox check\n", name);
        return false;
    }
    return true;
}

bool asc_can_recv(asc_dev_t *asc)
{
    if (asc->postoffice)
        return read32(asc->base + ASC_V8_MBOX_STATUS) & ASC_V8_MBOX_RX;
    return !(read32(asc->base + ASC_MBOX_I2A_CONTROL) & ASC_MBOX_CONTROL_EMPTY);
}

bool asc_recv(asc_dev_t *asc, struct asc_message *msg)
{
    if (!asc_can_recv(asc))
        return false;

    if (asc->postoffice) {
        u64 msg0, msg1;
        asm volatile("ldp %0, %1, [%2]" : "=&r"(msg0), "=&r"(msg1) : "r"(asc->base) : "memory");
        msg->msg0 = msg0;
        msg->msg1 = msg1;
    } else {
        msg->msg0 = read64(asc->base + ASC_MBOX_I2A_RECV0);
        msg->msg1 = (u32)read64(asc->base + ASC_MBOX_I2A_RECV1);
    }
    dma_rmb();

    // printf("received msg: %lx %x\n", msg->msg0, msg->msg1);

    return true;
}

bool asc_recv_timeout(asc_dev_t *asc, struct asc_message *msg, u32 delay_usec)
{
    u64 timeout = timeout_calculate(delay_usec);
    while (!timeout_expired(timeout)) {
        if (asc_recv(asc, msg))
            return true;
    }
    return false;
}

bool asc_can_send(asc_dev_t *asc)
{
    if (asc->postoffice)
        return read32(asc->base + ASC_V8_MBOX_STATUS) & ASC_V8_MBOX_TX;
    return !(read32(asc->base + ASC_MBOX_A2I_CONTROL) & ASC_MBOX_CONTROL_FULL);
}

bool asc_send(asc_dev_t *asc, const struct asc_message *msg)
{
    if (asc->postoffice) {
        u64 timeout = timeout_calculate(200000);
        while (!asc_can_send(asc)) {
            if (timeout_expired(timeout)) {
                printf("asc: ASCWrap-v8 mailbox full for 200ms\n");
                return false;
            }
        }
        dma_wmb();
        asm volatile("stp %0, %1, [%2]" ::"r"(msg->msg0), "r"((u64)msg->msg1),
                     "r"(asc->base + ASC_V8_MBOX_SEND)
                     : "memory");
        return true;
    }
    if (poll32(asc->base + ASC_MBOX_A2I_CONTROL, ASC_MBOX_CONTROL_FULL, 0, 200000)) {
        printf("asc: A2I mailbox full for 200ms. Is the ASC stuck?");
        return false;
    }

    dma_wmb();
    write64(asc->base + ASC_MBOX_A2I_SEND0, msg->msg0);
    write64(asc->base + ASC_MBOX_A2I_SEND1, msg->msg1);

    // printf("sent msg: %lx %x\n", msg->msg0, msg->msg1);
    return true;
}
