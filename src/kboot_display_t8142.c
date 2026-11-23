/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * T8142 display sessions that iBoot leaves running: the internal DCP (dcp, its
 * display subsystem disp0 and the PIODMA stream) and the external DCPs
 * (dcpextN, their scanout stream on disp0 and their PIODMA stream).
 *
 * Before a session is published, every page its three DART streams map
 * (firmware SID 23, scanout SID 0, PIODMA SID 16) is kept from Linux: RAM by a
 * memreserve, the IOVA by an iommu-addresses node on the stream's owner, and
 * the inherited page tables by a memreserve of their own.
 */
#include "adt.h"
#include "dart.h"
#include "devicetree.h"
#include "kboot_display_t8142.h"
#include "malloc.h"
#include "pmgr.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

/*
 * The internal PIODMA alias. "disp0-piodma" first (the J813/J815 trees; the
 * aliases schema allows only [a-z0-9-]), then "disp0_piodma" (the J704 tree,
 * and the name older T8142 loaders read). Both may be present, as one node.
 */
const char *kboot_t8142_disp0_piodma_alias(void *dt)
{
    const char *dash = fdt_get_alias(dt, "disp0-piodma");
    const char *underscore = fdt_get_alias(dt, "disp0_piodma");
    if (dash && underscore && fdt_path_offset(dt, dash) != fdt_path_offset(dt, underscore)) {
        printf("FDT: disp0-piodma and disp0_piodma aliases differ\n");
        return NULL;
    }
    if (dash)
        return "disp0-piodma";
    return underscore ? "disp0_piodma" : NULL;
}

/*
 * An external session's PIODMA node: the alias "dispextN-piodma", then
 * "dispextN_piodma", else the controller's "piodma" child (the J813/J815
 * trees have no alias). The kernel uses the child, so an alias must name it.
 */
int kboot_t8142_dcpext_piodma(void *dt, const char *alias)
{
    if (strncmp(alias, "dcpext", 6) || alias[6] < '0' || alias[6] > '7' || alias[7])
        return -1;
    int node = fdt_path_offset(dt, alias);
    int child = node < 0 ? node : fdt_subnode_offset(dt, node, "piodma");
    char names[2][24];
    snprintf(names[0], sizeof(names[0]), "dispext%c-piodma", alias[6]);
    snprintf(names[1], sizeof(names[1]), "dispext%c_piodma", alias[6]);
    for (size_t i = 0; i < ARRAY_SIZE(names); i++) {
        if (!fdt_get_alias(dt, names[i]))
            continue;
        int piodma = fdt_path_offset(dt, names[i]);
        if (piodma < 0 || (child >= 0 && piodma != child)) {
            printf("FDT: %s: alias %s is not its piodma node\n", alias, names[i]);
            return -1;
        }
        return piodma;
    }
    return child < 0 ? -1 : child;
}

struct display_stream {
    u32 owner, provider, sid;
    u64 base, span, bus_base, bus_size;
};

static int stream_bus(void *dt, int provider, struct display_stream *s)
{
    int range_len;
    const fdt64_t *range = fdt_getprop(dt, provider, "apple,dma-range", &range_len);
    if (!range || range_len != 16)
        return -1;
    s->bus_base = fdt64_ld(range);
    s->bus_size = fdt64_ld(range + 1);
    if ((s->bus_base & ((1ULL << 36) - 1)) || s->bus_size < SZ_16K || s->bus_size > (1ULL << 36) ||
        s->bus_base > UINT64_MAX - s->bus_size)
        return -1;
    return 0;
}

/* Internal: match both descriptions of the actual DART, rather than relying on alias order. */
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
        u64 window_base, window;
        u32 phandle = fdt32_ld(prop + i);
        if (found || dart_fdt_t8110_window(dt, phandle, &window_base, &window) ||
            stream_bus(dt, provider, s))
            return -1;
        s->provider = phandle;
        s->base = base;
        s->span = min(size, window);
        s->sid = sid;
        found = true;
    }
    return found ? 0 : -1;
}

/*
 * External: a stream given by its provider. Only the first register window
 * holds the stream registers (the J813/J815 dcpext DARTs list a second one).
 * An owner without a phandle (a piodma child) gets one for iommu-addresses.
 */
static int stream_at(void *dt, const char *owner, u32 provider_phandle, u32 sid,
                     struct display_stream *s)
{
    int node = fdt_path_offset(dt, owner);
    if (node < 0)
        return -1;
    u32 phandle = fdt_get_phandle(dt, node);
    if (!phandle &&
        (fdt_generate_phandle(dt, &phandle) || fdt_setprop_u32(dt, node, "phandle", phandle)))
        return -1;
    int provider = fdt_node_offset_by_phandle(dt, provider_phandle);
    /* Two address and two size cells, a T8110 and its first window: dart_fdt_t8110_window(). */
    if (provider < 0 || dart_fdt_t8110_window(dt, provider_phandle, &s->base, &s->span) ||
        stream_bus(dt, provider, s))
        return -1;
    s->owner = phandle;
    s->provider = provider_phandle;
    s->sid = sid;
    return 0;
}

