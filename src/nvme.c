/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include "adt.h"
#include "assert.h"
#include "malloc.h"
#include "nvme.h"
#include "pcie.h"
#include "pmgr.h"
#include "rtkit.h"
#include "sart.h"
#include "string.h"
#include "utils.h"

#define NVME_TIMEOUT          1000000
#define NVME_ENABLE_TIMEOUT   5000000
#define NVME_SHUTDOWN_TIMEOUT 5000000
#define NVME_QUEUE_SIZE       64

#define NVME_CC            0x14
#define NVME_CC_SHN        GENMASK(15, 14)
#define NVME_CC_SHN_NONE   0
#define NVME_CC_SHN_NORMAL 1
#define NVME_CC_SHN_ABRUPT 2
#define NVME_CC_EN         BIT(0)

#define NVME_CSTS             0x1c
#define NVME_CSTS_SHST        GENMASK(3, 2)
#define NVME_CSTS_SHST_NORMAL 0
#define NVME_CSTS_SHST_BUSY   1
#define NVME_CSTS_SHST_DONE   2
#define NVME_CSTS_RDY         BIT(0)
#define NVME_CSTS_CFS         BIT(1)

#define NVME_AQA 0x24
#define NVME_ASQ 0x28
#define NVME_ACQ 0x30

#define NVME_DB_ACQ  0x1004
#define NVME_DB_IOCQ 0x100c

#define NVME_BOOT_STATUS    0x1300
#define NVME_BOOT_STATUS_OK 0xde71ce55

#define NVME_LINEAR_SQ_CTRL    0x24908
#define NVME_LINEAR_SQ_CTRL_EN BIT(0)

/*
 * ADT nvme-secure-reg-layout (T8152): AQA/ASQ/ACQ and the I/O queue registers
 * move up by 0x4000 and LINEAR_SQ_CTRL moves into the NVMMU block. CC/CSTS,
 * BOOT_STATUS, the doorbells and the NVMMU TCB registers stay.
 */
#define NVME_SECURE_QREG_SHIFT     0x4000
#define NVME_SECURE_LINEAR_SQ_CTRL 0x2813c

#define NVME_IOQ_CMDS           0x1200
#define NVME_IOQ_CQES           0x1208
#define NVME_MAX_PEND_CMDS_CTRL 0x1210
#define NVME_DB_LINEAR_ASQ      0x2490c
#define NVME_DB_LINEAR_IOSQ     0x24910

#define NVMMU_NUM       0x28100
#define NVMMU_ASQ_BASE  0x28108
#define NVMMU_IOSQ_BASE 0x28110
#define NVMMU_TCB_INVAL 0x28118
#define NVMMU_TCB_STAT  0x28120

#define NVME_ADMIN_CMD_DELETE_SQ 0x00
#define NVME_ADMIN_CMD_CREATE_SQ 0x01
#define NVME_ADMIN_CMD_DELETE_CQ 0x04
#define NVME_ADMIN_CMD_CREATE_CQ 0x05
#define NVME_QUEUE_CONTIGUOUS    BIT(0)

#define NVME_CMD_FLUSH 0x00
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02

#define NVMMU_TCB_DMA_FROM_DEVICE BIT(0)
#define NVMMU_TCB_DMA_TO_DEVICE   BIT(1)

struct nvme_command {
    u8 opcode;
    u8 flags;
    u8 tag;
    u8 rsvd; // normal NVMe has tag as u16
    u32 nsid;
    u32 cdw2;
    u32 cdw3;
    u64 metadata;
    u64 prp1;
    u64 prp2;
    u32 cdw10;
    u32 cdw11;
    u32 cdw12;
    u32 cdw13;
    u32 cdw14;
    u32 cdw15;
};

struct nvme_completion {
    u64 result;
    u32 rsvd; // normal NVMe has the sq_head and sq_id here
    u16 tag;
    u16 status;
};

struct apple_nvmmu_tcb {
    u8 opcode;
    u8 dma_flags;
    u8 slot_id;
    u8 unk0;
    u32 len;
    u64 unk1[2];
    u64 prp1;
    u64 prp2;
    u64 unk2[2];
    u8 aes_iv[8];
    u8 _aes_unk[64];
};

struct nvme_queue {
    struct apple_nvmmu_tcb *tcbs;
    struct nvme_command *cmds;
    struct nvme_completion *cqes;

    u8 cq_head;
    u8 cq_phase;

    bool adminq;
    bool failed;
};

static_assert(sizeof(struct nvme_command) == 64, "invalid nvme_command size");
static_assert(sizeof(struct nvme_completion) == 16, "invalid nvme_completion size");
static_assert(sizeof(struct apple_nvmmu_tcb) == 128, "invalid apple_nvmmu_tcb size");

static enum {
    NVME_T8103 = 0,
    NVME_T8132 = 1,
} nvme_type;

static bool nvme_initialized = false;
static bool nvme_secure_regs;
static bool nvme_reboot_required;
/*
 * Post-M4: the SART entries in use when the last owner powered ANS off. The SART loses them with
 * ANS's power, and the firmware keeps the buffers they cover, so its next start writes them back.
 */
static struct sart_entry nvme_sart_saved[16];
static int nvme_sart_saved_cnt;
/* aura only reads the drive; only a proxy-issued flush means the drive has data to settle. */
static bool nvme_wrote;
bool nvme_keep_running_for_linux = false;
static u8 nvme_die;
/* When this stage started the ANS firmware, for the shutdown timing line. */
static u64 nvme_boot_ticks;

