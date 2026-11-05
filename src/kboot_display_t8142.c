/* SPDX-License-Identifier: GPL-2.0-only */
#include "adt.h"
#include "dart.h"
#include "devicetree.h"
#include "firmware.h"
#include "kboot_display_t8142.h"
#include "malloc.h"
#include "pmgr.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

struct display_session {
    char scanout[16], piodma[24], dart_scanout[48], dart_dcp[48];
};

static int session_description(const char *alias, struct display_session *s)
{
    if (!strcmp(alias, "dcp")) {
        strcpy(s->scanout, "disp0");
        strcpy(s->piodma, "disp0_piodma");
        strcpy(s->dart_scanout, "/arm-io/dart-disp0");
        strcpy(s->dart_dcp, "/arm-io/dart-dcp");
        return 0;
    }
    if (strncmp(alias, "dcpext", 6) || alias[6] < '0' || alias[6] > '7' || alias[7])
        return -1;
    snprintf(s->scanout, sizeof(s->scanout), "dispext%c", alias[6]);
    snprintf(s->piodma, sizeof(s->piodma), "dispext%c_piodma", alias[6]);
    snprintf(s->dart_scanout, sizeof(s->dart_scanout), "/arm-io/dart-dispext%c", alias[6]);
    snprintf(s->dart_dcp, sizeof(s->dart_dcp), "/arm-io/dart-dcpext%c", alias[6]);
    return 0;
}

struct display_stream {
    u32 owner, provider, sid;
    u64 base, span, bus_base, bus_size;
};

/* Match both descriptions of the actual DART, rather than relying on alias order. */
static int resolve_stream(void *dt, const char *owner, const char *adt_path, u32 sid,
                          struct display_stream *s)
{
    int path[8], len;
    u64 base, size;
    if (adt_path_offset_trace(adt, adt_path, path) < 0 ||
        adt_get_reg(adt, path, "reg", 0, &base, &size))
        return -1;
    int node = fdt_path_offset(dt, owner);
    const fdt32_t *prop = node < 0 ? NULL : fdt_getprop(dt, node, "iommus", &len);
    if (!prop || len <= 0 || len % 8)
        return -1;
    s->owner = fdt_get_phandle(dt, node);
    if (!s->owner)
        return -1;
    bool found = false;
    for (int i = 0; i < len / (int)sizeof(*prop); i += 2) {
        int provider = fdt_node_offset_by_phandle(dt, fdt32_ld(prop + i));
        if (provider < 0)
            return -1;
        if (dt_get_address(dt, provider) != base || fdt32_ld(prop + i + 1) != sid)
            continue;
        int reg_len, range_len;
        const fdt64_t *reg = fdt_getprop(dt, provider, "reg", &reg_len);
        const fdt64_t *range = fdt_getprop(dt, provider, "apple,dma-range", &range_len);
        if (found || !reg || reg_len != 16 || !range || range_len != 16 ||
            fdt_node_check_compatible(dt, provider, "apple,t8110-dart"))
            return -1;
        s->provider = fdt_get_phandle(dt, provider);
        s->base = base;
        s->span = min(size, fdt64_ld(reg + 1));
        s->sid = sid;
        s->bus_base = fdt64_ld(range);
        s->bus_size = fdt64_ld(range + 1);
        if ((s->bus_base & ((1ULL << 36) - 1)) || s->bus_size < SZ_16K ||
            s->bus_size > (1ULL << 36) || s->bus_base > UINT64_MAX - s->bus_size)
            return -1;
        found = true;
    }
    return found ? 0 : -1;
}

struct live_maps {
    void *dt;
    struct display_stream stream;
    u64 dram, dram_size, iova, phys, size, ram_pages;
    bool ram;
};

