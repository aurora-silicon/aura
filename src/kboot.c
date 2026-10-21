/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include <stdint.h>

#include "kboot.h"
#include "kboot_gpu_t8122.h"
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
#include "malloc.h"
#include "mcc.h"
#include "memory.h"
#include "pcie.h"
#include "pmgr.h"
#include "sep.h"
#include "sio.h"
#include "smp.h"
#include "tunables.h"
#include "types.h"
#include "usb.h"
#include "utils.h"
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
static char *uboot_config[MAX_UBOOT_CONFIGS][2];

extern const char *const m1n1_version;
extern char _file_end[];

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

    // save notch height in the dcp node if present
    if (cur_boot_args.video.height - fb_height) {
        int dcp = fdt_path_offset(dt, "dcp");
        if (dcp >= 0)
            if (fdt_appendprop_u32(dt, dcp, "apple,notch-height",
                                   cur_boot_args.video.height - fb_height))
                printf("FDT: couldn't set apple,notch-height\n");
    }

    return 0;
}

static bool dt_bootarg_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static int dt_set_wfx_args(int node)
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
        bail("FDT: couldn't set WFx bootargs\n");
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

    if (chip_id == T8140 && dt_set_wfx_args(node))
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

    if (fdt_setprop(dt, node, "asahi,m1n1-stage2-version", m1n1_version, strlen(m1n1_version) + 1))
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
     * handoff. Querying the SEP ASC mailbox in m1n1 desynchronizes this
     * state, causing the SEP firmware to refuse subsequent biometric commands.
     * M1 SoCs (T8103, T600x) rely on cold boot and do not have this restriction.
     */
    bool sep_warm_active =
        (chip_id != T8103 && chip_id != T6000 && chip_id != T6001 && chip_id != T6002);

    if (sep_warm_active) {
        printf("SEP: Preserving iBoot warm registration; seeding RNG from ADT\n");
        return dt_set_rng_seed_adt(node);
    }

    if (dt_set_rng_seed_sep(node))
        return dt_set_rng_seed_adt(node);

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

            u64 resv_start = fdt64_to_cpu(reg[0]);
            u64 resv_len = fdt64_to_cpu(reg[1]);
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

