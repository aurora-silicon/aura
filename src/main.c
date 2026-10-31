/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"
#include "../build/build_tag.h"

#include "../config.h"

#include "adt.h"
#include "aic.h"
#include "boot_options.h"
#include "chainload.h"
#include "cpufreq.h"
#include "display.h"
#include "dockchannel_uart.h"
#include "exception.h"
#include "fb.h"
#include "firmware.h"
#include "gxf.h"
#include "heapblock.h"
#include "j873.h"
#include "mcc.h"
#include "memory.h"
#include "nvme.h"
#include "payload.h"
#include "pcie.h"
#include "pmgr.h"
#include "pmu_counter.h"
#include "sep.h"
#include "smp.h"
#include "stage1_config.h"
#include "stage1_proxy.h"
#include "string.h"
#include "tps6598x.h"
#include "uart.h"
#include "uartproxy.h"
#include "usb.h"
#include "usb_cdc.h"
#include "utils.h"
#include "wdt.h"
#include "xnuboot.h"

struct vector_args next_stage;

const char version_tag[] = "##aura_ver##" BUILD_TAG;
const char *const aura_version = version_tag + 12;

u32 board_id = ~0, chip_id = ~0;

void get_device_info(void)
{
    const char *model = (const char *)adt_getprop(adt, 0, "model", NULL);
    const char *target = (const char *)adt_getprop(adt, 0, "target-type", NULL);

    printf("Device info:\n");

    if (model)
        printf("  Model: %s\n", model);

    if (target)
        printf("  Target: %s\n", target);

    is_mac = !!strstr(model, "Mac");

    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen > 0) {
        if (ADT_GETPROP(adt, chosen, "board-id", &board_id) < 0)
            printf("Failed to find board-id\n");
        if (ADT_GETPROP(adt, chosen, "chip-id", &chip_id) < 0)
            printf("Failed to find chip-id\n");

        printf("  Board-ID: 0x%x\n", board_id);
        printf("  Chip-ID: 0x%x\n", chip_id);
    } else {
        printf("No chosen node!\n");
    }

    printf("\n");
}

/* Stage 1 runs at most one proxy window per boot: from its config or an aura-proxy request. */
static bool stage1_window_done;
static enum stage1_proxy_result stage1_window_result = STAGE1_PROXY_TIMEOUT;

static void stage1_run_host_proxy(void)
{
    printf("Stage 1: host request received\n");
    fb_set_active(true);
    uartproxy_run_presynced();
    while (!next_stage.entry)
        uartproxy_run(NULL);
}

static bool stage1_wait_request_window(void)
{
    stage1_window_result = uartproxy_wait_stage1(STAGE1_PROXY_REQUEST_MS);
    if (stage1_window_result == STAGE1_PROXY_CDC || stage1_window_result == STAGE1_PROXY_KIS) {
        stage1_run_host_proxy();
        return true;
    }
    printf("Stage 1: proxy window ended\n");
    return false;
}

bool stage1_proxy_request_window(void)
{
    if (stage1_window_done || stage1_chained)
        return false;
    stage1_window_done = true;
    printf("Stage 1: aura-proxy on the ESP, waiting %u s for a host\n",
           STAGE1_PROXY_REQUEST_MS / 1000);
    return stage1_wait_request_window();
}

#ifdef DEBUGUSB_PROXY
bool stage1_window_offered(void)
{
    return stage1_window_done;
}

/*
 * A boot started by a VDM `reboot debugusb` with a host reading the DockChannel console gets the
 * aura-proxy window, once per chain: not after an earlier aura stage offered one.
 */
bool debugusb_proxy_window(void)
{
    if (stage1_window_done || stage1_chained || stage1_window_passed ||
        !dockchannel_uart_host_seen())
        return false;
    stage1_window_done = true;
    printf("aura: debugusb boot, opening proxy window\n");
    printf("Stage 1: waiting %u s for a host\n", STAGE1_PROXY_REQUEST_MS / 1000);
    return stage1_wait_request_window();
}
#endif

