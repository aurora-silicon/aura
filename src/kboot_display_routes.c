/* SPDX-License-Identifier: MIT */

/*
 * Type-C display routes from the ADT.
 *
 * A display processor node whose tree carries an empty typec-routes container asks for its
 * Type-C display routes (linux apple,dcp-typec-routes.yaml). The ADT describes them:
 *
 *  - the processor's node: its display crossbar (dp-switch-parent), die (dp-switch-ufp-domain)
 *    and crossbar input (dp-switch-ufp-endpoint and -port), so the crossbar state that selects
 *    it is 2 * endpoint + port on a crossbar of the T8142 layout;
 *  - each Type-C port's direct DP output (transport-tunneled 0): the same crossbar, its die
 *    (dp-switch-dfp-domain), DPTX target (dp-switch-dfp-endpoint), crossbar output cell
 *    (dp-switch-dfp-port), its ATC PHY (atc-phy), its port's crossbar (dpxbar-parent) and
 *    port-number;
 *  - the port's PD controller: the node below a bus with the same port-number and port-type
 *    (when both carry one: port numbers count per port type, so a MagSafe controller can share
 *    a USB-C port's number), at its address on that bus.
 *
 * Every output on the processor's die whose PHY, crossbar and PD controller the tree describes
 * becomes a route, matched by register address: the tree decides which ports Linux may have
 * (a port whose PHY it does not describe, or describes reserved, gets none). Nothing here
 * depends on the chip or the board. A tree that carries its own routes keeps them. A processor
 * that also drives a fixed output (an HDMI port's DP PHY, named dp-phy, and its crossbar output,
 * dp-xbar) keeps that first: the routes are written after it, a hybrid pipeline.
 */

#include "kboot_display_routes.h"
#include "adt.h"
#include "kboot.h"
#include "string.h"
#include "utils.h"

#include "libfdt/libfdt.h"

#define rlog(fmt, ...) printf("DCP routes: %s: " fmt, name, ##__VA_ARGS__)

#define ROUTES_TEXT AURA_XTEXT

/* <dt-bindings/phy/phy.h> */
#define PHY_TYPE_DP 6

#define ROUTE_PATH 160

static ROUTES_TEXT int adt_u32(int node, const char *prop, u32 *val)
{
    return ADT_GETPROP(adt, node, prop, val) == sizeof(*val) ? 0 : -1;
}

/* The /arm-io child with this AAPL,phandle, and its path */
static ROUTES_TEXT int adt_by_phandle(u32 ph, char *path, size_t len)
{
    int arm = adt_path_offset(adt, "/arm-io");
    if (arm < 0)
        return -1;
    int child = arm;
    ADT_FOREACH_CHILD(adt, child)
    {
        u32 v;
        if (!adt_u32(child, "AAPL,phandle", &v) && v == ph) {
            snprintf(path, len, "/arm-io/%s", adt_get_name(adt, child));
            return child;
        }
    }
    return -1;
}

static ROUTES_TEXT int adt_reg_at(const char *path, int idx, u64 *addr)
{
    int trace[8];
    if (adt_path_offset_trace(adt, path, trace) < 0)
        return -1;
    return adt_get_reg(adt, trace, "reg", idx, addr, NULL) ? -1 : 0;
}

/*
 * The PD controller of port `port`: a node below an /arm-io bus with that port-number and, if
 * both the output and the controller name one, the output's port-type (`type`, ~0 for none)
 */
static ROUTES_TEXT int adt_port_controller(const char *name, u32 port, u32 type,
                                           struct dcp_typec_port_facts *p)
{
    int arm = adt_path_offset(adt, "/arm-io"), found = 0;
    if (arm < 0)
        return -1;
    int bus = arm;
    ADT_FOREACH_CHILD(adt, bus)
    {
        if (!adt_get_child_count(adt, bus))
            continue;
        int hpm = bus;
        ADT_FOREACH_CHILD(adt, hpm)
        {
            u32 v, len = 0;
            const u32 *reg = adt_getprop(adt, hpm, "reg", &len);
            if (adt_u32(hpm, "port-number", &v) || v != port || !reg || len < 4)
                continue;
            if (type != ~0U && !adt_u32(hpm, "port-type", &v) && v != type)
                continue; /* another kind of port with the same number */
            char path[64];
            snprintf(path, sizeof(path), "/arm-io/%s", adt_get_name(adt, bus));
            if (adt_reg_at(path, 0, &p->bus))
                continue;
            p->usid = reg[0];
            found++;
        }
    }
    if (found != 1 && type != ~0U)
        rlog("port %u (port-type %u): %d PD controllers in the ADT, not one: no route\n", port,
             type, found);
    else if (found != 1)
        rlog("port %u: %d PD controllers in the ADT, not one: no route\n", port, found);
    return found == 1 ? 0 : -1;
}

