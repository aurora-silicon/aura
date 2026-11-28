# SPDX-License-Identifier: MIT
"""Type-C display routes written from the ADT (src/kboot_display_routes.c).

The ADT model has the shape of a two-die T6050 desktop's display crossbar: four rear Type-C
ports on die 0 (port 1's PHY reserved by the tree), a die-1 port, the DP IN outputs beside
the direct ones, and the PD controllers on two buses. The tree is built in C.
"""

from dcp_typec_routes_harness import run, u32

XBAR = 406


def adt_nodes(duplicate_port=None):
    nodes = [{"path": "/arm-io"},
             {"path": "/arm-io/display-crossbar0", "props": {"AAPL,phandle": u32(XBAR)}}]
    atc = [(0, 1, 0, 0x2a3000000, 0x2a324c000), (1, 2, 0, 0x2ab000000, 0x2ab24c000),
           (2, 3, 0, 0x383000000, 0x38324c000), (3, 4, 0, 0x38b000000, 0x38b24c000),
           (4, 5, 1, 0x42a3000000, 0x42a324c000)]
    for n, port, die, core, xbar in atc:
        nodes.append({"path": f"/arm-io/atc{n}-dpxbar", "props": {"AAPL,phandle": u32(500 + n)},
                      "regs": [xbar]})
        nodes.append({"path": f"/arm-io/atc{n}-dpphy", "props": {
            "dp-switch-parent": u32(XBAR), "transport-tunneled": u32(0),
            "dp-switch-dfp-endpoint": u32(n % 4), "dp-switch-dfp-port": u32(0),
            "dp-switch-dfp-domain": u32(die), "port-number": u32(port),
            "atc-phy": u32(600 + n), "dpxbar-parent": u32(500 + n)}})
        nodes.append({"path": f"/arm-io/atc{n}-dpin0", "props": {
            "dp-switch-parent": u32(XBAR), "transport-tunneled": u32(1),
            "dp-switch-dfp-endpoint": u32(n % 4), "dp-switch-dfp-port": u32(1),
            "dp-switch-dfp-domain": u32(die), "port-number": u32(port)}})
        # The PHY's windows: the USB2 PHY first, the core third, as an ATC PHY lists them
        nodes.append({"path": f"/arm-io/atc-phy{n}", "props": {"AAPL,phandle": u32(600 + n)},
                      "regs": [core - 0x570000, core - 0x800000, core]})
    nodes.append({"path": "/arm-io/nub-spmi-a0", "regs": [0x28a188000]})
    for n, usid in enumerate((0xc, 0xa, 0x8, 0x6)):
        nodes.append({"path": f"/arm-io/nub-spmi-a0/hpm{n}", "props": {
            "port-number": u32(n + 1), "reg": u32(usid, 3, 0, 4)}})
    nodes.append({"path": "/arm-io/nub-spmi-a2", "regs": [0x28a1c8000]})
    nodes.append({"path": "/arm-io/nub-spmi-a2/hpm4", "props": {
        "port-number": u32(5), "reg": u32(0xc, 3, 0, 4)}})
    if duplicate_port:
        nodes.append({"path": "/arm-io/nub-spmi-a2/hpm9", "props": {
            "port-number": u32(duplicate_port), "reg": u32(0x2, 3, 0, 4)}})
    for n, ep, die in ((0, 0, 0), (1, 1, 0), (4, 0, 1)):
        props = {"dp-switch-parent": u32(XBAR), "dp-switch-ufp-endpoint": u32(ep),
                 "dp-switch-ufp-port": u32(0)}
        if die:
            props["dp-switch-ufp-domain"] = u32(die)
        nodes.append({"path": f"/arm-io/dcpext{n}", "props": props, "regs": [0]})
        nodes.append({"path": f"/arm-io/dcpext{n}/iop-dcpext{n}-nub", "props": {}})
    return nodes


