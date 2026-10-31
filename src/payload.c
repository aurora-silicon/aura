/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"
#include "../build/build_tag.h"

#include "payload.h"
#include "adt.h"
#include "assert.h"
#include "boot_storage.h"
#include "chainload.h"
#include "cpufreq.h"
#include "display.h"
#include "firmware.h"
#include "heapblock.h"
#include "kboot.h"
#include "mitigations.h"
#include "nvme.h"
#include "payload_profile.h"
#include "smp.h"
#include "stage1_config.h"
#include "utils.h"

#include "libfdt/libfdt.h"
#include "minilzlib/minlzma.h"
#include "tinf/tinf.h"

// Kernels must be 2MB aligned
#define KERNEL_ALIGN (2 << 20)

static const u8 gz_magic[] = {0x1f, 0x8b};
static const u8 xz_magic[] = {0xfd, '7', 'z', 'X', 'Z', 0x00};
static const u8 fdt_magic[] = {0xd0, 0x0d, 0xfe, 0xed};
static const u8 kernel_magic[] = {'A', 'R', 'M', 0x64};          // at 0x38
static const u8 cpio_magic[] = {'0', '7', '0', '7', '0'};        // '1' or '2' next
static const u8 img4_magic[] = {0x16, 0x04, 'I', 'M', 'G', '4'}; // IA5String 'IMG4'
/*
 * aura's own payload records start "aura_". Payloads made for the bootloader aura replaced
 * start "m1n1_" instead; both are accepted so existing installs and payload builders keep
 * working. The two prefixes are the same length, so the record layout is unchanged.
 */
#define PAYLOAD_PREFIX_LEN 5
static const u8 payload_prefix[PAYLOAD_PREFIX_LEN] = {'a', 'u', 'r', 'a', '_'};
static const u8 legacy_payload_prefix[PAYLOAD_PREFIX_LEN] = {'m', '1', 'n', '1', '_'};
static const u8 sig_magic[] = {'s', 'i', 'g'};
static const u8 initramfs_magic[] = {'i', 'n', 'i', 't', 'r', 'a', 'm', 'f', 's'};
// followed by size as little endian uint32_t
static const u8 empty[] = {0, 0, 0, 0};

// custom logo is RGBA with 256x256 retina logo followed by half resolution
static const u8 custom_logo_magic[] = {'l', 'o', 'g', 'o', '_', '2', '5', '6', '1', '2', '8'};
#define CUSTOM_LOGO_SIZE (4 * ((256 * 256) + (128 * 128)))

static char expect_compatible[256];
static struct kernel_header *kernel = NULL;
static void *fdt = NULL;
static int fdt_rank;
static char *chainload_spec = NULL;
static char *boot_spec = NULL;
static bool payload_scanned = false;
#ifndef ESP_STAGE2
static bool stage1_config_applied = false;
static char stage1_esp_chosen[96];
#endif

static void *load_one_payload(void *start, size_t size);

bool stage1_chained;
#ifdef DEBUGUSB_PROXY
bool stage1_window_passed;
#endif

/* Length of the "aura_<tag>" (or legacy "m1n1_<tag>") record header at p, or 0. */
static size_t payload_record(const u8 *p, const u8 *tag, size_t tag_len)
{
    if (memcmp(p, payload_prefix, PAYLOAD_PREFIX_LEN) &&
        memcmp(p, legacy_payload_prefix, PAYLOAD_PREFIX_LEN))
        return 0;
    if (memcmp(p + PAYLOAD_PREFIX_LEN, tag, tag_len))
        return 0;
    return PAYLOAD_PREFIX_LEN + tag_len;
}

static void finalize_uncompression(void *dest, size_t dest_len)
{
    // Actually reserve the space. malloc is safe after this, but...
    assert(dest == heapblock_alloc_aligned(dest_len, KERNEL_ALIGN));

    void *end = ((u8 *)dest) + dest_len;
    void *next = load_one_payload(dest, dest_len);
    assert(!next || next >= dest);

    // If the payload needs padding, we need to reserve more, so it better have not used
    // malloc either.
    if (next > end) {
        // Explicitly *un*aligned or it'll fail this assert, since 64b alignment is the default
        assert(end == heapblock_alloc_aligned((u8 *)next - (u8 *)end, 1));
    }
}

