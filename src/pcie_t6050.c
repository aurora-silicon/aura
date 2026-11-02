/* SPDX-License-Identifier: MIT */

#include "adt.h"
#include "pcie.h"
#include "pmgr.h"
#include "string.h"
#include "tunables.h"
#include "utils.h"

/*
 * T6050 apcie0: cold bring-up of J775D's GP ports, the sequence measured on port 2 (the onboard
 * RTL8127A) step by step from the proxy (stages A, S, B-G below).
 * iBoot leaves the GP side powered down and storage live on the same controller, so this raises
 * only the GP power closure, starts the shared GP PHY only while no GP port has a link (A, S), then
 * brings up each port in `ports` on its own (B-G). Writes meant to stick are read back; the first
 * unexpected value stops that port with no cleanup (a reboot is the cleanup). An already trained
 * port is adopted as found. Storage (ANS, NVMe, its SART and power) is snapshotted before and
 * compared after each stage that touches shared state; any change Linux's nvme-apple would trip
 * over stops the boot.
 *
 * Linux (pcie-apple's T6050 adoption) then takes each link as is: no RID route, the endpoint's
 * command register clear, DART untouched (stage G).
 */

#define T6050_GP_PORTS 4

/* The ports brought up, in order. Port 2 is required (the result of pcie_t6050_init); the rest are
 * optional: a stop there leaves that port down and the others as they are. */
static const struct t6050_port {
    int port;
    int bus; /* its root port's secondary bus */
    u32 endpoint_id;
    const char *endpoint;
    bool required;
} ports[] = {
    {2, 1, 0x812710ec, "RTL8127A", true},
    {3, 2, 0x21421b21, "ASM3142 xHCI", false},
};

/* apcie0 "reg": shared windows, then six per port */
#define REG_ECAM          0
#define REG_RC            1
#define REG_PHY           2
#define REG_PHY_CTL       3
#define REG_AXI2AF        5
#define REG_AUS5_TOP      10
#define REG_AUS5_AON      11
#define REG_PORT(p)       (16 + 6 * (p))
#define REG_PORT_LANE(p)  (18 + 6 * (p))
#define REG_PORT_I2AXI(p) (19 + 6 * (p))
#define REG_COUNT         (16 + 6 * T6050_GP_PORTS)

/* Port registers */
#define PORT_LTSSMCTL   0x080
#define PORT_CLKRST     0x0a8
#define PORT_MSICFG     0x124
#define PORT_LINKSTS    0x208
#define PORT_APPCLK     0x800
#define PORT_STATUS     0x804
#define PORT_INT_RESET  0x82c
#define PORT_RID2SID    0x3000
#define PORT_RID2SID_EN BIT(31)

/* Root port DBI */
#define DBI_GEN3_EQ    0x194
#define DBI_LINK_WIDTH 0x710
#define DBI_GEN2_CTRL  0x80c
#define DBI_SHADOW_SEL 0x890
#define DBI_RO_WR_EN   0x8bc

/* PCI config space */
#define PCI_COMMAND               0x04
#define PCI_PRIMARY_BUS           0x18
#define PCI_CAPABILITY_LIST       0x34
#define PCI_CAP_ID_EXP            0x10
#define PCI_EXP_LNKCAP            0x0c
#define PCI_EXP_LNKCTL            0x10
#define PCI_EXP_LNKSTA            0x12
#define PCI_EXP_LNKCAP2           0x2c
#define PCI_EXP_LNKCAP2_SLS_8_0GB BIT(3)
#define PCI_EXP_LNKCTL2           0x30
#define PCI_CFG_SIZE              0x1000

#define GPIO_PERST_MASK  0x27f
#define GPIO_PERST_LOW   0x202
#define GPIO_PERST_LOCK  BIT(21)
#define GPIO_FOURCC_GPIO 0x4750494f /* "OIPG" in ADT byte order */

static const char apcie_path[] = "/arm-io/apcie0";

static struct {
    u64 ecam, rc, phy, phy_ctl, axi2af, aus5_top, aus5_aon;
    u64 port[T6050_GP_PORTS]; /* every GP port's control block, for the link checks in S */
    u64 phy_span, axi2af_span;
} r;

