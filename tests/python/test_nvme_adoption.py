# SPDX-License-Identifier: GPL-2.0-only
"""Exercise queue lifetime on failures in the production ANS adoption path."""

from pathlib import Path
import os
import subprocess

from host_compiler import host_cc


def test_adopted_queue_lifetime(tmp_path):
    source = Path(os.environ.get("NVME_SOURCE", Path(__file__).resolve().parents[2] / "src/nvme.c")).read_text()
    declarations = source[source.index("#define NVME_TIMEOUT"):source.index("static u64 nvme_read64_lo_hi")]
    # nvme_init resets this data-path state, but the read path is outside this lifetime fixture.
    for declaration in ("static bool nvme_multi_enabled", "static bool nvme_multi_verified",
                        "static u64 *nvme_prp_list", "static u8 *nvme_verify_bounce"):
        assert declarations.count(declaration) == 1
        declarations = declarations.replace(declaration, declaration.replace(
            "static ", "static __attribute__((unused)) ", 1))
    # The queue register helpers, which place the registers for the secure layout too.
    qregs = source[source.index("static u64 nvme_qreg(u32 reg)"):source.index("static void nvme_log_entry_state")]
    # The post-M4 power off and power on around a stage's use of ANS.
    power = source[source.index("/* The ADT describes a post-M4 ANS"):source.index("/* A write or flush, timed:")]
    init = source[source.index("bool nvme_init(void)"):source.index("void nvme_ensure_shutdown(void)")]
    # The M1-M3 sleep-and-reset helper that every legacy power-down goes through.
    sleep_reset = source[source.index("static bool nvme_ans_sleep_and_reset(void)"):
                         source.index("static bool nvme_exec_command(")]
    shutdown = source[source.index("bool nvme_shutdown(void)"):
                      source.index("int nvme_sart_entries(")]
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int asc_dev_t;
typedef int rtkit_dev_t;
typedef int sart_dev_t;
struct sart_entry { u8 index, flags; u64 paddr, size; };
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define static_assert _Static_assert
#define BIT(n) (1ULL << (n))
#define GENMASK(h, l) ((BIT((h) + 1) - 1) & ~(BIT(l) - 1))
#define FIELD_GET(mask, value) (((value) & (mask)) >> __builtin_ctzll(mask))
#define PMGR_DIE_ID GENMASK(31, 28)
#define USEC_PER_SEC 1000000
#define ADT_GETPROP(...) (-1)
#define T8140 0x8140
#define T8132 0x8132
#define T6040 0x6040
#define T6041 0x6041
#define T6050 0x6050
#define T8142 0x8142
static u32 chip_id = T8140;
static void *adt;
static u64 get_ticks(void) { return 0; }
static u64 ticks_to_msecs(u64 ticks) { return ticks; }
''' + declarations + r'''
static int failure, queue_frees, asc_frees, publications, commands;
static int shutdown_fault, disable_calls, shutdown_calls, cleared_ioq;
static void *allocations[12];
static unsigned allocated;
static int asc, rtkit, sart, resets, boots;
static bool started;
static bool alloc_queue(struct nvme_queue *q)
{
    q->tcbs = allocations[allocated++] = calloc(1, sizeof(*q->tcbs));
    q->cmds = allocations[allocated++] = calloc(1, sizeof(*q->cmds));
    q->cqes = allocations[allocated++] = calloc(1, sizeof(*q->cqes));
    return true;
}
static void free_queue(struct nvme_queue *q) { queue_frees++; }
static int adt_path_offset_trace(void *a, const char *p, int *trace) { return 1; }
static const void *adt_getprop(void *a, int n, const char *p, u32 *len)
{
    *len = 0;
    return failure == 7 ? NULL : &asc;
}
static bool adt_is_compatible(void *a, int n, const char *p) { return true; }
/* No secure register layout: the T8152 path is not under test here. */
static const void *adt_get_property(void *a, int n, const char *p) { return NULL; }
static int adt_get_reg(void *a, int *path, const char *p, int i, u64 *base, u64 *size)
{
    *base = i == 3 ? 0x10000000 : 0x20000000;
    *size = failure == 6 && i == 9 ? NVME_BOOT_STATUS + 4 : 0x100000;
    if (failure >= 10) {
        *base = i == 3 ? 0x38dcc0000 : 0x3cdcc0000;
        *size = i == 3 ? 0x60000 : 0x10000;
        if (failure == 12 && i == 9)
            *base += 0x10000;
        if (failure == 13 && i == 9)
            *size = 0x8000;
        if (failure == 14 && i == 3)
            *size = NVMMU_TCB_STAT;
        /* 23-27: the J713 (T8132) split BAR. */
        if (failure >= 23) {
            *base = i == 3 ? 0x485cc0000 : 0x4c5cc0000;
            if (failure == 24 && i == 9)
                *base += 0x10000;
            if (failure == 25 && i == 9)
                *size = 0x8000;
        }
    }
    return 0;
}
static asc_dev_t *asc_init(const char *path) { return &asc; }
static bool asc_cpu_running(asc_dev_t *a) { return started || (failure != 1 && failure != 27); }
static void asc_free(asc_dev_t *a) { asc_frees++; }
static u32 read32(u64 addr)
{
    if (addr == nvme_base + NVME_BOOT_STATUS)
        return NVME_BOOT_STATUS_OK;
    if (addr == nvme_base + NVME_CC)
        return NVME_CC_EN;
    return addr == nvme_base + NVME_CSTS ? NVME_CSTS_RDY : 0;
}
static void write32(u64 addr, u32 value) {}
static void set32(u64 addr, u32 value) {}
static void write64_lo_hi(u64 addr, u64 value)
{
    publications++;
    if (!value && (addr == nvme_base + NVME_IOQ_CMDS || addr == nvme_base + NVME_IOQ_CQES))
        cleared_ioq++;
}
static void nvme_log_entry_state(bool running) {}
/* No earlier aura stage's queue setup: a running session is iBoot's. */
static u64 nvme_read64_lo_hi(u64 addr) { return 0; }
static int adt_path_offset(void *a, const char *p) { return 1; }
static int pcie_init(void) { return 0; }
/* Non-T6050 power-on delegates to pcie_init() in production too. */
static int pcie_ans_power_on(void) { assert(chip_id != T6050); return pcie_init(); }
/* Shutdown faults 3 and 4: ANS doesn't power off. */
static int pcie_ans_power_off(void) { return shutdown_fault == 3 || shutdown_fault == 4 ? -1 : 0; }
static bool rtkit_idle(rtkit_dev_t *r) { return true; }
static bool rtkit_boot_warm(rtkit_dev_t *r, u32 mask) { return true; }
static int sart_get_entries(sart_dev_t *s, struct sart_entry *e, int max) { return 1; }
static bool sart_set_entries(sart_dev_t *s, const struct sart_entry *e, int n) { return true; }
static sart_dev_t *sart_init(const char *path) { return &sart; }
static void sart_free(sart_dev_t *s) {}
static rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *a, void *d, void *i,
                              sart_dev_t *s, bool sram) { return &rtkit; }