DT = r'''
    assert(fdt_create_empty_tree(dt, sizeof(blob)) == 0);
    int root = fdt_path_offset(dt, "/");
    fdt_setprop_u32(dt, root, "#address-cells", 2);
    fdt_setprop_u32(dt, root, "#size-cells", 2);
    int soc = add("/", "soc");
    fdt_setprop_u32(dt, soc, "#address-cells", 2);
    fdt_setprop_u32(dt, soc, "#size-cells", 2);
    static const char xbar_compat[] = "apple,t6050-display-crossbar\0apple,t8142-display-crossbar";
    const struct { const char *name; u64 a; bool phy; const char *status; } blocks[] = {
        {"phy@2ab000000", 0x2ab000000, true, NULL},
        {"phy@383000000", 0x383000000, true, "disabled"},
        {"phy@38b000000", 0x38b000000, true, PHY3_STATUS},
        {"mux@2a324c000", 0x2a324c000, false, "disabled"},
        {"mux@2ab24c000", 0x2ab24c000, false, "disabled"},
        {"mux@38324c000", 0x38324c000, false, "disabled"},
        {"mux@38b24c000", 0x38b24c000, false, "disabled"},
    };
    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        int n = add("/soc", blocks[i].name);
        fdt64_t reg[2] = {cpu_to_fdt64(blocks[i].a), cpu_to_fdt64(0x4000)};
        fdt_setprop(dt, n, "reg", reg, sizeof(reg));
        if (blocks[i].phy) {
            fdt_setprop_u32(dt, n, "#phy-cells", 1);
            fdt_setprop_empty(dt, n, "mode-switch");
        } else {
            fdt_setprop(dt, n, "compatible", xbar_compat, sizeof(xbar_compat));
            fdt_setprop_u32(dt, n, "#mux-control-cells", 1);
        }
        if (blocks[i].status)
            fdt_setprop_string(dt, n, "status", blocks[i].status);
    }
    const char *dcps[] = {"dcp@2b2e00000", "dcp@392e00000"};
    for (int i = 0; i < 2; i++) {
        int n = add("/soc", dcps[i]);
        fdt64_t reg[2] = {cpu_to_fdt64(i ? 0x392e00000 : 0x2b2e00000), cpu_to_fdt64(0x4000)};
        fdt_setprop(dt, n, "reg", reg, sizeof(reg));
        if (i == 0)
            fdt_setprop_u32(dt, n, "apple,dcp-index", 1);
        n = add(i ? "/soc/dcp@392e00000" : "/soc/dcp@2b2e00000", "typec-routes");
        fdt_setprop_u32(dt, n, "#address-cells", 1);
        fdt_setprop_u32(dt, n, "#size-cells", 0);
    }
    int spmi = add("/soc", "spmi@28a188000");
    fdt64_t sreg[2] = {cpu_to_fdt64(0x28a188000), cpu_to_fdt64(0x4000)};
    fdt_setprop(dt, spmi, "reg", sreg, sizeof(sreg));
    fdt_setprop_u32(dt, spmi, "#address-cells", 2);
    fdt_setprop_u32(dt, spmi, "#size-cells", 0);
    const struct { const char *name; u32 usid; const char *status; } hpms[] = {
        {"usb-pd@c", 0xc, "disabled"}, {"usb-pd@a", 0xa, NULL},
        {"usb-pd@8", 0x8, "disabled"}, {"usb-pd@6", 0x6, NULL},
    };
    for (int i = 0; i < 4; i++) {
        char path[64];
        int n = add("/soc/spmi@28a188000", hpms[i].name);
        u32 reg[2] = {cpu_to_fdt32(hpms[i].usid), 0};
        fdt_setprop(dt, n, "reg", reg, sizeof(reg));
        fdt_setprop_string(dt, n, "compatible", "apple,sn201202x");
        if (hpms[i].status)
            fdt_setprop_string(dt, n, "status", hpms[i].status);
        snprintf(path, sizeof(path), "/soc/spmi@28a188000/%s", hpms[i].name);
        n = add(path, "connector");
        fdt_setprop_string(dt, n, "compatible", "usb-c-connector");
        strcat(path, "/connector");
        n = add(path, "ports");
        fdt_setprop_u32(dt, n, "#address-cells", 1);
        fdt_setprop_u32(dt, n, "#size-cells", 0);
        strcat(path, "/ports");
        n = add(path, "port@1");
        fdt_setprop_u32(dt, n, "reg", 1);
        if (ROUTER_ON_PORT2 && hpms[i].usid == 6) {
            n = add(path, "port@2");
            fdt_setprop_u32(dt, n, "reg", 2);
            strcat(path, "/port@2");
            n = add(path, "endpoint");
            fdt_setprop_u32(dt, n, "remote-endpoint", 0x1234);
        }
    }
'''