/* The ADT's description of `adt_engine` (an /arm-io path) and the direct DP outputs on its die */
ROUTES_TEXT int dcp_typec_facts_from_adt(const char *adt_engine, struct dcp_typec_facts *f)
{
    const char *name = adt_engine;
    u32 ep, port, parent, domain = 0;
    int eng = adt_path_offset(adt, adt_engine), arm = adt_path_offset(adt, "/arm-io");

    memset(f, 0, sizeof(*f));
    if (eng < 0 || arm < 0 || adt_u32(eng, "dp-switch-ufp-endpoint", &ep) ||
        adt_u32(eng, "dp-switch-ufp-port", &port) || adt_u32(eng, "dp-switch-parent", &parent)) {
        rlog("no display crossbar input in the ADT: no routes\n");
        return -1;
    }
    adt_u32(eng, "dp-switch-ufp-domain", &domain);
    f->domain = domain;
    f->source = 2 * ep + port;
    f->index = ep + 1;

    int out = arm;
    ADT_FOREACH_CHILD(adt, out)
    {
        u32 xparent, tunneled, odomain = 0, phy_ph, xbar_ph, type = ~0U, v;
        struct dcp_typec_port_facts p = {0};
        char path[64];

        if (adt_u32(out, "dp-switch-parent", &xparent) || xparent != parent ||
            adt_u32(out, "transport-tunneled", &tunneled) || tunneled ||
            adt_u32(out, "dp-switch-dfp-endpoint", &p.endpoint) ||
            adt_u32(out, "dp-switch-dfp-port", &p.cell) || adt_u32(out, "port-number", &p.port) ||
            adt_u32(out, "atc-phy", &phy_ph) || adt_u32(out, "dpxbar-parent", &xbar_ph))
            continue;
        adt_u32(out, "dp-switch-dfp-domain", &odomain);
        if (!adt_u32(out, "port-type", &v))
            type = v;
        if (odomain != domain)
            continue;
        if (adt_by_phandle(xbar_ph, path, sizeof(path)) < 0 || adt_reg_at(path, 0, &p.xbar)) {
            rlog("port %u: no crossbar register window in the ADT: no route\n", p.port);
            continue;
        }
        if (adt_by_phandle(phy_ph, path, sizeof(path)) < 0) {
            rlog("port %u: no ATC PHY in the ADT: no route\n", p.port);
            continue;
        }
        while (p.nphy < DCP_TYPEC_PHY_REGS && !adt_reg_at(path, p.nphy, &p.phy[p.nphy]))
            p.nphy++;
        if (!p.nphy || adt_port_controller(name, p.port, type, &p))
            continue;
        if (f->nports == DCP_TYPEC_MAX_PORTS) {
            rlog("more than %d DP outputs on die %u: port %u and later not considered\n",
                 DCP_TYPEC_MAX_PORTS, domain, p.port);
            break;
        }
        /* In port-number order */
        u32 i = f->nports++;
        for (; i && f->ports[i - 1].port > p.port; i--)
            f->ports[i] = f->ports[i - 1];
        f->ports[i] = p;
    }
    return 0;
}

/* ---- the device tree side: no ADT access below ---- */

/* A node of `dt` whose first register window starts at one of `addr` and has `need` */
static ROUTES_TEXT int dt_node_at(void *dt, const u64 *addr, u32 n, const char *need)
{
    for (int node = fdt_next_node(dt, -1, NULL); node >= 0; node = fdt_next_node(dt, node, NULL)) {
        int len, parent = fdt_parent_offset(dt, node);
        const fdt64_t *reg = fdt_getprop(dt, node, "reg", &len);
        if (!reg || len < 8 || parent < 0 || fdt_address_cells(dt, parent) != 2 ||
            (need && !fdt_getprop(dt, node, need, NULL)))
            continue;
        for (u32 i = 0; i < n; i++)
            if (fdt64_ld(&reg[0]) == addr[i])
                return node;
    }
    return -1;
}