static int dt_set_smbios(void)
{
    int fdt_root = fdt_path_offset(dt, "/");
    int adt_root = adt_path_offset(adt, "/");
    int len, node, smbios_node;

    if (fdt_root < 0)
        bail("FDT: could not open a handle to FDT root.\n");
    if (adt_root < 0)
        bail("ADT: could not open a handle to ADT root.\n");

    node = fdt_add_subnode(dt, fdt_root, "smbios");
    if (node < 0)
        bail("FDT: failed to add node 'smbios' to  '/'\n");
    fdt_setprop_string(dt, node, "compatible", "u-boot,sysinfo-smbios");

    smbios_node = fdt_add_subnode(dt, node, "smbios");
    if (smbios_node < 0)
        bail("FDT: failed to add node 'smbios' to  '/smbios'\n");

    node = fdt_add_subnode(dt, smbios_node, "system");
    if (node < 0)
        bail("FDT: failed to add node 'system' to  '/smbios'\n");

    const char *manuf = adt_getprop(adt, adt_root, "manufacturer", NULL);
    if (!manuf)
        bail("ADT: failed to get manufacturer property\n");
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    const char *model = adt_getprop(adt, adt_root, "model", NULL);
    if (!model)
        bail("ADT: failed to get model property\n");
    fdt_setprop_string(dt, node, "product", model);
    const char *serial = adt_getprop(adt, adt_root, "serial-number", NULL);
    if (!serial)
        bail("ADT: failed to get serial-number property\n");
    fdt_setprop_string(dt, node, "serial", serial);

    node = fdt_add_subnode(dt, smbios_node, "baseboard");
    if (node < 0)
        bail("FDT: failed to add node 'baseboard' to  '/smbios'\n");
    fdt_setprop_string(dt, node, "manufacturer", manuf);
    const char *target_type = adt_getprop(adt, 0, "target-type", NULL);
    if (!target_type)
        bail("ADT: failed to get target-type property\n");
    fdt_setprop_string(dt, node, "product", target_type);

    node = fdt_add_subnode(dt, smbios_node, "chassis");
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

        if (!smp_is_alive(cpu)) {
            printf("FDT: CPU %d is not alive, disabling...\n", cpu);
            pruned_phandles[pruned++] = fdt_get_phandle(dt, node);

            int next = fdt_next_subnode(dt, node);
            fdt_nop_node(dt, node);
            cpu++;
            node = next;
            continue;
        }

        u64 mpidr = smp_get_mpidr(cpu);

        if (dt_mpidr != mpidr)
            bail_cleanup("FDT: DT CPU %d MPIDR mismatch: 0x%lx != 0x%lx\n", cpu, dt_mpidr, mpidr);

        u64 release_addr = smp_get_release_addr(cpu);
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

static int dt_disable_t8140_pcie(void)
{
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
            ret = -1;
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
     * it and breaking display scanout when booting with old m1n1 which
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
    /* enable dcp* */
    if (chip_id != T8140) {
        int dcp_node = fdt_path_offset(dt, dcp_alias);
        if (dcp_node < 0 || fdt_setprop_string(dt, dcp_node, "status", "okay") < 0)
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

static int dt_reserve_asc_firmware(const char *adt_path, const char *adt_path_alt,
                                   const char *fdt_path, bool remap, u64 base)
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

    uint32_t dev_phandle = fdt_get_phandle(dt, fdt_node);
    if (!dev_phandle) {
        ret = fdt_generate_phandle(dt, &dev_phandle);
        if (!ret)
            ret = fdt_setprop_u32(dt, fdt_node, "phandle", dev_phandle);
        if (ret != 0)
            bail("FDT: couldn't set '%s.phandle' property: %d\n", fdt_path, ret);
    }

    const struct adt_segment_ranges *seg;
    u32 segments_len;

    seg = adt_getprop(adt, node, "segment-ranges", &segments_len);
    unsigned int num_maps = segments_len / sizeof(*seg);

    for (unsigned i = 0; i < num_maps; i++) {
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
            return -1;
        uint32_t mem_phandle = fdt_get_phandle(dt, mem_node);

        ret = dt_device_set_reserved_mem(mem_node, node_name, dev_phandle, iova, seg_size);
        if (ret < 0)
            return ret;

        ret = dt_device_add_mem_region(fdt_path, mem_phandle, NULL);
        if (ret < 0)
            return ret;

        seg++;
    }

    return 0;
}

static int dt_set_ave(void)
{
    if (chip_id != T8140)
        return 0;

    const char *path = "/soc/video-codec@285100000";
    int node = fdt_path_offset(dt, path);
    if (node < 0)
        return 0;

    const char *reason = "invalid ADT firmware segments";
    int anode = adt_path_offset(adt, "/arm-io/ave");
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

    if (dt_reserve_asc_firmware("/arm-io/ave", NULL, path, true, 0)) {
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

static const char dcpext_aliases[][8] = {
    "dcpext",  "dcpext0", "dcpext1", "dcpext2", "dcpext3",
    "dcpext4", "dcpext5", "dcpext6", "dcpext7",
};

static int dt_reserve_dcpext_firmware(void)
{
    /* reserve asc for all  */
    for (size_t n = 0; n < ARRAY_SIZE(dcpext_aliases); n++) {
        const char *dcpext_alias = dcpext_aliases[n];
        char adt_path[48];

        snprintf(adt_path, sizeof(adt_path) - 1, "/arm-io/%s/iop-%s-nub", dcpext_alias,
                 dcpext_alias);
        adt_path[sizeof(adt_path) - 1] = '\0';

        if (adt_path_offset(adt, adt_path) < 0)
            continue;

        int dcpext_node = fdt_path_offset(dt, dcpext_alias);
        if (dcpext_node < 0)
            continue;

        int ret = dt_reserve_asc_firmware(adt_path, NULL, dcpext_alias, true, 0);
        if (ret < 0)
            bail("FDT: reserving ASC firmware for %s failed!\n", dcpext_alias);

        ret = dt_set_dcp_firmware(dcpext_alias);
        if (ret)
            continue;

        // refresh dcpext_node
        dcpext_node = fdt_path_offset(dt, dcpext_alias);
        if ((dcpext_node < 0) || (fdt_setprop_string(dt, dcpext_node, "status", "okay") < 0))
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

    printf("FDT: T8140 %s/%s: check ADT %s PA 0x%lx size 0x%lx\n", dcp_path, name, region_id,
           pair[0], pair[1]);

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

    printf("FDT: T8140 %s/%s: check ADT %s PA 0x%lx size 0x%lx,"
           " tail PA 0x%lx size 0x%x\n",
           dcp_path, name, region_id, pair[0], pair[1], seg[1].phys, seg[1].size);

    struct mem_region regions[2] = {{pair[0], pair[1] - seg[1].size}, {seg[1].phys, seg[1].size}};
    struct disp_mapping maps[2] = {0};
    snprintf(maps[0].mem_fdt, sizeof(maps[0].mem_fdt), "%s", name);
    snprintf(maps[1].mem_fdt, sizeof(maps[1].mem_fdt), "%s_tail", name);
    maps[0].map_dcp = maps[1].map_dcp = true;
    return dt_add_reserved_regions(dcp_path, NULL, NULL, "apple,asc-mem", maps, regions, 2);
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

static bool dt_t8140_validate_dcp_asc(const char *path, const char *name, bool allow_idle)
{
    asc_dev_t *asc = asc_init(path);
    if (!asc) {
        printf("FDT: T8140 %s: ASC unavailable at %s\n", name, path);
        return false;
    }
    bool ready = asc_validate_inherited_dcp(asc, name, allow_idle);
    asc_free(asc);
    return ready;
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

static int dt_t8140_refusal_group(const char *compatible, struct dt_t8140_refusal_group *group)
{
    memset(group, 0, sizeof(*group));
    group->controller = fdt_node_offset_by_compatible(dt, -1, compatible);
    group->piodma = -FDT_ERR_NOTFOUND;
    if (group->controller < 0)
        return group->controller == -FDT_ERR_NOTFOUND ? 0 : -1;
    /* An alias cannot choose among multiple controllers of the same role. */
    if (fdt_node_offset_by_compatible(dt, group->controller, compatible) != -FDT_ERR_NOTFOUND)
        return -1;
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
    if (dt_t8140_disable_dcpext())
        return -1;
    chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0)
        return -1;
    int ret = fdt_delprop(dt, chosen, "apple,dcp-rtkit-quiesced");
    if (ret && ret != -FDT_ERR_NOTFOUND)
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

/* A bounded observation of register descriptions, not firmware contents.
 * IDs and lengths make absent Linux aliases diagnosable without exporting ADT. */
static void dt_t8140_external_metadata(void)
{
    static const char *const paths[] = {
        "/arm-io/dcpext",        "/arm-io/dart-dcpext",
        "/arm-io/dart-dispext0", "/arm-io/dcpext/iop-dcpext-nub",
        "/arm-io/dart-dcpext0",  "/arm-io/dispext0",
        "/arm-io/disp-ext0",
    };
    static const struct {
        const char *name;
        bool cells;
    } props[] = {
        {"reg", true},
        {"interrupts", true},
        {"clock-gates", true},
        {"device-id", true},
        {"AAPL,phandle", true},
        {"#address-cells", true},
        {"#size-cells", true},
        {"ranges", false},
        {"dpxbar-parent", false},
        {"dp-switch-dfp-port", false},
        {"dp-switch-dfp-endpoint", false},
        {"atc-dp-link", false},
        {"service-gates", false},
        {"segment-ranges", false},
        {"segment-names", false},
        {"iommu-parent", true},
    };
    fdt32_t translated[2 + ARRAY_SIZE(paths) * 34];
    size_t translated_used = 0;
    fdt32_t words[1024];
    size_t used = 0;
    int chosen = fdt_path_offset(dt, "/chosen");
    if (chosen < 0 || !adt || adt_get_size() > 16 * SZ_1M)
        return;
    words[used++] = cpu_to_fdt32(1);
    words[used++] = cpu_to_fdt32(ARRAY_SIZE(paths));
    translated[translated_used++] = cpu_to_fdt32(1);
    translated[translated_used++] = cpu_to_fdt32(ARRAY_SIZE(paths));
    for (size_t i = 0; i < ARRAY_SIZE(paths); i++) {
        int trace[8] = {0};
        int node = adt_path_offset_trace(adt, paths[i], trace);
        translated[translated_used++] = cpu_to_fdt32(i);
        size_t count_offset = translated_used++;
        u32 registers = 0;
        /* The public ADT translator applies every parent bus range. Never
         * infer physical addresses from the raw bus-relative reg cells. */
        for (unsigned reg = 0; node >= 0 && reg < 8; reg++) {
            u64 address, size;
            if (adt_get_reg(adt, trace, "reg", reg, &address, &size) < 0)
                break;
            translated[translated_used++] = cpu_to_fdt32(address >> 32);
            translated[translated_used++] = cpu_to_fdt32(address);
            translated[translated_used++] = cpu_to_fdt32(size >> 32);
            translated[translated_used++] = cpu_to_fdt32(size);
            registers++;
        }
        translated[count_offset] = cpu_to_fdt32(registers);
        words[used++] = cpu_to_fdt32(i);
        words[used++] = cpu_to_fdt32(node >= 0);
        words[used++] = cpu_to_fdt32(ARRAY_SIZE(props));
        for (size_t j = 0; j < ARRAY_SIZE(props); j++) {
            u32 length = 0;
            const void *value = node >= 0 ? adt_getprop(adt, node, props[j].name, &length) : NULL;
            /* Only public register/cell descriptions have a numeric payload.
             * Segments, routes and every unknown encoding remain size-only. */
            size_t count = value && props[j].cells && length <= 128 && !(length & 3)
                               ? length / sizeof(u32)
                               : 0;
            if (used + 4 + count > ARRAY_SIZE(words)) {
                printf("FDT: external metadata exceeds bounded output\n");
                return;
            }
            words[used++] = cpu_to_fdt32(j);
            words[used++] = cpu_to_fdt32(value != NULL);
            words[used++] = cpu_to_fdt32(value ? length : 0);
            words[used++] = cpu_to_fdt32(count);
            for (size_t k = 0; k < count; k++) {
                u32 cell;
                memcpy(&cell, (const u8 *)value + k * sizeof(cell), sizeof(cell));
                words[used++] = cpu_to_fdt32(cell);
            }
        }
    }
    if (fdt_setprop(dt, chosen, "asahi,external-display-metadata", words, used * sizeof(words[0])))
        printf("FDT: external metadata observation unavailable\n");
    /* Mapper phandle/stream pairs are public ADT cell descriptions. Export
     * neither names nor arbitrary child properties, and cap both traversal
     * and output even if the firmware describes an unexpected topology. */
    fdt32_t mappers[2 + 2 * (2 + 16 * 3)];
    size_t mapper_used = 0;
    mappers[mapper_used++] = cpu_to_fdt32(1);
    mappers[mapper_used++] = cpu_to_fdt32(2);
    for (unsigned bank = 0; bank < 2; bank++) {
        int node = adt_path_offset(adt, paths[bank ? 2 : 4]);
        mappers[mapper_used++] = cpu_to_fdt32(bank ? 2 : 4);
        size_t count_offset = mapper_used++;
        unsigned accepted = 0;
        int child = node >= 0 ? adt_first_child_offset(adt, node) : -1;
        int children = node >= 0 ? adt_get_child_count(adt, node) : 0;
        for (int seen = 0; child >= 0 && seen < children && seen < 64 && accepted < 16;
             seen++, child = adt_next_sibling_offset(adt, child)) {
            u32 phandle, stream;
            if (adt_getprop_copy(adt, child, "AAPL,phandle", &phandle, sizeof(phandle)) < 0 ||
                adt_getprop_copy(adt, child, "reg", &stream, sizeof(stream)) < 0)
                continue;
            mappers[mapper_used++] = cpu_to_fdt32(seen);
            mappers[mapper_used++] = cpu_to_fdt32(phandle);
            mappers[mapper_used++] = cpu_to_fdt32(stream);
            accepted++;
        }
        mappers[count_offset] = cpu_to_fdt32(accepted);
    }
    if (fdt_setprop(dt, chosen, "asahi,external-display-mappers", mappers,
                    mapper_used * sizeof(mappers[0])))
        printf("FDT: external mapper observation unavailable\n");
    if (fdt_setprop(dt, chosen, "asahi,external-display-registers", translated,
                    translated_used * sizeof(translated[0])))
        printf("FDT: translated external register observation unavailable\n");
}

static int dt_set_display_t8140(void)
{
    dt_t8140_external_metadata();
    int ret = -1;
    const char *reason = "validation failed";
    const char *step = "preflight";
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
    printf("FDT: T8140 /vram: ADT reg PA 0x%lx size 0x%lx (ret %d)\n", base, size, reg_ret);
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
    printf("FDT: T8140 framebuffer: PA 0x%lx size 0x%lx; /vram PA 0x%lx size 0x%lx\n",
           cur_boot_args.video.base, fb_size, base, size);
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
    printf("FDT: T8140 physical DRAM: PA 0x%lx size 0x%lx; /vram PA 0x%lx size 0x%lx\n", dram.base,
           dram.size, base, size);
    if (!dram.size || dram.base > UINT64_MAX - dram.size) {
        reason = "invalid physical DRAM extent";
        goto refuse;
    }
    if (!dt_t8140_in_physical_dram(&dram, base, size)) {
        reason = "/vram lies outside physical DRAM";
        goto refuse;
    }
    printf("FDT: T8140 framebuffer: sub-range of /vram validated\n");

    /* On the measured J700 ADT, /vram exactly matches carveout region 14
     * and an opaque DCP nub segment. Check both independent ADT descriptions
     * before publishing it; neither establishes an IOVA on its own. */
    node = adt_path_offset(adt, "/chosen/carveout-memory-map");
    u64 carveout[2] = {0};
    int carveout_len = node < 0 ? -1 : ADT_GETPROP_ARRAY(adt, node, "region-id-14", carveout);
    printf("FDT: T8140 region-id-14: ADT PA 0x%lx size 0x%lx (len %d);"
           " /vram PA 0x%lx size 0x%lx\n",
           carveout[0], carveout[1], carveout_len, base, size);
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
        printf("FDT: T8140 DCP nub segment %u: PA 0x%lx size 0x%x;"
               " /vram PA 0x%lx size 0x%lx\n",
               i, seg[i].phys, seg[i].size, base, size);
        if (seg[i].phys == base && seg[i].size == size)
            segment_found = true;
    }
    if (!segment_found) {
        reason = "DCP nub has no segment matching /vram";
        goto refuse;
    }

    if (!fdt_get_alias(dt, "dcp") || !fdt_get_alias(dt, "disp0")) {
        reason = !fdt_get_alias(dt, "dcp") ? "FDT dcp alias absent" : "FDT disp0 alias absent";
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

    step = "internal DCP ASC and mailboxes";
    if (!dt_t8140_validate_dcp_asc("/arm-io/dcp", "dcp", false)) {
        reason = "inherited DCP ASC is not live with healthy empty mailboxes";
        goto rollback;
    }

    /* External firmware has its own DART. It is optional for the internal
     * panel, so a failed external validation only rolls back that controller. */
    void *ext_saved = malloc(dt_bufsize);
    if (!ext_saved) {
        step = "external FDT snapshot";
        goto rollback;
    }
    memcpy(ext_saved, dt, dt_bufsize);
    char ext_path[128];
    const char *ext = fdt_get_alias(dt, "dcpext");
    if (ext) {
        snprintf(ext_path, sizeof(ext_path), "%s", ext);
        ext = ext_path;
    } else {
        node = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
        if (node >= 0 && fdt_get_path(dt, node, ext_path, sizeof(ext_path)) == 0)
            ext = ext_path;
    }
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
            step = "DCPEXT ASC and mailboxes";
            ext_step = 7;
            ret = dt_t8140_validate_dcp_asc("/arm-io/dcpext", "dcpext", true) ? 0 : -1;
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
        ext_step = 8;
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
    step = "publish apple,dcp-rtkit-quiesced claim";
    node = fdt_path_offset(dt, "/chosen");
    if (node < 0)
        goto rollback;
    if (fdt_setprop_u32(dt, node, "apple,dcp-rtkit-quiesced", 1) < 0)
        goto rollback;
    /* A diagnostic result for this preflight, not a hardware-ready claim.
     * 0: absent, 1: power off, 2..7: validation stage, 8: dependencies. */
    if (fdt_setprop_u32(dt, node, "asahi,external-display-preflight", ext_ready ? 0x100 : ext_step))
        printf("FDT: external preflight observation unavailable\n");
    free(saved);
    printf("FDT: T8140 DCP enabled with validated framebuffer, firmware and live ASC%s;"
           " /chosen/apple,dcp-rtkit-quiesced=1\n",
           ext_ready ? " (dcpext enabled)" : "");
    return 0;

rollback:
    memcpy(dt, saved, dt_bufsize);
    free(saved);
    if (!strcmp(reason, "validation failed"))
        reason = "FDT consumer or property update failed";

refuse:
    printf("FDT: T8140 display refused at %s: %s; retaining simplefb\n", step, reason);
    return dt_t8140_refuse_display();
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
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t8142")) {
        ret = dt_carveout_reserved_regions("dcp", "disp0", "disp0_piodma",
                                           disp_reserved_regions_t8142,
                                           ARRAY_SIZE(disp_reserved_regions_t8142));
        if (ret)
            return ret;
    } else if (!fdt_node_check_compatible(dt, 0, "apple,t8122")) {
        /*
         * Fail-soft handoff (kboot_display_handoff.c): it always returns 0, never uses the
         * helpers above (they bail out of kboot or enable nodes) and never enables a node.
         * It reserves the DCP's __OS_LOG buffer and does nothing else unless
         * chosen.asahi,t8122-dcp=1. T6030 is not routed here yet: its handoff runs without the
         * switch and is untested with aura.
         */
        return dt_set_display_handoff(dt);
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
        dt_reserve_dcpext_firmware();

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

static int dt_set_pmp(void)
{
    /* The M3 Air's PMP is the kernel display gate's (dt_set_pmp_t8122()). */
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
 * Copies this Mac's PMP values from the ADT onto the FDT node @pmp_node, for dt_pass_pmp().
 * Stricter than dt_set_pmp(): every value must be exactly a u32.
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

/*
 * T8122: the kernel's display gate adds the PMP itself, halted until the display needs it, so
 * the device tree has no pmp node for dt_set_pmp(). The gate needs this Mac's PMP values for it:
 * copy them as dt_set_pmp() does, into /chosen/asahi,t8122-pmp, and leave the PMP alone. Only
 * when the PMP and its DART are where the gate puts them. Fail-soft: without the node the gate
 * keeps the display on the boot framebuffer. From m1n1-aurora, which also passes T6030's.
 */
struct pmp_values_soc {
    const char *compat; /* SoC compatible of the FDT root */
    const char *node;   /* the /chosen subnode */
    const char *name;
    u32 irqs[4]; /* /arm-io/pmp interrupts, in ADT order */
    u64 dart_addr, dart_size;
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

    if (fdt_node_check_compatible(dt, 0, soc->compat))
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
        printf("FDT: %s PMP is not where the kernel expects it; no PMP values passed\n",
               soc->name);
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

/* T8122: pass this Mac's PMP values on either M3 Air with chosen.asahi,t8122-dcp=1. */
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
    const char *alias = fdt_get_alias(dt, "sep");
    if (alias == NULL) {
        printf("FDT: sep alias not found in devtree\n");
        return 0;
    }

    /* fdt_get_alias() points into the FDT, which the reservation below grows. */
    char path[256];
    size_t path_len = strnlen(alias, sizeof(path));
    if (path_len == sizeof(path))
        bail("FDT: sep alias path is too long\n");
    memcpy(path, alias, path_len + 1);

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

void kboot_set_initrd(void *start, size_t size)
{
    initrd_start = start;
    initrd_size = size;
}

int kboot_set_chosen(const char *name, const char *value)
{
    int i = 0;

    if (!name)
        return -1;

    for (i = 0; i < MAX_CHOSEN_PARAMS; i++) {
        if (!chosen_params[i][0]) {
            chosen_params[i][0] = calloc(strlen(name) + 1, 1);
            strcpy(chosen_params[i][0], name);
            break;
        }

        if (!strcmp(name, chosen_params[i][0])) {
            free(chosen_params[i][1]);
            chosen_params[i][1] = NULL;
            break;
        }
    }

    if (i >= MAX_CHOSEN_PARAMS)
        return -1;

    if (value) {
        chosen_params[i][1] = calloc(strlen(value) + 1, 1);
        strcpy(chosen_params[i][1], value);
    }

    return i;
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
        bail("FDT: failed to allocate m1n1 log buffer\n");

    snprintf(node_name, sizeof(node_name), "flash@%lx", (u64)logbuf.buffer);
    node = dt_get_or_add_reserved_mem(node_name, "phram", false, (u64)logbuf.buffer, SZ_16K);

    if (node > 0) {
        int ret = fdt_setprop_string(dt, node, "label", "m1n1_stage2.log");
        if (ret)
            bail("FDT: failed to setup m1n1 log MTD phram label\n");
    }

    return 0;
}

int kboot_prepare_dt(void *fdt)
{
    if (dt) {
        free(dt);
        dt = NULL;
    }

    /* Need to init ISP early to carve out heap */
    isp_init();

    dt_bufsize = fdt_totalsize(fdt);
    assert(dt_bufsize);

    dt_bufsize += 6 * SZ_16K; // Add 96K of buffer for modifications
    // A T8122 also takes its PMP values, GPU setup and identity, and optional GPU diagnostics
    if (chip_id == T8122)
        dt_bufsize += 10 * SZ_16K;
    dt = memalign(DT_ALIGN, dt_bufsize);

    if (fdt_open_into(fdt, dt, dt_bufsize) < 0)
        bail("FDT: fdt_open_into() failed\n");

    if (fdt_add_mem_rsv(dt, (u64)dt, dt_bufsize))
        bail("FDT: couldn't add reservation for the devtree\n");

    u64 image_end = (u64)_end;
    if (chip_id == T8140) {
        u64 ram_end, inherited = cur_boot_args.top_of_kernel_data;
        u64 high_water = heapblock_high_water();

        if (!cur_boot_args.mem_size ||
            cur_boot_args.phys_base > UINT64_MAX - cur_boot_args.mem_size)
            bail("FDT: invalid inherited RAM extent\n");
        ram_end = cur_boot_args.phys_base + cur_boot_args.mem_size;
        image_end = max(image_end, inherited);
        if ((u64)_base < cur_boot_args.phys_base || image_end < (u64)_base || image_end > ram_end ||
            high_water > ram_end)
            bail("FDT: invalid T8140 retained memory extent\n");

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
        bail("FDT: couldn't add reservation for m1n1 and inherited high-water\n");

    // start.S moved this image above the chainloaded one; cores with a locked
    // RVBAR still reset into the forwarding vector at the old base.
    if (relocated_from) {
        extern u8 _vectors_start[];
        u64 old_vectors = relocated_from + ((u64)_vectors_start - (u64)_base);

        if (fdt_add_mem_rsv(dt, ALIGN_DOWN(old_vectors, SZ_16K), SZ_16K))
            bail("FDT: couldn't add reservation for the old reset vector\n");
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
        snprintf(name, sizeof(name), "m1n1-secondary-ro@%lx", ro_start);
        int node = fdt_subnode_offset(dt, resv, name);
        if (node < 0)
            node = fdt_add_subnode(dt, resv, name);
        if (node < 0)
            bail("FDT: couldn't create firmware RO node\n");
        fdt64_t reg[] = {cpu_to_fdt64(ro_start), cpu_to_fdt64(ro_end - ro_start)};
        if (fdt_setprop(dt, node, "reg", reg, sizeof(reg)) || fdt_setprop_empty(dt, node, "no-map"))
            bail("FDT: couldn't describe firmware RO node\n");
        printf("FDT: reserved firmware RO [0x%lx, 0x%lx)\n", ro_start, ro_end);
    } else if (!cpu_features->apple_sysregs_unlocked) {
        // On M4* / A18 Pro / M5*, there is a CTRR instance left enabled only
        // on the secondary (non-boot) cores. Add a reserved-memory region to
        // prevent Linux from crashing when allocating this memory and writing
        // to it from a secondary core.
        u64 ro_start = mrs(CTRR_M4_LWR_EL2);
        u64 ro_end = ALIGN_UP(mrs(CTRR_M4_UPR_EL2), SZ_4K);
        if (ro_end - ro_start > 12 * SZ_4K)
            printf("WARNING: M4+ SMP-RO is large (48KB)");
        if (fdt_add_mem_rsv(dt, ro_start, ro_end - ro_start))
            bail("FDT: couldn't add reservation for M4+ SMP-RO CTRR area\n");
    }

    /* setup console log buffer early to capture as much log as possible */
    dt_setup_mtd_phram();

    if (dt_set_chosen())
        return -1;
    if (dt_set_speaker_safety())
        return -1;
    if (dt_set_uboot_config())
        return -1;
    if (dt_set_serial_number())
        return -1;
    if (dt_set_smbios())
        return -1;
    if (dt_set_cpus())
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
    u64 isp_heap_phys, isp_heap_iova, isp_heap_size;
    if ((chip_id != T8140 || !isp_get_heap(&isp_heap_phys, &isp_heap_iova, &isp_heap_size)) &&
        dt_reserve_asc_firmware("/arm-io/isp", "/arm-io/isp0", "isp", false, isp_iova_base()))
        return -1;
    if (dt_set_isp_fwdata())
        return -1;
    if (dt_set_pmgr())
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

    /* Optional export goes last so it cannot consume space needed by fixups. */
    if (chip_id == T8122)
        dt_gpu_t8122_diagnostics(dt);

    /* Last, so that its values never take room the steps above need. */
    if (chip_id == T8122)
        dt_set_pmp_t8122();

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

int kboot_boot(void *kernel)
{
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
    ret = pcie_init();
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
    ret = chip_id == T8142 ? dapf_init_fdt(dt) : dapf_init_all();
    if (ret < 0 && chip_id == T8140) {
        printf("kboot: refusing T8140 handoff after DAPF failure\n");
        pcie_shutdown();
        return -1;
    }

    printf("Setting SMP mode to WFE...\n");
    smp_set_wfe_mode(true);
    if (chip_id == T8140) {
        int chosen = fdt_path_offset(dt, "/chosen");
        if (chosen < 0)
            return -1;
        if (fdt_getprop(dt, chosen, "apple,dcp-rtkit-quiesced", NULL)) {
            bool healthy = dt_t8140_validate_dcp_asc("/arm-io/dcp", "dcp", false);
            int ext = fdt_node_offset_by_compatible(dt, -1, "apple,t8140-dcpext");
            const char *status = ext < 0 ? NULL : fdt_getprop(dt, ext, "status", NULL);
            if (!healthy) {
                printf("kboot: DCP changed state before handoff; disabling display\n");
                if (dt_t8140_refuse_display() || fdt_pack(dt))
                    return -1;
            } else if (status && !strcmp(status, "okay") &&
                       (pmgr_power_is_on(0, "DISPEXT0_CPU") != 1 ||
                        !dt_t8140_validate_dcp_asc("/arm-io/dcpext", "dcpext", true))) {
                /* The internal panel does not depend on the external DCP. */
                printf("kboot: DCPEXT changed state before handoff; disabling it\n");
                if (dt_t8140_disable_dcpext() || fdt_pack(dt))
                    return -1;
            }
        }
    }
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