HPM = "/soc/spmi@28a188000/"
E0 = "/soc/dcp@2b2e00000"
E1 = "/soc/dcp@392e00000"


def dt_setup(phy3_status="NULL", router_on_port2=0):
    return f"#define PHY3_STATUS {phy3_status}\n#define ROUTER_ON_PORT2 {router_on_port2}\n" + DT


ENGINES = r'''
    int r = kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0");
    printf("ROUTES0 %%d\n", r);
    r = kboot_dcp_typec_routes(dt, "%(e1)s", "/arm-io/dcpext1");
    printf("ROUTES1 %%d\n", r);
    r = kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0");
    printf("AGAIN %%d\n", r);
''' % {"e0": E0, "e1": E1}


def test_every_described_port_from_every_engine(tmp_path):
    out = run(tmp_path, adt_nodes(), ENGINES + r'''
    /* dcpext0: ports 2, 3 and 4 in port order; port 1 has no PHY in the tree, port 5 is die 1 */
    const char *phy[] = {"/soc/phy@2ab000000", "/soc/phy@383000000", "/soc/phy@38b000000"};
    const char *xbar[] = {"/soc/mux@2ab24c000", "/soc/mux@38324c000", "/soc/mux@38b24c000"};
    const char *hpm[] = {"%(hpm)susb-pd@a", "%(hpm)susb-pd@8", "%(hpm)susb-pd@6"};
    for (int k = 0; k < 3; k++) {
        assert(fdt_node_offset_by_phandle(dt, cell("%(e0)s", "phys", 2 * k, 6)) == node(phy[k]));
        assert(cell("%(e0)s", "phys", 2 * k + 1, 6) == 6);
        assert(fdt_node_offset_by_phandle(dt, cell("%(e0)s", "mux-controls", 2 * k, 6)) ==
               node(xbar[k]));
        assert(cell("%(e0)s", "mux-controls", 2 * k + 1, 6) == 0);
        assert(cell("%(e0)s", "apple,typec-mux-indices", k, 3) == 0);
        assert(cell("%(e0)s", "apple,typec-dptx-phys", k, 3) == (u32)k + 1);
        assert(cell("%(e1)s", "apple,typec-mux-indices", k, 3) == 2);
        char route[96], cep[128];
        for (int e = 0; e < 2; e++) {
            snprintf(route, sizeof(route), "%%s/typec-routes/route@%%d/port/endpoint",
                     e ? "%(e1)s" : "%(e0)s", k);
            snprintf(cep, sizeof(cep), "%%s/connector/ports/port@2/endpoint@%%d", hpm[k], e);
            assert(remote(route) == node(cep));
            assert(remote(cep) == node(route));
            assert(cell(cep, "reg", 0, 1) == (u32)e);
        }
        snprintf(route, sizeof(route), "%(e0)s/typec-routes/route@%%d", k);
        assert(cell(route, "reg", 0, 1) == (u32)k);
        assert(fdt_getprop(dt, node(route), "mode-switch", NULL));
    }
    assert(cell("%(e0)s", "apple,dptx-die", 0, 1) == 0);
    assert(cell("%(e0)s", "apple,dcp-index", 0, 1) == 1);
    assert(cell("%(e1)s", "apple,dcp-index", 0, 1) == 2);
    int len;
    const char *names = fdt_getprop(dt, node("%(e0)s"), "phy-names", &len);
    assert(len == 21 && !memcmp(names, "typec0\0typec1\0typec2\0", 21));
    assert(fdt_subnode_offset(dt, node("%(hpm)susb-pd@c/connector/ports"), "port@2") < 0);
    assert(fdt_check_header(dt) == 0);
''' % {"e0": E0, "e1": E1, "hpm": HPM}, dt_setup())
    assert "ROUTES0 3" in out and "ROUTES1 3" in out and "AGAIN 0" in out
    assert "port 1: its ATC PHY is not described for Linux" in out