/* Enabled for Linux already: no status, "okay" or "ok" */
static ROUTES_TEXT bool dt_enabled(void *dt, int node)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return !s || !strcmp(s, "okay") || !strcmp(s, "ok");
}

/* Described for Linux to take: enabled or "disabled" (the handoff enables it) */
static ROUTES_TEXT bool dt_takeable(void *dt, int node)
{
    const char *s = fdt_getprop(dt, node, "status", NULL);
    return dt_enabled(dt, node) || (s && !strcmp(s, "disabled"));
}

/*
 * True if `target` belongs to an apple,atc-handoff group (apple,phy or apple,devices) that its
 * producer has not activated yet. Every member starts disabled and stays disabled until the
 * producer relinquishes USB/debug ownership and enables the whole group; display route
 * publication and the handoff walk must not route or enable it before then. A group with
 * missing, malformed or unresolvable apple,phy / apple,devices fails closed.
 */
ROUTES_TEXT bool dcp_typec_atc_handoff_pending(void *dt, int target)
{
    if (target < 0)
        return false;
    u32 tph = fdt_get_phandle(dt, target);
    if (!tph)
        return false;
    for (int g = fdt_node_offset_by_compatible(dt, -1, "apple,atc-handoff"); g >= 0;
         g = fdt_node_offset_by_compatible(dt, g, "apple,atc-handoff")) {
        int plen = 0, dlen = 0;
        const fdt32_t *phy = fdt_getprop(dt, g, "apple,phy", &plen);
        const fdt32_t *devs = fdt_getprop(dt, g, "apple,devices", &dlen);
        bool claimed = phy && plen >= 4 && fdt32_ld(phy) == tph;
        for (int i = 0; !claimed && devs && i < dlen / 4; i++)
            if (fdt32_ld(&devs[i]) == tph)
                claimed = true;
        if (!claimed)
            continue;
        if (!phy || plen != 4 || !devs || dlen <= 0 || (dlen & 3))
            return true;
        int pnode = fdt_node_offset_by_phandle(dt, fdt32_ld(phy));
        if (pnode < 0 || !dt_enabled(dt, pnode))
            return true;
        for (int i = 0; i < dlen / 4; i++) {
            int dnode = fdt_node_offset_by_phandle(dt, fdt32_ld(&devs[i]));
            if (dnode < 0 || !dt_enabled(dt, dnode))
                return true;
        }
    }
    return false;
}

/*
 * The Type-C PD controller owning a connector graph endpoint: walk upward from `ep` to the
 * usb-c-connector node, then return its parent (the controller on the bus), or a negative value.
 */
ROUTES_TEXT int dcp_typec_route_controller(void *dt, int ep)
{
    int n = ep;
    for (int up = 0; n >= 0 && up < 5; up++) {
        if (!fdt_node_check_compatible(dt, n, "usb-c-connector"))
            return fdt_parent_offset(dt, n);
        n = fdt_parent_offset(dt, n);
    }
    return -1;
}

/* `target`, then its power domains; with `all` checks every node without stopping at disabled */
ROUTES_TEXT int dcp_typec_route_node(void *dt, int target, int depth, bool all)
{
    const char *status = target < 0 ? NULL : fdt_getprop(dt, target, "status", NULL);
    if (target < 0 || depth > 4 || dcp_typec_atc_handoff_pending(dt, target))
        return DCP_TYPEC_ROUTE_BAD;
    if (!all && status && !strcmp(status, "disabled"))
        return target;
    return dcp_typec_route_refs(dt, target, "power-domains", "#power-domain-cells", depth + 1, all);
}

/* Each target of a phandle list, its arguments counted by the target's `cells` property */
ROUTES_TEXT int dcp_typec_route_refs(void *dt, int node, const char *prop, const char *cells,
                                     int depth, bool all)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, prop, &len);
    for (int i = 0; v && i < len / 4;) {
        int target = fdt_node_offset_by_phandle(dt, fdt32_ld(&v[i]));
        const fdt32_t *n = target < 0 ? NULL : fdt_getprop(dt, target, cells, NULL);
        int r = dcp_typec_route_node(dt, target, depth, all);
        if (r != DCP_TYPEC_ROUTE_DONE)
            return r;
        i += 1 + (n ? fdt32_ld(n) : 0);
        if (i > len / 4)
            return DCP_TYPEC_ROUTE_BAD;
    }
    return DCP_TYPEC_ROUTE_DONE;
}

