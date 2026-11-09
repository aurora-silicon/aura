/* SPDX-License-Identifier: MIT */

#include "hv_xfer.h"
#include "hv.h"
#include "malloc.h"
#include "memory.h"
#include "types.h"
#include "utils.h"

/*
 * Largest window this device will bind, whether Aura-owned or guest-supplied.
 * The bound exists so a malformed SET_WINDOW cannot make EL2 walk (and then
 * maintain) an arbitrary amount of the address space. 64 MiB is far more than
 * any plausible transfer and still a bounded page walk.
 */
#define HV_XFER_MAX_WINDOW (64UL << 20)

/*
 * Stage-2 PTE bits, duplicated from src/hv/hv_vm.c because that file keeps
 * them private. If the page table format there ever changes, this must follow.
 */
#define HV_XFER_PTE_VALID       BIT(0)
#define HV_XFER_PTE_TARGET_MASK GENMASK(49, 14)

/* Stage-2 IPA width, set by hv_pt_init(). hv_pt_walk() asserts on an IPA at or
 * beyond it, so every guest-controlled address is bounded against it first. */
extern uint64_t vaddr_bits;

struct xfer_dev {
    u64 base;    /* doorbell base (guest IPA) */
    u64 own_win; /* the Aura-owned window: physical == guest IPA */
    u64 own_size;
    u64 win;     /* the ACTIVE window, physical */
    u64 win_ipa; /* the ACTIVE window as the guest addresses it */
    u64 win_size;

    u32 last_cmd;
    u32 off;
    u32 len;
    u32 tag;
    s32 status;
    u32 result;
    u32 seq;
    u32 errors;
    u32 features;

    u32 uwin_lo;
    u32 uwin_hi;
    u32 uwin_size;

    char name[HV_XFER_NAME_SIZE];
};

static struct xfer_dev *xfer_device = NULL;

static bool xfer_ipa_range_ok(u64 ipa, u64 size)
{
    u64 limit = BIT(vaddr_bits);

    return size && ipa < limit && size <= limit - ipa;
}

static bool xfer_dram_range_ok(u64 pa, u64 size)
{
    return size && pa >= ram_base && pa - ram_base <= mem_size_actual &&
           size <= mem_size_actual - (pa - ram_base);
}

static bool xfer_overlaps_dram(u64 base, u64 size)
{
    return base < ram_base + mem_size_actual && ram_base < base + size;
}

/*
 * Cache maintenance policy, stated once.
 *
 * The window is ordinary DRAM. Aura maps all of it Normal write-back at EL2
 * (mmu_add_default_mappings(), src/memory.c), and the stage-2 entry created by
 * hv_map_hw() leaves the guest free to choose its own attributes. A guest that
 * maps it Normal write-back inner-shareable (MmCached on Windows) is coherent
 * with EL2 in hardware; one that maps it non-cacheable or Device is not.
 *
 * Rather than trusting the guest's choice, this device keeps one invariant:
 * after any command that touched the window, EL2 holds NO cache lines for the
 * touched range. Clean-and-invalidate after the access is lossless under both
 * attribute choices. It always runs AFTER the access, never before: an
 * invalidate before a read could discard lines the guest had legitimately
 * dirtied.
 */
static void xfer_window_sync(struct xfer_dev *dev, u32 off, u32 len)
{
    if (!len || off >= dev->win_size)
        return;
    if (len > dev->win_size - off)
        len = dev->win_size - off;

    sysop("dsb sy");
    dc_civac_range((void *)(dev->win + off), len);
    sysop("dsb sy");
}

/*
 * Validate a guest-supplied window and translate it to a physical range.
 *
 * This is the one place the guest hands EL2 a pointer, so everything here is a
 * refusal condition. The range must be 16 KiB aligned, bounded, inside the
 * stage-2 IPA space (hv_pt_walk() asserts beyond it), mapped into the guest as
 * plain HW pages (never a hook, never unmapped), physically contiguous, and
 * entirely inside DRAM -- the last check is what stops a guest from aiming a
 * bulk transfer at an MMIO aperture, since /arm-io is mapped HW too.
 */
static int xfer_validate_window(u64 ipa, u64 size, u64 *pa_out)
{
    u64 first = 0;

    if (!size || size > HV_XFER_MAX_WINDOW)
        return -1;
    if (ipa & (HV_XFER_PAGE_SIZE - 1) || size & (HV_XFER_PAGE_SIZE - 1))
        return -1;
    if (!xfer_ipa_range_ok(ipa, size))
        return -1;

    for (u64 off = 0; off < size; off += HV_XFER_PAGE_SIZE) {
        u64 pte = hv_pt_walk(ipa + off);

        if (!pte || !(pte & HV_XFER_PTE_VALID))
            return -1;

        u64 pa = pte & HV_XFER_PTE_TARGET_MASK;

        if (off == 0)
            first = pa;
        else if (pa != first + off)
            return -1;
    }

    if (!xfer_dram_range_ok(first, size))
        return -1;

    *pa_out = first;
    return 0;
}