def test_reserved_phy_and_foreign_display_port_are_left_alone(tmp_path):
    out = run(tmp_path, adt_nodes(), ENGINES + r'''
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 0, 2) == 1);
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 1, 2) == 2);
''' % {"e0": E0}, dt_setup('"reserved"'))
    assert "ROUTES0 2" in out and "port 4: its ATC PHY is not described for Linux" in out
    out = run(tmp_path, adt_nodes(), ENGINES, dt_setup(router_on_port2=1), name="router")
    assert "ROUTES0 2" in out and "port 4: its connector's port@2 holds something else" in out


def test_contradictory_adt_routes_nothing_on_that_port(tmp_path):
    out = run(tmp_path, adt_nodes(duplicate_port=3), ENGINES, dt_setup())
    assert "port 3: 2 PD controllers in the ADT, not one" in out and "ROUTES0 2" in out


FIXED = r'''
    /* A fixed HDMI output on dcpext0: its DP PHY and its crossbar output */
    int hphy = add("/soc", "hdmi-phy"), hxbar = add("/soc", "hdmi-xbar");
    fdt_setprop_u32(dt, hphy, "#phy-cells", 1);
    fdt_setprop_u32(dt, hphy, "phandle", 0x777);
    fdt_setprop_u32(dt, hxbar, "#mux-control-cells", 1);
    fdt_setprop_u32(dt, hxbar, "phandle", 0x778);
    fdt32_t fphy[2] = {cpu_to_fdt32(0x777), cpu_to_fdt32(6)};
    fdt32_t fmux[2] = {cpu_to_fdt32(0x778), cpu_to_fdt32(0)};
    fdt_setprop(dt, node("%(e0)s"), "phys", fphy, sizeof(fphy));
    fdt_setprop(dt, node("%(e0)s"), "mux-controls", fmux, sizeof(fmux));
    fdt_setprop_string(dt, node("%(e0)s"), "phy-names", PHY_NAME);
    fdt_setprop_string(dt, node("%(e0)s"), "mux-control-names", "dp-xbar");
''' % {"e0": E0}


def test_a_fixed_output_stays_first_in_a_hybrid_pipeline(tmp_path):
    out = run(tmp_path, adt_nodes(), "#define PHY_NAME \"dp-phy\"\n" + FIXED + ENGINES + r'''
    int len;
    const char *names = fdt_getprop(dt, node("%(e0)s"), "phy-names", &len);
    assert(len == 28 && !memcmp(names, "dp-phy\0typec0\0typec1\0typec2\0", 28));
    names = fdt_getprop(dt, node("%(e0)s"), "mux-control-names", &len);
    assert(len == 29 && !memcmp(names, "dp-xbar\0typec0\0typec1\0typec2\0", 29));
    assert(cell("%(e0)s", "phys", 0, 8) == 0x777 && cell("%(e0)s", "phys", 1, 8) == 6);
    assert(fdt_node_offset_by_phandle(dt, cell("%(e0)s", "phys", 2, 8)) ==
           node("/soc/phy@2ab000000"));
    assert(cell("%(e0)s", "mux-controls", 0, 8) == 0x778 &&
           cell("%(e0)s", "mux-controls", 1, 8) == 0);
    assert(fdt_node_offset_by_phandle(dt, cell("%(e0)s", "mux-controls", 6, 8)) ==
           node("/soc/mux@38b24c000"));
    /* The route indices still count the Type-C routes only */
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 0, 3) == 1);
    names = fdt_getprop(dt, node("%(e1)s"), "phy-names", &len);
    assert(len == 21 && !memcmp(names, "typec0\0typec1\0typec2\0", 21));
''' % {"e0": E0, "e1": E1}, dt_setup())
    assert "ROUTES0 3" in out and "ROUTES1 3" in out and "AGAIN 0" in out


