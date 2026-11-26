/* SPDX-License-Identifier: MIT */

#ifndef KBOOT_DISPLAY_ROUTES_H
#define KBOOT_DISPLAY_ROUTES_H

#include "types.h"

/* The routes a display processor node can carry: the binding's route@0..3 */
#define DCP_TYPEC_MAX_ROUTES 4
/* Direct DP outputs on one die considered, and the register windows kept per ATC PHY */
#define DCP_TYPEC_MAX_PORTS 8
#define DCP_TYPEC_PHY_REGS  64

/* One direct DisplayPort output of a Type-C port, as the ADT describes it */
struct dcp_typec_port_facts {
    u32 port;     /* port-number, shared by the output and the port's PD controller */
    u32 endpoint; /* dp-switch-dfp-endpoint: the DPTX target on its die */
    u32 cell;     /* dp-switch-dfp-port: the crossbar output (0: the DP PHY) */
    u64 xbar;     /* the crossbar's register window, CPU address */
    u64 bus;      /* the PD controller's bus's register window, CPU address */
    u32 usid;     /* the PD controller's address on that bus */
    u32 nphy;
    u64 phy[DCP_TYPEC_PHY_REGS]; /* every register window of the port's ATC PHY */
};

/* A display processor's place on the display crossbar, and the outputs on its die */
struct dcp_typec_facts {
    u32 domain; /* dp-switch-ufp-domain: its die */
    u32 source; /* crossbar state that selects it: 2 * ufp-endpoint + ufp-port */
    u32 index;  /* its die-local index (apple,dcp-index): ufp-endpoint + 1 */
    u32 nports;
    struct dcp_typec_port_facts ports[DCP_TYPEC_MAX_PORTS];
};

enum { DCP_TYPEC_ROUTE_DONE = -1, DCP_TYPEC_ROUTE_BAD = -2 };

int dcp_typec_facts_from_adt(const char *adt_engine, struct dcp_typec_facts *f);
bool dcp_typec_atc_handoff_pending(void *dt, int target);
int dcp_typec_route_controller(void *dt, int ep);
int dcp_typec_route_node(void *dt, int target, int depth, bool all);
int dcp_typec_route_refs(void *dt, int node, const char *prop, const char *cells, int depth,
                         bool all);
int dcp_typec_routes_write(void *dt, const char *engine, const struct dcp_typec_facts *f);
int kboot_dcp_typec_routes(void *dt, const char *engine, const char *adt_engine);

#endif