static asc_dev_t *nvme_asc = NULL;
static rtkit_dev_t *nvme_rtkit = NULL;
static sart_dev_t *nvme_sart = NULL;

static u64 nvme_base;
static u64 nvmmu_base;
/* Block holding LINEAR_SQ_CTRL and the linear submission queue doorbells. */
static u64 nvme_lsq_base;
static bool nvme_lsq_in_nvmmu;

static struct nvme_queue adminq, ioq;
static bool nvme_cmd_timed_out;
static bool nvme_multi_enabled = true;
static bool nvme_multi_verified;
static u64 *nvme_prp_list;
static u8 *nvme_verify_bounce;

static u64 nvme_read64_lo_hi(u64 addr)
{
    u64 lo = read32(addr);
    return lo | ((u64)read32(addr + 4) << 32);
}

static u64 nvme_qreg(u32 reg)
{
    return nvme_base + reg + (nvme_secure_regs ? NVME_SECURE_QREG_SHIFT : 0);
}

static u64 nvme_lsq_ctrl(void)
{
    return nvme_secure_regs ? nvme_base + NVME_SECURE_LINEAR_SQ_CTRL
                            : nvme_lsq_base + NVME_LINEAR_SQ_CTRL;
}

static void nvme_log_entry_state(bool running)
{
    printf("nvme: entry RUN=%d BOOT_STATUS=0x%x saved SART entries=%d\n", running,
           read32(nvme_base + NVME_BOOT_STATUS), nvme_sart_saved_cnt);
    printf("nvme: entry CC=0x%x CSTS=0x%x AQA=0x%x IOSQ=0x%lx IOCQ=0x%lx IOQA=0x%x\n",
           read32(nvme_base + NVME_CC), read32(nvme_base + NVME_CSTS), read32(nvme_qreg(NVME_AQA)),
           nvme_read64_lo_hi(nvme_qreg(NVME_IOQ_CMDS)), nvme_read64_lo_hi(nvme_qreg(NVME_IOQ_CQES)),
           read32(nvme_qreg(NVME_MAX_PEND_CMDS_CTRL)));
    printf("nvme: entry LINEAR_SQ=0x%x NVMMU_NUM=0x%x NVMMU_ASQ_TCB=0x%lx "
           "NVMMU_IOSQ_TCB=0x%lx\n",
           read32(nvme_lsq_ctrl()), read32(nvmmu_base + NVMMU_NUM),
           nvme_read64_lo_hi(nvmmu_base + NVMMU_ASQ_BASE),
           nvme_read64_lo_hi(nvmmu_base + NVMMU_IOSQ_BASE));
}

static bool alloc_queue(struct nvme_queue *q)
{
    memset(q, 0, sizeof(*q));

    q->tcbs = memalign(SZ_16K, NVME_QUEUE_SIZE * sizeof(*q->tcbs));
    if (!q->tcbs)
        return false;

    q->cmds = memalign(SZ_16K, NVME_QUEUE_SIZE * sizeof(*q->cmds));
    if (!q->cmds)
        goto free_tcbs;

    q->cqes = memalign(SZ_16K, NVME_QUEUE_SIZE * sizeof(*q->cqes));
    if (!q->cqes)
        goto free_cmds;

    memset(q->tcbs, 0, NVME_QUEUE_SIZE * sizeof(*q->tcbs));
    memset(q->cmds, 0, NVME_QUEUE_SIZE * sizeof(*q->cmds));
    memset(q->cqes, 0, NVME_QUEUE_SIZE * sizeof(*q->cqes));
    q->cq_head = 0;
    q->cq_phase = 1;
    return true;

free_cmds:
    free(q->cmds);
free_tcbs:
    free(q->tcbs);
    return false;
}

static void free_queue(struct nvme_queue *q)
{
    free(q->cmds);
    free(q->tcbs);
    free(q->cqes);
}

static void nvme_poll_syslog(void)
{
    if (!nvme_rtkit)
        return;
    struct rtkit_message msg;
    rtkit_recv(nvme_rtkit, &msg);
}

static bool nvme_ctrl_disable(void)
{
    u64 timeout = timeout_calculate(NVME_TIMEOUT);

    clear32(nvme_base + NVME_CC, NVME_CC_EN);
    while (read32(nvme_base + NVME_CSTS) & NVME_CSTS_RDY && !timeout_expired(timeout))
        nvme_poll_syslog();

    return !(read32(nvme_base + NVME_CSTS) & NVME_CSTS_RDY);
}

static bool nvme_ctrl_enable(void)
{
    u64 timeout = timeout_calculate(NVME_ENABLE_TIMEOUT);

    mask32(nvme_base + NVME_CC, NVME_CC_SHN, NVME_CC_EN);
    while (!(read32(nvme_base + NVME_CSTS) & NVME_CSTS_RDY) && !timeout_expired(timeout))
        nvme_poll_syslog();

    return read32(nvme_base + NVME_CSTS) & NVME_CSTS_RDY;
}

static bool nvme_ctrl_shutdown(void)
{
    u64 timeout = timeout_calculate(NVME_SHUTDOWN_TIMEOUT);

    mask32(nvme_base + NVME_CC, NVME_CC_SHN, FIELD_PREP(NVME_CC_SHN, NVME_CC_SHN_NORMAL));
    while (FIELD_GET(NVME_CSTS_SHST, read32(nvme_base + NVME_CSTS)) != NVME_CSTS_SHST_DONE &&
           !timeout_expired(timeout))
        nvme_poll_syslog();

    return FIELD_GET(NVME_CSTS_SHST, read32(nvme_base + NVME_CSTS)) == NVME_CSTS_SHST_DONE;
}

