/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"
#include "../build/build_tag.h"

#include "../config.h"

#include "adt.h"
#include "aic.h"
#include "boot_options.h"
#include "cpufreq.h"
#include "display.h"
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

const char version_tag[] = "##m1n1_ver##" BUILD_TAG;
const char *const m1n1_version = version_tag + 12;

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

void run_actions(void)
{
    bool usb_up = false;
    enum stage1_proxy_result window_result = STAGE1_PROXY_TIMEOUT;
    enum stage1_payload_action payload_action;

#ifndef J613_ESP_STAGE1
    /* J613 Stage 1 runs its own USB window after the board checks below. */
    u32 window_ms = stage1_config_window_ms();
    if (window_ms && !aura_proxy_enabled()) {
        window_result = uartproxy_wait_stage1(window_ms);
        if (window_result == STAGE1_PROXY_CDC || window_result == STAGE1_PROXY_KIS) {
            printf("Stage 1: host request received\n");
            fb_set_active(true);
            uartproxy_run_presynced();
            while (!next_stage.entry)
                uartproxy_run(NULL);
            return;
        }
        printf("Stage 1: proxy window ended\n");
    }
#endif

#ifdef J613_ESP_STAGE1
    /* The board and configuration checks fall back before any payload is checked. */
    payload_action =
        stage1_proxy_payload_action(window_result, false, usb_cdc_ready(), !usb_cdc_owns_carrier());
    const char *target = adt_getprop(adt, 0, "target-type", NULL);
    /* Stage 1 only chainloads; stage 2 applies each board's hardware gates. */
    bool board = target && (!strcmp(target, "J613") || !strcmp(target, "J615"));
    bool j613 = chip_id == T8122 && board && os_firmware.version == V26_6_2;
    if (!j613) {
        printf(
            "J613 Stage 1: board/25G83 guard rejected (needs J613 or J615, 26.6.2); proxy only\n");
        goto proxy_fallback;
    }
    if (!strcmp(target, "J615"))
        printf("J613 Stage 1: J615 admitted (EXPERIMENTAL)\n");
    if (!stage1_config_target())
        goto proxy_fallback;
    /* A zero window skips USB entirely: straight to the ESP file, as a plain stage 1 does. */
    if (stage1_config_window_ms()) {
        u32 window_ms = stage1_config_window_ms();
        usb_init();
        usb_iodev_init();
        usb_up = true;
        u64 deadline = timeout_calculate((u64)window_ms * 1000);
        while (!timeout_expired(deadline)) {
            for (int j = 0; j < USB_IODEV_COUNT; j++) {
                iodev_id_t iodev = IODEV_USB0 + j;
                if (!(iodev_get_usage(iodev) & USAGE_UARTPROXY))
                    continue;
                usb_iodev_vuart_setup(iodev);
                iodev_handle_events(iodev);
                if (iodev_can_write(iodev) || iodev_can_write(IODEV_USB_VUART)) {
                    printf("J613 Stage 1: host grabbed proxy window\n");
                    uartproxy_run(NULL);
                    return;
                }
            }
            mdelay(10);
        }
        printf("J613 Stage 1: window expired; loading ESP candidate\n");
    } else {
        printf("J613 Stage 1: no USB window; loading ESP candidate\n");
    }
#endif

#ifndef BRINGUP
#if defined(EARLY_PROXY_TIMEOUT) && !defined(J613_ESP_STAGE1)
    int node = adt_path_offset(adt, "/chosen/asmb");
    u64 lp_sip0 = 0;

    if (node >= 0) {
        ADT_GETPROP(adt, node, "lp-sip0", &lp_sip0);
        printf("Boot policy: sip0 = %ld\n", lp_sip0);
    }

    if (chip_id != T8140 && !cur_boot_args.video.display && lp_sip0 == 127) {
        printf("Bringing up USB for early debug...\n");

        usb_init();
        usb_iodev_init();

        usb_up = true;

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

    if (aura_proxy_enabled())
        printf("aura-proxy: stage 1 payload chainload disabled\n");
    else
        printf("Checking for payloads...\n");

    bool payload_valid = !aura_proxy_enabled() && payload_run() == 0;
    payload_action = stage1_proxy_payload_action(window_result, payload_valid, usb_cdc_ready(),
                                                 !usb_cdc_owns_carrier());
    if (payload_action == STAGE1_PAYLOAD_NATIVE) {
        printf("Valid payload found\n");
        return;
    }
#ifdef J613_ESP_STAGE1
proxy_fallback:
#endif
    fb_set_active(true);

    printf("No valid payload found\n");
#ifdef J613_ESP_STAGE1
    /* Shown on the panel: this is where a user without a USB host ends up. */
    printf("\nJ613 Stage 1: could not start Linux from the EFI partition.\n"
           "  Needs: MacBook Air M3 (13\" J613; 15\" J615 experimental), this volume's\n"
           "  macOS exactly 26.6.2,\n"
           "  and the stage-2 file named in this stage 1 (fill_j613_stage1_config.py).\n"
           "  Hold the power button to shut down, then hold it again to choose another system.\n");
#endif

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
    printf("Initialization complete. Running proxy...\n");
    uartproxy_run(NULL);
}

void m1n1_main(void)
{
    printf("\n\naura %s\n", m1n1_version);
    printf("Copyright The Asahi Linux Contributors\n");
    printf("Licensed under the MIT license\n\n");

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
    if (mcc_init() && chip_id == T8140)
        panic("T8140 MCC initialization failed\n");
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

    if (chip_id != T8152 && !nvme_shutdown()) {
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

    next_stage.entry(next_stage.args[0], next_stage.args[1], next_stage.args[2], next_stage.args[3],
                     next_stage.args[4]);

    panic("Next stage returned!\n");
}