/* An IOVA-only node reserves the alias; the memreserve keeps its RAM CPU mapped. */
static int flush_mapping(struct live_maps *m)
{
    if (!m->size)
        return 0;
    void *dt = m->dt;
    if (m->ram && fdt_add_mem_rsv(dt, m->phys, m->size))
        return -1;
    int parent = fdt_path_offset(dt, "/reserved-memory");
    char name[80];
    snprintf(name, sizeof(name), "display-iova-%x-%x-%x-%lx", m->stream.owner, m->stream.provider,
             m->stream.sid, m->iova);
    int region = fdt_add_subnode(dt, parent, name);
    if (region == -FDT_ERR_EXISTS)
        region = fdt_subnode_offset(dt, parent, name);
    if (region < 0)
        return -1;
    u32 phandle = fdt_get_phandle(dt, region);
    if (!phandle &&
        (fdt_generate_phandle(dt, &phandle) || fdt_setprop_u32(dt, region, "phandle", phandle)))
        return -1;
    fdt32_t address[] = {cpu_to_fdt32(m->stream.owner), cpu_to_fdt32(m->iova >> 32),
                         cpu_to_fdt32(m->iova), cpu_to_fdt32(m->size >> 32), cpu_to_fdt32(m->size)};
    if (fdt_setprop(dt, region, "iommu-addresses", address, sizeof(address)))
        return -1;
    int owner = fdt_node_offset_by_phandle(dt, m->stream.owner);
    int len;
    const fdt32_t *regions = owner < 0 ? NULL : fdt_getprop(dt, owner, "memory-region", &len);
    if (regions) {
        if (len < 0 || len % sizeof(*regions))
            return -1;
        for (int i = 0; i < len / (int)sizeof(*regions); i++)
            if (fdt32_ld(regions + i) == phandle)
                return 0;
    }
    if (owner < 0 || fdt_appendprop_u32(dt, owner, "memory-region", phandle))
        return -1;
    owner = fdt_node_offset_by_phandle(dt, m->stream.owner);
    return owner < 0 ? -1 : fdt_appendprop_string(dt, owner, "memory-region-names", "");
}

static int preserve_page(u64 iova, u64 phys, void *opaque)
{
    struct live_maps *m = opaque;
    if (iova > m->stream.bus_size - SZ_16K || (phys & (SZ_16K - 1)))
        return -1;
    bool ram = phys >= m->dram && phys - m->dram <= m->dram_size - SZ_16K;
    if (ram)
        m->ram_pages++;
    /* The walker supplies low 36-bit IOVAs; Linux's allocator uses the high window. */
    iova += m->stream.bus_base;
    if (m->size && m->ram == ram && m->iova + m->size == iova && m->phys + m->size == phys) {
        m->size += SZ_16K;
        return 0;
    }
    if (flush_mapping(m))
        return -1;
    m->iova = iova;
    m->phys = phys;
    m->size = SZ_16K;
    m->ram = ram;
    return 0;
}

static int session_streams(void *dt, const char *alias, struct display_stream *streams)
{
    struct display_session session;
    if (session_description(alias, &session) ||
        resolve_stream(dt, alias, session.dart_dcp, 23, streams) ||
        resolve_stream(dt, session.scanout, session.dart_scanout, 0, streams + 1) ||
        resolve_stream(dt, session.piodma, session.dart_scanout, 16, streams + 2))
        return -1;
    return 0;
}