/* One brought-up port's resources, from the ADT */
struct port_regs {
    const struct t6050_port *p;
    char bridge[40];
    u64 ctl, ctl_span, lane, i2axi;
    u64 rp; /* root port 00:0<port>.0 config space */
    u64 perst;
    u32 refclk_to_perst_us;
    u64 dart;
};
static struct port_regs pr[ARRAY_SIZE(ports)];

/* AXI2AF records whose written bit reads back 0, identical in every proxy run: bit 31 at 0x0-0x3c
 * and 0x410-0x440, bit 23 at 0x310-0x340 (where bit 31 persists). Every other masked bit must read
 * back. */
static const struct tunable_excuse axi2af_excuses[] = {
#define B31(o) {o, BIT(31)}
#define B23(o) {o, BIT(23)}
    B31(0x000), B31(0x004), B31(0x008), B31(0x00c), B31(0x010), B31(0x014), B31(0x018), B31(0x01c),
    B31(0x020), B31(0x024), B31(0x028), B31(0x02c), B31(0x030), B31(0x034), B31(0x038), B31(0x03c),
    B23(0x310), B23(0x314), B23(0x318), B23(0x31c), B23(0x320), B23(0x324), B23(0x328), B23(0x32c),
    B23(0x330), B23(0x334), B23(0x338), B23(0x33c), B23(0x340), B31(0x410), B31(0x414), B31(0x418),
    B31(0x41c), B31(0x420), B31(0x424), B31(0x428), B31(0x42c), B31(0x430), B31(0x434), B31(0x438),
    B31(0x43c), B31(0x440), {0, 0},
#undef B31
#undef B23
};

#define FAIL(...)                                                                                  \
    do {                                                                                           \
        printf("pcie-t6050: STOP: " __VA_ARGS__);                                                  \
        return -1;                                                                                 \
    } while (0)

#define TRY(x)                                                                                     \
    do {                                                                                           \
        if ((x) < 0)                                                                               \
            return -1;                                                                             \
    } while (0)

static int wait32(const char *what, u64 addr, u32 mask, u32 want, bool equal, u32 ms)
{
    u64 timeout = timeout_calculate(ms * 1000);
    u32 v;

    while (true) {
        v = read32(addr);
        if (((v & mask) == want) == equal)
            return 0;
        if (timeout_expired(timeout))
            FAIL("%s: 0x%lx = 0x%08x after %u ms\n", what, addr, v, ms);
        udelay(10);
    }
}

#define poll_set(what, addr, bits, ms)   wait32(what, addr, bits, bits, true, ms)
#define poll_clear(what, addr, bits, ms) wait32(what, addr, bits, 0, true, ms)

/* Clear `clear`, set `set`, and require exactly that under both masks */
static int rmw(const char *what, u64 addr, u32 clear, u32 set)
{
    u32 old = read32(addr);
    write32(addr, (old & ~clear) | set);
    u32 v = read32(addr);
    if ((v & (clear | set)) != set)
        FAIL("%s: 0x%lx 0x%08x -> 0x%08x\n", what, addr, old, v);
    return 0;
}

/* The Gen3/4/5 shadows behind DBI_SHADOW_SEL do not read back through the same offsets */
#define NO_CHECK ((const struct tunable_excuse *)-1)

/* Apply `prop` within [base, base + span), then read back each record unless NO_CHECK */
static int apply_tunables(const char *path, const char *prop, u64 base, u64 span,
                          const struct tunable_excuse *excuses)
{
    if (tunables_validate_local(path, prop, span))
        FAIL("%s: missing or out of its window\n", prop);
    if (tunables_apply_local_addr(path, prop, base))
        FAIL("%s: not applied\n", prop);
    if (excuses != NO_CHECK && tunables_check_local_addr(path, prop, base, excuses))
        FAIL("%s: readback differs\n", prop);
    return 0;
}

