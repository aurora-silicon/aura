/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "hv.h"
#include "adt.h"
#include "smp.h"
#include "string.h"
#include "uart.h"
#include "utils.h"

#define WDT_TIMEOUT 1

static bool hv_wdt_active = false;
static bool hv_wdt_enabled = false;
static bool hv_wdt_join_pending = false;
static volatile u64 hv_wdt_timestamp = 0;
static u64 hv_wdt_timeout = 0;
static volatile u64 hv_wdt_breadcrumbs[MAX_CPUS] = {0};

static int hv_wdt_cpu;

static u64 cpu_dbg_base = 0;

void hv_do_panic(void)
{
    printf("Breadcrumbs:\n");
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (cpu > 0 && !smp_is_alive(cpu))
            continue;

        u64 tmp = hv_wdt_breadcrumbs[cpu];

        printf("CPU %2d: ", cpu);
        for (int i = 56; i >= 0; i -= 8) {
            char c = (tmp >> i) & 0xff;
            if (c)
                printf("%c", c);
        }
        printf("\n");
    }

    printf("Attempting to enter proxy\n");
    iodev_console_flush();

    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = HV_WDT_BARK,
    };

    uartproxy_run(&start);
}

void hv_wdt_bark(void)
{
    uart_puts("HV watchdog: bark!");

    uart_printf("Breadcrumbs: ");
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) {
        if (cpu > 0 && !smp_is_alive(cpu))
            continue;

        u64 tmp = hv_wdt_breadcrumbs[cpu];

        uart_printf("CPU %2d: ", cpu);
        for (int i = 56; i >= 0; i -= 8) {
            char c = (tmp >> i) & 0xff;
            if (c)
                uart_putchar(c);
        }
        uart_putchar('\n');
    }

    uart_puts("Attempting to enter proxy");

    struct uartproxy_msg_start start = {
        .reason = START_HV,
        .code = HV_WDT_BARK,
    };

    uartproxy_run(&start);
#ifdef AURA_WINDOWS
    hv_safe_reset("HV watchdog bark");
#else
    reboot();
#endif
}

void hv_wdt_main(void)
{
    while (hv_wdt_active) {
        if (hv_wdt_enabled) {
            sysop("dmb ish");
            u64 timestamp = hv_wdt_timestamp;
            sysop("isb");
            u64 now = mrs(CNTPCT_EL0);
            sysop("isb");
            if ((now - timestamp) > hv_wdt_timeout)
                hv_wdt_bark();
        }

        udelay(1000);

        sysop("dmb ish");
    }
}

void hv_wdt_pet(void)
{
    hv_wdt_timestamp = mrs(CNTPCT_EL0);
    sysop("dmb ish");
}

void hv_wdt_suspend(void)
{
    hv_wdt_enabled = false;
    sysop("dsb ish");
}

void hv_wdt_resume(void)
{
    hv_wdt_pet();
    hv_wdt_enabled = true;
    sysop("dsb ish");
}

void hv_wdt_breadcrumb(char c)
{
    u64 cpu = mrs(TPIDR_EL2);
    u64 tmp = hv_wdt_breadcrumbs[cpu];
    tmp <<= 8;
    tmp |= c;
    hv_wdt_breadcrumbs[cpu] = tmp;
    sysop("dmb ish");
}

u64 hv_wdt_breadcrumbs_read(int cpu)
{
    return cpu >= 0 && cpu < MAX_CPUS ? hv_wdt_breadcrumbs[cpu] : 0;
}

void hv_wdt_init(void)
{
    char boot_cpu_name[32];

    snprintf(boot_cpu_name, sizeof(boot_cpu_name), "/cpus/cpu%d", boot_cpu_idx);
    int node = adt_path_offset(adt, boot_cpu_name);
    if (node < 0) {
        printf("Error getting %s node\n", boot_cpu_name);
        return;
    }

    u64 reg[2];
    if (ADT_GETPROP_ARRAY(adt, node, "cpu-uttdbg-reg", reg) < 0) {
        printf("Error getting cpu-uttdbg-reg property\n");
        return;
    }

    cpu_dbg_base = reg[0];
}

int hv_wdt_start(int cpu)
{
    if (hv_wdt_active)
        return 0;
    if (hv_wdt_join_pending)
        return -1;

    hv_wdt_cpu = cpu;
    memset((void *)hv_wdt_breadcrumbs, 0, sizeof(hv_wdt_breadcrumbs));
    hv_wdt_timeout = mrs(CNTFRQ_EL0) * WDT_TIMEOUT;
    hv_wdt_pet();
    hv_wdt_active = true;
    hv_wdt_enabled = true;
    int ret = smp_call4(hv_wdt_cpu, hv_wdt_main, 0, 0, 0, 0);
    hv_wdt_join_pending = ret == 0 || ret == SMP_TIMEOUT;
    if (ret) {
        hv_wdt_active = false;
        hv_wdt_enabled = false;
        sysop("dmb ish");
        return -1;
    }
    return 0;
}

int hv_wdt_stop(void)
{
    if (!hv_wdt_join_pending)
        return 0;

    hv_wdt_active = false;
    sysop("dmb ish");
    if (smp_wait(hv_wdt_cpu, NULL))
        return -1;
    hv_wdt_join_pending = false;
    return 0;
}
