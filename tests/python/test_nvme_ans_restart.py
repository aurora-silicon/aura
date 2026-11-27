# SPDX-License-Identifier: GPL-2.0-only
"""The post-M4 ANS start from off: one start chosen from the state read at power-on (cold after
a PMGR reset on an ANS type that takes one), and a running session handed over for adoption
when a stage cannot use it."""

from pathlib import Path
import os
import subprocess

from host_compiler import host_cc


def test_ans_start_read_once(tmp_path):
    source = Path(os.environ.get("NVME_SOURCE", Path(__file__).resolve().parents[2] / "src/nvme.c")).read_text()
    declarations = source[source.index("#define NVME_TIMEOUT"):source.index("static u64 nvme_read64_lo_hi")]
    qregs = source[source.index("static u64 nvme_qreg(u32 reg)"):source.index("static void nvme_log_entry_state")]
    power = source[source.index("/* The ADT describes a post-M4 ANS"):source.index("/* A write or flush, timed:")]
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
#define FIELD_PREP(mask, value) (((u64)(value) << __builtin_ctzll(mask)) & (mask))
#define USEC_PER_SEC 1000000
#define T8140 0x8140
#define T8132 0x8132
#define T6040 0x6040
#define T6041 0x6041
#define T6050 0x6050
#define T8142 0x8142
static u32 chip_id = T8132;
static void *adt;
static u64 get_ticks(void) { return 0; }
''' + declarations + r'''
static int asc, rtkit, sart;
static char events[128];
static u32 hello[8];
static int hellos;
/* Which tries the firmware starts on, one bit per try; the hand-over state. */
static unsigned starts_on;
static int try_no, resets_ans, resets_ans2, disables;
static bool running, booted, cfs, disable_fails;
static u64 cleared;
static void event(char c) { size_t n = strlen(events); assert(n + 1 < sizeof(events)); events[n] = c; }
static u32 read32(u64 addr)
{
    if (addr == nvme_base + NVME_BOOT_STATUS)
        return booted ? NVME_BOOT_STATUS_OK : 0;
    if (addr == nvme_base + NVME_CSTS)
        return cfs ? NVME_CSTS_CFS : 0;
    return 0;
}
static void write64_lo_hi(u64 addr, u64 value) { if (!value) cleared |= addr; }
static int poll32(u64 a, u32 mask, u32 value, u32 timeout) { return booted ? 0 : -1; }
static int adt_path_offset(void *a, const char *p) { return 1; }
/* The ANS node: the ADT's names only its ASC wrapper, as on the J604; the payload tree's the type. */
static const char *adt_ans_compatible = "iop,ascwrap-v6";
static char payload_ans_compatible[32];
static int payload_tree;
static bool payload_loaded;
static bool adt_is_compatible(const void *a, int n, const char *c) { return !strcmp(c, adt_ans_compatible); }
static const void *payload_fdt(void) { return payload_loaded ? &payload_tree : NULL; }
static int fdt_node_offset_by_compatible(const void *f, int s, const char *c)
{
    return f == &payload_tree && !strcmp(c, payload_ans_compatible) ? 1 : -1;
}
static const void *adt_get_property(void *a, int n, const char *p) { return NULL; }
static bool asc_cpu_running(asc_dev_t *a) { return running; }
struct asc_message { u64 msg0; u32 msg1; };
static void asc_i2a_enable(asc_dev_t *a, bool on) {}
static bool asc_recv(asc_dev_t *a, struct asc_message *m) { return false; }
static u32 asc_cpu_status(asc_dev_t *a) { return 0; }
static void asc_cpu_stop(asc_dev_t *a) { event('X'); running = false; }
static int pcie_ans_power_off(void) { event('O'); booted = false; return 0; }
static int pcie_ans_power_on(void) { event('P'); return 0; }
static void pmgr_reset(int die, const char *name)
{
    event('R');
    if (!strcmp(name, "ANS"))
        resets_ans++;
    else if (!strcmp(name, "ANS2"))
        resets_ans2++;
}
static bool sart_set_entries(sart_dev_t *s, const struct sart_entry *e, int n) { event('E'); return true; }
static int sart_get_entries(sart_dev_t *s, struct sart_entry *e, int max) { return 0; }
static rtkit_dev_t *rtkit_init(const char *name, asc_dev_t *a, void *d, void *i, sart_dev_t *s, bool sram)
{ event('I'); return &rtkit; }
static void rtkit_stop(rtkit_dev_t *r) { event('S'); running = false; }
static void rtkit_free(rtkit_dev_t *r) { event('F'); }
static bool rtkit_idle(rtkit_dev_t *r) { return true; }
static bool rtkit_boot_attempt(char c, u32 wait)
{
    event(c);
    hello[hellos++] = wait;
    running = true;
    booted = starts_on & (1u << try_no++);
    return booted;
}
static bool rtkit_boot_warm(rtkit_dev_t *r, u32 mask) { assert(mask == 1); return rtkit_boot_attempt('W', USEC_PER_SEC); }
static bool rtkit_boot_wait(rtkit_dev_t *r) { return rtkit_boot_attempt('C', USEC_PER_SEC); }
static bool nvme_ctrl_disable(void) { disables++; return !disable_fails; }
static bool nvme_ctrl_shutdown(void) { return true; }
static bool nvme_ctrl_suspend(void) { abort(); }
''' + qregs + power + r'''
static void reset(u32 chip, unsigned on)
{
    memset(events, 0, sizeof(events));
    memset(hello, 0, sizeof(hello));
    hellos = try_no = resets_ans = resets_ans2 = disables = 0;
    chip_id = chip;
    snprintf(payload_ans_compatible, sizeof(payload_ans_compatible), "apple,t%x-nvme-ans2", chip);
    payload_loaded = true;
    adt_ans_compatible = "iop,ascwrap-v6";
    starts_on = on;
    running = booted = cfs = disable_fails = false;
    cleared = 0;
    nvme_type = NVME_T8132;
    nvme_secure_regs = false;
    nvme_initialized = false;
    nvme_reboot_required = false;
    nvme_asc = &asc;
    nvme_rtkit = &rtkit;
    nvme_sart = &sart;
    nvme_base = 0x4c5cc0000;
    nvmmu_base = 0x485cc0000;
}

