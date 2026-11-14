/* SPDX-License-Identifier: MIT */

#include "kboot_gpu_t6031.h"
#include "adt.h"
#include "exception.h"
#include "gfx_fw_image_info.h"
#include "pmgr.h"
#include "soc.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/* PMGR power state register fields, as pmgr.c uses them */
#define PS_AUTO_ENABLE  BIT(28)
#define PS_WAS_CLKGATED BIT(9)
#define PS_WAS_PWRGATED BIT(8)
#define PS_ACTUAL       GENMASK(7, 4)
#define PS_TARGET       GENMASK(3, 0)

#define CLOSURE_VISITS 32 /* devices looked at while following parents, a bound against loops */
#define CLOSURE_DEPTH  8

/* The 14.8.3 GPU firmware family: its RTKit, and the iBoot of the 14.8.3 stub that loads it */
#define G15_RTKIT_14_8_3  "RTKit-2419.140.12.release"
#define STUB_IBOOT_14_8_3 "iBoot-10151.140.19.700.2"

static int guard_begin(void)
{
    int before = exc_count;
    exc_guard = GUARD_MARK | GUARD_SILENT;
    return before;
}

/* -1 if a synchronous abort or an SError was taken since guard_begin() */
static int guard_end(int before)
{
    sysop("dsb sy");
    sysop("isb");
    exc_guard = GUARD_OFF;
    return exc_count == before ? 0 : -1;
}

/* ---- the power closure ---- */

static int closure_add(struct t6031_closure *c, const struct pmgr_gate *g, u32 depth)
{
    for (int i = 0; i < c->n; i++) {
        if (c->ps[i].addr != g->psreg)
            continue;
        if (depth > c->ps[i].depth)
            c->ps[i].depth = depth;
        return 0;
    }
    if (c->n >= T6031_PS_MAX)
        return -1;
    c->ps[c->n++] = (struct t6031_ps){g->psreg, 0, 0, depth, g->name};
    return 0;
}

int t6031_closure_get(const char *path, struct t6031_closure *c)
{
    struct {
        u8 die;
        u16 id;
        u32 depth;
    } work[CLOSURE_VISITS];
    int nwork = 0;

    memset(c, 0, sizeof(*c));
    for (u32 i = 0; i < CLOSURE_VISITS; i++) {
        struct pmgr_gate g;
        if (pmgr_adt_gate(path, "clock-gates", i, &g) < 0)
            break;
        if (nwork >= CLOSURE_VISITS)
            return -1;
        work[nwork++] = (typeof(work[0])){g.die, g.id, 0};
    }
    if (!nwork)
        return -1;

    /* Every device and parent, a parent always deeper than its child: parents are restored last */
    for (int w = 0; w < nwork; w++) {
        struct pmgr_gate g;
        if (pmgr_device_gate(work[w].die, work[w].id, &g) < 0 || !g.found)
            return -1;
        if (!g.virt && (!g.psreg || closure_add(c, &g, work[w].depth)))
            return -1;
        for (int k = 0; k < 2; k++) {
            if (!g.parent[k])
                continue;
            if (nwork >= CLOSURE_VISITS || work[w].depth + 1 > CLOSURE_DEPTH)
                return -1;
            work[nwork++] = (typeof(work[0])){g.die, g.parent[k], work[w].depth + 1};
        }
    }
    if (!c->n)
        return -1;

    /* Stable sort by depth: children first */
    for (int i = 1; i < c->n; i++)
        for (int j = i; j > 0 && c->ps[j - 1].depth > c->ps[j].depth; j--) {
            struct t6031_ps t = c->ps[j];
            c->ps[j] = c->ps[j - 1];
            c->ps[j - 1] = t;
        }

    int g = guard_begin();
    for (int i = 0; i < c->n; i++)
        c->ps[i].before = read32(c->ps[i].addr);
    return guard_end(g);
}