static void xfer_bind_own_window(struct xfer_dev *dev)
{
    dev->win = dev->own_win;
    dev->win_ipa = dev->own_win;
    dev->win_size = dev->own_size;
}

/*
 * One proxy event per doorbell command, the hv_virtio.c pattern. This runs from
 * hv_handle_dabort(), which the Windows hv_exc_sync() reaches only on its slow
 * path, after hv_exc_entry() has taken the big HV lock. _hv_exc_proxy() then
 * rendezvouses every vCPU (time stealing is on by default), suspends the HV
 * watchdog for the host round trip and charges the round trip to stolen_time,
 * which hv_exc_exit() publishes through CNTVOFF_EL2.
 */
static void xfer_call_host(struct exc_info *ctx, struct xfer_dev *dev, u32 cmd)
{
    struct hv_xfer_exc_info info = {
        .devbase = dev->base,
        .win_phys = dev->win,
        .win_size = dev->win_size,
        .name_phys = (u64)dev->name,
        .cmd = cmd,
        .tag = dev->tag,
        .off = dev->off,
        .len = dev->len,
        .result = 0,
        /* Poison. A host that died mid-handling reads as "declined", never
         * as "succeeded, moved nothing". */
        .status = HV_XFER_ST_NODEV,
    };

    hv_exc_proxy(ctx, START_HV, HV_XFER, &info);

    dev->status = info.status;
    dev->result = info.status == HV_XFER_ST_OK ? info.result : 0;

    /*
     * A data command can never have moved more than it asked for. A host that
     * claims otherwise would make the guest copy past the range it validated,
     * so the claim is turned into a failure rather than passed on.
     */
    if ((cmd == HV_XFER_CMD_GET || cmd == HV_XFER_CMD_PUT) && dev->result > dev->len) {
        printf("hv_xfer: host claimed %u bytes for a %u-byte command %u\n", dev->result, dev->len,
               cmd);
        dev->status = HV_XFER_ST_IO;
        dev->result = 0;
    }

    if (info.status == HV_XFER_ST_OK)
        dev->features |= HV_XFER_FEAT_HOST;
}

static void xfer_execute(struct exc_info *ctx, struct xfer_dev *dev, u32 cmd)
{
    dev->last_cmd = cmd;
    dev->status = HV_XFER_ST_OK;
    dev->result = 0;

    switch (cmd) {
        case HV_XFER_CMD_NOP:
            break;

        case HV_XFER_CMD_PING:
        case HV_XFER_CMD_OPEN:
        case HV_XFER_CMD_CLOSE:
            xfer_call_host(ctx, dev, cmd);
            break;

        case HV_XFER_CMD_GET:
        case HV_XFER_CMD_PUT:
            if (!dev->len || dev->off >= dev->win_size || dev->len > dev->win_size - dev->off) {
                dev->status = HV_XFER_ST_RANGE;
                break;
            }
            xfer_call_host(ctx, dev, cmd);
            /* Sync the range the host actually touched. On a GET that is what
             * it wrote; on a PUT it is what it read, and the invariant in
             * xfer_window_sync() wants EL2 clean either way. */
            xfer_window_sync(dev, dev->off, cmd == HV_XFER_CMD_GET ? dev->result : dev->len);
            break;

        case HV_XFER_CMD_SET_WINDOW: {
            u64 ipa = ((u64)dev->uwin_hi << 32) | dev->uwin_lo;
            u64 size = dev->uwin_size;
            u64 pa;

            if (xfer_validate_window(ipa, size, &pa) < 0) {
                printf("hv_xfer: refusing guest window 0x%lx+0x%lx\n", ipa, size);
                dev->status = HV_XFER_ST_FAULT;
                break;
            }

            dev->win = pa;
            dev->win_ipa = ipa;
            dev->win_size = size;
            printf("hv_xfer: window bound to guest buffer 0x%lx (phys 0x%lx) +0x%lx\n", ipa, pa,
                   size);
            break;
        }

        case HV_XFER_CMD_RESET_WINDOW:
            xfer_bind_own_window(dev);
            break;

        default:
            dev->status = HV_XFER_ST_BADCMD;
            break;
    }

    if (dev->status != HV_XFER_ST_OK) {
        if (dev->errors != UINT32_MAX)
            dev->errors++;
    } else {
        dev->seq++;
    }
}

