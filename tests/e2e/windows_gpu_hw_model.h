/* SPDX-License-Identifier: MIT */
/*
 * Host model of the hardware behind hv_gpu.c's G1 code (HV_GPU_HOST_TEST): a synthetic PSD
 * recipe and three PSD blocks that behave as the recipe describes (request, acknowledge after a
 * few polls, a state read clears the acknowledge, so the recipe reads the acknowledge first), and the registers the G1 hooks may forward
 * (synthetic ID words, CPU_CONTROL / CPU_STATUS, the two post-office windows with an RX queue).
 * Every hardware access is recorded so the tests can prove that nothing outside the forwarded set
 * is ever touched.  All numbers are synthetic and deliberately unlike any real machine's.
 */
#ifndef WINDOWS_GPU_HW_MODEL_H
#define WINDOWS_GPU_HW_MODEL_H

#include <assert.h>
#include <string.h>

#include "gpu_t8152_g2.h"
#include "gpu_t8152_power.h"

/* Keep in step with PSD_BASE / PSD_NODE in tests/python/test_windows_t8152_gpu.py. */
#define MODEL_PSD_BASE 0x307000000UL

static struct gpu_psd_recipe test_recipe = {
    .available = true,
    .source = "synthetic test recipe",
    .base = MODEL_PSD_BASE,
    .stride = 0x80,
    .block_size = 0x80,
    .state_off = 0x40,
    .state_mask = 0xf,
    .request_off = 0x50,
    .ack_off = 0x60,
    .ack_bit = 0x2,
    .read_order = GPU_PSD_READ_ACK_FIRST,
    .cold_idle = 7,
    .poll_us = 10,
    .polls = 50,
    .nsteps = 3,
    .step = {{5, 1, 1, "core"}, {45, 3, 3, "asc-b"}, {44, 3, 3, "asc-a"}},
};

const struct gpu_psd_recipe *gpu_t8152_psd_recipe(void)
{
    return &test_recipe;
}

/*
 * G2: a synthetic G2 recipe.  The boot-control register sits in the synthetic pmgr-child hole of
 * the sgx window; the windows are anchored in the synthetic machine's aic, sgx and aon-x nodes
 * (tests/python/test_windows_t8152_gpu.py); the refused type and every value are synthetic.
 */
#define MODEL_BOOT_OFF    0xe94000u
#define MODEL_BOOT_COLD   0x700u
#define MODEL_BOOT_EXPECT 0x701u
#define MODEL_REFUSED_EP  0x21u
#define MODEL_REFUSED_T   0x7eu

static struct gpu_g2_recipe test_g2 = {
    .available = true,
    .source = "synthetic G2 recipe",
    .boot_offset = MODEL_BOOT_OFF,
    .boot_accept = {MODEL_BOOT_COLD, MODEL_BOOT_EXPECT},
    .boot_set = 0x1,
    .boot_expect = MODEL_BOOT_EXPECT,
    .nwindows = 5,
    .window = {
        {2, "aic", 0, 0, 0x8000, 0x4000, true},
        {5, "sgx", 0, 0, 0x100000, 0x8000, true},
        {6, "sgx", 0, 0, 0x1800, 0x27f8, false},  /* unaligned, inside the first sgx page */
        {7, "sgx", 0, 0, 0xe82200, 0xfc, false},  /* inside a pmgr-child hole */
        {8, "aon-x", 0, 1, 0x0, 0x14000, true},   /* the hull of two regs, gap included */
    },
    .nrefused = 1,
    .refused = {{MODEL_REFUSED_EP, MODEL_REFUSED_T}},
    .nroots = 1,
    .root = {{GPU_R_PAGETABLES, 0x40000}},
};

const struct gpu_g2_recipe *gpu_t8152_g2_recipe(void)
{
    return &test_g2;
}

/*
 * G2: the memory behind the GPU page tables, as the carrier's audit reads it: 16 KiB pages made
 * on first use (zero), except one address that cannot be read.
 */
static struct {
    u64 pa;
    u64 *page;
} uat_mem[256];
static int nuat_mem;
static u64 uat_unreadable;

