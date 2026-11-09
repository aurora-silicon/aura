/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "errno.h"
#include "malloc.h"
#include "sio.h"
#include "soc.h"
#include "types.h"
#include "utils.h"

// Reuse pages for different data sections, if space allows it
#define MERGE_SIO_FWDATA

#define SIO_KEY(s)  _SIO_KEY(#s)
#define _SIO_KEY(s) (((s)[0] << 24) | ((s)[1] << 16) | ((s)[2] << 8) | (s)[3])

static void *alloc_mapped_data(struct sio_data *siodata, size_t size, u64 *iova)
{
    if (siodata->num_fwdata >= MAX_FWDATA)
        return NULL;

    struct sio_mapping *mapping = &siodata->fwdata[siodata->num_fwdata];

#ifdef MERGE_SIO_FWDATA
    if (siodata->num_fwdata &&
        ALIGN_UP((mapping - 1)->size, SZ_16K) >= (mapping - 1)->size + size) {
        mapping--;
        *iova = mapping->iova + mapping->size;
        mapping->size = ALIGN_UP(mapping->size + size, SZ_4K);
        goto done;
    }
#endif

    if (!siodata->num_fwdata++)
        mapping->iova = *iova = siodata->iova_base;
    else
        mapping->iova = *iova = ALIGN_UP((mapping - 1)->iova + (mapping - 1)->size, SZ_16K);
    mapping->size = ALIGN_UP(size, SZ_4K);
    mapping->phys = top_of_memory_alloc(size);
    memset64((void *)mapping->phys, 0, ALIGN_UP(mapping->size, SZ_16K));

done:
    return (void *)((*iova - mapping->iova) + mapping->phys);
}

static void mapping_fixup(struct sio_data *siodata)
{
    for (int i = 0; i < siodata->num_fwdata; i++) {
        struct sio_mapping *mapping = &siodata->fwdata[i];
        mapping->size = ALIGN_UP(mapping->size, SZ_16K);
    }
}

static void *add_fwdata(struct sio_data *siodata, size_t size, u32 param_id)
{
    if (siodata->num_fwparams + 1 >= MAX_FWPARAMS)
        return NULL;

    u64 iova;
    void *p = alloc_mapped_data(siodata, size, &iova);

    if (!p)
        return NULL;

    struct sio_fwparam *param = &siodata->fwparams[siodata->num_fwparams];
    param->key = param_id;
    param->value = iova >> 12;
    param++;
    param->key = param_id + 1;
    param->value = size;
    siodata->num_fwparams += 2;

    return p;
}

#define PARAM_UNK_000b     0x000b
#define PARAM_PANIC_BUFFER 0x000f
#define PARAM_MAP_RANGE    0x001a
#define PARAM_DEVICE_TYPE  0x001c
#define PARAM_TUNABLES     0x001e
#define PARAM_DMASHIM_DATA 0x0022
#define PARAM_UNK_030d     0x030d

struct copy_rule {
    const char *prop;
    int fw_param;
    bool keyed;
    int blobsize;
    bool irq; // keyed records end in a u32 interrupt number, none (~0) from the ADT
    // only records the ADT describes get it; empty slots stay all zero
    bool irq_present;
    u32 nkeys;
    const char *keys[10];
};

#define SPACER "\xff\xff\xff\xff"

struct copy_rule copy_rules[] = {
    {
        .prop = "asio-ascwrap-tunables",
        .fw_param = PARAM_TUNABLES,
    },
    {
        .blobsize = 0x1b80,
        .fw_param = PARAM_UNK_000b,
    },
    {
        .blobsize = 0x1e000,
        .fw_param = PARAM_PANIC_BUFFER,
    },
    {
        // performance endpoint? FIFO?
        .blobsize = 0x4000,
        .fw_param = PARAM_UNK_030d,
    },
    {
        .prop = "map-range",
        .fw_param = PARAM_MAP_RANGE,
        .blobsize = 16,
        .keyed = true,
        .keys = {SPACER, SPACER, SPACER, "MISC", NULL},
    },
    {
        .prop = "dmashim",
        .fw_param = PARAM_DMASHIM_DATA,
        .blobsize = 32,
        .keyed = true,
        .keys = {"SSPI", "SUAR", "SAUD", "ADMA", "AAUD", NULL},
    },
    {
        // it seems 'device_type' must go after 'dmashim'
        .prop = "device-type",
        .fw_param = PARAM_DEVICE_TYPE,
        .blobsize = 8,
        .keyed = true,
        .keys = {"dSPI", "dUAR", "dMCA", "dDPA", "dPDM", "dALE", "dAMC", "dAPD", NULL},
    },
};

