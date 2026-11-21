/* SPDX-License-Identifier: MIT */

/*
 * What the block trace (sptm_virt_trace.c) takes from the ADT, apart from the hooks, so the host
 * runs it on a real ADT (tests/python/test_sptm_virt_trace_host.py): the /arm-io devices of the
 * debug link's USB-C port, which nothing may hook, and whether a range overlaps one.
 */

#include "adt.h"
#include "sptm_virt_s2r_soc.h"
#include "sptm_virt_trace.h"
#include "string.h"
#include "utils.h"

/* The /arm-io devices nothing may hook: the debug link's port. */
#define TRACE_REFUSE 24
static char trace_refuse[TRACE_REFUSE][24];
static u32 trace_nrefuse;
static bool trace_link_known;
static char trace_atc_pfx[2][8]; /* "atc<n>-": the ports' DP crossbars, DP inputs and PHYs */

static void trace_refuse_add(const char *name)
{
    for (u32 i = 0; i < trace_nrefuse; i++)
        if (!strcmp(trace_refuse[i], name))
            return;
    if (trace_nrefuse < TRACE_REFUSE)
        strncpy(trace_refuse[trace_nrefuse++], name, sizeof(trace_refuse[0]) - 1);
}

/*
 * From the PHYs the host hid as the debug link (sptm_virt_link_phys): each port's USB controller,
 * DART and PHY (whose reg[3] is its whole ATC), its USB4 and PCIe tunnel side (acio<n>,
 * acio-cpu<n>, dart-acio<n>, apciec<n>..., atc<n>-*), and the /arm-io buses of its port controller
 * (the hpm with its port-number and port-type) and of its retimer (atcrt<n>): run_guest_sptm's
 * debug_port() nodes. False: no port found.
 */
bool sptm_virt_trace_adt_link(const u32 *phys, int n_phys)
{
    struct s2r_soc soc;
    int io = adt_path_offset(adt, "/arm-io");

    trace_nrefuse = 0;
    memset(trace_atc_pfx, 0, sizeof(trace_atc_pfx));
    trace_link_known = false;
    if (io < 0 || s2r_soc_link_only(adt, phys, n_phys, &soc) < 0)
        return false;
    for (int i = 0; i < soc.n_link; i++) {
        const char *name = soc.link_paths[i] + 8; /* past "/arm-io/" */
        int phy = adt_subnode_offset(adt, io, name);
        u32 num, type, n2, t2;
        trace_refuse_add(name);
        if (strncmp(name, "atc-phy", 7) || phy < 0)
            continue;
        const char *idx = name + 7;
        char n[24];
        snprintf(n, sizeof(n), "acio%s", idx);
        trace_refuse_add(n);
        snprintf(n, sizeof(n), "acio-cpu%s", idx);
        trace_refuse_add(n);
        snprintf(n, sizeof(n), "dart-acio%s", idx);
        trace_refuse_add(n);
        snprintf(n, sizeof(n), "apciec%s", idx);
        trace_refuse_add(n);
        snprintf(n, sizeof(n), "apciec%s-piodma", idx);
        trace_refuse_add(n);
        snprintf(n, sizeof(n), "dart-apciec%s", idx);
        trace_refuse_add(n);
        for (int k = 0; k < 2; k++)
            if (!trace_atc_pfx[k][0] || k == 1) {
                snprintf(trace_atc_pfx[k], sizeof(trace_atc_pfx[k]), "atc%s-", idx);
                break;
            }
        bool port = ADT_GETPROP(adt, phy, "port-number", &num) >= 0 &&
                    ADT_GETPROP(adt, phy, "port-type", &type) >= 0;
        snprintf(n, sizeof(n), "atcrt%s", idx); /* its retimer */
        int bus = io;
        ADT_FOREACH_CHILD(adt, bus)
        {
            int kid = bus;
            ADT_FOREACH_CHILD(adt, kid)
            {
                const char *kn = adt_get_name(adt, kid);
                if (!strcmp(kn, n) ||
                    (port && !strncmp(kn, "hpm", 3) &&
                     ADT_GETPROP(adt, kid, "port-number", &n2) >= 0 &&
                     ADT_GETPROP(adt, kid, "port-type", &t2) >= 0 && n2 == num && t2 == type))
                    trace_refuse_add(adt_get_name(adt, bus));
            }
        }
    }
    trace_link_known = true;
    return true;
}

/* The refused /arm-io device a range overlaps, or NULL. With the link unknown, every USB-C
 * port's devices are refused. */
const char *sptm_virt_trace_adt_refused(u64 pa, u64 len)
{
    bool link = trace_link_known;
    int node = adt_path_offset(adt, "/arm-io");

    if (node < 0)
        return "/arm-io";
    ADT_FOREACH_CHILD(adt, node)
    {
        const char *name = adt_get_name(adt, node);
        bool refuse = false;
        if (link) {
            for (u32 i = 0; i < trace_nrefuse; i++)
                refuse |= !strcmp(name, trace_refuse[i]);
            for (int k = 0; k < 2; k++)
                refuse |= trace_atc_pfx[k][0] &&
                          !strncmp(name, trace_atc_pfx[k], strlen(trace_atc_pfx[k]));
        } else {
            refuse = !strncmp(name, "atc", 3) || !strncmp(name, "usb-drd", 7) ||
                     !strncmp(name, "dart-usb", 8) || !strncmp(name, "acio", 4) ||
                     !strncmp(name, "dart-acio", 9) || !strncmp(name, "apciec", 6) ||
                     !strncmp(name, "dart-apciec", 11);
            int kid = node; /* and every bus with a port controller or a retimer on it */
            ADT_FOREACH_CHILD(adt, kid)
            {
                const char *kn = adt_get_name(adt, kid);
                refuse |= !strncmp(kn, "hpm", 3) || !strncmp(kn, "atcrt", 5);
            }
        }
        char path[40];
        int p[8];
        snprintf(path, sizeof(path), "/arm-io/%s", name);
        if (!refuse || adt_path_offset_trace(adt, path, p) < 0)
            continue;
        for (int i = 0; i < 32; i++) {
            u64 a, sz;
            if (adt_get_reg(adt, p, "reg", i, &a, &sz) < 0)
                break;
            if (pa < a + sz && a < pa + len)
                return name;
        }
    }
    return NULL;
}