int t6031_closure_active(const struct t6031_closure *c)
{
    u32 val[T6031_PS_MAX];
    int g = guard_begin();
    for (int i = 0; i < c->n; i++)
        val[i] = read32(c->ps[i].addr);
    if (guard_end(g))
        return -1;
    for (int i = 0; i < c->n; i++)
        if (FIELD_GET(PS_ACTUAL, val[i]) != PMGR_PS_ACTIVE)
            return -1;
    return 0;
}

static bool closure_same(u32 before, u32 after)
{
    u32 kept = PS_AUTO_ENABLE | PS_TARGET;
    if ((before & kept) != (after & kept))
        return false;
    /* With automatic power management on, the actual state follows the hardware */
    return (before & PS_AUTO_ENABLE) || FIELD_GET(PS_ACTUAL, before) == FIELD_GET(PS_ACTUAL, after);
}

int t6031_closure_restore(struct t6031_closure *c)
{
    int ret = 0;

    for (int i = 0; i < c->n; i++) {
        struct t6031_ps *p = &c->ps[i];
        u32 want = FIELD_GET(PS_TARGET, p->before);

        int g = guard_begin();
        u32 now = read32(p->addr);
        if (guard_end(g)) {
            ret = -1;
            continue;
        }
        /* pmgr_set_mode() polls (bounded) until the actual state is the target */
        if (FIELD_GET(PS_TARGET, now) != want && pmgr_set_mode(p->addr, want))
            ret = -1;
        if (p->before & PS_AUTO_ENABLE)
            mask32(p->addr, PS_WAS_CLKGATED | PS_WAS_PWRGATED, PS_AUTO_ENABLE);
    }

    int g = guard_begin();
    for (int i = 0; i < c->n; i++)
        c->ps[i].after = read32(c->ps[i].addr);
    if (guard_end(g))
        return -1;
    for (int i = 0; i < c->n; i++)
        if (!closure_same(c->ps[i].before, c->ps[i].after))
            ret = -1;
    return ret;
}

/* ---- the identity ---- */

struct t8122_powered_identity t6031_probe_run(const struct t6031_probe_ops *ops, void *ctx)
{
    struct t8122_powered_identity id = {.result = T8122_ID_POWER_FAILED};

    /* Restore even after a failed enable: PMGR can have switched part of the closure on */
    if (!ops->enable(ctx) && !ops->active(ctx)) {
        if (ops->read(ctx, &id.id_version, id.core_mask, &id.id_counts_1, &id.id_counts_2)) {
            /* A faulted read leaves nothing worth publishing */
            id = (struct t8122_powered_identity){.result = T8122_ID_READ_FAULT};
        } else {
            id.result = t6031_identity_valid(&id) ? T8122_ID_PASS : T8122_ID_MISMATCH;
        }
    }
    if (ops->restore(ctx))
        id.result = T8122_ID_POWER_OFF_FAILED;
    return id;
}

bool t6031_identity_valid(const struct t8122_powered_identity *id)
{
    u32 version = id->id_version, counts = id->id_counts_1;
    u64 mask = id->core_mask[0] | ((u64)id->core_mask[1] << 32);

    return (version >> 24) == 7 && ((version >> 16) & 0xff) == 4 && ((counts >> 16) & 0xf) == 1 &&
           ((counts >> 8) & 0xff) == T6031_GPU_CLUSTERS && mask && !(mask >> T6031_CORE_SLOTS) &&
           !id->core_mask[2];
}

u32 t6031_core_counts(const struct t8122_powered_identity *id, u32 counts[T6031_GPU_CLUSTERS])
{
    u64 mask = id->core_mask[0] | ((u64)id->core_mask[1] << 32);
    u32 total = 0;

    for (int i = 0; i < T6031_GPU_CLUSTERS; i++) {
        counts[i] = __builtin_popcountll((mask >> (i * T6031_CORES_PER_CLUSTER)) &
                                         ((1UL << T6031_CORES_PER_CLUSTER) - 1));
        total += counts[i];
    }
    return total;
}

static const char *const identity_u32[] = {
    "probe-result", "id-version",  "core-mask-0", "core-mask-1",
    "core-mask-2",  "id-counts-1", "id-counts-2",
};