/*
 * Put an M1-M3 ANS to sleep and reset it, leaving it for the next owner to boot afresh: CPU
 * stopped, mailbox empty, block reset. The next stage (U-Boot, Linux) sets RUN and expects the
 * firmware's HELLO as its first message. Returns whether the firmware acknowledged the sleep.
 */
static bool nvme_ans_sleep_and_reset(void)
{
    bool asleep = nvme_rtkit && rtkit_sleep(nvme_rtkit);

    if (!asleep) {
        /*
         * Without the acknowledgement rtkit_sleep() leaves RUN set, and a reset under a
         * running CPU hands U-Boot an ANS that never says HELLO. Stop it first, as a
         * completed sleep would have.
         */
        printf("nvme: ANS did not acknowledge sleep; stopping it before the reset\n");
        if (nvme_rtkit)
            rtkit_stop(nvme_rtkit);
        else
            asc_cpu_stop(nvme_asc);
    }
    // Some machines call this ANS, some ANS2...
    pmgr_reset(nvme_die, "ANS");
    pmgr_reset(nvme_die, "ANS2");
    return asleep;
}

static bool nvme_exec_command(struct nvme_queue *q, struct nvme_command *cmd, u64 *result)
{
    /* A timed-out command may still own slot 0 and its DMA mappings. */
    if (nvme_reboot_required || q->failed)
        return false;

    bool found = false;
    bool completed = false;
    u64 timeout;
    u8 tag = 0;
    struct nvme_command *queue_cmd = &q->cmds[tag];
    struct apple_nvmmu_tcb *tcb = &q->tcbs[tag];

    nvme_cmd_timed_out = false;
    memcpy(queue_cmd, cmd, sizeof(*cmd));
    queue_cmd->tag = tag;

    memset(tcb, 0, sizeof(*tcb));
    tcb->opcode = 0;
    if (!queue_cmd->prp1)
        tcb->dma_flags = 0;
    else if (queue_cmd->opcode & 1)
        tcb->dma_flags = NVMMU_TCB_DMA_TO_DEVICE;
    else
        tcb->dma_flags = NVMMU_TCB_DMA_FROM_DEVICE;
    tcb->slot_id = tag;
    tcb->len = queue_cmd->cdw12;
    tcb->prp1 = queue_cmd->prp1;
    tcb->prp2 = queue_cmd->prp2;

    /* make sure ANS2 can see the command and tcb before triggering it */
    dma_wmb();

    nvme_poll_syslog();
    if (q->adminq)
        write32(nvme_lsq_base + NVME_DB_LINEAR_ASQ, tag);
    else
        write32(nvme_lsq_base + NVME_DB_LINEAR_IOSQ, tag);
    nvme_poll_syslog();

    timeout = timeout_calculate(NVME_TIMEOUT);
    struct nvme_completion cqe;
    while (!timeout_expired(timeout)) {
        nvme_poll_syslog();

        /* we need a DMA read barrier here since the CQ will be updated using DMA */
        dma_rmb();
        memcpy(&cqe, &q->cqes[q->cq_head], sizeof(cqe));
        if ((cqe.status & 1) != q->cq_phase)
            continue;
        completed = true;

        if (cqe.tag == tag) {
            found = true;
            if (result)
                *result = cqe.result;
        } else {
            printf("nvme: invalid tag in CQ: expected %d but got %d\n", tag, cqe.tag);
        }

        write32(nvmmu_base + NVMMU_TCB_INVAL, cqe.tag);
        if (read32(nvmmu_base + NVMMU_TCB_STAT))
            printf("nvme: NVMMU invalidation for tag %d failed\n", cqe.tag);

        /* increment head and switch phase once the end of the queue has been reached */
        q->cq_head += 1;
        if (q->cq_head == NVME_QUEUE_SIZE) {
            q->cq_head = 0;
            q->cq_phase ^= 1;
        }

        if (q->adminq)
            write32(nvme_base + NVME_DB_ACQ, q->cq_head);
        else
            write32(nvme_base + NVME_DB_IOCQ, q->cq_head);
        break;
    }

    if (!found) {
        nvme_cmd_timed_out = !completed;
        q->failed = true;
        printf("nvme: could not find command completion in CQ\n");
        return false;
    }

    cqe.status >>= 1;
    if (cqe.status) {
        printf("nvme: command failed with status %d\n", cqe.status);
        return false;
    }

    return true;
}

/* The ADT describes a post-M4 ANS: its firmware buffers outlive the stage that gave them. */
bool nvme_post_m4(void)
{
    int node = adt_path_offset(adt, "/arm-io/ans");

    return node >= 0 && (adt_get_property(adt, node, "nvme-secure-bar") ||
                         adt_get_property(adt, node, "nvme-secure-reg-layout"));
}

/*
 * Post-M4, done with ANS: disable the controller as M1-M3 do, idle the firmware as macOS does
 * before a sleep, keep the SART entries, then power ANS and the PCIe block it runs on off. Every
 * later stage that needs it starts it again with nvme_ans_power_on(). The firmware resumes from
 * what it saves at the idle, so the controller is disabled first, as earlier stages hand it over.
 */
