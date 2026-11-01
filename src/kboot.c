/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include <stdint.h>

#include "kboot.h"
#include "adt.h"
#include "asc.h"
#include "assert.h"
#include "clk.h"
#include "dapf.h"
#include "devicetree.h"
#include "display.h"
#include "exception.h"
#include "firmware.h"
#include "heapblock.h"
#include "iodev.h"
#include "isp.h"
#include "kboot_atc.h"
#include "kboot_display_facts.h"
#include "kboot_gpu_t8122.h"
#include "kboot_pmp_t8122.h"
#include "kboot_t6031.h"
#include "malloc.h"
#include "mcc.h"
#include "memory.h"
#include "monitor.h"
#include "pcie.h"
#include "pmgr.h"
#include "rtkit.h"
#include "sart.h"
#include "sep.h"
#include "sio.h"
#include "smp.h"
#include "tunables.h"
#include "types.h"
#include "usb.h"
#include "utils.h"
#include "vsprintf.h"
#include "xnuboot.h"

#include "libfdt/libfdt.h"
#include "tinf/tinf.h"

#define MAX_CHOSEN_PARAMS 16
#define MAX_UBOOT_CONFIGS 4

#define MAX_ATC_DEVS 8
#define MAX_CIO_DEVS 8

#define MAX_DISP_MAPPINGS 8

#define MAX_MEM_REGIONS 32

static void *dt = NULL;
static int dt_bufsize = 0;
static void *initrd_start = NULL;
static size_t initrd_size = 0;
static char *chosen_params[MAX_CHOSEN_PARAMS][2];
/* Entries dt_set_m3_defaults() added; any other kboot_set_chosen() of the entry clears its flag */
static bool chosen_is_default[MAX_CHOSEN_PARAMS];
/*
 * A config line was lost this session: a kboot_set_chosen() failed, or the payload scan lost one
 * (kboot_set_config_lost()). An owner's chosen.<switch>=0 may be among the lost.
 */
static bool chosen_lost;
static char *uboot_config[MAX_UBOOT_CONFIGS][2];

extern const char *const aura_version;
extern u64 relocated_from;

int dt_set_gpu(void *dt);
int dt_set_display_handoff(void *dt);

u64 kboot_ram_base(void)
{
    return cur_boot_args.phys_base;
}

u64 kboot_ram_size(void)
{
    return cur_boot_args.mem_size;
}

u64 kboot_fb_base(void)
{
    return cur_boot_args.video.base;
}

void kboot_loaded_file_part(u64 *base, u64 *size)
{
    *base = relocated_from ? relocated_from : (u64)_base;
    *size = (u64)_file_end - (u64)_base;
}

#define DT_ALIGN 16384

#define bail(...)                                                                                  \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        return -1;                                                                                 \
    } while (0)

#define bail_cleanup(...)                                                                          \
    do {                                                                                           \
        printf(__VA_ARGS__);                                                                       \
        ret = -1;                                                                                  \
        goto err;                                                                                  \
    } while (0)

void get_notchless_fb(u64 *fb_base, u64 *fb_height)
{
    *fb_base = cur_boot_args.video.base;
    *fb_height = cur_boot_args.video.height;

    int node = adt_path_offset(adt, "/product");

    if (node < 0) {
        printf("FDT: /product node not found\n");
        return;
    }

    u32 val;

    if ((ADT_GETPROP(adt, node, "partially-occluded-display", &val) < 0 || !val) &&
        (chip_id != T8015 || (board_id != 0x6 && board_id != 0xe))) {
        printf("FDT: No notch detected\n");
        return;
    }

    u64 hfrac = cur_boot_args.video.height * 16 / cur_boot_args.video.width;
    u64 new_height = cur_boot_args.video.width * hfrac / 16;
    if (chip_id == T8015 && (board_id == 0x6 || board_id == 0xe))
        new_height = 2346;

    if (new_height == cur_boot_args.video.height) {
        printf("FDT: Notch detected, but display aspect is already 16:%lu?\n", hfrac);
        return;
    }

    u64 offset = cur_boot_args.video.height - new_height;

    printf("display: Hiding notch, %lux%lu -> %lux%lu (+%lu, 16:%lu)\n", cur_boot_args.video.width,
           cur_boot_args.video.height, cur_boot_args.video.width, new_height, offset, hfrac);

    *fb_base += cur_boot_args.video.stride * offset;
    *fb_height = new_height;
}

/*
 * chosen.asahi,show-notch=1 (aura config): the OS drives the full panel, notch rows
 * included (appledrm.show_notch=1). Keep iBoot's full framebuffer then, so
 * the boot framebuffer covers the same rows as the display driver later does and early
 * users (fbcon, Plymouth) do not shift by the notch height at the handoff.
 */
static bool kboot_show_notch(void)
{
    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0])
            break;
        if (!strcmp(chosen_params[i][0], "asahi,show-notch"))
            return chosen_params[i][1] && !strcmp(chosen_params[i][1], "1");
    }
    return false;
}

static int dt_set_rng_seed_sep(int node)
{
    u64 kaslr_seed;
    uint8_t rng_seed[128]; // same size used by Linux for kexec

    if (sep_get_random(&kaslr_seed, sizeof(kaslr_seed)) != sizeof(kaslr_seed))
        bail("SEP: couldn't get enough random bytes for KASLR seed\n");
    if (sep_get_random(rng_seed, sizeof(rng_seed)) != sizeof(rng_seed))
        bail("SEP: couldn't get enough random bytes for RNG seed\n");

    if (fdt_setprop_u64(dt, node, "kaslr-seed", kaslr_seed))
        bail("FDT: couldn't set kaslr-seed\n");
    if (fdt_setprop(dt, node, "rng-seed", rng_seed, sizeof(rng_seed)))
        bail("FDT: couldn't set rng-seed\n");

    printf("FDT: Passing %ld bytes of KASLR seed and %ld bytes of random seed\n",
           sizeof(kaslr_seed), sizeof(rng_seed));

    return 0;
}

static int dt_set_rng_seed_adt(int node)
{
    int anode = adt_path_offset(adt, "/chosen");

    if (anode < 0)
        bail("ADT: /chosen not found\n");

    const uint8_t *random_seed;
    u32 seed_length;

    random_seed = adt_getprop(adt, anode, "random-seed", &seed_length);
    if (random_seed) {
        printf("ADT: %d bytes of random seed available\n", seed_length);

        if (seed_length >= sizeof(u64)) {
            u64 kaslr_seed;

            memcpy(&kaslr_seed, random_seed, sizeof(kaslr_seed));

            // Ideally we would throw away the kaslr_seed part of random_seed
            // and avoid reusing it. However, Linux wants 64 bytes of bootloader
            // random seed to consider its CRNG initialized, which is exactly
            // how much iBoot gives us. This probably doesn't matter, since
            // that entropy is going to get shuffled together and Linux makes
            // sure to clear the FDT randomness after using it anyway, but just
            // in case let's mix in a few bits from our own KASLR base to make
            // kaslr_seed unique.

            kaslr_seed ^= (u64)cur_boot_args.virt_base;

            if (fdt_setprop_u64(dt, node, "kaslr-seed", kaslr_seed))
                bail("FDT: couldn't set kaslr-seed\n");

            printf("FDT: KASLR seed initialized\n");
        } else {
            printf("ADT: not enough random data for kaslr-seed\n");
        }

        if (seed_length) {
            if (fdt_setprop(dt, node, "rng-seed", random_seed, seed_length))
                bail("FDT: couldn't set rng-seed\n");

            printf("FDT: Passing %d bytes of random seed\n", seed_length);
        }
    } else {
        printf("ADT: no random-seed available!\n");
    }

    return 0;
}

static int dt_set_fb(void)
{
    if (!cur_boot_args.video.base) {
        printf("FDT: Framebuffer unavailable, skipping framebuffer initialization");
        return 0;
    }

    int fb = fdt_path_offset(dt, "/chosen/framebuffer");

    if (fb < 0) {
        printf("FDT: No framebuffer found\n");
        return 0;
    }

    u64 fb_base, fb_height;
    get_notchless_fb(&fb_base, &fb_height);
    u64 notch_height = cur_boot_args.video.height - fb_height;
    if (notch_height && kboot_show_notch()) {
        printf("display: keeping the notch rows (chosen.asahi,show-notch=1), %lux%lu\n",
               cur_boot_args.video.width, cur_boot_args.video.height);
        fb_base = cur_boot_args.video.base;
        fb_height = cur_boot_args.video.height;
    }
    u64 fb_size = cur_boot_args.video.stride * fb_height;
    u64 fbreg[2] = {cpu_to_fdt64(fb_base), cpu_to_fdt64(fb_size)};
    char fbname[32];

    snprintf(fbname, sizeof(fbname), "framebuffer@%lx", fb_base);

    if (fdt_setprop(dt, fb, "reg", fbreg, sizeof(fbreg)))
        bail("FDT: couldn't set framebuffer.reg property\n");

    if (fdt_set_name(dt, fb, fbname))
        bail("FDT: couldn't set framebuffer name\n");

    if (fdt_setprop_u32(dt, fb, "width", cur_boot_args.video.width))
        bail("FDT: couldn't set framebuffer width\n");

    if (fdt_setprop_u32(dt, fb, "height", fb_height))
        bail("FDT: couldn't set framebuffer height\n");

    if (fdt_setprop_u32(dt, fb, "stride", cur_boot_args.video.stride))
        bail("FDT: couldn't set framebuffer stride\n");

    const char *format = NULL;

    switch (cur_boot_args.video.depth & 0xff) {
        case 32:
            format = "x8r8g8b8";
            break;
        case 30:
            format = "x2r10g10b10";
            break;
        case 16:
            format = "r5g6b5";
            break;
        default:
            printf("FDT: unsupported fb depth %lu, not enabling\n", cur_boot_args.video.depth);
            return 0; // Do not error out, but don't set the FB
    }

    if (fdt_setprop_string(dt, fb, "format", format))
        bail("FDT: couldn't set framebuffer format\n");

    fdt_delprop(dt, fb, "status"); // may fail if it does not exist

    printf("FDT: %s base 0x%lx size 0x%lx\n", fbname, fb_base, fb_size);

    // We do not need to reserve the framebuffer, as it will be excluded from the usable RAM
    // range already.

    // save notch height in the dcp node if present (also when the notch rows are kept,
    // so that a display driver hiding the notch still can)
    if (notch_height) {
        int dcp = fdt_path_offset(dt, "dcp");
        if (dcp >= 0)
            if (fdt_appendprop_u32(dt, dcp, "apple,notch-height", notch_height))
                printf("FDT: couldn't set apple,notch-height\n");
    }

    return 0;
}

static bool dt_bootarg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static int dt_set_t8140_wfx_args(int node)
{
    static const char token[] = "arm64.nowfxt";
    int len = 0;
    const char *args = fdt_getprop(dt, node, "bootargs", &len);
    size_t size = 0;

    if (args) {
        if (len <= 0 || !memchr(args, 0, len))
            bail("FDT: bootargs is not NUL-terminated\n");
        size = strnlen(args, len);
        for (size_t i = 0; i < size;) {
            while (i < size && dt_bootarg_space(args[i]))
                i++;
            size_t start = i;
            while (i < size && !dt_bootarg_space(args[i]))
                i++;
            if (i - start == sizeof(token) - 1 && !memcmp(args + start, token, sizeof(token) - 1))
                return 0;
        }
        while (size && dt_bootarg_space(args[size - 1]))
            size--;
    }

    if (size > SIZE_MAX - sizeof(token) - 1)
        return -1;
    char *updated = malloc(size + sizeof(token) + (size != 0));
    if (!updated)
        return -1;
    if (size)
        memcpy(updated, args, size);
    if (size)
        updated[size] = ' ';
    memcpy(updated + size + (size != 0), token, sizeof(token));
    int ret = fdt_setprop_string(dt, node, "bootargs", updated);
    free(updated);
    if (ret)
        bail("FDT: couldn't set T8140 bootargs\n");
    return 0;
}

/*
 * M3 graphics on by default, with no boot.bin config line. The M3 Airs (J613, J615), the M3 Pros
 * (J514S, J516S) and the 16-inch M3 Max with the 40-core GPU (J516C) hand the internal display and
 * the GPU to the OS on every boot, as M1 and M2 Macs do. Each switch below
 * is put in /chosen as "1" unless a config line set it
 * already, so chosen.<switch>=0 in /etc/m1n1.conf (update-m1n1 copies it into boot.bin) still
 * turns that one off, and chosen.asahi,m3-defaults=0 adds none of them (an owner's own lines,
 * an =1 included, stay as written). If a config line was
 * lost this session (a full table, a failed allocation, or one the payload scan lost), an owner's
 * =0 may be the one lost while an earlier =1 still stands, so these Macs hand no graphics over:
 * no default goes in, and every graphics line of the board is dropped. They still boot, on the
 * boot framebuffer. Only the switch
 * changes: every check behind it (board, firmware, identity, reservations, PMP values, resources)
 * still runs, and a failed check still leaves the boot framebuffer and a disabled GPU node.
 *
 * The Air switches are the recipe the boot package wrote on these Macs: the display handoff
 * (which also passes the PMP values), the GPU, and the stand-in power model with this Mac's
 * leakage fuse. It wrote the GPU lines only beside a display line of 1, so an owner's
 * chosen.asahi,t8122-dcp=0 keeps the default GPU switches off as well; an owner's own GPU line
 * still counts. chosen.asahi,t8122-dcpext stays off. The Pro switches are its display handoff
 * and GPU; its external display handoffs (t6030-dcpext, t6030-dispext) keep their own lines.
 *
 * The J516C switches are its display handoff and GPU, each on its own as on the Pro: an owner's
 * chosen.asahi,t6031-gpu=0 keeps the GPU off and the display default still goes in, and
 * chosen.asahi,t6031-dcp=0 the other way round. The kernel decides whether it takes the display
 * over; one that does not leaves it on the boot framebuffer. The J514C (a T6031 board the
 * kernel's GPU driver refuses) and the T6034 Maxes (J514M, J516M) are not touched, nor is any
 * other board.
 */
struct m3_default_board {
    const char *soc;
    const char *const *boards;
    const char *const *switches;
    /* The switches after the first get a default only while the first is 1 or unset */
    bool rest_need_first;
    /* Prefixes of the board's graphics lines, all dropped when a config line was lost */
    const char *const *graphics;
};

static const char *const m3_air_boards[] = {"apple,j613", "apple,j615", NULL};
static const char *const m3_air_switches[] = {"asahi,t8122-dcp", "asahi,t8122-gpu",
                                              "asahi,t8122-gpu-power-standin",
                                              "asahi,t8122-gpu-fuse-leakage", NULL};
static const char *const m3_pro_boards[] = {"apple,j514s", "apple,j516s", NULL};
static const char *const m3_pro_switches[] = {"asahi,t6030-dcp", "asahi,t6030-gpu", NULL};
static const char *const m3_max_boards[] = {"apple,j516c", NULL};
static const char *const m3_max_switches[] = {"asahi,t6031-dcp", "asahi,t6031-gpu", NULL};
static const char *const m3_air_graphics[] = {"asahi,t8122-", "asahi,j613-", "asahi,j615-", NULL};
static const char *const m3_pro_graphics[] = {"asahi,t6030-", NULL};
static const char *const m3_max_graphics[] = {"asahi,t6031-", NULL};

static const struct m3_default_board m3_default_boards[] = {
    {"apple,t8122", m3_air_boards, m3_air_switches, true, m3_air_graphics},
    {"apple,t6030", m3_pro_boards, m3_pro_switches, false, m3_pro_graphics},
    {"apple,t6031", m3_max_boards, m3_max_switches, false, m3_max_graphics},
};

#define M3_DEFAULTS_SWITCH "asahi,m3-defaults"

/* The config value of chosen.<name>, NULL when no config line set it */
static const char *kboot_chosen_param(const char *name)
{
    for (int i = 0; i < MAX_CHOSEN_PARAMS && chosen_params[i][0]; i++)
        if (!strcmp(chosen_params[i][0], name))
            return chosen_params[i][1] ? chosen_params[i][1] : "";
    return NULL;
}

/*
 * A later preparation (the proxy can prepare again, with another device tree or after changing
 * the config) first removes the defaults an earlier one added, so an earlier default never
 * answers for a later config or board. An owner's later value for a switch clears its flag and
 * is kept.
 */
static void dt_drop_m3_defaults(void)
{
    int i = 0;
    while (i < MAX_CHOSEN_PARAMS && chosen_params[i][0]) {
        if (chosen_is_default[i])
            kboot_delete_chosen(chosen_params[i][0]); /* the next entry moves to i */
        else
            i++;
    }
}

/* Every graphics line of the board, the owner's included */
static void dt_drop_m3_graphics(const struct m3_default_board *e)
{
    for (const char *const *g = e->graphics; *g; g++) {
        int i = 0;
        while (i < MAX_CHOSEN_PARAMS && chosen_params[i][0]) {
            if (!strncmp(chosen_params[i][0], *g, strlen(*g)))
                kboot_delete_chosen(chosen_params[i][0]); /* the next entry moves to i */
            else
                i++;
        }
    }
}

static int dt_set_m3_defaults(void)
{
    dt_drop_m3_defaults();
    for (size_t i = 0; i < sizeof(m3_default_boards) / sizeof(m3_default_boards[0]); i++) {
        const struct m3_default_board *e = &m3_default_boards[i];
        const char *board = NULL;

        if (fdt_node_check_compatible(dt, 0, e->soc))
            continue;
        for (const char *const *b = e->boards; *b; b++) {
            if (!fdt_node_check_compatible(dt, 0, *b)) {
                board = *b;
                break;
            }
        }
        if (!board)
            continue;

        if (chosen_lost) {
            dt_drop_m3_graphics(e);
            printf("kboot: %s: a config line was lost, so an owner's =0 may be missing: no "
                   "graphics handoff\n",
                   board);
            return 0;
        }
        const char *all = kboot_chosen_param(M3_DEFAULTS_SWITCH);
        if (all && strcmp(all, "1")) {
            printf("kboot: %s: graphics defaults off (chosen.%s=%s)\n", board, M3_DEFAULTS_SWITCH,
                   all);
            return 0;
        }
        /*
         * The Air's GPU only beside its display, as the boot package's recipe had it: an owner's
         * chosen.asahi,t8122-dcp other than 1 keeps the default GPU switches off too. The owner's
         * own lines, a GPU line included, stay as written.
         */
        int count = 0;
        while (e->switches[count])
            count++;
        const char *first = kboot_chosen_param(e->switches[0]);
        if (e->rest_need_first && first && strcmp(first, "1")) {
            printf("kboot: %s: chosen.%s=%s: no default switch beside it\n", board, e->switches[0],
                   first);
            count = 1;
        }
        /* All of a board's missing switches go in, or none: never half of the Air recipe */
        int missing = 0, used = 0;
        for (int k = 0; k < count; k++) {
            const char *sw = e->switches[k], *set = kboot_chosen_param(sw);
            if (set)
                printf("kboot: %s: chosen.%s=%s from the config\n", board, sw, set);
            else
                missing++;
        }
        while (used < MAX_CHOSEN_PARAMS && chosen_params[used][0])
            used++;
        if (missing > MAX_CHOSEN_PARAMS - used) {
            printf("kboot: %s: no room for %d default switches (%d free): graphics stay as the "
                   "config sets them\n",
                   board, missing, MAX_CHOSEN_PARAMS - used);
            return 0;
        }
        for (int k = 0; k < count; k++) {
            const char *sw = e->switches[k];
            if (kboot_chosen_param(sw))
                continue;
            int idx = kboot_set_chosen(sw, "1");
            if (idx < 0) {
                dt_drop_m3_defaults();
                printf("kboot: %s: could not add the default chosen.%s=1: graphics stay as the "
                       "config sets them\n",
                       board, sw);
                return 0;
            }
            chosen_is_default[idx] = true;
            printf("kboot: %s: chosen.%s=1 by default (chosen.%s=0 turns it off)\n", board, sw, sw);
        }
        return 0;
    }
    return 0;
}

static int dt_set_chosen(void)
{

    int node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        bail("FDT: /chosen node not found in devtree\n");

    for (int i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0])
            break;

        const char *name = chosen_params[i][0];
        const char *value = chosen_params[i][1];
        if (fdt_setprop(dt, node, name, value, strlen(value) + 1) < 0)
            bail("FDT: couldn't set chosen.%s property\n", name);
        printf("FDT: set chosen.%s\n", name);
    }

    if (chip_id == T8140 && dt_set_t8140_wfx_args(node))
        return -1;

    if (initrd_start && initrd_size) {
        if (fdt_setprop_u64(dt, node, "linux,initrd-start", (u64)initrd_start))
            bail("FDT: couldn't set chosen.linux,initrd-start property\n");

        u64 end = ((u64)initrd_start) + initrd_size;
        if (fdt_setprop_u64(dt, node, "linux,initrd-end", end))
            bail("FDT: couldn't set chosen.linux,initrd-end property\n");

        if (fdt_add_mem_rsv(dt, (u64)initrd_start, initrd_size))
            bail("FDT: couldn't add reservation for the initrd\n");

        printf("FDT: initrd at %p size 0x%lx\n", initrd_start, initrd_size);
    }

    if (dt_set_fb())
        return -1;

    node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        bail("FDT: /chosen node not found in devtree\n");

    if (is_mac) {
        if (fdt_setprop(dt, node, "asahi,iboot1-version", system_firmware.iboot,
                        strlen(system_firmware.iboot) + 1))
            bail("FDT: couldn't set asahi,iboot1-version\n");

        if (fdt_setprop(dt, node, "asahi,system-fw-version", system_firmware.string,
                        strlen(system_firmware.string) + 1))
            bail("FDT: couldn't set asahi,system-fw-version\n");
    }

    if (fdt_setprop(dt, node, "asahi,iboot2-version", os_firmware.iboot,
                    strlen(os_firmware.iboot) + 1))
        bail("FDT: couldn't set asahi,iboot2-version\n");

    if (fdt_setprop(dt, node, "asahi,os-fw-version", os_firmware.string,
                    strlen(os_firmware.string) + 1))
        bail("FDT: couldn't set asahi,os-fw-version\n");

    if (fdt_setprop(dt, node, "asahi,m1n1-stage2-version", aura_version, strlen(aura_version) + 1))
        bail("FDT: couldn't set asahi,m1n1-stage2-version\n");

    if (adt) {
        int anode = adt_path_offset(adt, "/chosen");
        if (anode >= 0) {
            u32 uuid_len = 0;
            const void *prop = adt_getprop(adt, anode, "apfs-preboot-uuid", &uuid_len);
            if (!prop)
                prop = adt_getprop(adt, anode, "boot-uuid", &uuid_len);

            if (prop && uuid_len > 0) {
                char uuid_str[37];
                if (uuid_len == 16) {
                    const u8 *b = prop;
                    snprintf(uuid_str, sizeof(uuid_str),
                             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                             b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10],
                             b[11], b[12], b[13], b[14], b[15]);
                    if (fdt_setprop(dt, node, "apfs-preboot-uuid", uuid_str, sizeof(uuid_str)) < 0)
                        printf("FDT: warning: couldn't set apfs-preboot-uuid\n");
                    else
                        printf("FDT: apfs-preboot-uuid = '%s'\n", uuid_str);
                } else if (uuid_len == 36 || uuid_len == 37) {
                    char str_buf[37];
                    memcpy(str_buf, prop, 36);
                    str_buf[36] = '\0';
                    if (fdt_setprop(dt, node, "apfs-preboot-uuid", str_buf, sizeof(str_buf)) < 0)
                        printf("FDT: warning: couldn't set apfs-preboot-uuid\n");
                    else
                        printf("FDT: apfs-preboot-uuid = '%s'\n", str_buf);
                }
            }
        }
    }

    /*
     * On M2 (T8112, T602x) and later SoCs, iBoot establishes hardware warm
     * registration of the Secure Enclave (SEP) and Touch ID sensor before
     * handoff. Querying the SEP ASC mailbox in aura desynchronizes this
     * state, causing the SEP firmware to refuse subsequent biometric commands.
     * M1 SoCs (T8103, T600x) rely on cold boot and do not have this restriction.
     */
    bool sep_warm_active =
        (chip_id != T8103 && chip_id != T6000 && chip_id != T6001 && chip_id != T6002);

    if (sep_warm_active) {
        printf("SEP: Preserving iBoot warm registration; seeding RNG from ADT\n");
        if (dt_set_rng_seed_adt(node))
            bail("FDT: couldn't set random seed");
    } else if (dt_set_rng_seed_sep(node)) {
        if (dt_set_rng_seed_adt(node))
            bail("FDT: couldn't set random seed");
    }

    /*
     * Upstream adds these on every chip with locked Apple system registers,
     * for kernels that cannot survive WFI state loss. Aurora's kernels for
     * T8132, T6040 and T6041 boot without them, and idle=nop would keep every
     * idle CPU spinning. T8140 adds only arm64.nowfxt, in dt_set_wfx_args().
     */
    if (!cpu_features->apple_sysregs_unlocked && chip_id != T8140 && chip_id != T8132 &&
        chip_id != T6040 && chip_id != T6041) {
        /*
         * M4 and later lose architectural state in WFI and WFIT. Add the
         * bootargs to prevent Linux from crashing in the idle loop and delay
         * function.
         */
        const char to_append_wfi[] = " idle=nop arm64.nowfxt";
        /* The T8152 kernel preserves the state its cores lose in WFI. */
        const char *to_append = chip_id == T8152 ? " arm64.nowfxt" : to_append_wfi;
        printf("FDT: appending to bootargs:%s\n", to_append);

        /*
         * fdt_appendprop_string would create another property value:
         * "a", "b". But we want "ab", so manually create or grow the prop,
         * then overwrite the \0 of the existing string.
         */
        int oldlen, newlen;
        void *prop_data;

        if (!fdt_get_property(dt, node, "bootargs", &oldlen) || oldlen < 1)
            oldlen = 1;
        newlen = oldlen + strlen(to_append);

        if (fdt_setprop_placeholder(dt, node, "bootargs", newlen, &prop_data))
            bail("FDT: failed to append to 'bootargs' property\n");

        strncpy(prop_data + oldlen - 1, to_append, strlen(to_append) + 1);
    }

    return 0;
}

static int dt_set_speaker_safety(void)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return -1;

    for (int node = fdt_first_subnode(dt, chosen); node >= 0;) {
        int next = fdt_next_subnode(dt, node);
        const char *name = fdt_get_name(dt, node, NULL);
        if (!strncmp(name, "asahi-speaker-safety", 20) && (name[20] == 0 || name[20] == '@'))
            fdt_nop_node(dt, node);
        node = next;
    }

    if (chip_id != T8140)
        return 0;

    int audio = adt_path_offset(adt, "/product/audio");
    u32 acoustic_id;
    if (audio < 0 || ADT_GETPROP(adt, audio, "acoustic-id", &acoustic_id) != sizeof(acoustic_id) ||
        acoustic_id != 44)
        return 0;

    static const struct {
        const char *adt_name;
        const char *dt_name;
    } words[] =
        {
            {"enabledChannels", "asahi,adt-enabled-channels"},
            {"historyChannels", "asahi,adt-history-channels"},
            {"supportedChannels", "asahi,adt-supported-channels"},
            {"safety-offset-seed-us", "asahi,adt-safety-offset-seed-us"},
        },
      longs[] = {
          {"speaker-cpms-bgd_100ms", "asahi,adt-speaker-cpms-bgd-100ms-raw"},
          {"speaker-cpms-bgd_1s", "asahi,adt-speaker-cpms-bgd-1s-raw"},
          {"speaker-cpms-bgd_inst", "asahi,adt-speaker-cpms-instant-raw"},
      };
    u32 word_values[ARRAY_SIZE(words)];
    u64 long_values[ARRAY_SIZE(longs)];
    for (size_t i = 0; i < ARRAY_SIZE(words); i++)
        if (ADT_GETPROP(adt, audio, words[i].adt_name, &word_values[i]) != sizeof(u32))
            bail("ADT: incomplete speaker safety profile\n");
    for (size_t i = 0; i < ARRAY_SIZE(longs); i++)
        if (ADT_GETPROP(adt, audio, longs[i].adt_name, &long_values[i]) != sizeof(u64))
            bail("ADT: incomplete speaker safety profile\n");

    int node = fdt_add_subnode(dt, chosen, "asahi-speaker-safety");
    if (node < 0 || fdt_setprop_string(dt, node, "compatible", "asahi,j700-speaker-safety") ||
        fdt_setprop_u32(dt, node, "asahi,schema-version", 1) ||
        fdt_setprop_string(dt, node, "asahi,speaker-output-policy", "forbidden") ||
        fdt_setprop_string(dt, node, "asahi,cpms-value-encoding", "opaque-adt-u64") ||
        fdt_setprop_string(dt, node, "asahi,calibration-data-status", "not-provided") ||
        fdt_setprop_string(dt, node, "asahi,source", "adt:/product/audio") ||
        fdt_setprop_u32(dt, node, "asahi,adt-acoustic-id", acoustic_id))
        bail("FDT: could not create speaker safety profile\n");

    for (size_t i = 0; i < ARRAY_SIZE(words); i++)
        if (fdt_setprop_u32(dt, node, words[i].dt_name, word_values[i]))
            bail("FDT: could not set speaker safety profile\n");
    for (size_t i = 0; i < ARRAY_SIZE(longs); i++)
        if (fdt_setprop_u64(dt, node, longs[i].dt_name, long_values[i]))
            bail("FDT: could not set speaker safety profile\n");

    return 0;
}

static int dt_set_uboot_config(void)
{
    // return without modifying dt if no params are set
    if (!uboot_config[0][0])
        return 0;

    int root = fdt_path_offset(dt, "/");
    if (root < 0)
        bail("FDT: root node not found in devtree\n");

    int node = fdt_add_subnode(dt, root, "config");
    if (node < 0)
        bail("FDT: could not add /config node\n");

    for (int i = 0; i < MAX_UBOOT_CONFIGS; i++) {
        if (!uboot_config[i][0])
            break;

        const char *name = uboot_config[i][0];
        const char *value = uboot_config[i][1];
        if (fdt_setprop(dt, node, name, value, strlen(value) + 1) < 0)
            bail("FDT: couldn't set config.%s property\n", name);
        printf("FDT: /config/%s = '%s'\n", name, value);
    }

    return 0;
}

static int dt_set_memory(void)
{
    int anode = adt_path_offset(adt, "/chosen");

    if (anode < 0)
        bail("ADT: /chosen not found\n");

    u64 dram_base = 0, dram_size = 0;

    // iOS 12 and below seems to be missing a bunch of stuff...
    if (ADT_GETPROP(adt, anode, "dram-base", &dram_base) < 0) {
        printf("ADT: Failed to get dram-base\n");
        dram_base = 0x800000000;
    }

    if (ADT_GETPROP(adt, anode, "dram-size", &dram_size) < 0) {
        printf("ADT: Failed to get dram-size\n");
        dram_size = mem_size_actual;
    }

    // Tell the kernel our usable memory range. We cannot declare all of DRAM, and just reserve the
    // bottom and top, because the kernel would still map it (and just not use it), which breaks
    // ioremap (e.g. simplefb).

    u64 dram_min = cur_boot_args.phys_base;
    u64 dram_max = cur_boot_args.phys_base + cur_boot_args.mem_size;

    printf("FDT: DRAM at 0x%lx size 0x%lx\n", dram_base, dram_size);
    printf("FDT: Usable memory is 0x%lx..0x%lx (0x%lx)\n", dram_min, dram_max, dram_max - dram_min);

    struct {
        fdt64_t start;
        fdt64_t size;
    } memreg[MAX_MEM_REGIONS] = {{cpu_to_fdt64(dram_min), cpu_to_fdt64(dram_max - dram_min)}};
    int num_regions = 1;

    int resv_node = fdt_path_offset(dt, "/reserved-memory");
    if (resv_node < 0) {
        printf("FDT: '/reserved-memory' not found\n");
    } else {
        int node;

        /* Find all reserved memory nodes */
        fdt_for_each_subnode(node, dt, resv_node)
        {
            const char *name = fdt_get_name(dt, node, NULL);
            const char *status = fdt_getprop(dt, node, "status", NULL);
            if (status && !strcmp(status, "disabled"))
                continue;

            if (fdt_getprop(dt, node, "no-map", NULL))
                continue;

            const fdt64_t *reg = fdt_getprop(dt, node, "reg", NULL);
            if (!reg)
                continue;

            u64 resv_start = fdt64_ld(&reg[0]);
            u64 resv_len = fdt64_ld(&reg[1]);
            u64 resv_end = resv_start + resv_len;

            if (resv_start < dram_min) {
                if (resv_end > dram_min) {
                    bail("FDT: reserved-memory node %s intersects start of RAM (%lx..%lx "
                         "%lx..%lx)\n",
                         name, resv_start, resv_end, dram_min, dram_max);
                }
            } else if (resv_end > dram_max) {
                if (resv_start < dram_max) {
                    bail("FDT: reserved-memory node %s intersects end of RAM (%lx..%lx %lx..%lx)\n",
                         name, resv_start, resv_end, dram_min, dram_max);
                }
            } else {
                continue;
            }

            if ((resv_start | resv_len) & (get_page_size() - 1)) {
                bail("FDT: reserved-memory node %s not page-aligned, ignoring (%lx..%lx)\n", name,
                     resv_start, resv_end);
                continue;
            }

            printf("FDT: Adding reserved-memory node %s (%lx..%lx) to RAM map\n", name, resv_start,
                   resv_end);

            if (num_regions >= MAX_MEM_REGIONS) {
                bail("FDT: Out of memory regions for reserved-memory\n");
            }

            memreg[num_regions].start = cpu_to_fdt64(resv_start);
            memreg[num_regions++].size = cpu_to_fdt64(resv_len);
        }
    }

    int node = fdt_path_offset(dt, "/memory");
    if (node < 0)
        bail("FDT: /memory node not found in devtree\n");

    if (fdt_setprop(dt, node, "reg", memreg, sizeof(memreg[0]) * num_regions))
        bail("FDT: couldn't set memory.reg property\n");

    return 0;
}

static int dt_reserve_bootloader_handoff(void)
{
    int node;

    while ((node = fdt_node_offset_by_compatible(
                dt, -1, "linux-enablement-mac,resident-bootloader")) >= 0) {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len != 2 * (int)sizeof(*reg)) {
            printf("FDT: invalid resident bootloader range on %s\n", fdt_get_name(dt, node, NULL));
            return -1;
        }

        u64 start = fdt64_ld(&reg[0]);
        u64 size = fdt64_ld(&reg[1]);
        if (!size || (start | size) & (SZ_16K - 1)) {
            printf("FDT: unaligned resident bootloader range on %s\n",
                   fdt_get_name(dt, node, NULL));
            return -1;
        }

        if (fdt_add_mem_rsv(dt, start, size))
            return -1;

        printf("FDT: reserved described bootloader handoff 0x%lx..0x%lx\n", start, start + size);

        /*
         * This node is packaging metadata, not an OS reserved-memory region.
         * In particular it may cover the running kernel and initramfs, so a
         * no-map property would make the handoff image itself inaccessible.
         * Consume it after promoting the range to the FDT reservation map.
         */
        if (fdt_del_node(dt, node))
            return -1;
    }

    return 0;
}

static int dt_set_serial_number(void)
{

    int fdt_root = fdt_path_offset(dt, "/");
    int adt_root = adt_path_offset(adt, "/");

    if (fdt_root < 0)
        bail("FDT: could not open a handle to FDT root.\n");
    if (adt_root < 0)
        bail("ADT: could not open a handle to ADT root.\n");

    u32 sn_len;
    const char *serial_number = adt_getprop(adt, adt_root, "serial-number", &sn_len);
    if (fdt_setprop_string(dt, fdt_root, "serial-number", serial_number))
        bail("FDT: unable to set device serial number!\n");
    printf("FDT: reporting device serial number: %s\n", serial_number);

    return 0;
}

/*
 * SMBIOS values for U-Boot's sysinfo-smbios driver, which builds the firmware tables Linux reads
 * (/sys/firmware/dmi, dmidecode, systemd-hostnamed, udev's memory properties). Everything comes
 * from the ADT and the device tree, so it holds for every machine without a per-model table.
 */

/* SMBIOS 3.7 values used below */
#define SMBIOS_PROCESSOR_TYPE_CENTRAL  0x03
#define SMBIOS_PROCESSOR_FAMILY_EXT    0xfe
#define SMBIOS_PROCESSOR_FAMILY2_ARMV8 0x101
#define SMBIOS_PROCESSOR_FAMILY2_ARMV9 0x102
#define SMBIOS_PROCESSOR_STATUS_OK     0x41 /* socket populated, CPU enabled */
#define SMBIOS_PROCESSOR_UPGRADE_NONE  0x06
#define SMBIOS_MA_LOCATION_BOARD       0x03
#define SMBIOS_MA_USE_SYSTEM           0x03
#define SMBIOS_MA_ERRCORR_NONE         0x03
#define SMBIOS_MD_FF_ROW_OF_CHIPS      0x0b
#define SMBIOS_MD_TD_SYNCHRONOUS       0x0080
#define SMBIOS_MD_TECH_DRAM            0x03
#define SMBIOS_MD_OPMC_VOLATILE        0x0008
#define SMBIOS_BOARD_FEATURE_HOSTING   0x01
#define SMBIOS_BOARD_TYPE_MOTHERBOARD  0x0a
#define SMBIOS_STATE_SAFE              0x03
#define SMBIOS_SECURITY_NONE           0x03
#define SMBIOS_PROCESSOR_EXEC_PROTECT  0x0020
#define SMBIOS_PROCESSOR_VIRT          0x0040
#define SMBIOS_PROCESSOR_POWER_CONTROL 0x0080
#define SMBIOS_CACHE_L2_CONFIG         0x0381 /* level 2, enabled, internal, mode unknown */
#define SMBIOS_CACHE_UNKNOWN           0x02
#define SMBIOS_CACHE_SRAM_UNKNOWN      0x0002
#define SMBIOS_CACHE_TYPE_UNIFIED      0x05
#define SMBIOS_FW_STATE_ENABLED        0x04
#define SMBIOS_FW_UPDATABLE            0x0001 /* can be updated by software */

/* An ADT string property, or NULL if it is absent or empty. Values may be NUL padded. */
static const char *adt_string(int node, const char *name)
{
    u32 len;
    const char *s = adt_getprop(adt, node, name, &len);

    if (!s || !len || !s[0] || !memchr(s, 0, len))
        return NULL;
    return s;
}

static int dt_smbios_node(int parent, const char *name)
{
    int node = fdt_subnode_offset(dt, parent, name);

    if (node >= 0)
        return node;
    return fdt_add_subnode(dt, parent, name);
}

/* SMBIOS Memory Device type for the ADT's /chosen dram-type */
static u32 smbios_memory_type(const char *dram_type)
{
    if (!dram_type)
        return 0x02; /* Unknown */
    if (!strncmp(dram_type, "LPDDR5", 6))
        return 0x23;
    if (!strncmp(dram_type, "LPDDR4", 6))
        return 0x1e;
    if (!strncmp(dram_type, "LPDDR3", 6))
        return 0x1d;
    return 0x01; /* Other */
}

/*
 * Cores and their highest frequency, from the CPU nodes left after dt_set_cpus() and the OPP
 * tables they point at.
 */
static void dt_cpu_summary(u32 *cores, u32 *max_mhz)
{
    int cpus = fdt_path_offset(dt, "/cpus");
    int node;
    u64 max_hz = 0;

    *cores = 0;
    *max_mhz = 0;
    if (cpus < 0)
        return;

    fdt_for_each_subnode(node, dt, cpus)
    {
        const char *type = fdt_getprop(dt, node, "device_type", NULL);
        const char *status = fdt_getprop(dt, node, "status", NULL);
        const fdt32_t *opp_ph;
        int opp, entry;

        if (!type || strcmp(type, "cpu"))
            continue;
        if (status && strcmp(status, "okay") && strcmp(status, "ok"))
            continue;
        (*cores)++;

        opp_ph = fdt_getprop(dt, node, "operating-points-v2", NULL);
        if (!opp_ph)
            continue;
        opp = fdt_node_offset_by_phandle(dt, fdt32_ld(opp_ph));
        if (opp < 0)
            continue;
        fdt_for_each_subnode(entry, dt, opp)
        {
            int len;
            const fdt32_t *hz = fdt_getprop(dt, entry, "opp-hz", &len);
            u64 val;

            if (!hz || len < 4)
                continue;
            val = len >= 8 ? fdt64_ld((const fdt64_t *)hz) : fdt32_ld(hz);
            if (val > max_hz)
                max_hz = val;
        }
    }

    *max_mhz = max_hz / 1000000;
}

/* An ADT property holding a NUL padded string of known size, copied into buf */
static const char *adt_padded_string(int node, const char *name, char *buf, size_t size)
{
    u32 len;
    const char *s = adt_getprop(adt, node, name, &len);
    size_t n;

    if (!s || !len)
        return NULL;
    n = strnlen(s, min((size_t)len, size - 1));
    if (!n)
        return NULL;
    memcpy(buf, s, n);
    buf[n] = 0;
    return buf;
}

/* The highest data rate of the DRAM's operating points (apple,data-rate), in MT/s */
static u32 dt_dram_data_rate(void)
{
    int opp = fdt_node_offset_by_compatible(dt, -1, "operating-points-v2-apple-dram");
    int entry;
    u64 max_rate = 0;

    if (opp < 0)
        return 0;
    fdt_for_each_subnode(entry, dt, opp)
    {
        int len;
        const fdt64_t *rate = fdt_getprop(dt, entry, "apple,data-rate", &len);

        if (rate && len == 8 && fdt64_ld(rate) > max_rate)
            max_rate = fdt64_ld(rate);
    }
    return (max_rate + 500000) / 1000000;
}

/*
 * The CPUs' L2 caches from the device tree: their total size in KiB, and a description of the
 * instances, e.g. "L2 Cache (2x 16 MiB, 1x 4 MiB)". The caches of the clusters differ, and an
 * SMBIOS processor links one L2 structure, so it describes them together.
 */
static u32 dt_l2_summary(char *desc, size_t size)
{
    int cpus = fdt_path_offset(dt, "/cpus");
    u32 phandles[16], sizes[16], n = 0, total = 0;
    int node;

    if (cpus < 0)
        return 0;
    fdt_for_each_subnode(node, dt, cpus)
    {
        const char *type = fdt_getprop(dt, node, "device_type", NULL);
        const fdt32_t *next = fdt_getprop(dt, node, "next-level-cache", NULL);
        int cache, len;
        u32 ph, i;

        if (!type || strcmp(type, "cpu") || !next)
            continue;
        ph = fdt32_ld(next);
        for (i = 0; i < n && phandles[i] != ph; i++)
            ;
        if (i < n || n == ARRAY_SIZE(phandles))
            continue;
        cache = fdt_node_offset_by_phandle(dt, ph);
        if (cache < 0)
            continue;
        const fdt32_t *level = fdt_getprop(dt, cache, "cache-level", &len);
        const fdt32_t *bytes = fdt_getprop(dt, cache, "cache-size", &len);
        if (!level || fdt32_ld(level) != 2 || !bytes)
            continue;
        phandles[n] = ph;
        sizes[n++] = fdt32_ld(bytes) >> 10;
        total += fdt32_ld(bytes) >> 10;
    }
    if (!total)
        return 0;

    /* Group equal sizes, largest first */
    size_t pos = snprintf(desc, size, "L2 Cache (");
    for (u32 done = 0; done < n;) {
        u32 big = 0, count = 0;
        for (u32 i = 0; i < n; i++)
            if (sizes[i] > big)
                big = sizes[i];
        for (u32 i = 0; i < n; i++)
            if (sizes[i] == big) {
                sizes[i] = 0;
                count++;
            }
        done += count;
        if (pos < size)
            pos += snprintf(desc + pos, size - pos, "%s%ux %u %s", pos > 10 ? ", " : "", count,
                            big >= 1024 ? big / 1024 : big, big >= 1024 ? "MiB" : "KiB");
    }
    if (pos < size)
        snprintf(desc + pos, size - pos, ")");
    return total;
}

/* An SMBIOS cache size word: KiB, or 64 KiB units with bit 15 set when that does not fit */
static u32 smbios_cache_size16(u32 kib)
{
    if (kib < 0x8000)
        return kib;
    if (kib / 64 < 0x8000)
        return 0x8000 | (kib / 64);
    return 0xffff;
}

/* "2024" or "Nov 2024": the release date Apple and the device trees end a product name with */
static bool smbios_is_date(const char *s, size_t n)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    if (n == 8 && s[3] == ' ') {
        bool month = false;
        for (size_t i = 0; i < 36; i += 3)
            if (!strncmp(s, months + i, 3))
                month = true;
        if (!month)
            return false;
        s += 4;
        n = 4;
    }
    if (n != 4)
        return false;
    for (size_t i = 0; i < 4; i++)
        if (s[i] < '0' || s[i] > '9')
            return false;
    return true;
}

/*
 * The product name without its release date: "MacBook Pro (16-inch, M4 Pro, Nov 2024)" becomes
 * "MacBook Pro (16-inch, M4 Pro)", "Mac mini (2023)" becomes "Mac mini". When chip is given and a
 * date went, the chip takes its place, for names that carry a year instead of the chip:
 * "MacBook Pro (16-inch, 2021)" with "M1 Pro" becomes "MacBook Pro (16-inch, M1 Pro)".
 */
static void smbios_product_name(const char *in, const char *chip, char *out, size_t size)
{
    size_t len = strlen(in);
    const char *open = strrchr(in, '(');

    snprintf(out, size, "%s", in);
    if (!len || in[len - 1] != ')' || !open)
        return;

    /* The last item in the parentheses */
    const char *item = open + 1;
    for (const char *c = open + 1; c < in + len - 1; c++)
        if (c[0] == ',' && c[1] == ' ')
            item = c + 2;
    if (!smbios_is_date(item, in + len - 1 - item))
        return;
    /* A name that already carries the chip does not take it twice */
    if (chip && strstr(in, chip) && strstr(in, chip) < item)
        chip = NULL;

    if (item == open + 1) {
        /* Only the date in the parentheses: drop them and the space before */
        size_t keep = open - in;
        while (keep && in[keep - 1] == ' ')
            keep--;
        if (chip)
            snprintf(out, size, "%.*s (%s)", (int)keep, in, chip);
        else
            snprintf(out, size, "%.*s", (int)keep, in);
    } else {
        size_t keep = item - 2 - in;
        if (chip)
            snprintf(out, size, "%.*s, %s)", (int)keep, in, chip);
        else
            snprintf(out, size, "%.*s)", (int)keep, in);
    }
}

static int dt_set_smbios(void)
{
    int fdt_root = fdt_path_offset(dt, "/");
    int adt_root = adt_path_offset(adt, "/");
    int adt_product = adt_path_offset(adt, "/product");
    int adt_chosen = adt_path_offset(adt, "/chosen");
    int len, node, smbios_node;
    char buf[160];

    if (fdt_root < 0)
        bail("FDT: could not open a handle to FDT root.\n");
    if (adt_root < 0)
        bail("ADT: could not open a handle to ADT root.\n");

    node = dt_smbios_node(fdt_root, "smbios");
    if (node < 0)
        bail("FDT: failed to add node 'smbios' to  '/'\n");
    fdt_setprop_string(dt, node, "compatible", "u-boot,sysinfo-smbios");

    smbios_node = dt_smbios_node(node, "smbios");
    if (smbios_node < 0)
        bail("FDT: failed to add node 'smbios' to  '/smbios'\n");

    const char *manuf = adt_string(adt_root, "manufacturer");
    if (!manuf)
        bail("ADT: failed to get manufacturer property\n");
    /* The identifier, e.g. "Mac16,7" */
    const char *model = adt_string(adt_root, "model");
    if (!model)
        bail("ADT: failed to get model property\n");
    const char *serial = adt_string(adt_root, "serial-number");
    if (!serial)
        bail("ADT: failed to get serial-number property\n");
    /* Apple's name for the product, e.g. "MacBook Pro (16-inch, 2024)" */
    const char *product_name = adt_product >= 0 ? adt_string(adt_product, "product-name") : NULL;
    /* The chip, e.g. "Apple M4 Pro" */
    const char *soc_name = adt_product >= 0 ? adt_string(adt_product, "product-soc-name") : NULL;

    /*
     * Product Name: the device tree's model without the brand and the release date, which names
     * the machine with its chip, e.g. "MacBook Pro (16-inch, M4 Pro)". Without a model, Apple's
     * product name with the chip in place of the year. The identifier ("Mac16,7") is the SKU
     * Number.
     */
    const char *dt_model = fdt_getprop(dt, fdt_root, "model", &len);
    char name_buf[96];
    const char *name = NULL;
    if (dt_model && len > 1 && memchr(dt_model, 0, len)) {
        const char *space = strchr(manuf, ' ');
        size_t brand = space ? (size_t)(space - manuf) : strlen(manuf);
        const char *model_name = dt_model;
        if (!strncmp(model_name, manuf, brand) && model_name[brand] == ' ')
            model_name += brand + 1;
        smbios_product_name(model_name, NULL, name_buf, sizeof(name_buf));
        name = name_buf;
    } else if (product_name) {
        const char *chip = soc_name;
        if (chip && !strncmp(chip, "Apple ", 6))
            chip += 6;
        smbios_product_name(product_name, chip, name_buf, sizeof(name_buf));
        name = name_buf;
    }

    /*
     * Platform firmware: aura and U-Boot together. U-Boot appends its own version to this one
     * ("aura <version> / U-Boot <version>").
     */
    node = dt_smbios_node(smbios_node, "bios");
    if (node < 0)
        bail("FDT: failed to add node 'bios' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "vendor", "Aurora Silicon");
    snprintf(buf, sizeof(buf), "aura %s", aura_version);
    fdt_setprop_string(dt, node, "version", buf);

    node = dt_smbios_node(smbios_node, "system");
    if (node < 0)
        bail("FDT: failed to add node 'system' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    fdt_setprop_string(dt, node, "product", name ? name : model);
    fdt_setprop_string(dt, node, "sku", model);
    fdt_setprop_string(dt, node, "serial", serial);
    /* Version: the regulatory model number, e.g. "A2681", the hardware revision Apple marks */
    char regulatory[16];
    if (adt_padded_string(adt_root, "regulatory-model-number", regulatory, sizeof(regulatory)))
        fdt_setprop_string(dt, node, "version", regulatory);
    else
        regulatory[0] = 0;
    /* UUID: the platform UUID macOS shows as the Hardware UUID */
    u8 uuid[16];
    if (!rust_platform_uuid(uuid)) {
        snprintf(buf, sizeof(buf),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[0],
                 uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9],
                 uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
        fdt_setprop_string(dt, node, "uuid", buf);
    }
    /* Family: the product line, Apple's product name up to its details, e.g. "MacBook Pro" */
    if (product_name) {
        const char *paren = strchr(product_name, '(');
        size_t n = paren ? (size_t)(paren - product_name) : strlen(product_name);
        while (n && product_name[n - 1] == ' ')
            n--;
        if (n && n < sizeof(buf)) {
            memcpy(buf, product_name, n);
            buf[n] = 0;
            fdt_setprop_string(dt, node, "family", buf);
        }
    }

    node = dt_smbios_node(smbios_node, "baseboard");
    if (node < 0)
        bail("FDT: failed to add node 'baseboard' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    const char *target_type = adt_string(adt_root, "target-type");
    if (!target_type)
        bail("ADT: failed to get target-type property\n");
    fdt_setprop_string(dt, node, "product", target_type);
    const char *mlb_serial = adt_string(adt_root, "mlb-serial-number");
    if (mlb_serial)
        fdt_setprop_string(dt, node, "serial", mlb_serial);
    fdt_setprop_u32(dt, node, "feature-flags", SMBIOS_BOARD_FEATURE_HOSTING);
    fdt_setprop_u32(dt, node, "board-type", SMBIOS_BOARD_TYPE_MOTHERBOARD);

    node = dt_smbios_node(smbios_node, "chassis");
    if (node < 0)
        bail("FDT: failed to add node 'chassis' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    u32 enclosure = 0x02;
    const char *chassis_type = fdt_getprop(dt, fdt_root, "chassis-type", &len);
    if (chassis_type) {
        if (!strcmp(chassis_type, "desktop"))
            enclosure = 0x03;
        else if (!strcmp(chassis_type, "laptop"))
            enclosure = 0x09;
        else if (!strcmp(chassis_type, "all-in-one"))
            enclosure = 0x0d;
        else if (!strcmp(chassis_type, "server"))
            enclosure = 0x11;
    }
    fdt_setprop_u32(dt, node, "chassis-type", enclosure);
    if (regulatory[0])
        fdt_setprop_string(dt, node, "version", regulatory);
    fdt_setprop_string(dt, node, "serial", serial);
    /* SKU Number: the part number Apple sells the machine as, e.g. "MLY13X/A" */
    char part[16], region[8];
    if (adt_padded_string(adt_root, "model-number", part, sizeof(part))) {
        if (adt_padded_string(adt_root, "region-info", region, sizeof(region)))
            snprintf(buf, sizeof(buf), "%s%s", part, region);
        else
            snprintf(buf, sizeof(buf), "%s", part);
        fdt_setprop_string(dt, node, "sku", buf);
    }
    /* It booted, and nothing in a Mac reports a chassis intrusion or lock */
    fdt_setprop_u32(dt, node, "bootup-state", SMBIOS_STATE_SAFE);
    fdt_setprop_u32(dt, node, "security-status", SMBIOS_SECURITY_NONE);

    /* Processor: the SoC as one package, with all of its cores */
    u32 cores, max_mhz;
    dt_cpu_summary(&cores, &max_mhz);

    node = dt_smbios_node(smbios_node, "processor");
    if (node < 0)
        bail("FDT: failed to add node 'processor' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "socket-design", "SoC");
    fdt_setprop_u32(dt, node, "processor-type", SMBIOS_PROCESSOR_TYPE_CENTRAL);
    fdt_setprop_u32(dt, node, "family", SMBIOS_PROCESSOR_FAMILY_EXT);
    /* SME is an Armv9 feature: M4 and later implement Armv9, earlier chips Armv8 */
    fdt_setprop_u32(dt, node, "family2",
                    FIELD_GET(ID_AA64PFR1_SME, mrs(SYS_ID_AA64PFR1_EL1))
                        ? SMBIOS_PROCESSOR_FAMILY2_ARMV9
                        : SMBIOS_PROCESSOR_FAMILY2_ARMV8);
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    if (soc_name)
        fdt_setprop_string(dt, node, "version", soc_name);
    fdt_setprop_u32(dt, node, "processor-status", SMBIOS_PROCESSOR_STATUS_OK);
    fdt_setprop_u32(dt, node, "upgrade", SMBIOS_PROCESSOR_UPGRADE_NONE);
    if (max_mhz) {
        fdt_setprop_u32(dt, node, "max-speed", max_mhz);
        fdt_setprop_u32(dt, node, "current-speed", max_mhz);
    }
    if (cores) {
        fdt_setprop_u32(dt, node, "core-count", cores);
        fdt_setprop_u32(dt, node, "core-enabled", cores);
        fdt_setprop_u32(dt, node, "thread-count", cores);
        fdt_setprop_u32(dt, node, "thread-enabled", cores);
    }
    /* The SoC's reference clock */
    int refclk = fdt_path_offset(dt, "/clock-ref");
    const fdt32_t *refhz = refclk >= 0 ? fdt_getprop(dt, refclk, "clock-frequency", &len) : NULL;
    if (refhz && len == 4)
        fdt_setprop_u32(dt, node, "external-clock", fdt32_ld(refhz) / 1000000);
    /* Part Number: the SoC, e.g. "T6040" */
    char platform[16];
    if (adt_padded_string(adt_root, "platform-name", platform, sizeof(platform))) {
        for (char *c = platform; *c; c++)
            if (*c >= 'a' && *c <= 'z')
                *c -= 'a' - 'A';
        fdt_setprop_string(dt, node, "part-number", platform);
    }
    /*
     * Added to what U-Boot reads from the CPU: execute protection (Armv8 XN), hardware
     * virtualization when aura runs at EL2, and frequency control when the CPUs have OPPs.
     */
    fdt_setprop_u32(dt, node, "characteristics",
                    SMBIOS_PROCESSOR_EXEC_PROTECT | (in_el2() ? SMBIOS_PROCESSOR_VIRT : 0) |
                        (max_mhz ? SMBIOS_PROCESSOR_POWER_CONTROL : 0));

    /* Caches: the L2s the device tree describes (the cores do not report their geometry) */
    char l2_desc[64];
    u32 l2_kib = dt_l2_summary(l2_desc, sizeof(l2_desc));
    if (l2_kib) {
        int cache = dt_smbios_node(smbios_node, "cache");
        if (cache < 0 || (node = dt_smbios_node(cache, "l2-cache")) < 0)
            bail("FDT: failed to add node 'cache' to  '/smbios'\n");
        fdt_setprop_string(dt, node, "socket-design", l2_desc);
        fdt_setprop_u32(dt, node, "config", SMBIOS_CACHE_L2_CONFIG);
        fdt_setprop_u32(dt, node, "max-size", smbios_cache_size16(l2_kib));
        fdt_setprop_u32(dt, node, "installed-size", smbios_cache_size16(l2_kib));
        fdt_setprop_u32(dt, node, "max-size2", l2_kib);
        fdt_setprop_u32(dt, node, "installed-size2", l2_kib);
        fdt_setprop_u32(dt, node, "supported-sram-type", SMBIOS_CACHE_SRAM_UNKNOWN);
        fdt_setprop_u32(dt, node, "current-sram-type", SMBIOS_CACHE_SRAM_UNKNOWN);
        fdt_setprop_u32(dt, node, "error-correction-type", SMBIOS_CACHE_UNKNOWN);
        fdt_setprop_u32(dt, node, "system-cache-type", SMBIOS_CACHE_TYPE_UNIFIED);
        fdt_setprop_u32(dt, node, "associativity", SMBIOS_CACHE_UNKNOWN);
    }

    /* No expansion slots: the PCIe links go to built-in devices */
    if (dt_smbios_node(smbios_node, "system-slot") < 0)
        bail("FDT: failed to add node 'system-slot' to  '/smbios'\n");

    /*
     * Memory: the installed DRAM as the ADT gives it, before any carve-out, as one array with
     * one device, since it sits on the SoC package.
     */
    u64 dram_base, dram_size;
    if (adt_chosen < 0 || ADT_GETPROP(adt, adt_chosen, "dram-base", &dram_base) != sizeof(u64) ||
        ADT_GETPROP(adt, adt_chosen, "dram-size", &dram_size) != sizeof(u64) || !dram_size) {
        printf("ADT: no DRAM size, leaving SMBIOS memory out\n");
        return 0;
    }
    const char *dram_type = adt_string(adt_chosen, "dram-type");
    const char *dram_vendor = adt_string(adt_chosen, "dram-vendor");
    u64 dram_kib = dram_size >> 10, dram_mib = dram_size >> 20;

    int mem = dt_smbios_node(smbios_node, "memory-array");
    if (mem < 0 || (node = dt_smbios_node(mem, "dram")) < 0)
        bail("FDT: failed to add node 'memory-array' to  '/smbios'\n");
    fdt_setprop_u32(dt, node, "location", SMBIOS_MA_LOCATION_BOARD);
    fdt_setprop_u32(dt, node, "use", SMBIOS_MA_USE_SYSTEM);
    fdt_setprop_u32(dt, node, "memory-error-correction", SMBIOS_MA_ERRCORR_NONE);
    if (dram_kib < 0x80000000) {
        fdt_setprop_u32(dt, node, "maximum-capacity", dram_kib);
    } else {
        fdt_setprop_u32(dt, node, "maximum-capacity", 0x80000000);
        fdt_setprop_u64(dt, node, "extended-maximum-capacity", dram_size);
    }
    fdt_setprop_u32(dt, node, "number-of-memory-devices", 1);

    mem = dt_smbios_node(smbios_node, "memory-device");
    if (mem < 0 || (node = dt_smbios_node(mem, "dram")) < 0)
        bail("FDT: failed to add node 'memory-device' to  '/smbios'\n");
    if (dram_mib < 0x7fff) {
        fdt_setprop_u32(dt, node, "size", dram_mib);
    } else {
        fdt_setprop_u32(dt, node, "size", 0x7fff);
        fdt_setprop_u32(dt, node, "extended-size", dram_mib);
    }
    fdt_setprop_u32(dt, node, "form-factor", SMBIOS_MD_FF_ROW_OF_CHIPS);
    fdt_setprop_u32(dt, node, "memory-type", smbios_memory_type(dram_type));
    fdt_setprop_u32(dt, node, "type-detail", SMBIOS_MD_TD_SYNCHRONOUS);
    fdt_setprop_string(dt, node, "device-locator", "SoC");
    if (dram_vendor)
        fdt_setprop_string(dt, node, "manufacturer", dram_vendor);
    fdt_setprop_u32(dt, node, "memory-technology", SMBIOS_MD_TECH_DRAM);
    fdt_setprop_u32(dt, node, "memory-operating-mode-capability", SMBIOS_MD_OPMC_VOLATILE);
    fdt_setprop_u64(dt, node, "volatile-size", dram_size);
    /* Speed: the highest data rate of the DRAM operating points */
    u32 rate = dt_dram_data_rate();
    if (rate && rate < 0xffff) {
        fdt_setprop_u32(dt, node, "speed", rate);
        fdt_setprop_u32(dt, node, "configured-memory-speed", rate);
    }
    /* Width: the DRAM controller's channels, 16 bits each on LPDDR4 and LPDDR5 */
    int mcc = adt_path_offset(adt, "/arm-io/mcc");
    u32 channels = 0;
    if (mcc >= 0 && dram_type && !strncmp(dram_type, "LPDDR", 5) &&
        (dram_type[5] == '4' || dram_type[5] == '5') &&
        ADT_GETPROP(adt, mcc, "dcs_num_channels", &channels) > 0 && channels) {
        fdt_setprop_u32(dt, node, "data-width", channels * 16);
        fdt_setprop_u32(dt, node, "total-width", channels * 16);
    }

    mem = dt_smbios_node(smbios_node, "memory-array-mapped-address");
    if (mem < 0 || (node = dt_smbios_node(mem, "dram")) < 0)
        bail("FDT: failed to add node 'memory-array-mapped-address' to  '/smbios'\n");
    fdt_setprop_u32(dt, node, "starting-address", 0xffffffff);
    fdt_setprop_u32(dt, node, "ending-address", 0xffffffff);
    fdt_setprop_u64(dt, node, "extended-starting-address", dram_base);
    fdt_setprop_u64(dt, node, "extended-ending-address", dram_base + dram_size - 1);

    printf("FDT: SMBIOS %s, %s, %u cores up to %u MHz, %lu MiB %s\n", name ? name : model,
           soc_name ? soc_name : "unnamed SoC", cores, max_mhz, (unsigned long)dram_mib,
           dram_type ? dram_type : "DRAM");
    return 0;
}

/* One firmware component for U-Boot's SMBIOS Firmware Inventory (type 45) */
static int dt_smbios_firmware(int *inventory, const char *id, const char *name, const char *version,
                              const char *manufacturer, u32 characteristics)
{
    int node = dt_smbios_node(*inventory, id);

    if (node < 0)
        bail("FDT: failed to add firmware '%s' to the SMBIOS inventory\n", id);
    if (name)
        fdt_setprop_string(dt, node, "component-name", name);
    if (version)
        fdt_setprop_string(dt, node, "version", version);
    if (manufacturer)
        fdt_setprop_string(dt, node, "manufacturer", manufacturer);
    fdt_setprop_u32(dt, node, "characteristics", characteristics);
    fdt_setprop_u32(dt, node, "state", SMBIOS_FW_STATE_ENABLED);
    *inventory = fdt_path_offset(dt, "/smbios/smbios/firmware-inventory");
    return *inventory < 0 ? -1 : 0;
}

/*
 * The firmware components that run before or beside U-Boot, for its SMBIOS Firmware Inventory:
 * Apple's system and OS-paired firmware, both aura stages and the firmware versions aura reads
 * for the OS. U-Boot adds itself. Run last, once the versions are in /chosen.
 */
static int dt_set_smbios_firmware(void)
{
    int adt_product = adt_path_offset(adt, "/product");
    const char *manuf = "Apple Inc.";
    char buf[96], stage1[64] = "", ave_text[16] = "", ave_data[16] = "";
    int smbios_node, inv, len;

    /*
     * Copy what /chosen holds first: adding nodes under /smbios, which libfdt places before
     * /chosen, moves it.
     */
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen >= 0) {
        const char *s1 = fdt_getprop(dt, chosen, "asahi,m1n1-stage1-version", &len);
        if (s1 && len > 1)
            strncpy(stage1, s1, sizeof(stage1) - 1);
        const char *text = fdt_getprop(dt, chosen, "aurora,ave-text-crc32", &len);
        const char *data = fdt_getprop(dt, chosen, "aurora,ave-data-crc32", NULL);
        if (text && data) {
            strncpy(ave_text, text, sizeof(ave_text) - 1);
            strncpy(ave_data, data, sizeof(ave_data) - 1);
        }
    }

    smbios_node = fdt_path_offset(dt, "/smbios/smbios");
    if (smbios_node < 0)
        return 0;
    inv = dt_smbios_node(smbios_node, "firmware-inventory");
    if (inv < 0)
        bail("FDT: failed to add node 'firmware-inventory' to  '/smbios'\n");

    if (system_firmware.string && system_firmware.iboot) {
        snprintf(buf, sizeof(buf), "%s (%s)", system_firmware.string, system_firmware.iboot);
        if (dt_smbios_firmware(&inv, "system", "Apple System Firmware", buf, manuf, 0))
            return -1;
    }
    if (os_firmware.string && os_firmware.iboot) {
        snprintf(buf, sizeof(buf), "%s (%s)", os_firmware.string, os_firmware.iboot);
        if (dt_smbios_firmware(&inv, "os", "Apple OS Firmware", buf, manuf, 0))
            return -1;
    }
    if (stage1[0] &&
        dt_smbios_firmware(&inv, "stage1", "aura Stage 1", stage1, "Aurora Silicon", 0))
        return -1;
    /* Stage 2 updates from the OS (update-m1n1); stage 1 needs recoveryOS (kmutil) */
    if (dt_smbios_firmware(&inv, "stage2", "aura Stage 2", aura_version, "Aurora Silicon",
                           SMBIOS_FW_UPDATABLE))
        return -1;
    /*
     * U-Boot describes itself in its entry. Tell it how it is kept: inside this Stage 2, so it
     * updates from the OS with it.
     */
    if (dt_smbios_firmware(&inv, "u-boot", NULL, NULL, NULL, SMBIOS_FW_UPDATABLE))
        return -1;

    /* The video encoder firmware aura hands Linux, identified by its CRC32s */
    if (ave_text[0]) {
        snprintf(buf, sizeof(buf), "text CRC32 %s, data CRC32 %s", ave_text, ave_data);
        if (dt_smbios_firmware(&inv, "ave", "Video Encoder Firmware", buf, manuf, 0))
            return -1;
    }

    /* The panel's timing controller firmware, where the ADT reports it */
    u32 tcon_fw, tcon_bundle;
    if (adt_product >= 0 &&
        ADT_GETPROP(adt, adt_product, "dp855-tcon-firmware-ver", &tcon_fw) > 0) {
        if (ADT_GETPROP(adt, adt_product, "dp855-tcon-bundle-ver", &tcon_bundle) > 0)
            snprintf(buf, sizeof(buf), "%u (bundle %u)", tcon_fw, tcon_bundle);
        else
            snprintf(buf, sizeof(buf), "%u", tcon_fw);
        if (dt_smbios_firmware(&inv, "tcon", "Display Timing Controller Firmware", buf, manuf, 0))
            return -1;
    }

    return 0;
}

static int dt_set_cpus(void)
{
    int ret = 0;

    int cpus = fdt_path_offset(dt, "/cpus");
    if (cpus < 0)
        bail("FDT: /cpus node not found in devtree\n");

    uint32_t *pruned_phandles = calloc(MAX_CPUS, sizeof(uint32_t));
    size_t pruned = 0;
    if (!pruned_phandles)
        bail("FDT: out of memory\n");

    /* Prune CPU nodes */
    int node, cpu = 0;
    for (node = fdt_first_subnode(dt, cpus); node >= 0;) {
        const char *name = fdt_get_name(dt, node, NULL);
        if (strncmp(name, "cpu@", 4))
            goto next_node;

        if (cpu > MAX_CPUS)
            bail_cleanup("Maximum number of CPUs exceeded, consider increasing MAX_CPUS\n");

        const fdt64_t *prop = fdt_getprop(dt, node, "reg", NULL);
        if (!prop)
            bail_cleanup("FDT: failed to get reg property of CPU\n");

        u64 dt_mpidr = fdt64_ld(prop);

        if (dt_mpidr == (mrs(MPIDR_EL1) & 0xFFFFFF))
            goto next_cpu;

        /*
         * DT and ADT CPU numbering can differ: J616s has no ADT cpu9, while
         * its DT numbers the cores contiguously. Match by MPIDR when the
         * positional CPU is absent or is a different core.
         */
        int idx = cpu;
        if (!smp_is_alive(idx) || smp_get_mpidr(idx) != dt_mpidr) {
            idx = -1;
            for (int i = 0; i < MAX_CPUS; i++) {
                if (smp_is_alive(i) && smp_get_mpidr(i) == dt_mpidr) {
                    idx = i;
                    break;
                }
            }
        }

        if (idx < 0) {
            printf("FDT: CPU %d (MPIDR 0x%lx) is not alive, disabling...\n", cpu, dt_mpidr);
            pruned_phandles[pruned++] = fdt_get_phandle(dt, node);

            int next = fdt_next_subnode(dt, node);
            fdt_nop_node(dt, node);
            cpu++;
            node = next;
            continue;
        }

        u64 mpidr = smp_get_mpidr(idx);
        u64 release_addr = smp_get_release_addr(idx);
        if (fdt_setprop_inplace_u64(dt, node, "cpu-release-addr", release_addr))
            bail_cleanup("FDT: couldn't set cpu-release-addr property\n");

        printf("FDT: CPU %d MPIDR=0x%lx release-addr=0x%lx\n", cpu, mpidr, release_addr);

    next_cpu:
        cpu++;
    next_node:
        node = fdt_next_subnode(dt, node);
    }

    if ((node < 0) && (node != -FDT_ERR_NOTFOUND)) {
        bail_cleanup("FDT: error iterating through CPUs\n");
    }

    /* Prune AIC PMU affinities */
    int aic = fdt_node_offset_by_compatible(dt, -1, "apple,aic");
    if (aic == -FDT_ERR_NOTFOUND)
        aic = fdt_node_offset_by_compatible(dt, -1, "apple,aic2");
    if (aic == -FDT_ERR_NOTFOUND)
        aic = fdt_node_offset_by_compatible(dt, -1, "apple,t8122-aic3");
    if (aic == -FDT_ERR_NOTFOUND)
        aic = fdt_node_offset_by_compatible(dt, -1, "apple,t8152-aic3");
    if (aic < 0)
        bail_cleanup("FDT: Failed to find AIC node\n");

    int affinities = fdt_subnode_offset(dt, aic, "affinities");
    if (affinities < 0) {
        printf("FDT: Failed to find AIC affinities node, ignoring...\n");
    } else {
        int node;
        for (node = fdt_first_subnode(dt, affinities); node >= 0;
             node = fdt_next_subnode(dt, node)) {
            int len;
            const fdt32_t *phs = fdt_getprop(dt, node, "cpus", &len);
            if (!phs)
                bail_cleanup("FDT: Failed to find cpus property under AIC affinity\n");

            fdt32_t *new_phs = calloc(len, 1);
            size_t index = 0;
            size_t count = len / sizeof(fdt32_t);

            for (size_t i = 0; i < count; i++) {
                uint32_t phandle = fdt32_ld(&phs[i]);
                bool prune = false;

                for (size_t j = 0; j < pruned; j++) {
                    if (pruned_phandles[j] == phandle) {
                        prune = true;
                        break;
                    }
                }
                if (!prune)
                    new_phs[index++] = phs[i];
            }

            ret = fdt_setprop(dt, node, "cpus", new_phs, sizeof(fdt32_t) * index);
            free(new_phs);

            if (ret < 0)
                bail_cleanup("FDT: Failed to set cpus property under AIC affinity\n");

            const char *name = fdt_get_name(dt, node, NULL);
            printf("FDT: Pruned %ld/%ld CPU references in [AIC]/affinities/%s\n", count - index,
                   count, name);
        }

        if ((node < 0) && (node != -FDT_ERR_NOTFOUND))
            bail_cleanup("FDT: Error iterating through affinity nodes\n");
    }

    /* Prune CPU-map */
    int cpu_map = fdt_path_offset(dt, "/cpus/cpu-map");
    if (cpu_map < 0) {
        printf("FDT: /cpus/cpu-map node not found in devtree, ignoring...\n");
        free(pruned_phandles);
        return 0;
    }

    int cluster_idx = 0;
    int cluster_node;
    for (cluster_node = fdt_first_subnode(dt, cpu_map); cluster_node >= 0;) {
        const char *name = fdt_get_name(dt, cluster_node, NULL);
        int cpu_idx = 0;

        if (strncmp(name, "cluster", 7))
            goto next_cluster;

        int cpu_node;
        for (cpu_node = fdt_first_subnode(dt, cluster_node); cpu_node >= 0;) {
            const char *cpu_name = fdt_get_name(dt, cpu_node, NULL);

            if (strncmp(cpu_name, "core", 4))
                goto next_map_cpu;

            int len;
            const fdt32_t *cpu_ph = fdt_getprop(dt, cpu_node, "cpu", &len);

            if (!cpu_ph || len != sizeof(*cpu_ph))
                bail_cleanup("FDT: Failed to get cpu prop for /cpus/cpu-map/%s/%s\n", name,
                             cpu_name);

            uint32_t phandle = fdt32_ld(cpu_ph);
            bool prune = false;
            for (size_t i = 0; i < pruned; i++) {
                if (pruned_phandles[i] == phandle) {
                    prune = true;
                    break;
                }
            }

            if (prune) {
                printf("FDT: Pruning /cpus/cpu-map/%s/%s\n", name, cpu_name);

                int next = fdt_next_subnode(dt, cpu_node);
                fdt_nop_node(dt, cpu_node);
                cpu_node = next;
                continue;
            } else {
                char new_name[16];

                snprintf(new_name, 16, "core%d", cpu_idx++);
                fdt_set_name(dt, cpu_node, new_name);
            }
        next_map_cpu:
            cpu_node = fdt_next_subnode(dt, cpu_node);
        }

        if ((cpu_node < 0) && (cpu_node != -FDT_ERR_NOTFOUND))
            bail_cleanup("FDT: Error iterating through CPU nodes\n");

        if (cpu_idx == 0) {
            printf("FDT: Pruning /cpus/cpu-map/%s\n", name);

            int next = fdt_next_subnode(dt, cluster_node);
            fdt_nop_node(dt, cluster_node);
            cluster_node = next;
            continue;
        } else {
            char new_name[16];

            snprintf(new_name, 16, "cluster%d", cluster_idx++);
            fdt_set_name(dt, cluster_node, new_name);
        }
    next_cluster:
        cluster_node = fdt_next_subnode(dt, cluster_node);
    }

    if ((cluster_node < 0) && (cluster_node != -FDT_ERR_NOTFOUND))
        bail_cleanup("FDT: Error iterating through CPU clusters\n");

    return 0;

err:
    free(pruned_phandles);
    return ret;
}

static struct {
    const char *alias;
    const char *fdt_property;
    bool swap;
} mac_address_devices[] = {
    {
        .alias = "bluetooth0",
        .fdt_property = "local-bd-address",
        .swap = true,
    },
    {
        .alias = "ethernet0",
        .fdt_property = "local-mac-address",
    },
    {
        .alias = "ethernet1",
        .fdt_property = "local-mac-address",
    },
    {
        .alias = "wifi0",
        .fdt_property = "local-mac-address",
    },
};

static int dt_set_mac_addresses(void)
{
    int anode = adt_path_offset(adt, "/chosen");

    if (anode < 0)
        bail("ADT: /chosen not found\n");

    for (size_t i = 0; i < sizeof(mac_address_devices) / sizeof(*mac_address_devices); i++) {
        char propname[32];
        snprintf(propname, sizeof(propname), "mac-address-%s", mac_address_devices[i].alias);

        uint8_t addr[6];
        if (ADT_GETPROP_ARRAY(adt, anode, propname, addr) != sizeof(addr))
            continue;

        if (mac_address_devices[i].swap) {
            for (size_t i = 0; i < sizeof(addr) / 2; ++i) {
                uint8_t tmp = addr[i];
                addr[i] = addr[sizeof(addr) - i - 1];
                addr[sizeof(addr) - i - 1] = tmp;
            }
        }

        const char *path = fdt_get_alias(dt, mac_address_devices[i].alias);
        if (path == NULL)
            continue;

        int node = fdt_path_offset(dt, path);
        if (node < 0)
            continue;

        if (fdt_setprop(dt, node, mac_address_devices[i].fdt_property, addr, sizeof(addr)))
            bail("FDT: could not set %s MAC address\n", mac_address_devices[i].alias);

        if (chip_id == T8140 && !strcmp(mac_address_devices[i].alias, "wifi0")) {
            int chosen = fdt_path_offset(dt, "/chosen");
            if (chosen < 0 || fdt_setprop(dt, chosen, "asahi,wifi-mac-address", addr, sizeof(addr)))
                bail("FDT: could not set deferred Wi-Fi MAC address\n");
        }
    }

    return 0;
}

static int dt_set_bluetooth_cal(int anode, int node, const char *adt_name, const char *fdt_name)
{
    u32 len;
    const u8 *cal_blob = adt_getprop(adt, anode, adt_name, &len);

    if (!cal_blob || !len) {
        printf("ADT: Failed to get %s. Replaced module?\n", adt_name);
        return 0;
    }

    fdt_setprop(dt, node, fdt_name, cal_blob, len);
    return 0;
}

static int dt_set_bluetooth(void)
{
    /* J700's Bluetooth function is part of its PCIe MediaTek radio. */
    if (chip_id == T8140)
        return 0;

    int ret;
    int anode = adt_path_offset(adt, "/arm-io/bluetooth");

    if (anode < 0) {
        /*
         * t8012-based system does not have bluetooth, and on
         * some other devices bluetooth is connected with UART.
         */
        printf("ADT: /arm-io/bluetooth not found\n");
        return 0;
    }

    const char *path = fdt_get_alias(dt, "bluetooth0");
    if (path == NULL)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;

    ret = dt_set_bluetooth_cal(anode, node, "bluetooth-taurus-calibration-bf",
                               "brcm,taurus-bf-cal-blob");
    if (ret)
        return ret;

    ret = dt_set_bluetooth_cal(anode, node, "bluetooth-taurus-calibration", "brcm,taurus-cal-blob");
    if (ret)
        return ret;

    return 0;
}

static int dt_set_multitouch(void)
{
    const char *path = fdt_get_alias(dt, "touchbar0");
    if (path == NULL)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        bail("FDT: alias points at nonexistent node\n");

    const char *adt_touchbar;
    if (fdt_node_check_compatible(dt, 0, "apple,j293") == 0)
        adt_touchbar = "/arm-io/spi0/multi-touch";
    else if (fdt_node_check_compatible(dt, 0, "apple,j493") == 0)
        adt_touchbar = "/arm-io/spi3/touch-bar";
    else
        return 0;

    int anode = adt_path_offset(adt, adt_touchbar);
    if (anode < 0)
        bail("ADT: touchbar node %s not found\n", adt_touchbar);

    u32 len;
    const u8 *cal_blob = adt_getprop(adt, anode, "multi-touch-calibration", &len);
    if (!cal_blob || !len) {
        printf("ADT: Failed to get multi-touch-calibration from %s\n", adt_touchbar);
        return 0;
    }

    fdt_setprop(dt, node, "apple,z2-cal-blob", cal_blob, len);
    return 0;
}

#include "keyboard_types.h"

static int dt_set_ipd(void)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        bail("FDT: /chosen node not found in devtree\n");

    int ipd = adt_path_offset(adt, "/arm-io/spi3/ipd");
    if (ipd < 0)
        ipd = adt_path_offset(adt, "/arm-io/dockchannel-mtp/mtp-transport/keyboard");

    if (ipd < 0) {
        printf("ADT: no keyboard found\n");
        return 0;
    }

    u32 len;
    const u8 *kblang = adt_getprop(adt, ipd, "kblang-calibration", &len);
    if (!kblang || len < 2) {
        printf("ADT: kblang-calibration not found, no keyboard layout\n");
        return 0;
    }
    u8 code = kblang[1];

    if (fdt_setprop_u32(dt, chosen, "asahi,kblang-code", code))
        bail("FDT: couldn't set asahi,kblang-code\n");

    const char *path = fdt_get_alias(dt, "keyboard");
    if (path == NULL)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        bail("FDT: keyboard alias points at nonexistent node\n");

    if (fdt_setprop_u32(dt, node, "apple,keyboard-layout-id", code))
        bail("FDT: couldn't set apple,keyboard-layout-id\n");

    if (code >= ARRAY_SIZE(keyboard_types))
        printf("ADT: kblang code out of range, not setting country code\n");
    else if (fdt_setprop_u32(dt, node, "hid-country-code", keyboard_types[code]))
        bail("FDT: couldn't set hid-country-code\n");

    return 0;
}

#define NVRAM_START    0x700000
#define NVRAM_MAX_SIZE SZ_1M

static int dt_set_nvram(void)
{
    int adt_path[8];
    u64 start, size;

    int anode = adt_path_offset_trace(adt, "/arm-io/spi1/spinor/anvram", adt_path);
    if (anode < 0) {
        printf("ADT: nvram partition not found\n");
        return 0;
    }

    int pp = 0;
    while (adt_path[pp])
        pp++;
    adt_path[pp + 1] = 0;

    int ret = adt_get_reg(adt, adt_path, "reg", 0, &start, &size);
    if (ret < 0) {
        printf("ADT: could not read nvram partition start/size\n");
        return 0;
    }

    if (start != NVRAM_START || size > NVRAM_MAX_SIZE) {

        printf("ADT: unexpected nvram partition start (0x%lx) size (0x%lx)\n", start, size);
        return 0;
    }

    int node = fdt_path_offset(dt, "nvram");
    if (node < 0) {
        printf("DT: nvram alias/partition not found\n");
        return 0;
    }

    fdt32_t reg[2];
    fdt32_st(reg + 0, start);
    fdt32_st(reg + 1, size);
    if (fdt_setprop(dt, node, "reg", reg, sizeof(reg))) {
        printf("FDT: couldn't set nvram.reg\n");
        return 0;
    }

    if (fdt_setprop_string(dt, node, "status", "okay") < 0)
        printf("FDT: failed to enable nvram partition node\n");

    return 0;
}

static int dt_set_wifi(void)
{
    int anode = adt_path_offset(adt, "/arm-io/wlan");

    if (anode < 0) {
        /*
         * t8012-based system does not have wifi, and on some other
         * devices WIFI is connected via an internal EHCI controller.
         */
        printf("ADT: /arm-io/wlan not found\n");
        return 0;
    }

    const char *path = fdt_get_alias(dt, "wifi0");
    if (path == NULL)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;

    /* The J700 endpoint is MediaTek, not a Broadcom device. */
    if (chip_id == T8140)
        return 0;

    uint8_t info[16];
    if (ADT_GETPROP_ARRAY(adt, anode, "wifi-antenna-sku-info", info) < 0)
        bail("ADT: Failed to get wifi-antenna-sku-info\n");

    char antenna[8];
    memcpy(antenna, &info[8], sizeof(antenna));
    fdt_setprop_string(dt, node, "apple,antenna-sku", antenna);

    u32 len;
    const u8 *cal_blob = adt_getprop(adt, anode, "wifi-calibration-msf", &len);

    if (!cal_blob || !len) {
        printf("ADT: Failed to get wifi-calibration-msf. Replaced module?\n");
        return 0;
    }

    fdt_setprop(dt, node, "brcm,cal-blob", cal_blob, len);

    return 0;
}

static void dt_set_uboot_dm_preloc(int node)
{
    // Tell U-Boot to bind this node early
    fdt_setprop_empty(dt, node, "u-boot,dm-pre-reloc");
    fdt_setprop_empty(dt, node, "bootph-all");

    // Make sure the power domains are bound early as well
    int pds_size;
    const fdt32_t *pds = fdt_getprop(dt, node, "power-domains", &pds_size);
    if (!pds)
        return;

    fdt32_t *phandles = calloc(pds_size, 1);
    if (!phandles) {
        printf("FDT: out of memory\n");
        return;
    }
    memcpy(phandles, pds, pds_size);

    for (int i = 0; i < pds_size / 4; i++) {
        node = fdt_node_offset_by_phandle(dt, fdt32_ld(&phandles[i]));
        if (node < 0)
            continue;
        dt_set_uboot_dm_preloc(node);

        // restore node offset after DT update
        node = fdt_node_offset_by_phandle(dt, fdt32_ld(&phandles[i]));
        if (node < 0)
            continue;

        // And make sure the PMGR node is bound early too
        node = fdt_parent_offset(dt, node);
        if (node < 0)
            continue;
        dt_set_uboot_dm_preloc(node);
    }

    free(phandles);
}

static int dt_set_uboot(void)
{
    // Make sure that U-Boot can initialize the serial port in its
    // pre-relocation phase by marking its node and the nodes of the
    // power domains it depends on with a "u-boot,dm-pre-reloc"
    // property.

    const char *path = fdt_get_alias(dt, "serial0");
    if (path == NULL)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;

    dt_set_uboot_dm_preloc(node);
    return 0;
}

struct adt_tunable_info {
    const char *adt_name;
    const char *fdt_name;
    size_t reg_offset;
    size_t reg_size;
    bool required;
};

static const struct adt_tunable_info usb4_nhi_tunables[] = {
    {"hi_up_tx_desc_fabric_tunables", "apple,tunable-nhi", 0xf0000, 0x4000, true},
    {"hi_up_tx_data_fabric_tunables", "apple,tunable-nhi", 0xec000, 0x4000, true},
    {"hi_up_rx_desc_fabric_tunables", "apple,tunable-nhi", 0xe8000, 0x4000, true},
    {"hi_up_wr_fabric_tunables", "apple,tunable-nhi", 0xf4000, 0x4000, true},
    {"hi_up_merge_fabric_tunables", "apple,tunable-nhi", 0xf8000, 0x4000, true},
    {"hi_dn_merge_fabric_tunables", "apple,tunable-nhi", 0xfc000, 0x4000, true},
    {"fw_int_ctl_management_tunables", "apple,tunable-nhi", 0x4000, 0x4000, true},
};

static const struct adt_tunable_info usb4_rc_tunables[] = {
    {"top_tunables", "apple,tunable-rc", 0x0, 0x4000, true},
    {"hbw_fabric_tunables", "apple,tunable-rc", 0x4000, 0x4000, true},
    {"lbw_fabric_tunables", "apple,tunable-rc", 0x8000, 0x4000, true},
};

/* Type 7 firmware does not supply the four per-direction fabric tables. */
static const struct adt_tunable_info usb4_t8142_nhi_tunables[] = {
    {"hi_up_tx_desc_fabric_tunables", "apple,tunable-nhi", 0xe8000, 0x4000, false},
    {"hi_up_tx_data_fabric_tunables", "apple,tunable-nhi", 0xec000, 0x4000, false},
    {"hi_up_rx_desc_fabric_tunables", "apple,tunable-nhi", 0xf0000, 0x4000, false},
    {"hi_up_wr_fabric_tunables", "apple,tunable-nhi", 0xf4000, 0x4000, false},
    {"hi_up_merge_fabric_tunables", "apple,tunable-nhi", 0xf8000, 0x4000, true},
    {"hi_dn_merge_fabric_tunables", "apple,tunable-nhi", 0xfc000, 0x4000, true},
    {"fw_int_ctl_management_tunables", "apple,tunable-nhi", 0x4000, 0x4000, true},
};

static const struct adt_tunable_info usb4_pcie_adapter_tunables[] = {
    {"pcie_adapter_regs_tunables", "apple,tunable-pcie-adapter", 0x0, 0x4000, true},
};

struct acio_tunable {
    u32 offset;
    u32 size;
    u64 mask;
    u64 value;
} PACKED;
static_assert(sizeof(struct acio_tunable) == 24, "Invalid acio_tunable size");

/*
 * This is *almost* identical to dt_append_atc_tunable except for the different
 * tunable struct and that tunable->size is in bytes instead of bits.
 * If only C had generics that aren't macros :-(
 */
static int dt_append_acio_tunable(int adt_node, int fdt_node,
                                  const struct adt_tunable_info *tunable_info)
{
    u32 tunables_len;
    const struct acio_tunable *tunable_adt =
        adt_getprop(adt, adt_node, tunable_info->adt_name, &tunables_len);

    if (!tunable_adt) {
        printf("ADT: tunable %s not found\n", tunable_info->adt_name);

        if (tunable_info->required)
            return -1;
        else
            return 0;
    }

    if ((tunable_info->required && !tunables_len) || tunables_len % sizeof(*tunable_adt)) {
        printf("ADT: tunable %s with invalid length %d\n", tunable_info->adt_name, tunables_len);
        return -1;
    }

    u32 n_tunables = tunables_len / sizeof(*tunable_adt);
    for (size_t j = 0; j < n_tunables; j++) {
        const struct acio_tunable *tunable = &tunable_adt[j];

        if (tunable->size != 4) {
            printf("kboot: ACIO tunable has invalid size %d\n", tunable->size);
            return -1;
        }

        if (tunable->offset % tunable->size) {
            printf("kboot: ACIO tunable has unaligned offset %x\n", tunable->offset);
            return -1;
        }

        if (tunable_info->reg_size < tunable->size ||
            tunable->offset > tunable_info->reg_size - tunable->size ||
            tunable_info->reg_offset > UINT32_MAX - tunable->offset) {
            printf("kboot: ACIO tunable has invalid offset %x\n", tunable->offset);
            return -1;
        }

        if (tunable->mask > UINT32_MAX || tunable->value > UINT32_MAX) {
            printf("kboot: ACIO tunable does not fit a 32-bit register\n");
            return -1;
        }

        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name,
                               tunable->offset + tunable_info->reg_offset) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name, tunable->mask) < 0)
            return -1;
        if (fdt_appendprop_u32(dt, fdt_node, tunable_info->fdt_name, tunable->value) < 0)
            return -1;
    }

    return 0;
}

static u8 crc8(u8 *b, size_t len)
{
    u8 value = 0xff;

    for (unsigned i = 0; i < len; i++) {
        value /*= value*/ ^= b[i];
        for (unsigned j = 0; j < 8; j++)
            value = (value << 1) ^ ((value & 0x80) ? 7 : 0);
    }

    return value;
}

static int dt_copy_usb4_drom(const char *adt_path, const char *dt_alias, u64 router_uuid)
{
    u8 drom[256];

    int adt_node = adt_path_offset(adt, adt_path);
    if (adt_node < 0)
        return -1;

    const char *fdt_path = fdt_get_alias(dt, dt_alias);
    if (fdt_path == NULL)
        bail("FDT: Unable to find alias %s\n", dt_alias);

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0)
        bail("FDT: Unable to find path %s for alias %s\n", fdt_path, dt_alias);

    u32 drom_len;
    const u8 *drom_blob = adt_getprop(adt, adt_node, "thunderbolt-drom", &drom_len);
    if (!drom_blob || drom_len < 1 + sizeof(router_uuid))
        bail("ADT: Failed to get thunderbolt-drom\n");

    if (drom_len > sizeof(drom))
        bail("ADT: thunderbolt-drom exceed buffer size (%u > %zu)\n", drom_len, sizeof(drom));

    memcpy(drom, drom_blob, drom_len);
    memcpy(&drom[1], &router_uuid, sizeof(router_uuid));

    drom[0] = crc8(&drom[1], 8);

    return fdt_setprop(dt, fdt_node, "apple,thunderbolt-drom", drom, drom_len);
}

static int dt_copy_acio_tunables(const char *adt_path, const char *dt_alias,
                                 const struct adt_tunable_info *tunables, size_t n_tunables)
{
    int ret;
    int adt_node = adt_path_offset(adt, adt_path);
    if (adt_node < 0)
        return -1;

    const char *fdt_path = fdt_get_alias(dt, dt_alias);
    if (fdt_path == NULL)
        bail("FDT: Unable to find alias %s\n", dt_alias);

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0)
        bail("FDT: Unable to find path %s for alias %s\n", fdt_path, dt_alias);

    for (size_t i = 0; i < n_tunables; ++i) {
        ret = dt_append_acio_tunable(adt_node, fdt_node, &tunables[i]);
        if (ret)
            bail_cleanup("ADT: unable to convert '%s' tunable", tunables[i].adt_name);
    }

    return 0;

err:
    for (size_t i = 0; i < n_tunables; ++i)
        fdt_delprop(dt, fdt_node, tunables[i].fdt_name);

    return -1;
}

static int dt_set_acio_tunables(void)
{
    char adt_path[32];
    char fdt_alias[32];
    u64 router_uuid = rust_usb4_router_uuid();
    const struct adt_tunable_info *nhi_tunables = usb4_nhi_tunables;
    size_t n_nhi_tunables = ARRAY_SIZE(usb4_nhi_tunables);

    if (chip_id == T8142) {
        nhi_tunables = usb4_t8142_nhi_tunables;
        n_nhi_tunables = ARRAY_SIZE(usb4_t8142_nhi_tunables);
    }

    if (!router_uuid)
        bail("ADT: unable to generate USB4 router UUID\n");

    for (int i = 0; i < MAX_CIO_DEVS; ++i) {
        snprintf(adt_path, sizeof(adt_path), "/arm-io/acio%d", i);

        snprintf(fdt_alias, sizeof(fdt_alias), "usb4_%d_rc", i);
        if (!fdt_get_alias(dt, fdt_alias))
            snprintf(fdt_alias, sizeof(fdt_alias), "usb4-%d-acio", i);
        dt_copy_acio_tunables(adt_path, fdt_alias, usb4_rc_tunables,
                              sizeof(usb4_rc_tunables) / sizeof(*usb4_rc_tunables));

        snprintf(fdt_alias, sizeof(fdt_alias), "usb4_%d_pcie_adapter", i);
        if (!fdt_get_alias(dt, fdt_alias))
            snprintf(fdt_alias, sizeof(fdt_alias), "usb4-%d-pcie-adapter", i);
        dt_copy_acio_tunables(adt_path, fdt_alias, usb4_pcie_adapter_tunables,
                              sizeof(usb4_pcie_adapter_tunables) /
                                  sizeof(*usb4_pcie_adapter_tunables));

        snprintf(fdt_alias, sizeof(fdt_alias), "usb4_%d_nhi", i);
        if (!fdt_get_alias(dt, fdt_alias))
            snprintf(fdt_alias, sizeof(fdt_alias), "usb4-%d-nhi", i);
        dt_copy_acio_tunables(adt_path, fdt_alias, nhi_tunables, n_nhi_tunables);
        dt_copy_usb4_drom(adt_path, fdt_alias, router_uuid | i);
    }

    return 0;
}

static const struct adt_tunable_info pciec_tunables[] = {
    {"atc-apcie-debug-tunables", "apple,tunable-debug", 0, 0x2000, true},
    {"atc-apcie-fabric-tunables", "apple,tunable-fabric", 0, 0x4000, true},
    {"atc-apcie-rc-tunables", "apple,tunable-rc", 0, 0x4000, true},
};
static const struct adt_tunable_info pciec_port_tunable = {
    .adt_name = "apcie-config-tunables",
    .fdt_name = "apple,tunable",
    .reg_offset = 0,
    .reg_size = 0x4000,
    .required = true,
};

static int dt_copy_pciec_tunables(const char *adt_path, const char *dt_alias)
{
    int ret;
    int adt_node = adt_path_offset(adt, adt_path);

    if (adt_node < 0)
        return -1;

    const char *fdt_path = fdt_get_alias(dt, dt_alias);
    if (fdt_path == NULL)
        bail("FDT: Unable to find alias %s\n", dt_alias);

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0)
        bail("FDT: Unable to find path %s for alias %s\n", fdt_path, dt_alias);

    int fdt_port_node = fdt_first_subnode(dt, fdt_node);
    if (fdt_port_node < 0)
        bail("FDT: Unable to find port node for %s\n", fdt_path);

    ret = dt_append_acio_tunable(adt_node, fdt_port_node, &pciec_port_tunable);
    if (ret)
        bail_cleanup("PCIEC: unable to convert '%s' tunable", pciec_tunables[0].adt_name);

    for (size_t i = 0; i < sizeof(pciec_tunables) / sizeof(*pciec_tunables); ++i) {
        ret = dt_append_acio_tunable(adt_node, fdt_node, &pciec_tunables[i]);
        if (ret)
            bail_cleanup("PCIEC: unable to convert '%s' tunable", pciec_tunables[i].adt_name);
    }

    return 0;

err:
    for (size_t i = 0; i < sizeof(pciec_tunables) / sizeof(*pciec_tunables); ++i)
        fdt_delprop(dt, fdt_node, pciec_tunables[i].fdt_name);
    fdt_delprop(dt, fdt_port_node, pciec_port_tunable.fdt_name);

    return -1;
}

/* Copy calibration at boot, never substitute another machine's DART values. */
static int dt_copy_pciec_dart_tunables(const char *adt_path, const char *dt_alias)
{
    const struct adt_tunable_info tunable = {
        .adt_name = "dart-tunables-instance-0",
        .fdt_name = "apple,tunable",
        .reg_size = 0x4000,
        .required = true,
    };
    const char *fdt_path = fdt_get_alias(dt, dt_alias);
    if (!fdt_path)
        return -1;
    int host = fdt_path_offset(dt, fdt_path);
    if (host < 0)
        return -1;

    int len;
    const fdt32_t *map = fdt_getprop(dt, host, "iommu-map", &len);
    /* PCIe-C describes one RID range and a one-cell stream specifier. */
    if (!map || len != 4 * sizeof(*map))
        return -1;
    int dart = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(map[1]));
    if (dart < 0 || fdt_node_check_compatible(dt, dart, "apple,t8110-dart"))
        return -1;

    /* Keep existing board tables, including the qualified T600x values. */
    if (fdt_getprop(dt, dart, "apple,tunable", NULL))
        return 0;
    int adt_node = adt_path_offset(adt, adt_path);
    if (adt_node < 0)
        return -1;

    if (dt_append_acio_tunable(adt_node, dart, &tunable)) {
        /* The converter may have appended a prefix before rejecting input. */
        fdt_delprop(dt, dart, "apple,tunable");
        return -1;
    }
    return 0;
}

static int dt_set_pcie_tunables(void)
{
    char adt_path[32];
    char fdt_alias[32];

    for (int i = 0; i < MAX_CIO_DEVS; ++i) {
        memset(adt_path, 0, sizeof(adt_path));
        snprintf(adt_path, sizeof(adt_path), "/arm-io/apciec%d", i);

        memset(fdt_alias, 0, sizeof(adt_path));
        snprintf(fdt_alias, sizeof(fdt_alias), "usb4_%d_pcie", i);

        dt_copy_pciec_tunables(adt_path, fdt_alias);

        snprintf(adt_path, sizeof(adt_path), "/arm-io/dart-apciec%d", i);
        dt_copy_pciec_dart_tunables(adt_path, fdt_alias);
    }

    return 0;
}

static int dt_add_pcie_iommu(uint32_t *phandles, int *count, uint32_t phandle)
{
    if (!phandle || fdt_node_offset_by_phandle(dt, phandle) < 0)
        return -1;

    for (int i = 0; i < *count; i++)
        if (phandles[i] == phandle)
            return 0;

    if (*count >= 128)
        return -1;
    phandles[(*count)++] = phandle;
    return 0;
}

static int dt_pcie_iommu_cells(uint32_t phandle)
{
    int iommu = fdt_node_offset_by_phandle(dt, phandle);
    int len;
    if (iommu < 0)
        return -1;
    const fdt32_t *cells = fdt_getprop(dt, iommu, "#iommu-cells", &len);
    if (!cells || len != (int)sizeof(fdt32_t))
        return -1;
    return fdt32_ld(cells) <= 16 ? (int)fdt32_ld(cells) : -1;
}

static int dt_collect_pcie_iommus(int node, uint32_t *phandles, int *count)
{
    int len;
    const fdt32_t *map = fdt_getprop(dt, node, "iommu-map", &len);
    if (map) {
        if (len % (int)sizeof(fdt32_t))
            return -1;
        for (int i = 0; i < len / (int)sizeof(fdt32_t);) {
            if (len / (int)sizeof(fdt32_t) - i < 3)
                return -1;
            uint32_t phandle = fdt32_ld(&map[i + 1]);
            int cells = dt_pcie_iommu_cells(phandle);
            if (cells < 0 || len / (int)sizeof(fdt32_t) - i < 3 + cells ||
                dt_add_pcie_iommu(phandles, count, phandle))
                return -1;
            i += 3 + cells;
        }
    } else if (len != -FDT_ERR_NOTFOUND) {
        return -1;
    }

    const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &len);
    if (!iommus)
        return len == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (len % (int)sizeof(fdt32_t))
        return -1;

    for (int i = 0; i < len / (int)sizeof(fdt32_t);) {
        uint32_t phandle = fdt32_ld(&iommus[i]);
        int cells = dt_pcie_iommu_cells(phandle);
        if (cells < 0 || cells > len / (int)sizeof(fdt32_t) - i - 1 ||
            dt_add_pcie_iommu(phandles, count, phandle))
            return -1;
        i += 1 + cells;
    }

    return 0;
}

static bool dt_is_pcie_mapper(int node)
{
    const char *name = fdt_get_name(dt, node, NULL);
    return (name && (!strncmp(name, "piodma", 6) || !strncmp(name, "pio-dma", 7) ||
                     !strncmp(name, "mapper", 6) || !strncmp(name, "iommu-mapper", 12))) ||
           !fdt_node_check_compatible(dt, node, "apple,t8140-pcie-piodma") ||
           !fdt_node_check_compatible(dt, node, "iommu-mapper");
}

static int dt_disable_pcie_mappers(int parent)
{
    int nodes[64];
    int count = 0;
    int depth = 0;
    int node = parent;

    while ((node = fdt_next_node(dt, node, &depth)) >= 0 && depth > 0) {
        if (!dt_is_pcie_mapper(node))
            continue;
        if (count >= (int)ARRAY_SIZE(nodes))
            return -1;
        nodes[count++] = node;
    }
    if (node < 0 && node != -FDT_ERR_NOTFOUND)
        return -1;

    for (int i = count - 1; i >= 0; i--) {
        printf("FDT: disabling PCIe mapper/PIO-DMA %s\n", fdt_get_name(dt, nodes[i], NULL));
        if (fdt_setprop_string(dt, nodes[i], "status", "disabled") < 0)
            return -1;
    }
    return 0;
}

static int dt_disable_t8140_piodma(void)
{
    static const char *const compatibles[] = {
        "apple,t8140-piodma-diagnostic",
        "apple,t8140-pcie-piodma",
    };
    for (size_t i = 0; i < ARRAY_SIZE(compatibles); i++) {
        int node = -1;
        while ((node = fdt_node_offset_by_compatible(dt, node, compatibles[i])) >= 0) {
            if (fdt_setprop_string(dt, node, "status", "disabled"))
                return -1;
        }
        if (node != -FDT_ERR_NOTFOUND)
            return -1;
    }
    return 0;
}

static int dt_disable_t8140_pcie(void)
{
    if (dt_disable_t8140_piodma())
        return -1;
    int nodes[64];
    int count = 0;
    int node = -1;
    uint32_t iommus[128];
    int iommu_count = 0;

    while ((node = fdt_node_offset_by_prop_value(dt, node, "device_type", "pci", sizeof("pci"))) >=
           0) {
        if (count >= (int)ARRAY_SIZE(nodes)) {
            printf("FDT: too many PCIe controller or port nodes\n");
            return -1;
        }
        if (dt_collect_pcie_iommus(node, iommus, &iommu_count)) {
            printf("FDT: invalid PCIe IOMMU reference in %s\n", fdt_get_name(dt, node, NULL));
            return -1;
        }
        nodes[count++] = node;
    }
    if (node != -FDT_ERR_NOTFOUND)
        return -1;

    /* Work backwards so inserting a property cannot move an earlier offset. */
    for (int i = count - 1; i >= 0; i--) {
        if (dt_disable_pcie_mappers(nodes[i]))
            return -1;
        if (fdt_setprop_string(dt, nodes[i], "status", "disabled") < 0)
            return -1;
    }

    /* Resolve each phandle again: the PCIe status writes can move DART nodes. */
    for (int i = 0; i < iommu_count; i++) {
        node = fdt_node_offset_by_phandle(dt, iommus[i]);
        if (node < 0 || dt_disable_pcie_mappers(node))
            return -1;
        node = fdt_node_offset_by_phandle(dt, iommus[i]);
        if (node < 0)
            return -1;
        printf("FDT: disabling PCIe IOMMU %s (phandle %u)\n", fdt_get_name(dt, node, NULL),
               iommus[i]);
        if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
            return -1;
    }

    printf("FDT: T8140 APCIe skipped; disabled %d PCIe controller/port nodes and %d IOMMUs\n",
           count, iommu_count);
    return 0;
}

static int dt_get_iommu_node(int node, u32 num)
{
    int len;
    assert(num < 32);
    const void *prop = fdt_getprop(dt, node, "iommus", &len);
    if (!prop || len < 0 || (u32)len < 8 * (num + 1)) {
        printf("FDT: unexpected 'iommus' prop / len %d\n", len);
        return -FDT_ERR_NOTFOUND;
    }

    const fdt32_t *iommus = prop;
    uint32_t phandle = fdt32_ld(&iommus[num * 2]);

    return fdt_node_offset_by_phandle(dt, phandle);
}

static dart_dev_t *dt_init_dart_by_node(int node, u32 num)
{
    int len;
    assert(num < 32);
    const void *prop = fdt_getprop(dt, node, "iommus", &len);
    if (!prop || len < 0 || (u32)len < 8 * (num + 1)) {
        printf("FDT: unexpected 'iommus' prop / len %d\n", len);
        return NULL;
    }

    const fdt32_t *iommus = prop;
    u32 iommu_phandle = fdt32_ld(&iommus[num * 2]);
    u32 iommu_stream = fdt32_ld(&iommus[num * 2 + 1]);

    printf("FDT: iommu phande:%u stream:%u\n", iommu_phandle, iommu_stream);

    if (chip_id == T8140)
        return dart_init_fdt_locked(dt, iommu_phandle, iommu_stream);
    return dart_init_fdt(dt, iommu_phandle, iommu_stream, true);
}

static u64 dart_get_mapping(dart_dev_t *dart, const char *path, u64 paddr, size_t size)
{
    u64 iova = dart_search_range(dart, paddr, size);
    if (DART_IS_ERR(iova)) {
        printf("ADT: %s paddr: 0x%lx is not contiguously mapped\n", path, paddr);
        return iova;
    }
    return iova;
}

static int dt_device_set_reserved_mem(int node, const char *name, uint32_t phandle, u64 iova,
                                      u64 size)
{
    int ret;

    ret = fdt_appendprop_u32(dt, node, "iommu-addresses", phandle);
    if (ret != 0)
        bail("FDT: could not append phandle to '%s.iommu-addresses' property: %d\n", name, ret);

    ret = fdt_appendprop_u64(dt, node, "iommu-addresses", iova);
    if (ret != 0)
        bail("FDT: could not append iova to '%s.iommu-addresses' property: %d\n", name, ret);

    ret = fdt_appendprop_u64(dt, node, "iommu-addresses", size);
    if (ret != 0)
        bail("FDT: could not append size to '%s.iommu-addresses' property: %d\n", name, ret);

    return 0;
}

static int dt_device_set_reserved_mem_from_dart(int node, dart_dev_t *dart, const char *device,
                                                const char *name, uint32_t phandle, u64 paddr,
                                                u64 size)
{
    u64 iova = dart_get_mapping(dart, name, paddr, size);
    if (DART_IS_ERR(iova)) {
        printf("FDT: %s DART %s: no contiguous mapping for PA 0x%lx size 0x%lx\n", device, name,
               paddr, size);
        if (chip_id == T8140) {
            u64 first = dart_search_range(dart, paddr, SZ_16K);
            if (DART_IS_ERR(first)) {
                printf("FDT: %s DART %s: first PA 0x%lx has no IOVA\n", device, name, paddr);
            } else {
                size_t pages = ALIGN_UP(size, SZ_16K) / SZ_16K;
                for (size_t page = 0; page < pages; page++) {
                    u64 check_iova = first + page * SZ_16K;
                    u64 expected = paddr + page * SZ_16K;
                    u64 actual = (u64)dart_translate_silent(dart, check_iova);
                    if (actual != expected) {
                        printf("FDT: %s DART %s: IOVA 0x%lx expected PA 0x%lx, got 0x%lx"
                               " (range IOVA 0x%lx size 0x%lx)\n",
                               device, name, check_iova, expected, actual, first, size);
                        break;
                    }
                }
            }
        }
        /* A T8140 reservation must never claim a device without a complete,
         * inherited DART view of every page in the range. */
        return chip_id == T8140 ? -1 : 0;
    }

    if (chip_id == T8140)
        printf("FDT: %s DART %s: IOVA 0x%lx -> PA 0x%lx size 0x%lx validated\n", device, name, iova,
               paddr, size);
    /* dart_search returns a page-table offset without the bus address's
     * high bits. On T8142 the display DARTs translate a size-aligned window
     * high in the bus address space (64 GiB at 1 TiB); add the window base so
     * Linux reserves the IOVA the device actually uses.
     */
    if (!fdt_node_check_compatible(dt, 0, "apple,t8142")) {
        int owner = fdt_node_offset_by_phandle(dt, phandle);
        int iommu = owner < 0 ? owner : dt_get_iommu_node(owner, 0);
        int len = 0;
        const fdt64_t *range = iommu < 0 ? NULL : fdt_getprop(dt, iommu, "apple,dma-range", &len);
        if (range && len == 16) {
            u64 base = fdt64_ld(&range[0]), span = fdt64_ld(&range[1]);
            if (span && !(span & (span - 1)) && base >= span && !(base & (span - 1)) && iova < span)
                iova |= base;
        }
    }

    return dt_device_set_reserved_mem(node, name, phandle, iova, size);
}

static int dt_get_or_add_reserved_mem(const char *node_name, const char *compat, bool nomap,
                                      u64 paddr, size_t size)
{
    int ret;
    int resv_node = fdt_path_offset(dt, "/reserved-memory");
    if (resv_node < 0)
        bail("FDT: '/reserved-memory' not found\n");

    int node = fdt_subnode_offset(dt, resv_node, node_name);
    if (node < 0) {
        node = fdt_add_subnode(dt, resv_node, node_name);
        if (node < 0)
            bail("FDT: failed to add node '%s' to  '/reserved-memory'\n", node_name);

        uint32_t phandle;
        ret = fdt_generate_phandle(dt, &phandle);
        if (ret)
            bail("FDT: failed to generate phandle: %d\n", ret);

        ret = fdt_setprop_u32(dt, node, "phandle", phandle);
        if (ret != 0)
            bail("FDT: couldn't set '%s.phandle' property: %d\n", node_name, ret);
    }

    u64 reg[2] = {cpu_to_fdt64(paddr), cpu_to_fdt64(size)};
    ret = fdt_setprop(dt, node, "reg", reg, sizeof(reg));
    if (ret != 0)
        bail("FDT: couldn't set '%s.reg' property: %d\n", node_name, ret);

    ret = fdt_setprop_string(dt, node, "compatible", compat);
    if (ret != 0)
        bail("FDT: couldn't set '%s.compatible' property: %d\n", node_name, ret);

    if (nomap) {
        ret = fdt_setprop_empty(dt, node, "no-map");
        if (ret != 0)
            bail("FDT: couldn't set '%s.no-map' property: %d\n", node_name, ret);
    }

    return node;
}

static int dt_device_add_mem_region(const char *alias, uint32_t phandle, const char *name)
{
    int ret;
    int dev_node = fdt_path_offset(dt, alias);
    if (dev_node < 0)
        bail("FDT: failed to get node for alias '%s'\n", alias);

    ret = fdt_appendprop_u32(dt, dev_node, "memory-region", phandle);
    if (ret != 0)
        bail("FDT: failed to append to 'memory-region' property\n");

    dev_node = fdt_path_offset(dt, alias);
    if (dev_node < 0)
        bail("FDT: failed to update node for alias '%s'\n", alias);

    if (!name)
        return 0;

    ret = fdt_appendprop_string(dt, dev_node, "memory-region-names", name);
    if (ret != 0)
        bail("FDT: failed to append to 'memory-region-names' property\n");

    return 0;
}

static int dt_set_dcp_firmware(const char *alias)
{
    const char *path = fdt_get_alias(dt, alias);

    /* A complete DT need not provide an alias for every external DCP. */
    if (!path && alias[0] == '/')
        path = alias;

    if (!path)
        return 0;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;

    if (firmware_set_fdt(dt, node, "apple,firmware-version", &os_firmware) < 0)
        bail("FDT: Could not set apple,firmware-version for %s\n", path);

    const struct fw_version_info *compat;

    switch (os_firmware.version) {
        case V12_3_1:
        case V12_4:
            compat = &fw_versions[V12_3];
            break;
        case V13_5B4:
        case V13_5:
        case V13_6_1:
            compat = &fw_versions[V13_5];
            break;
        case V14_7:
            compat = &fw_versions[V14_7];
            break;
        default:
            compat = &os_firmware;
            break;
    }

    if (firmware_set_fdt(dt, node, "apple,firmware-compat", compat) < 0)
        bail("FDT: Could not set apple,firmware-compat for %s\n", path);

    return 0;
}

/*
 * The T600x loader owns the external DCP handoff. Publish its explicitly
 * disabled Type-C routes and controller together, after firmware reservation.
 * Keep other chips' route ownership (notably the T8122 kernel gate) intact.
 */
static int dt_enable_dcp_with_typec_routes(const char *alias)
{
    int node = fdt_path_offset(dt, alias);
    if (node < 0)
        return node;
    /* Other SoCs keep the original single-property enable path. */
    if (chip_id != T6000 && chip_id != T6001 && chip_id != T6002)
        return fdt_setprop_string(dt, node, "status", "okay");

    int routes = fdt_subnode_offset(dt, node, "typec-routes");
    if (routes < 0 && routes != -FDT_ERR_NOTFOUND)
        return routes;
    /* A controller without routes, such as the internal DCP, has nothing to publish with it. */
    if (routes == -FDT_ERR_NOTFOUND)
        return fdt_setprop_string(dt, node, "status", "okay");

    void *original = dt;
    size_t capacity = fdt_totalsize(original);
    void *scratch = malloc(capacity);
    if (!scratch) {
        /* Without the transaction, enable the controller as before; leave route statuses alone. */
        printf("FDT: no memory to publish the %s Type-C routes; leaving route statuses unchanged\n",
               alias);
        return fdt_setprop_string(dt, node, "status", "okay");
    }
    memcpy(scratch, original, capacity);
    dt = scratch;

    int ret = 0;
    node = fdt_path_offset(dt, alias);
    routes = node < 0 ? node : fdt_subnode_offset(dt, node, "typec-routes");
    if (routes < 0) {
        ret = routes;
        goto out;
    }
    int route;
    fdt_for_each_subnode(route, dt, routes)
    {
        int len = 0;
        const char *status = fdt_getprop(dt, route, "status", &len);
        if (!status) {
            if (len == -FDT_ERR_NOTFOUND)
                continue;
            ret = len;
            goto out;
        }
        /* Do not turn a reserved, failed or malformed route on. */
        if (len != sizeof("disabled") || memcmp(status, "disabled", len))
            continue;
        ret = fdt_setprop_string(dt, route, "status", "okay");
        if (ret)
            goto out;
    }
    if (route != -FDT_ERR_NOTFOUND) {
        ret = route;
        goto out;
    }

    /* Resolve the owner again after editing its route properties. */
    node = fdt_path_offset(dt, alias);
    ret = node < 0 ? node : fdt_setprop_string(dt, node, "status", "okay");
out:
    dt = original;
    if (!ret)
        memcpy(original, scratch, capacity);
    free(scratch);
    return ret;
}

struct disp_mapping {
    char region_adt[24];
    char mem_fdt[24];
    bool map_dcp;
    bool map_disp;
    bool map_piodma;
};

struct mem_region {
    u64 paddr;
    u64 size;
};

static int dt_add_reserved_regions(const char *dcp_alias, const char *disp_alias,
                                   const char *piodma_alias, const char *compat,
                                   struct disp_mapping *maps, struct mem_region *region,
                                   u32 num_maps)
{
    int ret = 0;
    dart_dev_t *dart_dcp = NULL, *dart_disp = NULL, *dart_piodma = NULL;
    uint32_t dcp_phandle = 0, disp_phandle = 0, piodma_phandle = 0;

    /* Check for display device aliases, if one is missing assume it is an old DT
     * without display nodes and return without error.
     * Otherwise init each dart and retrieve the node's phandle.
     */
    if (dcp_alias) {
        int dcp_node = fdt_path_offset(dt, dcp_alias);
        if (dcp_node < 0) {
            printf("FDT: could not resolve '%s' alias\n", dcp_alias);
            ret = chip_id == T8140 ? -1 : 0;
            goto err; // cleanup
        }
        dart_dcp = dt_init_dart_by_node(dcp_node, 0);
        if (!dart_dcp)
            bail_cleanup("FDT: failed to init DART for '%s'\n", dcp_alias);
        dcp_phandle = fdt_get_phandle(dt, dcp_node);
    }

    if (disp_alias) {
        int disp_node = fdt_path_offset(dt, disp_alias);
        if (disp_node < 0) {
            printf("FDT: could not resolve '%s' alias\n", disp_alias);
            ret = chip_id == T8140 ? -1 : 0;
            goto err; // cleanup
        }
        dart_disp = dt_init_dart_by_node(disp_node, 0);
        if (!dart_disp)
            bail_cleanup("FDT: failed to init DART for '%s'\n", disp_alias);
        disp_phandle = fdt_get_phandle(dt, disp_node);
    }

    if (piodma_alias) {
        int piodma_node = fdt_path_offset(dt, piodma_alias);
        if (piodma_node < 0) {
            printf("FDT: could not resolve '%s' alias\n", piodma_alias);
            ret = chip_id == T8140 ? -1 : 0;
            goto err; // cleanup
        }

        dart_piodma = dt_init_dart_by_node(piodma_node, 0);
        if (!dart_piodma)
            bail_cleanup("FDT: failed to init DART for '%s'\n", piodma_alias);
        piodma_phandle = fdt_get_phandle(dt, piodma_node);
    }

    for (unsigned i = 0; i < num_maps; i++) {
        const char *name = maps[i].mem_fdt;
        char node_name[64];

        snprintf(node_name, sizeof(node_name), "%s@%lx", name, region[i].paddr);
        int mem_node =
            dt_get_or_add_reserved_mem(node_name, compat, true, region[i].paddr, region[i].size);
        if (mem_node < 0) {
            ret = mem_node;
            goto err;
        }

        uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);

        if (maps[i].map_dcp && dart_dcp) {
            ret =
                dt_device_set_reserved_mem_from_dart(mem_node, dart_dcp, dcp_alias, node_name,
                                                     dcp_phandle, region[i].paddr, region[i].size);
            if (ret != 0)
                goto err;
        }
        if (maps[i].map_disp && dart_disp) {
            ret =
                dt_device_set_reserved_mem_from_dart(mem_node, dart_disp, disp_alias, node_name,
                                                     disp_phandle, region[i].paddr, region[i].size);
            if (ret != 0)
                goto err;
        }
        if (maps[i].map_piodma && dart_piodma) {
            ret = dt_device_set_reserved_mem_from_dart(mem_node, dart_piodma, piodma_alias,
                                                       node_name, piodma_phandle, region[i].paddr,
                                                       region[i].size);
            if (ret != 0)
                goto err;
        }

        /* modify device nodes after filling /reserved-memory to avoid
         * reloading mem_node's offset */
        if (maps[i].map_dcp && dcp_alias) {
            ret = dt_device_add_mem_region(dcp_alias, mem_phandle, maps[i].mem_fdt);
            if (ret < 0)
                goto err;
        }
        if (maps[i].map_disp && disp_alias) {
            ret = dt_device_add_mem_region(disp_alias, mem_phandle, maps[i].mem_fdt);
            if (ret < 0)
                goto err;
        }
        if (maps[i].map_piodma && piodma_alias) {
            ret = dt_device_add_mem_region(piodma_alias, mem_phandle, maps[i].mem_fdt);
            if (ret < 0)
                goto err;
        }
    }

    /* enable dart-disp0, it is disabled in device tree to avoid resetting
     * it and breaking display scanout when booting with old aura which
     * does not lock dart-disp0.
     */
    if (disp_alias && chip_id != T8140) {
        int disp_node = fdt_path_offset(dt, disp_alias);

        int dart_disp0 = dt_get_iommu_node(disp_node, 0);
        if (dart_disp0 < 0)
            bail_cleanup("FDT: failed to find 'dart-disp0'\n");

        if (fdt_setprop_string(dt, dart_disp0, "status", "okay") < 0)
            bail_cleanup("FDT: failed to enable 'dart-disp0'\n");
    }
    /* T8140 admission is published by its ownership-aware handoff. */
    if (chip_id != T8140) {
        /* Enable the owner and its loader-controlled routes atomically. */
        ret = dt_enable_dcp_with_typec_routes(dcp_alias);
        if (ret)
            bail_cleanup("FDT: failed to enable '%s'\n", dcp_alias);
    }

err:
    if (dart_dcp)
        dart_shutdown(dart_dcp);
    if (dart_disp)
        dart_shutdown(dart_disp);
    if (dart_piodma)
        dart_shutdown(dart_piodma);

    return ret;
}

static int dt_carveout_reserved_regions(const char *dcp_alias, const char *disp_alias,
                                        const char *piodma_alias, struct disp_mapping *maps,
                                        u32 num_maps)
{
    int ret = 0;

    struct mem_region region[MAX_DISP_MAPPINGS];

    assert(num_maps <= MAX_DISP_MAPPINGS);

    // return early if dcp_alias does not exists
    if (!fdt_get_alias(dt, dcp_alias))
        return 0;

    ret = dt_set_dcp_firmware(dcp_alias);
    if (ret)
        return ret;

    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    if (node < 0)
        bail("ADT: '/chosen/carveout-memory-map' not found\n");

    /* read physical addresses of reserved memory regions */
    /* do this up front to avoid errors after modifying the DT */
    for (unsigned i = 0; i < num_maps; i++) {

        int ret;
        u64 phys_map[2];
        struct disp_mapping *map = &maps[i];
        const char *name = map->region_adt;

        ret = ADT_GETPROP_ARRAY(adt, node, name, phys_map);
        if (ret != sizeof(phys_map))
            bail("ADT: could not get carveout memory '%s'\n", name);
        if (!phys_map[0] || !phys_map[1])
            bail("ADT: carveout memory '%s'\n", name);

        region[i].paddr = phys_map[0];
        region[i].size = phys_map[1];
    }

    return dt_add_reserved_regions(dcp_alias, disp_alias, piodma_alias, "apple,asc-mem", maps,
                                   region, num_maps);
}

static struct disp_mapping disp_reserved_regions_vram[] = {
    // boot framebuffer, mapped to dart-disp0 sid 0 and dart-dcp sid 0/5
    {"vram", "framebuffer", true, true, false},
};

static int dt_vram_reserved_region(const char *dcp_alias, const char *disp_alias)
{
    int ret = 0;
    int adt_path[4];
    struct mem_region region;

    // return early if dcp_alias does not exists
    if (!fdt_get_alias(dt, dcp_alias))
        return 0;

    int node = adt_path_offset_trace(adt, "/vram", adt_path);

    if (node < 0)
        bail("ADT: '/vram' not found\n");

    int pp = 0;
    while (adt_path[pp])
        pp++;
    adt_path[pp + 1] = 0;

    ret = adt_get_reg(adt, adt_path, "reg", 0, &region.paddr, &region.size);
    if (ret < 0)
        bail("ADT: failed to read /vram/reg\n");

    return dt_add_reserved_regions(dcp_alias, disp_alias, NULL, "framebuffer",
                                   disp_reserved_regions_vram, &region, 1);
}

/*
 * Reserve the firmware segments of an ASC coprocessor from its ADT
 * segment-ranges and attach them to its FDT node. With dram_only, segments
 * outside DRAM (such as the coprocessor's own SRAM, which the OS maps as MMIO)
 * are left out.
 */
static int dt_reserve_asc_segments(const char *adt_path, const char *adt_path_alt,
                                   const char *fdt_path, bool remap, u64 base, bool dram_only)
{
    int ret = 0;

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0) {
        printf("FDT: '%s' not found\n", fdt_path);
        return 0;
    }

    int node = adt_path_offset(adt, adt_path);
    if (node < 0 && adt_path_alt)
        node = adt_path_offset(adt, adt_path_alt);
    if (node < 0)
        bail("ADT: '%s' not found\n", adt_path);

    u32 segments_len = 0;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &segments_len);
    if (!seg || !segments_len || segments_len % sizeof(*seg))
        bail("ADT: invalid ASC firmware segment list for %s\n", fdt_path);
    unsigned int num_maps = segments_len / sizeof(*seg);

    u64 dram_base = 0, dram_end = 0;
    if (dram_only) {
        int chosen = adt_path_offset(adt, "/chosen");
        u64 dram_size;

        if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base) ||
            ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size) || !dram_size ||
            dram_base > UINT64_MAX - dram_size)
            bail("ADT: no DRAM range for '%s'\n", adt_path);
        dram_end = dram_base + dram_size;
    }

    uint32_t dev_phandle = fdt_get_phandle(dt, fdt_node);
    if (!dev_phandle) {
        ret = fdt_generate_phandle(dt, &dev_phandle);
        if (!ret)
            ret = fdt_setprop_u32(dt, fdt_node, "phandle", dev_phandle);
        if (ret != 0)
            bail("FDT: couldn't set '%s.phandle' property: %d\n", fdt_path, ret);
    }

    for (unsigned i = 0; i < num_maps; i++, seg++) {
        if (dram_only) {
            u64 span = ALIGN_UP((u64)seg->size, SZ_16K);

            if (seg->phys < dram_base || seg->phys >= dram_end || span > dram_end - seg->phys) {
                printf("FDT: %s: segment at 0x%lx is outside DRAM, not reserved\n", fdt_path,
                       seg->phys);
                continue;
            }
        }

        u64 iova = (remap ? seg->remap : seg->iova) | base;

        char node_name[64];
        snprintf(node_name, sizeof(node_name), "asc-firmware@%lx", seg->phys);

        size_t seg_size = seg->size;
        if (seg->size & (SZ_16K - 1)) {
            printf("ADT: segment %s uses non 16k aligned size: 0x%06zx\n", node_name, seg_size);
            seg_size = ALIGN_UP(seg_size, SZ_16K);
        }

        int mem_node =
            dt_get_or_add_reserved_mem(node_name, "apple,asc-mem", true, seg->phys, seg_size);
        if (mem_node < 0)
            return mem_node;
        uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);

        /*
         * The OS log segment (flags 0xa) of a T6030 display coprocessor runs at
         * its physical address and is not reached through the DART: reserve
         * its RAM, marked as such, without an IOMMU mapping.
         */
        if (((chip_id == T6030 && !strncmp(fdt_path, "dcp", 3)) ||
             (chip_id == T8132 && !strcmp(fdt_path, "dcp"))) &&
            seg->unk == 0xa)
            ret = fdt_setprop_empty(dt, mem_node, "apple,dcp-os-log");
        else
            ret = dt_device_set_reserved_mem(mem_node, node_name, dev_phandle, iova, seg_size);
        if (ret < 0)
            return ret;

        ret = dt_device_add_mem_region(fdt_path, mem_phandle, NULL);
        if (ret < 0)
            return ret;
    }

    return 0;
}

static int dt_reserve_asc_firmware(const char *adt_path, const char *adt_path_alt,
                                   const char *fdt_path, bool remap, u64 base)
{
    return dt_reserve_asc_segments(adt_path, adt_path_alt, fdt_path, remap, base, false);
}

/*
 * The AVE video encoder runs firmware that iBoot preloads. The ADT calls the
 * encoder "ave" on the M2 and A18 Pro and "ave0" on the M1 Pro/Max and M4 Pro.
 * The DT node is found by its compatible, so describing a SoC's encoder is all
 * it takes.
 */
static const char *const ave_compatibles[] = {
    "apple,t6000-ave",
    "apple,t8112-ave",
    "apple,t8140-ave",
    "apple,t6041-ave",
};

static int dt_set_ave_node(int node)
{
    char path[64];
    if (fdt_get_path(dt, node, path, sizeof(path)))
        bail("FDT: AVE node path too long\n");

    const char *reason = "invalid ADT firmware segments";
    const char *adt_path = "/arm-io/ave";
    int anode = adt_path_offset(adt, adt_path);
    if (anode < 0) {
        adt_path = "/arm-io/ave0";
        anode = adt_path_offset(adt, adt_path);
    }
    if (anode < 0) {
        reason = "ADT node absent";
        goto disable;
    }

    u32 preloaded;
    if (ADT_GETPROP(adt, anode, "pre-loaded", &preloaded) != sizeof(preloaded) || preloaded != 1) {
        reason = "firmware not preloaded";
        goto disable;
    }

    u32 seg_len, names_len;
    const struct adt_segment_ranges *seg = adt_getprop(adt, anode, "segment-ranges", &seg_len);
    const char *names = adt_getprop(adt, anode, "segment-names", &names_len);
    if (!seg || !names || !seg_len || seg_len % sizeof(*seg) || seg_len > 4 * sizeof(*seg) ||
        !names_len)
        goto disable;

    int dram_node = adt_path_offset(adt, "/chosen");
    u64 dram_base;
    if (dram_node < 0 || ADT_GETPROP(adt, dram_node, "dram-base", &dram_base) != sizeof(dram_base))
        goto disable;
    if (dram_base > cur_boot_args.phys_base)
        goto disable;

    size_t count = seg_len / sizeof(*seg);
    char normalized[4][16] = {{0}};
    u64 phys_end[4], remap_end[4];
    size_t pos = 0;
    for (size_t i = 0; i < count; i++) {
        while (pos < names_len && names[pos] == '_')
            pos++;
        size_t n = 0;
        while (pos < names_len && names[pos] != ';' && names[pos]) {
            char c = names[pos++];
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (n == 15 || !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                goto disable;
            normalized[i][n++] = c;
        }
        if (!n || (i + 1 < count && (pos >= names_len || names[pos++] != ';')))
            goto disable;
        for (size_t j = 0; j < i; j++)
            if (!strcmp(normalized[i], normalized[j]))
                goto disable;

        u64 span = ALIGN_UP((u64)seg[i].size, SZ_16K);
        if (!seg[i].size || !seg[i].remap || (seg[i].phys & (SZ_16K - 1)) ||
            (seg[i].remap & (SZ_16K - 1)) || seg[i].phys > UINT64_MAX - span ||
            seg[i].remap > UINT64_MAX - span || seg[i].phys < dram_base ||
            seg[i].phys + span > cur_boot_args.phys_base)
            goto disable;
        phys_end[i] = seg[i].phys + span;
        remap_end[i] = seg[i].remap + span;
        for (size_t j = 0; j < i; j++)
            if ((seg[i].phys < phys_end[j] && seg[j].phys < phys_end[i]) ||
                (seg[i].remap < remap_end[j] && seg[j].remap < remap_end[i]))
                goto disable;
    }
    if (pos < names_len && (names[pos] != 0 || pos + 1 != names_len))
        goto disable;

    u32 crcs[4];
    for (size_t i = 0; i < count; i++)
        crcs[i] = tinf_crc32((void *)seg[i].phys, seg[i].size);

    if (dt_reserve_asc_firmware(adt_path, NULL, path, true, 0)) {
        reason = "firmware reservation failed";
        goto disable;
    }
    for (size_t i = 0; i < count; i++) {
        char prop[48], value[9];
        snprintf(prop, sizeof(prop), "aurora,ave-%s-crc32", normalized[i]);
        snprintf(value, sizeof(value), "%08x", crcs[i]);
        int chosen = fdt_path_offset(dt, "/chosen");
        if (chosen < 0 || fdt_setprop_string(dt, chosen, prop, value)) {
            reason = "CRC export failed";
            goto disable;
        }
    }
    return 0;

disable:
    node = fdt_path_offset(dt, path);
    if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled"))
        return -1;
    printf("FDT: AVE disabled: %s\n", reason);
    return 0;
}

static int dt_set_ave(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(ave_compatibles); i++) {
        int node = fdt_node_offset_by_compatible(dt, -1, ave_compatibles[i]);
        if (node >= 0)
            return dt_set_ave_node(node);
    }
    return 0;
}

/*
 * The M4 Pro Neural Engine runs firmware that iBoot preloads into DRAM below
 * the OS: /arm-io/ane0 is pre-loaded, with exactly a __TEXT and a __DATA
 * segment range. Reserve both segments no-map and give them to the ANE node
 * as its "firmware-text" and "firmware-data" memory regions, with the device
 * addresses the firmware runs them at (their remap addresses) as
 * apple,firmware-iovas. A DT that describes the two regions as placeholders
 * gets them filled in place, renamed to their physical addresses and
 * enabled; a DT without them gets them added. The node's status is left as
 * the DT has it, except that a node whose preload cannot be handed over is
 * disabled.
 */
static const char *const ane_compatibles[] = {
    "apple,t6041-ane",
};

static int dt_ane_fill_region(int resv, uint32_t phandle, const char *kind, u64 phys, u64 size)
{
    int node = fdt_node_offset_by_phandle(dt, phandle);
    if (node < 0 || fdt_parent_offset(dt, node) != resv)
        return -1;

    char name[48];
    snprintf(name, sizeof(name), "ane-firmware-%s@%lx", kind, phys);
    int other = fdt_subnode_offset(dt, resv, name);
    if (other >= 0 && other != node)
        return -1;

    u64 reg[2] = {cpu_to_fdt64(phys), cpu_to_fdt64(size)};
    if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) || fdt_setprop_empty(dt, node, "no-map") ||
        fdt_setprop_string(dt, node, "status", "okay"))
        return -1;
    node = fdt_node_offset_by_phandle(dt, phandle);
    if (node < 0 || fdt_set_name(dt, node, name))
        return -1;
    return 0;
}

static int dt_set_ane_node(int node)
{
    static const char names_expected[] = "__TEXT;__DATA";
    static const char *const kinds[2] = {"text", "data"};

    char path[64];
    if (fdt_get_path(dt, node, path, sizeof(path)))
        bail("FDT: ANE node path too long\n");

    const char *reason = "invalid ADT firmware segments";
    int anode = adt_path_offset(adt, "/arm-io/ane0");
    if (anode < 0)
        anode = adt_path_offset(adt, "/arm-io/ane");
    if (anode < 0) {
        reason = "ADT node absent";
        goto disable;
    }

    u32 preloaded;
    if (ADT_GETPROP(adt, anode, "pre-loaded", &preloaded) != sizeof(preloaded) || preloaded != 1) {
        reason = "firmware not preloaded";
        goto disable;
    }

    u32 seg_len, names_len;
    const struct adt_segment_ranges *seg = adt_getprop(adt, anode, "segment-ranges", &seg_len);
    const char *names = adt_getprop(adt, anode, "segment-names", &names_len);
    if (!seg || seg_len != 2 * sizeof(*seg) || !names || names_len < sizeof(names_expected) - 1 ||
        names_len > sizeof(names_expected) ||
        memcmp(names, names_expected, sizeof(names_expected) - 1) ||
        (names_len == sizeof(names_expected) && names[names_len - 1]))
        goto disable;

    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram_base;
    if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base) ||
        dram_base > cur_boot_args.phys_base)
        goto disable;

    u64 span[2];
    for (int i = 0; i < 2; i++) {
        span[i] = ALIGN_UP((u64)seg[i].size, SZ_16K);
        if (!seg[i].size || !seg[i].remap || (seg[i].phys & (SZ_16K - 1)) ||
            (seg[i].remap & (SZ_16K - 1)) || seg[i].phys < dram_base ||
            seg[i].phys > cur_boot_args.phys_base ||
            span[i] > cur_boot_args.phys_base - seg[i].phys || seg[i].remap > UINT64_MAX - span[i])
            goto disable;
    }
    if ((seg[0].phys < seg[1].phys + span[1] && seg[1].phys < seg[0].phys + span[0]) ||
        (seg[0].remap < seg[1].remap + span[1] && seg[1].remap < seg[0].remap + span[0]))
        goto disable;

    int resv = fdt_path_offset(dt, "/reserved-memory");
    const fdt32_t *ac = resv < 0 ? NULL : fdt_getprop(dt, resv, "#address-cells", NULL);
    const fdt32_t *sc = resv < 0 ? NULL : fdt_getprop(dt, resv, "#size-cells", NULL);
    if (!ac || !sc || fdt32_ld(ac) != 2 || fdt32_ld(sc) != 2) {
        reason = "no 64-bit /reserved-memory";
        goto disable;
    }

    reason = "firmware reservation failed";
    int len = 0;
    node = fdt_path_offset(dt, path);
    const fdt32_t *regions = node < 0 ? NULL : fdt_getprop(dt, node, "memory-region", &len);
    uint32_t phandles[2];
    if (regions) {
        /* Placeholders from the DT: fill them where they are. */
        if (len != sizeof(phandles))
            goto disable;
        phandles[0] = fdt32_ld(&regions[0]);
        phandles[1] = fdt32_ld(&regions[1]);
        for (int i = 0; i < 2; i++) {
            resv = fdt_path_offset(dt, "/reserved-memory");
            if (dt_ane_fill_region(resv, phandles[i], kinds[i], seg[i].phys, span[i]))
                goto disable;
        }
    } else {
        for (int i = 0; i < 2; i++) {
            char name[48];
            snprintf(name, sizeof(name), "ane-firmware-%s@%lx", kinds[i], seg[i].phys);
            int mem = dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, seg[i].phys, span[i]);
            if (mem < 0)
                goto disable;
            phandles[i] = fdt_get_phandle(dt, mem);
            if (!phandles[i])
                goto disable;
        }
    }

    fdt32_t region_cells[2] = {cpu_to_fdt32(phandles[0]), cpu_to_fdt32(phandles[1])};
    static const char region_names[] = "firmware-text\0firmware-data";
    fdt64_t iovas[2] = {cpu_to_fdt64(seg[0].remap), cpu_to_fdt64(seg[1].remap)};
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop(dt, node, "memory-region", region_cells, sizeof(region_cells)))
        goto disable;
    node = fdt_path_offset(dt, path);
    if (node < 0 ||
        fdt_setprop(dt, node, "memory-region-names", region_names, sizeof(region_names)))
        goto disable;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop(dt, node, "apple,firmware-iovas", iovas, sizeof(iovas)))
        goto disable;

    printf("FDT: %s: ANE firmware TEXT 0x%lx DATA 0x%lx reserved\n", path, seg[0].phys,
           seg[1].phys);
    return 0;

disable:
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_string(dt, node, "status", "disabled"))
        return -1;
    printf("FDT: ANE disabled: %s\n", reason);
    return 0;
}

static int dt_set_ane_firmware(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(ane_compatibles); i++) {
        int node = fdt_node_offset_by_compatible(dt, -1, ane_compatibles[i]);
        if (node >= 0)
            return dt_set_ane_node(node);
    }
    return 0;
}

static const char dcpext_aliases[][8] = {
    "dcpext",  "dcpext0", "dcpext1", "dcpext2", "dcpext3",
    "dcpext4", "dcpext5", "dcpext6", "dcpext7",
};

/* Commit a complete FDT update, or discard every partial edit. */
static int dt_display_transaction(int (*apply)(unsigned int), unsigned int index)
{
    void *original = dt;
    size_t capacity = fdt_totalsize(original);
    void *scratch = malloc(capacity);
    if (!scratch)
        return -1;
    memcpy(scratch, original, capacity);
    dt = scratch;
    int ret = apply(index);
    dt = original;
    if (!ret)
        memcpy(original, scratch, capacity);
    free(scratch);
    return ret;
}

#define T8122_DCPEXT_READY "apple,t8122-dcpext-firmware-ready"

static int dt_reserve_t8122_dcpext_one(unsigned int n)
{
    if (n >= ARRAY_SIZE(dcpext_aliases))
        return -1;
    const char *alias = dcpext_aliases[n];
    char path[48];
    snprintf(path, sizeof(path), "/arm-io/%s/iop-%s-nub", alias, alias);
    int nub = adt_path_offset(adt, path);
    int chosen = adt_path_offset(adt, "/chosen");
    u32 len = 0;
    const struct adt_segment_ranges *segments =
        nub < 0 ? NULL : adt_getprop(adt, nub, "segment-ranges", &len);
    u64 dram_base = 0, dram_size = 0;
    if (!segments || !len || len % sizeof(*segments) || len / sizeof(*segments) > 64 ||
        chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base) ||
        ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size) || !dram_size ||
        dram_base > UINT64_MAX - dram_size)
        return -1;
    for (u32 i = 0; i < len / sizeof(*segments); i++) {
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);
        if (!size || (segments[i].phys & (SZ_16K - 1)) || segments[i].phys < dram_base ||
            size > dram_size || segments[i].phys - dram_base > dram_size - size ||
            segments[i].remap > UINT64_MAX - size)
            return -1;
    }
    if (dt_reserve_asc_firmware(path, NULL, alias, true, 0) || dt_set_dcp_firmware(alias))
        return -1;
    int node = fdt_path_offset(dt, alias);
    return node < 0 ? node : fdt_setprop_u32(dt, node, T8122_DCPEXT_READY, 1);
}

/*
 * T8122: reserve the external DCP firmware for the kernel's gate only with the boot.bin config
 * line chosen.asahi,t8122-dcpext=1, and only when the internal display was handed over
 * (apple,t8122-handoff on dcp), the one case where the kernel would enable it. Off by default:
 * on an M3 Air with 14.x firmware, a published marker lets the kernel enable dcpext, and the
 * internal DCP then crashes after disp0 DART faults, leaving the panel dark.
 */
static bool dt_t8122_dcpext_armed(void)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t8122-dcpext", &len);

    if (!opt || len != 2 || opt[0] != '1' || opt[1] != '\0')
        return false;
    int dcp = fdt_path_offset(dt, "dcp");
    return dcp >= 0 && fdt_getprop(dt, dcp, "apple,t8122-handoff", NULL);
}

/*
 * T8122 firmware-ready handoff adapted from Marius Gehrmann's public patch.
 * Copyright (c) 2026 Marius Gehrmann; additional MIT notice:
 * 3rdparty_licenses/LICENSE.Marius-Gehrmann.
 *
 * With enable=false, leave the controller disabled for the kernel's gate, and reserve its
 * firmware only when dt_t8122_dcpext_armed(). A previous promise is withdrawn either way.
 */
static int dt_reserve_dcpext_firmware(bool enable)
{
    if (!enable && (chip_id != T8122 || fdt_node_check_compatible(dt, 0, "apple,t8122")))
        return 0;
    /* reserve asc for all  */
    for (size_t n = 0; n < ARRAY_SIZE(dcpext_aliases); n++) {
        const char *dcpext_alias = dcpext_aliases[n];
        char adt_path[48];

        snprintf(adt_path, sizeof(adt_path) - 1, "/arm-io/%s/iop-%s-nub", dcpext_alias,
                 dcpext_alias);
        adt_path[sizeof(adt_path) - 1] = '\0';

        if (enable && adt_path_offset(adt, adt_path) < 0)
            continue;

        int dcpext_node = fdt_path_offset(dt, dcpext_alias);
        if (dcpext_node < 0)
            continue;

        if (!enable) {
            /* Withdraw a previous promise before attempting a fresh transaction. */
            int ret = fdt_delprop(dt, dcpext_node, T8122_DCPEXT_READY);
            if (ret && ret != -FDT_ERR_NOTFOUND)
                return ret;
            if (!dt_t8122_dcpext_armed()) {
                printf("FDT: '%s' firmware not reserved (needs chosen.asahi,t8122-dcpext=1 "
                       "and the internal display handoff); kernel gate remains closed\n",
                       dcpext_alias);
                continue;
            }
            dcpext_node = fdt_path_offset(dt, dcpext_alias);
            int len = 0;
            const char *status = fdt_getprop(dt, dcpext_node, "status", &len);
            if (!status || len != sizeof("disabled") || strcmp(status, "disabled") ||
                fdt_node_check_compatible(dt, dcpext_node, "apple,t8122-dcpext"))
                continue;
            if (dt_display_transaction(dt_reserve_t8122_dcpext_one, n))
                printf("FDT: '%s' firmware not ready; kernel gate remains closed\n", dcpext_alias);
            else
                printf("FDT: '%s' firmware reserved, left to the kernel\n", dcpext_alias);
            continue;
        }

        int ret = dt_reserve_asc_firmware(adt_path, NULL, dcpext_alias, true, 0);
        if (ret < 0)
            bail("FDT: reserving ASC firmware for %s failed!\n", dcpext_alias);

        ret = dt_set_dcp_firmware(dcpext_alias);
        if (ret)
            continue;

        ret = dt_enable_dcp_with_typec_routes(dcpext_alias);
        if (ret)
            printf("FDT: failed to enable '%s'\n", dcpext_alias);
    }

    return 0;
}

static struct disp_mapping disp_reserved_regions_t8103[] = {
    {"region-id-50", "dcp_data", true, false, false},
    {"region-id-57", "region57", true, false, false},
    // The 2 following regions are mapped in dart-dcp sid 0 and dart-disp0 sid 0 and 4
    {"region-id-94", "region94", true, true, false},
    {"region-id-95", "region95", true, false, true},
};

static struct disp_mapping dcpext_reserved_regions_t8103[] = {
    {"region-id-73", "dcpext_data", true, false, false},
    {"region-id-74", "region74", true, false, false},
};

static struct disp_mapping disp_reserved_regions_t8112[] = {
    {"region-id-49", "asc-firmware", true, false, false},
    {"region-id-50", "dcp_data", true, false, false},
    {"region-id-57", "region57", true, false, false},
    // The 2 following regions are mapped in dart-dcp sid 5 and dart-disp0 sid 0 and 4
    {"region-id-94", "region94", true, true, false},
    {"region-id-95", "region95", true, false, true},
};

static struct disp_mapping disp_reserved_regions_t600x[] = {
    {"region-id-50", "dcp_data", true, false, false},
    {"region-id-57", "region57", true, false, false},
    // The 2 following regions are mapped in dart-dcp sid 0 and dart-disp0 sid 0 and 4
    {"region-id-94", "region94", true, true, false},
    {"region-id-95", "region95", true, false, true},
    // used on M1 Pro/Max/Ultra, mapped to dcp and disp0
    {"region-id-157", "region157", true, true, false},
};

#define MAX_DCPEXT 8

static struct disp_mapping dcpext_reserved_regions_t600x[MAX_DCPEXT][2] = {
    {
        {"region-id-73", "dcpext0_data", true, false, false},
        {"region-id-74", "", true, false, false},
    },
    {
        {"region-id-88", "dcpext1_data", true, false, false},
        {"region-id-89", "region89", true, false, false},
    },
    {
        {"region-id-111", "dcpext2_data", true, false, false},
        {"region-id-112", "region112", true, false, false},
    },
    {
        {"region-id-119", "dcpext3_data", true, false, false},
        {"region-id-120", "region120", true, false, false},
    },
    {
        {"region-id-127", "dcpext4_data", true, false, false},
        {"region-id-128", "region128", true, false, false},
    },
    {
        {"region-id-135", "dcpext5_data", true, false, false},
        {"region-id-136", "region136", true, false, false},
    },
    {
        {"region-id-143", "dcpext6_data", true, false, false},
        {"region-id-144", "region144", true, false, false},
    },
    {
        {"region-id-151", "dcpext7_data", true, false, false},
        {"region-id-152", "region152", true, false, false},
    },
};

static struct disp_mapping disp_reserved_regions_t602x[] = {
    {"region-id-49", "asc-firmware", true, false, false},
    {"region-id-50", "dcp_data", true, false, false},
    {"region-id-57", "region57", true, false, false},
    // The 2 following regions are mapped in dart-dcp sid 0 and dart-disp0 sid 0 and 4
    {"region-id-94", "region94", true, true, false},
    {"region-id-95", "region95", true, false, true},
    // used on M1 Pro/Max/Ultra, mapped to dcp and disp0
    {"region-id-157", "region157", true, true, false},
};

/* Reserve a measured carveout only if the live, locked DART has a complete
 * contiguous translation for it. The ADT supplies physical ownership, not
 * an authoritative IOVA. */
struct dt_t8140_dram {
    u64 base, size;
};

/* These opaque disp0 inputs are described by the public apple,dcp binding.
 * Keep their firmware byte order; they are not FDT integer cells. */
static int dt_t8140_copy_provider_props(const char *dcp_path)
{
    static const struct {
        const char *name;
        u32 size;
    } props[] = {
        {"power-lut-data-x", 4},      {"power-lut-data-y", 4},    {"power-lut-data-xindex", 52},
        {"power-lut-data-yindex", 4}, {"power-lut-data-lut", 52},
    };
    const void *values[ARRAY_SIZE(props)];
    int disp = adt_path_offset(adt, "/arm-io/disp0");
    if (disp < 0)
        return 0; /* Leave absent data to the kernel's qualified fallback. */

    /* Validate all present inputs before modifying the transactional FDT. */
    for (size_t i = 0; i < ARRAY_SIZE(props); i++) {
        u32 size = 0;
        values[i] = adt_getprop(adt, disp, props[i].name, &size);
        if (values[i] && size != props[i].size) {
            printf("FDT: disp0 %s has %u bytes, expected %u\n", props[i].name, size, props[i].size);
            return -FDT_ERR_BADVALUE;
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(props); i++) {
        if (!values[i])
            continue;
        int node = fdt_path_offset(dt, dcp_path);
        if (node < 0)
            return node;
        char name[64];
        snprintf(name, sizeof(name), "apple,%s", props[i].name);
        int ret = fdt_setprop(dt, node, name, values[i], props[i].size);
        if (ret)
            return ret;
    }
    return 0;
}

static bool dt_t8140_in_physical_dram(const struct dt_t8140_dram *dram, u64 base, u64 size)
{
    return size && base >= dram->base && base <= dram->base + dram->size &&
           size <= dram->base + dram->size - base;
}

static int dt_t8140_reserve_carveout(const char *dcp_path, const char *disp_path,
                                     const char *region_id, const char *name, bool map_disp,
                                     const struct dt_t8140_dram *dram)
{
    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u64 pair[2] = {0};
    int len = node < 0 ? -1 : ADT_GETPROP_ARRAY(adt, node, region_id, pair);
    if (len != sizeof(pair) || !pair[0] || !pair[1] || (pair[0] & (SZ_16K - 1)) ||
        (pair[1] & (SZ_16K - 1)) || pair[0] > UINT64_MAX - pair[1]) {
        printf("FDT: T8140 %s/%s: invalid ADT %s PA 0x%lx size 0x%lx (len %d)\n", dcp_path, name,
               region_id, pair[0], pair[1], len);
        return -1;
    }
    if (!dt_t8140_in_physical_dram(dram, pair[0], pair[1])) {
        printf("FDT: T8140 %s/%s: ADT %s PA 0x%lx size 0x%lx outside physical DRAM"
               " 0x%lx..0x%lx\n",
               dcp_path, name, region_id, pair[0], pair[1], dram->base, dram->base + dram->size);
        return -1;
    }

    struct disp_mapping map = {0};
    struct mem_region region = {pair[0], pair[1]};
    snprintf(map.mem_fdt, sizeof(map.mem_fdt), "%s", name);
    map.map_dcp = true;
    map.map_disp = map_disp;
    return dt_add_reserved_regions(dcp_path, disp_path, NULL, "apple,asc-mem", &map, &region, 1);
}

/* Region 50/73 ends with the nub's shared-data segment. Its physical pages
 * are adjacent, but the live DART puts the final segment at a different IOVA.
 * Export two truthful translations instead of one invented contiguous one. */
static int dt_t8140_reserve_data(const char *dcp_path, const char *nub_path, const char *region_id,
                                 const char *name, const struct dt_t8140_dram *dram)
{
    int node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u64 pair[2] = {0};
    int pair_len = node < 0 ? -1 : ADT_GETPROP_ARRAY(adt, node, region_id, pair);
    if (pair_len != sizeof(pair)) {
        printf("FDT: T8140 %s/%s: missing ADT %s (len %d)\n", dcp_path, name, region_id, pair_len);
        return -1;
    }
    node = adt_path_offset(adt, nub_path);
    u32 len = 0;
    const struct adt_segment_ranges *seg =
        node < 0 ? NULL : adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || len < 2 * sizeof(*seg) || len % sizeof(*seg) || len > 32 * sizeof(*seg) ||
        !pair[0] || !pair[1] || (pair[0] & (SZ_16K - 1)) || (pair[1] & (SZ_16K - 1)) ||
        pair[0] > UINT64_MAX - pair[1]) {
        printf("FDT: T8140 %s/%s: invalid ADT %s PA 0x%lx size 0x%lx; %s segment len %u\n",
               dcp_path, name, region_id, pair[0], pair[1], nub_path, len);
        return -1;
    }
    if (!dt_t8140_in_physical_dram(dram, pair[0], pair[1])) {
        printf("FDT: T8140 %s/%s: ADT %s PA 0x%lx size 0x%lx outside physical DRAM"
               " 0x%lx..0x%lx\n",
               dcp_path, name, region_id, pair[0], pair[1], dram->base, dram->base + dram->size);
        return -1;
    }
    if (!seg[1].size || (seg[1].phys & (SZ_16K - 1)) || (seg[1].size & (SZ_16K - 1)) ||
        seg[1].size >= pair[1] || seg[1].phys != pair[0] + pair[1] - seg[1].size) {
        printf("FDT: T8140 %s/%s: %s tail PA 0x%lx size 0x%x does not end"
               " ADT %s PA 0x%lx size 0x%lx\n",
               dcp_path, name, nub_path, seg[1].phys, seg[1].size, region_id, pair[0], pair[1]);
        return -1;
    }

    struct mem_region regions[2] = {{pair[0], pair[1] - seg[1].size}, {seg[1].phys, seg[1].size}};
    struct disp_mapping maps[2] = {0};
    snprintf(maps[0].mem_fdt, sizeof(maps[0].mem_fdt), "%s", name);
    snprintf(maps[1].mem_fdt, sizeof(maps[1].mem_fdt), "%s_tail", name);
    maps[0].map_dcp = maps[1].map_dcp = true;
    return dt_add_reserved_regions(dcp_path, NULL, NULL, "apple,asc-mem", maps, regions, 2);
}

/* The DART helpers below read each specifier as <phandle stream>: require that width. */
static bool dt_t8140_one_cell_iommu(int provider)
{
    int len;
    const fdt32_t *cells = fdt_getprop(dt, provider, "#iommu-cells", &len);
    return cells && len == sizeof(*cells) && fdt32_to_cpu(*cells) == 1;
}

/* A display DART is published only with its firmware tables reserved. */
static bool dt_t8140_dart_handed_off(int dart)
{
    int len;
    const fdt32_t *cell =
        dart < 0 ? NULL : fdt_getprop(dt, dart, "apple,firmware-root-handoff", &len);
    return cell && len == sizeof(*cell) && fdt32_to_cpu(*cell) == 1;
}

static bool dt_t8140_node_enabled(int node)
{
    int length;
    const char *status = fdt_getprop(dt, node, "status", &length);
    if (!status)
        return length == -FDT_ERR_NOTFOUND;
    return (length == 5 && !memcmp(status, "okay", 5)) || (length == 3 && !memcmp(status, "ok", 3));
}

static int dt_t8140_audit_iommus(int node)
{
    int len;
    const fdt32_t *refs = fdt_getprop(dt, node, "iommus", &len);
    if (!refs)
        return len == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (len <= 0 || len % (2 * sizeof(*refs)))
        return -1;
    for (int i = 0; i < len / (int)sizeof(*refs); i += 2) {
        int dart = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(refs[i]));
        if (dart < 0 || !dt_t8140_one_cell_iommu(dart) || !dt_t8140_node_enabled(dart) ||
            !dt_t8140_dart_handed_off(dart))
            return -1;
    }
    return 0;
}

/* Linux drops every translation of a consumer whose IOMMU list names a
 * disabled DART. Before publication, each enabled display consumer and its
 * PIODMA child may reference only enabled DARTs with reserved tables. */
static int dt_t8140_audit_display_iommus(void)
{
    static const char *const compatibles[] = {
        "apple,t8140-dcp",
        "apple,t8140-dcpext",
        "apple,display-subsystem",
    };
    for (size_t i = 0; i < ARRAY_SIZE(compatibles); i++) {
        int node = -1;
        while ((node = fdt_node_offset_by_compatible(dt, node, compatibles[i])) >= 0) {
            if (!dt_t8140_node_enabled(node))
                continue;
            if (dt_t8140_audit_iommus(node)) {
                printf("FDT: T8140 %s references an unpublished DART\n",
                       fdt_get_name(dt, node, NULL));
                return -1;
            }
            int piodma = fdt_subnode_offset(dt, node, "piodma");
            if (piodma < 0 && piodma != -FDT_ERR_NOTFOUND)
                return -1;
            if (piodma >= 0 && dt_t8140_audit_iommus(piodma)) {
                printf("FDT: T8140 %s/piodma references an unpublished DART\n",
                       fdt_get_name(dt, node, NULL));
                return -1;
            }
        }
        if (node != -FDT_ERR_NOTFOUND)
            return -1;
    }
    return 0;
}

static int dt_t8140_enable_consumer(const char *path)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0) {
        printf("FDT: T8140 %s: consumer node missing (%d)\n", path, node);
        return -1;
    }
    int dart = dt_get_iommu_node(node, 0);
    if (dart < 0) {
        printf("FDT: T8140 %s: IOMMU node missing (%d)\n", path, dart);
        return -1;
    }
    if (!dt_t8140_dart_handed_off(dart)) {
        printf("FDT: T8140 %s: IOMMU has no reserved firmware tables\n", path);
        return -1;
    }
    int ret = fdt_setprop_string(dt, dart, "status", "okay");
    if (ret < 0) {
        printf("FDT: T8140 %s: cannot enable IOMMU (%d)\n", path, ret);
        return -1;
    }
    node = fdt_path_offset(dt, path);
    ret = node < 0 ? node : fdt_setprop_string(dt, node, "status", "okay");
    if (ret < 0)
        printf("FDT: T8140 %s: cannot enable consumer (%d)\n", path, ret);
    return ret;
}

static bool dt_t8140_validate_dcp_asc(const char *path, const char *name,
                                      enum asc_inherited_state state)
{
    asc_dev_t *asc = asc_init(path);
    if (!asc) {
        printf("FDT: T8140 %s: ASC unavailable at %s\n", name, path);
        return false;
    }
    bool ready = asc_validate_inherited_dcp(asc, name, state);
    asc_free(asc);
    return ready;
}

static bool dt_t8140_table_reservations_fit(void)
{
    u64 start = cur_boot_args.phys_base;
    if (start > UINT64_MAX - cur_boot_args.mem_size)
        return false;
    u64 end = start + cur_boot_args.mem_size;
    int parent = fdt_path_offset(dt, "/reserved-memory"), node;
    unsigned int regions = 1;
    fdt_for_each_subnode(node, dt, parent)
    {
        const char *status = fdt_getprop(dt, node, "status", NULL);
        if ((status && !strcmp(status, "disabled")) || fdt_getprop(dt, node, "no-map", NULL))
            continue;
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg)
            continue;
        if (len < 2 * (int)sizeof(*reg))
            return false;
        u64 base = fdt64_ld(&reg[0]), size = fdt64_ld(&reg[1]);
        if (base > UINT64_MAX - size)
            return false;
        if (base >= start && base + size <= end)
            continue;
        if ((base < start && base + size > start) || (base < end && base + size > end) ||
            (base | size) & (SZ_16K - 1) || ++regions > MAX_MEM_REGIONS)
            return false;
    }
    return true;
}

/* The display subsystem lists the internal and external scanout DARTs
 * together. A DART referenced by the external controller or its PIODMA child
 * belongs to the external display; every other display DART is internal.
 * Each half reserves and publishes only the DARTs it owns, so the internal
 * handoff never depends on external translations. */
struct dt_t8140_external_darts {
    u32 phandles[16];
    size_t count;
};

static int dt_t8140_collect_external_darts(struct dt_t8140_external_darts *ext)
{
    ext->count = 0;
    int controller = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
    if (controller < 0)
        return controller == -FDT_ERR_NOTFOUND ? 0 : -1;
    int piodma = fdt_subnode_offset(dt, controller, "piodma");
    if (piodma < 0 && piodma != -FDT_ERR_NOTFOUND)
        return -1;
    const int nodes[] = {controller, piodma};
    for (size_t n = 0; n < ARRAY_SIZE(nodes); n++) {
        if (nodes[n] < 0)
            continue;
        int len;
        const fdt32_t *refs = fdt_getprop(dt, nodes[n], "iommus", &len);
        if (!refs) {
            if (len != -FDT_ERR_NOTFOUND)
                return -1;
            continue;
        }
        if (len <= 0 || len % (2 * sizeof(*refs)))
            return -1;
        for (int i = 0; i < len / (int)sizeof(*refs); i += 2) {
            u32 phandle = fdt32_to_cpu(refs[i]);
            int provider = fdt_node_offset_by_phandle(dt, phandle);
            if (provider < 0 || !dt_t8140_one_cell_iommu(provider))
                return -1;
            size_t j = 0;
            while (j < ext->count && ext->phandles[j] != phandle)
                j++;
            if (j < ext->count)
                continue;
            if (ext->count == ARRAY_SIZE(ext->phandles))
                return -1;
            ext->phandles[ext->count++] = phandle;
        }
    }
    return 0;
}

static bool dt_t8140_external_dart(const struct dt_t8140_external_darts *ext, u32 phandle)
{
    for (size_t i = 0; i < ext->count; i++)
        if (ext->phandles[i] == phandle)
            return true;
    return false;
}

static int dt_t8140_reserve_tables(const char *alias, const struct dt_t8140_dram *dram,
                                   const struct dt_t8140_external_darts *ext, bool external)
{
    int node = fdt_path_offset(dt, alias), len;
    const fdt32_t *prop = node < 0 ? NULL : fdt_getprop(dt, node, "iommus", &len);
    if (!prop || len <= 0 || len % (2 * sizeof(*prop)) || len > (int)(16 * sizeof(*prop)))
        return -1;
    fdt32_t iommus[16];
    memcpy(iommus, prop, len);
    u64 *pages = malloc(2049 * sizeof(*pages));
    if (!pages)
        return -1;
    int ret = -1;
    for (int i = 0; i < len / (int)sizeof(*prop); i += 2) {
        ret = -1;
        int provider = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(iommus[i]));
        if (provider < 0 || !dt_t8140_one_cell_iommu(provider) ||
            fdt32_to_cpu(iommus[i + 1]) >= 256)
            goto out;
        if (dt_t8140_external_dart(ext, fdt32_to_cpu(iommus[i])) != external)
            continue;
        dart_dev_t *dart =
            dart_init_fdt_locked(dt, fdt32_to_cpu(iommus[i]), fdt32_to_cpu(iommus[i + 1]));
        size_t count;
        if (!dart)
            goto out;
        ret = dart_collect_locked_tables(dart, dram->base, dram->size, pages, 2049, &count);
        dart_shutdown(dart);
        if (ret)
            goto out;
        for (size_t j = 1; j < count; j++) {
            u64 page = pages[j];
            size_t k = j;
            while (k && pages[k - 1] > page) {
                pages[k] = pages[k - 1];
                k--;
            }
            pages[k] = page;
        }
        for (size_t j = 0; j < count;) {
            u64 base = pages[j], size = SZ_16K;
            while (++j < count && pages[j] == base + size)
                size += SZ_16K;
            char name[64];
            snprintf(name, sizeof(name), "dart-tables@%lx", base);
            int parent = fdt_path_offset(dt, "/reserved-memory");
            int existing = parent < 0 ? parent : fdt_subnode_offset(dt, parent, name);
            int reg_len;
            const fdt64_t *reg = existing < 0 ? NULL : fdt_getprop(dt, existing, "reg", &reg_len);
            if (reg) {
                if (reg_len != 2 * sizeof(*reg) || fdt64_ld(&reg[0]) != base ||
                    fdt64_ld(&reg[1]) > dram->size ||
                    !dt_t8140_in_physical_dram(dram, base, fdt64_ld(&reg[1]))) {
                    ret = -1;
                    goto out;
                }
                if (fdt64_ld(&reg[1]) > size)
                    size = fdt64_ld(&reg[1]);
            }
            int region =
                dt_get_or_add_reserved_mem(name, "apple,firmware-dart-tables", false, base, size);
            if (region < 0 || fdt_getprop(dt, region, "no-map", NULL)) {
                ret = -1;
                goto out;
            }
        }
        if (!dt_t8140_table_reservations_fit()) {
            ret = -1;
            goto out;
        }
        int target = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(iommus[i]));
        if (target < 0 || fdt_setprop_u32(dt, target, "apple,firmware-root-handoff", 1)) {
            ret = -1;
            goto out;
        }
    }
    ret = 0;
out:
    free(pages);
    return ret;
}

static int dt_t8140_reserve_piodma_tables(const char *alias, bool required,
                                          const struct dt_t8140_dram *dram,
                                          const struct dt_t8140_external_darts *ext, bool external)
{
    int parent = fdt_path_offset(dt, alias);
    int child = parent < 0 ? parent : fdt_subnode_offset(dt, parent, "piodma");
    if (child == -FDT_ERR_NOTFOUND && !required)
        return 0;
    char path[128];
    if (child < 0 || fdt_get_path(dt, child, path, sizeof(path)))
        return -1;
    return dt_t8140_reserve_tables(path, dram, ext, external);
}

/* Reserve the external streams of every scanout master. */
static int dt_t8140_reserve_master_tables(const struct dt_t8140_dram *dram,
                                          const struct dt_t8140_external_darts *ext)
{
    char paths[8][128];
    size_t count = 0;
    int node = -1;
    while ((node = fdt_node_offset_by_compatible(dt, node, "apple,display-subsystem")) >= 0) {
        int len;
        if (!fdt_getprop(dt, node, "iommus", &len)) {
            if (len != -FDT_ERR_NOTFOUND)
                return -1;
            continue;
        }
        if (count == ARRAY_SIZE(paths) || fdt_get_path(dt, node, paths[count], sizeof(paths[0])))
            return -1;
        count++;
    }
    if (node != -FDT_ERR_NOTFOUND)
        return -1;
    for (size_t i = 0; i < count; i++)
        if (dt_t8140_reserve_tables(paths[i], dram, ext, true))
            return -1;
    return 0;
}

static int dt_t8140_disable_refs(int node, const char *prop, const char *cells_prop)
{
    int len;
    const fdt32_t *refs = fdt_getprop(dt, node, prop, &len);
    if (!refs)
        return len == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (len < 0 || len % (int)sizeof(*refs) || len > 64 * (int)sizeof(*refs))
        return -1;

    u32 phandles[64];
    size_t count = 0;
    for (int i = 0; i < len / (int)sizeof(*refs);) {
        u32 phandle = fdt32_to_cpu(refs[i]);
        int target = fdt_node_offset_by_phandle(dt, phandle);
        int cells_len;
        const fdt32_t *args = target < 0 ? NULL : fdt_getprop(dt, target, cells_prop, &cells_len);
        if (!args || cells_len != (int)sizeof(*args))
            return -1;
        u32 arg_count = fdt32_to_cpu(*args);
        if (arg_count > 16 || arg_count >= (u32)(len / (int)sizeof(*refs) - i))
            return -1;
        phandles[count++] = phandle;
        i += arg_count + 1;
    }
    for (size_t i = 0; i < count; i++) {
        int target = fdt_node_offset_by_phandle(dt, phandles[i]);
        if (target < 0)
            return -1;
        if (!strcmp(cells_prop, "#iommu-cells")) {
            int ret = fdt_delprop(dt, target, "apple,firmware-root-handoff");
            if (ret && ret != -FDT_ERR_NOTFOUND)
                return -1;
            target = fdt_node_offset_by_phandle(dt, phandles[i]);
        }
        if (target < 0 || fdt_setprop_string(dt, target, "status", "disabled"))
            return -1;
    }
    return 0;
}

/* Validate optional aliases before libfdt follows their property as a C string. */
static int dt_t8140_alias_offset(const char *alias)
{
    int aliases = fdt_path_offset(dt, "/aliases");
    if (aliases < 0)
        return aliases;
    int length;
    const char *path = fdt_getprop(dt, aliases, alias, &length);
    if (!path)
        return length;
    /* Relative aliases can recurse, and malformed strings can overrun the property. */
    if (length < 2 || path[0] != '/' || memchr(path, '\0', length) != path + length - 1)
        return -FDT_ERR_BADVALUE;
    int node = fdt_path_offset(dt, path);
    return node == -FDT_ERR_NOTFOUND ? -FDT_ERR_BADVALUE : node;
}

/* A PD controller linked to a Type-C route on a disabled DCP would wait for a
 * mode switch that never registers. */
static int dt_t8140_disable_typec_routes(const char *dcp_path)
{
    char names[4][32];
    size_t count = 0;
    int route;

    int node = fdt_path_offset(dt, dcp_path);
    if (node < 0)
        return -1;
    int routes = fdt_subnode_offset(dt, node, "typec-routes");
    if (routes < 0)
        return routes == -FDT_ERR_NOTFOUND ? 0 : -1;
    fdt_for_each_subnode(route, dt, routes)
    {
        int length;
        const char *name = fdt_get_name(dt, route, &length);
        if (!name || length < 0 || (size_t)length >= sizeof(names[0]) || count == ARRAY_SIZE(names))
            return -1;
        memcpy(names[count++], name, length + 1);
    }
    if (route != -FDT_ERR_NOTFOUND)
        return -1;
    for (size_t i = 0; i < count; i++) {
        char path[160];
        int length = snprintf(path, sizeof(path), "%s/typec-routes/%s", dcp_path, names[i]);
        if (length < 0 || (size_t)length >= sizeof(path))
            return -1;
        node = fdt_path_offset(dt, path);
        if (node < 0 || fdt_setprop_string(dt, node, "status", "disabled") < 0)
            return -1;
    }
    return 0;
}

/* Follow the bounded, zero-cell power-domain chain described by the DT. */
static int dt_t8140_power_paths(int node, char (*paths)[128], size_t capacity)
{
    if (!capacity || capacity > 3)
        return -1;
    int count = 0;
    while (node >= 0) {
        int length;
        const fdt32_t *ref = fdt_getprop(dt, node, "power-domains", &length);
        if (!ref)
            return length == -FDT_ERR_NOTFOUND ? count : -1;
        if (length != sizeof(*ref) || (size_t)count == capacity)
            return -1;
        node = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(*ref));
        if (node < 0 || fdt_get_path(dt, node, paths[count], sizeof(paths[0])))
            return -1;
        for (int i = 0; i < count; i++)
            if (!strcmp(paths[i], paths[count]))
                return -1;
        const fdt32_t *cells = fdt_getprop(dt, node, "#power-domain-cells", &length);
        if (!cells || length != sizeof(*cells) || fdt32_to_cpu(*cells))
            return -1;
        count++;
    }
    return -1;
}

/* Publish the external controller only when its complete dependency group can
 * be enabled. Node offsets must be refreshed after each FDT mutation. */
static int dt_t8140_enable_external(const char *path)
{
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return -1;
    int piodma = fdt_subnode_offset(dt, node, "piodma");
    int dcp_dart = dt_get_iommu_node(node, 0);
    int display_dart = piodma < 0 ? piodma : dt_get_iommu_node(piodma, 0);
    int length = 0;
    const fdt32_t *mailbox = fdt_getprop(dt, node, "mboxes", &length);
    if (piodma < 0 || dcp_dart < 0 || display_dart < 0 || !mailbox || length != sizeof(*mailbox))
        return -1;
    int mbox = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(*mailbox));
    if (mbox < 0)
        return -1;
    /* Both external DARTs keep firmware translations Linux adopts. */
    if (!dt_t8140_dart_handed_off(dcp_dart) || !dt_t8140_dart_handed_off(display_dart))
        return -1;

    int nodes[] = {dcp_dart, display_dart, mbox, piodma, node};
    char paths[8][128];
    int power_count = dt_t8140_power_paths(node, &paths[ARRAY_SIZE(nodes)],
                                           ARRAY_SIZE(paths) - ARRAY_SIZE(nodes));
    if (power_count <= 0)
        return -1;
    size_t count = ARRAY_SIZE(nodes) + power_count;
    for (size_t i = 0; i < ARRAY_SIZE(nodes); i++)
        if (fdt_get_path(dt, nodes[i], paths[i], sizeof(paths[i])))
            return -1;

    void *saved = malloc(dt_bufsize);
    if (!saved)
        return -1;
    memcpy(saved, dt, dt_bufsize);
    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        node = fdt_path_offset(dt, paths[i]);
        ret = node < 0 ? node : fdt_setprop_string(dt, node, "status", "okay");
        if (ret)
            break;
    }
    if (ret)
        memcpy(dt, saved, dt_bufsize);
    free(saved);
    return ret;
}

/* A specifier occupies its provider phandle and the provider's complete argument list. */
static int dt_t8140_iommu_width(const fdt32_t *cells, int remaining)
{
    if (remaining <= 0)
        return -1;
    int provider = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(cells[0]));
    int length;
    const fdt32_t *args = provider < 0 ? NULL : fdt_getprop(dt, provider, "#iommu-cells", &length);
    if (!args || length != sizeof(*args))
        return -1;
    u32 count = fdt32_to_cpu(*args);
    if (count > 16 || count >= (u32)remaining)
        return -1;
    return count + 1;
}

/* Withdraw only the external scanout DART from component masters. Parse every
 * specifier before editing and retain paths rather than offsets across writes. */
static int dt_t8140_withdraw_external_scanout(int external)
{
    enum { MAX_CELLS = 64, MAX_MASTERS = 8 };
    struct {
        char path[256];
        fdt32_t cells[MAX_CELLS];
        int count;
    } edits[MAX_MASTERS];
    u32 external_darts[MAX_CELLS];
    int n_darts = 0, n_edits = 0, length;
    int piodma = fdt_subnode_offset(dt, external, "piodma");
    if (piodma == -FDT_ERR_NOTFOUND)
        return 0;
    if (piodma < 0)
        return -1;
    const fdt32_t *refs = fdt_getprop(dt, piodma, "iommus", &length);
    if (!refs)
        return length == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (length < 0 || length % sizeof(*refs) || length > (int)sizeof(external_darts))
        return -1;
    int count = length / sizeof(*refs);
    for (int i = 0; i < count;) {
        int width = dt_t8140_iommu_width(&refs[i], count - i);
        if (width < 0)
            return -1;
        external_darts[n_darts++] = fdt32_to_cpu(refs[i]);
        i += width;
    }
    if (!n_darts)
        return 0;

    int master = -1;
    while ((master = fdt_node_offset_by_compatible(dt, master, "apple,display-subsystem")) >= 0) {
        refs = fdt_getprop(dt, master, "iommus", &length);
        if (!refs) {
            if (length == -FDT_ERR_NOTFOUND)
                continue;
            return -1;
        }
        if (length < 0 || length % sizeof(*refs) || length > MAX_CELLS * (int)sizeof(*refs))
            return -1;
        fdt32_t retained[MAX_CELLS];
        int kept = 0;
        count = length / sizeof(*refs);
        for (int i = 0; i < count;) {
            int width = dt_t8140_iommu_width(&refs[i], count - i);
            if (width < 0)
                return -1;
            bool remove = false;
            for (int j = 0; j < n_darts; j++)
                if (fdt32_to_cpu(refs[i]) == external_darts[j])
                    remove = true;
            if (!remove) {
                memcpy(&retained[kept], &refs[i], width * sizeof(*refs));
                kept += width;
            }
            i += width;
        }
        if (kept == count)
            continue;
        if (n_edits == MAX_MASTERS ||
            fdt_get_path(dt, master, edits[n_edits].path, sizeof(edits[0].path)))
            return -1;
        memcpy(edits[n_edits].cells, retained, kept * sizeof(*refs));
        edits[n_edits++].count = kept;
    }
    if (master != -FDT_ERR_NOTFOUND)
        return -1;
    if (!n_edits)
        return 0;

    void *saved = malloc(dt_bufsize);
    if (!saved)
        return -1;
    memcpy(saved, dt, dt_bufsize);
    int ret = 0;
    for (int i = 0; i < n_edits; i++) {
        master = fdt_path_offset(dt, edits[i].path);
        ret = master < 0 ? master
                         : fdt_setprop(dt, master, "iommus", edits[i].cells,
                                       edits[i].count * sizeof(fdt32_t));
        if (ret)
            break;
    }
    if (ret)
        memcpy(dt, saved, dt_bufsize);
    free(saved);
    return ret;
}

/* Refusal aliases must identify the role established by the public DT graph.
 * Keep offsets only during this read-only preflight; edits reacquire paths. */
struct dt_t8140_refusal_group {
    int controller, piodma;
    int providers[4][64]; /* controller DART, scanout DART, mailbox, power */
    int count[4];
};

static int dt_t8140_ref_nodes(int node, const char *prop, const char *cells_prop, int *nodes)
{
    int length;
    const fdt32_t *refs = fdt_getprop(dt, node, prop, &length);
    if (!refs)
        return length == -FDT_ERR_NOTFOUND ? 0 : -1;
    if (length < 0 || length % sizeof(*refs) || length > 64 * (int)sizeof(*refs))
        return -1;
    int count = 0, cells = length / sizeof(*refs);
    for (int i = 0; i < cells;) {
        int provider = fdt_node_offset_by_phandle(dt, fdt32_to_cpu(refs[i]));
        int size;
        const fdt32_t *args = provider < 0 ? NULL : fdt_getprop(dt, provider, cells_prop, &size);
        if (!args || size != sizeof(*args))
            return -1;
        u32 width = fdt32_to_cpu(*args);
        if (width > 16 || width >= (u32)(cells - i))
            return -1;
        nodes[count++] = provider;
        i += width + 1;
    }
    return count;
}

static bool dt_t8140_group_has(const struct dt_t8140_refusal_group *group, int role, int node)
{
    for (int i = 0; i < group->count[role]; i++)
        if (group->providers[role][i] == node)
            return true;
    return false;
}

/* An alias must not select a different controller with a valid absolute path. */
static int dt_t8140_unique_controller(const char *compatible)
{
    int node = fdt_node_offset_by_compatible(dt, -1, compatible);
    if (node < 0)
        return node;
    if (fdt_node_offset_by_compatible(dt, node, compatible) != -FDT_ERR_NOTFOUND)
        return -FDT_ERR_BADVALUE;
    return node;
}

static int dt_t8140_refusal_group(const char *compatible, struct dt_t8140_refusal_group *group)
{
    memset(group, 0, sizeof(*group));
    group->controller = dt_t8140_unique_controller(compatible);
    group->piodma = -FDT_ERR_NOTFOUND;
    if (group->controller < 0)
        return group->controller == -FDT_ERR_NOTFOUND ? 0 : -1;
    group->piodma = fdt_subnode_offset(dt, group->controller, "piodma");
    if (group->piodma < 0 && group->piodma != -FDT_ERR_NOTFOUND)
        return -1;
    group->count[0] =
        dt_t8140_ref_nodes(group->controller, "iommus", "#iommu-cells", group->providers[0]);
    group->count[1] = group->piodma < 0 ? 0
                                        : dt_t8140_ref_nodes(group->piodma, "iommus",
                                                             "#iommu-cells", group->providers[1]);
    group->count[2] =
        dt_t8140_ref_nodes(group->controller, "mboxes", "#mbox-cells", group->providers[2]);
    char paths[3][128];
    group->count[3] = dt_t8140_power_paths(group->controller, paths, ARRAY_SIZE(paths));
    for (int role = 0; role < 4; role++) {
        if (group->count[role] < 0)
            return -1;
        for (int i = 0; i < group->count[role]; i++) {
            int node = role == 3 ? fdt_path_offset(dt, paths[i]) : group->providers[role][i];
            if (node < 0)
                return -1;
            group->providers[role][i] = node;
            const char *type = role < 2    ? "apple,t8140-dart"
                               : role == 2 ? "apple,t8140-asc-mailbox"
                                           : "apple,t8140-pmgr-pwrstate";
            if (fdt_node_check_compatible(dt, node, type))
                return -1;
        }
    }
    return 0;
}

/* A component master is owned only through its complete scanout tuples. */
static int dt_t8140_refusal_master(int node, const struct dt_t8140_refusal_group *internal,
                                   const struct dt_t8140_refusal_group *external,
                                   bool external_only)
{
    if (fdt_node_check_compatible(dt, node, "apple,display-subsystem"))
        return 0;
    int refs[64];
    int count = dt_t8140_ref_nodes(node, "iommus", "#iommu-cells", refs);
    if (count < 0)
        return -1;
    bool ours = false, other = false;
    for (int i = 0; i < count; i++) {
        bool owned = dt_t8140_group_has(external, 1, refs[i]) ||
                     (!external_only && dt_t8140_group_has(internal, 1, refs[i]));
        ours |= owned;
        other |= !owned;
    }
    return ours && other ? -1 : ours;
}

/* Phandles and saved paths must identify one node before establishing ownership. */
static int dt_t8140_refusal_identity(void)
{
    int node = -1, depth = 0;
    while ((node = fdt_next_node(dt, node, &depth)) >= 0 && depth >= 0) {
        int name_length;
        const char *name = fdt_get_name(dt, node, &name_length);
        if (!name || name_length < 0)
            return -1;
        int parent = fdt_parent_offset(dt, node);
        if (parent >= 0) {
            int sibling = fdt_first_subnode(dt, parent);
            for (; sibling >= 0 && sibling != node; sibling = fdt_next_subnode(dt, sibling)) {
                int length;
                const char *other = fdt_get_name(dt, sibling, &length);
                if (!other || length < 0 || (length == name_length && !memcmp(name, other, length)))
                    return -1;
            }
            if (sibling != node)
                return -1;
        } else if (parent != -FDT_ERR_NOTFOUND || node != 0) {
            return -1;
        }
        u32 phandle = 0;
        static const char *const props[] = {"phandle", "linux,phandle"};
        for (size_t i = 0; i < ARRAY_SIZE(props); i++) {
            int length;
            const fdt32_t *value = fdt_getprop(dt, node, props[i], &length);
            if (!value) {
                if (length != -FDT_ERR_NOTFOUND)
                    return -1;
                continue;
            }
            if (length != sizeof(*value))
                return -1;
            u32 current = fdt32_to_cpu(*value);
            if (!current || current == UINT32_MAX || (phandle && current != phandle))
                return -1;
            phandle = current;
        }
        if (phandle && fdt_node_offset_by_phandle(dt, phandle) != node)
            return -1;
    }
    return node == -FDT_ERR_NOTFOUND || (node >= 0 && depth < 0) ? 0 : -1;
}

static int dt_t8140_refusal_preflight(bool whole, struct dt_t8140_refusal_group *internal,
                                      struct dt_t8140_refusal_group *external)
{
    if (dt_t8140_refusal_identity() || dt_t8140_refusal_group("apple,t8140-dcp", internal) ||
        dt_t8140_refusal_group("apple,t8140-dcpext", external))
        return -1;
    if (internal->controller >= 0 && internal->controller == external->controller)
        return -1;
    static const char *const aliases[] = {
        "dcpext", "dispext0", "dcpext_dart", "dispext0_dart", "dcpext_mbox",
        "dcp",    "disp0",    "dcp_dart",    "disp0_dart",    "dcp_mbox",
    };
    for (size_t i = 0; i < (whole ? ARRAY_SIZE(aliases) : 5); i++) {
        int node = dt_t8140_alias_offset(aliases[i]);
        if (node == -FDT_ERR_NOTFOUND)
            continue;
        if (node < 0)
            return -1;
        const struct dt_t8140_refusal_group *group = i < 5 ? external : internal;
        size_t role = i % 5;
        bool owned = role == 0   ? node == group->controller
                     : role == 1 ? dt_t8140_refusal_master(node, internal, external, i < 5) == 1
                                 : dt_t8140_group_has(group, role - 2, node);
        if (!owned)
            return -1;
    }
    /* Suppliers cannot be withdrawn while another consumer still owns them.
     * Scan complete specifiers so argument cells cannot impersonate phandles. */
    static const char *const props[] = {"iommus", "mboxes", "power-domains"};
    static const char *const widths[] = {"#iommu-cells", "#mbox-cells", "#power-domain-cells"};
    int node = -1, depth = 0;
    while ((node = fdt_next_node(dt, node, &depth)) >= 0 && depth >= 0) {
        for (size_t p = 0; p < ARRAY_SIZE(props); p++) {
            int refs[64];
            int count = dt_t8140_ref_nodes(node, props[p], widths[p], refs);
            if (count < 0)
                return -1;
            for (int i = 0; i < count; i++) {
                bool ext = dt_t8140_group_has(external, p == 0 ? 0 : p + 1, refs[i]) ||
                           (p == 0 && dt_t8140_group_has(external, 1, refs[i]));
                bool in = whole && p < 2 &&
                          (dt_t8140_group_has(internal, p == 0 ? 0 : 2, refs[i]) ||
                           (p == 0 && dt_t8140_group_has(internal, 1, refs[i])));
                if (!ext && !in)
                    continue;
                const struct dt_t8140_refusal_group *group = ext ? external : internal;
                bool owned = node == group->controller || node == group->piodma;
                if (p == 2)
                    owned |=
                        dt_t8140_group_has(group, 2, node) || dt_t8140_group_has(group, 3, node);
                if (p == 0 && dt_t8140_group_has(group, 1, refs[i]))
                    owned |= !fdt_node_check_compatible(dt, node, "apple,display-subsystem");
                if (!owned)
                    return -1;
            }
        }
    }
    return node == -FDT_ERR_NOTFOUND || (node >= 0 && depth < 0) ? 0 : -1;
}

/* Leave the external DCP to its firmware: disable it, its DART, mailbox and routes. */
static int dt_t8140_disable_dcpext(void)
{
    struct dt_t8140_refusal_group internal, external_group;
    if (dt_t8140_refusal_preflight(false, &internal, &external_group))
        return -1;
    int external = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
    if (external < 0 && external != -FDT_ERR_NOTFOUND)
        return -1;
    if (external >= 0 && dt_t8140_withdraw_external_scanout(external))
        return -1;

    static const char *const aliases[] = {
        "dcpext", "dispext0", "dcpext_dart", "dispext0_dart", "dcpext_mbox",
    };
    for (size_t i = 0; i < ARRAY_SIZE(aliases); i++) {
        int node = dt_t8140_alias_offset(aliases[i]);
        if (node < 0 && node != -FDT_ERR_NOTFOUND)
            return -1;
        if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled") < 0)
            return -1;
    }

    int node = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
    if (node < 0)
        return node == -FDT_ERR_NOTFOUND ? 0 : -1;
    char path[128];
    char power_paths[3][128];
    int power_count = dt_t8140_power_paths(node, power_paths, ARRAY_SIZE(power_paths));
    if (fdt_get_path(dt, node, path, sizeof(path)))
        return -1;
    if (power_count < 0)
        return -1;
    for (int i = 0; i < power_count; i++) {
        int power = fdt_path_offset(dt, power_paths[i]);
        if (power < 0 || fdt_setprop_string(dt, power, "status", "disabled"))
            return -1;
    }
    node = fdt_path_offset(dt, path);
    if (node < 0)
        return -1;
    /* PIODMA has its own scanout DART. External resource aliases are optional. */
    int piodma = fdt_subnode_offset(dt, node, "piodma");
    if (piodma >= 0) {
        if (dt_t8140_disable_refs(piodma, "iommus", "#iommu-cells"))
            return -1;
    } else if (piodma != -FDT_ERR_NOTFOUND) {
        return -1;
    }
    node = fdt_path_offset(dt, path);
    if (node < 0 || dt_t8140_disable_refs(node, "iommus", "#iommu-cells"))
        return -1;
    node = fdt_path_offset(dt, path);
    if (node < 0 || dt_t8140_disable_refs(node, "mboxes", "#mbox-cells"))
        return -1;
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_string(dt, node, "status", "disabled") < 0)
        return -1;
    return dt_t8140_disable_typec_routes(path);
}

/* A refused handoff must not let Linux bind an unclaimed live controller. */
static int dt_t8140_refuse_display(void)
{
    struct dt_t8140_refusal_group internal, external;
    if (dt_t8140_refusal_preflight(true, &internal, &external))
        return -1;
    static const char *const compatibles[] = {
        "apple,t8140-dcp",
        "apple,t8140-dcpext",
        "apple,display-subsystem",
    };
    static const char *const consumers[] = {
        "dcp",        "disp0",       "dcpext",        "dispext0", "dcp_dart",
        "disp0_dart", "dcpext_dart", "dispext0_dart", "dcp_mbox", "dcpext_mbox",
    };
    char paths[16][128];
    size_t count = 0;

    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return -1;
    for (size_t i = 0; i < ARRAY_SIZE(compatibles); i++) {
        int node = -1;
        while ((node = fdt_node_offset_by_compatible(dt, node, compatibles[i])) >= 0) {
            if (i == 2) {
                int owned = dt_t8140_refusal_master(node, &internal, &external, false);
                if (owned < 0)
                    return -1;
                if (!owned)
                    continue;
            }
            if (count == ARRAY_SIZE(paths) ||
                fdt_get_path(dt, node, paths[count], sizeof(paths[count])))
                return -1;
            count++;
        }
        if (node != -FDT_ERR_NOTFOUND)
            return -1;
    }
    /* Ownership and the complete owned path set were checked before edits. */
    int ret = fdt_delprop(dt, chosen, "apple,firmware-root-handoff");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return -1;
    ret = fdt_delprop(dt, chosen, "apple,dcp-rtkit-live");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return -1;
    ret = fdt_delprop(dt, chosen, "apple,dcp-rtkit-quiesced");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        return -1;
    chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || fdt_setprop_string(dt, chosen, "apple,dcp-handoff-reason", "withdrawn"))
        return -1;
    if (dt_t8140_disable_dcpext())
        return -1;
    for (size_t i = 0; i < count; i++) {
        int node = fdt_path_offset(dt, paths[i]);
        if (node < 0)
            return -1;
        /* A master is selected only through direct scanout references. Its
         * other references do not establish ownership of those suppliers. */
        bool controller = !fdt_node_check_compatible(dt, node, "apple,t8140-dcp") ||
                          !fdt_node_check_compatible(dt, node, "apple,t8140-dcpext");
        int piodma = controller ? fdt_subnode_offset(dt, node, "piodma") : -FDT_ERR_NOTFOUND;
        if (piodma >= 0) {
            if (dt_t8140_disable_refs(piodma, "iommus", "#iommu-cells"))
                return -1;
        } else if (piodma != -FDT_ERR_NOTFOUND) {
            return -1;
        }
        node = fdt_path_offset(dt, paths[i]);
        if (node < 0 || dt_t8140_disable_refs(node, "iommus", "#iommu-cells"))
            return -1;
        node = fdt_path_offset(dt, paths[i]);
        if (node < 0 || (controller && dt_t8140_disable_refs(node, "mboxes", "#mbox-cells")))
            return -1;
        node = fdt_path_offset(dt, paths[i]);
        if (node < 0)
            return -1;
        if (fdt_setprop_string(dt, node, "status", "disabled"))
            return -1;
        if (dt_t8140_disable_typec_routes(paths[i]))
            return -1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(consumers); i++) {
        int node = dt_t8140_alias_offset(consumers[i]);
        if (node < 0 && node != -FDT_ERR_NOTFOUND)
            return -1;
        if (node >= 0 && fdt_setprop_string(dt, node, "status", "disabled"))
            return -1;
    }
    return 0;
}

/* Validate roles before metadata export, reservation or DART access. A master
 * belongs to the internal display through its scanout IOMMU references, not
 * through its name. A separate external-only master remains valid.
 *
 * Each alias the handoff follows must name one absolute path: libfdt follows
 * a relative alias recursively. Only the external controller's alias is
 * optional.
 *
 * Returns 1, and sets *absent, when the tree does not describe the internal
 * display: no internal controller, or no dcp or disp0 alias. The caller keeps
 * simplefb then. Returns -1 when a description is present but inconsistent. */
static int dt_t8140_display_alias_roles(const char **absent)
{
    int internal = dt_t8140_unique_controller("apple,t8140-dcp");
    int external = dt_t8140_unique_controller("apple,t8140-dcpext");
    int dcp = dt_t8140_alias_offset("dcp");
    int disp0 = dt_t8140_alias_offset("disp0");
    int alias = dt_t8140_alias_offset("dcpext");
    /* Whatever is present must be consistent, absent parts or not. */
    if ((internal < 0 && internal != -FDT_ERR_NOTFOUND) ||
        (external < 0 && external != -FDT_ERR_NOTFOUND) || (dcp < 0 && dcp != -FDT_ERR_NOTFOUND) ||
        (disp0 < 0 && disp0 != -FDT_ERR_NOTFOUND) || (alias < 0 && alias != -FDT_ERR_NOTFOUND) ||
        (internal >= 0 && internal == external) || (dcp >= 0 && dcp != internal) ||
        (alias >= 0 && alias != external) ||
        (disp0 >= 0 && fdt_node_check_compatible(dt, disp0, "apple,display-subsystem")))
        return -1;
    if (internal == -FDT_ERR_NOTFOUND) {
        *absent = "FDT T8140 DCP absent";
        return 1;
    }
    if (dcp == -FDT_ERR_NOTFOUND) {
        *absent = "FDT dcp alias absent";
        return 1;
    }

    int piodma = fdt_subnode_offset(dt, internal, "piodma");
    if (piodma < 0 && piodma != -FDT_ERR_NOTFOUND)
        return -1;
    int scanout[64];
    int count = piodma < 0 ? 0 : dt_t8140_ref_nodes(piodma, "iommus", "#iommu-cells", scanout);
    if (count < 0)
        return -1;
    /* Without a scanout reference or a disp0 alias, the tree describes no display pipeline. */
    if (!count) {
        if (disp0 == -FDT_ERR_NOTFOUND) {
            *absent = "FDT disp0 alias absent";
            return 1;
        }
        return -1;
    }

    int master = -FDT_ERR_NOTFOUND;
    int node = -1;
    while ((node = fdt_node_offset_by_compatible(dt, node, "apple,display-subsystem")) >= 0) {
        int refs[64];
        int nrefs = dt_t8140_ref_nodes(node, "iommus", "#iommu-cells", refs);
        if (nrefs < 0)
            return -1;
        bool owned = false;
        for (int i = 0; i < nrefs; i++)
            for (int j = 0; j < count; j++)
                owned |= refs[i] == scanout[j];
        if (owned) {
            if (master >= 0)
                return -1;
            master = node;
        }
    }
    if (node != -FDT_ERR_NOTFOUND)
        return -1;
    if (disp0 == -FDT_ERR_NOTFOUND) {
        *absent = "FDT disp0 alias absent";
        return 1;
    }
    if (master < 0 || disp0 != master)
        return -1;
    return 0;
}

static int dt_set_display_t8140(void)
{
    int ret = -1;
    const char *reason = "validation failed";
    const char *step = "preflight";
    int roles = dt_t8140_display_alias_roles(&reason);
    if (roles < 0) {
        printf("FDT: T8140 display aliases do not identify unique display roles\n");
        return -1;
    }
    /* The observation also diagnoses a tree that describes no display. */
    if (roles)
        goto refuse;
    u64 base, size, end, fb_size;
    struct dt_t8140_dram dram = {0};
    int adt_path[4];
    int node = adt_path_offset_trace(adt, "/vram", adt_path);
    if (node < 0) {
        reason = "ADT /vram absent";
        goto refuse;
    }

    int depth = 0;
    while (depth < 3 && adt_path[depth])
        depth++;
    if (depth >= 3) {
        reason = "ADT /vram trace too deep";
        goto refuse;
    }
    adt_path[depth + 1] = 0;
    base = size = 0;
    int reg_ret = adt_get_reg(adt, adt_path, "reg", 0, &base, &size);
    if (reg_ret < 0 || !size || (base & (SZ_16K - 1)) || (size & (SZ_16K - 1)) ||
        base > UINT64_MAX - size) {
        reason = "invalid ADT /vram reg or alignment";
        goto refuse;
    }
    if (!cur_boot_args.video.base || !cur_boot_args.video.height || !cur_boot_args.video.stride ||
        cur_boot_args.video.stride > UINT64_MAX / cur_boot_args.video.height) {
        printf("FDT: T8140 framebuffer geometry: PA 0x%lx stride 0x%lx height 0x%lx\n",
               cur_boot_args.video.base, cur_boot_args.video.stride, cur_boot_args.video.height);
        reason = "invalid boot framebuffer geometry";
        goto refuse;
    }

    end = base + size;
    fb_size = cur_boot_args.video.stride * cur_boot_args.video.height;
    if (cur_boot_args.video.base < base || cur_boot_args.video.base > end ||
        fb_size > end - cur_boot_args.video.base) {
        reason = "boot framebuffer lies outside /vram";
        goto refuse;
    }
    /* Match dt_set_memory's physical DRAM source, not the boot-args usable
     * window. iBoot keeps the display carveouts above that window. */
    node = adt_path_offset(adt, "/chosen");
    if (node < 0) {
        reason = "ADT /chosen absent";
        goto refuse;
    }
    if (ADT_GETPROP(adt, node, "dram-base", &dram.base) < 0)
        dram.base = 0x800000000;
    if (ADT_GETPROP(adt, node, "dram-size", &dram.size) < 0)
        dram.size = mem_size_actual;
    if (!dram.size || dram.base > UINT64_MAX - dram.size) {
        reason = "invalid physical DRAM extent";
        goto refuse;
    }
    if (!dt_t8140_in_physical_dram(&dram, base, size)) {
        reason = "/vram lies outside physical DRAM";
        goto refuse;
    }

    /* On the measured J700 ADT, /vram exactly matches carveout region 14
     * and an opaque DCP nub segment. Check both independent ADT descriptions
     * before publishing it; neither establishes an IOVA on its own. */
    node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u64 carveout[2] = {0};
    int carveout_len = node < 0 ? -1 : ADT_GETPROP_ARRAY(adt, node, "region-id-14", carveout);
    if (carveout_len != sizeof(carveout) || carveout[0] != base || carveout[1] != size) {
        reason = "ADT region-id-14 differs from /vram";
        goto refuse;
    }
    node = adt_path_offset(adt, "/arm-io/dcp/iop-dcp-nub");
    u32 seg_len = 0;
    const struct adt_segment_ranges *seg =
        node < 0 ? NULL : adt_getprop(adt, node, "segment-ranges", &seg_len);
    if (!seg || !seg_len || seg_len % sizeof(*seg) || seg_len > 32 * sizeof(*seg)) {
        printf("FDT: T8140 DCP nub: invalid segment-ranges len %u (node %d)\n", seg_len, node);
        reason = "invalid DCP nub segment-ranges";
        goto refuse;
    }
    bool segment_found = false;
    for (u32 i = 0; i < seg_len / sizeof(*seg); i++) {
        if (seg[i].phys == base && seg[i].size == size)
            segment_found = true;
    }
    if (!segment_found) {
        reason = "DCP nub has no segment matching /vram";
        goto refuse;
    }

    /* The upstream helpers mutate the FDT as they go. Restore the complete
     * tree if a DART span or any property write fails. */
    void *saved = malloc(dt_bufsize);
    if (!saved) {
        reason = "FDT snapshot allocation failed";
        goto refuse;
    }
    memcpy(saved, dt, dt_bufsize);
    step = "/vram DCP/disp0 DART reservation";
    ret = dt_vram_reserved_region("dcp", "disp0");
    if (!ret) {
        step = "DCP firmware properties";
        ret = dt_set_dcp_firmware("dcp");
    }
    if (!ret) {
        step = "DCP region-id-49 reservation";
        ret = dt_t8140_reserve_carveout("dcp", NULL, "region-id-49", "asc-firmware", false, &dram);
    }
    if (!ret) {
        step = "DCP region-id-50 reservation";
        ret = dt_t8140_reserve_data("dcp", "/arm-io/dcp/iop-dcp-nub", "region-id-50", "dcp_data",
                                    &dram);
    }
    const struct {
        const char *region, *name;
        bool disp;
    } internal[] = {
        {"region-id-57", "region57", false},
        {"region-id-94", "region94", true},
        {"region-id-95", "region95", false},
        {"region-id-233", "region233", false},
    };
    for (size_t i = 0; !ret && i < ARRAY_SIZE(internal); i++) {
        step = internal[i].region;
        ret =
            dt_t8140_reserve_carveout("dcp", internal[i].disp ? "disp0" : NULL, internal[i].region,
                                      internal[i].name, internal[i].disp, &dram);
    }
    if (ret) {
        memcpy(dt, saved, dt_bufsize);
        free(saved);
        reason = "reservation or DART validation failed";
        goto refuse;
    }

    step = "internal DCP provider properties";
    if (dt_t8140_copy_provider_props("dcp"))
        goto rollback;

    /* H17P keeps its locked translations and live RTKit session. Linux
     * performs INIT/HELLO; stage 2 must not change AP/IOP power first. */
    step = "internal DCP ASC and mailboxes";
    if (!dt_t8140_validate_dcp_asc("/arm-io/dcp", "dcp", ASC_INHERITED_RUNNING)) {
        reason = "inherited DCP ASC is not live with healthy empty mailboxes";
        goto rollback;
    }

    step = "internal firmware DART table reservation";
    struct dt_t8140_external_darts ext_darts;
    if (dt_t8140_collect_external_darts(&ext_darts) ||
        dt_t8140_reserve_tables("dcp", &dram, &ext_darts, false) ||
        dt_t8140_reserve_tables("disp0", &dram, &ext_darts, false) ||
        dt_t8140_reserve_piodma_tables("dcp", true, &dram, &ext_darts, false))
        goto rollback;

    /* External firmware has its own DART. It is optional for the internal
     * panel, so a failed external validation only rolls back that controller. */
    void *ext_saved = malloc(dt_bufsize);
    if (!ext_saved) {
        step = "external FDT snapshot";
        goto rollback;
    }
    memcpy(ext_saved, dt, dt_bufsize);
    char ext_path[128];
    const char *ext = NULL;
    node = dt_t8140_alias_offset("dcpext");
    if (node == -FDT_ERR_NOTFOUND)
        node = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
    if (node >= 0 && fdt_get_path(dt, node, ext_path, sizeof(ext_path)) == 0)
        ext = ext_path;
    bool ext_ready = false;
    u32 ext_step = ext ? 1 : 0;
    if (ext && pmgr_power_is_on(0, "DISPEXT0_CPU") == 1) {
        step = "DCPEXT firmware properties";
        ext_step = 2;
        ret = dt_set_dcp_firmware(ext);
        if (!ret) {
            step = "DCPEXT region-id-49 reservation";
            ext_step = 3;
            ret =
                dt_t8140_reserve_carveout(ext, NULL, "region-id-49", "asc-firmware", false, &dram);
        }
        if (!ret) {
            step = "DCPEXT region-id-73 reservation";
            ext_step = 4;
            ret = dt_t8140_reserve_data(ext, "/arm-io/dcpext/iop-dcpext-nub", "region-id-73",
                                        "dcpext0_data", &dram);
        }
        if (!ret) {
            step = "DCPEXT region-id-74 reservation";
            ext_step = 5;
            ret = dt_t8140_reserve_carveout(ext, NULL, "region-id-74", "region74", false, &dram);
        }
        if (!ret) {
            step = "DCPEXT region-id-234 reservation";
            ext_step = 6;
            ret = dt_t8140_reserve_carveout(ext, NULL, "region-id-234", "region234", false, &dram);
        }
        if (!ret) {
            step = "DCPEXT provider properties";
            ret = dt_t8140_copy_provider_props(ext);
        }
        if (!ret) {
            step = "DCPEXT ASC and mailboxes";
            ext_step = 7;
            bool idle = dt_t8140_validate_dcp_asc("/arm-io/dcpext", "dcpext", ASC_INHERITED_IDLE);
            ret = idle ? 0 : -1;
        }
        if (!ret) {
            step = "external firmware DART table reservation";
            ext_step = 8;
            ret = dt_t8140_reserve_tables(ext, &dram, &ext_darts, true);
            if (!ret)
                ret = dt_t8140_reserve_piodma_tables(ext, true, &dram, &ext_darts, true);
            if (!ret)
                ret = dt_t8140_reserve_master_tables(&dram, &ext_darts);
        }
        ext_ready = !ret;
        if (ret) {
            memcpy(dt, ext_saved, dt_bufsize);
            printf("FDT: T8140 %s failed (%d); leaving dcpext disabled\n", step, ret);
        }
    } else {
        printf("FDT: T8140 DCPEXT absent or powered off; internal panel continues\n");
    }
    free(ext_saved);

    step = "enable DCP and disp0 consumers/IOMMUs";
    if (dt_t8140_enable_consumer("dcp") || dt_t8140_enable_consumer("disp0"))
        goto rollback;
    if (ext_ready) {
        step = "enable DCPEXT dependency group";
        ext_step = 9;
        if (dt_t8140_enable_external(ext)) {
            printf("FDT: T8140 external dependencies unavailable; internal panel continues\n");
            ext_ready = false;
            if (dt_t8140_disable_dcpext())
                goto rollback;
        }
    } else {
        step = "disable unvalidated DCPEXT";
        if (dt_t8140_disable_dcpext())
            goto rollback;
    }
    step = "disable unvalidated external display consumer";
    node = -1;
    while ((node = fdt_node_offset_by_compatible(dt, node, "apple,display-subsystem")) >= 0) {
        int name_len;
        const char *name = fdt_get_name(dt, node, &name_len);
        if (!name || name_len < 0)
            goto rollback;
        if (strstr(name, "dispext")) {
            char path[128];
            if (fdt_get_path(dt, node, path, sizeof(path)))
                goto rollback;
            if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
                goto rollback;
            node = fdt_path_offset(dt, path);
            if (node < 0)
                goto rollback;
        }
    }
    step = "audit published display IOMMUs";
    if (dt_t8140_audit_display_iommus())
        goto rollback;
    step = "publish live RTKit and firmware-root handoff";
    node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        goto rollback;
    ret = fdt_delprop(dt, node, "apple,dcp-rtkit-quiesced");
    if (ret && ret != -FDT_ERR_NOTFOUND)
        goto rollback;
    node = fdt_path_offset(dt, "/chosen");
    if (fdt_setprop_string(dt, node, "apple,dcp-handoff-reason", "live-session") < 0)
        goto rollback;
    node = fdt_path_offset(dt, "/chosen");
    if (fdt_setprop_u32(dt, node, "apple,dcp-rtkit-live", 1) < 0)
        goto rollback;
    /* A diagnostic result for this preflight, not a hardware-ready claim.
     * 0: absent, 1: power off, 2..7: validation stage, 8: firmware DART
     * table reservation, 9: dependencies. */
    if (fdt_setprop_u32(dt, node, "asahi,external-display-preflight", ext_ready ? 0x100 : ext_step))
        printf("FDT: external preflight observation unavailable\n");
    free(saved);
    printf("FDT: T8140 DCP enabled with validated framebuffer, firmware and live inherited RTKit "
           "session%s;"
           " /chosen/apple,dcp-rtkit-live=1\n",
           ext_ready ? " (dcpext enabled)" : "");
    return 0;

rollback:
    memcpy(dt, saved, dt_bufsize);
    free(saved);
    if (!strcmp(reason, "validation failed"))
        reason = "FDT consumer or property update failed";

refuse:
    printf("FDT: T8140 display refused at %s: %s; retaining simplefb\n", step, reason);
    if (dt_t8140_refuse_display())
        return -1;
    node = fdt_path_offset(dt, "/chosen");
    if (node < 0 || fdt_setprop_string(dt, node, "apple,dcp-handoff-reason", reason))
        return -1;
    return 0;
}

static int dt_t8140_recheck_display(void)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return -1;
    if (!fdt_getprop(dt, chosen, "apple,dcp-rtkit-live", NULL))
        return 0;

    bool healthy = dt_t8140_validate_dcp_asc("/arm-io/dcp", "dcp", ASC_INHERITED_RUNNING);
    int ext = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
    const char *status = ext < 0 ? NULL : fdt_getprop(dt, ext, "status", NULL);
    if (!healthy) {
        printf("kboot: DCP changed state before handoff; disabling display\n");
        if (dt_t8140_refuse_display() || fdt_pack(dt))
            return -1;
    } else if (status && !strcmp(status, "okay") &&
               (pmgr_power_is_on(0, "DISPEXT0_CPU") != 1 ||
                !dt_t8140_validate_dcp_asc("/arm-io/dcpext", "dcpext", ASC_INHERITED_IDLE))) {
        /* The internal panel does not depend on the external DCP. */
        printf("kboot: DCPEXT changed state before handoff; disabling it\n");
        if (dt_t8140_disable_dcpext())
            return -1;
        if (dt_t8140_audit_display_iommus()) {
            printf(
                "kboot: display IOMMUs inconsistent after DCPEXT withdrawal; disabling display\n");
            if (dt_t8140_refuse_display())
                return -1;
        }
        if (fdt_pack(dt))
            return -1;
    }
    return 0;
}

/* T8142 display carveouts: DCP uses SID 23, the display pipe SID 0 and
 * PIODMA SID 16. IOVAs are resolved from the inherited DART tables, since the
 * live OS-log alias can differ from its static segment descriptor.
 */
static struct disp_mapping disp_reserved_regions_t8142[] = {
    {"region-id-49", "asc-firmware", true, false, false},
    {"region-id-50", "dcp_data", true, false, false},
    {"region-id-57", "region57", true, false, false},
    {"region-id-94", "region94", true, true, false},
    {"region-id-95", "region95", true, false, true},
};

static bool dt_display_phys_reserved(u64 phys, u64 size)
{
    int parent = fdt_path_offset(dt, "/reserved-memory");
    int node;

    fdt_for_each_subnode(node, dt, parent)
    {
        int len;
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len != 16 || !fdt_getprop(dt, node, "no-map", NULL))
            continue;
        u64 base = fdt64_ld(&reg[0]), count = fdt64_ld(&reg[1]);
        if (phys >= base && size <= count && phys - base <= count - size)
            return true;
    }
    return false;
}

/* The T8142 DCP data carveout is fragmented in DVA space, and __SHARED
 * extends outside it. Preserve every live segment, including aliases whose ADT
 * physical field differs from the active DART translation (__OS_LOG).
 * IOVA-only reservations avoid overlapping physical reserved-memory nodes
 * when a segment is already contained in a carveout or the framebuffer.
 */
static int dt_reserve_m5_display_segments(void)
{
    const char *alias = "dcp";
    int owner = fdt_path_offset(dt, alias);
    int nub = adt_path_offset(adt, "/arm-io/dcp/iop-dcp-nub");
    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram, dram_size;
    u32 len = 0;
    int ret = 0;
    const struct adt_segment_ranges *segments;

    if (owner < 0)
        return 0;
    if (nub < 0 || chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram) != sizeof(dram) ||
        ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size))
        bail("ADT: missing M5 display firmware memory bounds\n");
    segments = adt_getprop(adt, nub, "segment-ranges", &len);
    if (!segments || !len || len % sizeof(*segments) || len / sizeof(*segments) > 64)
        bail("ADT: invalid M5 display segment inventory\n");
    u32 owner_phandle = fdt_get_phandle(dt, owner);
    dart_dev_t *dart = dt_init_dart_by_node(owner, 0);
    if (!dart)
        bail("FDT: cannot inspect M5 display firmware translations\n");

    u64 total = 0;
    for (u32 i = 0; i < len / sizeof(*segments); i++) {
        u64 start = segments[i].remap;
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);
        total += size;
        if (!size || (start & (SZ_16K - 1)) || start < (1ULL << 40) ||
            start - (1ULL << 40) >= (1ULL << 36) || size > (1ULL << 36) - (start - (1ULL << 40)) ||
            total > 128 * SZ_1M)
            bail_cleanup("ADT: unsupported M5 display firmware segment\n");
        for (u64 offset = 0; offset < size;) {
            u64 iova = start + offset;
            u64 phys = (u64)dart_translate(dart, iova);
            if (phys < dram || phys - dram > dram_size - SZ_16K || (phys & (SZ_16K - 1)))
                bail_cleanup("DART: invalid M5 display firmware page\n");
            bool reserved = dt_display_phys_reserved(phys, SZ_16K);
            u64 count = SZ_16K;
            while (offset + count < size) {
                u64 next = (u64)dart_translate(dart, iova + count);
                if (next != phys + count || next < dram || next - dram > dram_size - SZ_16K ||
                    dt_display_phys_reserved(next, SZ_16K) != reserved)
                    break;
                count += SZ_16K;
            }
            char name[64];
            int node;
            if (!reserved) {
                snprintf(name, sizeof(name), "dcp-firmware@%lx", phys);
                node = dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, phys, count);
            } else {
                snprintf(name, sizeof(name), "dcp-iova-%lx", iova);
                int parent = fdt_path_offset(dt, "/reserved-memory");
                node = fdt_add_subnode(dt, parent, name);
                if (node < 0)
                    bail_cleanup("FDT: cannot add M5 display IOVA reservation\n");
                u32 phandle;
                if (fdt_generate_phandle(dt, &phandle) ||
                    fdt_setprop_u32(dt, node, "phandle", phandle) ||
                    fdt_setprop_string(dt, node, "compatible", "apple,asc-mem"))
                    bail_cleanup("FDT: cannot describe M5 display IOVA reservation\n");
            }
            if (node < 0)
                bail_cleanup("FDT: cannot reserve M5 display firmware\n");
            u32 phandle = fdt_get_phandle(dt, node);
            if (dt_device_set_reserved_mem(node, name, owner_phandle, iova, count) ||
                dt_device_add_mem_region(alias, phandle, NULL))
                bail_cleanup("FDT: cannot attach M5 display firmware reservation\n");
            offset += count;
        }
    }
    printf("FDT: reserved live M5 display firmware segments (%lu bytes)\n", total);
err:
    dart_shutdown(dart);
    return ret;
}

/*
 * J514S (T6030) internal display.
 *
 * iBoot leaves the panel's display coprocessor (DCP) running, its firmware
 * DART and the display frontend DART locked and translating. The OS adopts
 * that session as it is, so this only describes it: the firmware segments are
 * reserved at the DART addresses the firmware uses, and the frontend's
 * inherited scanout (SID0) and PIODMA (SID4) mappings are published once every
 * page of them matched a display segment of the firmware. Nothing is reset or
 * enabled in hardware; a refused handoff keeps the boot framebuffer.
 */

/* DCP firmware builds whose handoff the J514S display driver implements. */
static const char *const t6030_dcp_uuids[] = {
    "DDF38191-93B3-324A-BC8F-643006F5AC82",
};

/* Why the frontend handoff stopped, published in /chosen for the OS. */
static char t6030_display_handoff[160] = "not attempted";

static int t6030_display_handoff_fail(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(t6030_display_handoff, sizeof(t6030_display_handoff), fmt, ap);
    va_end(ap);
    printf("FDT: J514S display handoff: %s\n", t6030_display_handoff);
    return -1;
}

struct t6030_display_maps {
    const struct adt_segment_ranges *segments;
    u32 count;
    u64 bytes[32];
    u64 rejected_iova, rejected_paddr;
    bool rejected;
};

/* Account an inherited page to the display segment it belongs to. */
static int t6030_display_page(u64 iova, u64 paddr, void *opaque)
{
    struct t6030_display_maps *maps = opaque;

    for (u32 i = 0; i < maps->count; i++) {
        const struct adt_segment_ranges *seg = &maps->segments[i];
        u64 start = seg->remap & (BIT(36) - 1);
        u64 size = ALIGN_UP((u64)seg->size, SZ_16K);

        if (seg->unk != 2 || iova < start || iova - start >= size)
            continue;
        if ((seg->remap >> 36) != 0x10 || paddr != seg->phys + iova - start)
            break;
        maps->bytes[i] += SZ_16K;
        return 0;
    }

    /* Never drop an inherited mapping that cannot be described. */
    maps->rejected = true;
    maps->rejected_iova = iova;
    maps->rejected_paddr = paddr;
    return -1;
}

static int dt_t6030_enable_iommu(int node)
{
    int dart = dt_get_iommu_node(node, 0);

    if (dart < 0)
        return -1;
    return fdt_setprop_string(dt, dart, "status", "okay") ? -1 : 0;
}

static int dt_set_t6030_display_maps(const struct adt_segment_ranges *segments, u32 count,
                                     u64 dram_base, u64 dram_size)
{
    static const char *const aliases[] = {"disp0", "disp0_piodma"};
    static const u8 streams[] = {0, 4};
    static const u32 expected[] = {3, 1};
    struct t6030_display_maps maps[2] = {
        {.segments = segments, .count = count},
        {.segments = segments, .count = count},
    };
    u32 phandles[2];
    int adt_path[8];
    u64 adt_base, adt_size;
    int len;

    /* The frontend DART's registers, as described by both trees. */
    int display = fdt_path_offset(dt, "disp0");
    int dart = display < 0 ? display : dt_get_iommu_node(display, 0);
    if (dart < 0)
        return t6030_display_handoff_fail("disp0 has no DART node (%d)", dart);
    const fdt32_t *reg = fdt_getprop(dt, dart, "reg", &len);
    if (!reg || len < 16)
        return t6030_display_handoff_fail("disp0 DART has no reg");
    u64 dart_base = ((u64)fdt32_ld(&reg[0]) << 32) | fdt32_ld(&reg[1]);
    u64 dart_size = ((u64)fdt32_ld(&reg[2]) << 32) | fdt32_ld(&reg[3]);
    if (adt_path_offset_trace(adt, "/arm-io/dart-disp0", adt_path) < 0 ||
        adt_get_reg(adt, adt_path, "reg", 0, &adt_base, &adt_size) < 0)
        return t6030_display_handoff_fail("ADT has no /arm-io/dart-disp0 reg");
    if (adt_size < 0x1440 || dart_size < 0x1440 || dart_base > UINT64_MAX - 0x1440)
        return t6030_display_handoff_fail("disp0 DART register window is too small");
    if (adt_base != dart_base)
        return t6030_display_handoff_fail("DART base: FDT 0x%lx, ADT 0x%lx", dart_base, adt_base);

    for (u32 n = 0; n < 2; n++) {
        if (dart_walk_locked_t8110(dart_base, min(adt_size, dart_size), streams[n], dram_base,
                                   dram_size, t6030_display_page, &maps[n])) {
            if (maps[n].rejected)
                return t6030_display_handoff_fail("SID%u: no segment for iova 0x%lx -> 0x%lx",
                                                  streams[n], maps[n].rejected_iova,
                                                  maps[n].rejected_paddr);
            /* PROTECT, TCR and TTBR, as the walk has just read them. */
            return t6030_display_handoff_fail(
                "SID%u: walk refused, protect 0x%x tcr 0x%x ttbr 0x%x", streams[n],
                read32(dart_base + 0x200), read32(dart_base + 0x1000 + 4 * streams[n]),
                read32(dart_base + 0x1400 + 4 * streams[n]));
        }

        u32 regions = 0;
        for (u32 i = 0; i < count; i++) {
            u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);

            if (!maps[n].bytes[i])
                continue;
            if (maps[n].bytes[i] != size)
                return t6030_display_handoff_fail("SID%u: segment %u mapped 0x%lx of 0x%lx bytes",
                                                  streams[n], i, maps[n].bytes[i], size);
            regions++;
        }
        if (regions != expected[n])
            return t6030_display_handoff_fail("SID%u: %u segments mapped, expected %u", streams[n],
                                              regions, expected[n]);
    }

    /* Every inherited page is accounted for; publish them. */
    for (u32 n = 0; n < 2; n++) {
        int node = fdt_path_offset(dt, aliases[n]);
        if (node < 0)
            return t6030_display_handoff_fail("could not publish the mappings");
        phandles[n] = fdt_get_phandle(dt, node);
        if (!phandles[n] && (fdt_generate_phandle(dt, &phandles[n]) ||
                             fdt_setprop_u32(dt, node, "phandle", phandles[n])))
            return t6030_display_handoff_fail("could not publish the mappings");
    }
    for (u32 i = 0; i < count; i++) {
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);
        char name[64];

        if (!maps[0].bytes[i] && !maps[1].bytes[i])
            continue;
        snprintf(name, sizeof(name), "asc-firmware@%lx", segments[i].phys);
        int node = dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, segments[i].phys, size);
        if (node < 0)
            return t6030_display_handoff_fail("could not publish the mappings");
        u32 phandle = fdt_get_phandle(dt, node);
        for (u32 n = 0; n < 2; n++)
            if (maps[n].bytes[i] &&
                dt_device_set_reserved_mem(node, name, phandles[n], segments[i].remap, size))
                return t6030_display_handoff_fail("could not publish the mappings");
        for (u32 n = 0; n < 2; n++)
            if (maps[n].bytes[i] && dt_device_add_mem_region(aliases[n], phandle, NULL))
                return t6030_display_handoff_fail("could not publish the mappings");
    }

    int piodma = fdt_path_offset(dt, "disp0_piodma");
    if (piodma < 0 || dt_t6030_enable_iommu(piodma))
        return t6030_display_handoff_fail("could not publish the mappings");
    piodma = fdt_path_offset(dt, "disp0_piodma");
    if (fdt_setprop_empty(dt, piodma, "apple,j514s-inherited-mappings") ||
        fdt_setprop_string(dt, piodma, "status", "okay"))
        return t6030_display_handoff_fail("could not publish the mappings");
    printf("FDT: J514S DISP0 SID0/SID4 mappings preserved; PIODMA enabled\n");
    snprintf(t6030_display_handoff, sizeof(t6030_display_handoff), "PIODMA enabled");

    int dcp = fdt_path_offset(dt, "dcp");
    if (dcp >= 0 && fdt_getprop(dt, dcp, "apple,j514s-native-scanout", NULL)) {
        display = fdt_path_offset(dt, "disp0");
        if (display < 0 ||
            fdt_node_check_compatible(dt, display, "apple,t6030-display-diagnostics") ||
            fdt_setprop_empty(dt, display, "apple,j514s-inherited-mappings"))
            return t6030_display_handoff_fail("could not publish the mappings");
        display = fdt_path_offset(dt, "disp0");
        if (fdt_setprop_string(dt, display, "status", "okay"))
            return t6030_display_handoff_fail("could not publish the mappings");
        printf("FDT: J514S scanout enabled with the inherited SID0 mappings\n");
    }

    return 0;
}

/*
 * Reserve the running panel DCP's firmware and publish its identity. The DCP
 * firmware DART is enabled only once its reservations are published.
 */
static int dt_set_t6030_dcp_handoff(void)
{
    int dcp = fdt_path_offset(dt, "dcp");
    if (dcp < 0)
        return -1; /* no display coprocessor described: boot framebuffer only */
    if (chip_id != T6030 || fdt_node_check_compatible(dt, 0, "apple,j514s") ||
        fdt_node_check_compatible(dt, dcp, "apple,t6030-dcp"))
        bail("FDT: unsupported T6030 DCP handoff target\n");

    int nub = adt_path_offset(adt, "/arm-io/dcp/iop-dcp-nub");
    if (nub < 0)
        bail("ADT: missing J514S DCP firmware metadata\n");

    u32 len;
    const struct adt_segment_ranges *segments = adt_getprop(adt, nub, "segment-ranges", &len);
    if (!segments || !len || len % sizeof(*segments) || len / sizeof(*segments) > 32)
        bail("ADT: invalid J514S DCP firmware segment list\n");
    u32 count = len / sizeof(*segments);

    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram_base, dram_size;
    if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base) ||
        ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size))
        bail("ADT: missing J514S DRAM bounds\n");
    u64 dram_end = dram_base + dram_size;
    if (!dram_size || dram_end <= dram_base)
        bail("ADT: invalid J514S DRAM bounds\n");

    /* Validate the whole list before changing the FDT. */
    for (u32 i = 0; i < count; i++) {
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);

        if (!size || (segments[i].phys | segments[i].remap) & (SZ_16K - 1) ||
            segments[i].phys < dram_base || segments[i].phys + size < segments[i].phys ||
            segments[i].phys + size > dram_end || segments[i].remap + size < segments[i].remap)
            bail("ADT: invalid J514S DCP firmware segment %u\n", i);
    }

    const char *uuid = adt_getprop(adt, nub, "uuid", &len);
    if (!uuid || len != 37 || uuid[len - 1])
        bail("ADT: invalid J514S DCP firmware UUID\n");
    bool qualified = false;
    for (size_t i = 0; i < ARRAY_SIZE(t6030_dcp_uuids); i++)
        if (!strcmp(uuid, t6030_dcp_uuids[i]))
            qualified = true;
    if (!qualified) {
        printf("FDT: J514S DCP firmware %s not supported; keeping the boot framebuffer\n", uuid);
        return -1;
    }
    if (fdt_setprop_string(dt, dcp, "apple,firmware-uuid", uuid))
        bail("FDT: cannot publish the J514S DCP firmware UUID\n");

    if (dt_set_dcp_firmware("dcp"))
        return -1;
    if (dt_reserve_asc_firmware("/arm-io/dcp/iop-dcp-nub", NULL, "dcp", true, 0))
        return -1;

    dcp = fdt_path_offset(dt, "dcp");
    if (dcp < 0 || dt_t6030_enable_iommu(dcp))
        bail("FDT: cannot enable the J514S DCP firmware DART\n");

    dcp = fdt_path_offset(dt, "dcp");
    if (fdt_getprop(dt, dcp, "apple,j514s-native-piodma", NULL)) {
        if (dt_set_t6030_display_maps(segments, count, dram_base, dram_size))
            return -1; /* The transaction discards partial consumer enablement. */
        chosen = fdt_path_offset(dt, "/chosen");
        if (chosen < 0 ||
            fdt_setprop_string(dt, chosen, "aurora,j514s-display-handoff", t6030_display_handoff))
            printf("FDT: cannot record the J514S display handoff result\n");
    }

    printf("FDT: J514S DCP firmware %s handed over\n", uuid);
    return 0;
}

/*
 * J514S external display controllers (DCPEXT0/1). iBoot loads the panel's
 * firmware image for them as well and leaves their segments and DART tables in
 * memory; it starts a controller only for a display attached at power-on. The
 * OS may run a controller once its segments are reserved before it allocates
 * memory (the OS log segment lies in usable RAM) at the DART addresses the
 * firmware uses, and the controller is published with its firmware identity
 * and its Type-C display routes. Only the ADT and the FDT are read here: the
 * controllers may be unpowered. A controller that does not validate stays
 * disabled.
 */
#define T6030_DCPEXT_SEGMENTS 4

/* ADT segment flags and runtime DART addresses of TEXT, DATA, OS log and scratch. */
static const u32 t6030_dcpext_seg_flags[T6030_DCPEXT_SEGMENTS] = {0x1, 0x0, 0xa, 0x2};
static const u64 t6030_dcpext_text_remap = 0x10040000000UL;
static const u64 t6030_dcpext_data_remap = 0x10080000000UL;
static const u64 t6030_dcpext_scratch_remap[2] = {0x100c0000000UL, 0x100c1000000UL};

/* Carveouts holding the page tables of each controller's DART streams. */
static const u32 t6030_dcpext_table_regions[2][3] = {{70, 68, 69}, {85, 83, 84}};

static bool ranges_overlap(u64 a, u64 a_size, u64 b, u64 b_size)
{
    return a < b + b_size && b < a + a_size;
}

/* An existing reservation of the same name must cover exactly the same range. */
static bool dt_t6030_reservation_matches(const char *name, u64 paddr, u64 size)
{
    int resv = fdt_path_offset(dt, "/reserved-memory");
    int node = resv < 0 ? resv : fdt_subnode_offset(dt, resv, name);
    int len;

    if (node < 0)
        return true;

    const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
    return reg && len == 2 * sizeof(*reg) && fdt64_ld(&reg[0]) == paddr &&
           fdt64_ld(&reg[1]) == size && fdt_getprop(dt, node, "no-map", NULL);
}

static int dt_t6030_dcpext_validate(unsigned int n, const char *nub_path, char *uuid_out)
{
    int nub = adt_path_offset(adt, nub_path);
    if (nub < 0)
        bail("ADT: J514S DCPEXT%u: %s not found\n", n, nub_path);

    u32 len = 0;
    const struct adt_segment_ranges *segs = adt_getprop(adt, nub, "segment-ranges", &len);
    if (!segs || len != T6030_DCPEXT_SEGMENTS * sizeof(*segs))
        bail("ADT: J514S DCPEXT%u: unexpected segment-ranges (%u bytes)\n", n, segs ? len : 0);

    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram_base = 0, dram_size = 0;
    if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base) ||
        ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size) || !dram_size ||
        dram_base > UINT64_MAX - dram_size)
        bail("ADT: J514S DCPEXT%u: no DRAM bounds\n", n);

    for (unsigned int i = 0; i < T6030_DCPEXT_SEGMENTS; i++) {
        struct adt_segment_ranges seg;
        u64 remap;
        char name[64];

        memcpy(&seg, &segs[i], sizeof(seg));
        u64 size = ALIGN_UP((u64)seg.size, SZ_16K);

        switch (i) {
            case 0:
                remap = t6030_dcpext_text_remap;
                break;
            case 1:
                remap = t6030_dcpext_data_remap;
                break;
            case 2:
                remap = seg.phys; /* the OS log runs at its physical address */
                break;
            default:
                remap = t6030_dcpext_scratch_remap[n];
                break;
        }

        if (seg.unk != t6030_dcpext_seg_flags[i] || seg.remap != remap || !seg.size ||
            (seg.phys & (SZ_16K - 1)) || seg.phys < dram_base || size > dram_size ||
            seg.phys - dram_base > dram_size - size)
            bail("ADT: J514S DCPEXT%u: unexpected segment %u: PA 0x%lx remap 0x%lx size 0x%x "
                 "flags 0x%x\n",
                 n, i, seg.phys, seg.remap, seg.size, seg.unk);

        for (unsigned int j = 0; j < i; j++) {
            struct adt_segment_ranges prev;

            memcpy(&prev, &segs[j], sizeof(prev));
            if (ranges_overlap(seg.phys, size, prev.phys, ALIGN_UP((u64)prev.size, SZ_16K)))
                bail("ADT: J514S DCPEXT%u: segments %u and %u overlap\n", n, j, i);
        }

        /* TEXT is the panel's image and shares its reservation. */
        snprintf(name, sizeof(name), "asc-firmware@%lx", seg.phys);
        if (!dt_t6030_reservation_matches(name, seg.phys, size))
            bail("FDT: J514S DCPEXT%u: conflicting reservation %s\n", n, name);
    }

    int cmap = adt_path_offset(adt, "/chosen/carveout-memory-map");
    for (unsigned int i = 0; i < 3; i++) {
        char prop[24];
        u64 region[2];

        snprintf(prop, sizeof(prop), "region-id-%u", t6030_dcpext_table_regions[n][i]);
        if (cmap < 0 || ADT_GETPROP_ARRAY(adt, cmap, prop, region) != sizeof(region) ||
            !region[1] || (region[0] | region[1]) & (SZ_16K - 1) || region[0] < dram_base ||
            region[1] > dram_size || region[0] - dram_base > dram_size - region[1])
            bail("ADT: J514S DCPEXT%u: no DART table storage %s\n", n, prop);
    }

    /* The image is the panel's: only hand over what the panel's handoff qualified. */
    const char *uuid = adt_getprop(adt, nub, "uuid", &len);
    if (!uuid || !len || len > 64 || strnlen(uuid, len) != 36)
        bail("ADT: J514S DCPEXT%u: no firmware UUID\n", n);
    int dcp = fdt_path_offset(dt, "dcp");
    int panel_len = 0;
    const char *panel = dcp < 0 ? NULL : fdt_getprop(dt, dcp, "apple,firmware-uuid", &panel_len);
    if (!panel || panel_len != 37)
        bail("FDT: J514S DCPEXT%u: the internal display firmware was not handed over\n", n);
    if (strncmp(uuid, panel, 36) || panel[36])
        bail("FDT: J514S DCPEXT%u: firmware %.36s is not the panel's %s\n", n, uuid, panel);

    memcpy(uuid_out, panel, 37);
    return 0;
}

/* Reserve the DART table storage that lies in usable RAM. */
static int dt_t6030_dcpext_reserve_tables(unsigned int n)
{
    int cmap = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u64 ram = cur_boot_args.phys_base, ram_size = cur_boot_args.mem_size;

    for (unsigned int i = 0; i < 3; i++) {
        char prop[24], name[40];
        u64 region[2];

        snprintf(prop, sizeof(prop), "region-id-%u", t6030_dcpext_table_regions[n][i]);
        if (ADT_GETPROP_ARRAY(adt, cmap, prop, region) != sizeof(region))
            return -1;

        u64 size = ALIGN_UP(region[1], SZ_16K);
        if (!ranges_overlap(region[0], size, ram, ram_size))
            continue;

        snprintf(name, sizeof(name), "dart-tables@%lx", region[0]);
        if (dt_get_or_add_reserved_mem(name, "apple,dart-mem", true, region[0], size) < 0)
            return -1;
    }

    return 0;
}

/*
 * Enable the controller's Type-C display routes, then the controller. DCPEXT0
 * drives the built-in HDMI port instead: a fixed output without routes.
 */
static int dt_t6030_dcpext_enable(const char *alias)
{
    int node = fdt_path_offset(dt, alias);
    if (node < 0)
        return -1;

    int routes = fdt_subnode_offset(dt, node, "typec-routes");
    if (routes < 0 && routes != -FDT_ERR_NOTFOUND)
        return -1;

    int route;
    if (routes >= 0)
        fdt_for_each_subnode(route, dt, routes)
        {
            int len = 0;
            const char *status = fdt_getprop(dt, route, "status", &len);
            if (!status) {
                if (len == -FDT_ERR_NOTFOUND)
                    continue;
                return len;
            }
            /* Preserve unavailable and malformed route policy from the DT. */
            if (len != sizeof("disabled") || memcmp(status, "disabled", len))
                continue;
            if (fdt_setprop_string(dt, route, "status", "okay"))
                return -1;
        }
    if (routes >= 0 && route != -FDT_ERR_NOTFOUND)
        return route;

    node = fdt_path_offset(dt, alias);
    if (node < 0 || fdt_setprop_string(dt, node, "status", "okay"))
        return -1;

    return 0;
}

static int dt_set_t6030_dcpext(unsigned int n)
{
    char alias[16], nub_path[48], uuid[40];

    if (n >= 2)
        return -1;

    snprintf(alias, sizeof(alias), "dcpext%u", n);
    if (!fdt_get_alias(dt, alias))
        return 0; /* not described by this device tree */

    int node = fdt_path_offset(dt, alias);
    if (node < 0)
        return -1;
    int status_len = 0;
    const char *status = fdt_getprop(dt, node, "status", &status_len);
    if (status && status_len == sizeof("okay") && !memcmp(status, "okay", status_len)) {
        printf("FDT: J514S DCPEXT%u already enabled; leaving it alone\n", n);
        return 0;
    }
    if (status && (status_len != sizeof("disabled") || memcmp(status, "disabled", status_len)))
        return -1;
    if (!status && status_len != -FDT_ERR_NOTFOUND)
        return status_len;

    snprintf(nub_path, sizeof(nub_path), "/arm-io/%s/iop-%s-nub", alias, alias);
    if (dt_t6030_dcpext_validate(n, nub_path, uuid))
        return -1;

    /* Everything validated: from here on only FDT updates. */
    if (dt_t6030_dcpext_reserve_tables(n) ||
        dt_reserve_asc_firmware(nub_path, NULL, alias, true, 0) || dt_set_dcp_firmware(alias)) {
        printf("FDT: J514S DCPEXT%u: cannot reserve the firmware\n", n);
        return -1;
    }

    node = fdt_path_offset(dt, alias);
    if (node < 0 || fdt_setprop_string(dt, node, "apple,firmware-uuid", uuid) ||
        dt_t6030_dcpext_enable(alias)) {
        printf("FDT: J514S DCPEXT%u: cannot publish the controller\n", n);
        return -1;
    }

    printf("FDT: J514S DCPEXT%u firmware %s handed over\n", n, uuid);
    return 0;
}

/* Native handoffs publish a complete dependency group or leave the live DT
 * byte-for-byte unchanged. No hardware is changed by these callbacks. */
static int dt_t6030_panel(unsigned int unused)
{
    (void)unused;
    /* The frontend mapping check replaces this with its own result. */
    snprintf(t6030_display_handoff, sizeof(t6030_display_handoff), "DCP firmware handoff refused");
    return dt_set_t6030_dcp_handoff();
}

/* The native path of the matched J514S development DT: the panel, then each external controller. */
static void dt_set_j514s_native(void)
{
    /* The panel step replaces this as it starts. */
    snprintf(t6030_display_handoff, sizeof(t6030_display_handoff),
             "no memory for the handoff transaction");
    if (dt_display_transaction(dt_t6030_panel, 0)) {
        printf("FDT: J514S native handoff refused; keeping the boot framebuffer\n");
        /* The refused transaction discarded its edits, its result with them. */
        int chosen = fdt_path_offset(dt, "/chosen");
        if (chosen < 0 ||
            fdt_setprop_string(dt, chosen, "aurora,j514s-display-handoff", t6030_display_handoff))
            printf("FDT: cannot record the J514S display handoff result\n");
        return;
    }
    for (unsigned int n = 0; n < 2; n++)
        if (dt_display_transaction(dt_set_t6030_dcpext, n))
            printf("FDT: J514S DCPEXT%u handoff refused\n", n);
}

struct j713_display_maps {
    const struct adt_segment_ranges *segments;
    u32 count;
    u64 bytes[32];
};

static int j713_display_page(u64 iova, u64 physical, void *opaque)
{
    struct j713_display_maps *maps = opaque;
    for (u32 i = 0; i < maps->count; i++) {
        const struct adt_segment_ranges *seg = &maps->segments[i];
        u64 start = seg->remap & ((1ULL << 36) - 1);
        u64 size = ALIGN_UP((u64)seg->size, SZ_16K);
        if (seg->unk != 2 || iova < start || iova - start >= size)
            continue;
        if ((seg->remap >> 36) != 0x10 || physical != seg->phys + iova - start)
            return -1;
        maps->bytes[i] += SZ_16K;
        return 0;
    }
    return -1; /* Never drop an inherited mapping that we cannot describe. */
}

static int dt_set_j713_display_maps(const struct adt_segment_ranges *segments, u32 count,
                                    u64 dram_start, u64 dram_size)
{
    const char *aliases[] = {"disp0", "disp0_piodma"};
    const u8 streams[] = {0, 16};
    struct j713_display_maps maps[2] = {
        {.segments = segments, .count = count},
        {.segments = segments, .count = count},
    };
    u32 phandles[2];

    /* Exact J713 register block; reading tables must not enable/reset streams. */
    for (u32 n = 0; n < 2; n++) {
        if (dart_walk_locked_t8110(0x30a300000, 0xc000, streams[n], dram_start, dram_size,
                                   j713_display_page, &maps[n]))
            return -1;
        u32 regions = 0;
        for (u32 i = 0; i < count; i++) {
            if (!maps[n].bytes[i])
                continue;
            if (maps[n].bytes[i] != ALIGN_UP((u64)segments[i].size, SZ_16K))
                return -1;
            regions++;
        }
        if (regions != (n == 0 ? 2 : 1))
            return -1;
    }

    /* Publish only after every inherited PTE passed the physical/IOVA check. */
    for (u32 n = 0; n < 2; n++) {
        int node = fdt_path_offset(dt, aliases[n]);
        if (node < 0)
            return -1;
        phandles[n] = fdt_get_phandle(dt, node);
        if (!phandles[n]) {
            if (fdt_generate_phandle(dt, &phandles[n]) ||
                fdt_setprop_u32(dt, node, "phandle", phandles[n]))
                return -1;
        }
    }
    for (u32 i = 0; i < count; i++) {
        if (!maps[0].bytes[i] && !maps[1].bytes[i])
            continue;
        char name[64];
        snprintf(name, sizeof(name), "asc-firmware@%lx", segments[i].phys);
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);
        int node = dt_get_or_add_reserved_mem(name, "apple,asc-mem", true, segments[i].phys, size);
        if (node < 0)
            return -1;
        u32 phandle = fdt_get_phandle(dt, node);
        for (u32 n = 0; n < 2; n++) {
            if (maps[n].bytes[i] &&
                dt_device_set_reserved_mem(node, name, phandles[n], segments[i].remap, size))
                return -1;
        }
        for (u32 n = 0; n < 2; n++) {
            if (maps[n].bytes[i] && dt_device_add_mem_region(aliases[n], phandle, NULL))
                return -1;
        }
    }
    int piodma = fdt_path_offset(dt, "disp0_piodma");
    int dart = dt_get_iommu_node(piodma, 0);
    if (dart < 0 || fdt_setprop_string(dt, dart, "status", "okay"))
        return -1;
    piodma = fdt_path_offset(dt, "disp0_piodma");
    if (fdt_setprop_empty(dt, piodma, "apple,j713-inherited-mappings"))
        return -1;
    if (fdt_setprop_string(dt, piodma, "status", "okay"))
        return -1;
    printf("FDT: J713 DISP0 SID0/SID16 mappings preserved; PIODMA enabled\n");
    int dcp = fdt_path_offset(dt, "dcp");
    if (dcp >= 0 && fdt_getprop(dt, dcp, "apple,j713-native-scanout", NULL)) {
        int display = fdt_path_offset(dt, "disp0");
        if (display < 0 ||
            (fdt_node_check_compatible(dt, display, "apple,t8132-display-diagnostics") &&
             fdt_node_check_compatible(dt, display, "apple,t8132-display-subsystem")))
            return -1;
        if (fdt_setprop_empty(dt, display, "apple,j713-inherited-mappings") ||
            fdt_setprop_string(dt, display, "status", "okay"))
            return -1;
        printf("FDT: J713 native scanout diagnostic enabled with preserved SID0 mappings\n");
    }
    return 0;
}

/* Publish the inherited J713 firmware mapping without starting a display driver. */
static int dt_set_j713_dcp_handoff(void)
{
    int dcp_node = fdt_path_offset(dt, "dcp");
    if (dcp_node < 0)
        return 0; /* Older DT with simple-framebuffer only. */
    if (chip_id != T8132 || fdt_node_check_compatible(dt, 0, "apple,j713") ||
        fdt_node_check_compatible(dt, dcp_node, "apple,t8132-dcp"))
        bail("FDT: unsupported native DCP handoff target\n");

    int nub = adt_path_offset(adt, "/arm-io/dcp/iop-dcp-nub");
    if (nub < 0)
        bail("ADT: missing J713 DCP firmware metadata\n");

    u32 len;
    const struct adt_segment_ranges *segments = adt_getprop(adt, nub, "segment-ranges", &len);
    if (!segments || !len || len % sizeof(*segments) || len / sizeof(*segments) > 32)
        bail("ADT: invalid J713 DCP firmware segment list\n");
    u32 count = len / sizeof(*segments);

    /* Firmware carveouts are outside the OS-usable boot-args interval. */
    int chosen = adt_path_offset(adt, "/chosen");
    u64 dram_start, dram_size;
    if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_start) < 0 ||
        ADT_GETPROP(adt, chosen, "dram-size", &dram_size) < 0)
        bail("ADT: missing J713 physical DRAM bounds\n");
    u64 dram_end = dram_start + dram_size;
    if (!dram_size || dram_end <= dram_start)
        bail("ADT: invalid J713 DRAM bounds\n");

    /* Validate the entire list before mutating the FDT. Use remap addresses:
     * CPU-image offsets are not the addresses in the inherited DCP DART.
     */
    for (unsigned i = 0; i < len / sizeof(*segments); i++) {
        u64 size = ALIGN_UP((u64)segments[i].size, SZ_16K);
        if (!size || (segments[i].phys | segments[i].remap) & (SZ_16K - 1) ||
            segments[i].phys < dram_start || segments[i].phys + size < segments[i].phys ||
            segments[i].phys + size > dram_end || segments[i].remap + size < segments[i].remap)
            bail("ADT: invalid J713 DCP firmware segment %u\n", i);
    }

    const char *uuid = adt_getprop(adt, nub, "uuid", &len);
    if (!uuid || len != 37 || uuid[len - 1])
        bail("ADT: invalid J713 DCP firmware UUID\n");
    if (fdt_setprop_string(dt, dcp_node, "apple,firmware-uuid", uuid))
        bail("FDT: cannot publish J713 DCP firmware UUID\n");

    int ret = dt_set_dcp_firmware("dcp");
    if (ret)
        return ret;
    ret = dt_reserve_asc_firmware("/arm-io/dcp/iop-dcp-nub", NULL, "dcp", true, 0);
    if (ret)
        return ret;

    dcp_node = fdt_path_offset(dt, "dcp");
    if (fdt_getprop(dt, dcp_node, "apple,j713-native-piodma", NULL)) {
        if (strcmp(uuid, "81BAF476-679D-3CC0-8C6A-E6B6FA659D5E") ||
            dt_set_j713_display_maps(segments, count, dram_start, dram_size))
            printf("FDT: J713 PIODMA handoff unqualified; retaining disabled device\n");
    }

    printf("FDT: J713 DCP firmware mappings published; display device status unchanged\n");
    return 0;
}

static int dt_set_display(void)
{
    if (chip_id == T8140)
        return dt_set_display_t8140();

    /* lock dart-disp0 to prevent old software from resetting it */
    dart_lock_adt("/arm-io/dart-disp0", 0);
    const char compat_piodma_alias[] = "disp0_piodma";
    const char piodma_alias[] = "disp0-piodma";
    const char *disp0_piodma_alias = piodma_alias;

    /* Add "/reserved-memory" nodes with iommu mapping and link them to their
     * devices. The memory is already excluded from usable RAM so these nodes
     * are only required to inform the OS about the existing mappings.
     * Required for disp0, dcp and all dcpext.
     * Checks for dcp* / disp*_piodma / disp* aliases and fails silently if
     * they are missing. */

    int ret = 0;

    /* Use old style disp0_piodma alias with underscore if it exists. */
    if (!!fdt_get_alias(dt, compat_piodma_alias))
        disp0_piodma_alias = compat_piodma_alias;

    if (chip_id == T8132) {
        if (dt_set_j713_dcp_handoff())
            printf("FDT: J713 DCP handoff refused; keeping the boot framebuffer\n");
        return 0;
    }

    if (!fdt_node_check_compatible(dt, 0, "apple,t8103")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", disp0_piodma_alias,
                                           disp_reserved_regions_t8103,
                                           ARRAY_SIZE(disp_reserved_regions_t8103));
        if (ret)
            return ret;

        ret = dt_carveout_reserved_regions("dcpext", NULL, NULL, dcpext_reserved_regions_t8103,
                                           ARRAY_SIZE(dcpext_reserved_regions_t8103));
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t8112")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", disp0_piodma_alias,
                                           disp_reserved_regions_t8112,
                                           ARRAY_SIZE(disp_reserved_regions_t8112));
        if (ret)
            return ret;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6000") ||
               !fdt_node_check_compatible(dt, 0, "apple,t6001") ||
               !fdt_node_check_compatible(dt, 0, "apple,t6002")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", disp0_piodma_alias,
                                           disp_reserved_regions_t600x,
                                           ARRAY_SIZE(disp_reserved_regions_t600x));
        if (ret)
            return ret;

        if (os_firmware.version >= V13_5) {
            for (int n = 0; n < MAX_DCPEXT && ret == 0; n++) {
                char dcpext_alias[16];

                snprintf(dcpext_alias, sizeof(dcpext_alias), "dcpext%d", n);
                ret = dt_carveout_reserved_regions(dcpext_alias, NULL, NULL,
                                                   dcpext_reserved_regions_t600x[n],
                                                   ARRAY_SIZE(dcpext_reserved_regions_t600x[n]));
            }
        }
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6020") ||
               !fdt_node_check_compatible(dt, 0, "apple,t6021")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", disp0_piodma_alias,
                                           disp_reserved_regions_t602x,
                                           ARRAY_SIZE(disp_reserved_regions_t602x));
        if (ret)
            return ret;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6022")) {
        /* noop */
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6030") &&
               !fdt_node_check_compatible(dt, 0, "apple,j514s")) {
        /* Keep unconditional live-log reservations and the legacy handoff.
         * Only the matched development DT asks for native consumer enablement. */
        dt_set_display_handoff(dt);
        int dcp = fdt_path_offset(dt, "dcp");
        if (dcp < 0 || !fdt_getprop(dt, dcp, "apple,j514s-native-piodma", NULL))
            return 0;
        dt_set_j514s_native();
        return 0;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6030") ||
               !fdt_node_check_compatible(dt, 0, "apple,t8122") ||
               !fdt_node_check_compatible(dt, 0, "apple,t6031")) {
        /*
         * Fail-soft handoff (kboot_display_handoff.c): it always returns 0, never uses the
         * helpers above (they bail out of kboot or enable nodes) and never enables a node.
         * On T8122 it does nothing unless chosen.asahi,t8122-dcp=1; the external DCP firmware is
         * reserved only with chosen.asahi,t8122-dcpext=1 and that handoff. On T6031 it does nothing
         * unless the board is a J514C or J516C, and reserves only __OS_LOG unless
         * chosen.asahi,t6031-dcp=1 (on a J516C, dt_set_m3_defaults() sets it unless a config line
         * set it).
         */
        ret = dt_set_display_handoff(dt);
        if (!fdt_node_check_compatible(dt, 0, "apple,t8122"))
            dt_reserve_dcpext_firmware(false);
        return ret;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6040")) {
        /*
         * Fail-soft T6041 display block handoff (kboot_display_handoff.c): nothing unless the
         * board is a J616s whose device tree describes the display block; the display nodes stay
         * disabled.
         */
        return dt_set_display_handoff(dt);
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t6032")) {
        /*
         * T6032 (J575D): refused until the handoff's firmware ABI exists. The missing pieces:
         * a dcp6 display-gate profile for T6032 pinned to this OS firmware (27.0/27.0.1,
         * mBoot-20457.1.29) in kboot_display_handoff.c, and a kernel side T6032 ABI; the 27.0.1
         * ADT also has no dcp/disp0 processor at all (the Studio's displays are dcpext0..7), so
         * the T6031 profile cannot be reused as is. Nothing is enabled by default.
         */
        printf("FDT: T6032: display handoff refused: no T6032 dcp6 display-gate profile for this "
               "OS firmware (%s) or kernel ABI exists yet; the ADT also has no dcp/disp0 "
               "(displays are dcpext0..7)\n",
               os_firmware.string);
        return 0;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t8142")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", "disp0_piodma",
                                           disp_reserved_regions_t8142,
                                           ARRAY_SIZE(disp_reserved_regions_t8142));
        if (ret)
            return ret;
    } else {
        printf("FDT: unknown compatible, skip display reserved-memory setup\n");
        return 0;
    }
    if (ret)
        return ret;

    /*
     * Ignore errors for dcpext firmware reservation, nodes should be disabled
     * if the reservation fails allowing basic operation with just dcp.
     */
    if (!fdt_node_check_compatible(dt, 0, "apple,t8112") ||
        !fdt_node_check_compatible(dt, 0, "apple,t6020") ||
        !fdt_node_check_compatible(dt, 0, "apple,t6021") ||
        !fdt_node_check_compatible(dt, 0, "apple,t6022"))
        dt_reserve_dcpext_firmware(true);

    const display_config_t *disp_cfg = display_get_config();

    ret = dt_vram_reserved_region(disp_cfg ? disp_cfg->dcp_alias : "dcp", "disp0");
    if (ret)
        return ret;
    if (!fdt_node_check_compatible(dt, 0, "apple,t8142"))
        return dt_reserve_m5_display_segments();
    return 0;
}

static const char *excluded_pmp_props[] = {
    "compatible",    "AAPL,phandle",    "region-base", "region-size",
    "segment-names", "segment-ranges",  "pre-loaded",  "firmware-name",
    "dram-capacity", "coredump-enable", "name",        NULL,
};

static bool skip_pmp_prop(const char *prop_name)
{
    for (int i = 0; excluded_pmp_props[i]; i++)
        if (!strcmp(prop_name, excluded_pmp_props[i]))
            return true;
    return false;
}

/* Reads a u32 ADT property only if it is exactly four bytes long. */
static int adt_get_u32_exact(int node, const char *name, u32 *val)
{
    u32 len = 0;
    const void *p = adt_getprop(adt, node, name, &len);

    if (!p || len != sizeof(*val))
        return -1;
    memcpy(val, p, sizeof(*val));
    return 0;
}

/*
 * Copies this Mac's PMP values from the ADT onto the FDT node @pmp_node, for dt_pass_pmp() on
 * T6030 and T8122. Stricter than dt_set_pmp(), which M1 and M2 Macs with a pmp node take: every
 * value must be exactly a u32.
 */
static int dt_fill_pmp(int pmp_node)
{
    int chosen_anode = adt_path_offset(adt, "/chosen");
    if (chosen_anode < 0)
        bail("ADT: /chosen not found \n");
    int pmp_iop_anode = adt_path_offset(adt, "/arm-io/pmp/iop-pmp-nub");
    if (pmp_iop_anode < 0)
        bail("ADT: /arm-io/pmp/iop-pmp-nub not found \n");

    u32 board_id, dram_vendor_id, dram_capacity = 0xFFFFFFFF;
    if (adt_get_u32_exact(chosen_anode, "board-id", &board_id))
        bail("ADT: failed to get board id\n");
    if (adt_get_u32_exact(chosen_anode, "dram-vendor-id", &dram_vendor_id))
        bail("ADT: failed to get dram vendor id\n");
    if (adt_getprop(adt, pmp_iop_anode, "dram-capacity", NULL) &&
        adt_get_u32_exact(pmp_iop_anode, "dram-capacity", &dram_capacity))
        bail("ADT: dram-capacity is not a u32\n");

    if (fdt_setprop_u32(dt, pmp_node, "apple,board-id", board_id))
        bail("FDT: failed to set board id\n");
    if (fdt_setprop_u32(dt, pmp_node, "apple,dram-vendor-id", dram_vendor_id))
        bail("FDT: failed to set dram vendor id\n");
    if (dram_capacity != 0xFFFFFFFF &&
        fdt_setprop_u32(dt, pmp_node, "apple,dram-capacity", dram_capacity))
        bail("FDT: failed to set dram capacity\n");

    ADT_FOREACH_PROPERTY(adt, pmp_iop_anode, prop)
    {
        if (skip_pmp_prop(prop->name))
            continue;
        char prop_name[128];
        snprintf(prop_name, sizeof(prop_name), "apple,tunable-%s", prop->name);
        if (fdt_setprop(dt, pmp_node, prop_name, prop->value, prop->size))
            bail("FDT: failed to transfer pmp tunable\n");
    }
    return 0;
}

/* The device tree's pmp node (M1 Pro/Max/Ultra, M2, M2 Pro/Max/Ultra): as upstream has it */
static int dt_set_pmp(void)
{
    /* J613 25G83 static topology is enabled only by the kernel handoff gate. */
    if (chip_id == T8122)
        return 0;
    int pmp_node = fdt_path_offset(dt, "pmp");
    if (pmp_node < 0) {
        printf("FDT: pmp not found in devtree\n");
        return 0;
    }
    int chosen_anode = adt_path_offset(adt, "/chosen");
    if (chosen_anode < 0)
        bail("ADT: /chosen not found \n");
    int pmp_iop_anode = adt_path_offset(adt, "/arm-io/pmp/iop-pmp-nub");
    if (pmp_iop_anode < 0)
        bail("ADT: /arm-io/pmp/iop-pmp-nub not found \n");

    u32 board_id, dram_vendor_id, dram_capacity = 0xFFFFFFFF;
    if (ADT_GETPROP(adt, chosen_anode, "board-id", &board_id) < 0)
        bail("ADT: failed to get board id\n");
    if (ADT_GETPROP(adt, chosen_anode, "dram-vendor-id", &dram_vendor_id) < 0)
        bail("ADT: failed to get dram vendor id\n");
    ADT_GETPROP(adt, pmp_iop_anode, "dram-capacity", &dram_capacity);

    if (fdt_setprop_u32(dt, pmp_node, "apple,board-id", board_id))
        bail("FDT: failed to set board id\n");
    if (fdt_setprop_u32(dt, pmp_node, "apple,dram-vendor-id", dram_vendor_id))
        bail("FDT: failed to set dram vendor id\n");
    if (dram_capacity != 0xFFFFFFFF &&
        fdt_setprop_u32(dt, pmp_node, "apple,dram-capacity", dram_capacity))
        bail("FDT: failed to set dram capacity\n");

    ADT_FOREACH_PROPERTY(adt, pmp_iop_anode, prop)
    {
        if (skip_pmp_prop(prop->name))
            continue;
        char prop_name[128];
        snprintf(prop_name, sizeof(prop_name), "apple,tunable-%s", prop->name);
        if (fdt_setprop(dt, pmp_node, prop_name, prop->value, prop->size))
            bail("FDT: failed to transfer pmp tunable");
    }
    pmp_node = fdt_path_offset(dt, "pmp");
    fdt_setprop_string(dt, pmp_node, "status", "okay");
    return 0;
}

/*
 * T6030 and T8122: the kernel's display gate adds the PMP itself, halted until
 * the display needs it, so the device tree has no pmp node for dt_set_pmp().
 * The gate needs this Mac's PMP values for it: copy them as dt_set_pmp() does,
 * into /chosen/asahi,<soc>-pmp, and leave the PMP alone. Only when the PMP and
 * its DART are where the gate puts them. Fail-soft: without the node the gate
 * keeps the display on the boot framebuffer, except on a J516S, which has
 * built-in values.
 */
struct pmp_values_soc {
    const char *compat; /* SoC compatible of the FDT root */
    const char *node;   /* the /chosen subnode */
    const char *name;
    u32 irqs[4]; /* /arm-io/pmp interrupts, in ADT order */
    u64 dart_addr, dart_size;
};

static const struct pmp_values_soc pmp_values_t6030 = {
    "apple,t6030", "asahi,t6030-pmp", "T6030", {731, 730, 733, 732}, 0x350300000, 0x4000,
};

static const struct pmp_values_soc pmp_values_t8122 = {
    "apple,t8122", "asahi,t8122-pmp", "T8122", {622, 621, 624, 623}, 0x2d0300000, 0x4000,
};

static void dt_pass_pmp(const struct pmp_values_soc *soc)
{
    int dart_path[8];
    const void *irqs;
    u64 dart_addr = 0, dart_size = 0;
    u32 len = 0;
    int pmp_anode, chosen, node;
    bool where;
    char path[40];

    if (fdt_node_check_compatible(dt, 0, soc->compat) ||
        (fdt_path_offset(dt, "pmp") >= 0 && chip_id != T8122))
        return;

    chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return;
    node = fdt_add_subnode(dt, chosen, soc->node);
    if (node < 0) {
        printf("FDT: %s PMP values not passed: %d\n", soc->name, node);
        return;
    }

    pmp_anode = adt_path_offset(adt, "/arm-io/pmp");
    irqs = pmp_anode < 0 ? NULL : adt_getprop(adt, pmp_anode, "interrupts", &len);
    where = irqs && len == sizeof(soc->irqs) && !memcmp(irqs, soc->irqs, sizeof(soc->irqs)) &&
            adt_path_offset_trace(adt, "/arm-io/dart-pmp", dart_path) >= 0 &&
            !adt_get_reg(adt, dart_path, "reg", 0, &dart_addr, &dart_size) &&
            dart_addr == soc->dart_addr && dart_size == soc->dart_size;
    if (!where) {
        /* An empty node: the kernel refuses the PMP rather than using built-in values. */
        printf("FDT: %s PMP is not where the kernel expects it; no PMP values passed\n", soc->name);
        return;
    }
    snprintf(path, sizeof(path), "/chosen/%s", soc->node);
    if (dt_fill_pmp(node)) {
        node = fdt_path_offset(dt, path);
        if (node >= 0)
            fdt_del_node(dt, node);
        printf("FDT: %s PMP values not passed\n", soc->name);
        return;
    }
    printf("FDT: %s PMP values passed in %s\n", soc->name, path);
}

static int dt_set_pmp_t6030(void)
{
    dt_pass_pmp(&pmp_values_t6030);
    return 0;
}

/* T6031: on every boot of a J514C or J516C; the PMP and its DART are where the kernel puts them.
 * There is no T6032 entry: the J575D 27.0.1 ADT has neither /arm-io/pmp nor /arm-io/dart-pmp,
 * so there are no PMP values to pass (dt_pass_pmp() would refuse them anyway).
 */
static const struct pmp_values_soc pmp_values_t6031 = {
    "apple,t6031", "asahi,t6031-pmp", "T6031", {1181, 1180, 1183, 1182}, 0x290300000, 0x4000,
};

static int dt_set_pmp_t6031(void)
{
    if (t6031_board(dt))
        dt_pass_pmp(&pmp_values_t6031);
    return 0;
}

/*
 * T8122: pass this Mac's PMP values on either M3 Air with the display
 * handoff switch, chosen.asahi,t8122-dcp=1.
 */
static int dt_set_pmp_t8122(void)
{
    int chosen = fdt_path_offset(dt, "/chosen");
    int len = 0;
    const char *opt = chosen < 0 ? NULL : fdt_getprop(dt, chosen, "asahi,t8122-dcp", &len);

    if (!opt || len != 2 || opt[0] != '1' || opt[1] != '\0' ||
        (fdt_node_check_compatible(dt, 0, "apple,j613") &&
         fdt_node_check_compatible(dt, 0, "apple,j615")))
        return 0;
    dt_pass_pmp(&pmp_values_t8122);
    return 0;
}

static int dt_set_sep(void)
{
    int aliases = fdt_path_offset(dt, "/aliases");
    int alias_len = 0;
    const char *alias = aliases < 0 ? NULL : fdt_getprop(dt, aliases, "sep", &alias_len);
    if (alias == NULL) {
        printf("FDT: sep alias not found in devtree\n");
        return 0;
    }

    /* FDT insertion moves properties. Copy the bounded alias before reserving memory. */
    char path[256];
    if (alias_len < 2 || alias_len > (int)sizeof(path) || alias[0] != '/' ||
        memchr(alias, '\0', alias_len) != alias + alias_len - 1)
        bail("FDT: invalid sep alias path\n");
    memcpy(path, alias, alias_len);

    u64 dram_base = 0, dram_size = 0;
    if (chip_id == T8140) {
        int chosen = adt_path_offset(adt, "/chosen");
        if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &dram_base) != sizeof(dram_base))
            bail("ADT: invalid DRAM base for SEP\n");
        if (ADT_GETPROP(adt, chosen, "dram-size", &dram_size) != sizeof(dram_size))
            dram_size = mem_size_actual;
        if (!dram_size || dram_base > UINT64_MAX - dram_size)
            bail("ADT: invalid DRAM extent for SEP\n");
    }

    int anode_mmap = adt_path_offset(adt, "/chosen/memory-map");
    if (anode_mmap < 0)
        bail("ADT: /chosen/memory-map not found \n");

    u64 phys_map[2];
    int ret = ADT_GETPROP_ARRAY(adt, anode_mmap, "SEPFW", phys_map);
    if (ret != (int)sizeof(phys_map) || !phys_map[0] || !phys_map[1] ||
        phys_map[0] > UINT64_MAX - phys_map[1] ||
        (chip_id == T8140 && (phys_map[0] < dram_base || phys_map[0] > dram_base + dram_size ||
                              phys_map[1] > dram_base + dram_size - phys_map[0])))
        bail("ADT: could not get sepfw memory\n");

    const char *node_name = "sep-firmware";
    int mem_node =
        dt_get_or_add_reserved_mem(node_name, "apple,asc-mem", false, phys_map[0], phys_map[1]);
    if (mem_node < 0)
        bail("FDT: failed to reserve sepfw");

    uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);
    ret = dt_device_add_mem_region(path, mem_phandle, "sepfw");
    if (ret < 0)
        bail("FDT: failed to add sepfw region");

    int anode_manifest = adt_path_offset(adt, "/chosen/boot-object-manifests");
    if (anode_manifest < 0) {
        printf("ADT: no SEP boot-object manifests\n");
        return 0;
    }

    const char *adt_names[] = {"lpol", "ibot"};
    const char *dt_names[] = {"local-policy-manifest", "iboot-manifest"};
    for (size_t i = 0; i < ARRAY_SIZE(adt_names); i++) {
        ret = ADT_GETPROP_ARRAY(adt, anode_manifest, adt_names[i], phys_map);
        if (ret != (int)sizeof(phys_map) || !phys_map[0] || !phys_map[1] ||
            phys_map[1] > (u64)dt_bufsize || phys_map[0] > UINT64_MAX - phys_map[1] ||
            (chip_id == T8140 && (phys_map[0] < dram_base || phys_map[0] > dram_base + dram_size ||
                                  phys_map[1] > dram_base + dram_size - phys_map[0]))) {
            printf("ADT: no usable SEP %s manifest\n", adt_names[i]);
            continue;
        }
        int node = fdt_path_offset(dt, path);
        if (node < 0 || fdt_setprop(dt, node, dt_names[i], (void *)phys_map[0], phys_map[1]))
            printf("FDT: could not publish SEP %s manifest\n", adt_names[i]);
    }

    return 0;
}

static int dt_set_sio_fwdata(const char *adt_path, const char *fdt_alias)
{
    int node = fdt_path_offset(dt, fdt_alias);
    if (node < 0) {
        printf("FDT: '%s' node not found\n", fdt_alias);
        return 0;
    }

    int phandle = fdt_get_phandle(dt, node);
    uint32_t max_phandle;
    int ret = fdt_find_max_phandle(dt, &max_phandle);
    if (ret)
        bail("FDT: failed to get max phandle: %d\n", ret);

    if (!phandle) {
        phandle = ++max_phandle;
        ret = fdt_setprop_u32(dt, node, "phandle", phandle);
        if (ret != 0)
            bail("FDT: couldn't set '%s.phandle' property: %d\n", fdt_alias, ret);
    }

    struct sio_data *siodata = sio_setup_fwdata(adt_path);
    if (!siodata)
        bail("FDT: failed to set up SIO(%s) firmware data\n", adt_path);

    for (int i = 0; i < siodata->num_fwdata; i++) {
        struct sio_mapping *mapping = &siodata->fwdata[i];

        char node_name[64];
        snprintf(node_name, sizeof(node_name), "sio-firmware-data@%lx", mapping->phys);

        int mem_node = dt_get_or_add_reserved_mem(node_name, "apple,asc-mem", true, mapping->phys,
                                                  mapping->size);
        if (mem_node < 0)
            return -1;
        uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);

        int ret =
            dt_device_set_reserved_mem(mem_node, node_name, phandle, mapping->iova, mapping->size);
        if (ret < 0)
            return ret;

        ret = dt_device_add_mem_region(fdt_alias, mem_phandle, NULL);
        if (ret < 0)
            return ret;
    }

    node = fdt_path_offset(dt, fdt_alias);
    if (node < 0)
        bail_cleanup("FDT: '%s' not found\n", fdt_alias);

    for (int i = 0; i < siodata->num_fwparams; i++) {
        struct sio_fwparam *param = &siodata->fwparams[i];

        if (fdt_appendprop_u32(dt, node, "apple,sio-firmware-params", param->key))
            bail_cleanup("FDT: couldn't append to SIO parameters\n");

        if (fdt_appendprop_u32(dt, node, "apple,sio-firmware-params", param->value))
            bail_cleanup("FDT: couldn't append to SIO parameters\n");
    }

err:
    free(siodata);
    return 0;
}

static int dt_setup_sio(void)
{
    static const char *sio_names[2] = {"sio", "sio1"};

    for (size_t i = 0; i < ARRAY_SIZE(sio_names); i++) {

        int node = fdt_path_offset(dt, sio_names[i]);
        if (node < 0)
            continue;

        char adt_path[16];
        snprintf(adt_path, sizeof(adt_path), "/arm-io/%s", sio_names[i]);

        if (dt_reserve_asc_firmware(adt_path, NULL, sio_names[i], true, 0))
            continue;
        if (dt_set_sio_fwdata(adt_path, sio_names[i]))
            continue;

        node = fdt_path_offset(dt, sio_names[i]);
        fdt_setprop_string(dt, node, "status", "okay");
    }

    return 0;
}

/* The T8132 AOP DART's DAPF bank has 16 slices; their windows merge to fewer. */
#define T8132_AOP_DAPF_WINDOWS 16

/*
 * The J713 AOP firmware reaches its DAPF windows (MMIO) through its DART at
 * their physical addresses: reserve them there, so that the OS maps them in
 * the AOP's DART before it starts the firmware.
 */
static int dt_reserve_t8132_aop_windows(const char *path)
{
    struct dapf_window windows[T8132_AOP_DAPF_WINDOWS];
    int count = dapf_t8132_windows("/arm-io/dart-aop", 1, windows, ARRAY_SIZE(windows));
    if (count <= 0) {
        printf("FDT: %s: no AOP DAPF windows\n", path);
        return -1;
    }

    int node = fdt_path_offset(dt, path);
    uint32_t dev_phandle = node < 0 ? 0 : fdt_get_phandle(dt, node);
    if (!dev_phandle)
        return -1;

    for (int i = 0; i < count; i++) {
        char node_name[64];
        u64 size = windows[i].end - windows[i].start;

        snprintf(node_name, sizeof(node_name), "asc-mmio@%lx", windows[i].start);
        int mem_node =
            dt_get_or_add_reserved_mem(node_name, "apple,asc-mmio", true, windows[i].start, size);
        if (mem_node < 0)
            return -1;
        uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);
        if (dt_device_set_reserved_mem(mem_node, node_name, dev_phandle, windows[i].start, size) ||
            dt_device_add_mem_region(path, mem_phandle, NULL) < 0)
            return -1;
    }

    printf("FDT: %s: %d AOP DAPF windows reserved\n", path, count);
    return 0;
}

/*
 * J713: iBoot leaves the AOP halted, its DART's DAPF empty, its external code
 * (__ETEXT) in DRAM at a fixed address and its log (__OS_LOG) at a new one
 * every boot. When the OS asks for the AOP's handoff, program the DAPF, then
 * reserve both segments at the IOVAs the firmware uses (their remap
 * addresses: EC0p for __ETEXT, the physical address for __OS_LOG) and the
 * DAPF windows. Without all of it the firmware cannot run, so on any failure
 * the AOP is disabled.
 */
static int dt_set_t8132_aop(const char *path)
{
    const char *why;
    int node;

    if (fdt_node_check_compatible(dt, 0, "apple,j713"))
        return 0; /* No other M4 board's AOP handoff is qualified. */

    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 ||
        fdt_stringlist_search(dt, chosen, "linux-enablement-mac,dapf-handoff", "/arm-io/aop") < 0) {
        why = "its handoff was not requested";
        goto disable;
    }

    asc_dev_t *asc = asc_init("/arm-io/aop");
    if (!asc) {
        why = "its ASC is unavailable";
        goto disable;
    }
    bool running = asc_cpu_running(asc);
    asc_free(asc);
    if (running) {
        why = "its CPU is running";
        goto disable;
    }

    if (dapf_init_t8132_halted("/arm-io/dart-aop", 1)) {
        why = "its DAPF is not programmed";
        goto disable;
    }
    if (dt_reserve_asc_segments("/arm-io/aop/iop-aop-nub", NULL, path, true, 0, true)) {
        why = "its firmware segments are not reserved";
        goto disable;
    }
    if (dt_reserve_t8132_aop_windows(path)) {
        why = "its DAPF windows are not reserved";
        goto disable;
    }

    printf("FDT: %s: AOP handed off with its DAPF and firmware segments\n", path);
    return 0;

disable:
    printf("FDT: %s: disabling the AOP: %s\n", path, why);
    node = fdt_path_offset(dt, path);
    if (node < 0 || fdt_setprop_string(dt, node, "status", "disabled"))
        bail("FDT: %s: cannot disable the AOP\n", path);
    return 0;
}

/*
 * The AOP's accelerometer and gyroscope report only once the OS has sent them
 * the machine's calibration. Copy it from the ADT services into the matching
 * children of the OS AOP node; a missing service or property is skipped.
 */
static void dt_set_aop_motion_cal(const char *path)
{
    static const char *const services[] = {"accel", "gyro"};
    static const char *const props[][2] = {
        {"range-intr-cal", "apple,range-intrinsic-cal"},
        {"nominal-extr-cal", "apple,nominal-extrinsic-cal"},
        {"range-extr-cal", "apple,range-extrinsic-cal"},
    };

    for (size_t i = 0; i < ARRAY_SIZE(services); i++) {
        char adt_path[64], name[32];

        snprintf(adt_path, sizeof(adt_path), "/arm-io/aop/iop-aop-nub/%s", services[i]);
        int anode = adt_path_offset(adt, adt_path);
        int aop = fdt_path_offset(dt, path);
        int node = aop < 0 ? aop : fdt_subnode_offset(dt, aop, services[i]);
        if (anode < 0 || node < 0)
            continue;

        int copied = 0;
        for (size_t j = 0; j < ARRAY_SIZE(props); j++) {
            u32 len = 0;

            snprintf(name, sizeof(name), "%s-%s", services[i], props[j][0]);
            const void *val = adt_getprop(adt, anode, name, &len);
            if (!val || !len)
                continue;
            if (fdt_setprop(dt, node, props[j][1], val, len)) {
                printf("FDT: %s/%s: cannot set %s\n", path, services[i], props[j][1]);
                continue;
            }
            copied++;
        }
        printf("FDT: %s/%s: %d of %d calibration properties\n", path, services[i], copied,
               (int)ARRAY_SIZE(props));
    }
}

/*
 * The T6040/T6041 AOP runs its __TEXT and __DATA from its own SRAM, but iBoot
 * leaves its external code (__ETEXT) and its log (__OS_LOG) in DRAM. Reserve
 * those segments with the IOVAs the firmware uses for them (their remap
 * addresses), so that the OS maps them in the AOP's DART before it starts the
 * firmware.
 */
static int dt_set_aop_firmware(void)
{
    if (chip_id != T6040 && chip_id != T6041 && chip_id != T8132)
        return 0;

    int node = fdt_node_offset_by_compatible(
        dt, -1, chip_id == T8132 ? "apple,t8132-aop" : "apple,t6041-aop");
    if (node < 0)
        return 0;

    const char *status = fdt_getprop(dt, node, "status", NULL);
    if (status && strcmp(status, "okay") && strcmp(status, "ok"))
        return 0;

    char path[64];
    if (fdt_get_path(dt, node, path, sizeof(path))) {
        printf("FDT: AOP node path too long\n");
        return 0;
    }

    dt_set_aop_motion_cal(path);

    if (chip_id == T8132)
        return dt_set_t8132_aop(path);

    u32 len = 0;
    int anode = adt_path_offset(adt, "/arm-io/aop");
    if (anode < 0 || !adt_getprop(adt, anode, "segment-ranges", &len) || !len ||
        len % sizeof(struct adt_segment_ranges)) {
        printf("ADT: /arm-io/aop has no firmware segments, %s left without them\n", path);
        return 0;
    }

    if (dt_reserve_asc_segments("/arm-io/aop", NULL, path, true, 0, true))
        printf("FDT: %s: reserving the AOP firmware segments failed\n", path);
    else
        printf("FDT: %s: AOP firmware segments reserved\n", path);

    return 0;
}

struct isp_segment_ranges {
    u64 phys;
    u64 iova;
    u64 remap;
    u32 size;
    u32 unk;
} PACKED;

static int dt_set_isp_fwdata(void)
{
    const char *fdt_path = "isp";
    int ret = 0;

    u64 phys, iova, size;

    int fdt_node = fdt_path_offset(dt, fdt_path);
    if (fdt_node < 0) {
        printf("FDT: '%s' not found\n", fdt_path);
        return 0;
    }

    if (firmware_set_fdt(dt, fdt_node, "apple,firmware-version", &os_firmware) < 0)
        bail("FDT: Could not set apple,firmware-version for %s\n", fdt_path);

    const struct fw_version_info *compat;

    switch (os_firmware.version) {
        case V13_6_1:
            compat = &fw_versions[V13_5];
            break;
        default:
            compat = &os_firmware;
            break;
    }

    if (firmware_set_fdt(dt, fdt_node, "apple,firmware-compat", compat) < 0)
        bail("FDT: Could not set apple,firmware-compat for %s\n", fdt_path);

    if (isp_get_heap(&phys, &iova, &size)) {
        const char *status = fdt_getprop(dt, fdt_node, "status", NULL);

        if (!status || strcmp(status, "disabled")) {
            printf("FDT: ISP enabled but not initialized, disabling\n");
            if (fdt_setprop_string(dt, fdt_node, "status", "disabled") < 0)
                bail("FDT: failed to set status property of ISP\n");
        }

        return 0;
    }

    int adt_node = adt_path_offset(adt, "/arm-io/isp");
    if (adt_node < 0)
        adt_node = adt_path_offset(adt, "/arm-io/isp0");
    if (adt_node < 0)
        return 0;

    uint32_t dev_phandle = fdt_get_phandle(dt, fdt_node);
    if (!dev_phandle) {
        ret = fdt_generate_phandle(dt, &dev_phandle);
        if (!ret)
            ret = fdt_setprop_u32(dt, fdt_node, "phandle", dev_phandle);
        if (ret != 0)
            bail("FDT: couldn't set '%s.phandle' property: %d\n", fdt_path, ret);
    }

    int mem_node = dt_get_or_add_reserved_mem("isp-heap", "apple,asc-mem", true, phys, size);
    if (mem_node < 0)
        return -1;

    ret = dt_device_set_reserved_mem(mem_node, "isp-heap", dev_phandle, iova, size);
    if (ret < 0)
        return ret;

    return 0;
}

/*
 * The camera is optional. Without a valid ADT firmware description, or if its firmware or heap
 * reservation fails, disable the ISP and boot on without it instead of failing the whole handoff.
 */
static int dt_set_isp(void)
{
    u64 heap_phys, heap_iova, heap_size;

    /* A T8140 ISP that isp_init() did not bring up has no firmware to reserve. */
    if (chip_id == T8140 && isp_get_heap(&heap_phys, &heap_iova, &heap_size)) {
        if (!dt_set_isp_fwdata())
            return 0;
    } else if (!dt_reserve_asc_firmware("/arm-io/isp", "/arm-io/isp0", "isp", false,
                                        isp_iova_base()) &&
               !dt_set_isp_fwdata()) {
        return 0;
    }

    int node = fdt_path_offset(dt, "isp");
    if (node < 0)
        return -1;
    printf("FDT: ISP firmware or heap not reserved, disabling the ISP\n");
    if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
        return -1;
    return 0;
}

/* J700 is sold both with and without a built-in Touch ID sensor.  A paired
 * external keyboard is not evidence of the SPI sensor: iBoot omits spi2 from
 * the ADT on the sensorless variant.  Leave every other machine unchanged. */
static int dt_disable_missing_j700_touch_id(void)
{
    if (fdt_node_check_compatible(dt, 0, "apple,j700") != 0)
        return 0;
    if (adt_path_offset(adt, "/arm-io") < 0)
        return -1;
    if (adt_path_offset(adt, "/arm-io/spi2") >= 0)
        return 0;

    int sensor = fdt_node_offset_by_compatible(dt, -1, "apple,mesa-fingerprint");
    if (sensor == -FDT_ERR_NOTFOUND)
        return 0;
    if (sensor < 0)
        return -1;
    int controller = fdt_parent_offset(dt, sensor);
    if (controller < 0 || fdt_node_check_compatible(dt, controller, "apple,t8140-spi") != 0)
        return -1;

    /* The parent gate prevents probing either the absent controller or child. */
    if (fdt_setprop_string(dt, controller, "status", "disabled") < 0)
        return -1;
    printf("FDT: disabled J700 Touch ID controller absent from ADT\n");
    return 0;
}

static int dt_disable_missing_devs(const char *adt_prefix, const char *dt_prefix, int max_devs,
                                   int regnum)
{
    int ret = -1;
    int adt_prefix_len = strlen(adt_prefix);
    int dt_prefix_len = strlen(dt_prefix);

    int acnt = 0, phcnt = 0;
    u64 *addrs = calloc(max_devs, sizeof(u64));
    u32 *phandles = calloc(max_devs * 5, sizeof(u32)); // Allow up to 5 extra nodes per device
    if (!addrs || !phandles)
        bail_cleanup("FDT: out of memory\n");

    int path[8];
    int node = adt_path_offset_trace(adt, "/arm-io", path);
    if (node < 0)
        bail_cleanup("ADT: /arm-io not found\n");

    int pp = 0;
    while (path[pp])
        pp++;
    path[pp + 1] = 0;

    u32 die_count;
    if (ADT_GETPROP(adt, node, "die-count", &die_count) < 0) {
        die_count = 1;
    }
    if (die_count > 8) {
        printf("ADT: limiting die-count from %u to 8\n", die_count);
        die_count = 8;
    }

    /* Find ADT registers */
    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        if (strncmp(name, adt_prefix, adt_prefix_len))
            continue;

        if (name[adt_prefix_len] && (name[adt_prefix_len] < '0' || name[adt_prefix_len] > '9'))
            continue;

        if (acnt >= max_devs)
            bail_cleanup("ADT: too many /arm-io/%s devices\n", adt_prefix);

        path[pp] = node;
        if (adt_get_reg(adt, path, "reg", regnum, &addrs[acnt++], NULL) < 0)
            bail_cleanup("Error getting /arm-io/%s regs\n", name);
    }

    for (u32 die = 0; die < die_count; ++die) {
        char path[32] = "/soc";

        if (die_count > 1) {
            // pre-linux submission multi-die path
            // can probably removed the next time someone read this comment.
            snprintf(path, sizeof(path), "/soc/die%u", die);
            int die_node = fdt_path_offset(dt, path);
            if (die_node < 0) {
                /* this should use aliases for the soc nodes */
                u64 die_unit_addr = die * PMGR_DIE_OFFSET + 0x200000000;
                snprintf(path, sizeof(path), "/soc@%lx", die_unit_addr);
            }
        }

        int soc = fdt_path_offset(dt, path);
        if (soc < 0 && die_count == 1) {
            /* Newer trees (e.g. T6040) place their devices at the root. */
            soc = fdt_path_offset(dt, "/");
            snprintf(path, sizeof(path), "/");
        }
        if (soc < 0)
            bail("FDT: %s node not found in devtree\n", path);

        // parse ranges for address translation
        struct dt_ranges_tbl ranges[DT_MAX_RANGES] = {0};
        dt_parse_ranges(dt, soc, ranges);

        /* Disable primary devices */
        fdt_for_each_subnode(node, dt, soc)
        {
            const char *name = fdt_get_name(dt, node, NULL);
            if (strncmp(name, dt_prefix, dt_prefix_len))
                continue;

            const fdt64_t *reg = fdt_getprop(dt, node, "reg", NULL);
            if (!reg)
                bail_cleanup("FDT: failed to get reg property of %s\n", name);

            u64 addr = dt_translate(ranges, reg);

            int i;
            for (i = 0; i < acnt; i++)
                if (addrs[i] == addr)
                    break;
            if (i < acnt)
                continue;

            int iommus_size;
            const fdt32_t *iommus = fdt_getprop(dt, node, "iommus", &iommus_size);
            if (iommus) {
                if (iommus_size & 7 || iommus_size > 4 * 8) {
                    printf("FDT: bad iommus property for %s/%s\n", path, name);
                } else {
                    for (int i = 0; i < iommus_size / 8; i++)
                        phandles[phcnt++] = fdt32_ld(&iommus[i * 2]);
                }
            }

            int phys_size;
            const fdt32_t *phys = fdt_getprop(dt, node, "phys", &phys_size);
            if (phys) {
                if (phys_size & 7 || phys_size > 4 * 8) {
                    printf("FDT: bad phys property for %s/%s\n", path, name);
                } else {
                    for (int i = 0; i < phys_size / 8; i++)
                        phandles[phcnt++] = fdt32_ld(&phys[i * 2]);
                }
            }

            const char *status = fdt_getprop(dt, node, "status", NULL);
            if (!status || strcmp(status, "disabled")) {
                printf("FDT: Disabling missing device %s/%s\n", path, name);

                if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
                    bail_cleanup("FDT: failed to set status property of %s/%s\n", path, name);
            }

            // disable child devices
            int child;
            fdt_for_each_subnode(child, dt, node)
            {
                if (fdt_setprop_string(dt, child, "status", "disabled") < 0) {
                    const char *child_name = fdt_get_name(dt, child, NULL);
                    bail_cleanup("FDT: failed to set status property of %s/%s/%s\n", path, name,
                                 child_name);
                }
            }
        }

        /* Disable secondary devices */
        fdt_for_each_subnode(node, dt, soc)
        {
            const char *name = fdt_get_name(dt, node, NULL);
            u32 phandle = fdt_get_phandle(dt, node);
            if (!phandle)
                continue;

            for (int i = 0; i < phcnt; i++) {
                if (phandles[i] != phandle)
                    continue;

                phandles[i] = 0;

                const char *status = fdt_getprop(dt, node, "status", NULL);
                if (status && !strcmp(status, "disabled"))
                    continue;

                printf("FDT: Disabling secondary device %s/%s (%d)\n", path, name, phandle);

                if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
                    bail_cleanup("FDT: failed to set status property of %s/%s\n", path, name);
            }
        }
    }

    for (int i = 0; i < phcnt; i++) {
        if (!phandles[i])
            continue;

        int node = fdt_node_offset_by_phandle(dt, phandles[i]);
        if (node < 0)
            bail_cleanup("FDT: failed to find secondary phandle %d\n", phandles[i]);

        const char *name = fdt_get_name(dt, node, NULL);

        // Should be usb-pd/connector/ports/port/endpoint
        if (!strcmp(name, "endpoint")) {
            for (int i = 0; i < 4; i++)
                if (node >= 0)
                    node = fdt_parent_offset(dt, node);
            if (node < 0) {
                printf("FDT: failed to walk up from endpoint phandle %d\n", phandles[i]);
                continue;
            }
            name = fdt_get_name(dt, node, NULL);
            if (strncmp(name, "usb-pd", 6)) {
                printf("FDT: unexpected secondary device %s walking up from endpoint\n", name);
                continue;
            }
        } else {
            printf("FDT: unexpected secondary device %s (%d)\n", name, phandles[i]);
            continue;
        }

        const char *status = fdt_getprop(dt, node, "status", NULL);
        if (status && !strcmp(status, "disabled"))
            continue;

        printf("FDT: Disabling secondary device %s\n", name);

        if (fdt_setprop_string(dt, node, "status", "disabled") < 0)
            bail_cleanup("FDT: failed to set status property of %s\n", name);
        break;
    }

    ret = 0;
err:
    free(phandles);
    free(addrs);

    return ret;
}

__attribute__((unused)) static int dt_transfer_virtios(void)
{
    int path[3];
    path[0] = adt_path_offset(adt, "/arm-io/");
    if (path[0] < 0)
        bail("ADT: /arm-io not found\n");

    int aic = fdt_node_offset_by_compatible(dt, -1, "apple,aic");
    if (aic == -FDT_ERR_NOTFOUND)
        aic = fdt_node_offset_by_compatible(dt, -1, "apple,aic2");
    if (aic == -FDT_ERR_NOTFOUND)
        aic = fdt_node_offset_by_compatible(dt, -1, "apple,t8122-aic3");
    if (aic < 0)
        bail("FDT: failed to find AIC node\n");

    u32 aic_phandle = fdt_get_phandle(dt, aic);
    const fdt32_t *ic_prop = fdt_getprop(dt, aic, "#interrupt-cells", NULL);
    u32 intcells = 0;
    if (ic_prop)
        intcells = fdt32_ld(ic_prop);
    if (intcells < 3 || intcells > 4)
        bail("FDT: bad '#interrupt-cells' on AIC node (%d)\n", intcells);

    for (u32 i = 0; i < 16; i++) {
        char name[16], fullname[32];
        snprintf(name, sizeof(name) - 1, "virtio%d", i);

        path[1] = adt_subnode_offset(adt, path[0], name);
        if (path[1] < 0)
            break;
        path[2] = 0;

        u64 addr, size;
        if (adt_get_reg(adt, path, "reg", 0, &addr, &size) < 0)
            bail("ADT: error getting /arm-io/%s regs\n", name);

        u32 irq;
        ADT_GETPROP(adt, path[1], "interrupts", &irq);

        snprintf(fullname, sizeof(fullname) - 1, "virtio@%lx", addr);
        printf("FDT: Adding %s found in ADT\n", name);

        int fnode = fdt_add_subnode(dt, 0, fullname);
        if (fnode < 0)
            bail("FDT: failed to create %s\n", fullname);

        if (fdt_setprop_string(dt, fnode, "compatible", "virtio,mmio"))
            bail("FDT: couldn't set %s.compatible\n", fullname);

        fdt64_t reg[2];
        fdt64_st(reg + 0, addr);
        fdt64_st(reg + 1, size);
        if (fdt_setprop(dt, fnode, "reg", reg, sizeof(reg)))
            bail("FDT: couldn't set %s.reg\n", fullname);

        if (fdt_setprop_u32(dt, fnode, "interrupt-parent", aic_phandle))
            bail("FDT: couldn't set %s.interrupt-parent\n", fullname);

        fdt32_t intprop[4];
        fdt32_st(intprop + 0, 0); // AIC_IRQ
        fdt32_st(intprop + 1, 0);
        fdt32_st(intprop + intcells - 2, irq);
        fdt32_st(intprop + intcells - 1, 4); // IRQ_TYPE_LEVEL_HIGH
        if (fdt_setprop(dt, fnode, "interrupts", intprop, 4 * intcells))
            bail("FDT: couldn't set %s.interrupts\n", fullname);
    }

    return 0;
}

/*
 * Remove unneeded apple,always-on from unused memory channels on T8012.
 * This is needed because the amount of memory and the amount of used
 * memory channels depends on the storage configuration of the host Mac
 * and can only be determined at runtime.
 */
static int dt_set_pmgr(void)
{
    if (chip_id != T8012)
        return 0;

    if (mem_size_actual > 0x40000000)
        return 0;

    int pmgr_node = fdt_path_offset(dt, "/soc/power-management@20e000000");
    if (pmgr_node < 0) {
        printf("FDT: Failed to find pmgr node\n");
        return 0;
    }

    int dcs2_node = fdt_path_offset(dt, "/soc/power-management@20e000000/power-controller@80258");
    if (dcs2_node < 0)
        bail("FDT: failed to find ps_dcs2 node\n");

    /* Allow failure */
    fdt_delprop(dt, dcs2_node, "apple,always-on");

    int dcs3_node = fdt_path_offset(dt, "/soc/power-management@20e000000/power-controller@80260");
    if (dcs3_node < 0)
        bail("FDT: failed to find ps_dcs3 node\n");

    /* Allow failure */
    fdt_delprop(dt, dcs3_node, "apple,always-on");

    return 0;
}

static int dt_set_adt_regs(const char *alias, const u64 *addrs, const u64 *sizes, size_t count)
{
    const char *path = fdt_get_alias(dt, alias);
    if (!path)
        return -FDT_ERR_NOTFOUND;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return node;

    fdt64_t regs[6];
    if (count > ARRAY_SIZE(regs) / 2)
        return -FDT_ERR_BADVALUE;

    for (size_t i = 0; i < count; ++i) {
        fdt64_st(&regs[2 * i], addrs[i]);
        fdt64_st(&regs[2 * i + 1], sizes[i]);
    }

    return fdt_setprop(dt, node, "reg", regs, count * 2 * sizeof(*regs));
}

static int dt_set_adt_irqs(const char *alias, const u32 *irqs, size_t count)
{
    const char *path = fdt_get_alias(dt, alias);
    if (!path)
        return -FDT_ERR_NOTFOUND;

    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return node;

    fdt32_t cells[12];
    if (count > ARRAY_SIZE(cells) / 3)
        return -FDT_ERR_BADVALUE;

    for (size_t i = 0; i < count; ++i) {
        fdt32_st(&cells[3 * i], 0); /* AIC_IRQ */
        fdt32_st(&cells[3 * i + 1], irqs[i]);
        fdt32_st(&cells[3 * i + 2], 4); /* IRQ_TYPE_LEVEL_HIGH */
    }

    return fdt_setprop(dt, node, "interrupts", cells, count * 3 * sizeof(*cells));
}

/*
 * M4 and T8140 expose a separate NVMMU window. Its physical layout is supplied by
 * iBoot's live ADT, so transfer it instead of encoding machine-specific
 * addresses in the Linux devicetree.
 */
static int dt_set_ans_nvme(bool polled)
{
    if (!fdt_get_alias(dt, "ans"))
        return 0;

    int ans_path[8];
    int ans = adt_path_offset_trace(adt, "/arm-io/ans", ans_path);
    if (ans < 0)
        bail("ADT: /arm-io/ans not found\n");
    if (!adt_get_property(adt, ans, "nvme-secure-bar"))
        bail("ADT: ANS is missing the separate NVMe register layout\n");

    u64 ans_addr, ans_size, nvmmu_addr, nvmmu_size, nvme_addr, nvme_size;
    if (adt_get_reg(adt, ans_path, "reg", 0, &ans_addr, &ans_size) < 0 ||
        adt_get_reg(adt, ans_path, "reg", 3, &nvmmu_addr, &nvmmu_size) < 0 ||
        adt_get_reg(adt, ans_path, "reg", 9, &nvme_addr, &nvme_size) < 0)
        bail("ADT: failed to resolve ANS register ranges\n");

    /*
     * T8132/T8140's secure NVMe BAR is described as 64 KiB in the ADT, but its
     * linear submission registers extend through offset 0x24910.  Linux
     * needs that complete aperture while the child ANS control resource must
     * stop before the independently-owned mailbox at ANS + 0x8000.
     */
    if (ans_size < 0x4000)
        bail("ADT: ANS control aperture is too small\n");

    const u64 nvme_addrs[] = {nvme_addr, nvmmu_addr, ans_addr};
    const u64 nvme_sizes[] = {nvme_size < 0x40000 ? 0x40000 : nvme_size, nvmmu_size, 0x4000};
    int ret = dt_set_adt_regs("ans", nvme_addrs, nvme_sizes, ARRAY_SIZE(nvme_addrs));
    if (ret < 0)
        bail("FDT: failed to set ANS register ranges: %d\n", ret);

    const u64 mbox_addrs[] = {ans_addr + 0x8000};
    const u64 mbox_sizes[] = {0x4000};
    ret = dt_set_adt_regs("ans-mbox", mbox_addrs, mbox_sizes, 1);
    if (ret < 0)
        bail("FDT: failed to set ANS mailbox range: %d\n", ret);

    int sart_path[8];
    if (adt_path_offset_trace(adt, "/arm-io/sart-ans", sart_path) < 0)
        bail("ADT: /arm-io/sart-ans not found\n");
    u64 sart_addr, sart_size;
    if (adt_get_reg(adt, sart_path, "reg", 0, &sart_addr, &sart_size) < 0)
        bail("ADT: failed to resolve ANS SART range\n");
    ret = dt_set_adt_regs("sart-ans", &sart_addr, &sart_size, 1);
    if (ret < 0)
        bail("FDT: failed to set ANS SART range: %d\n", ret);

    /* The minimal J700 U-Boot console polls; IRQ semantics are not yet validated. */
    if (polled) {
        printf("FDT: Transferred polled ANS/NVMMU/SART resources from ADT\n");
    } else {
        u32 irq_len;
        const u32 *irqs = adt_getprop(adt, ans, "interrupts", &irq_len);
        if (!irqs || irq_len < 5 * sizeof(*irqs) || irq_len % sizeof(*irqs))
            bail("ADT: invalid ANS interrupt list\n");

        /*
         * The ANS ADT lists each FIFO pair as not-empty, empty. Linux's ASC
         * mailbox binding uses empty, not-empty for both directions. Copying
         * positionally enables the permanently asserted receive-empty IRQ
         * as receive-not-empty and locks the CPU in an interrupt storm.
         */
        const u32 mbox_irqs[] = {irqs[1], irqs[0], irqs[3], irqs[2]};
        ret = dt_set_adt_irqs("ans-mbox", mbox_irqs, ARRAY_SIZE(mbox_irqs));
        if (ret < 0)
            bail("FDT: failed to set ANS mailbox interrupts: %d\n", ret);

        /* The controller interrupt follows the four ASC mailbox interrupts. */
        ret = dt_set_adt_irqs("ans", &irqs[4], 1);
        if (ret < 0)
            bail("FDT: failed to set NVMe interrupt: %d\n", ret);
        printf("FDT: Transferred T8132 ANS/NVMMU/SART resources from ADT\n");
    }

    /* Common DT placeholders stay disabled until all resources are valid.
     * Resolve aliases again after each mutation: offsets and property pointers
     * into the FDT are not stable while the blob grows. Enable ANS last. */
    const char *aliases[] = {"sart-ans", "ans-mbox", "ans"};
    for (unsigned i = 0; i < ARRAY_SIZE(aliases); i++) {
        int node = fdt_path_offset(dt, aliases[i]);
        if (node < 0 || fdt_setprop_string(dt, node, "status", "okay"))
            bail("FDT: failed to enable %s\n", aliases[i]);
    }
    return 0;
}

/*
 * The primary PMU moves between Apple SoCs and firmware revisions.  Locate
 * the primary SPMI child in the live ADT and transfer only the controller
 * range and slave ID required by Linux's fixed Abbey NVMEM description.
 */
static int dt_set_t8132_lifecycle(void)
{
    if (!fdt_get_alias(dt, "spmi-primary"))
        return 0;

    int arm_io = adt_path_offset(adt, "/arm-io");
    if (arm_io < 0)
        bail("ADT: /arm-io not found\n");

    int spmi = -1;
    int pmu = -1;
    int bus = arm_io;
    ADT_FOREACH_CHILD(adt, bus)
    {
        const char *name = adt_get_name(adt, bus);
        if (strncmp(name, "nub-spmi", 8) && strncmp(name, "spmi", 4))
            continue;

        int dev = bus;
        ADT_FOREACH_CHILD(adt, dev)
        {
            if (!adt_is_compatible(adt, dev, "pmu,spmi") &&
                !adt_is_compatible(adt, dev, "pmu,d2422") &&
                !adt_is_compatible(adt, dev, "pmu,d2449"))
                continue;

            u32 primary;
            if (ADT_GETPROP(adt, dev, "is-primary", &primary) != sizeof(primary) || primary != 1)
                continue;

            spmi = bus;
            pmu = dev;
            break;
        }

        if (pmu >= 0)
            break;
    }

    if (spmi < 0 || pmu < 0)
        bail("ADT: primary SPMI PMU not found\n");

    char spmi_path[64];
    int ret = snprintf(spmi_path, sizeof(spmi_path), "/arm-io/%s", adt_get_name(adt, spmi));
    if (ret < 0 || (size_t)ret >= sizeof(spmi_path))
        bail("ADT: primary SPMI path is too long\n");

    int path[8];
    if (adt_path_offset_trace(adt, spmi_path, path) < 0)
        bail("ADT: failed to resolve primary SPMI path\n");

    u64 addr, size;
    if (adt_get_reg(adt, path, "reg", 0, &addr, &size) < 0)
        bail("ADT: failed to resolve primary SPMI register range\n");

    ret = dt_set_adt_regs("spmi-primary", &addr, &size, 1);
    if (ret < 0)
        bail("FDT: failed to set primary SPMI register range: %d\n", ret);

    u32 reg_len;
    const u32 *pmu_reg = adt_getprop(adt, pmu, "reg", &reg_len);
    if (!pmu_reg || reg_len < sizeof(*pmu_reg) || pmu_reg[0] > 0xf)
        bail("ADT: primary SPMI PMU has an invalid slave ID\n");

    const char *pmu_path = fdt_get_alias(dt, "pmu-primary");
    if (!pmu_path)
        bail("FDT: primary PMU alias not found\n");
    int pmu_node = fdt_path_offset(dt, pmu_path);
    if (pmu_node < 0)
        bail("FDT: primary PMU node not found\n");

    fdt32_t pmu_fdt_reg[2];
    fdt32_st(&pmu_fdt_reg[0], pmu_reg[0]);
    fdt32_st(&pmu_fdt_reg[1], 0); /* SPMI_USID */
    ret = fdt_setprop(dt, pmu_node, "reg", pmu_fdt_reg, sizeof(pmu_fdt_reg));
    if (ret < 0)
        bail("FDT: failed to set primary PMU slave ID: %d\n", ret);

    printf("FDT: Transferred primary T8132 SPMI/PMU resources from ADT\n");
    return 0;
}

void kboot_set_initrd(void *start, size_t size)
{
    initrd_start = start;
    initrd_size = size;
}

int kboot_set_chosen(const char *name, const char *value)
{
    int i = 0;
    char *copy = NULL;

    if (!name) {
        chosen_lost = true;
        return -1;
    }
    /* The new value first: a failure leaves an existing entry (an owner's =0) as it was */
    if (value) {
        copy = calloc(strlen(value) + 1, 1);
        if (!copy) {
            chosen_lost = true;
            return -1;
        }
        strcpy(copy, value);
    }

    for (i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0]) {
            chosen_params[i][0] = calloc(strlen(name) + 1, 1);
            if (!chosen_params[i][0]) {
                free(copy);
                chosen_lost = true;
                return -1;
            }
            strcpy(chosen_params[i][0], name);
            break;
        }

        if (!strcmp(name, chosen_params[i][0]))
            break;
    }

    if (i >= MAX_CHOSEN_PARAMS) {
        free(copy);
        chosen_lost = true;
        return -1;
    }

    free(chosen_params[i][1]);
    chosen_params[i][1] = copy;
    chosen_is_default[i] = false;
    return i;
}

void kboot_set_config_lost(void)
{
    chosen_lost = true;
}

void kboot_delete_chosen(const char *name)
{
    if (!name)
        return;

    for (int i = 0; i < MAX_CHOSEN_PARAMS && chosen_params[i][0]; i++) {
        if (strcmp(name, chosen_params[i][0]))
            continue;
        free(chosen_params[i][0]);
        free(chosen_params[i][1]);
        memmove(&chosen_params[i], &chosen_params[i + 1],
                (MAX_CHOSEN_PARAMS - i - 1) * sizeof(chosen_params[0]));
        memset(&chosen_params[MAX_CHOSEN_PARAMS - 1], 0, sizeof(chosen_params[0]));
        memmove(&chosen_is_default[i], &chosen_is_default[i + 1],
                (MAX_CHOSEN_PARAMS - i - 1) * sizeof(chosen_is_default[0]));
        chosen_is_default[MAX_CHOSEN_PARAMS - 1] = false;
        return;
    }
}

int kboot_set_uboot(const char *name, const char *value)
{
    int i = 0;

    if (!name)
        return -1;

    for (i = 0; i < MAX_UBOOT_CONFIGS; i++) {
        if (!uboot_config[i][0]) {
            uboot_config[i][0] = calloc(strlen(name) + 1, 1);
            strcpy(uboot_config[i][0], name);
            break;
        }

        if (!strcmp(name, uboot_config[i][0])) {
            free(uboot_config[i][1]);
            uboot_config[i][1] = NULL;
            break;
        }
    }

    if (i >= MAX_UBOOT_CONFIGS)
        return -1;

    if (value) {
        uboot_config[i][1] = calloc(strlen(value) + 1, 1);
        strcpy(uboot_config[i][1], value);
    }

    return i;
}

#define LOGBUF_SIZE SZ_16K

struct {
    void *buffer;
    size_t wp;
} logbuf;

static bool log_console_iodev_can_write(void *opaque)
{
    UNUSED(opaque);
    return !!logbuf.buffer;
}

static ssize_t log_console_iodev_write(void *opaque, const void *buf, size_t len)
{
    UNUSED(opaque);

    if (!logbuf.buffer)
        return 0;

    ssize_t wrote = 0;
    size_t remain = LOGBUF_SIZE - logbuf.wp;
    while (remain < len) {
        memcpy(logbuf.buffer + logbuf.wp, buf, remain);
        logbuf.wp = 0;
        wrote += remain;
        buf += remain;
        len -= remain;
        remain = LOGBUF_SIZE;
    }
    memcpy(logbuf.buffer + logbuf.wp, buf, len);
    wrote += len;
    logbuf.wp = (logbuf.wp + len) % LOGBUF_SIZE;

    return wrote;
}

const struct iodev_ops iodev_log_ops = {
    .can_write = log_console_iodev_can_write,
    .write = log_console_iodev_write,
};

struct iodev iodev_log = {
    .ops = &iodev_log_ops,
    .usage = USAGE_CONSOLE,
    .lock = SPINLOCK_INIT,
};

/* "0x..." to a number; 0 for anything else. */
static u64 parse_hex_u64(const char *s)
{
    u64 v = 0;
    if (!s || s[0] != '0' || (s[1] != 'x' && s[1] != 'X') || !s[2])
        return 0;
    for (s += 2; *s; s++) {
        int d = (*s >= '0' && *s <= '9')   ? *s - '0'
                : (*s >= 'a' && *s <= 'f') ? *s - 'a' + 10
                : (*s >= 'A' && *s <= 'F') ? *s - 'A' + 10
                                           : -1;
        if (d < 0 || v >> 60)
            return 0;
        v = (v << 4) | d;
    }
    return v;
}

static int dt_setup_mtd_phram(void)
{
    char node_name[64];
    snprintf(node_name, sizeof(node_name), "flash@%lx", (u64)adt);

    int node = dt_get_or_add_reserved_mem(node_name, "phram", false, (u64)adt,
                                          ALIGN_UP(cur_boot_args.devtree_size, SZ_16K));

    if (node > 0) {
        int ret = fdt_setprop_string(dt, node, "label", "adt");
        if (ret)
            bail("FDT: failed to setup ADT MTD phram label\n");
    }

    // init memory backed iodev for console log
    logbuf.buffer = (void *)top_of_memory_alloc(LOGBUF_SIZE);
    if (!logbuf.buffer)
        bail("FDT: failed to allocate aura log buffer\n");

    snprintf(node_name, sizeof(node_name), "flash@%lx", (u64)logbuf.buffer);
    node = dt_get_or_add_reserved_mem(node_name, "phram", false, (u64)logbuf.buffer, SZ_16K);

    if (node > 0) {
        int ret = fdt_setprop_string(dt, node, "label", "aura_stage2.log");
        if (ret)
            bail("FDT: failed to setup aura log MTD phram label\n");
    }

    /* Stage 1 left its console in a page it took off the top of RAM. */
    for (int i = 0; i < MAX_CHOSEN_PARAMS && chosen_params[i][0]; i++) {
        if (strcmp(chosen_params[i][0], "aurora,stage1-log") || !chosen_params[i][1])
            continue;
        u64 pa = parse_hex_u64(chosen_params[i][1]);
        if (!pa || pa & (SZ_16K - 1))
            break;
        snprintf(node_name, sizeof(node_name), "flash@%lx", pa);
        node = dt_get_or_add_reserved_mem(node_name, "phram", false, pa, SZ_16K);
        if (node > 0 && fdt_setprop_string(dt, node, "label", "aura_stage1.log"))
            bail("FDT: failed to setup aura stage 1 log label\n");
        break;
    }

    return 0;
}

/*
 * The second display (J873g HDMI on DCPEXT0 next to USB-C) becomes a second
 * simple-framebuffer at the root: Linux creates a device only for the first
 * simple-framebuffer child of /chosen.
 */
static int dt_set_fb2_t8152(void)
{
    const struct boot_video *v = &display_second_video;

    if (!v->base)
        return 0;
    if ((v->depth & 0xff) != 32) {
        printf("FDT: second framebuffer depth %lu unsupported, not adding it\n", v->depth);
        return 0;
    }

    char name[32];
    snprintf(name, sizeof(name), "framebuffer@%lx", v->base);
    int node = fdt_add_subnode(dt, 0, name);
    if (node < 0)
        bail("FDT: couldn't add %s\n", name);

    fdt64_t reg[2] = {cpu_to_fdt64(v->base), cpu_to_fdt64(v->stride * v->height)};
    if (fdt_setprop_string(dt, node, "compatible", "simple-framebuffer") ||
        fdt_setprop(dt, node, "reg", reg, sizeof(reg)) ||
        fdt_setprop_u32(dt, node, "width", v->width) ||
        fdt_setprop_u32(dt, node, "height", v->height) ||
        fdt_setprop_u32(dt, node, "stride", v->stride) ||
        fdt_setprop_string(dt, node, "format", "x8r8g8b8"))
        bail("FDT: couldn't describe %s\n", name);

    printf("FDT: %s %lux%lu\n", name, v->width, v->height);
    return 0;
}

/*
 * The J873g (T8152) device trees come with their own loader contract (Linux
 * tools/aurora/j873/loader): the machine is brought up in j873g_run_proxy(),
 * and beyond /chosen only the CPUs, MAC addresses, the second framebuffer and
 * the memory map are filled in here.
 */
static int dt_prepare_t8152(void)
{
    if (dt_set_cpus())
        return -1;
    if (dt_set_mac_addresses())
        return -1;
    if (dt_set_fb2_t8152())
        return -1;
    if (dt_set_memory())
        return -1;
    if (fdt_pack(dt))
        bail("FDT: fdt_pack() failed\n");

    printf("FDT prepared at %p\n", dt);
    return 0;
}

/* T8152 has no clock, USB, PCIe or DAPF setup here: j873g_run_proxy() did
 * what the device tree expects, and Linux owns the rest. */
static int kboot_boot_t8152(void *kernel)
{
    if (smp_has_pending_calls()) {
        next_stage.entry = NULL;
        bail("kboot: refusing handoff while secondary callbacks are pending\n");
    }
    printf("Setting SMP mode to WFE...\n");
    smp_set_wfe_mode(true);
    printf("Preparing to boot kernel at %p with fdt at %p\n", kernel, dt);
    next_stage.entry = kernel;
    next_stage.args[0] = (u64)dt;
    next_stage.args[1] = 0;
    next_stage.args[2] = 0;
    next_stage.args[3] = 0;
    next_stage.args[4] = 0;
    next_stage.restore_logo = false;
    return 0;
}

int kboot_prepare_dt(void *fdt)
{
    if (smp_has_pending_calls()) {
        next_stage.entry = NULL;
        bail("FDT: refusing preparation while secondary callbacks are pending\n");
    }
    if (dt) {
        free(dt);
        dt = NULL;
    }

    /* Need to init ISP early to carve out heap */
    isp_init();

    dt_bufsize = fdt_totalsize(fdt);
    assert(dt_bufsize);

    dt_bufsize += 6 * SZ_16K; // Add 96K of buffer for modifications
    dt_bufsize += SZ_16K;     // and the SMBIOS description U-Boot builds its tables from (about 6K)
    // A T6030 or T8122 also takes its PMP values
    if (chip_id == T6030 || chip_id == T8122)
        dt_bufsize += 2 * SZ_16K;
    // A T8122 also takes its GPU and PMP facts and the display clock description
    if (chip_id == T8122)
        dt_bufsize += 8 * SZ_16K;
    // A T6030 also takes its display ADT facts (at most 46K, headers and strings included)
    if (chip_id == T6030)
        dt_bufsize += 4 * SZ_16K;
    // A T6031 takes its PMP values (32K) and its display, GPU and firmware handoff (about 16K)
    if (chip_id == T6031)
        dt_bufsize += 4 * SZ_16K;
    // A T6032 takes only its firmware image UUID list (about 16K)
    if (chip_id == T6032)
        dt_bufsize += SZ_16K;
    dt = memalign(DT_ALIGN, dt_bufsize);

    if (fdt_open_into(fdt, dt, dt_bufsize) < 0)
        bail("FDT: fdt_open_into() failed\n");

    if (fdt_add_mem_rsv(dt, (u64)dt, dt_bufsize))
        bail("FDT: couldn't add reservation for the devtree\n");

    if (dart_reserve_legacy_tables(dt, cur_boot_args.phys_base, cur_boot_args.mem_size))
        bail("FDT: couldn't reserve inherited display translation tables\n");

    u64 image_end = (u64)_end;
    if (display_dart_tables_inherited()) {
        /* chainload_image carries the previous stage's live heap in this
         * high-water mark. Tables that stage published into a locked display
         * DART's inherited roots have no local record here, so keep the
         * inherited prefix as well as our own image. This excludes
         * allocations made by the current stage. A mark below our own base
         * covers nothing of this image, which then keeps only itself. */
        u64 inherited = cur_boot_args.top_of_kernel_data;
        if (!cur_boot_args.mem_size ||
            cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
            bail("FDT: invalid inherited RAM extent\n");
        u64 ram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
        if ((u64)_base < cur_boot_args.phys_base || image_end > ram_end || inherited > ram_end)
            bail("FDT: invalid inherited memory extent\n");
        if (inherited >= (u64)_base)
            image_end = max(image_end, inherited);
    }
    /*
     * Both post-M4 storage paths retain firmware buffers in this stage's heap. T6041 hands the
     * live ANS over the same way (nvme.adopt=live-rtkit-v1): its RTKit buffers must stay out of
     * Linux's allocator (J616C: ANS syslog lines in page-cache pages at the SART-allowed heap
     * addresses).
     */
    if (chip_id == T8140 || chip_id == T6041 || chip_id == T8152 ||
        (chip_id == T8132 && !fdt_node_check_compatible(dt, 0, "apple,j713"))) {
        u64 ram_end, inherited = cur_boot_args.top_of_kernel_data;
        u64 high_water = heapblock_high_water();

        if (!cur_boot_args.mem_size ||
            cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
            bail("FDT: invalid inherited RAM extent\n");
        ram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
        image_end = max(image_end, inherited);
        if ((u64)_base < cur_boot_args.phys_base || image_end < (u64)_base || image_end > ram_end ||
            high_water > ram_end)
            bail("FDT: invalid retained memory extent\n");

        u64 heap_top = max(inherited, high_water);
        if (heap_top > (u64)_end) {
            if (heap_top > UINT64_MAX - (SZ_16K - 1))
                bail("FDT: retained heap extent overflows\n");
            u64 heap_start = ALIGN_DOWN((u64)_end, SZ_16K);
            u64 heap_end = ALIGN_UP(heap_top, SZ_16K);
            if (heap_end > ram_end || fdt_add_mem_rsv(dt, heap_start, heap_end - heap_start))
                bail("FDT: couldn't reserve retained heap\n");
            printf("FDT: retained heap [0x%lx, 0x%lx)\n", heap_start, heap_end);
        }
    }
    if (fdt_add_mem_rsv(dt, (u64)_base, image_end - (u64)_base))
        bail("FDT: couldn't add reservation for aura and inherited high-water\n");

    /*
     * T8152: keep everything below this image as well (the earlier stage and
     * what iBoot placed there), as the host loader did. With that range free,
     * Linux took write permission faults there from secondary cores; the boot
     * CPU's CTRR registers do not describe it.
     */
    if (chip_id == T8152 && (u64)_base > cur_boot_args.phys_base) {
        if (fdt_add_mem_rsv(dt, cur_boot_args.phys_base, (u64)_base - cur_boot_args.phys_base))
            bail("FDT: couldn't reserve the memory below this stage\n");
        printf("FDT: reserved [0x%lx, 0x%lx) below this stage\n", cur_boot_args.phys_base,
               (u64)_base);
    }

    /*
     * A live ANS handed to Linux keeps every buffer an earlier stage gave it (on a normal boot
     * Stage 1 read the ESP, so the buffers are in Stage 1's heap, not this one's). Reserve each
     * SART-allowed range inside RAM; the nvme-apple adoption requires them reserved anyway.
     */
    if (chip_id == T6041 || chip_id == T8142) {
        sart_dev_t *sart = sart_init("/arm-io/sart-ans");
        u64 ram_start = cur_boot_args.phys_base;
        u64 ram_end = ram_start + cur_boot_args.mem_size;

        if (!sart)
            bail("FDT: cannot read the ANS SART\n");
        for (int i = 0; i < 16; i++) {
            u64 paddr;
            size_t sz;

            if (!sart_get_allowed(sart, i, &paddr, &sz) || paddr < ram_start || paddr >= ram_end)
                continue;
            u64 start = ALIGN_DOWN(paddr, SZ_16K);
            u64 end = ALIGN_UP(min(paddr + sz, ram_end), SZ_16K);
            if (fdt_add_mem_rsv(dt, start, end - start)) {
                sart_release(sart);
                bail("FDT: couldn't reserve ANS buffer\n");
            }
            printf("FDT: ANS buffer [0x%lx, 0x%lx) reserved\n", start, end);
        }
        sart_release(sart);
    }

    // aura moved itself (start.S). On M3 it moved above the chainloaded image,
    // and cores with a locked RVBAR still reset into the forwarding vector at
    // the old base. On T8140 it moved down to the run base stage 1 gave it;
    // the old base is the staged copy, already inside the extent above.
    if (relocated_from && fdt_add_mem_rsv(dt, relocated_from, SZ_16K))
        bail("FDT: couldn't add reservation for aura's old reset vector\n");

    /*
     * CTXR_D starts below this image, and a secondary at EL1 that executes
     * inside it takes an abort straight to EL2. The firmware RO range below
     * covers windows A to C.
     */
    if (monitor_required()) {
        u64 start = ALIGN_DOWN(mrs(SYS_IMP_APL_CTXR_D_LWR_EL1), SZ_16K);
        u64 end = ALIGN_UP(mrs(SYS_IMP_APL_CTXR_D_UPR_EL1) + SZ_4K, SZ_16K);

        if (!start || end <= start || fdt_add_mem_rsv(dt, start, end - start))
            bail("FDT: couldn't reserve CTXR_D\n");
        printf("FDT: reserved CTXR_D [0x%lx, 0x%lx)\n", start, end);
    }

    if (chip_id == T8140) {
        u64 ro_start, ro_end;
        if (memory_fw_ro_range(&ro_start, &ro_end) != 1)
            bail("FDT: invalid T8140 firmware RO range\n");
        if (fdt_add_mem_rsv(dt, ro_start, ro_end - ro_start))
            bail("FDT: couldn't reserve T8140 firmware RO range\n");

        int resv = fdt_path_offset(dt, "/reserved-memory");
        if (resv < 0)
            bail("FDT: missing /reserved-memory for firmware RO range\n");
        char name[48];
        snprintf(name, sizeof(name), "aura-secondary-ro@%lx", ro_start);
        int node = fdt_subnode_offset(dt, resv, name);
        if (node < 0)
            node = fdt_add_subnode(dt, resv, name);
        if (node < 0)
            bail("FDT: couldn't create firmware RO node\n");
        fdt64_t reg[] = {cpu_to_fdt64(ro_start), cpu_to_fdt64(ro_end - ro_start)};
        if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) || fdt_setprop_empty(dt, node, "no-map"))
            bail("FDT: couldn't describe firmware RO node\n");
        printf("FDT: reserved firmware RO [0x%lx, 0x%lx)\n", ro_start, ro_end);
    } else if (!cpu_features->apple_sysregs_unlocked && !in_el2()) {
        /*
         * A guest under the aura hypervisor runs at EL1. The host keeps the
         * SMP-RO range and starts the guest's CPUs itself, so the guest
         * leaves it out. This is how the J616s guest boots.
         */
        printf("FDT: running at EL1, leaving the M4+ SMP-RO range to the host\n");
    } else if (!cpu_features->apple_sysregs_unlocked) {
        // On M4* / A18 Pro / M5*, there is a CTRR instance left enabled only
        // on the secondary (non-boot) cores. Add a reserved-memory region to
        // prevent Linux from crashing when allocating this memory and writing
        // to it from a secondary core.
        u64 ro_start = mrs(SYS_IMP_APL_CTRR_C_LWR_EL1);
        u64 ro_end = ALIGN_UP(mrs(SYS_IMP_APL_CTRR_C_UPR_EL1), SZ_4K);
        if (ro_end - ro_start > 12 * SZ_4K)
            printf("WARNING: M4+ SMP-RO is large (48KB)");
        if (fdt_add_mem_rsv(dt, ro_start, ro_end - ro_start))
            bail("FDT: couldn't add reservation for M4+ SMP-RO CTRR area\n");

        // The secondaries also enforce CTRR_D: on J813 (M5) it covers the
        // ~1 MiB just below the iBoot-loaded image.
        ro_start = mrs(SYS_IMP_APL_CTRR_D_LWR_EL1);
        ro_end = ALIGN_UP(mrs(SYS_IMP_APL_CTRR_D_UPR_EL1), SZ_4K) + SZ_4K;
        if (ro_start && ro_start < ro_end) {
            if (fdt_add_mem_rsv(dt, ro_start, ro_end - ro_start))
                bail("FDT: couldn't add reservation for M4+ SMP-RO CTRR_D area\n");
            printf("FDT: reserved M4+ SMP-RO CTRR_D [0x%lx, 0x%lx)\n", ro_start, ro_end);
        }
    }

    if (chip_id == T8132 && !fdt_node_check_compatible(dt, 0, "apple,j713")) {
        if (dt_reserve_bootloader_handoff())
            return -1;
        u64 start = min(ALIGN_UP((u64)adt + cur_boot_args.devtree_size, SZ_16K), (u64)_base);
        u64 end = cur_boot_args.top_of_kernel_data;
        if (end > start && fdt_add_mem_rsv(dt, start, end - start))
            bail("FDT: couldn't reserve the J713 chainload high-water interval\n");
    }

    /* setup console log buffer early to capture as much log as possible */
    dt_setup_mtd_phram();

    if (dt_set_m3_defaults())
        return -1;
    if (dt_set_chosen())
        return -1;
    if (chip_id == T8152)
        return dt_prepare_t8152();
    if (dt_set_speaker_safety())
        return -1;
    if (dt_set_uboot_config())
        return -1;
    if (dt_set_serial_number())
        return -1;
    if (dt_set_cpus())
        return -1;
    if (dt_set_smbios())
        return -1;
    if (dt_set_mac_addresses())
        return -1;
    if (dt_set_wifi())
        return -1;
    if (dt_set_bluetooth())
        return -1;
    if (dt_set_uboot())
        return -1;
    if (kboot_setup_atc(dt))
        return -1;
    if (dt_set_acio_tunables())
        return -1;
    if (dt_set_pcie_tunables())
        return -1;
    if (dt_set_ave())
        return -1;
    if (dt_set_ane_firmware())
        return -1;
    if (dt_set_display())
        return -1;
    if (dt_set_gpu(dt))
        return -1;
    if (dt_set_multitouch())
        return -1;
    if (dt_set_sep())
        return -1;
    if (dt_set_pmp())
        return -1;
    if (dt_set_nvram())
        return -1;
    if (dt_set_ipd())
        return -1;
    if (dt_disable_missing_devs("usb-drd", "usb@", 8, 0))
        return -1;
    if (dt_disable_missing_devs("acio", "cio@", 8, 2))
        return -1;
    if (dt_disable_missing_devs("i2c", "i2c@", 8, 0))
        return -1;
    if (dt_disable_missing_j700_touch_id())
        return -1;
    if (dt_setup_sio())
        return -1;
    if (dt_set_aop_firmware())
        return -1;
    if (dt_set_isp())
        return -1;
    if (dt_set_pmgr())
        return -1;
    if (chip_id == T8132 && !fdt_node_check_compatible(dt, 0, "apple,j713") &&
        (dt_set_ans_nvme(false) || dt_set_t8132_lifecycle()))
        return -1;
#ifndef RELEASE
    if (dt_transfer_virtios())
        return 1;
#endif

    /*
     * Append generic "apple,*" not carried in the upstream Linux DT for t602x
     * devices.
     * DO NOT remove before 2027-07-01
     */
    if (fdt_node_check_compatible(dt, 0, "apple,t6020") == 0 ||
        fdt_node_check_compatible(dt, 0, "apple,t6021") == 0 ||
        fdt_node_check_compatible(dt, 0, "apple,t6022") == 0)
        dt_fixup_t6020_compat(dt);

    /*
     * Set the /memory node late since we might be allocating from the top of memory
     * in one of the above devicetree prep functions, and we want an up-to-date value
     * for the usable memory span to make it into the devicetree.
     */
    if (dt_set_memory())
        return -1;
    if (dt_set_smbios_firmware())
        return -1;

    /* Last, so that its 15K never takes room the steps above need. */
    if (dt_set_pmp_t6030())
        return -1;

    /* Optional export goes last so it cannot consume space needed by fixups. */
    if (chip_id == T8122)
        dt_gpu_t8122_diagnostics(dt);

    /* After the GPU export, so that one stays as before. */
    if (chip_id == T8122) {
        dt_set_pmp_t8122();
        dt_pmp_t8122_facts(dt);
    }

    /* Read-only ADT export, last, in its own buffer room. */
    if (chip_id == T6030)
        dt_display_facts_t6030(dt);

    /* T6031 (J514C/J516C): this Mac's PMP values and firmware image UUIDs, in their own room.
     * T6032 (J575D): the UUID list only (its 27.0.1 ADT has no PMP and no disp0 clock facts).
     */
    if (chip_id == T6031) {
        dt_set_pmp_t6031();
        dt_t6031_fw_uuids(dt);
        dt_t6031_facts(dt);
    } else if (chip_id == T6032) {
        dt_t6031_fw_uuids(dt);
    }

    /* T8140 PCIe status is known only after pcie_init() in kboot_boot(). */
    if (chip_id != T8140 && fdt_pack(dt))
        bail("FDT: fdt_pack() failed\n");

    if (chip_id != T8140) {
        u32 dt_remain = dt_bufsize - fdt_totalsize(dt);
        if (dt_remain < SZ_16K)
            printf("FDT: free dt buffer space low, %u bytes left\n", dt_remain);
    }

    printf("FDT prepared at %p\n", dt);

    return 0;
}

static int dt_disable_j713_pcie(void)
{
    /* prepare_dt packed the tree. Restore its already reserved spare capacity
     * before adding status, then pack again for downstream source-size bounds. */
    if (fdt_open_into(dt, dt, dt_bufsize) || pcie_disable_j713_fdt(dt) || fdt_pack(dt))
        return -1;
    return 0;
}

/* J614s AOP handoff. Run only after the DAPF result is known. */
static int dt_finish_j614s_aop(void *tree, bool ready)
{
    fdt32_t keep[2];
    u32 remove[8], handles[8];
    int len, count, code = 0, log = 0, nkeep = 0, nremove = 0;
    int aop = fdt_node_offset_by_compatible(tree, -1, "apple,t6041-aop");
    const fdt32_t *refs;

    if (!ready || aop < 0)
        return -1;
    refs = fdt_getprop(tree, aop, "memory-region", &len);
    if (!refs || len <= 0 || len > (int)sizeof(handles) || len % 4)
        return -1;
    count = len / 4;
    for (int i = 0; i < count; i++)
        handles[i] = fdt32_ld(refs + i);
    for (int i = 0; i < count; i++) {
        int node = fdt_node_offset_by_phandle(tree, handles[i]), bytes;
        const fdt64_t *reg;
        const fdt32_t *maps;
        u64 phys, size, iova, span;
        if (node < 0)
            return -1;
        reg = fdt_getprop(tree, node, "reg", &bytes);
        if (!reg || bytes != 16 || !fdt_getprop(tree, node, "no-map", NULL))
            return -1;
        phys = fdt64_ld(reg);
        size = fdt64_ld(reg + 1);
        maps = fdt_getprop(tree, node, "iommu-addresses", &bytes);
        if (!maps || bytes != 20 || fdt32_ld(maps) != fdt_get_phandle(tree, aop))
            return -1;
        iova = (u64)fdt32_ld(maps + 1) << 32 | fdt32_ld(maps + 2);
        span = (u64)fdt32_ld(maps + 3) << 32 | fdt32_ld(maps + 4);
        if (!size || size != span || (phys & 0x3fff) || (size & 0x3fff) ||
            (iova & 0x3fff) || phys > UINT64_MAX - size || iova > UINT64_MAX - size)
            return -1;
        if (iova != 0x10000000000ULL && iova != phys) {
            remove[nremove++] = handles[i];
            continue;
        }
        if (nkeep == 2)
            return -1;
        code += iova == 0x10000000000ULL;
        log += iova == phys;
        keep[nkeep++] = cpu_to_fdt32(handles[i]);
    }
    if (code != 1 || log != 1 || nkeep != 2)
        return -1;
    for (int i = 0; i < nremove; i++) {
        int node = fdt_node_offset_by_phandle(tree, remove[i]);
        if (node < 0 || fdt_delprop(tree, node, "iommu-addresses"))
            return -1;
    }
    aop = fdt_node_offset_by_compatible(tree, -1, "apple,t6041-aop");
    if (aop < 0 || fdt_setprop(tree, aop, "memory-region", keep, sizeof(keep)))
        return -1;
    aop = fdt_node_offset_by_compatible(tree, -1, "apple,t6041-aop");
    refs = fdt_getprop(tree, aop, "iommus", &len);
    if (!refs || len < 8)
        return -1;
    int dart = fdt_node_offset_by_phandle(tree, fdt32_ld(refs));
    if (dart < 0 || fdt_setprop_string(tree, dart, "status", "okay"))
        return -1;
    return 0;
}


int kboot_boot(void *kernel)
{
    if (chip_id == T8152)
        return kboot_boot_t8152(kernel);

    if (smp_has_pending_calls()) {
        next_stage.entry = NULL;
        bail("kboot: refusing handoff while secondary callbacks are pending\n");
    }
    int ret = mcc_enable_cache();
    if (ret && chip_id == T8140) {
        printf("kboot: refusing T8140 handoff after MCC cache failure\n");
        return -1;
    }
    tunables_apply_static();
    clk_init();

    if (chip_id == T8140)
        printf("kboot: normal USB init skipped on T8140\n");
    else
        usb_init();
    int described = chip_id == T8132 ? pcie_prepare_fdt(dt) : 0;
    bool j713 = chip_id == T8132 && !fdt_node_check_compatible(dt, 0, "apple,j713");
    if (described < 0) {
        if (!j713 || dt_disable_j713_pcie())
            return -1;
        /* Do not train an optional port whose rail could not be prepared. */
        ret = 0;
    } else {
        ret = pcie_init();
        if (chip_id == T8132 && ((ret && described) || pcie_handoff_fdt(dt) < 0)) {
            if (!j713 || dt_disable_j713_pcie())
                return -1;
        }
    }
    if (chip_id == T8140) {
        if (ret < 0) {
            printf("kboot: refusing T8140 handoff after partial PCIe setup\n");
            return -1;
        }
        if (ret > 0) {
            printf("kboot: T8140 APCIe unavailable; disabling PCIe consumers\n");
            if (dt_disable_t8140_pcie())
                return -1;
        } else {
            printf("kboot: T8140 APCIe initialized; retaining PCIe consumers\n");
        }
        if (fdt_pack(dt))
            bail("FDT: fdt_pack() failed\n");
        u32 dt_remain = dt_bufsize - fdt_totalsize(dt);
        if (dt_remain < SZ_16K)
            printf("FDT: free dt buffer space low, %u bytes left\n", dt_remain);
    }
    ret = (chip_id == T8142 || chip_id == T6041 || chip_id == T8132) ? dapf_init_fdt(dt)
                                                                     : dapf_init_all();
    if (ret < 0 && chip_id == T8140) {
        printf("kboot: refusing T8140 handoff after DAPF failure\n");
        pcie_shutdown();
        return -1;
    }

    if (chip_id == T6040 && board_id == 4 && adt_is_compatible(adt, 0, "J614sAP") &&
        !fdt_node_check_compatible(dt, 0, "apple,j614s")) {
        if (fdt_open_into(dt, dt, dt_bufsize))
            return -1;
        void *aop_tree = malloc(dt_bufsize);
        if (!aop_tree)
            return -1;
        int aop_ret = fdt_open_into(dt, aop_tree, dt_bufsize);
        if (!aop_ret)
            aop_ret = dt_finish_j614s_aop(aop_tree, dapf_j614s_aop_ready());
        if (!aop_ret) {
            memcpy(dt, aop_tree, dt_bufsize);
        } else {
            int aop = fdt_node_offset_by_compatible(dt, -1, "apple,t6041-aop");
            if (aop >= 0 && fdt_setprop_string(dt, aop, "status", "disabled")) {
                free(aop_tree);
                return -1;
            }
            printf("FDT: J614s AOP handoff failed, keeping AOP disabled\n");
        }
        free(aop_tree);
        if (fdt_pack(dt))
            return -1;
    }
    printf("Setting SMP mode to WFE...\n");
    smp_set_wfe_mode(true);
    if (chip_id == T8140 && dt_t8140_recheck_display())
        return -1;
    printf("Preparing to boot kernel at %p with fdt at %p\n", kernel, dt);

    if (smp_has_pending_calls()) {
        next_stage.entry = NULL;
        bail("kboot: secondary callback became pending before kernel entry\n");
    }
    next_stage.entry = kernel;
    next_stage.args[0] = (u64)dt;
    next_stage.args[1] = 0;
    next_stage.args[2] = 0;
    next_stage.args[3] = 0;
    next_stage.args[4] = 0;
    next_stage.restore_logo = false;
    next_stage.monitor = monitor_required();

    return 0;
}
