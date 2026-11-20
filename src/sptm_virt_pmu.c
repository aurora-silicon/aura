/* SPDX-License-Identifier: MIT */
/*
 * iBoot's failed-boot count while a guest runs.
 *
 * aura_main clears the count once, when this image starts (pmu_counter.c). A guest under the
 * hypervisor never ends its boot the way iBoot counts as clean, and on a T8132 Mac mini a run of
 * guests, each ended by a reset, left 2 behind every time and twice ended in recoveryOS (11-19,
 * 11-20). So aura also keeps the count at zero while the guest runs: XNU entry + 30 s, + 90 s, then
 * every 120 s, and logs every non-zero value it clears (which also says who counts, and when).
 *
 * aura's transaction must not interleave with one of XNU's on the same controller. The guest's
 * accesses to the controller's register page trap here, under one lock, and this file follows
 * XNU's commands: a command word opens one, its data words follow, and it closes once XNU has read
 * the reply and the RX FIFO is empty again. aura writes only between XNU's commands, with both
 * FIFOs empty, holding the lock, so XNU's next access waits until aura's reply has been read.
 *
 * That holds for a driver that polls the controller with 32-bit accesses to its status, command and
 * reply registers. aura stops keeping the count, for the rest of the boot, on the first sign of
 * anything else: XNU writing a non-zero value to any other register of the page (an interrupt
 * enable or acknowledge: aura's own reply would then interrupt XNU), a command-register write of
 * another width, or a transaction of aura's that fails (its reply is drained first, so XNU never
 * reads it). The registers XNU uses besides those three are logged with the first rounds.
 */
#include "adt.h"
#include "hv.h"
#include "pmu_counter.h"
#include "spmi.h"
#include "sptm_virt.h"
#include "sptm_virt_priv.h"
#include "utils.h"

#define PMU_KEEP_FIRST_S  30
#define PMU_KEEP_SECOND_S 90
#define PMU_KEEP_EVERY_S  120
/* XNU abandoned a command (timeout, reset) if it stays open this long with both FIFOs empty. */
#define PMU_STALE_S 1

#define SPMI_OPC_EXT_WRITE  0x00 /* 0x00-0x0f: 8-bit register, (opc & 0xf) + 1 bytes */
#define SPMI_OPC_EXT_WRITEL 0x30 /* 0x30-0x37: 16-bit register, (opc & 7) + 1 bytes */
#define SPMI_OPC_EXT_READL  0x38 /* 0x38-0x3f: 16-bit register, (opc & 7) + 1 bytes */
/* XNU's commands on the scratchpad (boot stage +1, failed-boot count +2, ...) are logged. */
#define PK_WATCH_LOGS 48
#define PK_OTHER      16

static struct {
    spmi_dev_t *dev;
    u8 slave;
    u16 counter;
    u64 status, cmd, reply;
    u32 rx_empty, tx_empty;
    bool on;
    /* XNU's command in flight */
    bool open;
    int data_words;
    u64 open_t;
    /* the schedule */
    u64 next_t;
    int round;
    u32 cleared, skipped;
    /* XNU's accesses to the page's other registers */
    struct {
        u32 off, reads, writes;
        u64 last;
    } other[PK_OTHER];
    int n_other;
    bool other_lost, other_nonzero_write;
    /* XNU's command on the scratchpad in flight: its register, and whether it writes */
    int watch_reg;
    bool watch_write;
    u32 watch_logs;
} pk;

static DECLARE_SPINLOCK(pk_lock);

static bool pk_fifos_empty(void)
{
    u32 st = read32(pk.status);
    return (st & pk.rx_empty) && (st & pk.tx_empty);
}

static void pk_print_other(void)
{
    if (!pk.n_other)
        printf("SPTM-virt[pmu]: XNU has used only the status, command and reply registers\n");
    for (int i = 0; i < pk.n_other; i++)
        printf(
            "SPTM-virt[pmu]: XNU's other register +0x%x: %u reads, %u writes, last write 0x%lx\n",
            pk.other[i].off, pk.other[i].reads, pk.other[i].writes, pk.other[i].last);
    if (pk.other_lost)
        printf("SPTM-virt[pmu]: (more registers than recorded)\n");
}