static int find_regs(void)
{
    int path_nodes[8];

    int node = adt_path_offset_trace(adt, apcie_path, path_nodes);
    u32 reg_len = 0;
    if (node < 0 || !adt_is_compatible(adt, node, "apcie,t6050"))
        FAIL("no apcie,t6050 at %s\n", apcie_path);
    if (!adt_getprop(adt, node, "reg", &reg_len) || reg_len != 16 * REG_COUNT)
        FAIL("%s has %u reg bytes, expected %u\n", apcie_path, reg_len, 16 * REG_COUNT);
    u32 lane_cfg;
    if (ADT_GETPROP(adt, node, "lane-cfg", &lane_cfg) < 0 || lane_cfg)
        FAIL("lane-cfg is not 0\n");

    int max_bus = 0;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++)
        max_bus = max(max_bus, ports[i].bus);
    u64 unused;
    struct {
        u64 *dst, *span;
        int idx;
        u64 min_span;
    } regs[] = {
        {&r.ecam, &unused, REG_ECAM, (u64)(max_bus + 1) << 20},
        {&r.rc, &unused, REG_RC, 0x60},
        {&r.phy, &r.phy_span, REG_PHY, 0x4004},
        {&r.phy_ctl, &unused, REG_PHY_CTL, 0xc},
        {&r.axi2af, &r.axi2af_span, REG_AXI2AF, 0x444},
        {&r.aus5_top, &unused, REG_AUS5_TOP, 0x98},
        {&r.aus5_aon, &unused, REG_AUS5_AON, 0x60},
        {&r.port[0], &unused, REG_PORT(0), PORT_LINKSTS + 4},
        {&r.port[1], &unused, REG_PORT(1), PORT_LINKSTS + 4},
        {&r.port[2], &unused, REG_PORT(2), PORT_LINKSTS + 4},
        {&r.port[3], &unused, REG_PORT(3), PORT_LINKSTS + 4},
    };
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++)
        if (adt_get_reg(adt, path_nodes, "reg", regs[i].idx, regs[i].dst, regs[i].span) ||
            !*regs[i].dst || *regs[i].span < regs[i].min_span)
            FAIL("apcie0 reg[%d] missing or short\n", regs[i].idx);
    return 0;
}

/* A port's own windows, PERST# pad, refclk-to-PERST# time and DART, from apcie0 and its bridge */
static int find_port_regs(struct port_regs *pp)
{
    int path_nodes[8], gpio_path[8], dart_path[8];
    u64 unused, span;
    int n = pp->p->port;

    if (adt_path_offset_trace(adt, apcie_path, path_nodes) < 0)
        FAIL("no %s\n", apcie_path);
    struct {
        u64 *dst, *span;
        int idx;
        u64 min_span;
    } regs[] = {
        {&pp->ctl, &pp->ctl_span, REG_PORT(n), PORT_RID2SID + 64 * 4},
        {&pp->lane, &unused, REG_PORT_LANE(n), 0x4},
        {&pp->i2axi, &unused, REG_PORT_I2AXI(n), 0x84},
    };
    for (size_t i = 0; i < ARRAY_SIZE(regs); i++)
        if (adt_get_reg(adt, path_nodes, "reg", regs[i].idx, regs[i].dst, regs[i].span) ||
            !*regs[i].dst || *regs[i].span < regs[i].min_span)
            FAIL("apcie0 reg[%d] missing or short\n", regs[i].idx);
    pp->rp = r.ecam + ((u64)n << 15);

    /* PERST#: pci-bridge<n> function-perst = GPIO(pin) of /arm-io/gpio0 */
    snprintf(pp->bridge, sizeof(pp->bridge), "%s/pci-bridge%d", apcie_path, n);
    struct {
        u32 phandle, fourcc, pin, flags;
    } perst;
    int bridge = adt_path_offset(adt, pp->bridge);
    int gpio = adt_path_offset_trace(adt, "/arm-io/gpio0", gpio_path);
    u32 gpio_phandle, apcie_port;
    u64 gpio_base;
    if (bridge < 0 || ADT_GETPROP(adt, bridge, "apcie-port", &apcie_port) < 0 ||
        apcie_port != (u32)n)
        FAIL("%s is not apcie port %d\n", pp->bridge, n);
    if (gpio < 0 || adt_getprop_copy(adt, bridge, "function-perst", &perst, sizeof(perst)) < 0 ||
        perst.fourcc != GPIO_FOURCC_GPIO ||
        ADT_GETPROP(adt, gpio, "AAPL,phandle", &gpio_phandle) < 0 ||
        gpio_phandle != perst.phandle || adt_get_reg(adt, gpio_path, "reg", 0, &gpio_base, &span) ||
        span < 4 * (perst.pin + 1))
        FAIL("cannot resolve %s function-perst\n", pp->bridge);
    pp->perst = gpio_base + 4 * perst.pin;
    if (ADT_GETPROP(adt, bridge, "t-refclk-to-perst", &pp->refclk_to_perst_us) < 0)
        FAIL("%s has no t-refclk-to-perst\n", pp->bridge);

    char dart_name[32];
    snprintf(dart_name, sizeof(dart_name), "/arm-io/dart-apcie%d", n);
    if (adt_path_offset_trace(adt, dart_name, dart_path) < 0 ||
        adt_get_reg(adt, dart_path, "reg", 0, &pp->dart, &span) || span < 0xc04)
        FAIL("no %s\n", dart_name);
    return 0;
}