def test_other_phys_than_a_fixed_output_are_the_tree_s_own(tmp_path):
    out = run(tmp_path, adt_nodes(), "#define PHY_NAME \"typec0\"\n" + FIXED + ENGINES,
              dt_setup())
    assert "names PHYs or crossbars already: no routes" in out and "ROUTES0 0" in out
    assert "ROUTES1 3" in out


def test_a_tree_without_the_request_is_untouched(tmp_path):
    out = run(tmp_path, adt_nodes(), r'''
    fdt_del_node(dt, node("%(e0)s/typec-routes"));
    unsigned char before[sizeof(blob)];
    memcpy(before, blob, fdt_totalsize(dt));
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 0);
    assert(!memcmp(before, blob, fdt_totalsize(dt)));
    puts("UNTOUCHED");
''' % {"e0": E0}, dt_setup())
    assert "UNTOUCHED" in out


WALK = r'''
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 3);
    assert(t6050_route(dt, "%(e0)s", 0) == 0);
    const char *on[] = {"/soc/phy@383000000", "/soc/mux@2ab24c000", "/soc/mux@38324c000",
                        "/soc/mux@38b24c000", "%(hpm)susb-pd@8"};
    for (unsigned i = 0; i < sizeof(on) / sizeof(on[0]); i++)
        assert(!strcmp(fdt_getprop(dt, node(on[i]), "status", NULL), "okay"));
    /* Port 1 has no route: its PD controller and crossbar stay as they were */
    assert(!strcmp(fdt_getprop(dt, node("%(hpm)susb-pd@c"), "status", NULL), "disabled"));
    assert(!strcmp(fdt_getprop(dt, node("/soc/mux@2a324c000"), "status", NULL), "disabled"));
    assert(t6050_route_next(dt, node("%(e0)s"), false) == T6050_ROUTE_DONE);
    puts("WALKED");
''' % {"e0": E0, "hpm": HPM}


def test_the_handoff_enables_what_every_route_needs(tmp_path):
    out = run(tmp_path, adt_nodes(), WALK, dt_setup(), walk=True)
    assert "WALKED" in out and "5 route node(s) enabled" in out


ORDER = r'''
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 3);
    assert(kboot_dcp_typec_routes(dt, "%(e1)s", "/arm-io/dcpext1") == 3);
    /*
     * Written nodes follow their siblings: the Type-C class reads a connector's graph in tree
     * order and keeps only its first mode switches, so the PHY's port@1 must come first.
     */
    int child, i = 0;
    const char *ports[] = {"port@1", "port@2"};
    fdt_for_each_subnode(child, dt, node("%(hpm)susb-pd@8/connector/ports"))
        assert(i < 2 && !strcmp(fdt_get_name(dt, child, NULL), ports[i++]));
    assert(i == 2);
    i = 0;
    fdt_for_each_subnode(child, dt, node("%(hpm)susb-pd@8/connector/ports/port@2"))
        assert(cell("%(hpm)susb-pd@8/connector/ports/port@2/endpoint@0", "reg", 0, 1) == 0 &&
               fdt32_ld(fdt_getprop(dt, child, "reg", NULL)) == (u32)i++);
    assert(i == 2);
    i = 0;
    fdt_for_each_subnode(child, dt, node("%(e0)s/typec-routes"))
        assert(fdt32_ld(fdt_getprop(dt, child, "reg", NULL)) == (u32)i++);
    assert(i == 3 && fdt_check_full(dt, fdt_totalsize(dt)) == 0);
    puts("ORDERED");
''' % {"e0": E0, "e1": E1, "hpm": HPM}


def test_written_nodes_follow_their_siblings(tmp_path):
    assert "ORDERED" in run(tmp_path, adt_nodes(), ORDER, dt_setup())