int kboot_t8142_display_reserve(void *dt, const char *alias)
{
    /* Do not probe optional DART apertures while their display island is off. */
    char controller[32];
    snprintf(controller, sizeof(controller), "/arm-io/%s", alias);
    if (pmgr_adt_is_powered(controller) != 1)
        return -1;
    struct display_stream streams[3];
    struct live_maps m = {.dt = dt};
    int chosen = adt_path_offset(adt, "/chosen");
    if (session_streams(dt, alias, streams) || chosen < 0 ||
        ADT_GETPROP(adt, chosen, "dram-base", &m.dram) != sizeof(m.dram) ||
        ADT_GETPROP(adt, chosen, "dram-size", &m.dram_size) != sizeof(m.dram_size) ||
        m.dram_size < SZ_16K || ((m.dram | m.dram_size) & (SZ_16K - 1)) ||
        m.dram > UINT64_MAX - m.dram_size)
        return -1;
    /* One root plus the architectural maximum of 2048 L2 pages. */
    size_t capacity = 1 + SZ_16K / sizeof(u64);
    u64 *tables = malloc(capacity * sizeof(*tables));
    if (!tables)
        return -1;
    int ret = -1;
    for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
        m.stream = streams[i];
        m.size = m.ram_pages = 0;
        if (dart_walk_locked_t8110(m.stream.base, m.stream.span, m.stream.sid, m.dram, m.dram_size,
                                   preserve_page, &m) ||
            flush_mapping(&m)) {
            printf("FDT: %s SID%u: cannot preserve inherited translations\n", alias, m.stream.sid);
            goto out;
        }
        /*
         * An idle external pipe has valid, locked SID0/SID16 roots with no
         * mappings until its first modeset/PIODMA allocation. Requiring RAM
         * in those streams prevents handing it over for hotplug. SID23 must
         * still map the resident firmware; all three roots and their L2
         * tables must be validated and retained even when they are empty.
         */
        if (!m.ram_pages && (!i || !strcmp(alias, "dcp"))) {
            printf("FDT: %s SID%u: required RAM mappings are absent\n", alias, m.stream.sid);
            goto out;
        }
        dart_dev_t *dart = dart_init_fdt_locked(dt, m.stream.provider, m.stream.sid);
        size_t count;
        if (!dart) {
            printf("FDT: %s SID%u: no locked translation root\n", alias, m.stream.sid);
            goto out;
        }
        ret = dart_collect_locked_tables(dart, m.dram, m.dram_size, tables, capacity, &count);
        dart_shutdown(dart);
        if (ret) {
            printf("FDT: %s SID%u: invalid inherited page tables\n", alias, m.stream.sid);
            goto out;
        }
        ret = -1;
        for (size_t j = 0; j < count; j++)
            if (fdt_add_mem_rsv(dt, tables[j], SZ_16K))
                goto out;
    }
    ret = 0;
out:
    free(tables);
    return ret;
}

static int display_clocks(void *dt, const char *alias)
{
    if (os_firmware.version != V27_0)
        return 0;
    int arm_io = adt_path_offset(adt, "/arm-io");
    u32 len;
    const u32 *rates = arm_io < 0 ? NULL : adt_getprop(adt, arm_io, "clock-frequencies", &len);
    if (!rates || len < 7 * sizeof(*rates) || len % sizeof(*rates))
        return -1;
    u32 hz;
    memcpy(&hz, rates + 6, sizeof(hz));
    if (!hz)
        return -1;
    int owner = fdt_path_offset(dt, alias), clock_len;
    const fdt32_t *clocks = fdt_getprop(dt, owner, "clocks", &clock_len);
    if (!clocks || (clock_len != 4 && clock_len != 8))
        return -1;
    u32 reference = fdt32_ld(clocks + clock_len / 4 - 1);
    int ref = fdt_node_offset_by_phandle(dt, reference), rate_len;
    const fdt32_t *rate = ref < 0 ? NULL : fdt_getprop(dt, ref, "clock-frequency", &rate_len);
    if (!rate || rate_len != 4 || fdt32_ld(rate) != 24000000)
        return -1;
    int video = fdt_path_offset(dt, "/clock-display-video");
    if (video < 0)
        video = fdt_add_subnode(dt, 0, "clock-display-video");
    u32 phandle = video < 0 ? 0 : fdt_get_phandle(dt, video);
    if (video < 0 || (!phandle && fdt_generate_phandle(dt, &phandle)) ||
        fdt_setprop_u32(dt, video, "phandle", phandle) ||
        fdt_setprop_string(dt, video, "compatible", "fixed-clock") ||
        fdt_setprop_u32(dt, video, "#clock-cells", 0) ||
        fdt_setprop_u32(dt, video, "clock-frequency", hz))
        return -1;
    fdt32_t pair[] = {cpu_to_fdt32(phandle), cpu_to_fdt32(reference)};
    owner = fdt_path_offset(dt, alias);
    if (fdt_setprop(dt, owner, "clocks", pair, sizeof(pair)))
        return -1;
    owner = fdt_path_offset(dt, alias);
    return fdt_setprop(dt, owner, "clock-names", "video\0reference", sizeof("video\0reference"));
}

