/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../src/kboot_gpu_t8122_power.h"
#include "../src/libfdt/libfdt.h"

struct fixture {
    struct t8122_powered_identity raw;
    int read_error;
    unsigned reads;
};

static int read_identity(void *context, u32 *id, u32 masks[3], u32 *counts, u32 *counts2)
{
    struct fixture *f = context;
    f->reads++;
    *id = f->raw.id_version;
    memcpy(masks, f->raw.core_mask, sizeof(f->raw.core_mask));
    *counts = f->raw.id_counts_1;
    *counts2 = f->raw.id_counts_2;
    return f->read_error;
}

static void tree(void *dt, int size, const char *compatible, const char *intent)
{
    assert(!fdt_create_empty_tree(dt, size));
    assert(!fdt_setprop(dt, 0, "compatible", compatible, strlen(compatible) + 1));
    if (!strcmp(compatible, "apple,j613")) {
        static const char compat[] = "apple,j613\0apple,t8122";
        assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    } else if (!strcmp(compatible, "apple,j615")) {
        static const char compat[] = "apple,j615\0apple,t8122";
        assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    } else if (!strcmp(compatible, "apple,j504")) {
        static const char compat[] = "apple,j504\0apple,t8122";
        assert(!fdt_setprop(dt, 0, "compatible", compat, sizeof(compat)));
    }
    int chosen = fdt_add_subnode(dt, 0, "chosen");
    assert(chosen >= 0);
    if (intent)
        assert(!fdt_setprop_string(dt, chosen, "asahi,t8122-gpu", intent));
}

static u32 property(void *dt, const char *name)
{
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    assert(node >= 0);
    int len;
    const fdt32_t *value = fdt_getprop(dt, node, name, &len);
    assert(value && len == 4);
    return fdt32_ld(value);
}

static void check_fields(void *dt, const struct t8122_powered_identity *raw)
{
    assert(property(dt, "schema-version") == 1);
    assert(property(dt, "probe-result") == 1);
    assert(property(dt, "id-version") == raw->id_version);
    assert(property(dt, "core-mask-0") == raw->core_mask[0]);
    assert(property(dt, "core-mask-1") == raw->core_mask[1]);
    assert(property(dt, "core-mask-2") == raw->core_mask[2]);
    assert(property(dt, "id-counts-1") == raw->id_counts_1);
    assert(property(dt, "id-counts-2") == raw->id_counts_2);
    int node = fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity");
    const u8 *id = fdt_getprop(dt, node, "id-version", NULL);
    assert(id[0] == 7 && id[1] == 2 && id[2] == 0x20 && id[3] == 0);
}

static const struct fixture supported = {
    .raw = {.id_version = 0x07022000,
            .core_mask = {0x3ff, 0, 0},
            .id_counts_1 = 0x0011010a,
            .id_counts_2 = 0x00040404},
};

static void rejected(struct fixture f)
{
    char dt[2048];
    tree(dt, sizeof(dt), "apple,j613", "1");
    assert(t8122_full_identity_prepare(dt) >= 0);
    struct t8122_powered_identity identity;
    assert(t8122_full_identity_read(read_identity, &f, &identity) < 0);
    assert(f.reads == 1);
    assert(t8122_full_identity_publish(dt, &identity) < 0);
    assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
}