ATC0_HANDOFF_SETUP = r'''
    /* Add port 1's PHY and power domain, plus /firmware/atc0-handoff owning port 1 */
    int ps = add("/soc", "power-domain@atc0");
    fdt_setprop_u32(dt, ps, "phandle", 0x900);
    fdt_setprop_u32(dt, ps, "#power-domain-cells", 0);
    fdt_setprop_string(dt, ps, "status", ATC0_PS_STATUS);

    int p0 = add("/soc", "phy@2a3000000");
    fdt64_t preg[2] = {cpu_to_fdt64(0x2a3000000), cpu_to_fdt64(0x4000)};
    fdt_setprop(dt, p0, "reg", preg, sizeof(preg));
    fdt_setprop_u32(dt, p0, "phandle", 0x901);
    fdt_setprop_u32(dt, p0, "#phy-cells", 1);
    fdt_setprop_empty(dt, p0, "mode-switch");
    fdt_setprop_u32(dt, p0, "power-domains", 0x900);
    fdt_setprop_string(dt, p0, "status", ATC0_DEV_STATUS);

    fdt_setprop_u32(dt, node("/soc/mux@2a324c000"), "phandle", 0x902);
    fdt_setprop_string(dt, node("/soc/mux@2a324c000"), "status", ATC0_DEV_STATUS);
    fdt_setprop_u32(dt, node("%(hpm)susb-pd@c"), "phandle", 0x903);
    fdt_setprop_string(dt, node("%(hpm)susb-pd@c"), "status", ATC0_DEV_STATUS);

    add("/", "firmware");
    int ho = add("/firmware", "atc0-handoff");
    fdt_setprop_string(dt, ho, "compatible", "apple,atc-handoff");
    fdt_setprop_u32(dt, ho, "apple,phy", 0x901);
    fdt32_t devs[4] = {cpu_to_fdt32(0x900), cpu_to_fdt32(0x901), cpu_to_fdt32(0x902),
                       cpu_to_fdt32(0x903)};
    fdt_setprop(dt, ho, "apple,devices", devs, sizeof(devs));
''' % {"hpm": HPM}


def dt_atc0_handoff(dev_status, ps_status=None):
    if ps_status is None:
        ps_status = dev_status
    return (f'#define ATC0_DEV_STATUS "{dev_status}"\n'
            f'#define ATC0_PS_STATUS "{ps_status}"\n' + dt_setup() + ATC0_HANDOFF_SETUP)


def test_unactivated_atc_handoff_skips_port_and_leaves_group_disabled(tmp_path):
    out = run(tmp_path, adt_nodes(), WALK + r'''
    assert(!strcmp(fdt_getprop(dt, node("/soc/power-domain@atc0"), "status", NULL), "disabled"));
    assert(!strcmp(fdt_getprop(dt, node("/soc/phy@2a3000000"), "status", NULL), "disabled"));
    assert(fdt_subnode_offset(dt, node("%(hpm)susb-pd@c/connector/ports"), "port@2") < 0);
''' % {"hpm": HPM}, dt_atc0_handoff("disabled"), walk=True)
    assert "WALKED" in out and "un-activated apple,atc-handoff" in out


def test_partially_activated_atc_handoff_fails_closed(tmp_path):
    out = run(tmp_path, adt_nodes(), r'''
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 3);
    assert(fdt_subnode_offset(dt, node("%(hpm)susb-pd@c/connector/ports"), "port@2") < 0);
    puts("PARTIAL_SKIPPED");
''' % {"e0": E0, "hpm": HPM}, dt_atc0_handoff("okay", "disabled"))
    assert "PARTIAL_SKIPPED" in out and "un-activated apple,atc-handoff" in out