struct live_maps {
    void *dt;
    struct display_stream stream;
    u64 dram, dram_size, iova, phys, size, pages, ram_pages;
    u32 runs;
    bool ram;
};

/*
 * Each run adds a /memreserve/ entry, a node and two property appends (about 120
 * bytes), and a fragmented stream has no bound on its runs. Refuse the stream
 * while enough room is left to withdraw a refused session and for the rest of
 * kboot's edits: the J704 has 209,123 bytes left after its whole handoff.
 */
#define T8142_DISPLAY_FDT_FLOOR (64 * 1024)

static bool fdt_room_left(void *dt)
{
    int used = fdt_off_dt_strings(dt) + fdt_size_dt_strings(dt);
    return (int)fdt_totalsize(dt) - used >= T8142_DISPLAY_FDT_FLOOR;
}

/* An IOVA-only node reserves the alias; the memreserve keeps its RAM CPU mapped. */
static int flush_mapping(struct live_maps *m)
{
    if (!m->size)
        return 0;
    void *dt = m->dt;
    if (!fdt_room_left(dt)) {
        printf("FDT: display SID%u: below %d bytes of FDT space; refused\n", m->stream.sid,
               T8142_DISPLAY_FDT_FLOOR);
        return -1;
    }
    m->runs++;
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
    m->pages++;
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

/*
 * Reserve every live mapping and page table of streams[0..2] (firmware,
 * scanout, PIODMA); this is the only place their page tables are reserved. The
 * internal session must translate RAM on all three. An idle external one has
 * no scanout or PIODMA mappings until its first modeset or PIODMA allocation,
 * and may have no translating root there yet; its firmware stream must still
 * map the resident firmware. A stream that is neither idle nor locked and
 * translating, or any failure once a root is seen, refuses the session.
 */
static int preserve_streams(void *dt, const char *alias, struct display_stream *streams,
                            bool internal)
{
    struct live_maps m = {.dt = dt};
    int chosen = adt_path_offset(adt, "/chosen");
    if (chosen < 0 || ADT_GETPROP(adt, chosen, "dram-base", &m.dram) != sizeof(m.dram) ||
        ADT_GETPROP(adt, chosen, "dram-size", &m.dram_size) != sizeof(m.dram_size) ||
        m.dram_size < SZ_16K || ((m.dram | m.dram_size) & (SZ_16K - 1)) ||
        m.dram > UINT64_MAX - m.dram_size) {
        printf("FDT: %s: no DRAM bounds in the ADT\n", alias);
        return -1;
    }
    /* One root plus the architectural maximum of 2048 L2 pages. */
    size_t capacity = 1 + SZ_16K / sizeof(u64);
    u64 *tables = malloc(capacity * sizeof(*tables));
    if (!tables)
        return -1;
    int ret = -1;
    for (size_t i = 0; i < 3; i++) {
        m.stream = streams[i];
        m.size = m.pages = m.ram_pages = m.runs = 0;
        bool optional = !internal && i;
        /* Only a stream with no valid root is idle; anything else unwalkable refuses. */
        int state = dart_t8110_stream_state(m.stream.base, m.stream.span, m.stream.sid);
        if (state < 0 || (!state && !optional)) {
            printf("FDT: %s SID%u: stream is not locked and translating\n", alias, m.stream.sid);
            goto out;
        }
        if (!state) {
            printf("FDT: %s SID%u: no translating root, nothing to preserve\n", alias,
                   m.stream.sid);
            continue;
        }
        /* dart_t8110_stream_state() bounded the SID by the T8110's 256 streams. */
        if (dart_walk_locked_t8110(m.stream.base, m.stream.span, (u8)m.stream.sid, m.dram,
                                   m.dram_size, preserve_page, &m) ||
            flush_mapping(&m)) {
            printf("FDT: %s SID%u: cannot preserve inherited translations\n", alias, m.stream.sid);
            goto out;
        }
        if (!m.ram_pages && !optional) {
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
            if (fdt_add_mem_rsv(dt, tables[j], SZ_16K)) {
                printf("FDT: %s SID%u: cannot reserve page tables\n", alias, m.stream.sid);
                goto out;
            }
        printf("FDT: %s SID%u: kept %lu pages (%lu RAM) in %u runs, %zu table pages\n", alias,
               m.stream.sid, m.pages, m.ram_pages, m.runs, count);
    }
    ret = 0;
out:
    free(tables);
    return ret;
}

/* The internal session: dcp SID 23 on dart-dcp, disp0 SID 0 and PIODMA SID 16 on dart-disp0. */
static int internal_streams(void *dt, struct display_stream *streams)
{
    const char *piodma = kboot_t8142_disp0_piodma_alias(dt);
    if (!piodma || resolve_stream(dt, "dcp", "/arm-io/dart-dcp", 23, streams) ||
        resolve_stream(dt, "disp0", "/arm-io/dart-disp0", 0, streams + 1) ||
        resolve_stream(dt, piodma, "/arm-io/dart-disp0", 16, streams + 2))
        return -1;
    return 0;
}

/*
 * Runs outside the FDT transaction: a refused session may still DMA, so the
 * RAM and tables proven live stay reserved.
 */
int kboot_t8142_display_reserve(void *dt)
{
    /* Do not probe optional DART apertures while the display island is off. */
    if (pmgr_adt_is_powered("/arm-io/dcp") != 1) {
        printf("FDT: dcp: display island is not powered\n");
        return -1;
    }
    struct display_stream streams[3];
    if (internal_streams(dt, streams)) {
        printf("FDT: dcp: display streams do not match the ADT\n");
        return -1;
    }
    return preserve_streams(dt, "dcp", streams, true);
}

/*
 * An external session, once its controller is known to be started and
 * powered: SID 23 of its firmware DART (owner dcpextN), SID 0 of its frontend
 * DART (owner disp0, which scans out for it) and the PIODMA stream.
 */
int kboot_t8142_dcpext_preserve(void *dt, const char *alias, const char *piodma)
{
    struct display_stream streams[3];
    int node = fdt_path_offset(dt, alias), len;
    const fdt32_t *fw = node < 0 ? NULL : fdt_getprop(dt, node, "iommus", &len);
    if (!fw || len != 8)
        return -1;
    u32 fw_dart = fdt32_ld(fw), fw_sid = fdt32_ld(fw + 1);
    node = fdt_path_offset(dt, piodma);
    const fdt32_t *pio = node < 0 ? NULL : fdt_getprop(dt, node, "iommus", &len);
    if (!pio || len != 8)
        return -1;
    u32 pio_dart = fdt32_ld(pio), pio_sid = fdt32_ld(pio + 1);
    if (stream_at(dt, alias, fw_dart, fw_sid, streams) ||
        stream_at(dt, "disp0", pio_dart, 0, streams + 1) ||
        stream_at(dt, piodma, pio_dart, pio_sid, streams + 2)) {
        printf("FDT: %s: display streams are not described\n", alias);
        return -1;
    }
    return preserve_streams(dt, alias, streams, false);
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
    const fdt32_t *cells =
        supplier < 0 ? NULL : fdt_getprop(dt, supplier, "#power-domain-cells", &len);
    if (!cells || len != sizeof(*cells) || fdt32_ld(cells) ||
        enable_power_domains(dt, parent, depth + 1))
        return -1;
    supplier = fdt_node_offset_by_phandle(dt, parent);
    return supplier < 0 ? -1 : fdt_setprop_string(dt, supplier, "status", "okay");
}

/* The internal session: its power chain, mailbox, DARTs and the three owners. */
int kboot_t8142_display_enable(void *dt)
{
    struct display_stream streams[3];
    if (internal_streams(dt, streams) || enable_power_domains(dt, streams[0].owner, 0))
        return -1;
    int controller = fdt_path_offset(dt, "dcp"), len;
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
    for (size_t i = 0; i < ARRAY_SIZE(streams); i++) {
        int node = fdt_node_offset_by_phandle(dt, streams[i].owner);
        if (node < 0 || fdt_setprop_string(dt, node, "status", "okay"))
            return -1;
    }
    return 0;
}

/*
 * A tree may list an external frontend DART on disp0 up front (linux#2603's
 * J704 HDMI tree). Drop the ones whose external DCP was not handed over, so
 * that a disabled supplier does not hold back the internal display.
 */
int kboot_t8142_display_prune(void *dt)
{
    int master = fdt_path_offset(dt, "disp0"), len;
    const fdt32_t *prop = master < 0 ? NULL : fdt_getprop(dt, master, "iommus", &len);
    fdt32_t keep[32];
    if (!prop || len <= 0 || len % 8 || len > (int)sizeof(keep))
        return -1;
    int count = 0;
    for (int i = 0; i < len / (int)sizeof(*prop); i += 2) {
        int provider = fdt_node_offset_by_phandle(dt, fdt32_ld(prop + i));
        if (provider < 0)
            return -1;
        const char *status = fdt_getprop(dt, provider, "status", NULL);
        /* The first entry is the internal scanout DART, published with the session. */
        if (i && status && strcmp(status, "okay") && strcmp(status, "ok"))
            continue;
        keep[count++] = prop[i];
        keep[count++] = prop[i + 1];
    }
    if (count * (int)sizeof(*keep) == len)
        return 0;
    return fdt_setprop(dt, master, "iommus", keep, count * sizeof(*keep));
}