static void *decompress_gz(void *p, size_t size)
{
    unsigned int source_len = size, dest_len = 1 << 30; // 1 GiB should be enough hopefully

    // Start at the end of the heap area, no allocation yet. The following code must not use
    // malloc or heapblock, until finalize_uncompression is called.
    void *dest = heapblock_alloc_aligned(0, KERNEL_ALIGN);

    printf("Uncompressing... ");
    int ret = tinf_gzip_uncompress(dest, &dest_len, p, &source_len);

    if (ret != TINF_OK) {
        printf("Error %d\n", ret);
        return NULL;
    }

    printf("%d bytes uncompressed to %d bytes\n", source_len, dest_len);

    finalize_uncompression(dest, dest_len);

    return ((u8 *)p) + source_len;
}

static void *decompress_xz(void *p, size_t size)
{
    uint32_t source_len = size, dest_len = 1 << 30; // 1 GiB should be enough hopefully

    // Start at the end of the heap area, no allocation yet. The following code must not use
    // malloc or heapblock, until finalize_uncompression is called.
    void *dest = heapblock_alloc_aligned(0, KERNEL_ALIGN);

    printf("Uncompressing... ");
    int ret = XzDecode(p, &source_len, dest, &dest_len);

    if (!ret) {
        printf("XZ decode failed\n");
        return NULL;
    }

    printf("%d bytes uncompressed to %d bytes\n", source_len, dest_len);

    finalize_uncompression(dest, dest_len);

    return ((u8 *)p) + source_len;
}

static void *load_fdt(void *p, size_t size)
{
    int rank = payload_profile_rank(p, os_firmware.version == V26_6_2);
    if (rank && rank >= fdt_rank && fdt_node_check_compatible(p, 0, expect_compatible) == 0) {
        printf("Found a devicetree for %s at %p\n", expect_compatible, p);
        fdt = p;
        fdt_rank = rank;
    }
    assert(!size || size == fdt_totalsize(p));
    return ((u8 *)p) + fdt_totalsize(p);
}

static void *load_cpio(void *p, size_t size)
{
    if (!size) {
        // We could handle this, but who uses uncompressed initramfs?
        printf("Uncompressed cpio archives not supported\n");
        return NULL;
    }

    kboot_set_initrd(p, size);
    return ((u8 *)p) + size;
}

static void *load_kernel(void *p, size_t size)
{
    kernel = p;

    assert(size <= kernel->image_size);

    // If this is an in-line kernel, it's probably not aligned, so we need to make a copy
    if (((u64)kernel) & (KERNEL_ALIGN - 1)) {
        void *new_addr = heapblock_alloc_aligned(kernel->image_size, KERNEL_ALIGN);
        memcpy(new_addr, kernel, size ? size : kernel->image_size);
        kernel = new_addr;
    }

    /*
     * Kernel blobs unfortunately do not have an accurate file size header, so
     * this will fail for in-line payloads. However, conversely, this is required for
     * compressed payloads, in order to allocate padding that the kernel needs, which will be
     * beyond the end of the compressed data. So if we know the input size, tell the caller
     * about the true image size; otherwise don't.
     */
    if (size) {
        return ((u8 *)p) + kernel->image_size;
    } else {
        return NULL;
    }
}

#define MAX_VAR_NAME 64
#define MAX_VAR_SIZE 1024

#define IS_VAR(x) !strncmp((char *)*p, x, strlen(x))

#define MAX_CHOSEN_VARS 16
/* Passed after every other line to a chainloaded stage when a config line was lost */
#define CONFIG_LOST_VAR "m1n1.config-lost=1"

/* The two extra slots carry CONFIG_LOST_VAR and nvme.adopt without reducing the chosen limit. */
#ifdef CHAINLOADING
static size_t chosen_cnt = 1;
static char *chosen[MAX_CHOSEN_VARS + 2] = {
    "chosen.asahi,m1n1-stage1-version=" BUILD_TAG,
};
#else
static size_t chosen_cnt = 0;
static char *chosen[MAX_CHOSEN_VARS + 2];
#endif

static bool enable_tso = false;

/*
 * A config line that cannot be kept or read: a chosen. line past the end of chosen[]; a line of a
 * variable m1n1 knows that has no newline or a name or value too long (the scan stops there), or
 * that a line without '=' took into its name; or a stop at unknown data after the kernel or a
 * config line, before config that may follow. A chainloading stage 1 that lost one says so with
 * CONFIG_LOST_VAR. The value the config set before the lost line still stands, an owner's
 * chosen.<switch>=1 against a lost =0 perhaps, so on the Macs with M3 graphics defaults kboot
 * then hands no graphics over (kboot_set_config_lost()). Everything else boots as before.
 */