/* Storage as iBoot left it, read only (never the mailbox payloads) */
#define SART_SLOTS 16
enum {
    ST_ANS_CTRL,
    ST_ANS_STATUS,
    ST_BOOT_STATUS,
    ST_CC,
    ST_CSTS,
    ST_A2I,
    ST_I2A,
    ST_PS,
    ST_SART = ST_PS + 6,
    ST_COUNT = ST_SART + 3 * SART_SLOTS,
};
static const char *const storage_ps[] = {"APCIE_ST0", "APCIE_ST1",     "FAB6_SOC",
                                         "ANS",       "APCIE_SYS_ST0", "APCIE_SYS_ST1"};
static u64 storage_addr[ST_COUNT];
static u32 storage_ref[ST_COUNT];
#define NVME_BOOT_STATUS_OK 0xde71ce55

static int storage_find(void)
{
    int path_nodes[8];
    u64 ans, nvme, sart, span;

    if (adt_path_offset_trace(adt, "/arm-io/ans", path_nodes) < 0 ||
        adt_get_reg(adt, path_nodes, "reg", 0, &ans, &span) || span < 0x8118 ||
        adt_get_reg(adt, path_nodes, "reg", 3, &nvme, &span) || span < 0x1304)
        FAIL("no /arm-io/ans\n");
    if (adt_path_offset_trace(adt, "/arm-io/sart-ans", path_nodes) < 0 ||
        adt_get_reg(adt, path_nodes, "reg", 0, &sart, &span) || span < 0xc0 + 4 * SART_SLOTS)
        FAIL("no /arm-io/sart-ans\n");
    storage_addr[ST_ANS_CTRL] = ans + 0x44;
    storage_addr[ST_ANS_STATUS] = ans + 0x48;
    storage_addr[ST_A2I] = ans + 0x8110;
    storage_addr[ST_I2A] = ans + 0x8114;
    storage_addr[ST_BOOT_STATUS] = nvme + 0x1300;
    storage_addr[ST_CC] = nvme + 0x14;
    storage_addr[ST_CSTS] = nvme + 0x1c;
    for (int i = 0; i < (int)ARRAY_SIZE(storage_ps); i++)
        if (!(storage_addr[ST_PS + i] = pmgr_psreg_by_name(0, storage_ps[i])))
            FAIL("no PMGR device %s\n", storage_ps[i]);
    for (int i = 0; i < SART_SLOTS; i++)
        for (int k = 0; k < 3; k++)
            storage_addr[ST_SART + 3 * i + k] = sart + 0x60 * k + 4 * i;
    return 0;
}