static bool nvme_ans_power_off(void)
{
    if (read32(nvme_base + NVME_CC) & NVME_CC_EN) {
        if (nvme_wrote && !nvme_ctrl_shutdown())
            printf("nvme: controller shutdown timed out\n");
        if (!nvme_ctrl_disable())
            printf("nvme: controller disable timed out\n");
    }
    nvme_sart_saved_cnt = sart_get_entries(nvme_sart, nvme_sart_saved, ARRAY_SIZE(nvme_sart_saved));
    if (!rtkit_idle(nvme_rtkit)) {
        printf("nvme: ANS didn't acknowledge idle, stopping it anyway\n");
        asc_cpu_stop(nvme_asc);
    }
    if (pcie_ans_power_off()) {
        printf("nvme: couldn't power ANS off; it stays down until a reboot\n");
        return false;
    }
    printf("nvme: ANS powered off, %d SART entries kept\n", nvme_sart_saved_cnt);
    return true;
}

/* Post-M4, ANS off: power, the SART entries back, then the firmware's start after a power loss. */
static bool nvme_ans_power_on(void)
{
    if (pcie_init() || !sart_set_entries(nvme_sart, nvme_sart_saved, nvme_sart_saved_cnt) ||
        !rtkit_boot_warm(nvme_rtkit, BIT(0)) ||
        poll32(nvme_base + NVME_BOOT_STATUS, 0xffffffff, NVME_BOOT_STATUS_OK, USEC_PER_SEC) < 0) {
        printf("nvme: ANS didn't start from off; it stays down until a reboot\n");
        return false;
    }
    nvme_boot_ticks = get_ticks();
    printf("nvme: ANS started from off\n");
    return true;
}