/*
 * SIO firmware protocol 10 (T6030) takes six DMA shim records of 36 bytes, the
 * ADT's 32 bytes and an interrupt (none for any of them), and nine device types.
 * The ADT still describes the shims and device types as above.
 */
static struct copy_rule copy_rules_v10[] = {
    {
        .prop = "asio-ascwrap-tunables",
        .fw_param = PARAM_TUNABLES,
    },
    {
        .blobsize = 0x1b80,
        .fw_param = PARAM_UNK_000b,
    },
    {
        .blobsize = 0x1e000,
        .fw_param = PARAM_PANIC_BUFFER,
    },
    {
        .blobsize = 0x4000,
        .fw_param = PARAM_UNK_030d,
    },
    {
        .prop = "map-range",
        .fw_param = PARAM_MAP_RANGE,
        .blobsize = 16,
        .keyed = true,
        .keys = {SPACER, SPACER, SPACER, "MISC", NULL},
    },
    {
        .prop = "dmashim",
        .fw_param = PARAM_DMASHIM_DATA,
        .blobsize = 32,
        .irq = true,
        .keyed = true,
        .keys = {"SSPI", "SUAR", "SAUD", "ADMA", "AAUD", SPACER, NULL},
    },
    {
        .prop = "device-type",
        .fw_param = PARAM_DEVICE_TYPE,
        .blobsize = 8,
        .keyed = true,
        .keys = {"dSPI", "dUAR", "dMCA", "dDPA", "dPDM", "dALE", "dAMC", "dAPD", SPACER, NULL},
    },
};

/*
 * The T6040/T6041 (M4 Pro/Max) and T8140 (MacBook Neo) SIO firmware also reports protocol 10, but
 * its tables differ from T6030's: only the shim records the ADT describes end in the all-ones
 * selector (the empty slots stay zero), and the response array (key 0x0b) belongs to the OS driver,
 * which publishes it as an entry count along with the segment array. The tables live in the SIO
 * DART's address space, which on these SoCs begins after the preloaded firmware's segments
 * (sio_iova_base()).
 */
static struct copy_rule copy_rules_t6041[] = {
    {
        .prop = "asio-ascwrap-tunables",
        .fw_param = PARAM_TUNABLES,
    },
    {
        .blobsize = 0x1e000,
        .fw_param = PARAM_PANIC_BUFFER,
    },
    {
        .blobsize = 0x4000,
        .fw_param = PARAM_UNK_030d,
    },
    {
        .prop = "map-range",
        .fw_param = PARAM_MAP_RANGE,
        .blobsize = 16,
        .keyed = true,
        .keys = {SPACER, SPACER, SPACER, "MISC", NULL},
    },
    {
        .prop = "dmashim",
        .fw_param = PARAM_DMASHIM_DATA,
        .blobsize = 32,
        .irq = true,
        .irq_present = true,
        .keyed = true,
        .keys = {"SSPI", "SUAR", "SAUD", "ADMA", "AAUD", SPACER, NULL},
    },
    {
        .prop = "device-type",
        .fw_param = PARAM_DEVICE_TYPE,
        .blobsize = 8,
        .keyed = true,
        .keys = {"dSPI", "dUAR", "dMCA", "dDPA", "dPDM", "dALE", "dAMC", "dAPD", SPACER, NULL},
    },
};

/*
 * Where the tables go in the SIO's address space: 0x30000 on earlier SoCs; on T6041 and T8140 the
 * first 16K page past the remapped firmware segments iBoot loaded, inside the SIO DART's window.
 */