const u64 *hv_gpu_host_uat_page(u64 pa)
{
    assert(!(pa & 0x3fff) && "UAT page read not 16 KiB aligned");
    if (pa == uat_unreadable)
        return NULL;
    for (int i = 0; i < nuat_mem; i++)
        if (uat_mem[i].pa == pa)
            return uat_mem[i].page;
    assert(nuat_mem < 256);
    uat_mem[nuat_mem].pa = pa;
    uat_mem[nuat_mem].page = calloc(2048, sizeof(u64));
    return uat_mem[nuat_mem++].page;
}

/* Write entry idx of the table page at pa (what the guest or the firmware would do). */
static inline void uat_put(u64 pa, u32 idx, u64 desc)
{
    ((u64 *)hv_gpu_host_uat_page(pa))[idx] = desc;
}

static inline void uat_mem_reset(void)
{
    for (int i = 0; i < nuat_mem; i++)
        free(uat_mem[i].page);
    nuat_mem = 0;
    uat_unreadable = 0;
}

/* ---- PSD blocks ---------------------------------------------------------------------------- */
static struct psd_block {
    u32 state, ack, request, countdown;
    bool pending, never_ack;
    int ack_state; /* >= 0: the state the block reaches instead of the request */
} psdm[3];
static int psd_reads, psd_writes;
static u32 psd_rlog[64]; /* the register of each PSD read, in order (first 64) */
static struct {
    u32 service, value;
} psd_wlog[16];
static u64 psd_delay_us;

static int psd_index(u64 addr, u32 *reg)
{
    for (int i = 0; i < 3; i++) {
        u64 b = test_recipe.base + (u64)test_recipe.step[i].service * test_recipe.stride;
        if (addr >= b && addr < b + test_recipe.block_size) {
            *reg = (u32)(addr - b);
            return i;
        }
    }
    return -1;
}

static inline void psd_model_reset(void)
{
    memset(psdm, 0, sizeof(psdm));
    for (int i = 0; i < 3; i++) {
        psdm[i].state = test_recipe.cold_idle;
        psdm[i].ack_state = -1;
    }
    psd_reads = psd_writes = 0;
    psd_delay_us = 0;
}

static bool psd_all_on(void)
{
    for (int i = 0; i < 3; i++)
        if (psdm[i].state != test_recipe.step[i].target)
            return false;
    return true;
}

u32 hv_gpu_host_read32(u64 addr)
{
    u32 reg;
    int i = psd_index(addr, &reg);
    assert(i >= 0 && "PSD read outside the recipe's blocks");
    if (psd_reads < 64)
        psd_rlog[psd_reads] = reg;
    psd_reads++;
    struct psd_block *b = &psdm[i];
    if (reg == test_recipe.ack_off) {
        if (b->pending && !b->never_ack && b->countdown && !--b->countdown) {
            b->pending = false;
            b->ack = test_recipe.ack_bit;
            b->state = b->ack_state >= 0 ? (u32)b->ack_state : b->request;
        }
        return b->ack;
    }
    assert(reg == test_recipe.state_off && "PSD read of an unknown register");
    b->ack = 0; /* this synthetic block clears the acknowledge on a state read */
    return b->state | 0xf0; /* bits outside the mask must be ignored */
}

void hv_gpu_host_write32(u64 addr, u32 value)
{
    u32 reg;
    int i = psd_index(addr, &reg);
    assert(i >= 0 && reg == test_recipe.request_off && "PSD write that is not a request");
    if (psd_writes < 16) {
        psd_wlog[psd_writes].service = test_recipe.step[i].service;
        psd_wlog[psd_writes].value = value;
    }
    psd_writes++;
    psdm[i].request = value;
    psdm[i].pending = true;
    psdm[i].countdown = 3;
}

void hv_gpu_host_delay(u32 us)
{
    psd_delay_us += us;
}

/* ---- the forwarded registers --------------------------------------------------------------- */
static u64 model_sgx, model_wrap[2], model_mbox;
static const u32 model_id_off[7] = {0xd04000, 0xd04008, 0xd04010, 0xd04014,
                                    0xd04018, 0xd0401c, 0xe01500};