static int storage_check(const char *tag, bool reference)
{
    u32 cur[ST_COUNT];
    int changed = 0;

    for (int i = 0; i < ST_COUNT; i++)
        cur[i] = read32(storage_addr[i]);
    if (reference)
        memcpy(storage_ref, cur, sizeof(cur));
    for (int i = 0; i < ST_COUNT && !reference; i++) {
        if (cur[i] == storage_ref[i])
            continue;
        printf("pcie-t6050: storage %s: 0x%lx 0x%08x -> 0x%08x\n", tag, storage_addr[i],
               storage_ref[i], cur[i]);
        changed++;
        /* PS actual state and was-gated flags may move; target and auto must not */
        if (i >= ST_PS && i < ST_SART && !((cur[i] ^ storage_ref[i]) & ~0x3f0))
            continue;
        if (i >= ST_PS || i == ST_BOOT_STATUS || i == ST_CC)
            FAIL("storage changed %s\n", tag);
    }
    if (!(cur[ST_ANS_CTRL] & BIT(4)))
        FAIL("ANS is not running (%s)\n", tag);
    if (cur[ST_BOOT_STATUS] != NVME_BOOT_STATUS_OK)
        FAIL("NVMe BOOT_STATUS 0x%x (%s)\n", cur[ST_BOOT_STATUS], tag);
    if (cur[ST_CSTS] & BIT(1))
        FAIL("NVMe CSTS fatal (%s)\n", tag);
    printf("pcie-t6050: storage %s: %s\n", tag,
           reference ? "reference"
           : changed ? "allowed"
                     : "unchanged");
    return 0;
}

/* A: the GP power closure; the rest of it must already be active */
static int stage_a(void)
{
    static const char *const keep[] = {"GPIO", "APCIE_GP", "FAB6_SOC", "APHY_GP_AUS5_A"};
    static const char *const raise[] = {"APCIE_SYS_GP", "APCIE_PHY_SW", "APHY_GP_AUS5"};

    /* Every port still to be brought up is held in reset as iBoot leaves it */
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++)
        if (pr[i].ctl && !(read32(pr[i].ctl + PORT_LINKSTS) & BIT(0)) &&
            (read32(pr[i].perst) & GPIO_PERST_MASK) != GPIO_PERST_LOW)
            FAIL("port %d PERST# is not output-low: 0x%x\n", ports[i].port, read32(pr[i].perst));
    for (size_t i = 0; i < ARRAY_SIZE(keep); i++)
        if (pmgr_power_is_on(0, keep[i]) != 1)
            FAIL("%s is not active, and it is not ours to raise\n", keep[i]);
    for (size_t i = 0; i < ARRAY_SIZE(raise); i++) {
        uintptr_t ps = pmgr_psreg_by_name(0, raise[i]);
        if (!ps || pmgr_power_on(0, raise[i]) || read32(ps) & BIT(28))
            FAIL("%s did not come up with auto-PM off\n", raise[i]);
    }
    /* macOS waits here */
    return poll_clear("AUS5 top +0x94 bit 1", r.aus5_top + 0x94, BIT(1), 100);
}

/* S: the shared GP init, Apple's post-iBoot GP order; only with no GP port linked (it would drop
 * the link) */
static int stage_s(void)
{
    for (int p = 0; p < T6050_GP_PORTS; p++)
        if (read32(r.port[p] + PORT_LINKSTS) & BIT(0))
            FAIL("GP port %d has a link\n", p);
    if (read32(r.phy_ctl + 0x08))
        FAIL("shared PHY status 0x%x: not cold\n", read32(r.phy_ctl + 0x08));

    TRY(rmw("AUS5 aon +0x5c bit 2", r.aus5_aon + 0x5c, 0, BIT(2)));
    TRY(apply_tunables(apcie_path, "apcie-axi2af-tunables", r.axi2af, r.axi2af_span,
                       axi2af_excuses));
    write32(r.rc + 0x04, 0); /* lane-cfg 0 */
    if (read32(r.rc + 0x04))
        FAIL("lane cfg did not take\n");
    TRY(apply_tunables(apcie_path, "apcie-phy-tunables", r.phy, r.phy_span, NULL));
    TRY(poll_set("refclk", r.phy + 0x4000, BIT(31), 250));
    /* Gen5 PHY start */
    TRY(rmw("H+0x04 bit 4", r.phy_ctl + 0x04, 0, BIT(4)));
    TRY(poll_set("H+0x08 bit 4", r.phy_ctl + 0x08, BIT(4), 250));
    TRY(rmw("H+0x04 bit 5 clear", r.phy_ctl + 0x04, BIT(5), 0));
    TRY(poll_set("H+0x08 bit 0", r.phy_ctl + 0x08, BIT(0), 250));
    TRY(rmw("refclk enable", r.phy + 0x4000, 0, BIT(0)));
    TRY(rmw("H+0x00 bit 27", r.phy_ctl + 0x00, 0, BIT(27)));
    write32(r.rc + 0x54, 0x140);
    TRY(rmw("RC run", r.rc + 0x50, 0, BIT(0)));
    return poll_set("RC ready", r.rc + 0x58, BIT(0), 250);
}