bool nvme_init(void)
{
    if (nvme_reboot_required) {
        printf("nvme: setup failed; reboot required before initialization\n");
        return false;
    }
    if (nvme_initialized) {
        printf("nvme: already initialized\n");
        return true;
    }

    nvme_base = 0;
    nvmmu_base = 0;
    nvme_lsq_base = 0;
    nvme_lsq_in_nvmmu = false;
    nvme_secure_regs = false;
    nvme_die = 0;
    nvme_asc = NULL;
    nvme_rtkit = NULL;
    nvme_sart = NULL;
    memset(&adminq, 0, sizeof(adminq));
    memset(&ioq, 0, sizeof(ioq));
    nvme_multi_enabled = true;
    nvme_multi_verified = false;
    nvme_prp_list = NULL;
    nvme_verify_bounce = NULL;
    nvme_boot_ticks = 0;

    bool from_off = false, earlier = false;
    int adt_path[8];
    int node = adt_path_offset_trace(adt, "/arm-io/ans", adt_path);
    if (node < 0) {
        printf("nvme: Error getting NVMe node /arm-io/ans\n");
        goto out_reset;
    }

    u32 secure_len;
    const void *secure_bar = adt_getprop(adt, node, "nvme-secure-bar", &secure_len);
    if (secure_bar) {
        if (secure_len || !adt_is_compatible(adt, node, "iop,ascwrap-v6")) {
            printf("nvme: invalid split secure BAR descriptor\n");
            goto out_reset;
        }
        // M4+ generations have the nvme-secure-bar property and 10+ regs.
        // They use reg[3] for NVMMU registers and reg[9] for NVMe registers,
        // and require extra writes to set up the IO queues.
        nvme_type = NVME_T8132;
    } else if (adt_get_property(adt, node, "nvme-secure-reg-layout")) {
        // T8152 has the post-M4 queue setup in one reg[3] window like M1-M3,
        // with the queue registers moved (nvme-secure-reg-layout).
        nvme_type = NVME_T8132;
        nvme_secure_regs = true;
    } else {
        // M1-M3 generations use reg[3] for both NVMMU and NVMe registers.
        nvme_type = NVME_T8103;
    }

    u64 nvmmu_size, nvme_size;
    if (adt_get_reg(adt, adt_path, "reg", 3, &nvmmu_base, &nvmmu_size) < 0 || !nvmmu_base) {
        printf("nvme: Error getting NVMMU base address.\n");
        goto out_reset;
    }
    if (nvme_type == NVME_T8132 && !nvme_secure_regs) {
        if (adt_get_reg(adt, adt_path, "reg", 9, &nvme_base, &nvme_size) < 0 || !nvme_base) {
            printf("nvme: Error getting NVMe base address.\n");
            goto out_reset;
        }
        /*
         * The J700 and J713 ADTs advertise only the first 64 KiB of this alias.
         * Its linear queue registers live at +0x24908..+0x24910 in the
         * same non-secure alias. The public T8140 Linux binding describes
         * the full 0x30000 aperture. Admit only that measured SoC/layout;
         * retain the normal bounds and overlap checks for every other BAR.
         */
        if (nvme_size == 0x10000 &&
            ((chip_id == T8140 && nvmmu_base == 0x38dcc0000 && nvme_base == 0x3cdcc0000) ||
             (chip_id == T8132 && nvmmu_base == 0x485cc0000 && nvme_base == 0x4c5cc0000) ||
             /* J616C (T6041) has the same split layout: 64 KiB advertised, LSQ at +0x24908. */
             (chip_id == T6041 && nvmmu_base == 0x40dcc0000 && nvme_base == 0x44dcc0000) ||
             /* J813 (T8142) has the same split layout: 64 KiB advertised, LSQ at +0x24908. */
             (chip_id == T8142 && nvmmu_base == 0x485cc0000 && nvme_base == 0x4c5cc0000)))
            nvme_size = 0x30000;
        /*
         * On T6040/T6041 the "nvme" block really ends at 0x10000: LINEAR_SQ_CTRL and the
         * linear submission queue doorbells sit at the same offsets in the NVMMU block,
         * and the firmware enables the linear queues itself (as the Linux driver's
         * apple,t6041-nvme-ans2 entry does). T6050 has the same layout (apple,t6050-nvme-ans2).
         */
        nvme_lsq_in_nvmmu = chip_id == T6040 || chip_id == T6041 || chip_id == T6050;
        u64 lsq_size = nvme_lsq_in_nvmmu ? nvmmu_size : nvme_size;
        u64 nvme_need =
            nvme_lsq_in_nvmmu ? NVME_BOOT_STATUS + sizeof(u32) : NVME_DB_LINEAR_IOSQ + sizeof(u32);
        if (nvmmu_size < NVMMU_TCB_STAT + sizeof(u32) || nvme_size < nvme_need ||
            lsq_size < NVME_DB_LINEAR_IOSQ + sizeof(u32) || nvmmu_base > UINT64_MAX - nvmmu_size ||
            nvme_base > UINT64_MAX - nvme_size ||
            (nvmmu_base < nvme_base + nvme_size && nvme_base < nvmmu_base + nvmmu_size)) {
            printf("nvme: invalid split BAR geometry\n");
            goto out_reset;
        }
    } else {
        nvme_base = nvmmu_base;
    }
    nvme_lsq_base = nvme_lsq_in_nvmmu ? nvmmu_base : nvme_base;

    u32 cg;
    if (ADT_GETPROP(adt, node, "clock-gates", &cg) < 0) {
        printf("nvme: clock-gates not set\n");
        nvme_die = (nvme_base >> 37) & 3;
    } else {
        nvme_die = FIELD_GET(PMGR_DIE_ID, cg);
    }
    printf("nvme: ANS is on die %d\n", nvme_die);

    if (!alloc_queue(&adminq)) {
        printf("nvme: Error allocating admin queue\n");
        goto out_reset;
    }
    if (!alloc_queue(&ioq)) {
        printf("nvme: Error allocating admin queue\n");
        goto out_adminq;
    }

    ioq.adminq = false;
    adminq.adminq = true;

    nvme_asc = asc_init("/arm-io/ans");
    if (!nvme_asc)
        goto out_ioq;

    if (nvme_type == NVME_T8132 && !nvme_secure_regs) {
        /* ANS that an earlier owner powered off has nothing to read until its power is back. */
        if (nvme_sart_saved_cnt && pcie_init()) {
            printf("nvme: couldn't power ANS back on\n");
            goto out_asc;
        }
        bool running = asc_cpu_running(nvme_asc);
        nvme_log_entry_state(running);
        /*
         * iBoot leaves its session running before the one-time RTKit handshake, with none of the
         * queue setup an aura stage leaves. An older aura stage passed its session on running.
         */
        earlier = running && (nvme_read64_lo_hi(nvmmu_base + NVMMU_ASQ_BASE) ||
                              read32(nvme_qreg(NVME_MAX_PEND_CMDS_CTRL)));
        from_off = !running || earlier;
        if (from_off && !earlier && !nvme_sart_saved_cnt) {
            printf("nvme: ANS is off and no earlier stage passed its SART entries\n");
            goto out_asc;
        }
    }

    nvme_sart = sart_init("/arm-io/sart-ans");
    if (!nvme_sart)
        goto out_asc;

    nvme_rtkit = rtkit_init("nvme", nvme_asc, NULL, NULL, nvme_sart, false);
    if (!nvme_rtkit)
        goto out_sart;

    if (from_off) {
        /* An older stage's running session is shut down first, as that stage should have. */
        if ((earlier && !nvme_ans_power_off()) || !nvme_ans_power_on())
            goto out_preserve;
    } else {
        if (!rtkit_boot(nvme_rtkit))
            goto out_rtkit;
        nvme_boot_ticks = get_ticks();

        if (poll32(nvme_base + NVME_BOOT_STATUS, 0xffffffff, NVME_BOOT_STATUS_OK, USEC_PER_SEC) <
            0) {
            printf("nvme: ANS did not boot correctly.\n");
            goto out_shutdown;
        }
    }

    /* setup controller and NVMMU for linear submission queue */
    if (!nvme_lsq_in_nvmmu)
        set32(nvme_lsq_ctrl(), NVME_LINEAR_SQ_CTRL_EN);
    write32(nvme_qreg(NVME_MAX_PEND_CMDS_CTRL),
            ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1));
    write32(nvmmu_base + NVMMU_NUM, NVME_QUEUE_SIZE - 1);
    write64_lo_hi(nvmmu_base + NVMMU_ASQ_BASE, (u64)adminq.tcbs);
    write64_lo_hi(nvmmu_base + NVMMU_IOSQ_BASE, (u64)ioq.tcbs);

    /* setup admin queue */
    if (!nvme_ctrl_disable()) {
        printf("nvme: timeout while waiting for CSTS.RDY to clear\n");
        goto out_shutdown;
    }
    write64_lo_hi(nvme_qreg(NVME_ASQ), (u64)adminq.cmds);
    write64_lo_hi(nvme_qreg(NVME_ACQ), (u64)adminq.cqes);
    write32(nvme_qreg(NVME_AQA), ((NVME_QUEUE_SIZE - 1) << 16) | (NVME_QUEUE_SIZE - 1));
    if (!nvme_ctrl_enable()) {
        printf("nvme: timeout while waiting for CSTS.RDY to be set\n");
        goto out_disable_ctrl;
    }

    /* setup IO queue */
    struct nvme_command cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CMD_CREATE_CQ;
    cmd.prp1 = (u64)ioq.cqes;
    cmd.cdw10 = 1; // cq id
    cmd.cdw10 |= (NVME_QUEUE_SIZE - 1) << 16;
    cmd.cdw11 = NVME_QUEUE_CONTIGUOUS;
    if (!nvme_exec_command(&adminq, &cmd, NULL)) {
        printf("nvme: create cq command failed\n");
        goto out_disable_ctrl;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CMD_CREATE_SQ;
    cmd.prp1 = (u64)ioq.cmds;
    cmd.cdw10 = 1; // sq id
    cmd.cdw10 |= (NVME_QUEUE_SIZE - 1) << 16;
    cmd.cdw11 = NVME_QUEUE_CONTIGUOUS;
    cmd.cdw11 |= 1 << 16; // cq id for this sq
    if (!nvme_exec_command(&adminq, &cmd, NULL)) {
        printf("nvme: create sq command failed\n");
        goto out_delete_cq;
    }

    if (nvme_type == NVME_T8132) {
        // Extra write required to set up the IO queues on M4+.
        // Otherwise, the ANS crashes and the crashlog says
        // `I/O SQ  : 0x000000000` / `I/O CQ  : 0x000000000`.
        write64_lo_hi(nvme_qreg(NVME_IOQ_CQES), (u64)ioq.cqes);
        write64_lo_hi(nvme_qreg(NVME_IOQ_CMDS), (u64)ioq.cmds);
    }

    nvme_initialized = true;
    printf("nvme: initialized at 0x%lx\n", nvme_base);
    return true;

out_delete_cq:
    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CMD_DELETE_CQ;
    cmd.cdw10 = 1; // cq id
    if (!nvme_exec_command(&adminq, &cmd, NULL))
        printf("nvme: delete cq command failed\n");
out_disable_ctrl:
    nvme_ctrl_shutdown();
    nvme_ctrl_disable();
    nvme_poll_syslog();
out_shutdown:
    if (from_off) {
        /* ANS may still reference the queues if controller shutdown failed. */
        printf("nvme: preserving ANS queues after setup failure; reboot required\n");
        goto out_preserve;
    }
    if (nvme_type != NVME_T8132)
        nvme_ans_sleep_and_reset();
out_rtkit:
    if (nvme_type == NVME_T8132 && nvme_asc && asc_cpu_running(nvme_asc)) {
        printf("nvme: preserving running post-M4 ANS after setup failure; reboot required\n");
        goto out_preserve;
    }
    rtkit_free(nvme_rtkit);
out_sart:
    sart_free(nvme_sart);
out_asc:
    asc_free(nvme_asc);
out_ioq:
    free_queue(&ioq);
out_adminq:
    free_queue(&adminq);
out_reset:
    memset(&adminq, 0, sizeof(adminq));
    memset(&ioq, 0, sizeof(ioq));
    nvme_asc = NULL;
    nvme_rtkit = NULL;
    nvme_sart = NULL;
    nvme_base = 0;
    nvmmu_base = 0;
    nvme_lsq_base = 0;
    nvme_lsq_in_nvmmu = false;
    nvme_die = 0;
    return false;

out_preserve:
    /* Retained DMA resources must not be reused or handed off to another image. */
    nvme_reboot_required = true;
    return false;
}