int t6031_identity_node_named(void *dt, int nps, const char *node_name)
{
    fdt64_t zero64[T6031_PS_MAX] = {0};
    fdt32_t zero32[T6031_PS_MAX] = {0};
    int chosen = fdt_path_offset(dt, "/chosen");

    if (nps < 0 || nps > T6031_PS_MAX || chosen < 0 ||
        fdt_subnode_offset(dt, chosen, node_name) >= 0)
        return -1;
    int node = fdt_add_subnode(dt, chosen, node_name);
    if (node < 0)
        return -1;
    int err = fdt_setprop_u32(dt, node, "schema-version", 1);
    for (size_t i = 0; i < ARRAY_SIZE(identity_u32) && !err; i++)
        err = fdt_setprop_u32(dt, node, identity_u32[i], 0);
    if (!err)
        err = fdt_setprop(dt, node, "pmgr-addresses", zero64, nps * sizeof(fdt64_t));
    if (!err)
        err = fdt_setprop(dt, node, "pmgr-before", zero32, nps * sizeof(fdt32_t));
    if (!err)
        err = fdt_setprop(dt, node, "pmgr-after", zero32, nps * sizeof(fdt32_t));
    if (err) {
        fdt_del_node(dt, node);
        return -1;
    }
    return node;
}

int t6031_identity_node(void *dt, int nps)
{
    return t6031_identity_node_named(dt, nps, T6031_GPU_IDENTITY);
}

static int set_u32(void *dt, int node, const char *name, u32 value)
{
    fdt32_t v = cpu_to_fdt32(value);
    return fdt_setprop_inplace(dt, node, name, &v, sizeof(v));
}

int t6031_identity_publish_named(void *dt, const struct t8122_powered_identity *id,
                                 const struct t6031_closure *c, const char *node_name)
{
    fdt64_t addr[T6031_PS_MAX];
    fdt32_t before[T6031_PS_MAX], after[T6031_PS_MAX];
    int chosen = fdt_path_offset(dt, "/chosen");
    int node = chosen < 0 ? chosen : fdt_subnode_offset(dt, chosen, node_name);

    if (node < 0)
        return -1;
    for (int i = 0; i < c->n; i++) {
        addr[i] = cpu_to_fdt64(c->ps[i].addr);
        before[i] = cpu_to_fdt32(c->ps[i].before);
        after[i] = cpu_to_fdt32(c->ps[i].after);
    }
    /* Every property was reserved at this size: nothing here grows the device tree */
    if (set_u32(dt, node, "id-version", id->id_version) ||
        set_u32(dt, node, "core-mask-0", id->core_mask[0]) ||
        set_u32(dt, node, "core-mask-1", id->core_mask[1]) ||
        set_u32(dt, node, "core-mask-2", id->core_mask[2]) ||
        set_u32(dt, node, "id-counts-1", id->id_counts_1) ||
        set_u32(dt, node, "id-counts-2", id->id_counts_2) ||
        fdt_setprop_inplace(dt, node, "pmgr-addresses", addr, c->n * sizeof(fdt64_t)) ||
        fdt_setprop_inplace(dt, node, "pmgr-before", before, c->n * sizeof(fdt32_t)) ||
        fdt_setprop_inplace(dt, node, "pmgr-after", after, c->n * sizeof(fdt32_t)))
        return -1;
    /* The result last: a consumer that sees it sees the values above */
    return set_u32(dt, node, "probe-result", id->result);
}

int t6031_identity_publish(void *dt, const struct t8122_powered_identity *id,
                           const struct t6031_closure *c)
{
    return t6031_identity_publish_named(dt, id, c, T6031_GPU_IDENTITY);
}

/* ---- the loaded GPU firmware image ---- */