def test_activated_atc_handoff_routes_port(tmp_path):
    out = run(tmp_path, adt_nodes(), r'''
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 4);
    assert(t6050_route(dt, "%(e0)s", 0) == 0);
    assert(fdt_subnode_offset(dt, node("%(hpm)susb-pd@c/connector/ports"), "port@2") >= 0);
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 0, 4) == 0);
    puts("ACTIVATED_ROUTED");
''' % {"e0": E0, "hpm": HPM}, dt_atc0_handoff("okay", "okay"), walk=True)
    assert "ACTIVATED_ROUTED" in out


def test_static_route_into_unactivated_atc_handoff_refused_by_walk(tmp_path):
    out = run(tmp_path, adt_nodes(), r'''
    /* Hand-written static route pointing at un-activated ATC0 PHY must be refused by walk */
    fdt32_t phys[2] = {cpu_to_fdt32(0x901), cpu_to_fdt32(6)};
    fdt32_t mux[2] = {cpu_to_fdt32(0x902), cpu_to_fdt32(0)};
    fdt_setprop(dt, node("%(e0)s"), "phys", phys, sizeof(phys));
    fdt_setprop(dt, node("%(e0)s"), "mux-controls", mux, sizeof(mux));
    assert(t6050_route_next(dt, node("%(e0)s"), true) == T6050_ROUTE_BAD);
    assert(t6050_route(dt, "%(e0)s", 0) == 0);
    assert(!strcmp(fdt_getprop(dt, node("/soc/phy@2a3000000"), "status", NULL), "disabled"));
    assert(!strcmp(fdt_getprop(dt, node("/soc/mux@2a324c000"), "status", NULL), "disabled"));
    assert(!strcmp(fdt_getprop(dt, node("/soc/power-domain@atc0"), "status", NULL), "disabled"));
    puts("STATIC_REFUSED");
''' % {"e0": E0}, dt_atc0_handoff("disabled"), walk=True)
    assert "STATIC_REFUSED" in out and "not fully described" in out


def test_pending_transitive_dependency_skips_only_that_port(tmp_path):
    """If a port's PHY mailbox or power domain is in a pending apple,atc-handoff group (while
    the PHY itself is not), the route writer skips only that port so sibling ports still route
    and pass the handoff walk cleanly."""
    setup = dt_setup() + r'''
    int mb = add("/soc", "mailbox@2ab008000");
    fdt_setprop_u32(dt, mb, "phandle", 0x910);
    fdt_setprop_u32(dt, mb, "#mbox-cells", 0);
    fdt_setprop_string(dt, mb, "status", "disabled");
    fdt_setprop_u32(dt, node("/soc/phy@2ab000000"), "mboxes", 0x910);

    add("/", "firmware");
    int ho = add("/firmware", "atc1-mbox-handoff");
    fdt_setprop_string(dt, ho, "compatible", "apple,atc-handoff");
    fdt_setprop_u32(dt, ho, "apple,phy", 0x910);
    fdt_setprop_u32(dt, ho, "apple,devices", 0x910);
'''
    out = run(tmp_path, adt_nodes(), r'''
    assert(kboot_dcp_typec_routes(dt, "%(e0)s", "/arm-io/dcpext0") == 2);
    assert(t6050_route(dt, "%(e0)s", 0) == 0);
    /* Port 2 skipped and its mailbox left disabled; ports 3 and 4 routed and enabled */
    assert(!strcmp(fdt_getprop(dt, node("/soc/mailbox@2ab008000"), "status", NULL), "disabled"));
    assert(fdt_subnode_offset(dt, node("%(hpm)susb-pd@a/connector/ports"), "port@2") < 0);
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 0, 2) == 2);
    assert(cell("%(e0)s", "apple,typec-dptx-phys", 1, 2) == 3);
    assert(!strcmp(fdt_getprop(dt, node("/soc/mux@38324c000"), "status", NULL), "okay"));
    assert(!strcmp(fdt_getprop(dt, node("/soc/mux@38b24c000"), "status", NULL), "okay"));
    puts("TRANSITIVE_SKIPPED");
''' % {"e0": E0, "hpm": HPM}, setup, walk=True)
    assert "TRANSITIVE_SKIPPED" in out and "un-activated apple,atc-handoff" in out