void nvme_ensure_shutdown(void)
{
    if (nvme_post_m4()) {
        printf("nvme: refusing post-M4 ANS power-down\n");
        return;
    }

    nvme_asc = asc_init("/arm-io/ans");
    if (!nvme_asc)
        return;

    if (!asc_cpu_running(nvme_asc)) {
        printf("nvme: ANS not running\n");
        asc_free(nvme_asc);
        nvme_asc = NULL;
        return;
    }

    printf("nvme: Found ANS left powered, doing a proper shutdown\n");

    nvme_sart = sart_init("/arm-io/sart-ans");
    if (!nvme_sart)
        goto fail;

    nvme_rtkit = rtkit_init("nvme", nvme_asc, NULL, NULL, nvme_sart, false);
    if (!nvme_rtkit)
        goto fail;

    if (!rtkit_boot(nvme_rtkit)) {
        rtkit_free(nvme_rtkit);
        nvme_rtkit = NULL;
    }

fail:
    nvme_ans_sleep_and_reset();
    if (nvme_rtkit) {
        rtkit_free(nvme_rtkit);
        nvme_rtkit = NULL;
    }
    if (nvme_sart) {
        sart_free(nvme_sart);
        nvme_sart = NULL;
    }
    asc_free(nvme_asc);
    nvme_asc = NULL;
}