/*
 * The image iBoot loaded into the gfx-asc text segment is of the 14.8.3 GPU firmware family when
 * its image-info header is well formed, the text size the header gives is the ADT's text segment
 * size, its RTKit version string is the one the 14.8.3 images of T6030 and T8122 carry, and iBoot
 * is the one of the 14.8.3 stub that loads them (it reports itself as 14.7). The image itself is
 * not pinned: the kernel identifies it by UUID and contents before it starts anything.
 */
const struct fw_version_info *dt_t6031_gpu_image_named(void *dt, const char *node_name)
{
    struct gfx_fw_image_info image;
    char rtkit[GFX_FW_VERSION_MAX] = "";
    u32 adt_text = 0, len = 0;
    const char *why = NULL;

    int asc = adt_path_offset(adt, "/arm-io/gfx-asc");
    const struct adt_segment_ranges *seg =
        asc < 0 ? NULL : adt_getprop(adt, asc, "segment-ranges", &len);
    if (seg && len == 2 * sizeof(*seg)) {
        struct adt_segment_ranges r;
        memcpy(&r, seg, sizeof(r));
        adt_text = r.size;
    }

    bool have_image = firmware_gfx_image_info(&image);
    bool have_rtkit = have_image && firmware_gfx_rtkit_version(rtkit);
    if (!have_image)
        why = "no readable image-info header";
    else if (!adt_text || image.text_size != adt_text)
        why = "the header's text size is not the ADT's";
    else if (!have_rtkit)
        why = "no readable RTKit version";
    else if (strcmp(rtkit, G15_RTKIT_14_8_3))
        why = "not the 14.8.3 family's RTKit";
    else if (os_firmware.version != V14_7 || strcmp(os_firmware.iboot, STUB_IBOOT_14_8_3))
        why = "not the 14.8.3 stub's iBoot";

    if (have_image) {
        const u8 *u = image.uuid;
        printf("FDT: GPU: T6031 image %02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-"
               "%02X%02X%02X%02X%02X%02X, %s, text 0x%x (ADT 0x%x): %s\n",
               u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7], u[8], u[9], u[10], u[11], u[12],
               u[13], u[14], u[15], have_rtkit ? rtkit : "no RTKit version", image.text_size,
               adt_text, why ? why : "14.8.3 family, image not pinned");
    } else {
        printf("FDT: GPU: T6031 image: %s\n", why);
    }

    int chosen = fdt_path_offset(dt, "/chosen");
    int node = chosen < 0 ? chosen : fdt_add_subnode(dt, chosen, node_name);
    if (node < 0) {
        printf("FDT: GPU: could not add /chosen/%s (%d)\n", node_name, node);
        return why ? NULL : &fw_versions[V14_8_3];
    }
    int err = 0;
    if (have_image)
        err = fdt_setprop(dt, node, "image-uuid", image.uuid, sizeof(image.uuid)) ||
              fdt_setprop_u32(dt, node, "image-text-size", image.text_size) ||
              fdt_setprop_u32(dt, node, "image-patchbay-offset", image.patchbay_offset) ||
              fdt_setprop_u32(dt, node, "image-patchbay-size", image.patchbay_size);
    if (!err && have_rtkit)
        err = fdt_setprop_string(dt, node, "image-rtkit-version", rtkit);
    if (!err)
        err = fdt_setprop_u32(dt, node, "adt-text-size", adt_text) ||
              fdt_setprop_string(dt, node, "firmware-family", why ? "unknown" : "14.8.3") ||
              fdt_setprop_string(dt, node, "family-check", why ? why : "passed");
    if (err) {
        /* All or nothing: remove the node this call added */
        printf("FDT: GPU: could not fill /chosen/%s\n", node_name);
        chosen = fdt_path_offset(dt, "/chosen");
        node = chosen < 0 ? -1 : fdt_subnode_offset(dt, chosen, node_name);
        if (node >= 0)
            fdt_del_node(dt, node);
    }
    return why ? NULL : &fw_versions[V14_8_3];
}

const struct fw_version_info *dt_t6031_gpu_image(void *dt)
{
    return dt_t6031_gpu_image_named(dt, T6031_GPU_IMAGE);
}