static bool config_lost = false;
static bool config_seen = false;
static bool config_lost_passed = false;

static const char *const config_names[] = {
    "chosen.",  "chainload=",   "boot=", "nvme.adopt=",
    "display=", "mitigations=", "tso=",  "m1n1.config-lost="};

/* The start of a line of a variable m1n1 knows */
static bool config_line(const char *s)
{
    for (size_t i = 0; i < ARRAY_SIZE(config_names); i++)
        if (!strncmp(s, config_names[i], strlen(config_names[i])))
            return true;
    return false;
}

/* check_var() cannot read this line: lost if it is one of m1n1's */
static bool config_unreadable(u8 *p)
{
    if (config_line((char *)p))
        config_lost = true;
    return false;
}

static bool check_var(u8 **p)
{
    char *val = memchr(*p, '=', strnlen((char *)*p, MAX_VAR_NAME + 1));
    if (!val)
        return config_unreadable(*p);
    /* A line without '=' took the lines after it into this name */
    for (char *nl = memchr(*p, '\n', val - (char *)*p); nl; nl = memchr(nl + 1, '\n', val - nl - 1))
        if (config_line(nl + 1))
            config_lost = true;

    val++;

    char *end = memchr(val, '\n', strnlen(val, MAX_VAR_SIZE + 1));
    if (!end)
        return config_unreadable(*p);

    *end = 0;
    printf("Found variable %.*s\n", (int)(val - (char *)*p - 1), (char *)*p);

    if (IS_VAR("chosen.")) {
        if (chosen_cnt >= MAX_CHOSEN_VARS) {
            printf("Too many chosen vars, ignoring %.*s\n", (int)(val - (char *)*p - 1),
                   (char *)*p);
            config_lost = true;
        } else {
            chosen[chosen_cnt++] = (char *)*p;
        }
    } else if (IS_VAR("m1n1.config-lost=")) {
        /* Any value: an earlier stage lost a line, and nothing after it can take that back */
        config_lost = true;
    } else if (IS_VAR("chainload=")) {
        chainload_spec = val;
    } else if (IS_VAR("boot=")) {
        if (!boot_spec)
            boot_spec = val;
    } else if (IS_VAR("nvme.adopt=")) {
        if (!strcmp(val, "live-rtkit-v1")) {
            nvme_adopt_live_session = true;
            /* Started by an earlier aura stage, which has had its host window already. */
            stage1_chained = true;
        }
#ifdef DEBUGUSB_PROXY
    } else if (IS_VAR("aura.host-window=")) {
        stage1_window_passed = true;
#endif
    } else if (IS_VAR("display=")) {
        display_configure(val);
    } else if (IS_VAR("display2=")) {
        display_configure_second(val);
    } else if (IS_VAR("mitigations=")) {
        mitigations_configure(val);
    } else if (IS_VAR("tso=")) {
        enable_tso = val[0] == '1';
    } else {
        printf("Unknown variable %.*s\n", (int)(val - (char *)*p - 1), (char *)*p);
    }

    *p = (u8 *)(end + 1);
    config_seen = true;
    return true;
}

