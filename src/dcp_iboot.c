/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "dcp_iboot.h"
#include "afk.h"
#include "assert.h"
#include "firmware.h"
#include "malloc.h"
#include "string.h"
#include "utils.h"

#define DCP_IBOOT_ENDPOINT     0x23
#define DCP_IBOOT_NUM_SERVICES 1

#define TXBUF_LEN 0x4000
#define RXBUF_LEN 0x4000

/* Payload bytes reside in the fixed-size transport buffers. */
struct txcmd {
    u32 op;
    u32 len;
    u32 unk1;
    u32 unk2;
};

struct rxcmd {
    u32 op;
    u32 len;
};

struct dcp_iboot_if {
    dcp_dev_t *dcp;
    afk_epic_ep_t *epic;
    int channel;
    bool enabled;

    union {
        u8 txbuf[TXBUF_LEN];
        struct txcmd txcmd;
    };

    union {
        u8 rxbuf[RXBUF_LEN];
        struct rxcmd rxcmd;
    };
};

enum IBootCmd {
    IBOOT_SET_SURFACE = 1,
    IBOOT_SET_POWER = 2,
    IBOOT_GET_HPD = 3,
    IBOOT_GET_TIMING_MODES = 4,
    IBOOT_GET_COLOR_MODES = 5,
    IBOOT_SET_MODE = 6,
    IBOOT_SWAP_BEGIN = 15,
    IBOOT_SWAP_SET_LAYER = 16,
    IBOOT_SWAP_END = 18,
};

struct get_hpd_resp {
    u8 hpd;
    u8 pad[3];
    u32 timing_cnt;
    u32 color_cnt;
};

struct get_tmode_resp {
    u32 count;
    dcp_timing_mode_t modes[];
};

struct get_cmode_resp {
    u32 count;
    dcp_color_mode_t modes[];
};

struct swap_start_resp {
    u32 unk1, unk2, unk3;
    u32 swap_id;
    u32 unk4;
};

struct swap_set_layer_cmd {
    u32 unk;
    u32 layer_id;
    dcp_layer_t layer;
    dcp_rect_t src;
    dcp_rect_t dst;
    u32 unk2;
} PACKED;

struct swap_set_layer_cmd_v13_3 {
    u32 unk;
    u32 layer_id;
    dcp_layer_t layer;
    u32 unk3; // possibly part of layer
    u32 unk4; // possibly part of layer
    dcp_rect_t src;
    dcp_rect_t dst;
    u32 unk2;
} PACKED;

void dcp_ib_service_init(afk_epic_service_t *service, const char *name, const char *eclass,
                         s64 unit)
{
    dcp_iboot_if_t *iboot = service->intf;
    if (strncmp("disp0-service", eclass, 32) == 0) {
        if (iboot->enabled) {
            printf("dcp-iboot: service init for enabled 'disp0-service' on channel: %d\n",
                   iboot->channel);
            return;
        }
        iboot->enabled = true;
        iboot->channel = service->channel;
    }
    UNUSED(name);
    UNUSED(unit);
}

static const afk_epic_service_ops_t iboot_service_ops[] = {
    {
        .name = "disp0-service",
        .init = dcp_ib_service_init,
    },
    {},
};

dcp_iboot_if_t *dcp_ib_init(dcp_dev_t *dcp)
{
    dcp_iboot_if_t *iboot = calloc(1, sizeof(dcp_iboot_if_t));
    if (!iboot)
        return NULL;

    iboot->dcp = dcp;
    iboot->epic = afk_epic_start_ep(dcp->afk, DCP_IBOOT_ENDPOINT, iboot_service_ops, false);
    if (!iboot->epic) {
        printf("dcp-iboot: failed to initialize EPIC\n");
        goto err_free;
    }

    int err =
        afk_epic_start_interface(iboot->epic, iboot, DCP_IBOOT_NUM_SERVICES, TXBUF_LEN, RXBUF_LEN);

    if (err < 0 || !iboot->enabled) {
        printf("dcp-iboot: failed to initialize disp0 service\n");
        goto err_shutdown;
    }

    return iboot;

err_shutdown:
    /* Callbacks from an endpoint that did not stop can still reach this. */
    if (afk_epic_shutdown_ep(iboot->epic) < 0)
        return NULL;
err_free:
    free(iboot);
    return NULL;
}

