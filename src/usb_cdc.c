/* SPDX-License-Identifier: GPL-2.0-only */

#include "../build/build_cfg.h"

#include "boot_options.h"
#include "string.h"
#include "tps6598x.h"
#include "usb.h"
#include "usb_cdc.h"
#include "usb_cdc_state.h"
#include "utils.h"
#include "wdt.h"

static struct usb_cdc_state cdc;
static tps6598x_irq_state_t cdc_irq_state;
static bool watchdog_armed;
static bool carrier_owned;

static bool usb2_target(void)
{
    return ((chip_id == T8142 || chip_id == T8152) && board_id == 0x24) || usb_cdc_deferred_usb2();
}

static bool cdc_usb2_hpm(char *path, void *unused)
{
    (void)unused;
    return !strcmp(path, "/arm-io/nub-spmi-a0/hpm0");
}

static int cdc_usb2_route(char *path, tps6598x_dev_t *hpm, void *unused)
{
    (void)path;
    (void)unused;
    /* The command may retire KIS even when its reply is lost. */
    carrier_owned = true;
    return tps6598x_route_ap_usb2(hpm) ? HPM_ACTION_ERROR : HPM_ACTION_STOP;
}

static bool cdc_dfu_hpm(char *path, void *unused)
{
    (void)unused;
    return !strcmp(path, "/arm-io/nub-spmi-a0/hpm0");
}

static int cdc_reset_hpm(char *path, tps6598x_dev_t *hpm, void *unused)
{
    (void)path;
    (void)unused;
    return tps6598x_cold_reset(hpm) ? HPM_ACTION_ERROR : HPM_ACTION_STOP;
}

static int cdc_power_hpm(char *path, tps6598x_dev_t *hpm, void *unused)
{
    (void)path;
    (void)unused;
    if (tps6598x_powerup(hpm) || tps6598x_disable_irqs(hpm, &cdc_irq_state))
        return HPM_ACTION_ERROR;
    return HPM_ACTION_STOP;
}

static void cdc_set_step(unsigned step)
{
    cdc.step = step;
}

int usb_cdc_schedule(u32 delay_ms, u32 reserved, u32 flags)
{
    if (usb2_target()) {
        /* Explicit USB2 transition only: no HPM reset, lane swap or watchdog. */
        if (flags || reserved || cdc.state != USB_CDC_IDLE || delay_ms < 1000 || delay_ms > 15000)
            return -1;
        cdc.deadline_ms = ticks_to_msecs(get_ticks()) + delay_ms;
        cdc.state = USB_CDC_SCHEDULED;
        return 0;
    }
    if (aura_debug_enabled())
        return -1;
    if (chip_id != T8140)
        return -1;
    int ret = usb_cdc_state_schedule(&cdc, delay_ms, reserved, flags, ticks_to_msecs(get_ticks()));
    if (!ret && usb_cdc_arm_watchdog()) {
        cdc.state = USB_CDC_IDLE;
        return -1;
    }
    return ret;
}

u32 usb_cdc_status(void)
{
    return usb_cdc_state_status(&cdc);
}

void usb_cdc_poll(void)
{
    if (usb2_target()) {
        if (!usb_cdc_state_due(&cdc, ticks_to_msecs(get_ticks())))
            return;
        /* Consume once before touching hardware. Never retry failed setup. */
        cdc.state = USB_CDC_FAILED;
        cdc.step = 1;
        if (usb_cdc_link_start(cdc_set_step, false))
            goto usb2_failed;
        cdc.step = 5;
        if (tps6598x_foreach_hpm(cdc_usb2_hpm, cdc_usb2_route, NULL) != HPM_ACTION_STOP)
            goto usb2_failed;
        cdc.state = USB_CDC_DWC_READY;
        printf("CDC USB2 controller ready; awaiting host enumeration, status=0x%x\n",
               usb_cdc_status());
        return;
    usb2_failed:
        printf("CDC USB2 failed at step %u; allocations retained\n", cdc.step);
        return;
    }
    if (!usb_cdc_state_due(&cdc, ticks_to_msecs(get_ticks())))
        return;

    cdc.step = 1;
    /* HPM0 cold reset irrevocably retires the inherited KIS carrier. */
    carrier_owned = true;
    if (cdc.flags & BIT(2)) {
        if (tps6598x_foreach_hpm(cdc_dfu_hpm, cdc_reset_hpm, NULL) != HPM_ACTION_STOP)
            goto failed;
        mdelay(1000);
    }

    cdc.step = 2;
    if (tps6598x_foreach_hpm(cdc_dfu_hpm, cdc_power_hpm, NULL) != HPM_ACTION_STOP)
        goto failed;

    if (usb_cdc_link_start(cdc_set_step, !!(cdc.flags & BIT(0))))
        goto failed;

    cdc.state = USB_CDC_DWC_READY;
    printf("CDC Gen1 ready; awaiting host, status=0x%x\n", usb_cdc_status());
    return;

failed:
    cdc.state = USB_CDC_FAILED;
    printf("CDC transition failed at step %u; VDM recovery required\n", cdc.step);
}

int usb_cdc_arm_watchdog(void)
{
    if (chip_id != T8140 || wdt_arm_seconds(170))
        return -1;
    watchdog_armed = true;
    return 0;
}

bool usb_cdc_ready(void)
{
    return cdc.state == USB_CDC_DWC_READY;
}

bool usb_cdc_failed(void)
{
    return cdc.state == USB_CDC_FAILED;
}

bool usb_cdc_owns_carrier(void)
{
    /* Keep polling DockChannel: the host can explicitly restore KIS without
     * resetting the machine after the CDC upload. */
    return !usb2_target() && carrier_owned;
}

int usb_cdc_cancel(void)
{
    if (carrier_owned || cdc.state != USB_CDC_SCHEDULED)
        return -1;
    cdc.state = USB_CDC_IDLE;
    if (watchdog_armed) {
        wdt_disable();
        watchdog_armed = false;
    }
    return 0;
}

void usb_cdc_cleanup(void)
{
    usb_iodev_shutdown();
    if (watchdog_armed) {
        wdt_disable();
        watchdog_armed = false;
    }
}

void usb_cdc_primary_opened(void)
{
    if (cdc.state == USB_CDC_DWC_READY && watchdog_armed) {
        wdt_disable();
        watchdog_armed = false;
        printf("CDC primary open; watchdog disabled\n");
    }
}

void usb_cdc_link_failed(void)
{
    cdc.state = USB_CDC_FAILED;
    cdc.step = 5;
    printf("CDC link mode mismatch; device stopped for VDM recovery\n");
}
