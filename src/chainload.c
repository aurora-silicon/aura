/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include "chainload.h"
#include "adt.h"
#include "boot_options.h"
#include "display.h"
#include "dockchannel_uart.h"
#include "heapblock.h"
#include "malloc.h"
#include "memory.h"
#include "nvme.h"
#include "secboot.h"
#include "stage1_config.h"
#include "string.h"
#include "types.h"
#include "utils.h"
#include "xnuboot.h"

#ifdef CHAINLOADING
int rust_load_image(const char *spec, void **image, size_t *size, bool *proxy_request, char *trial,
                    size_t trial_capacity);
void rust_free_image(void *image, size_t size);
#endif

extern u8 _chainload_stub_start[];
extern u8 _chainload_stub_end[];

_Static_assert(sizeof(struct chainload_header) == CHAINLOAD_HDR_SIZE, "load header size");
_Static_assert(offsetof(struct chainload_header, footprint) == CHAINLOAD_HDR_FOOTPRINT_OFF,
               "load header footprint offset");
_Static_assert(offsetof(struct chainload_header, run_base) == CHAINLOAD_HDR_RUN_BASE_OFF,
               "load header run_base offset");

/*
 * Return how many bytes from its base an image needs to run at a run_base below its staged copy
 * (chainload.h), or 0 if it does not support that.
 */
static u64 chainload_run_footprint(const void *image, size_t size, u32 *flags)
{
    struct chainload_header hdr;

    if (size < CHAINLOAD_HDR_OFFSET + sizeof(hdr))
        return 0;
    memcpy(&hdr, (const u8 *)image + CHAINLOAD_HDR_OFFSET, sizeof(hdr));
    if (hdr.magic != CHAINLOAD_HDR_MAGIC || hdr.version != CHAINLOAD_HDR_VERSION ||
        !(hdr.flags & CHAINLOAD_HDR_F_RUN_BASE) || hdr.run_base || !hdr.footprint ||
        (hdr.footprint & (SZ_16K - 1)) || hdr.footprint > size)
        return 0;
    if (flags)
        *flags = hdr.flags;
    return hdr.footprint;
}

#define LIVE_DCP_ADT_ENTRY_SIZE (sizeof(struct adt_property) + sizeof(u64))

/* A copied tree has one new property before its memory-map child. */
static int chainload_set_adt_range(void *copy, int node, const char *name, u64 range[2])
{
    if (!copy)
        return adt_setprop(adt, node, name, range, 2 * sizeof(u64));

    u32 size;
    const u8 *value = adt_getprop(adt, node, name, &size);
    if (!value || size != 2 * sizeof(u64))
        return -1;
    size_t offset = value - (const u8 *)adt;
    if (offset > cur_boot_args.devtree_size || size > cur_boot_args.devtree_size - offset)
        return -1;
    memcpy((u8 *)copy + offset + LIVE_DCP_ADT_ENTRY_SIZE, range, size);
    return 0;
}