static ROUTES_TEXT u32 dt_cells(void *dt, int node, const char *prop)
{
    int len;
    const fdt32_t *v = fdt_getprop(dt, node, prop, &len);
    return v && len == 4 ? fdt32_ld(v) : ~0U;
}

static ROUTES_TEXT int dt_phandle(void *dt, const char *path, u32 *ph)
{
    int node = kboot_fdt_path_exact(dt, path);
    if (node < 0)
        return -1;
    *ph = fdt_get_phandle(dt, node);
    if (*ph)
        return 0;
    return fdt_generate_phandle(dt, ph) || fdt_setprop_u32(dt, node, "phandle", *ph) ? -1 : 0;
}

/* The offset just past `node`'s subtree */
static ROUTES_TEXT int dt_node_end(void *dt, int node)
{
    int depth = 0, next, offset = node;
    do {
        u32 tag = fdt_next_tag(dt, offset, &next);
        if (tag == FDT_BEGIN_NODE)
            depth++;
        else if (tag == FDT_END_NODE)
            depth--;
        else if (tag == FDT_END || next < 0)
            return -1;
        offset = next;
    } while (depth > 0);
    return offset;
}

static ROUTES_TEXT void dt_reverse(u8 *p, int len)
{
    for (int i = 0, j = len - 1; i < j; i++, j--) {
        u8 b = p[i];
        p[i] = p[j];
        p[j] = b;
    }
}

/*
 * Move `node`, which libfdt adds as its parent's first child, after its siblings, so nodes go
 * in the order they are written: the Type-C class reads a connector's graph in tree order and
 * keeps only the first few mode switches, so port@2 must follow the PHY's port@1. The bytes
 * of the node and of the siblings after it swap places (a rotation): no size, string or
 * phandle changes.
 */
static ROUTES_TEXT int dt_move_last(void *dt, int node)
{
    int end = dt_node_end(dt, node), last = end;
    for (int sib = fdt_next_subnode(dt, node); sib >= 0; sib = fdt_next_subnode(dt, sib))
        last = dt_node_end(dt, sib);
    if (end < 0 || last < 0)
        return -1;
    u8 *p = fdt_offset_ptr_w(dt, node, last - node);
    if (!p)
        return -1;
    dt_reverse(p, last - node);
    dt_reverse(p, last - end);
    dt_reverse(p + (last - end), end - node);
    return 0;
}

/* `path`'s child `child`, after its siblings, created with this reg if absent; path in `out` */
static ROUTES_TEXT int dt_child(void *dt, const char *path, const char *child, int reg, char *out)
{
    int node = kboot_fdt_path_exact(dt, path);
    if (node < 0)
        return -1;
    snprintf(out, ROUTE_PATH, "%s/%s", path, child);
    if (fdt_subnode_offset(dt, node, child) >= 0)
        return 0;
    node = fdt_add_subnode(dt, node, child);
    if (node < 0 || (reg >= 0 && fdt_setprop_u32(dt, node, "reg", reg)))
        return -1;
    return dt_move_last(dt, kboot_fdt_path_exact(dt, out));
}

static ROUTES_TEXT int dt_bus_cells(void *dt, const char *path)
{
    int node = kboot_fdt_path_exact(dt, path);
    return node < 0 || fdt_setprop_u32(dt, node, "#address-cells", 1) ||
                   fdt_setprop_u32(dt, node, "#size-cells", 0)
               ? -1
               : 0;
}

struct route_target {
    char phy[ROUTE_PATH], xbar[ROUTE_PATH], connector[ROUTE_PATH];
    u32 phy_cells, endpoint, cell, next_ep;
};

/*
 * The connector's display port (port@2) takes another route endpoint: it is absent, or holds
 * only numbered endpoints of other processors' routes. *next is the first free number.
 */