/* Publish the retained PMGR chain only for a validated, powered session. */
static int enable_power_domains(void *dt, u32 phandle, unsigned int depth)
{
    int node = fdt_node_offset_by_phandle(dt, phandle), len;
    if (node < 0 || depth > 8)
        return -1;
    const fdt32_t *domains = fdt_getprop(dt, node, "power-domains", &len);
    if (!domains)
        return len == -FDT_ERR_NOTFOUND ? 0 : -1;
    /* The T8142 display islands have one parent, with no argument cells. */
    if (len != sizeof(*domains))
        return -1;
    u32 parent = fdt32_ld(domains);
    int supplier = fdt_node_offset_by_phandle(dt, parent);
    const fdt32_t *cells = supplier < 0 ? NULL :
        fdt_getprop(dt, supplier, "#power-domain-cells", &len);
    if (!cells || len != sizeof(*cells) || fdt32_ld(cells) ||
        enable_power_domains(dt, parent, depth + 1))
        return -1;
    supplier = fdt_node_offset_by_phandle(dt, parent);
    return supplier < 0 ? -1 : fdt_setprop_string(dt, supplier, "status", "okay");
}

int kboot_t8142_display_enable(void *dt, const char *alias)
{
    struct display_stream streams[3];
    struct display_session session;
    if (session_streams(dt, alias, streams) || session_description(alias, &session) ||
        display_clocks(dt, alias) || enable_power_domains(dt, streams[0].owner, 0))
        return -1;
    int controller = fdt_path_offset(dt, alias), len;
    const fdt32_t *mboxes = fdt_getprop(dt, controller, "mboxes", &len);
    if (mboxes) {
        if (len != sizeof(*mboxes))
            return -1;
        int mailbox = fdt_node_offset_by_phandle(dt, fdt32_ld(mboxes));
        if (mailbox < 0 || fdt_setprop_string(dt, mailbox, "status", "okay"))
            return -1;
    } else if (len != -FDT_ERR_NOTFOUND) {
        return -1;
    }
    for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
        int node = fdt_node_offset_by_phandle(dt, streams[i].provider);
        if (node < 0 || fdt_setprop_string(dt, node, "status", "okay"))
            return -1;
    }
    const char *owners[] = {alias, session.scanout, session.piodma};
    for (size_t i = 0; i < ARRAY_SIZE(owners); i++) {
        int node = fdt_path_offset(dt, owners[i]);
        if (node < 0 || fdt_setprop_string(dt, node, "status", "okay"))
            return -1;
    }
    int owner = fdt_path_offset(dt, alias);
    int routes = fdt_subnode_offset(dt, owner, "typec-routes");
    if (routes == -FDT_ERR_NOTFOUND)
        return 0;
    if (routes < 0)
        return -1;
    int route;
    fdt_for_each_subnode(route, dt, routes)
    {
        int len;
        const char *status = fdt_getprop(dt, route, "status", &len);
        if (status && len == sizeof("disabled") && !strcmp(status, "disabled") &&
            fdt_setprop_string(dt, route, "status", "okay"))
            return -1;
    }
    return route == -FDT_ERR_NOTFOUND ? 0 : -1;
}

/* An optional external domain must not become a required disabled supplier. */
int kboot_t8142_display_prune(void *dt)
{
    int master = fdt_path_offset(dt, "disp0"), len;
    const fdt32_t *prop = master < 0 ? NULL : fdt_getprop(dt, master, "iommus", &len);
    if (!prop || len <= 0 || len % 8 || len > 16 * (int)sizeof(*prop))
        return -1;
    fdt32_t keep[16];
    int count = 0;
    for (int i = 0; i < len / (int)sizeof(*prop); i += 2) {
        int provider = fdt_node_offset_by_phandle(dt, fdt32_ld(prop + i));
        if (provider < 0)
            return -1;
        const char *status = fdt_getprop(dt, provider, "status", NULL);
        if (status && strcmp(status, "okay") && strcmp(status, "ok"))
            continue;
        keep[count++] = prop[i];
        keep[count++] = prop[i + 1];
    }
    if (!count)
        return -1;
    return fdt_setprop(dt, master, "iommus", keep, count * sizeof(*keep));
}