static bool rtkit_boot(rtkit_dev_t *r) { boots++; started = failure != 8; return failure != 8; }
/* M1-M3 power-down order, one letter per step: S sleep acked, F sleep failed, X stop, R reset. */
static char events[64];
static bool sleep_fails;
static void event(char c) { size_t n = strlen(events); if (n + 1 < sizeof(events)) events[n] = c; }
static bool rtkit_sleep(rtkit_dev_t *r) { event(sleep_fails ? 'F' : 'S'); return !sleep_fails; }
static void rtkit_stop(rtkit_dev_t *r) { event('X'); }
static void asc_cpu_stop(asc_dev_t *a) { event('X'); }
static bool rtkit_quiesce(rtkit_dev_t *r) { return true; }
static void rtkit_free(rtkit_dev_t *r) {}
static void pmgr_reset(u8 die, const char *name) { resets++; event('R'); }
static int poll32(u64 a, u32 mask, u32 value, unsigned timeout) { return 0; }
static bool nvme_ctrl_disable(void)
{ disable_calls++; return failure != 2 && !(shutdown_fault == 4 && disable_calls == 2); }
static bool nvme_ctrl_enable(void) { return failure != 3; }
static bool nvme_ctrl_shutdown(void) { shutdown_calls++; return shutdown_fault != 3; }
/* The T6050 controller suspend protocol is outside this queue-lifetime model. */
static bool nvme_ctrl_suspend(void) { abort(); }
static void nvme_poll_syslog(void) {}
static bool nvme_exec_command(struct nvme_queue *q, struct nvme_command *cmd, void *result)
{
    commands++;
    if ((shutdown_fault == 1 && commands == 3) || (shutdown_fault == 2 && commands == 4)) {
        q->failed = true;
        return false;
    }
    return !((failure == 4 && commands == 1) || (failure == 5 && commands == 2));
}
''' + qregs + sleep_reset + power + init + shutdown + r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    failure = atoi(argv[1]);
    if (failure == 28 || failure == 29) {
        /*
         * M1-M3 handoff from a stage that only read. The firmware does its shutdown work
         * either way, so the normal shutdown still runs. A sleep that is never acknowledged
         * must stop the CPU before the reset, never reset under a running ANS.
         */
        sleep_fails = failure == 29;
        failure = 7;
        assert(nvme_init());
        assert(!nvme_wrote);
        assert(nvme_shutdown());
        assert(shutdown_calls == 1 && disable_calls == 2);
        assert(!strcmp(events, sleep_fails ? "FXRR" : "SRR"));
        assert(!nvme_initialized && !nvme_reboot_required && queue_frees == 2);
        /* The next owner gets a stopped, reset ANS and can boot it again. */
        sleep_fails = false;
        memset(events, 0, sizeof(events));
        assert(nvme_init());
        assert(nvme_shutdown());
        assert(!strcmp(events, "SRR"));
        for (unsigned i = 0; i < allocated; i++)
            free(allocations[i]);
        return 0;
    }
    if (failure >= 15 && failure <= 22) {
        shutdown_fault = failure - 14;
        failure = 0;
        assert(nvme_init());
        if (shutdown_fault == 5 || shutdown_fault == 7)
            adminq.failed = true;
        if (shutdown_fault == 6 || shutdown_fault == 8)
            ioq.failed = true;
        nvme_keep_running_for_linux = shutdown_fault >= 7;
        int saved_publications = publications;
        assert(!nvme_shutdown());
        assert(nvme_reboot_required);
        assert(nvme_initialized);
        assert(!queue_frees && !asc_frees && !resets);
        /* Faults 3 and 4 fail the power off, after the I/O queue registers were cleared. */
        int cleared = shutdown_fault == 3 || shutdown_fault == 4 ? 2 : 0;
        assert(cleared_ioq == cleared && publications == saved_publications + cleared);
        assert(commands == (shutdown_fault == 1 ? 3 : shutdown_fault <= 4 ? 4 : 2));
        /* A stage that only read disables the controller before the power off, with no shutdown. */
        assert(shutdown_calls == 0);
        assert(disable_calls == 1 + (shutdown_fault == 3 || shutdown_fault == 4));
        int saved_commands = commands, saved_disable = disable_calls;
        shutdown_fault = 0;
        adminq.failed = ioq.failed = false;
        nvme_keep_running_for_linux = !nvme_keep_running_for_linux;
        assert(!nvme_init() && !nvme_shutdown());
        assert(commands == saved_commands && disable_calls == saved_disable);
        assert(!queue_frees && !asc_frees && !resets);
        for (unsigned i = 0; i < allocated; i++)
            free(allocations[i]);
        return 0;
    }
    if (failure == 11 || failure >= 23)
        chip_id = T8132;
    nvme_keep_running_for_linux = failure == 9;
    assert(nvme_shutdown());
    assert(nvme_init() == (failure == 0 || failure == 7 || failure == 9 || failure == 10 ||
                           failure == 23 || failure == 26));
    /* A running session from iBoot gets the handshake; ANS off with no entries is refused. */
    if (failure == 26)
        assert(boots == 1);
    if (failure == 27)
        assert(boots == 0);
    if (failure == 1 || failure == 6 || (failure >= 11 && failure <= 14) || failure == 24 ||
        failure == 25 || failure == 27) {
        /* Rejection before publication does not poison a later attempt. */
        assert(publications == 0);
        assert(queue_frees == (failure == 1 || failure == 27 ? 2 : 0));
        assert(nvme_shutdown());
        failure = 0;
        chip_id = T8140;
        assert(nvme_init());
        assert(nvme_shutdown());
    } else if ((failure >= 2 && failure <= 5) || failure == 8) {
        assert(queue_frees == 0 && asc_frees == 0 && resets == 0);
        assert(!nvme_initialized);
        unsigned saved_allocated = allocated;
        int saved_publications = publications, saved_commands = commands;
        /* Clearing the mock fault or changing handoff policy cannot recover DMA. */
        failure = 0;
        nvme_keep_running_for_linux = true;
        assert(!nvme_shutdown());
        assert(!nvme_init());
        assert(!nvme_shutdown());
        assert(allocated == saved_allocated && publications == saved_publications);
        assert(commands == saved_commands && queue_frees == 0 && asc_frees == 0);
        assert(adminq.tcbs == allocations[0] && ioq.tcbs == allocations[3]);
        assert(nvme_asc && nvme_base && nvmmu_base);
    } else {
        bool legacy = failure == 7;
        assert(nvme_initialized);
        assert(nvme_shutdown());
        assert(!nvme_initialized);
        /* Powered off, post-M4 frees its queues too; only Linux's live handoff keeps them. */
        assert(queue_frees == (failure == 9 ? 0 : 2));
        assert(resets == (legacy ? 2 : 0));
        /* A healthy shutdown still allows reinitialization. */
        assert(nvme_init());
        assert(nvme_shutdown());
    }
    /* The host mock owns these allocations; no real DMA is running. */
    for (unsigned i = 0; i < allocated; i++)
        free(allocations[i]);
    return 0;
}
'''
    binary = tmp_path / "nvme-adoption"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-variable", "-Wno-format", "-x", "c", "-", "-o", str(binary)],
        input=harness, text=True, check=True,
    )
    for failure in range(30):
        subprocess.run([str(binary), str(failure)], check=True)