static ROUTES_TEXT bool dt_display_port_free(void *dt, int connector, u32 *next)
{
    int ports = fdt_subnode_offset(dt, connector, "ports");
    int port = ports < 0 ? -1 : fdt_subnode_offset(dt, ports, "port@2");
    *next = 0;
    if (port < 0)
        return true;
    int ep;
    fdt_for_each_subnode(ep, dt, port)
    {
        int len;
        const fdt32_t *reg = fdt_getprop(dt, ep, "reg", &len);
        const fdt32_t *remote = fdt_getprop(dt, ep, "remote-endpoint", NULL);
        int r = remote ? fdt_node_offset_by_phandle(dt, fdt32_ld(remote)) : -1;
        for (int up = 0; r >= 0 && up < 3; up++)
            r = fdt_parent_offset(dt, r);
        if (!reg || len != 4 || r < 0 || strcmp(fdt_get_name(dt, r, NULL), "typec-routes"))
            return false;
        *next = max(*next, fdt32_ld(reg) + 1);
    }
    return true;
}

/* The tree's nodes for port `p`, or false (logged) if it does not describe them all */
static ROUTES_TEXT bool dt_route_target(void *dt, const char *name,
                                        const struct dcp_typec_port_facts *p,
                                        struct route_target *t)
{
    int phy = dt_node_at(dt, p->phy, p->nphy, "#phy-cells");
    int xbar = dt_node_at(dt, &p->xbar, 1, "#mux-control-cells");
    int bus = dt_node_at(dt, &p->bus, 1, NULL), hpm = -1, connector = -1;

    if (bus >= 0) {
        int child;
        fdt_for_each_subnode(child, dt, bus)
        {
            int len;
            const fdt32_t *reg = fdt_getprop(dt, child, "reg", &len);
            if (reg && len >= 4 && fdt32_ld(reg) == p->usid)
                hpm = child;
        }
    }
    connector = hpm < 0 ? -1 : fdt_subnode_offset(dt, hpm, "connector");
    t->phy_cells = phy < 0 ? ~0U : dt_cells(dt, phy, "#phy-cells");

    const char *why = NULL;
    if (phy < 0 || !dt_takeable(dt, phy) || t->phy_cells > 1)
        why = "its ATC PHY is not described for Linux";
    else if (!fdt_getprop(dt, phy, "mode-switch", NULL))
        why = "its ATC PHY is no Type-C mode switch";
    else if (xbar < 0 || !dt_takeable(dt, xbar) || dt_cells(dt, xbar, "#mux-control-cells") != 1)
        why = "its display crossbar is not described for Linux";
    else if (fdt_node_check_compatible(dt, xbar, "apple,t8142-display-crossbar"))
        /* The state encoding above is that crossbar's */
        why = "its display crossbar is not of the T8142 layout";
    else if (hpm < 0 || !dt_takeable(dt, hpm) || connector < 0 ||
             fdt_node_check_compatible(dt, connector, "usb-c-connector"))
        why = "its PD controller and connector are not described for Linux";
    else if (dcp_typec_route_node(dt, phy, 0, true) != DCP_TYPEC_ROUTE_DONE ||
             dcp_typec_route_refs(dt, phy, "mboxes", "#mbox-cells", 0, true) !=
                 DCP_TYPEC_ROUTE_DONE ||
             dcp_typec_route_node(dt, xbar, 0, true) != DCP_TYPEC_ROUTE_DONE ||
             dcp_typec_route_node(dt, hpm, 0, true) != DCP_TYPEC_ROUTE_DONE ||
             dcp_typec_route_node(dt, bus, 0, true) != DCP_TYPEC_ROUTE_DONE)
        why = "its route dependencies are incomplete or reserved by an un-activated "
              "apple,atc-handoff group";
    else if (!dt_display_port_free(dt, connector, &t->next_ep))
        why = "its connector's port@2 holds something else";
    if (why) {
        rlog("port %u: %s: no route\n", p->port, why);
        return false;
    }
    t->endpoint = p->endpoint;
    t->cell = p->cell;
    return !fdt_get_path(dt, phy, t->phy, ROUTE_PATH) &&
           !fdt_get_path(dt, xbar, t->xbar, ROUTE_PATH) &&
           !fdt_get_path(dt, connector, t->connector, ROUTE_PATH);
}

