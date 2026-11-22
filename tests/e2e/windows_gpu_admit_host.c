/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Host driver for src/hv/windows/gpu_t8152_admit.c (G0 admission, range table, handoff page).
 * Driven by tests/python/test_windows_t8152_gpu.py with a synthetic binary ADT, synthetic TEXT
 * blobs and a synthetic profile; prints what the admission lifted, the holes, the class table
 * and the handoff page so the test can compare them with its own model.
 *
 *   windows_gpu_admit_host <config>
 *
 * Config lines: "chip N", "board N", "ram BASE TOP", "receipt S", "adt PATH",
 * "text R PHYS PATH", "profile R UUID TEXT_SIZE PATCHBAY DIGEST_HEX", "pool BASE SIZE",
 * "handoff IPA", "out PATH", "cap N" (range table capacity).
 */
#include <inttypes.h>

#include "windows_gpu_adt_standin.h"

#include "gpu_t8152_admit.h"
#include "utils.h"

#include "tlvs.inc"

#include "windows_gpu_sha256.h"

/* ---- synthetic TEXT behind read_text ----------------------------------------------------- */
static struct {
    u64 phys, size;
    u8 *data;
} texts[2];
static int text_reads;

static bool host_read_text(u64 phys, void *buf, u64 size)
{
    text_reads++;
    for (int i = 0; i < 2; i++)
        if (texts[i].data && phys >= texts[i].phys && size <= texts[i].size &&
            phys - texts[i].phys <= texts[i].size - size) {
            memcpy(buf, texts[i].data + (phys - texts[i].phys), size);
            return true;
        }
    return false;
}

static u8 *slurp(const char *path, u64 *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *size = (u64)ftell(f);
    fseek(f, 0, SEEK_SET);
    u8 *buf = malloc(*size ? *size : 1);
    if (fread(buf, 1, *size, f) != *size)
        exit(2);
    fclose(f);
    return buf;
}