/* B: the port-local reset table; port interrupts masked, RID and MSI tables untouched */
static int stage_b(const struct port_regs *pp)
{
    static const u32 table[][2] = {
        {0x088, 0x110},   {0x100, 0xffffffff}, {0x148, 0xffffffff}, {0x210, 0xffffffff},
        {0x080, 0},       {0x084, 0},          {0x104, 0xffffffff}, {0x124, 0x100},
        {0x16c, 0},       {0x13c, 0},          {0x800, 0x100100},   {0x808, 0x1001ff},
        {0x82c, 0x10000}, {0x130, 0x03020000}, {0x140, 0x10},       {0x144, 0x253770},
        {0x21c, 0},       {0x834, 0},          {0x83c, 0},
    };
    u64 p2 = pp->ctl;

    if (read32(p2 + PORT_LINKSTS) & BIT(0) || read32(p2 + PORT_LTSSMCTL) & BIT(0))
        FAIL("port %d has a link or LTSSM running\n", pp->p->port);
    /* H+0x08 bit 0 is transient after S; bit 4 stays */
    if (!(read32(r.phy_ctl + 0x08) & BIT(4)) || !(read32(r.rc + 0x58) & BIT(0)))
        FAIL("shared GP init not in place\n");
    /* Command and W1C registers among these do not read back as written: the predicates below are
     * what the sequence requires. */
    for (size_t i = 0; i < ARRAY_SIZE(table); i++)
        write32(p2 + table[i][0], table[i][1]);
    if (read32(p2 + PORT_LTSSMCTL) & BIT(0) || read32(p2 + PORT_MSICFG) & BIT(0) ||
        read32(p2 + PORT_APPCLK) & BIT(0) || !(read32(p2 + PORT_INT_RESET) & BIT(16)) ||
        read32(p2 + PORT_INT_RESET) & BIT(0))
        FAIL("port reset table did not take\n");
    return 0;
}

/* C: port config tunables, app clock, internal reset handshake */
static int stage_c(const struct port_regs *pp)
{
    u64 p2 = pp->ctl;

    TRY(apply_tunables(pp->bridge, "apcie-config-tunables", p2, pp->ctl_span, NULL));
    TRY(rmw("APPCLK", p2 + PORT_APPCLK, 0, BIT(0)));
    TRY(wait32("clk/rst bit 0 or 16 clear", p2 + PORT_CLKRST, BIT(0) | BIT(16), BIT(0) | BIT(16),
               false, 10));
    TRY(rmw("internal reset bit 0", p2 + PORT_INT_RESET, 0, BIT(0)));
    TRY(poll_set("clk/rst bit 0", p2 + PORT_CLKRST, BIT(0), 20));
    TRY(rmw("internal reset bit 16 clear", p2 + PORT_INT_RESET, BIT(16), 0));
    TRY(poll_set("port ready", p2 + PORT_STATUS, BIT(0), 250));
    TRY(rmw("APPCLK bit 8", p2 + PORT_APPCLK, 0, BIT(8)));
    return poll_clear("port not busy", p2 + PORT_LINKSTS, BIT(2), 250);
}

static int find_pcie_cap(u64 rp)
{
    u32 cap = read32(rp + PCI_CAPABILITY_LIST) & 0xfc;

    for (int i = 0; i < 48 && cap >= 0x40 && !(cap & 3); i++) {
        u32 hdr = read32(rp + cap);
        if ((hdr & 0xff) == PCI_CAP_ID_EXP)
            return cap;
        cap = (hdr >> 8) & 0xfc;
    }
    FAIL("no PCI Express capability on the root port\n");
}