/* Route `k` of `engine` to the connector of `t`: the route node and both graph endpoints */
static ROUTES_TEXT int dt_route_graph(void *dt, const char *engine, int k,
                                      const struct route_target *t)
{
    char routes[ROUTE_PATH], route[ROUTE_PATH], port[ROUTE_PATH], ep[ROUTE_PATH];
    char ports[ROUTE_PATH], display[ROUTE_PATH], cep[ROUTE_PATH], rname[16], ename[20];
    u32 ph_ep, ph_cep;

    snprintf(routes, sizeof(routes), "%s/typec-routes", engine);
    snprintf(rname, sizeof(rname), "route@%d", k);
    snprintf(ename, sizeof(ename), "endpoint@%u", t->next_ep);
    if (dt_child(dt, routes, rname, k, route) || dt_child(dt, route, "port", -1, port) ||
        dt_child(dt, port, "endpoint", -1, ep) ||
        fdt_setprop_empty(dt, kboot_fdt_path_exact(dt, route), "mode-switch") ||
        dt_phandle(dt, ep, &ph_ep))
        return -1;

    int connector = kboot_fdt_path_exact(dt, t->connector);
    bool new_ports = connector < 0 || fdt_subnode_offset(dt, connector, "ports") < 0;
    if (dt_child(dt, t->connector, "ports", -1, ports) || (new_ports && dt_bus_cells(dt, ports)))
        return -1;
    int pnode = kboot_fdt_path_exact(dt, ports);
    bool new_display = pnode < 0 || fdt_subnode_offset(dt, pnode, "port@2") < 0;
    if (dt_child(dt, ports, "port@2", 2, display) || (new_display && dt_bus_cells(dt, display)) ||
        dt_child(dt, display, ename, t->next_ep, cep) || dt_phandle(dt, cep, &ph_cep) ||
        fdt_setprop_u32(dt, kboot_fdt_path_exact(dt, cep), "remote-endpoint", ph_ep) ||
        fdt_setprop_u32(dt, kboot_fdt_path_exact(dt, ep), "remote-endpoint", ph_cep))
        return -1;
    return 0;
}

/*
 * The fixed output `node` names before its routes are written: none, or exactly one entry
 * named `name` in `prop` (`names` holding the names). Copies it to `out` (`*len` bytes, at most
 * two cells) and returns 0, or returns -1 if the property holds anything else.
 */
static ROUTES_TEXT int dt_fixed_output(void *dt, int node, const char *prop, const char *names,
                                       const char *name, fdt32_t *out, int *len)
{
    int plen, nlen;
    const fdt32_t *v = fdt_getprop(dt, node, prop, &plen);
    const char *n = fdt_getprop(dt, node, names, &nlen);

    *len = 0;
    if (!v)
        return n ? -1 : 0;
    if (!n || nlen != (int)strlen(name) + 1 || strcmp(n, name) || plen < 4 || plen > 8 || plen % 4)
        return -1;
    memcpy(out, v, plen);
    *len = plen;
    return 0;
}

/*
 * Writes the routes of the processor at `engine` (a DT path) from `f`, if its node asks for
 * them (an empty typec-routes). Returns the number written, 0 if none was asked for or
 * possible, -1 if a write failed (the caller discards the transaction).
 */