int main(int argc, char **argv)
{
    char dt[2048];
    const char *intents[] = {NULL, "0", "1x", ""};
    for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
        tree(dt, sizeof(dt), "apple,j613", intents[i]);
        assert(t8122_full_identity_prepare(dt) < 0);
        assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity") < 0);
    }
    tree(dt, sizeof(dt), "apple,j713", "1");
    assert(t8122_full_identity_prepare(dt) < 0);
    tree(dt, sizeof(dt), "apple,t6030", "1");
    assert(t8122_full_identity_prepare(dt) < 0);

    /* The 15-inch Air uses its own measured mask, never a J613 mask constant. */
    const u32 j615_masks[] = {0x3ff, 0x37e, 0xff};
    for (unsigned i = 0; i < sizeof(j615_masks) / sizeof(j615_masks[0]); i++) {
        struct fixture f = supported;
        f.raw.core_mask[0] = j615_masks[i];
        tree(dt, sizeof(dt), "apple,j615", "1");
        assert(t8122_full_identity_prepare(dt) >= 0);
        assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
        struct t8122_powered_identity identity;
        assert(!t8122_full_identity_read(read_identity, &f, &identity));
        assert(!t8122_full_identity_publish(dt, &identity));
        check_fields(dt, &f.raw);
        t8122_full_identity_invalidate(dt);
        assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
    }
    for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
        tree(dt, sizeof(dt), "apple,j615", intents[i]);
        assert(t8122_full_identity_prepare(dt) < 0);
    }
    tree(dt, sizeof(dt), "apple,j615", "1");
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j615"));
    assert(t8122_full_identity_prepare(dt) < 0); /* Exact chip is mandatory. */

    /* The 14-inch M3 MacBook Pro (J504) takes the same path with its own measured mask. */
    for (unsigned i = 0; i < sizeof(j615_masks) / sizeof(j615_masks[0]); i++) {
        struct fixture f = supported;
        f.raw.core_mask[0] = j615_masks[i];
        tree(dt, sizeof(dt), "apple,j504", "1");
        assert(t8122_full_identity_prepare(dt) >= 0);
        assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
        struct t8122_powered_identity identity;
        assert(!t8122_full_identity_read(read_identity, &f, &identity));
        assert(!t8122_full_identity_publish(dt, &identity));
        check_fields(dt, &f.raw);
        t8122_full_identity_invalidate(dt);
    }
    for (unsigned i = 0; i < sizeof(intents) / sizeof(intents[0]); i++) {
        tree(dt, sizeof(dt), "apple,j504", intents[i]);
        assert(t8122_full_identity_prepare(dt) < 0);
    }
    tree(dt, sizeof(dt), "apple,j504", "1");
    assert(!fdt_setprop_string(dt, 0, "compatible", "apple,j504"));
    assert(t8122_full_identity_prepare(dt) < 0); /* Exact chip is mandatory. */
    /* Other base-M3 boards (the J433/J434 iMacs) stay refused. */
    tree(dt, sizeof(dt), "apple,j433", "1");
    static const char j433[] = "apple,j433\0apple,t8122";
    assert(!fdt_setprop(dt, 0, "compatible", j433, sizeof(j433)));
    assert(t8122_full_identity_prepare(dt) < 0);

    const u32 masks[] = {0x3ff, 0x37e, 0xff};
    for (unsigned i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        struct fixture f = supported;
        f.raw.core_mask[0] = masks[i];
        tree(dt, sizeof(dt), "apple,j613", "1");
        assert(t8122_full_identity_prepare(dt) >= 0);
        assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
        assert(t8122_full_identity_prepare(dt) < 0); /* Never replace an input witness. */
        struct t8122_powered_identity identity;
        assert(!t8122_full_identity_read(read_identity, &f, &identity));
        assert(f.reads == 1);
        /* Other setup writes move node offsets before publication. */
        assert(fdt_add_subnode(dt, 0, "later-setup-node") >= 0);
        assert(!t8122_full_identity_publish(dt, &identity));
        check_fields(dt, &f.raw);
        if (argc == 2 && i < 2) {
            char path[1024];
            assert(snprintf(path, sizeof(path), "%s/identity-%u.dtb", argv[1], i) > 0);
            FILE *output = fopen(path, "wb");
            assert(output);
            assert(fwrite(dt, fdt_totalsize(dt), 1, output) == 1);
            assert(!fclose(output));
        }
        t8122_full_identity_invalidate(dt);
        assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);
    }

    struct fixture f = supported;
    f.read_error = -1;
    rejected(f);
    const u32 ids[] = {0, 0x08022000, 0x07032000, 0x07022100, 0xffffffff};
    for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        f = supported;
        f.raw.id_version = ids[i];
        rejected(f);
    }
    const u32 counts[] = {0, 0x0012010a, 0x0011020a, 0x00110108, 0xffffffff};
    for (unsigned i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        f = supported;
        f.raw.id_counts_1 = counts[i];
        rejected(f);
    }
    const u32 invalid_masks[] = {0, 1, 0x1ff, 0x7ff};
    for (unsigned i = 0; i < sizeof(invalid_masks) / sizeof(invalid_masks[0]); i++) {
        f = supported;
        f.raw.core_mask[0] = invalid_masks[i];
        rejected(f);
    }
    for (unsigned i = 1; i < 3; i++) {
        f = supported;
        f.raw.core_mask[i] = 1;
        rejected(f);
    }

    /* An incomplete write never commits PASS. */
    tree(dt, sizeof(dt), "apple,j613", "1");
    int node = t8122_full_identity_prepare(dt);
    assert(node >= 0);
    assert(!fdt_delprop(dt, node, "id-counts-2"));
    f = supported;
    struct t8122_powered_identity identity;
    assert(!t8122_full_identity_read(read_identity, &f, &identity));
    assert(t8122_full_identity_publish(dt, &identity) < 0);
    assert(property(dt, "probe-result") == T8122_ID_NOT_RUN);

    tree(dt, sizeof(dt), "apple,j613", "1");
    assert(!fdt_pack(dt));
    assert(!fdt_open_into(dt, dt, fdt_totalsize(dt) + 60));
    assert(t8122_full_identity_prepare(dt) < 0);
    assert(fdt_path_offset(dt, "/chosen/asahi,t8122-gpu-powered-identity") < 0);
    puts("full14 powered identity producer: positive 8/10-core, opt-in, chip/read/topology, "
         "rollback and FDT failure controls PASS");
    return 0;
}