/* For the rest of the boot: the hook stays, as a pass-through. With pk_lock held. */
static void pk_off(const char *why)
{
    if (!pk.on)
        return;
    pk.on = false;
    printf("SPTM-virt[pmu]: no longer keeping the failed-boot count: %s (round %d, %u cleared)\n",
           why, pk.round, pk.cleared);
    pk_print_other();
}

/*
 * aura's transaction failed, and its reply may still come. With pk_lock held (XNU waits), drain
 * RX until both FIFOs have stayed empty for 1 ms, for at most 20 ms, then stop keeping.
 */
static void pk_fail(const char *what)
{
    u64 f = mrs(CNTFRQ_EL0), t0 = mrs(CNTPCT_EL0), quiet = t0, now;
    u32 drained = 0;
    bool clean = false;

    while ((now = mrs(CNTPCT_EL0)) - t0 < f / 50) {
        u32 st = read32(pk.status);
        if (!(st & pk.rx_empty)) {
            read32(pk.reply);
            drained++;
            quiet = now;
        } else if (!(st & pk.tx_empty)) {
            quiet = now;
        } else if (now - quiet > f / 1000) {
            clean = true;
            break;
        }
    }
    printf("SPTM-virt[pmu]: %s failed; %u reply words drained, FIFOs %s\n", what, drained,
           clean ? "empty" : "STILL NOT EMPTY after 20 ms");
    pk_off("aura's own transaction failed");
}

/* With pk_lock held and XNU between commands. */
static void pk_clear(void)
{
    u8 v = 0, zero = 0;
    if (spmi_ext_read_long(pk.dev, pk.slave, pk.counter, &v, 1) < 0) {
        pk_fail("reading the failed-boot count");
        return;
    }
    if (v && spmi_ext_write_long(pk.dev, pk.slave, pk.counter, &zero, 1) < 0) {
        printf("SPTM-virt[pmu]: clearing the failed-boot count (0x%02x)\n", v);
        pk_fail("clearing the failed-boot count");
        return;
    }
    if (v || pk.round < 2) {
        printf("SPTM-virt[pmu]: failed-boot count 0x%02x%s (round %d)\n", v, v ? " -> 0x00" : "",
               pk.round);
        if (pk.round < 2)
            pk_print_other();
    }
    if (v)
        pk.cleared++;
}

/* With pk_lock held: an access of XNU's to a register other than status, command and reply. */
static void pk_note_other(u64 addr, bool write, u64 val)
{
    u32 off = addr & 0x3fff;
    int i;

    for (i = 0; i < pk.n_other && pk.other[i].off != off; i++)
        ;
    if (i == pk.n_other) {
        if (i == PK_OTHER) {
            pk.other_lost = true;
            i = -1;
        } else {
            pk.other[pk.n_other++].off = off;
        }
    }
    if (i >= 0 && write) {
        pk.other[i].writes++;
        pk.other[i].last = val;
    } else if (i >= 0) {
        pk.other[i].reads++;
    }
    if (write && val)
        pk.other_nonzero_write = true;
}

static void pk_schedule(u64 now)
{
    u64 f = mrs(CNTFRQ_EL0);
    pk.round++;
    pk.next_t = now + f * (pk.round == 1 ? PMU_KEEP_SECOND_S - PMU_KEEP_FIRST_S : PMU_KEEP_EVERY_S);
}

/* With pk_lock held. */
static void pk_maybe(u64 now)
{
    if (!pk.on || !pk.next_t || now < pk.next_t)
        return;
    if (pk.other_nonzero_write) {
        pk_off("XNU wrote a non-zero value to another register of the controller (interrupts?)");
        return;
    }
    if (pk.open && !(now - pk.open_t > PMU_STALE_S * mrs(CNTFRQ_EL0) && pk_fifos_empty()))
        return; /* XNU has a command in flight: at its end, or the next trap */
    if (!pk_fifos_empty()) {
        if (pk.skipped++ < 4)
            printf("SPTM-virt[pmu]: controller FIFOs not empty between XNU's commands; later\n");
        return;
    }
    pk.open = false;
    pk.data_words = 0;
    pk_clear();
    pk_schedule(now);
}