ROUTES_TEXT int dcp_typec_routes_write(void *dt, const char *engine,
                                       const struct dcp_typec_facts *f)
{
    const char *name = engine;
    static struct route_target t[DCP_TYPEC_MAX_ROUTES];
    int node = kboot_fdt_path_exact(dt, engine);
    int routes = node < 0 ? -1 : fdt_subnode_offset(dt, node, "typec-routes");
    int n = 0;

    if (routes < 0 || fdt_first_subnode(dt, routes) >= 0)
        return 0;
    /* A fixed output's PHY and crossbar output stay first */
    fdt32_t fixed_phy[2], fixed_mux[2];
    int fixed_phy_len, fixed_mux_len;
    if (dt_fixed_output(dt, node, "phys", "phy-names", "dp-phy", fixed_phy, &fixed_phy_len) ||
        dt_fixed_output(dt, node, "mux-controls", "mux-control-names", "dp-xbar", fixed_mux,
                        &fixed_mux_len) ||
        (fixed_mux_len && !fixed_phy_len)) {
        rlog("asks for its Type-C routes but names PHYs or crossbars already: no routes\n");
        return 0;
    }
    for (u32 i = 0; i < f->nports; i++) {
        if (n == DCP_TYPEC_MAX_ROUTES) {
            rlog("port %u: the node takes %d routes: no route\n", f->ports[i].port,
                 DCP_TYPEC_MAX_ROUTES);
            continue;
        }
        if (dt_route_target(dt, name, &f->ports[i], &t[n]))
            n++;
    }
    if (!n) {
        rlog("no port to route on die %u\n", f->domain);
        return 0;
    }

    fdt32_t phys[2 * DCP_TYPEC_MAX_ROUTES + 2], mux[2 * DCP_TYPEC_MAX_ROUTES + 2];
    fdt32_t states[DCP_TYPEC_MAX_ROUTES], targets[DCP_TYPEC_MAX_ROUTES];
    char phy_names[DCP_TYPEC_MAX_ROUTES * 8 + 8], mux_names[DCP_TYPEC_MAX_ROUTES * 8 + 8];
    int nphys = fixed_phy_len / 4, nmux = fixed_mux_len / 4;
    int nphy_names = nphys ? (int)sizeof("dp-phy") : 0;
    int nmux_names = nmux ? (int)sizeof("dp-xbar") : 0;
    memcpy(phys, fixed_phy, fixed_phy_len);
    memcpy(mux, fixed_mux, fixed_mux_len);
    memcpy(phy_names, "dp-phy", nphy_names);
    memcpy(mux_names, "dp-xbar", nmux_names);
    for (int k = 0; k < n; k++) {
        u32 ph_phy, ph_xbar;
        if (dt_phandle(dt, t[k].phy, &ph_phy) || dt_phandle(dt, t[k].xbar, &ph_xbar))
            return -1;
        phys[nphys++] = cpu_to_fdt32(ph_phy);
        if (t[k].phy_cells)
            phys[nphys++] = cpu_to_fdt32(PHY_TYPE_DP);
        mux[nmux++] = cpu_to_fdt32(ph_xbar);
        mux[nmux++] = cpu_to_fdt32(t[k].cell);
        states[k] = cpu_to_fdt32(f->source);
        targets[k] = cpu_to_fdt32(t[k].endpoint);
        nphy_names +=
            snprintf(phy_names + nphy_names, sizeof(phy_names) - nphy_names, "typec%d", k) + 1;
        nmux_names +=
            snprintf(mux_names + nmux_names, sizeof(mux_names) - nmux_names, "typec%d", k) + 1;
    }

    node = kboot_fdt_path_exact(dt, engine);
    /* Properties added to a node move the nodes after it, not the node itself */
    if (node < 0 || fdt_setprop(dt, node, "phys", phys, nphys * 4) ||
        fdt_setprop(dt, node, "phy-names", phy_names, nphy_names) ||
        fdt_setprop(dt, node, "mux-controls", mux, nmux * 4) ||
        fdt_setprop(dt, node, "mux-control-names", mux_names, nmux_names) ||
        fdt_setprop(dt, node, "apple,typec-mux-indices", states, n * 4) ||
        fdt_setprop(dt, node, "apple,typec-dptx-phys", targets, n * 4) ||
        fdt_setprop_u32(dt, node, "apple,dptx-die", f->domain))
        return -1;

    /* The die-local index the processor's peers are paired by */
    u32 index = dt_cells(dt, kboot_fdt_path_exact(dt, engine), "apple,dcp-index");
    if (index == ~0U) {
        if (fdt_setprop_u32(dt, kboot_fdt_path_exact(dt, engine), "apple,dcp-index", f->index))
            return -1;
    } else if (index != f->index) {
        rlog("apple,dcp-index %u is not the ADT's %u: the tree's kept\n", index, f->index);
    }

    for (int k = 0; k < n; k++)
        if (dt_route_graph(dt, engine, k, &t[k]))
            return -1;
    for (int k = 0; k < n; k++)
        rlog("route@%d: %s (DPTX %u:%u, crossbar %s cell %u, state %u)\n", k, t[k].connector,
             f->domain, t[k].endpoint, t[k].xbar, t[k].cell, f->source);
    return n;
}

/* The routes of the processor at DT path `engine`, whose ADT node is `adt_engine` */
ROUTES_TEXT int kboot_dcp_typec_routes(void *dt, const char *engine, const char *adt_engine)
{
    static struct dcp_typec_facts f;
    int node = kboot_fdt_path_exact(dt, engine);
    int routes = node < 0 ? -1 : fdt_subnode_offset(dt, node, "typec-routes");

    if (routes < 0 || fdt_first_subnode(dt, routes) >= 0)
        return 0;
    if (dcp_typec_facts_from_adt(adt_engine, &f))
        return 0;
    return dcp_typec_routes_write(dt, engine, &f);
}