void run_actions(void)
{
    bool usb_up = false;
    enum stage1_proxy_result window_result = STAGE1_PROXY_TIMEOUT;
    enum stage1_payload_action payload_action;

    u32 window_ms = stage1_config_window_ms();
    /* aura-proxy: wait for a host, then boot as normal if none came. */
    if (aura_proxy_enabled() && window_ms < STAGE1_PROXY_REQUEST_MS) {
        printf("aura-proxy: waiting %u s for a host\n", STAGE1_PROXY_REQUEST_MS / 1000);
        window_ms = STAGE1_PROXY_REQUEST_MS;
    }
    if (window_ms) {
        window_result = stage1_window_result = uartproxy_wait_stage1(window_ms);
        stage1_window_done = true;
        if (window_result == STAGE1_PROXY_CDC || window_result == STAGE1_PROXY_KIS) {
            stage1_run_host_proxy();
            return;
        }
        printf("Stage 1: proxy window ended\n");
    }

#ifndef BRINGUP
#ifdef EARLY_PROXY_TIMEOUT
    int node = adt_path_offset(adt, "/chosen/asmb");
    u64 lp_sip0 = 0;

    if (node >= 0) {
        ADT_GETPROP(adt, node, "lp-sip0", &lp_sip0);
        printf("Boot policy: sip0 = %ld\n", lp_sip0);
    }

    if (chip_id != T8140 && !cur_boot_args.video.display && lp_sip0 == 127) {
        if (!usb_up) {
            printf("Bringing up USB for early debug...\n");
            usb_init();
            usb_iodev_init();
            usb_up = true;
        }

        printf("Waiting for proxy connection... ");
        for (int i = 0; i < EARLY_PROXY_TIMEOUT * 100; i++) {
            for (int j = 0; j < USB_IODEV_COUNT; j++) {
                iodev_id_t iodev = IODEV_USB0 + j;

                if (!(iodev_get_usage(iodev) & USAGE_UARTPROXY))
                    continue;

                usb_iodev_vuart_setup(iodev);
                iodev_handle_events(iodev);
                if (iodev_can_write(iodev) || iodev_can_write(IODEV_USB_VUART)) {
                    printf(" Connected!\n");
                    uartproxy_run(NULL);
                    return;
                }
            }

            mdelay(10);
            if (i % 100 == 99)
                printf(".");
        }
        printf(" Timed out\n");
    }
#endif
#endif

    printf("Checking for payloads...\n");

    bool payload_valid = payload_run() == 0;
    payload_action = stage1_proxy_payload_action(stage1_window_result, payload_valid,
                                                 usb_cdc_ready(), !usb_cdc_owns_carrier());
    if (payload_action == STAGE1_PAYLOAD_NATIVE) {
        printf("Valid payload found\n");
        return;
    }
    fb_set_active(true);

    printf("No valid payload found\n");
    if (stage1_config_target())
        /* Shown on the panel: this is where a user without a USB host ends up. */
        printf("\nStage 1: could not start the configured Stage 2 from the EFI partition.\n"
               "  Check that the file this Stage 1 names exists on its EFI partition.\n"
               "  Hold the power button to shut down, then hold it again to choose another "
               "system.\n");

    if (payload_action == STAGE1_PAYLOAD_HALT)
        panic("Stage 1: no safe proxy carrier after CDC failure\n");

#ifndef BRINGUP
    if (!usb_up && !aura_debug_enabled()) {
        usb_init();
        usb_iodev_init();
    }
#endif

    printf("Running proxy...\n");

    uartproxy_run(NULL);
}