static int sio_iova_base(int node, u64 *base)
{
    if (chip_id != T6040 && chip_id != T6041 && chip_id != T8140) {
        *base = 0x30000;
        return 0;
    }

    u32 len;
    const struct adt_segment_ranges *seg = adt_getprop(adt, node, "segment-ranges", &len);
    if (!seg || !len || len % sizeof(*seg)) {
        printf("sio: missing or bad segment-ranges\n");
        return -1;
    }

    u64 end = 0;
    for (u32 i = 0; i < len / sizeof(*seg); i++) {
        if (!seg[i].remap || !seg[i].size || seg[i].remap > UINT64_MAX - seg[i].size) {
            printf("sio: bad firmware segment %u\n", i);
            return -1;
        }
        end = max(end, seg[i].remap + seg[i].size);
    }
    *base = ALIGN_UP(end, SZ_16K);
    /* Every table address goes to the firmware as a 32-bit page number. */
    if (!end || *base < end || (*base + SZ_1M) >> 12 > UINT32_MAX) {
        printf("sio: firmware segments end at %lx, no room for tables\n", end);
        return -1;
    }
    return 0;
}

int find_key_index(const char *keylist[], u32 needle)
{
    int i;
    for (i = 0; keylist[i]; i++) {
        const char *s = keylist[i];
        u32 key = ((u32)s[0]) << 24 | ((u32)s[1]) << 16 | ((u32)s[2]) << 8 | s[3];
        if (key == needle)
            break;
    }
    return i;
}

struct sio_data *sio_setup_fwdata(const char *adt_path)
{
    struct sio_data *siodata = calloc(1, sizeof(struct sio_data));

    if (!siodata)
        return NULL;

    int node = adt_path_offset(adt, adt_path);
    if (node < 0) {
        printf("%s: missing node %s\n", __func__, adt_path);
        goto err;
    }

    if (sio_iova_base(node, &siodata->iova_base))
        goto err;

    struct copy_rule *rules = copy_rules;
    int nrules = ARRAY_SIZE(copy_rules);

    if (chip_id == T6030) {
        rules = copy_rules_v10;
        nrules = ARRAY_SIZE(copy_rules_v10);
    } else if (chip_id == T6040 || chip_id == T6041 || chip_id == T8140) {
        rules = copy_rules_t6041;
        nrules = ARRAY_SIZE(copy_rules_t6041);
    }

    for (int i = 0; i < nrules; i++) {
        struct copy_rule *rule = &rules[i];
        u32 len;

        if (!rule->prop) {
            if (!add_fwdata(siodata, rule->blobsize, rule->fw_param))
                goto err;

            continue;
        }

        const u8 *adt_blob = adt_getprop(adt, node, rule->prop, &len);
        if (!adt_blob) {
            printf("%s: missing ADT property '%s'\n", __func__, rule->prop);
            goto err;
        }

        if (!rule->keyed) {
            u8 *sio_blob = add_fwdata(siodata, len, rule->fw_param);
            if (!sio_blob)
                goto err;
            memcpy8(sio_blob, (void *)adt_blob, len);
            continue;
        }

        int nkeys = find_key_index(rule->keys, 0);
        int stride = rule->blobsize + (rule->irq ? 4 : 0);
        u8 *sio_blob = add_fwdata(siodata, nkeys * stride, rule->fw_param);
        if (!sio_blob)
            goto err;

        if (rule->irq && !rule->irq_present) {
            u32 none = ~0u;

            for (int k = 0; k < nkeys; k++)
                memcpy8(sio_blob + k * stride + rule->blobsize, &none, sizeof(none));
        }

        if (len % (rule->blobsize + 4) != 0) {
            printf("%s: bad length %d of ADT property '%s', expected multiple of %d + 4\n",
                   __func__, len, rule->prop, rule->blobsize);
            goto err;
        }

        for (u32 off = 0; off + rule->blobsize <= len; off += (rule->blobsize + 4)) {
            const u8 *p = &adt_blob[off];
            u32 key = *((u32 *)p);
            int key_idx = find_key_index(rule->keys, key);

            if (key_idx >= nkeys) {
                printf("%s: unknown key %x found in ADT property '%s'\n", __func__, key,
                       rule->prop);
                goto err;
            }

            memcpy8(sio_blob + (key_idx * stride), (void *)(p + 4), rule->blobsize);
            if (rule->irq_present) {
                u32 none = ~0u;

                memcpy8(sio_blob + key_idx * stride + rule->blobsize, &none, sizeof(none));
            }
        }
    }