static char uuids[2][64], receipt[80], outpath[512];
static struct gpu_profile profiles[2];

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    FILE *cfg = fopen(argv[1], "r");
    if (!cfg)
        return 2;
    struct gpu_t8152_env env = {0};
    u64 pool_base = 0, pool_size = 0, handoff_ipa = 0;
    u32 cap = GPU_T8152_MAX_RANGES;
    char line[1024];
    while (fgets(line, sizeof(line), cfg)) {
        char key[32], a[512], b[512], c[128], d[128];
        unsigned long long x, y;
        int r;
        if (sscanf(line, "%31s", key) != 1)
            continue;
        if (!strcmp(key, "chip") && sscanf(line, "%*s %lli", &x) == 1)
            env.chip_id = (u32)x;
        else if (!strcmp(key, "board") && sscanf(line, "%*s %lli", &x) == 1)
            env.board_id = (u32)x;
        else if (!strcmp(key, "ram") && sscanf(line, "%*s %lli %lli", &x, &y) == 2) {
            env.ram_base = x;
            env.ram_top = y;
        } else if (!strcmp(key, "receipt") && sscanf(line, "%*s %79s", receipt) == 1)
            env.receipt = receipt;
        else if (!strcmp(key, "adt") && sscanf(line, "%*s %511s", a) == 1)
            standin_load_adt(a);
        else if (!strcmp(key, "text") && sscanf(line, "%*s %d %lli %511s", &r, &x, a) == 3) {
            texts[r].phys = x;
            texts[r].data = slurp(a, &texts[r].size);
        } else if (!strcmp(key, "profile") &&
                   sscanf(line, "%*s %d %63s %127s %127s %511s", &r, b, c, d, a) == 5) {
            strcpy(uuids[r], b);
            profiles[r].role = r ? "gfx1-asc" : "gfx-asc";
            profiles[r].uuid = uuids[r];
            profiles[r].text_size = (u32)strtoull(c, NULL, 0);
            profiles[r].patchbay = (u32)strtoull(d, NULL, 0);
            for (int i = 0; i < 32; i++) {
                unsigned v;
                sscanf(a + 2 * i, "%2x", &v);
                profiles[r].digest[i] = (u8)v;
            }
        } else if (!strcmp(key, "pool") && sscanf(line, "%*s %lli %lli", &x, &y) == 2) {
            pool_base = x;
            pool_size = y;
        } else if (!strcmp(key, "handoff") && sscanf(line, "%*s %lli", &x) == 1)
            handoff_ipa = x;
        else if (!strcmp(key, "out") && sscanf(line, "%*s %511s", outpath) == 1)
            ;
        else if (!strcmp(key, "cap") && sscanf(line, "%*s %lli", &x) == 1)
            cap = (u32)x;
    }
    fclose(cfg);
    env.profiles = profiles;
    env.tlvs = tlvs;
    env.tlv_count = ARRAY_SIZE(tlvs);
    env.read_text = host_read_text;
    env.sha256 = host_sha256;

    struct gpu_t8152_admission adm;
    memset(&adm, 0xa5, sizeof(adm));
    int rc = gpu_t8152_admit(&env, &adm);
    printf("A rc %d\n", rc);
    if (rc) {
        /* F12: a refusal leaves nothing behind that could be mistaken for an admission. */
        static const struct gpu_t8152_admission zero;
        printf("A zeroed %d\n", !memcmp(&adm, &zero, sizeof(adm)));
        return 0;
    }
    printf("A board 0x%x chip 0x%x\n", adm.board_id, adm.chip_id);
    printf("A dram 0x%" PRIx64 " 0x%" PRIx64 "\n", (uint64_t)adm.dram_base,
           (uint64_t)adm.dram_size);
    printf("A vm 0x%" PRIx64 "\n", (uint64_t)adm.vm_base);
    for (int i = 0; i < GPU_T8152_REGIONS; i++)
        printf("A region %d 0x%" PRIx64 " 0x%" PRIx64 "\n", i, (uint64_t)adm.region[i].base,
               (uint64_t)adm.region[i].size);
    for (int r = 0; r < 2; r++) {
        printf("A text %d 0x%" PRIx64 " 0x%" PRIx64 "\n", r, (uint64_t)adm.text[r].base,
               (uint64_t)adm.text[r].size);
        printf("A wrapper %d 0x%" PRIx64 " 0x%" PRIx64 "\n", r, (uint64_t)adm.wrapper[r].base,
               (uint64_t)adm.wrapper[r].size);
        printf("A vbar %d 0x%" PRIx64 " 0x%" PRIx64 "\n", r, (uint64_t)adm.vbar[r].base,
               (uint64_t)adm.vbar[r].size);
        printf("A mbnum %d %u\n", r, adm.mailbox_num[r]);
        printf("A irq %d %u %u\n", r, adm.irq[r][0], adm.irq[r][1]);
        printf("A uuid %d %s\n", r, adm.uuid[r]);
    }
    printf("A sgx 0x%" PRIx64 " 0x%" PRIx64 "\n", (uint64_t)adm.sgx.base, (uint64_t)adm.sgx.size);
    printf("A sgx1 0x%" PRIx64 " 0x%" PRIx64 "\n", (uint64_t)adm.sgx1.base,
           (uint64_t)adm.sgx1.size);
    printf("A mailbox 0x%" PRIx64 " 0x%" PRIx64 "\n", (uint64_t)adm.mailbox.base,
           (uint64_t)adm.mailbox.size);
    printf("A perf %u\n", adm.perf_count);
    printf("A receipt %s\n", adm.receipt);
    printf("A text_reads %d\n", text_reads);

    struct gpu_range holes[GPU_T8152_MAX_HOLES];
    int nh = gpu_t8152_collect_holes(&adm, holes, GPU_T8152_MAX_HOLES);
    printf("H count %d\n", nh);
    for (int i = 0; i < nh; i++)
        printf("H %d 0x%" PRIx64 " 0x%" PRIx64 " %s\n", i, (uint64_t)holes[i].base,
               (uint64_t)holes[i].size, holes[i].name);
    if (nh < 0)
        return 0;

    struct gpu_range *table = calloc(cap ? cap : 1, sizeof(*table));
    int n = gpu_t8152_build_ranges(&adm, holes, (u32)nh, table, cap);
    printf("R count %d\n", n);
    if (n < 0)
        return 0;
    u32 count = (u32)n;
    if (pool_size)
        printf("R pool %d\n",
               gpu_t8152_add_range(table, &count, cap, pool_base, pool_size, GPU_C_POOL, "pool"));
    if (handoff_ipa)
        printf("R handoff %d\n", gpu_t8152_add_range(table, &count, cap, handoff_ipa,
                                                       GPU_HANDOFF_SIZE, GPU_C_HANDOFF, "handoff"));
    for (u32 i = 0; i < count; i++)
        printf("R %u 0x%" PRIx64 " 0x%" PRIx64 " %u %u %s\n", i, (uint64_t)table[i].base,
               (uint64_t)table[i].size, table[i].cls, table[i].tag,
               gpu_class_name(table[i].cls));

    /* Lookups: every range's first and last byte, and the GFX mailbox windows. */
    for (u32 i = 0; i < count; i++) {
        const struct gpu_range *f = gpu_t8152_find(table, count, table[i].base);
        const struct gpu_range *l = gpu_t8152_find(table, count, table[i].base + table[i].size - 1);
        if (f != &table[i] || l != &table[i])
            printf("F lookup-miss %u\n", i);
    }
    for (u64 off = 0; off < 0x400; off += 0x40) {
        u32 role = 9;
        bool in = gpu_t8152_mailbox_window(&adm, adm.mailbox.base + off, &role);
        printf("M 0x%" PRIx64 " %d %u\n", (uint64_t)off, in, in ? role : 9);
    }

    if (outpath[0]) {
        struct gpu_t8152_handoff_in in = {.state = GPU_HS_ADMITTED | GPU_HS_HOOKS_LOG,
                                          .handoff_ipa = handoff_ipa,
                                          .pool_base = pool_base,
                                          .pool_size = pool_size};
        u8 *page = calloc(1, GPU_HANDOFF_SIZE);
        u32 used = gpu_t8152_handoff_build(page, GPU_HANDOFF_SIZE, &adm, table, count, &in);
        printf("P used %u\n", used);
        printf("P small %u\n", gpu_t8152_handoff_build(page + 0x2000, 0x200, &adm, table, count,
                                                       &in));
        FILE *f = fopen(outpath, "wb");
        fwrite(page, 1, GPU_HANDOFF_SIZE, f);
        fclose(f);
    }
    return 0;
}