static void *load_one_payload(void *start, size_t size)
{
    u8 *p = start;
    size_t hdr;

    if (!start)
        return NULL;

    if (!memcmp(p, gz_magic, sizeof gz_magic)) {
        printf("Found a gzip compressed payload at %p\n", p);
        return decompress_gz(p, size);
    } else if (!memcmp(p, xz_magic, sizeof xz_magic)) {
        printf("Found an XZ compressed payload at %p\n", p);
        return decompress_xz(p, size);
    } else if (!memcmp(p, fdt_magic, sizeof fdt_magic)) {
        return load_fdt(p, size);
    } else if (!memcmp(p, cpio_magic, sizeof cpio_magic)) {
        printf("Found a cpio initramfs at %p\n", p);
        return load_cpio(p, size);
    } else if (!memcmp(p + 0x38, kernel_magic, sizeof kernel_magic)) {
        printf("Found a kernel at %p\n", p);
        return load_kernel(p, size);
    } else if ((hdr = payload_record(p, sig_magic, sizeof sig_magic))) {
        u32 size;
        memcpy(&size, p + hdr, 4);

        printf("Found a signature at %p, skipping 0x%x bytes\n", p, size);
        return p + size;
    } else if ((hdr = payload_record(p, initramfs_magic, sizeof initramfs_magic))) {
        u32 size;
        memcpy(&size, p + hdr, 4);
        printf("Found an initramfs payload at %p, 0x%x bytes\n", p, size);
        p += hdr + 4;
        return load_cpio(p, size);
    } else if ((hdr = payload_record(p, custom_logo_magic, sizeof custom_logo_magic))) {
        printf("Found a custom logo payload at %p, skipping 0x%lx bytes\n", p,
               hdr + CUSTOM_LOGO_SIZE);
        return p + hdr + CUSTOM_LOGO_SIZE;
    } else if (!memcmp(p, empty, sizeof empty) ||
               !memcmp(p + 0x05, img4_magic, sizeof img4_magic)) { // SEPFW after aura
        /* Before check_var(): an end whose bytes also read as a line is still the end */
        printf("No more payloads at %p\n", p);
        return NULL;
    } else if (check_var(&p)) {
        return p;
    } else {
        /* After the kernel or a config line, more config may follow (update-m1n1 puts it last) */
        if (kernel || config_seen)
            config_lost = true;
        printf("Unknown payload at %p (magic: %02x%02x%02x%02x)\n", p, p[0], p[1], p[2], p[3]);
        return NULL;
    }
}

void do_enable_tso(void)
{
    u64 actlr = mrs(ACTLR_EL1);
    actlr |= BIT(1); // Enable TSO
    msr(ACTLR_EL1, actlr);
}

/*
 * Set by start.S when this stage moved itself above the chainloaded image, or
 * down to the run base stage 1 put in its load header (chainload.h): the
 * payload is still after the image at the old base. In .data, because start.S
 * writes it before .bss is cleared.
 */
u64 relocated_from __attribute__((section(".data"))) = 0;

/*
 * Read by start.S before anything else runs: only an ESP stage 2 moves, and only on M3 (start.S
 * checks the CPU part), where a stage 1 chainloads it to stage 1's own base.
 */
#ifdef ESP_STAGE2
const u32 stage2_relocates = 1;
#else
const u32 stage2_relocates = 0;
#endif

static void *payload_start(void)
{
    if (relocated_from)
        return (void *)(relocated_from + (_payload_start - _base));
    return _payload_start;
}

bool payload_logo(void **custom_128, void **custom_256)
{
    void *p = payload_start();

    *custom_128 = NULL;
    *custom_256 = NULL;

    size_t hdr = payload_record(p, custom_logo_magic, sizeof custom_logo_magic);
    if (hdr) {
        void *data = p + hdr;
        printf("Found a custom logo payload at %p\n", p);
        *custom_256 = data;
        *custom_128 = data + 4 * 256 * 256;

        return true;
    }

    return false;
}