bool nvme_shutdown(void)
{
    if (nvme_reboot_required) {
        printf("nvme: refusing shutdown after setup failure; reboot required\n");
        return false;
    }
    if (!nvme_initialized) {
        // nvme_ensure_shutdown();
        return true;
    }

    /* A failed queue can still own DMA even when the controller looks ready. */
    if (nvme_type == NVME_T8132 && (adminq.failed || ioq.failed))
        goto out_preserve;

    if (nvme_type == NVME_T8132 && nvme_keep_running_for_linux && !nvme_secure_regs) {
        u32 cc = read32(nvme_base + NVME_CC);
        u32 csts = read32(nvme_base + NVME_CSTS);
        u32 boot_status = read32(nvme_base + NVME_BOOT_STATUS);
        if (!nvme_asc || !asc_cpu_running(nvme_asc) || boot_status != NVME_BOOT_STATUS_OK ||
            !(cc & NVME_CC_EN) || !(csts & NVME_CSTS_RDY) || (csts & NVME_CSTS_CFS)) {
            printf("nvme: refusing Linux handoff: CC=0x%x CSTS=0x%x BOOT_STATUS=0x%x\n", cc, csts,
                   boot_status);
            return false;
        }
        nvme_initialized = false;
        printf("nvme: leaving controller and RTKit running for Linux adoption\n");
        return true;
    }

    struct nvme_command cmd;
    u64 t_start = get_ticks();

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CMD_DELETE_SQ;
    cmd.cdw10 = 1; // sq id
    if (!nvme_exec_command(&adminq, &cmd, NULL)) {
        printf("nvme: delete sq command failed\n");
        if (nvme_type == NVME_T8132)
            goto out_preserve;
    }

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_ADMIN_CMD_DELETE_CQ;
    cmd.cdw10 = 1; // cq id
    if (!nvme_exec_command(&adminq, &cmd, NULL)) {
        printf("nvme: delete cq command failed\n");
        if (nvme_type == NVME_T8132)
            goto out_preserve;
    }

    if (nvme_type == NVME_T8132 && !nvme_secure_regs) {
        write64_lo_hi(nvme_qreg(NVME_IOQ_CMDS), 0);
        write64_lo_hi(nvme_qreg(NVME_IOQ_CQES), 0);
        if (!nvme_ans_power_off())
            goto out_preserve;
        /* The firmware keeps the RTKit buffers through the power loss: they stay allocated. */
        free_queue(&ioq);
        free_queue(&adminq);
        nvme_initialized = false;
        return true;
    }

    u64 t_queues = get_ticks();
    /*
     * The M1-M3 firmware finishes its GC bitmap rebuild and saves its context before it stops,
     * for a normal shutdown or, without one, inside the RTKit sleep below. Skipping the shutdown
     * saves nothing there; it only moves that work (0.15 to 0.55 s, varying by boot) under the
     * sleep handshake. T8152 keeps its session running, so only a stage that wrote pays there.
     */
    bool normal_shutdown = nvme_wrote || nvme_type != NVME_T8132;
    if (normal_shutdown && !nvme_ctrl_shutdown()) {
        printf("nvme: timeout while waiting for controller shutdown\n");
        if (nvme_type == NVME_T8132)
            goto out_preserve;
    }
    if (!nvme_ctrl_disable()) {
        printf("nvme: timeout while waiting for CSTS.RDY to clear\n");
        if (nvme_type == NVME_T8132)
            goto out_preserve;
    }

    if (nvme_type == NVME_T8132) {
        /* T8152: Linux wakes ANS with a fresh RTKit handshake and neither adopts a live session
         * nor restarts a stopped ANS. Quiesce RTKit and leave the ASC running, as iBoot does. */
        write64_lo_hi(nvme_qreg(NVME_IOQ_CMDS), 0);
        write64_lo_hi(nvme_qreg(NVME_IOQ_CQES), 0);
        if (!rtkit_quiesce(nvme_rtkit))
            goto out_preserve;
        nvme_initialized = false;
        printf("nvme: controller disabled, RTKit quiesced\n");
        return true;
    }

    u64 t_disabled = get_ticks();
    bool asleep = nvme_ans_sleep_and_reset();
    u64 t_asleep = get_ticks();
    /*
     * The sleep handshake waits for the ANS firmware to finish its own work first. Name the
     * phases, so a slow handoff shows where it spent its time and how long the firmware had run.
     */
    printf("nvme: shutdown %lu ms: queues %lu ms, %s %lu ms, %s %lu ms; ANS up %lu ms\n",
           ticks_to_msecs(t_asleep - t_start), ticks_to_msecs(t_queues - t_start),
           normal_shutdown ? "shutdown+disable" : "disable", ticks_to_msecs(t_disabled - t_queues),
           asleep ? "IOP sleep" : "IOP sleep FAILED", ticks_to_msecs(t_asleep - t_disabled),
           nvme_boot_ticks ? ticks_to_msecs(t_asleep - nvme_boot_ticks) : 0);
    rtkit_free(nvme_rtkit);
    sart_free(nvme_sart);
    asc_free(nvme_asc);
    free_queue(&ioq);
    free_queue(&adminq);
    nvme_initialized = false;

    printf("nvme: shutdown done\n");
    return true;

out_preserve:
    /* Leave every queue allocation and the live RTKit session owned. */
    nvme_reboot_required = true;
    printf("nvme: unsafe post-M4 queue handoff; retaining DMA resources until reboot\n");
    return false;
}

/*
 * ANS's SART entries for the next stage, and whether ANS will be off when it starts: this
 * stage's own session unless Linux adopts it, else what an earlier stage powered off with, else
 * a running session an older stage passed on, which the next stage receives as it is.
 */
int nvme_sart_entries(struct sart_entry *entries, int max, bool *off)
{
    *off = false;
    if (nvme_initialized) {
        if (nvme_type != NVME_T8132)
            return 0;
        *off = !nvme_secure_regs && !nvme_keep_running_for_linux;
        return sart_get_entries(nvme_sart, entries, max);
    }
    if (nvme_sart_saved_cnt) {
        *off = true;
        memcpy(entries, nvme_sart_saved, min(max, nvme_sart_saved_cnt) * sizeof(*entries));
        return min(max, nvme_sart_saved_cnt);
    }
    if (!nvme_post_m4())
        return 0;
    sart_dev_t *sart = sart_init("/arm-io/sart-ans");
    int count = sart ? sart_get_entries(sart, entries, max) : 0;
    if (sart)
        sart_release(sart);
    return count;
}