/* D: root-port DBI tunables and the Gen3 x1 policy, in the order Apple's driver uses */
static int stage_d(const struct port_regs *pp)
{
    static const struct {
        u32 sel;
        const char *prop;
    } shadows[] = {
        {0, "pcie-rc-gen3-shadow-tunables"},
        {1 << 24, "pcie-rc-gen4-shadow-tunables"},
        {0, "pcie-rc-gen5-shadow-tunables"},
    };
    u64 rp = pp->rp;
    int cap = find_pcie_cap(rp);
    TRY(cap);

    TRY(rmw("DBI write enable", rp + DBI_RO_WR_EN, 0, BIT(0)));
    TRY(apply_tunables(pp->bridge, "pcie-rc-tunables", rp, PCI_CFG_SIZE, NULL));
    for (size_t i = 0; i < ARRAY_SIZE(shadows); i++) {
        TRY(rmw("shadow select", rp + DBI_SHADOW_SEL, 3 << 24, shadows[i].sel));
        TRY(apply_tunables(pp->bridge, shadows[i].prop, rp, PCI_CFG_SIZE, NO_CHECK));
    }
    TRY(rmw("Gen3 EQ", rp + DBI_GEN3_EQ, 0, BIT(0)));
    if (!(read32(rp + cap + PCI_EXP_LNKCAP2) & PCI_EXP_LNKCAP2_SLS_8_0GB))
        FAIL("Gen3 not advertised\n");
    u16 lnkctl2 = read16(rp + cap + PCI_EXP_LNKCTL2);
    write16(rp + cap + PCI_EXP_LNKCTL2, (lnkctl2 & ~0xf) | 3);
    if ((read16(rp + cap + PCI_EXP_LNKCTL2) & 0xf) != 3)
        FAIL("target link speed did not take\n");
    TRY(rmw("link width 1", rp + DBI_LINK_WIDTH, 0x3f << 16, 1 << 16));
    TRY(rmw("lanes 1", rp + DBI_GEN2_CTRL, 0x1f << 8, 1 << 8));
    TRY(rmw("link cap x1", rp + cap + PCI_EXP_LNKCAP, 0x3f << 4, 1 << 4));
    TRY(rmw("DBI write lock", rp + DBI_RO_WR_EN, BIT(0), 0));
    write16(rp + cap + PCI_EXP_LNKCTL, read16(rp + cap + PCI_EXP_LNKCTL) | 0x400);
    TRY(rmw("speed change", rp + DBI_GEN2_CTRL, 0, BIT(17)));
    /* Interrupt translation; reads back 0 (not needed for config reads) */
    write32(pp->i2axi + 0x80, 1);
    return 0;
}

/* E: lane PHY, refclk, PERST# release, LTSSM, link up */
static int stage_e(const struct port_regs *pp)
{
    u64 p2 = pp->ctl;

    if (read32(pp->lane) & BIT(28))
        FAIL("lane PHY bit 28 set\n");
    TRY(rmw("lane clk0 req", pp->lane, 0, BIT(0)));
    TRY(poll_set("lane clk0 ack", pp->lane, BIT(2), 50));
    TRY(rmw("lane clk1 req", pp->lane, 0, BIT(1)));
    TRY(poll_set("lane clk1 ack", pp->lane, BIT(3), 50));
    TRY(rmw("lane bit 4 clear", pp->lane, BIT(4), 0));
    mdelay(1);
    TRY(rmw("lane bit 9", pp->lane, 0, BIT(9)));
    TRY(rmw("lane bit 10", pp->lane, 0, BIT(10)));
    /* refclk to PERST#: the bridge's t-refclk-to-perst (port 2: 100 us), at least 1 ms */
    udelay(max(pp->refclk_to_perst_us, 1000u));

    u32 old = read32(pp->perst);
    if (old & GPIO_PERST_LOCK)
        FAIL("PERST# pad locked\n");
    u32 want = (old & ~GPIO_PERST_MASK) | GPIO_PERST_LOW | 1;
    write32(pp->perst, want);
    if ((read32(pp->perst) ^ want) & GPIO_PERST_MASK)
        FAIL("PERST# readback 0x%x\n", read32(pp->perst));

    TRY(rmw("LTSSM start", p2 + PORT_LTSSMCTL, 0, BIT(0)));
    TRY(poll_set("link up", p2 + PORT_LINKSTS, BIT(0), 500));
    mdelay(100);
    return 0;
}

