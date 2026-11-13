# SPDX-License-Identifier: MIT
"""A DCP whose firmware does not stop keeps every buffer and mapping it may use.

These compile the real afk.c and dcp.c functions against small stand-ins.
"""
from pathlib import Path
import os
import re
import subprocess

REPO = Path(os.environ.get("AURA_DCP_SOURCE", Path(__file__).resolve().parents[2]))


def function(source, name):
    match = re.search(r"^(?:static )?(?:bool|int|void) " + name + r"\([^;]*?\)\s*\{",
                      source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def compile_and_run(tmp_path, name, source):
    binary = tmp_path / name
    subprocess.run(
        ["cc", "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
         "-Wno-unused-function", "-x", "c", "-", "-o", str(binary)],
        input=source, text=True, check=True,
    )
    subprocess.run([str(binary)], check=True)


def test_epic_endpoint_that_does_not_stop_keeps_its_rings(tmp_path):
    source = (REPO / "src/afk.c").read_text()
    bodies = "\n".join(function(source, name)
                       for name in ("afk_epic_shutdown_ep", "afk_epic_shutdown"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t u64;
typedef uint32_t u32;
#define FIELD_PREP(mask, value) 0
#define RBEP_TYPE 0
#define RBEP_SHUTDOWN 0
typedef struct { int unused; } rtkit_dev_t;
struct rtkit_message { u32 ep; u64 msg; };
struct rtkit_buffer { bool freed; };
typedef struct afk_epic afk_epic_t;
typedef struct afk_epic_ep afk_epic_ep_t;
struct afk_epic { rtkit_dev_t *rtk; afk_epic_ep_t *endpoint[0x10]; };
struct afk_epic_ep {
    afk_epic_t *afk;
    u32 ep;
    bool started, stuck, released;
    struct rtkit_buffer buf, rxbuf, txbuf;
};
static int contexts_freed;
static afk_epic_t contexts[2];
static bool rtkit_send(rtkit_dev_t *rtk, const struct rtkit_message *msg) { return true; }
static int afk_epic_poll(afk_epic_t *afk, u32 ep, bool block)
{
    afk_epic_ep_t *epic = afk->endpoint[ep - 0x20];
    if (epic->stuck)
        return -1;
    epic->started = false;
    return 0;
}
static void rtkit_free_buffer(rtkit_dev_t *rtk, struct rtkit_buffer *bfr) { bfr->freed = true; }
/* Record frees without releasing anything, so the test can inspect it. */
static void record_free(void *p)
{
    if ((char *)p >= (char *)contexts && (char *)p < (char *)(contexts + 2))
        contexts_freed++;
    else
        ((afk_epic_ep_t *)p)->released = true;
}
#define free(p) record_free(p)
''' + bodies + r'''
#undef free
static bool freed(afk_epic_ep_t *e) { return e->buf.freed || e->rxbuf.freed || e->txbuf.freed; }

int main(void)
{
    afk_epic_t *afk = &contexts[0];
    afk_epic_ep_t stuck = {.afk = afk, .ep = 0x20, .started = true, .stuck = true};
    afk->endpoint[0] = &stuck;
    assert(afk_epic_shutdown_ep(&stuck) == -1);
    assert(!freed(&stuck) && !stuck.released && afk->endpoint[0] == &stuck);

    afk_epic_ep_t good = {.afk = afk, .ep = 0x21, .started = true};
    afk->endpoint[1] = &good;
    assert(afk_epic_shutdown(afk) == -1);
    assert(good.buf.freed && good.rxbuf.freed && good.txbuf.freed && good.released);
    assert(afk->endpoint[1] == NULL);
    assert(!freed(&stuck) && !stuck.released && afk->endpoint[0] == &stuck);
    assert(contexts_freed == 0);

    afk_epic_t *clean = &contexts[1];
    afk_epic_ep_t one = {.afk = clean, .ep = 0x22, .started = true};
    clean->endpoint[2] = &one;
    assert(afk_epic_shutdown(clean) == 0);
    assert(one.buf.freed && one.released && contexts_freed == 1);
    return 0;
}
'''
    compile_and_run(tmp_path, "epic-shutdown", harness)


def test_dcp_that_does_not_stop_keeps_buffers_and_mappings(tmp_path):
    source = (REPO / "src/dcp.c").read_text()
    bodies = "\n".join(function(source, name)
                       for name in ("dcp_mark_running", "dcp_left_running", "dcp_shutdown"))
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32;
typedef struct {
    void *asc, *system_ep, *dptx_ep, *dpav_ep, *phy, *afk, *rtkit;
    void *dart_disp, *iovad_dcp, *dart_dcp;
    bool no_reset;
} dcp_dev_t;
static char dcp_pmgr_dev[16] = "DISP0_CPU0";
static u32 dcp_die;
static bool dcp_running;
static int endpoint_result, afk_result, resets, released, darts, iovads, freed;
static bool stops, slept, quiesced;
static bool asc_is_v8(void *asc) { return false; }
static int dcp_system_shutdown(void *ep) { return endpoint_result; }
static int dcp_dptx_shutdown(void *ep) { return 0; }
static int dcp_dpav_shutdown(void *ep) { return 0; }
static int afk_epic_shutdown(void *afk) { return afk_result; }
static bool rtkit_sleep(void *rtk) { slept = true; return stops; }
static bool rtkit_quiesce(void *rtk) { quiesced = true; return stops; }
static void pmgr_reset(u32 die, const char *name) { resets++; }
static void rtkit_free(void *rtk) { released++; }
static void dart_shutdown(void *dart) { darts++; }
static void iovad_shutdown(void *iovad, void *dart) { iovads++; }
static void record_free(void *p) { freed++; }
#define free(p) record_free(p)
''' + bodies + r'''
#undef free
static void reset(int endpoint, int afk, bool stop)
{
    endpoint_result = endpoint, afk_result = afk, stops = stop;
    resets = released = darts = iovads = freed = 0;
    slept = quiesced = dcp_running = false;
}

static void kept(void)
{
    assert(released == 0 && darts == 0 && iovads == 0 && resets == 0);
    assert(dcp_left_running());
}

int main(void)
{
    dcp_dev_t dcp = {0};

    reset(0, 0, false);
    assert(dcp_shutdown(&dcp, false) == -1 && quiesced);
    kept();
    assert(freed == 1); /* only the PHY description; the DCP stays */

    reset(0, 0, false);
    assert(dcp_shutdown(&dcp, true) == -1 && slept);
    kept();

    reset(-1, 0, true);
    assert(dcp_shutdown(&dcp, false) == -1 && !quiesced && !slept);
    kept();

    reset(0, -1, true);
    assert(dcp_shutdown(&dcp, false) == -1 && !quiesced && !slept);
    kept();

    reset(0, 0, true);
    assert(dcp_shutdown(&dcp, true) == 0 && slept && resets == 1);
    assert(released == 1 && darts == 2 && iovads == 1 && freed == 2);
    assert(!dcp_left_running());
    return 0;
}
'''
    compile_and_run(tmp_path, "dcp-shutdown", harness)


def definition(source, name):
    """Like function(), for any return type, including pointers."""
    match = re.search(r"^\w[\w *]*?\b" + name + r"\([^;]*?\)\s*\{", source, re.M | re.S)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


def test_interface_of_an_endpoint_that_does_not_stop_stays_allocated(tmp_path):
    files = {
        "src/dcp/system_ep.c": ("dcp_system_init", "dcp_system_shutdown"),
        "src/dcp/dptx_port_ep.c": ("dcp_dptx_init", "dcp_dptx_shutdown"),
        "src/dcp/dpav_ep.c": ("dcp_dpav_init", "dcp_dpav_shutdown"),
        "src/dcp_iboot.c": ("dcp_ib_init", "dcp_ib_shutdown"),
        "src/dcp.c": ("dcp_mark_running", "dcp_left_running", "dcp_shutdown"),
    }
    bodies = "\n".join(definition((REPO / path).read_text(), name)
                       for path, names in files.items() for name in names)
    harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint32_t u32;
#define DCP_SYSTEM_ENDPOINT 0x20
#define DCP_IBOOT_ENDPOINT 0x23
#define DCP_DPAV_ENDPOINT 0x24
#define DCP_DPTX_PORT_ENDPOINT 0x2a
#define DCP_SYSTEM_NUM_SERVICES 2
#define DCP_IBOOT_NUM_SERVICES 1
#define DCP_DPAV_NUM_SERVICES 4
#define TXBUF_LEN 0x4000
#define RXBUF_LEN 0x4000
#define slot(ep) ((ep) - 0x20)
typedef struct { int unused; } afk_epic_t;
typedef struct { int unused; } afk_epic_service_ops_t;
typedef struct { int ep; } afk_epic_ep_t;
typedef struct dcp_dev dcp_dev_t;
/* One layout serves all four interfaces; each body uses only its own fields. */
struct interface {
    afk_epic_ep_t *epic;
    dcp_dev_t *dcp;
    void *sys_service;
    bool enabled;
};
typedef struct interface dcp_system_if_t, dcp_dptx_if_t, dcp_dpav_if_t, dcp_iboot_if_t;
struct dcp_dev {
    void *asc, *rtkit, *dart_disp, *iovad_dcp, *dart_dcp, *phy;
    afk_epic_t *afk;
    dcp_system_if_t *system_ep;
    dcp_dptx_if_t *dptx_ep;
    dcp_dpav_if_t *dpav_ep;
    bool no_reset;
};
static const afk_epic_service_ops_t dcp_system_ops[1], dcp_dptx_ops[1], dcp_dpav_ops[1],
    iboot_service_ops[1];
static char dcp_pmgr_dev[16] = "DISP0_CPU0";
static u32 dcp_die;
static bool dcp_running;

static afk_epic_ep_t endpoints[16];
/* The interface each started endpoint dispatches its callbacks to. */
static void *cookie[16];
static bool registered[16];
static int stuck, start_result, dptx_callbacks_during_dpav_stop;
static void *freed[64];
static int nfreed;

static bool was_freed(const void *p)
{
    for (int i = 0; i < nfreed; i++)
        if (freed[i] == p)
            return true;
    return false;
}
static void record_free(void *p)
{
    if (p)
        freed[nfreed++] = p;
}

static afk_epic_ep_t *afk_epic_start_ep(afk_epic_t *afk, int endpoint,
                                        const afk_epic_service_ops_t *ops, bool notify)
{
    endpoints[slot(endpoint)].ep = endpoint;
    return &endpoints[slot(endpoint)];
}
static int afk_epic_start_interface(afk_epic_ep_t *epic, void *intf, int expected, size_t insize,
                                    size_t outsize)
{
    cookie[slot(epic->ep)] = intf;
    registered[slot(epic->ep)] = true;
    return start_result;
}
/* Polling one endpoint can dispatch a notification for any endpoint that is
 * still registered, through the interface it was started with. */
static int afk_epic_shutdown_ep(afk_epic_ep_t *epic)
{
    for (int i = 0; i < 16; i++)
        if (registered[i])
            assert(!was_freed(cookie[i]));
    if (epic->ep == DCP_DPAV_ENDPOINT && registered[slot(DCP_DPTX_PORT_ENDPOINT)])
        dptx_callbacks_during_dpav_stop++;
    if (epic->ep == stuck)
        return -1;
    registered[slot(epic->ep)] = false;
    return 0;
}
static int afk_epic_shutdown(afk_epic_t *afk) { return 0; }
static bool asc_is_v8(void *asc) { return false; }
static bool rtkit_sleep(void *rtk) { return true; }
static bool rtkit_quiesce(void *rtk) { return true; }
static void pmgr_reset(u32 die, const char *name) {}
static void rtkit_free(void *rtk) {}
static void dart_shutdown(void *dart) {}
static void iovad_shutdown(void *iovad, void *dart) {}
#define free(p) record_free(p)
''' + bodies + r'''
#undef free

static void reset(int stuck_endpoint)
{
    memset(registered, 0, sizeof(registered));
    memset(cookie, 0, sizeof(cookie));
    nfreed = start_result = dptx_callbacks_during_dpav_stop = 0;
    stuck = stuck_endpoint;
    dcp_running = false;
}

static struct interface *started(int endpoint)
{
    struct interface *intf = calloc(1, sizeof(*intf));
    intf->epic = afk_epic_start_ep(NULL, endpoint, NULL, true);
    afk_epic_start_interface(intf->epic, intf, 1, 0, 0);
    return intf;
}

static dcp_dev_t *dcp_with_all_interfaces(void)
{
    dcp_dev_t *dcp = calloc(1, sizeof(*dcp));
    dcp->phy = calloc(1, 16);
    dcp->system_ep = started(DCP_SYSTEM_ENDPOINT);
    dcp->dptx_ep = started(DCP_DPTX_PORT_ENDPOINT);
    dcp->dpav_ep = started(DCP_DPAV_ENDPOINT);
    return dcp;
}

int main(void)
{
    static const struct {
        int endpoint;
        int (*stop)(struct interface *);
    } wrappers[] = {
        {DCP_SYSTEM_ENDPOINT, dcp_system_shutdown},
        {DCP_DPTX_PORT_ENDPOINT, dcp_dptx_shutdown},
        {DCP_DPAV_ENDPOINT, dcp_dpav_shutdown},
        {DCP_IBOOT_ENDPOINT, dcp_ib_shutdown},
    };
    for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++) {
        reset(wrappers[i].endpoint);
        struct interface *intf = started(wrappers[i].endpoint);
        assert(wrappers[i].stop(intf) == -1 && !was_freed(intf));

        reset(0);
        intf = started(wrappers[i].endpoint);
        assert(wrappers[i].stop(intf) == 0 && was_freed(intf));
    }

    /* An interface whose start failed is kept, too, if its endpoint then does not stop. */
    dcp_dev_t owner = {0};
    for (int stops = 0; stops < 2; stops++) {
        for (size_t i = 0; i < sizeof(wrappers) / sizeof(wrappers[0]); i++) {
            int endpoint = wrappers[i].endpoint;
            reset(stops ? 0 : endpoint);
            start_result = -1;
            struct interface *intf = endpoint == DCP_SYSTEM_ENDPOINT ? dcp_system_init(&owner)
                : endpoint == DCP_DPTX_PORT_ENDPOINT ? dcp_dptx_init(&owner, 2)
                : endpoint == DCP_DPAV_ENDPOINT      ? dcp_dpav_init(&owner)
                                                     : dcp_ib_init(&owner);
            assert(intf == NULL && cookie[slot(endpoint)]);
            assert(was_freed(cookie[slot(endpoint)]) == stops);
        }
    }

    /* A DPTX stop fails and DPAV is still stopped: its poll delivers a DPTX
     * callback, which must find the DPTX interface and its PHY intact. */
    reset(DCP_DPTX_PORT_ENDPOINT);
    dcp_dev_t *dcp = dcp_with_all_interfaces();
    struct interface *dptx = dcp->dptx_ep, *dpav = dcp->dpav_ep, *system = dcp->system_ep;
    void *phy = dcp->phy;
    assert(dcp_shutdown(dcp, false) == -1);
    assert(dptx_callbacks_during_dpav_stop == 1);
    assert(dcp->dptx_ep == dptx && !was_freed(dptx));
    assert(dcp->phy == phy && !was_freed(phy));
    assert(!dcp->system_ep && was_freed(system) && !dcp->dpav_ep && was_freed(dpav));
    assert(!was_freed(dcp) && dcp_left_running());

    /* Only the PHY's user keeps it: a stuck DPAV leaves the PHY free to go. */
    reset(DCP_DPAV_ENDPOINT);
    dcp = dcp_with_all_interfaces();
    dpav = dcp->dpav_ep;
    phy = dcp->phy;
    assert(dcp_shutdown(dcp, false) == -1);
    assert(dcp->dpav_ep == dpav && !was_freed(dpav));
    assert(!dcp->dptx_ep && !dcp->phy && was_freed(phy));
    assert(!was_freed(dcp) && dcp_left_running());

    reset(0);
    dcp = dcp_with_all_interfaces();
    dptx = dcp->dptx_ep, dpav = dcp->dpav_ep, system = dcp->system_ep, phy = dcp->phy;
    assert(dcp_shutdown(dcp, false) == 0);
    assert(was_freed(dptx) && was_freed(dpav) && was_freed(system) && was_freed(phy));
    assert(was_freed(dcp) && !dcp_left_running());
    return 0;
}
'''
    compile_and_run(tmp_path, "interface-lifetime", harness)