/* "nvme.sart=" and, per entry, (index << 8 | flags), address and size as 16 hex digits each */
char *nvme_sart_var(void)
{
    static char var[sizeof("nvme.sart=") + 16 * 48];
    struct sart_entry e[16];
    bool off;
    int n = nvme_sart_entries(e, ARRAY_SIZE(e), &off);
    char *p = var + snprintf(var, sizeof(var), "nvme.sart=");

    if (!off || !n)
        return NULL;
    for (int i = 0; i < n; i++)
        p += snprintf(p, var + sizeof(var) - p, "%016lx%016lx%016lx",
                      (u64)e[i].index << 8 | e[i].flags, e[i].paddr, e[i].size);
    return var;
}

bool nvme_set_sart_var(const char *val)
{
    size_t len = strlen(val);
    u64 word[3];

    nvme_sart_saved_cnt = 0;
    if (len % 48 || len / 48 > ARRAY_SIZE(nvme_sart_saved)) {
        printf("nvme: malformed nvme.sart\n");
        return false;
    }
    for (size_t i = 0; i < len / 16; i++) {
        word[i % 3] = 0;
        for (int d = 0; d < 16; d++) {
            char c = val[16 * i + d];
            word[i % 3] = word[i % 3] << 4 | (c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        if (i % 3 == 2)
            nvme_sart_saved[nvme_sart_saved_cnt++] = (struct sart_entry){
                .index = word[0] >> 8, .flags = word[0], .paddr = word[1], .size = word[2]};
    }
    return true;
}

bool nvme_flush(u32 nsid)
{
    struct nvme_command cmd;

    if (nvme_reboot_required || !nvme_initialized)
        return false;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_CMD_FLUSH;
    cmd.nsid = nsid;
    nvme_wrote = true;

    return nvme_exec_command(&ioq, &cmd, NULL);
}

static bool nvme_submit_read(u32 nsid, u64 lba, void *buffer, u32 count, u64 prp2)
{
    struct nvme_command cmd;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_CMD_READ;
    cmd.nsid = nsid;
    cmd.prp1 = (u64)buffer;
    cmd.prp2 = prp2;
    cmd.cdw10 = lba;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = count - 1;

    return nvme_exec_command(&ioq, &cmd, NULL);
}

bool nvme_read_blocks(u32 nsid, u64 lba, void *buffer, u32 count)
{
    if (nvme_reboot_required || !nvme_initialized || ioq.failed || !count ||
        count > NVME_MAX_READ_BLOCKS || ((u64)buffer & (SZ_4K - 1)) ||
        lba > UINT64_MAX - (count - 1))
        return false;

    if (count > 1 && nvme_multi_enabled) {
        if (!nvme_verify_bounce)
            nvme_verify_bounce = memalign(SZ_16K, SZ_4K);
        if (count > 2 && !nvme_prp_list)
            nvme_prp_list = memalign(SZ_16K, SZ_4K);
        if (nvme_verify_bounce && (count <= 2 || nvme_prp_list)) {
            u64 prp2 = (u64)buffer + SZ_4K;
            if (count > 2) {
                for (u32 i = 1; i < count; i++)
                    nvme_prp_list[i - 1] = (u64)buffer + (u64)i * SZ_4K;
                prp2 = (u64)nvme_prp_list;
            }
            if (!nvme_multi_verified)
                memset(buffer, 0xa5, (size_t)count * SZ_4K);
            if (nvme_submit_read(nsid, lba, buffer, count, prp2)) {
                if (nvme_multi_verified)
                    return true;

                u8 *bytes = buffer;
                bool verified = true;
                for (u32 i = 0; i < 2; i++) {
                    u32 block = i ? count - 1 : 0;
                    memset(nvme_verify_bounce, 0x5a, SZ_4K);
                    if (!nvme_submit_read(nsid, lba + block, nvme_verify_bounce, 1, 0) ||
                        memcmp(nvme_verify_bounce, bytes + (size_t)block * SZ_4K, SZ_4K)) {
                        verified = false;
                        break;
                    }
                }
                if (verified) {
                    nvme_multi_verified = true;
                    printf("nvme: %u-block reads verified\n", count);
                    return true;
                }
            }
            if (nvme_cmd_timed_out)
                return false;
            printf("nvme: multi-block read failed verification; using single blocks\n");
            nvme_multi_enabled = false;
        }
    }

    u8 *bytes = buffer;
    for (u32 i = 0; i < count; i++) {
        if (!nvme_submit_read(nsid, lba + i, bytes + (size_t)i * SZ_4K, 1, 0))
            return false;
    }
    return true;
}

bool nvme_read(u32 nsid, u64 lba, void *buffer)
{
    return nvme_read_blocks(nsid, lba, buffer, 1);
}

bool nvme_write(u32 nsid, u64 lba, const void *buffer)
{
    struct nvme_command cmd;

    if (nvme_reboot_required || !nvme_initialized || ioq.failed || ((u64)buffer & (SZ_4K - 1)))
        return false;

    memset(&cmd, 0, sizeof(cmd));
    cmd.opcode = NVME_CMD_WRITE;
    cmd.nsid = nsid;
    cmd.prp1 = (u64)buffer;
    cmd.cdw10 = lba;
    cmd.cdw11 = lba >> 32;
    cmd.cdw12 = 0;
    /* Shut the controller down normally so it writes back its cache. */
    nvme_wrote = true;

    return nvme_exec_command(&ioq, &cmd, NULL);
}