int payload_run(void)
{
    const char *target = adt_getprop(adt, 0, "target-type", NULL);
    if (target) {
        strcpy(expect_compatible, "apple,");
        char *p = expect_compatible + strlen(expect_compatible);
        while (*target && p != expect_compatible + sizeof(expect_compatible) - 1) {
            *p++ = tolower(*target++);
        }
        *p = 0;
        printf("Devicetree compatible value: %s\n", expect_compatible);
    } else {
        printf("Cannot find target type! %p %p\n", target, adt);
        return -1;
    }

    if (!payload_scanned) {
        void *p = payload_start();
        cpufreq_payload_boost(true);
        while (p)
            p = load_one_payload(p, 0);
        cpufreq_payload_boost(false);
        payload_scanned = true;
    }

#ifndef ESP_STAGE2
    if (!stage1_config_applied) {
        const char *target = stage1_config_target();
        if (target) {
            if (chainload_spec && strcmp(chainload_spec, target)) {
                printf("Payload: embedded and appended chainload targets differ\n");
                return -1;
            }
            chainload_spec = (char *)target;
            if (chosen_cnt >= MAX_CHOSEN_VARS)
                return -1;
            snprintf(stage1_esp_chosen, sizeof(stage1_esp_chosen),
                     "chosen.asahi,efi-system-partition=%s", stage1_config_esp_uuid());
            chosen[chosen_cnt++] = stage1_esp_chosen;
        }
        stage1_config_applied = true;
    }
#endif

#ifdef DEBUGUSB_PROXY
    /* Before NVMe is touched: a host that takes the window finds it as the firmware left it. */
    if (debugusb_proxy_window())
        return next_stage.entry ? 0 : -1;
#endif

    if (chainload_spec && boot_spec) {
        printf("Payload: cannot combine boot= and chainload=\n");
        next_stage.entry = NULL;
        return -1;
    }

    if (chainload_spec) {
        next_stage.entry = NULL;
        /* Last, after every owner line, in a slot of its own: the next stage cannot miss it */
        if (config_lost && !config_lost_passed) {
            printf("Payload: a config line was lost: telling the next stage\n");
            chosen[chosen_cnt++] = CONFIG_LOST_VAR;
            config_lost_passed = true;
        }
        int ret = chainload_load(chainload_spec, chosen, &chosen_cnt, ARRAY_SIZE(chosen));
        if (ret) {
            next_stage.entry = NULL;
            nvme_shutdown();
        }
        return ret;
    }

    if (boot_spec) {
        cpufreq_payload_boost(true);
        int ret = boot_storage_load(boot_spec, &kernel, &fdt);
        cpufreq_payload_boost(false);
        if (ret) {
            next_stage.entry = NULL;
            return -1;
        }
    }

    if (kernel && fdt) {
        /* The shared post-M4 U-Boot driver adopts ANS instead of starting it.
         * An inline ESP payload has not gone through boot_storage_load(). */
        if (chip_id == T8132 && !fdt_node_check_compatible(fdt, 0, "apple,j713") && !nvme_init()) {
            printf("Payload: cannot prepare the M4 ANS session\n");
            goto boot_failed;
        }
        cpufreq_init();
        if (smp_start_secondaries() && chip_id == T8140) {
            printf("SMP: refusing payload handoff with missing T8140 CPUs\n");
            goto boot_failed;
        }
        int mitigation_ret = mitigations_perform();
        if (mitigation_ret) {
            if (mitigation_ret == SMP_TIMEOUT || chip_id == T8140)
                goto boot_failed;
            printf("Mitigations: continuing payload handoff with incomplete CPU initialization\n");
        }
        if (enable_tso) {
            kboot_delete_chosen("apple,tso");
            bool tso_failed = false;
            do_enable_tso();
            for (int i = 0; i < MAX_CPUS; i++) {
                if (i == boot_cpu_idx)
                    continue;
                if (smp_is_alive(i)) {
                    int ret = smp_call_sync0(i, do_enable_tso);
                    if (ret) {
                        printf("TSO: CPU %d initialization failed\n", i);
                        if (ret == SMP_TIMEOUT || chip_id == T8140)
                            goto boot_failed;
                        tso_failed = true;
                    }
                }
            }
            if (!tso_failed)
                kboot_set_chosen("apple,tso", "");
            else
                printf("TSO: continuing payload handoff without advertising complete TSO setup\n");
        }

        for (size_t i = 0; i < chosen_cnt; i++) {
            char *val = memchr(chosen[i], '=', MAX_VAR_NAME + 1);
            char var[MAX_VAR_NAME + 1];

            assert(val);
            memcpy(var, chosen[i], val - chosen[i]);
            var[val - chosen[i]] = 0; // Terminate var name
            if (kboot_set_chosen(var + 7, val + 1) < 0)
                printf("Failed to kboot set %s\n", var + 7);
        }

        if (config_lost)
            kboot_set_config_lost();
        if (kboot_prepare_dt(fdt)) {
            printf("Failed to prepare FDT!\n");
            goto boot_failed;
        }

        if (kboot_boot(kernel))
            goto boot_failed;
        if (boot_spec)
            nvme_keep_running_for_linux = true;
        return 0;

    boot_failed:
        next_stage.entry = NULL;
        if (boot_spec) {
            kboot_set_initrd(NULL, 0);
            nvme_keep_running_for_linux = false;
            nvme_shutdown();
        }
        return -1;
    } else if (kernel && !fdt) {
        printf("ERROR: Kernel found but no devicetree for %s available.\n", expect_compatible);
    } else if (!kernel && fdt) {
        printf("ERROR: Devicetree found but no kernel.\n");
    }

    return -1;
}

int payload_boot_storage(const char *spec)
{
    if (!spec)
        return -1;
    boot_spec = (char *)spec;
    return payload_run();
}