int chainload_image(void *image, size_t size, char **vars, size_t var_cnt)
{
    u64 new_base = (u64)_base;
    u64 image_size = size;
    u64 ram_end, ro_start = 0, ro_end = 0;
    u64 live_heap;
    u64 inherited_top = cur_boot_args.top_of_kernel_data;
    u64 protected_heap_start = inherited_top;
    int ro_status = memory_fw_ro_range(&ro_start, &ro_end);

    printf("chainload: Preparing image...\n");
    next_stage.entry = NULL;
    if (!image || !size || ro_status < 0 ||
        cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
        return -1;
    u32 target_flags = 0;
    u64 target_footprint = chainload_run_footprint(image, size, &target_flags);
    if (!(target_flags & CHAINLOAD_HDR_F_LIVE_DCP) && display_needs_live_receiver()) {
        printf("chainload: next image cannot inherit the running DCP\n");
        return -1;
    }
    /* Capture any newly retained DCP buffers before laying out the next image. */
    if (display_prepare_live_handoff() < 0)
        return -1;
    live_heap = heapblock_high_water();
    ram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
    display_inherited_live_heap(&protected_heap_start);
    if (new_base < cur_boot_args.phys_base || new_base >= ram_end || live_heap > ram_end ||
        inherited_top > ram_end || protected_heap_start > inherited_top ||
        protected_heap_start < cur_boot_args.phys_base)
        return -1;

    for (size_t i = 0; i < var_cnt; i++) {
        size_t len = strlen(vars[i]) + 1;
        if (image_size > UINT64_MAX - len)
            return -1;
        image_size += len;
    }
    if (image_size > UINT64_MAX - 4 - (SZ_16K - 1))
        return -1;
    image_size = ALIGN_UP(image_size + 4, SZ_16K);

    int anode = adt_path_offset(adt, "/chosen/memory-map");
    if (anode < 0)
        return -1;
    u64 sepfw[2];
    if (ADT_GETPROP_ARRAY(adt, anode, "SEPFW", sepfw) < 0 || sepfw[0] > UINT64_MAX - sepfw[1] ||
        sepfw[0] < cur_boot_args.phys_base || sepfw[0] + sepfw[1] > ram_end ||
        sepfw[1] > UINT64_MAX - (SZ_16K - 1))
        return -1;

    u64 fw_bootargs_off = ALIGN_UP(sepfw[1], SZ_16K);
    /*
     * Keep iBoot's pre-OS log with the firmware block, as the host chainloader does, so the
     * next stage (and macOS-style log readers) still find it after the move.
     */
    u64 preoslog[2] = {0};
    const void *log_prop = adt_getprop(adt, anode, "preoslog", NULL);
    if (log_prop && ADT_GETPROP_ARRAY(adt, anode, "preoslog", preoslog) < 0)
        return -1;
    if (preoslog[1] &&
        (preoslog[0] < cur_boot_args.phys_base || preoslog[1] > cur_boot_args.mem_size ||
         preoslog[0] - cur_boot_args.phys_base > cur_boot_args.mem_size - preoslog[1] ||
         preoslog[1] > UINT64_MAX - fw_bootargs_off - (SZ_16K - 1)))
        return -1;
    u64 fw_preoslog_off = fw_bootargs_off;
    fw_bootargs_off = ALIGN_UP(fw_bootargs_off + preoslog[1], SZ_16K);
    if (fw_bootargs_off > UINT64_MAX - SZ_16K)
        return -1;
    u64 fw_size = fw_bootargs_off + SZ_16K;
    int chosen = -1, prop_offset = -1;
    size_t adt_size = cur_boot_args.devtree_size;
    u64 adt_off = fw_size;
    if (display_handed_on_live()) {
        /* Build a private ADT copy for the next stage. The current stage may retry. */
        chosen = adt_path_offset(adt, "/chosen");
        if (chosen < 0)
            return -1;
        if (adt_get_property_count(adt, chosen) <= 0)
            return -1;
        prop_offset = adt_first_property_offset(adt, chosen);
        if (prop_offset < 0 || (size_t)prop_offset > adt_size ||
            adt_size > UINT32_MAX - LIVE_DCP_ADT_ENTRY_SIZE)
            return -1;
        adt_size += LIVE_DCP_ADT_ENTRY_SIZE;
        if (fw_size > UINT64_MAX - ALIGN_UP(adt_size, SZ_16K))
            return -1;
        fw_size += ALIGN_UP(adt_size, SZ_16K);
    }
    if (image_size > UINT64_MAX - fw_size || new_base > UINT64_MAX - image_size - fw_size)
        return -1;

    u64 image_end = new_base + image_size;
    u64 packed_end = image_end + fw_size;
    u64 run_base = 0, footprint = 0;
    bool must_stage = false;
    if (ro_status == 1 && image_end > ro_start && new_base < ro_end) {
        /*
         * T8140: the firmware RO range lies about 10 MiB above our base on a J700, and the
         * locked RVBAR of the secondary cores points at our base. Stage an image that does not
         * fit below the range after the live heap instead, and have it run its code from our
         * base (start.S) while its payload, SEPFW and boot_args stay in the staged copy, below
         * the new high-water.
         */
        footprint = target_footprint;
        if (!footprint || (new_base & (SZ_16K - 1)) || new_base >= ro_start ||
            footprint > ro_start - new_base) {
            printf("chainload: image overlaps firmware RO and cannot run below it\n");
            return -1;
        }
        run_base = new_base;
    } else if (packed_end > ram_end) {
        printf("chainload: image destination is invalid or overlaps firmware RO\n");
        return -1;
    } else if (live_heap > protected_heap_start && image_end > protected_heap_start &&
               new_base < live_heap) {
        /*
         * The next stage is larger than the room our caller left between our base and the heap
         * (a Stage 1 smaller than its Stage 2 plus the firmware block), so a copy to our base
         * would run over the heap that holds the staged image. An aura image can run from where
         * it is staged instead and move only its first footprint bytes to our base, which is
         * safe while those stay below the heap.
         */
        footprint = target_footprint;
        if (!footprint || (new_base & (SZ_16K - 1)) || protected_heap_start <= new_base ||
            footprint > protected_heap_start - new_base) {
            printf("chainload: image span overlaps the live heap\n");
            return -1;
        }
        must_stage = true;
    }

    bool park_fw = !run_base && !must_stage &&
                   ((ro_status == 1 && packed_end > ro_start && image_end < ro_end) ||
                    (live_heap > protected_heap_start && packed_end > protected_heap_start &&
                     image_end < live_heap));
    u8 *parked = NULL;
    u64 fw_base = image_end;
    if (park_fw) {
        printf(
            "chainload: packed span would overlap protected/live memory; parking firmware block\n");
        parked = memalign(SZ_16K, fw_size);
        if (!parked || (u64)parked > ram_end || fw_size > ram_end - (u64)parked) {
            free(parked);
            return -1;
        }
        fw_base = (u64)parked;
    }

    u64 copy_size = park_fw ? image_size : image_size + fw_size;
    size_t stub_size = _chainload_stub_end - _chainload_stub_start;
    if (copy_size > SIZE_MAX - stub_size) {
        free(parked);
        return -1;
    }
    u8 *new_image = memalign(SZ_16K, copy_size + stub_size);
    if (!new_image) {
        free(parked);
        return -1;
    }
    /*
     * The stub runs with the MMU and caches off, so copying a whole image (with its SEPFW block)
     * into place costs hundreds of milliseconds. An aura image can instead run from where it is
     * staged and move only its code and data (start.S): do that whenever the staged copy fits.
     */
    u64 staged = (u64)new_image;
    bool fits_staged =
        staged >= new_base + footprint && staged <= ram_end &&
        copy_size + stub_size <= ram_end - staged &&
        !(ro_status == 1 && staged < ro_end && staged + copy_size + stub_size > ro_start);
    if (!run_base && !park_fw && fits_staged) {
        u64 run_footprint = target_footprint;
        if (run_footprint && !(new_base & (SZ_16K - 1)) && staged >= new_base + run_footprint &&
            (ro_status != 1 || new_base + run_footprint <= ro_start || new_base >= ro_end)) {
            footprint = run_footprint;
            run_base = new_base;
        }
    }
    if (must_stage && !run_base) {
        printf("chainload: no room to stage the image above the live heap\n");
        free(new_image);
        return -1;
    }
    if (run_base) {
        /*
         * The staged copy is the destination: the stub has nothing to copy. The stub runs from
         * just after it, so the whole allocation must stay in RAM and clear of firmware RO.
         */
        new_base = staged;
        if (new_base < run_base + footprint || !fits_staged) {
            printf("chainload: no room to stage the image above firmware RO\n");
            free(new_image);
            return -1;
        }
        image_end = new_base + image_size;
        fw_base = image_end;
    }
    memset(new_image, 0, copy_size);
    memcpy(new_image, image, size);
    if (run_base)
        memcpy(new_image + CHAINLOAD_HDR_OFFSET + CHAINLOAD_HDR_RUN_BASE_OFF, &run_base,
               sizeof(run_base));

    u8 *p = new_image + size;
    for (size_t i = 0; i < var_cnt; i++) {
        size_t len = strlen(vars[i]);
        memcpy(p, vars[i], len);
        p[len] = '\n';
        p += len + 1;
    }

    u8 *fw_data = parked ? parked : new_image + image_size;
    memset(fw_data, 0, fw_size);
    memcpy(fw_data, (void *)sepfw[0], sepfw[1]);
    if (preoslog[1])
        memcpy(fw_data + fw_preoslog_off, (void *)preoslog[0], preoslog[1]);
    struct boot_args *new_boot_args = (struct boot_args *)(fw_data + fw_bootargs_off);
    *new_boot_args = cur_boot_args;
    u64 high_water = max(image_end, fw_base + fw_size);
    high_water = max(high_water, live_heap);
    high_water = max(high_water, inherited_top);
    if (ro_status == 1)
        high_water = max(high_water, ro_end);
    new_boot_args->top_of_kernel_data = high_water;

    u8 *next_adt = NULL;
    if (chosen >= 0) {
        next_adt = fw_data + adt_off;
        struct adt_property *prop = (void *)(next_adt + prop_offset);
        struct adt_node_hdr *node = (void *)(next_adt + chosen);
        u64 next_adt_pa = fw_base + adt_off;

        memcpy(next_adt, adt, prop_offset);
        memcpy(prop->name, DISPLAY_LIVE_ADT_PROP, sizeof(DISPLAY_LIVE_ADT_PROP));
        prop->size = sizeof(inherited_top);
        memcpy(prop->value, &inherited_top, sizeof(inherited_top));
        memcpy(prop->value + sizeof(inherited_top), (u8 *)adt + prop_offset,
               cur_boot_args.devtree_size - prop_offset);
        node->property_count++;
        new_boot_args->devtree =
            (void *)(next_adt_pa - cur_boot_args.phys_base + cur_boot_args.virt_base);
        new_boot_args->devtree_size = adt_size;
        u64 tree_range[2] = {next_adt_pa, ALIGN_UP(adt_size, SZ_16K)};
        if (adt_getprop(adt, anode, "DeviceTree", NULL) &&
            chainload_set_adt_range(next_adt, anode, "DeviceTree", tree_range) < 0)
            goto bad_adt_range;
    }

    u64 new_sepfw[2] = {fw_base, sepfw[1]};
    if (chainload_set_adt_range(next_adt, anode, "SEPFW", new_sepfw) < 0)
        goto bad_adt_range;
    if (preoslog[1]) {
        preoslog[0] = fw_base + fw_preoslog_off;
        if (chainload_set_adt_range(next_adt, anode, "preoslog", preoslog) < 0)
            goto bad_adt_range;
    }
    /* Point the memory map at the moved boot arguments, where the ADT lists them. */
    u64 bootargs_map[2] = {fw_base + fw_bootargs_off, SZ_16K};
    if (adt_getprop(adt, anode, "BootArgs", NULL) &&
        chainload_set_adt_range(next_adt, anode, "BootArgs", bootargs_map) < 0)
        goto bad_adt_range;
    if (parked) {
        dc_cvac_range(parked, fw_size);
        sysop("dsb sy");
    }

    void *stub = new_image + copy_size;
    memcpy(stub, _chainload_stub_start, stub_size);
    dc_cvac_range(stub, stub_size);
    sysop("dsb sy");
    ic_ivau_range(stub, stub_size);
    sysop("dsb sy");
    sysop("isb");

    printf("chainload: image 0x%lx bytes; firmware block at 0x%lx; high-water 0x%lx\n", copy_size,
           fw_base, high_water);
    if (run_base)
        printf("chainload: image staged at 0x%lx runs its first 0x%lx bytes at 0x%lx\n", new_base,
               footprint, run_base);
    next_stage.entry = stub;
    next_stage.args[0] = fw_base + fw_bootargs_off;
    next_stage.args[1] = (u64)new_image;
    next_stage.args[2] = new_base;
    next_stage.args[3] = copy_size;
    next_stage.args[4] = new_base + CHAINLOAD_ENTRY_OFFSET;
    next_stage.restore_logo = false;
    return 0;

bad_adt_range:
    free(new_image);
    free(parked);
    return -1;
}

#ifdef CHAINLOADING

#ifndef ESP_STAGE2
extern char con_buf[];
extern size_t con_wp;
#define STAGE1_LOG_SIZE  SZ_16K
#define CON_BUFFER_BYTES 8192

/*
 * Keep Stage 1's console for the next stage: its ring, oldest line first, in a page taken off
 * the top of RAM (so the next stage never reuses it), named by chosen.aurora,stage1-log.
 */
static char stage1_log_var[64];
static char *stage1_log;

/* Copy the console ring, oldest line first. Run again right before the jump to the next stage. */
void stage1_log_flush(void)
{
    if (!stage1_log)
        return;
    size_t len = min(con_wp, (size_t)CON_BUFFER_BYTES);
    memset(stage1_log, 0, STAGE1_LOG_SIZE);
    for (size_t i = 0; i < len; i++)
        stage1_log[i] = con_buf[(con_wp - len + i) % CON_BUFFER_BYTES];
    dc_cvac_range(stage1_log, STAGE1_LOG_SIZE);
}

static void stage1_log_handoff(char **vars, size_t *var_cnt, size_t var_capacity)
{
    if (*var_cnt >= var_capacity)
        return;
    stage1_log = (char *)top_of_memory_alloc(STAGE1_LOG_SIZE);
    if (!stage1_log)
        return;
    stage1_log_flush();
    snprintf(stage1_log_var, sizeof(stage1_log_var), "chosen.aurora,stage1-log=0x%lx",
             (u64)stage1_log);
    vars[(*var_cnt)++] = stage1_log_var;
}
#endif

/*
 * A trial: an aura-try file at the ESP root names an image to boot once in place of the
 * configured one (chainload.rs). It is spent on the ESP before the image is read, so a trial
 * that never reaches Linux leaves the next boot on the configured path. The trial's stages run
 * under the trial watchdog (wdt.c); Linux finds the path in chosen/aurora,trial.
 */
static char trial_path[128];
static char trial_var[sizeof("chosen.aurora,trial=") + sizeof(trial_path)];

static bool chainload_start_trial(char **vars, size_t *var_cnt, size_t var_capacity)
{
    size_t capacity = cur_boot_args.revision == 1   ? CMDLINE_LENGTH_RV1
                      : cur_boot_args.revision == 2 ? CMDLINE_LENGTH_RV2
                                                    : CMDLINE_LENGTH_RV3;
    if (*var_cnt >= var_capacity ||
        !boot_options_append(cur_boot_args.rv3.cmdline, capacity, "aura-trial")) {
        printf("chainload: no room to mark the trial for the next stage\n");
        return false;
    }
    snprintf(trial_var, sizeof(trial_var), "chosen.aurora,trial=%s", trial_path);
    vars[(*var_cnt)++] = trial_var;
    boot_options_set_trial(true);
    printf("chainload: trial boot of %s\n", trial_path);
    return true;
}

int chainload_load(const char *spec, char **vars, size_t *var_cnt, size_t var_capacity)
{
#if defined(DEBUGUSB_PROXY) && !defined(ESP_STAGE2)
    static char host_window_var[] = "aura.host-window=done";
#endif
    void *image;
    size_t size;
    int ret;

#ifndef ESP_STAGE2
    /* The last boot stopped before an OS: give a host the chance to replace this Stage 2. */
    if (stage1_recovery_window())
        return next_stage.entry ? 0 : -1;
#endif
    if (!nvme_init()) {
        printf("chainload: NVME init failed\n");
        return -1;
    }

    /* Post-M4: the SART entries the next stage starts ANS again with, after nvme_shutdown(). */
    char *sart_var = nvme_sart_var();
    if (sart_var) {
        bool present = false;
        for (size_t i = 0; i < *var_cnt; i++)
            present |= vars[i] == sart_var;
        if (!present) {
            if (*var_cnt >= var_capacity) {
                printf("chainload: no room for nvme.sart\n");
                nvme_shutdown();
                return -1;
            }
            vars[(*var_cnt)++] = sart_var;
        }
    }

    bool proxy_request = false;
    /* Only the first aura stage of a boot takes a trial: later ones already run in it. */
    bool may_trial = !aura_trial_enabled();
    trial_path[0] = 0;
    ret = rust_load_image(spec, &image, &size, &proxy_request, may_trial ? trial_path : NULL,
                          sizeof(trial_path));
    /*
     * A command that timed out fails every later one of this NVMe session, so one failure
     * (spending an aura-try marker writes and flushes the ESP) also lost the configured image.
     * Start NVMe again once and read that image, leaving any marker alone, rather than stopping.
     */
    if (ret != 0 && nvme_io_failed()) {
        printf("chainload: NVMe I/O failed; restarting NVMe for the configured image\n");
        may_trial = false;
        trial_path[0] = 0;
        if (nvme_shutdown() && nvme_init())
            ret = rust_load_image(spec, &image, &size, NULL, NULL, 0);
    }
#ifndef ESP_STAGE2
    /*
     * An aura-proxy file at the ESP root asks Stage 1 to wait for a host. Hand NVMe
     * back first so a host that takes the window finds the same state as before any load,
     * and read the Stage 2 again only if nobody did.
     */
    if (ret == 0 && proxy_request && !stage1_chained) {
        rust_free_image(image, size);
        if (!nvme_shutdown()) {
            next_stage.entry = NULL;
            return -1;
        }
        if (stage1_proxy_request_window())
            return next_stage.entry ? 0 : -1;
        if (!nvme_init()) {
            printf("chainload: NVME init failed\n");
            return -1;
        }
        /* The trial, if any, is spent already: the image it named is read again directly. */
        ret = rust_load_image(spec, &image, &size, NULL, may_trial ? trial_path : NULL,
                              sizeof(trial_path));
    }
#ifdef DEBUGUSB_PROXY
    /* The next aura stage skips its debugusb window when this one offered a window already. */
    if (ret == 0 && stage1_window_offered() && *var_cnt < var_capacity)
        vars[(*var_cnt)++] = host_window_var;
    /*
     * A debugusb host reading the DockChannel console makes this a debug boot: the OS keeps the
     * debug UART as its console (kboot.c, dt_set_stdout_path). Stage 2 is built without
     * DEBUGUSB_PROXY, so the boot arguments carry it there, as they carry aura-trial.
     */
    if (ret == 0 && !aura_console_enabled() && dockchannel_uart_host_seen()) {
        size_t capacity = cur_boot_args.revision == 1   ? CMDLINE_LENGTH_RV1
                          : cur_boot_args.revision == 2 ? CMDLINE_LENGTH_RV2
                                                        : CMDLINE_LENGTH_RV3;
        if (boot_options_append(cur_boot_args.rv3.cmdline, capacity, "aura-console")) {
            boot_options_set_console(true);
            printf("chainload: debugusb host, the OS keeps the debug console\n");
        } else {
            printf("chainload: no room to pass aura-console on\n");
        }
    }
#endif
    if (ret == 0)
        stage1_log_handoff(vars, var_cnt, var_capacity);
#endif
    /*
     * T8152: the next stage cannot stop a session it only adopts, so this one hands its display
     * controllers over now, as it would to Linux (display_shutdown(DCP_HANDOFF)).
     */
    if (ret == 0 && chip_id == T8152 && display_shutdown(DCP_HANDOFF) < 0)
        printf("chainload: a DCP endpoint did not stop; the next stage is told which\n");
    /* A display handed on live: the next stage adopts it, its live AFK endpoints too. */
    char *afk_var = display_afk_var();
    if (ret == 0 && afk_var) {
        if (*var_cnt < var_capacity)
            vars[(*var_cnt)++] = afk_var;
        else
            printf("chainload: no room for %s\n", afk_var);
    }
    char saved_cmdline[CMDLINE_LENGTH_RV3];
    size_t saved_vars = *var_cnt;
    bool trial = false;
    if (ret == 0 && trial_path[0]) {
        memcpy(saved_cmdline, cur_boot_args.rv3.cmdline, sizeof(saved_cmdline));
        trial = chainload_start_trial(vars, var_cnt, var_capacity);
        if (!trial) {
            rust_free_image(image, size);
            ret = rust_load_image(spec, &image, &size, NULL, NULL, 0);
        }
    }
    if (ret == 0) {
        size_t run_size = size;
        ret = secboot_check_chainload(image, &run_size, vars, var_cnt, var_capacity);
        if (ret == 0)
            ret = chainload_image(image, run_size, vars, *var_cnt);
        rust_free_image(image, size);
    }
    if (ret && trial) {
        /* The trial image cannot be placed: boot the configured one, as the next boot would. */
        memcpy(cur_boot_args.rv3.cmdline, saved_cmdline, sizeof(saved_cmdline));
        *var_cnt = saved_vars;
        boot_options_set_trial(false);
        printf("chainload: trial image failed, loading the configured one\n");
        ret = rust_load_image(spec, &image, &size, NULL, NULL, 0);
        if (ret == 0) {
            size_t run_size = size;
            ret = secboot_check_chainload(image, &run_size, vars, var_cnt, var_capacity);
            if (ret == 0)
                ret = chainload_image(image, run_size, vars, *var_cnt);
            rust_free_image(image, size);
        }
    }
    if (!nvme_shutdown()) {
        next_stage.entry = NULL;
        return -1;
    }
    if (ret)
        next_stage.entry = NULL;
    return ret;
}

#else

int chainload_load(const char *spec, char **vars, size_t *var_cnt, size_t var_capacity)
{
    UNUSED(spec);
    UNUSED(vars);
    UNUSED(var_cnt);
    UNUSED(var_capacity);

    printf("Chainloading files not supported in this build!\n");
    return -1;
}

#endif