int main(void)
{
    /* A good start: the one warm start every good boot took, unchanged. */
    reset(T8132, 1);
    assert(nvme_ans_power_on());
    assert(!strcmp(events, "PEW") && hellos == 1 && hello[0] == USEC_PER_SEC && !resets_ans);
    /* Stopped and fatal at power-on, on an ANS that takes a cold reset: straight to the cold start. */
    reset(T8132, 1);
    cfs = true;
    assert(nvme_ans_power_on());
    assert(!strcmp(events, "PRREC") && hellos == 1 && hello[0] == USEC_PER_SEC);
    assert(resets_ans == 1 && resets_ans2 == 1);
    /* Fatal but the CPU still running: not the stopped state, so the warm start. */
    reset(T8132, 1);
    cfs = running = true;
    assert(nvme_ans_power_on());
    assert(!strcmp(events, "PEW") && !resets_ans);
    /* No HELLO on the warm start: undone completely, then the cold start, once. */
    reset(T8132, 2);
    assert(nvme_ans_power_on());
    assert(!strcmp(events, "PEW" "SFOPRREIC") && hellos == 2 && hello[1] == USEC_PER_SEC);
    assert(resets_ans == 1 && resets_ans2 == 1);
    /* Never: the warm start and one cold start, then down; nothing is tried a third time. */
    reset(T8132, 0);
    assert(!nvme_ans_power_on());
    assert(!strcmp(events, "PEW" "SFOPRREIC") && hellos == 2);
    /* Fatal at power-on and the cold start fails: down, no second cold start. */
    reset(T8132, 0);
    cfs = true;
    assert(!nvme_ans_power_on());
    assert(!strcmp(events, "PRREC") && hellos == 1);
    /* An ANS type without cold_reset: one warm start, never a PMGR reset, fatal or not. */
    reset(T8140, 0);
    assert(!nvme_ans_power_on());
    assert(!strcmp(events, "PEW") && hellos == 1 && !resets_ans && !resets_ans2);
    reset(T8140, 0);
    cfs = true;
    assert(!nvme_ans_power_on());
    assert(!strcmp(events, "PEW") && hellos == 1 && !resets_ans && !resets_ans2);
    /* The type is the ANS node's compatible, the one Linux keys cold_reset on, never the chip. */
    reset(T8132, 0);
    assert(nvme_ans_cold_reset());
    for (unsigned c = 0; c < 5; c++) {
        static const u32 others[] = {T8140, T6040, T6041, T8142, T6050};
        reset(others[c], 0);
        assert(!nvme_ans_cold_reset());
    }
    /* The chip alone picks nothing: a T8132 whose payload tree names another type, or no tree. */
    reset(T8132, 0);
    strcpy(payload_ans_compatible, "apple,t8140-nvme-ans2");
    assert(!nvme_ans_cold_reset());
    reset(T8132, 0);
    payload_loaded = false;
    assert(!nvme_ans_cold_reset());
    /* An ADT ANS node that names the type is enough, on any chip, without a payload tree. */
    reset(0x9999, 0);
    payload_loaded = false;
    adt_ans_compatible = "apple,t8132-nvme-ans2";
    assert(nvme_ans_cold_reset());
    return 0;
}
'''
    binary = tmp_path / "nvme-ans-restart"
    subprocess.run(
        [*host_cc(), "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-variable", "-Wno-unused-function", "-Wno-format", "-x", "c", "-", "-o",
         str(binary)],
        input=harness, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)