static const u32 model_id[7] = {0x5100, 0x5108, 0x5110, 0x5114, 0x5118, 0x511c, 0x77};
static u32 model_cpu_control[2] = {0x10, 0x10}, model_cpu_status[2] = {0x9, 0xa};
static u64 rx_queue[8][2];
static int rx_head, rx_tail, rx_role;
static u64 tx_log[8][2];
static int tx_count;
static u64 fault_addr;

static struct hw_access {
    u64 addr;
    int width;
    bool write;
    u64 value;
} hwlog[512];
static int hw_count, hw_unexpected, cpu_control_writes;
/* G2: the boot-control register (MODEL_BOOT_OFF from the sgx base). */
static u32 model_boot, model_boot_readback = 0;
static int boot_reads, boot_writes;

static inline void hw_model_reset(void)
{
    rx_head = rx_tail = tx_count = 0;
    hw_count = hw_unexpected = cpu_control_writes = 0;
    fault_addr = 0;
    model_cpu_control[0] = model_cpu_control[1] = 0x10;
    model_boot = MODEL_BOOT_COLD;
    model_boot_readback = 0;
    boot_reads = boot_writes = 0;
}

static inline void rx_push(int role, u64 msg, u64 ep)
{
    rx_role = role;
    rx_queue[rx_tail % 8][0] = msg;
    rx_queue[rx_tail % 8][1] = ep;
    rx_tail++;
}

bool hv_gpu_host_mmio(u64 addr, u64 *val, bool write, int width)
{
    if (hw_count < 512)
        hwlog[hw_count] = (struct hw_access){addr, width, write, write ? val[0] : 0};
    hw_count++;
    if (addr == fault_addr)
        return false;
    if (addr == model_sgx + MODEL_BOOT_OFF && width == 2) {
        assert(psd_all_on() && "boot-control access while the GPU is not powered");
        if (write) {
            boot_writes++;
            model_boot = (u32)val[0];
        } else {
            boot_reads++;
            val[0] = model_boot_readback ? model_boot_readback : model_boot;
        }
        return true;
    }
    for (int i = 0; i < 7; i++)
        if (addr == model_sgx + model_id_off[i] && !write && width == 2) {
            assert(psd_all_on() && "sgx read while the GPU is not powered");
            val[0] = model_id[i];
            return true;
        }
    for (int r = 0; r < 2; r++) {
        if (addr == model_wrap[r] + 0x44 && width == 2) {
            assert(psd_all_on());
            if (write) {
                cpu_control_writes++;
                model_cpu_control[r] = (u32)val[0];
            } else {
                val[0] = model_cpu_control[r];
            }
            return true;
        }
        if (addr == model_wrap[r] + 0x48 && width == 2 && !write) {
            val[0] = model_cpu_status[r];
            return true;
        }
        u64 w = model_mbox + 0x100 * (u64)(r + 1);
        if (addr == w + 0x20 && width == 2 && !write) {
            val[0] = r == rx_role ? (u32)(rx_tail - rx_head) : 0;
            return true;
        }
        if (addr == w && width == 4 && !write) {
            if (r == rx_role && rx_head < rx_tail) {
                val[0] = rx_queue[rx_head % 8][0];
                val[1] = rx_queue[rx_head % 8][1];
                rx_head++;
            } else {
                val[0] = val[1] = 0;
            }
            return true;
        }
        if ((addr == w || addr == w + 8) && width == 3 && !write) {
            val[0] = r == rx_role && rx_head < rx_tail ? rx_queue[rx_head % 8][addr == w ? 0 : 1] :
                                                         0;
            if (addr == w + 8 && r == rx_role && rx_head < rx_tail)
                rx_head++;
            return true;
        }
        if (addr == w + 0x10 && width == 4 && write) {
            tx_log[tx_count % 8][0] = val[0];
            tx_log[tx_count % 8][1] = val[1];
            tx_count++;
            return true;
        }
        if ((addr == w + 0x10 || addr == w + 0x18) && width == 3 && write)
            return true;
    }
    hw_unexpected++;
    return true;
}

#endif