static u32 xfer_read_reg(struct xfer_dev *dev, u32 off)
{
    switch (off) {
        case HV_XFER_REG_ID:
            return HV_XFER_ID;
        case HV_XFER_REG_VERSION:
            return HV_XFER_VERSION;
        case HV_XFER_REG_FEATURES:
            return dev->features;
        case HV_XFER_REG_PAGESIZE:
            return HV_XFER_PAGE_SIZE;
        case HV_XFER_REG_WIN_LO:
            return (u32)dev->win_ipa;
        case HV_XFER_REG_WIN_HI:
            return (u32)(dev->win_ipa >> 32);
        case HV_XFER_REG_WIN_SIZE:
            return (u32)dev->win_size;
        case HV_XFER_REG_MAX_XFER:
            return (u32)dev->win_size;
        case HV_XFER_REG_CMD:
            return dev->last_cmd;
        case HV_XFER_REG_OFF:
            return dev->off;
        case HV_XFER_REG_LEN:
            return dev->len;
        case HV_XFER_REG_TAG:
            return dev->tag;
        case HV_XFER_REG_STATUS:
            return (u32)dev->status;
        case HV_XFER_REG_RESULT:
            return dev->result;
        case HV_XFER_REG_SEQ:
            return dev->seq;
        case HV_XFER_REG_ERRORS:
            return dev->errors;
        case HV_XFER_REG_UWIN_LO:
            return dev->uwin_lo;
        case HV_XFER_REG_UWIN_HI:
            return dev->uwin_hi;
        case HV_XFER_REG_UWIN_SIZE:
            return dev->uwin_size;
        default:
            /*
             * Zero, never all-ones: an all-ones read is how a MISSING device
             * presents, and a guest probing for this one must be able to tell
             * "present but this register is unimplemented" from "not there".
             */
            return 0;
    }
}

static void xfer_write_reg(struct exc_info *ctx, struct xfer_dev *dev, u32 off, u32 val,
                           bool isolated32)
{
    switch (off) {
        case HV_XFER_REG_CMD:
            /*
             * Commands execute only on an isolated 32-bit store. A wider store
             * covering CMD would also cover OFF, and there is no ordering in
             * which both "OFF was set first" and "the store is one access" can
             * be true. Refusing is the only honest answer.
             */
            if (!isolated32) {
                dev->status = HV_XFER_ST_INVAL;
                if (dev->errors != UINT32_MAX)
                    dev->errors++;
                break;
            }
            xfer_execute(ctx, dev, val);
            break;
        case HV_XFER_REG_OFF:
            dev->off = val;
            break;
        case HV_XFER_REG_LEN:
            dev->len = val;
            break;
        case HV_XFER_REG_TAG:
            dev->tag = val;
            break;
        case HV_XFER_REG_UWIN_LO:
            dev->uwin_lo = val;
            break;
        case HV_XFER_REG_UWIN_HI:
            dev->uwin_hi = val;
            break;
        case HV_XFER_REG_UWIN_SIZE:
            dev->uwin_size = val;
            break;
        default:
            /* Read-only, reserved or unimplemented: absorbed. An MMIO abort
             * inside the guest's driver is far worse than a dropped write. */
            break;
    }
}

/*
 * Stage-2 hook. Returning false makes hv_handle_dabort() fail, which forwards
 * the access as EXC_SYNC; the Windows launcher answers that by aborting the
 * guest. That is reserved for accesses the ABI forbids -- anything not 4 or 8
 * bytes wide, unaligned, or running off the end of NAME -- because there is no
 * honest value to return for them. The guest driver issues 32-bit accesses
 * only, including into NAME.
 */
static bool handle_xfer(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    struct xfer_dev *dev = xfer_device;
    u32 bytes;

    if (!dev || (addr & ~(u64)(HV_XFER_REGS_SIZE - 1)) != dev->base)
        return false;

    addr &= HV_XFER_REGS_SIZE - 1;

    /* `width` is log2 of the access size in bytes -- see hv_emulate_rw() in
     * src/hv/hv_vm.c, which hands hooks `1 << width` bytes. */
    if (width < 0 || width > 3)
        return false;
    bytes = 1u << width;

    /* The name field is a byte array and is accessed as one. */
    if (addr >= HV_XFER_REG_NAME && addr < HV_XFER_REG_NAME + HV_XFER_NAME_SIZE) {
        u64 pos = addr - HV_XFER_REG_NAME;

        if (pos + bytes > HV_XFER_NAME_SIZE)
            return false;

        if (write) {
            for (u32 i = 0; i < bytes; i++)
                dev->name[pos + i] = (*val >> (8 * i)) & 0xff;
        } else {
            u64 v = 0;
            for (u32 i = 0; i < bytes; i++)
                v |= ((u64)(u8)dev->name[pos + i]) << (8 * i);
            *val = v;
        }
        return true;
    }

    if (addr & 3)
        return false;

    if (write) {
        if (bytes == 8) {
            /* Ascending order, so a 64-bit store to OFF/LEN or UWIN_LO/HI does
             * what it looks like. CMD refuses this path (see xfer_write_reg). */
            xfer_write_reg(ctx, dev, addr, (u32)*val, false);
            xfer_write_reg(ctx, dev, addr + 4, (u32)(*val >> 32), false);
        } else if (bytes == 4) {
            xfer_write_reg(ctx, dev, addr, (u32)*val, true);
        } else {
            return false;
        }
    } else {
        if (bytes == 8)
            *val = xfer_read_reg(dev, addr) | ((u64)xfer_read_reg(dev, addr + 4) << 32);
        else if (bytes == 4)
            *val = xfer_read_reg(dev, addr);
        else
            return false;
    }

    return true;
}