    mapping_fixup(siodata);

    return siodata;

err:
    for (int i = 0; i < MAX_FWDATA; i++) {
        if (!siodata->fwdata[i].size)
            break;
        // No way to give back memory with the top of memory
        // allocator.
        // free((void *)sio_fwdata[i].phys);
    }
    free(siodata);

    return NULL;
}

static u64 adt_u64(const u8 *p)
{
    return (u64)*(const u32 *)p | ((u64)*(const u32 *)(p + 4) << 32);
}

/* Each keyed ADT record: a four-character key, then its fields. */
struct native_rule {
    const char *prop;
    u32 stride;
    bool size64;
    const char *keys[4];
};

static const struct native_rule native_rules[] = {
    // map-range: key, u64 base, u32 size, u32 reserved
    {.prop = "map-range", .stride = 20, .size64 = false, .keys = {"MISC", NULL}},
    // dmashim: key, u64 base, u64 size, four configuration words
    {.prop = "dmashim", .stride = 36, .size64 = true, .keys = {"SSPI", "SUAR", "SAUD", NULL}},
};

/*
 * The T8140 (MacBook Neo) SIO firmware reaches its own MMIO, the MISC map range and the native
 * DMA shims, through the SIO DART at the physical addresses in its records, which stay physical in
 * the boot tables. Unlike T6041's, its DART has no bypass stream for them, so the OS has to map
 * each range 1:1 before it starts the firmware; without that the firmware takes an SError when
 * key 0x23 brings up the shims. List those ranges from the ADT. Returns their number, 0 on SoCs
 * that do not need them, or -1 if the ADT records are unusable.
 */
int sio_native_mmio(const char *adt_path, struct sio_mapping *ranges, int max)
{
    if (chip_id != T8140)
        return 0;

    int node = adt_path_offset(adt, adt_path);
    if (node < 0) {
        printf("sio: missing node %s\n", adt_path);
        return -1;
    }

    int n = 0;
    for (size_t r = 0; r < ARRAY_SIZE(native_rules); r++) {
        const struct native_rule *rule = &native_rules[r];
        u32 len;
        const u8 *blob = adt_getprop(adt, node, rule->prop, &len);

        if (!blob || !len || len % rule->stride) {
            printf("sio: missing or bad ADT property '%s'\n", rule->prop);
            return -1;
        }

        for (u32 off = 0; off < len; off += rule->stride) {
            const u8 *p = blob + off;
            u32 key = *(const u32 *)p;
            bool native = false;

            for (int k = 0; rule->keys[k]; k++)
                native |= key == (u32)_SIO_KEY(rule->keys[k]);
            if (!native)
                continue;

            u64 base = adt_u64(p + 4);
            u64 size = rule->size64 ? adt_u64(p + 12) : *(const u32 *)(p + 12);

            if (!base || !size || base > UINT64_MAX - size || (base + size) >> 42 ||
                base % SZ_16K || size % SZ_16K) {
                printf("sio: bad native MMIO range %lx+%lx in '%s'\n", base, size, rule->prop);
                return -1;
            }
            for (int i = 0; i < n; i++) {
                if (base < ranges[i].phys + ranges[i].size && ranges[i].phys < base + size) {
                    printf("sio: native MMIO range %lx+%lx overlaps %lx+%lx\n", base, size,
                           ranges[i].phys, ranges[i].size);
                    return -1;
                }
            }
            if (n == max) {
                printf("sio: too many native MMIO ranges\n");
                return -1;
            }
            ranges[n].phys = ranges[n].iova = base;
            ranges[n].size = size;
            n++;
        }
    }

    if (!n) {
        printf("sio: no native MMIO ranges in the ADT\n");
        return -1;
    }
    return n;
}