/* F: bus numbers and the endpoint's ID */
static int stage_f(const struct port_regs *pp)
{
    u64 p2 = pp->ctl, rp = pp->rp;
    u32 bus = pp->p->bus;

    if (!(read32(p2 + PORT_STATUS) & BIT(0)) || !(read32(p2 + PORT_LINKSTS) & BIT(0)))
        FAIL("port not ready or link down\n");
    int cap = find_pcie_cap(rp);
    TRY(cap);
    u16 lnksta = read16(rp + cap + PCI_EXP_LNKSTA);
    printf("pcie-t6050: port %d link Gen%d x%d\n", pp->p->port, lnksta & 0xf, (lnksta >> 4) & 0x3f);
    u32 buses = read32(rp + PCI_PRIMARY_BUS);
    if (!((buses >> 8) & 0xff)) {
        u32 want = (buses & 0xff000000) | (bus << 16) | (bus << 8);
        write32(rp + PCI_PRIMARY_BUS, want);
        if (read32(rp + PCI_PRIMARY_BUS) != want)
            FAIL("bus numbers did not take\n");
    } else if (((buses >> 8) & 0xff) != bus) {
        FAIL("secondary bus is %d, not %d\n", (buses >> 8) & 0xff, bus);
    }
    u32 id = read32(r.ecam + ((u64)bus << 20));
    if (id != pp->p->endpoint_id)
        FAIL("endpoint %02x:00.0 is 0x%08x\n", bus, id);
    return 0;
}

/* G: what Linux's adoption requires, read only */
static int stage_g(const struct port_regs *pp)
{
    u64 p2 = pp->ctl;

    for (int i = 0; i < 64; i++)
        if (read32(p2 + PORT_RID2SID + 4 * i) & PORT_RID2SID_EN)
            FAIL("RID route %d is already valid\n", i);
    if (read32(r.ecam + ((u64)pp->p->bus << 20) + PCI_COMMAND) & 7)
        FAIL("endpoint I/O, memory or bus mastering enabled\n");
    if (read32(pp->dart + 0x200) || read32(pp->dart + 0x208) || read32(pp->dart + 0xc00))
        FAIL("DART%d protection or streams no longer 0\n", pp->p->port);
    return 0;
}

/* B-G for one port, after the shared A and S */
static int port_up(struct port_regs *pp)
{
    u64 p2 = pp->ctl;

    if (read32(p2 + PORT_LINKSTS) & BIT(0)) {
        printf("pcie-t6050: port %d already has a link: adopting it\n", pp->p->port);
    } else {
        TRY(stage_b(pp));
        TRY(stage_c(pp));
        TRY(stage_d(pp));
        TRY(stage_e(pp));
    }
    TRY(stage_f(pp));
    TRY(storage_check("after F", false));
    TRY(stage_g(pp));
    printf("pcie-t6050: port %d up, %s at %02x:00.0\n", pp->p->port, pp->p->endpoint, pp->p->bus);
    return 0;
}

/* 0 once every required port is up; -1, for the rest of this boot, after a stop in shared state or
 * a required port: a partial state is not retried. An optional port's stop leaves it down. */
int pcie_t6050_init(void)
{
    static int state = 1;

    if (state <= 0)
        return state;
    state = -1;
    TRY(find_regs());
    TRY(storage_find());
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        pr[i].p = &ports[i];
        if (find_port_regs(&pr[i])) {
            if (ports[i].required)
                return -1;
            pr[i].ctl = 0; /* left down: stages skip it */
        }
    }
    TRY(storage_check("before", true));

    bool any_down = false;
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++)
        any_down |= !(read32(pr[i].ctl + PORT_LINKSTS) & BIT(0));
    if (any_down || !(read32(r.rc + 0x58) & BIT(0))) {
        TRY(stage_a());
        TRY(storage_check("after A", false));
        if (read32(r.phy_ctl + 0x08) & BIT(4) && read32(r.rc + 0x58) & BIT(0)) {
            printf("pcie-t6050: shared GP init already in place\n");
        } else {
            TRY(stage_s());
            TRY(storage_check("after S", false));
        }
    }
    for (size_t i = 0; i < ARRAY_SIZE(ports); i++) {
        if (!pr[i].ctl) /* its resources were not found: reported above */
            continue;
        if (port_up(&pr[i]) < 0) {
            if (ports[i].required)
                return -1;
            printf("pcie-t6050: port %d left down\n", ports[i].port);
        }
    }
    state = 0;
    return 0;
}