int dcp_ib_shutdown(dcp_iboot_if_t *iboot)
{
    /* An endpoint that did not stop can still call back into this interface. */
    if (afk_epic_shutdown_ep(iboot->epic) < 0)
        return -1;
    free(iboot);
    return 0;
}

static int dcp_ib_cmd(dcp_iboot_if_t *iboot, int op, size_t in_size)
{
    size_t rxsize = RXBUF_LEN;
    /* The 26A428 AFKEPV2 handlers return only transport status for these
     * setters. Supplying a reply buffer makes firmware try to copy an empty
     * message and assert while queueing the response. */
    bool setter = op == IBOOT_SET_POWER || op == IBOOT_SET_MODE || op == IBOOT_SET_SURFACE;
    bool status_only = afk_epic_is_v2(iboot->epic) && setter;
    assert(in_size <= TXBUF_LEN - sizeof(struct txcmd));

    iboot->txcmd.op = op;
    iboot->txcmd.len = sizeof(struct txcmd) + in_size;

    int ret = afk_epic_command(iboot->epic, iboot->channel, 0xc0, iboot->txbuf,
                               sizeof(struct txcmd) + in_size, status_only ? NULL : iboot->rxbuf,
                               status_only ? NULL : &rxsize);
    if (ret)
        return ret;
    if (status_only)
        return 0;
    /* AFKEPV1 setters can also answer with transport status alone. J293
     * SET_POWER was observed replying with no bytes at all. SET_MODE and
     * SET_SURFACE carry no reply payload either (by the iBoot command
     * contract), so their empty reply is admitted the same way; a non-empty
     * reply must still be a well-formed header. */
    if (setter && !rxsize)
        return 0;
    if (rxsize < sizeof(struct rxcmd) || iboot->rxcmd.len < sizeof(struct rxcmd) ||
        iboot->rxcmd.len > rxsize) {
        printf("dcp-iboot: invalid response length for operation %d (%zu bytes)\n", op, rxsize);
        return -1;
    }
    size_t payload_size = iboot->rxcmd.len - sizeof(struct rxcmd);
    if ((op == IBOOT_GET_HPD && payload_size < sizeof(struct get_hpd_resp)) ||
        (op == IBOOT_SWAP_BEGIN && payload_size < sizeof(struct swap_start_resp)))
        return -1;
    if (op == IBOOT_GET_TIMING_MODES) {
        struct get_tmode_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
        if (payload_size < sizeof(*resp) ||
            resp->count > (payload_size - sizeof(*resp)) / sizeof(resp->modes[0]))
            return -1;
    }
    if (op == IBOOT_GET_COLOR_MODES) {
        struct get_cmode_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
        if (payload_size < sizeof(*resp) ||
            resp->count > (payload_size - sizeof(*resp)) / sizeof(resp->modes[0]))
            return -1;
    }
    return 0;
}

int dcp_ib_set_surface(dcp_iboot_if_t *iboot, dcp_layer_t *layer)
{
    dcp_layer_t *cmd = (void *)(iboot->txbuf + sizeof(struct txcmd));
    *cmd = *layer;

    return dcp_ib_cmd(iboot, IBOOT_SET_SURFACE, sizeof(*layer));
}

int dcp_ib_set_power(dcp_iboot_if_t *iboot, bool power)
{
    u32 *pwr = (void *)(iboot->txbuf + sizeof(struct txcmd));
    *pwr = power;

    return dcp_ib_cmd(iboot, IBOOT_SET_POWER, 1);
}