static void j873g_run_proxy(void)
{
    u32 model_len = 0, target_len = 0;
    const char *model = adt_getprop(adt, 0, "model", &model_len);
    const char *target = adt_getprop(adt, 0, "target-type", &target_len);

    if (board_id != 0x24 || !model || model_len != sizeof("Mac18,5") ||
        memcmp(model, "Mac18,5", sizeof("Mac18,5")) || !target || target_len != sizeof("J873g") ||
        memcmp(target, "J873g", sizeof("J873g")))
        panic("Unsupported T8152 board\n");

    /* Adopt firmware power and USB state. Only the qualified display route
     * may be initialized here; secondary CPUs remain parked for the proxy. */
    printf("J873g: KIS bring-up with native SMP\n");
    mmu_init();
    wdt_disable();
    if (smp_init() < 0)
        panic("Unsupported T8152 CPU topology\n");
    /* The DCPEXT0/DPTX_PHY power gates are PMGR2 devices; without this the
     * HDMI phy stays gated and the first register access aborts. */
    pmgr_init();
#ifdef USE_FB
    if (display_init() < 0)
        printf("display: initialization failed, continuing with firmware framebuffer\n");
    fb_init(false);
    fb_display_logo();
    fb_set_active(true);
#endif
    /* What the J873 Linux device trees expect the loader to have done. */
    j873_usb_prep();
    j873_n1_power();
    j873_pcie_prep();
    printf("Initialization complete.\n");

    /* An appended payload (or boot= from storage) boots directly. */
    if (!aura_proxy_enabled()) {
        printf("Checking for payloads...\n");
        if (payload_run() == 0) {
            printf("Valid payload found\n");
            return;
        }
        printf("No valid payload found\n");
    }
    printf("Running proxy...\n");
    uartproxy_run(NULL);
}

void aura_main(void)
{
    printf("\n\naura %s\n", aura_version);
    printf("Copyright Aurora Silicon and contributors\n");
    printf("GPL-2.0-only, with inherited code under MIT. See LICENSE.\n\n");

    printf("Running in EL%lu\n\n", mrs(CurrentEL) >> 2);

    firmware_init();

    heapblock_init();

    if (chip_id == T8152) {
        j873g_run_proxy();
        goto next_stage;
    }

#ifndef BRINGUP
    if (supports_gxf())
        gxf_init();
    if (mcc_init() && (chip_id == T8140 || chip_id == T6040))
        panic("MCC initialization failed\n");
    mmu_init();
    aic_init();
    smp_init();
#endif
    wdt_disable();
#ifndef BRINGUP
    if (pmgr_init() && chip_id == T8140)
        panic("T8140 PMGR initialization failed\n");
#ifdef USE_DEBUG_USB
    tps6598x_enable_debugusb();
#else
    if (aura_debug_enabled())
        tps6598x_enable_debugusb();
#endif
#ifdef USE_FB
    display_init();
    // Kick DCP to sleep, so dodgy monitors which cause reconnect cycles don't cause us to lose the
    // framebuffer.
    display_shutdown(DCP_SLEEP_IF_EXTERNAL);
    // On idevice we need to always clear, because otherwise it looks scuffed on white devices
    fb_init(!is_mac);
    fb_display_logo();
#ifdef FB_SILENT_MODE
    fb_set_active(!cur_boot_args.video.display);
#else
    fb_set_active(true);
#endif
#endif

    cpufreq_fixup();
#endif

    /* T6040: nothing else in an aura tether chain clears iBoot's failed-boot count. */
    if (chip_id == T6040)
        pmu_clear_panic_counter();

    printf("Initialization complete.\n");

    run_actions();

next_stage:
    if (!next_stage.entry) {
        panic("Nothing to do!\n");
    }
    if (smp_has_pending_calls()) {
        next_stage.entry = NULL;
        panic("Refusing next stage while secondary callbacks are pending\n");
    }

    printf("Preparing to run next stage at %p...\n", next_stage.entry);

    if (!nvme_shutdown()) {
        printf("NVMe handoff failed; returning to proxy\n");
        uartproxy_run(NULL);
        panic("NVMe handoff failed\n");
    }
    usb_cdc_cleanup();
    exception_shutdown();
#ifndef BRINGUP
    if (chip_id != T8152) {
        usb_iodev_shutdown();
        /* Its firmware may still use memory the next stage will reuse. */
        if (display_shutdown(DCP_SLEEP_IF_EXTERNAL) < 0) {
            printf("Display handoff failed; returning to proxy\n");
            uartproxy_run(NULL);
            panic("Display handoff failed\n");
        }
    }
#ifdef USE_FB
    fb_shutdown(next_stage.restore_logo);
#endif
    mmu_shutdown();
#endif

    printf("Vectoring to next stage...\n");
#if defined(CHAINLOADING) && !defined(ESP_STAGE2)
    stage1_log_flush();
#endif

    next_stage.entry(next_stage.args[0], next_stage.args[1], next_stage.args[2], next_stage.args[3],
                     next_stage.args[4]);

    panic("Next stage returned!\n");
}