static bool pk_hook(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width)
{
    UNUSED(ctx);
    u64 now = mrs(CNTPCT_EL0);

    spin_lock(&pk_lock);
    if (addr != pk.status && addr != pk.cmd && addr != pk.reply)
        pk_note_other(addr, write, write ? *val : 0);
    if (write && addr == pk.cmd && width != 2)
        pk_off("a command-register write that isn't 32-bit");
    if (write && addr == pk.cmd && width == 2) {
        u32 v = *val;
        if (pk.data_words > 0) {
            pk.data_words--;
            if (pk.watch_reg >= 0 && pk.watch_write) {
                /* The first data word after an EXT_WRITEL: its bytes, the register's first. */
                if (pk.watch_logs++ < PK_WATCH_LOGS)
                    printf("SPTM-virt[pmu]: XNU writes PMU 0x%x = 0x%08x (first data word)\n",
                           pk.watch_reg, v);
                pk.watch_reg = -1;
            }
        } else {
            /* A new command: aura's turn first, if one is due. */
            pk_maybe(now);
            u32 opc = v & 0xff, len = 0;
            if ((opc & 0xf8) == SPMI_OPC_EXT_WRITEL)
                len = (opc & 7) + 1;
            else if (opc < 0x10)
                len = (opc & 0xf) + 1;
            pk.data_words = (len + 3) / 4;
            pk.open = true;
            pk.open_t = now;
            /* gen-1 command word: register [31:16], slave [14:8], opcode [7:0] */
            u32 reg = v >> 16, slave = (v >> 8) & 0x7f, scr = pk.counter - 2;
            pk.watch_reg = -1;
            if (slave == pk.slave && reg >= scr && reg < scr + 4 &&
                ((opc & 0xf8) == SPMI_OPC_EXT_WRITEL || (opc & 0xf8) == SPMI_OPC_EXT_READL)) {
                pk.watch_reg = reg;
                pk.watch_write = (opc & 0xf8) == SPMI_OPC_EXT_WRITEL;
                if (!pk.watch_write && pk.watch_logs++ < PK_WATCH_LOGS)
                    printf("SPTM-virt[pmu]: XNU reads PMU 0x%x (%u bytes)\n", reg, (opc & 7) + 1);
            }
        }
    }
    sptm_virt_mmio_rw(addr, val, write, width);
    if (!write && addr == pk.reply && pk.open && !pk.data_words &&
        (read32(pk.status) & pk.rx_empty))
        pk.open = false;
    spin_unlock(&pk_lock);
    return true;
}

/* From the guest's frequent traps (GL calls): a guest that leaves SPMI alone still gets cleared. */
void sptm_virt_pmu_tick(void)
{
    if (!pk.on || !pk.next_t)
        return;
    u64 now = mrs(CNTPCT_EL0);
    if (now < pk.next_t)
        return;
    spin_lock(&pk_lock);
    pk_maybe(now);
    spin_unlock(&pk_lock);
}

/* At XNU's entry: start the schedule. */
void sptm_virt_pmu_xnu_entry(void)
{
    if (!pk.on || pk.next_t)
        return;
    pk.next_t = mrs(CNTPCT_EL0) + (u64)PMU_KEEP_FIRST_S * mrs(CNTFRQ_EL0);
}

void sptm_virt_pmu_setup(void)
{
    char path[64];
    if (sptm_virt_flags & SPTM_VIRT_S2R) {
        printf("SPTM-virt[pmu]: the sleep path owns the SPMI hooks; failed-boot count not kept\n");
        return;
    }
    if (pmu_counter_locate(path, sizeof(path), &pk.slave, &pk.counter) < 0)
        return;
    pk.watch_reg = -1;
    pk.dev = spmi_init(path);
    if (!pk.dev)
        return;
    spmi_layout(pk.dev, &pk.status, &pk.cmd, &pk.reply, &pk.rx_empty, &pk.tx_empty);
    if ((pk.status & ~0x3fffUL) != (pk.cmd & ~0x3fffUL) ||
        (pk.reply & ~0x3fffUL) != (pk.cmd & ~0x3fffUL) ||
        hv_map_hook(pk.cmd & ~0x3fffUL, pk_hook, 0x4000) < 0) {
        printf("SPTM-virt[pmu]: cannot hook %s's registers; failed-boot count not kept\n", path);
        return;
    }
    pk.on = true;
    printf("SPTM-virt[pmu]: keeping iBoot's failed-boot count (%s SID 0x%x reg 0x%x) at zero: XNU "
           "entry + %d s, + %d s, then every %d s\n",
           path, pk.slave, pk.counter, PMU_KEEP_FIRST_S, PMU_KEEP_SECOND_S, PMU_KEEP_EVERY_S);
}