int dcp_ib_get_hpd(dcp_iboot_if_t *iboot, int *timing_cnt, int *color_cnt)
{
    struct get_hpd_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
    int ret = dcp_ib_cmd(iboot, IBOOT_GET_HPD, 0);

    if (ret < 0)
        return ret;

    if (timing_cnt)
        *timing_cnt = resp->timing_cnt;
    if (color_cnt)
        *color_cnt = resp->color_cnt;

    return !!resp->hpd;
}

int dcp_ib_get_timing_modes(dcp_iboot_if_t *iboot, dcp_timing_mode_t **modes)
{
    struct get_tmode_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
    int ret = dcp_ib_cmd(iboot, IBOOT_GET_TIMING_MODES, 0);

    if (ret < 0)
        return ret;

    *modes = resp->modes;
    return resp->count;
}

int dcp_ib_get_color_modes(dcp_iboot_if_t *iboot, dcp_color_mode_t **modes)
{
    struct get_cmode_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
    int ret = dcp_ib_cmd(iboot, IBOOT_GET_COLOR_MODES, 0);

    if (ret < 0)
        return ret;

    *modes = resp->modes;
    return resp->count;
}

int dcp_ib_set_mode(dcp_iboot_if_t *iboot, dcp_timing_mode_t *tmode, dcp_color_mode_t *cmode)
{
    struct {
        dcp_timing_mode_t tmode;
        dcp_color_mode_t cmode;
    } *cmd = (void *)(iboot->txbuf + sizeof(struct txcmd));

    cmd->tmode = *tmode;
    cmd->cmode = *cmode;
    return dcp_ib_cmd(iboot, IBOOT_SET_MODE, sizeof(*cmd));
}

int dcp_ib_swap_begin(dcp_iboot_if_t *iboot)
{
    struct swap_start_resp *resp = (void *)(iboot->rxbuf + sizeof(struct rxcmd));
    int ret = dcp_ib_cmd(iboot, IBOOT_SWAP_BEGIN, 0);
    if (ret < 0)
        return ret;

    return resp->swap_id;
}

static int swap_set_layer_v12_3(dcp_iboot_if_t *iboot, int layer_id, dcp_layer_t *layer,
                                dcp_rect_t *src_rect, dcp_rect_t *dst_rect)
{
    struct swap_set_layer_cmd *cmd = (void *)(iboot->txbuf + sizeof(struct txcmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->layer_id = layer_id;
    cmd->layer = *layer;
    cmd->src = *src_rect;
    cmd->dst = *dst_rect;

    return dcp_ib_cmd(iboot, IBOOT_SWAP_SET_LAYER, sizeof(*cmd));
}

static int swap_set_layer_v13_3(dcp_iboot_if_t *iboot, int layer_id, dcp_layer_t *layer,
                                dcp_rect_t *src_rect, dcp_rect_t *dst_rect)
{
    struct swap_set_layer_cmd_v13_3 *cmd = (void *)(iboot->txbuf + sizeof(struct txcmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->layer_id = layer_id;
    cmd->layer = *layer;
    cmd->src = *src_rect;
    cmd->dst = *dst_rect;

    return dcp_ib_cmd(iboot, IBOOT_SWAP_SET_LAYER, sizeof(*cmd));
}

int dcp_ib_swap_set_layer(dcp_iboot_if_t *iboot, int layer_id, dcp_layer_t *layer,
                          dcp_rect_t *src_rect, dcp_rect_t *dst_rect)
{
    if (os_firmware.version < V13_3)
        return swap_set_layer_v12_3(iboot, layer_id, layer, src_rect, dst_rect);
    else
        return swap_set_layer_v13_3(iboot, layer_id, layer, src_rect, dst_rect);
}

int dcp_ib_swap_end(dcp_iboot_if_t *iboot)
{
    memset(iboot->txbuf + sizeof(struct txcmd), 0, 12);
    return dcp_ib_cmd(iboot, IBOOT_SWAP_END, 12);
}
