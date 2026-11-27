/* SPDX-License-Identifier: MIT */
/*
 * Host driver for src/sptm_virt_esp_plan.c and src/sha256.c (tests/python/test_esp_macos.py).
 *
 *   esp_plan_host sha256 <file>
 *   esp_plan_host plan <manifest> <adt> <params> <outdir>
 *
 * params: one "key value..." per line (dram_base, phys_base, mem_size, mem_size_actual,
 * tc_container, video <6 words>, file <id> <len>, copy <name> <pa> <size>, reserved <pa> <size>).
 * outdir gets adt.bin, boot_args.bin, regions.txt, loads.txt and info.txt.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/sptm_virt_esp_plan.h"

static void *slurp(const char *path, u64 *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc(*len + 1);
    if (fread(b, 1, *len, f) != *len)
        exit(2);
    b[*len] = 0;
    fclose(f);
    return b;
}

static void dump(const char *dir, const char *name, const void *p, u64 len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, len, f) != len)
        exit(2);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "sha256")) {
        u64 n;
        void *b = slurp(argv[2], &n);
        u8 h[32];
        sha256(b, n, h);
        for (int i = 0; i < 32; i++)
            printf("%02x", h[i]);
        printf("\n");
        return 0;
    }
    if (argc != 6 || strcmp(argv[1], "plan"))
        return 2;
    static struct esp_manifest m;
    static struct esp_plan pl;
    static struct esp_plan_in in;
    char err[256] = {0};
    u64 mlen, alen, plen;
    char *mt = slurp(argv[2], &mlen);
    u8 *adt = slurp(argv[3], &alen);
    char *params = slurp(argv[4], &plen);
    if (esp_manifest_parse(&m, mt, mlen, err, sizeof(err))) {
        printf("ERR %s\n", err);
        return 1;
    }
    static const char *const ids[ESP_NIMG] = {"sptm", "txm", "xnu", "tc", "cl4", "exic", "extc"};
    in.m = &m;
    in.adt = adt;
    in.adt_len = alen;
    for (char *l = strtok(params, "\n"); l; l = strtok(NULL, "\n")) {
        char k[32], a[32];
        unsigned long long x, y, v6[6];
        if (sscanf(l, "dram_base %llx", &x) == 1)
            in.dram_base = x;
        else if (sscanf(l, "phys_base %llx", &x) == 1)
            in.phys_base = x;
        else if (sscanf(l, "mem_size_actual %llx", &x) == 1)
            in.mem_size_actual = x;
        else if (sscanf(l, "mem_size %llx", &x) == 1)
            in.mem_size = x;
        else if (sscanf(l, "tc_container %llx", &x) == 1)
            in.tc_container = x;
        else if (sscanf(l, "video %llx %llx %llx %llx %llx %llx", &v6[0], &v6[1], &v6[2], &v6[3],
                        &v6[4], &v6[5]) == 6)
            for (int i = 0; i < 6; i++)
                in.video[i] = v6[i];
        else if (sscanf(l, "file %31s %llx", a, &x) == 2) {
            for (int i = 0; i < ESP_NIMG; i++)
                if (!strcmp(a, ids[i]))
                    in.file_len[i] = x;
        } else if (sscanf(l, "copy %15s %llx %llx", a, &x, &y) == 3) {
            strcpy(in.copies[in.ncopies].name, a);
            in.copies[in.ncopies].pa = x;
            in.copies[in.ncopies++].size = y;
        } else if (sscanf(l, "reserved %llx %llx", &x, &y) == 2) {
            in.reserved[in.nreserved][0] = x;
            in.reserved[in.nreserved++][1] = y;
        } else if (sscanf(l, "%31s", k) == 1) {
            printf("ERR unknown param %s\n", k);
            return 1;
        }
    }
    u64 cap = (alen + 0x2000 + 0x3fff) & ~0x3fffULL;
    u8 *out = malloc(cap);
    if (esp_plan(&in, &pl, out, cap, err, sizeof(err))) {
        printf("ERR %s\n", err);
        return 1;
    }
    const char *d = argv[5];
    dump(d, "adt.bin", out, pl.adt_len);
    dump(d, "boot_args.bin", pl.boot_args, sizeof(pl.boot_args));
    char path[512];
    snprintf(path, sizeof(path), "%s/regions.txt", d);
    FILE *f = fopen(path, "w");
    for (u32 i = 0; i < pl.nregion; i++)
        fprintf(f, "%s %llx %llx\n", pl.region[i].name, (unsigned long long)pl.region[i].pa,
                (unsigned long long)pl.region[i].size);
    fclose(f);
    snprintf(path, sizeof(path), "%s/loads.txt", d);
    f = fopen(path, "w");
    for (u32 i = 0; i < pl.nload; i++) {
        const struct esp_load *l = &pl.load[i];
        fprintf(f, "%llx %s %llx %llx %llx %d\n", (unsigned long long)l->pa,
                l->img == ESP_NIMG ? "copy" : ids[l->img], (unsigned long long)l->fileoff,
                (unsigned long long)l->size, (unsigned long long)l->total, l->tc_header);
    }
    fclose(f);
    snprintf(path, sizeof(path), "%s/info.txt", d);
    f = fopen(path, "w");
    fprintf(f,
            "adt_pa %llx\nsptm_pa %llx\nsptm_entry_pa %llx\ntxm_pa %llx\nboot_args_pa "
            "%llx\nblob_end %llx\nvirt_base %llx\n",
            (unsigned long long)pl.adt_pa, (unsigned long long)pl.sptm_pa,
            (unsigned long long)pl.sptm_entry_pa, (unsigned long long)pl.txm_pa,
            (unsigned long long)pl.boot_args_pa, (unsigned long long)pl.blob_end,
            (unsigned long long)pl.virt_base);
    fclose(f);
    return 0;
}