/* True if any word of [base, base + size) already has a stage-2 entry. Walks
 * every 4-byte L4 slot so a sub-page hook (an AIC or vGIC window registered by
 * C code the host knows nothing about) cannot hide inside the page. */
static bool xfer_range_mapped(u64 base, u64 size, u64 step)
{
    for (u64 off = 0; off < size; off += step)
        if (hv_pt_walk(base + off))
            return true;
    return false;
}

int hv_map_xfer(u64 base, u64 win, u64 win_size)
{
    struct xfer_dev *dev;

    if (xfer_device) {
        printf("hv_xfer: a channel is already mapped at 0x%lx\n", xfer_device->base);
        return -1;
    }

    if (base & (HV_XFER_REGS_SIZE - 1)) {
        printf("hv_xfer: doorbell base 0x%lx is not 16 KiB aligned\n", base);
        return -1;
    }

    if (!xfer_ipa_range_ok(base, HV_XFER_REGS_SIZE)) {
        printf("hv_xfer: doorbell base 0x%lx is outside the %lu-bit IPA space\n", base, vaddr_bits);
        return -1;
    }

    if (xfer_overlaps_dram(base, HV_XFER_REGS_SIZE)) {
        printf("hv_xfer: doorbell base 0x%lx is inside DRAM\n", base);
        return -1;
    }

    if (!win_size || win_size > HV_XFER_MAX_WINDOW || (win & (HV_XFER_PAGE_SIZE - 1)) ||
        (win_size & (HV_XFER_PAGE_SIZE - 1))) {
        printf("hv_xfer: window 0x%lx+0x%lx is not a valid 16 KiB-aligned range\n", win, win_size);
        return -1;
    }

    if (!xfer_dram_range_ok(win, win_size) || !xfer_ipa_range_ok(win, win_size)) {
        printf("hv_xfer: window 0x%lx+0x%lx is outside DRAM 0x%lx+0x%lx\n", win, win_size, ram_base,
               mem_size_actual);
        return -1;
    }

    /* Refuse, never replace: an existing entry is guest RAM, passthrough MMIO
     * or another device, and silently remapping any of them is worse than
     * having no bulk channel. */
    if (xfer_range_mapped(base, HV_XFER_REGS_SIZE, 4)) {
        printf("hv_xfer: doorbell 0x%lx already has a stage-2 mapping\n", base);
        return -1;
    }
    if (xfer_range_mapped(win, win_size, HV_XFER_PAGE_SIZE)) {
        printf("hv_xfer: window 0x%lx+0x%lx is already mapped into the guest\n", win, win_size);
        return -1;
    }

    dev = calloc(1, sizeof(*dev));
    if (!dev)
        return -1;

    dev->base = base;
    dev->own_win = win;
    dev->own_size = win_size;
    dev->features = HV_XFER_FEAT_SET_WINDOW;
    xfer_bind_own_window(dev);

    /*
     * Order matters. Map the window first: a guest that somehow reached the
     * doorbell before the window existed could be handed a WIN_LO/WIN_HI that
     * is not mapped yet. A failure leaves the guest with no channel at all.
     */
    if (hv_map_hw(win, win, win_size) < 0) {
        printf("hv_xfer: failed to map window 0x%lx+0x%lx into the guest\n", win, win_size);
        free(dev);
        return -1;
    }

    if (hv_map_hook(base, handle_xfer, HV_XFER_REGS_SIZE) < 0) {
        printf("hv_xfer: failed to map doorbell at 0x%lx\n", base);
        hv_unmap(win, win_size);
        free(dev);
        return -1;
    }

    xfer_device = dev;

    printf("hv_xfer: doorbell at 0x%lx, window 0x%lx+0x%lx (%lu KiB), name field at %p\n", base,
           win, win_size, win_size >> 10, dev->name);

    return 0;
}
