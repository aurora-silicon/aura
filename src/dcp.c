/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../config.h"

#include "adt.h"
#include "afk.h"
#include "dcp.h"
#include "firmware.h"
#include "malloc.h"
#include "pmgr.h"
#include "rtkit.h"
#include "smc.h"
#include "string.h"
#include "utils.h"

#include "dcp/dptx_phy.h"

struct adt_function_smc_gpio {
    u32 phandle;
    char four_cc[4];
    u32 gpio;
    u32 unk;
};

/* T8152 encodes the pcon power enable as pKW8("pcIO", lo, hi): an eight-byte
 * SMC write of (hi << 32) | lo | level. */
struct adt_function_smc_pw8 {
    u32 phandle;
    char four_cc[4];
    u32 key;
    u32 lo;
    u32 hi;
};

/* T8152 PMGR2 PSD service blocks (as in t8152-j873-gpu.dtsi): request at
 * +0x90, acknowledge bit 0 at +0xbc, state at +0x80 (read after the ACK). */
#define T8152_PSD_BASE    0x303a10000
#define T8152_PSD_STRIDE  0x100
#define T8152_PSD_STATE   0x80
#define T8152_PSD_REQUEST 0x90
#define T8152_PSD_ACK     0xbc

static int t8152_psd_power_on(u32 service)
{
    u64 base = T8152_PSD_BASE + service * T8152_PSD_STRIDE;

    if (!read32(base + T8152_PSD_STATE))
        return 0;
    write32(base + T8152_PSD_REQUEST, 0);
    if (poll32(base + T8152_PSD_ACK, 1, 1, 1000000)) {
        printf("dcp: PSD service %u did not acknowledge power on\n", service);
        return -1;
    }
    u32 state = read32(base + T8152_PSD_STATE);
    printf("dcp: PSD service %u state %u\n", service, state);
    return state ? -1 : 0;
}

static char dcp_pmgr_dev[16] = "DISP0_CPU0";
static u32 dcp_die;

/* Set when a DCP did not stop. Its buffers and mappings are kept, and the next
 * stage must not be entered while the firmware may still use them. */
static bool dcp_running;

static void dcp_mark_running(void)
{
    printf("dcp: firmware did not stop; keeping its buffers and mappings\n");
    dcp_running = true;
}

bool dcp_left_running(void)
{
    return dcp_running;
}

static int dcp_hdmi_dptx_init(dcp_dev_t *dcp, const display_config_t *cfg)
{
    int node = adt_path_offset(adt, cfg->dp2hdmi_gpio);
    if (node < 0) {
        printf("dcp: failed to find dp2hdmi-gpio node '%s'\n", cfg->dp2hdmi_gpio);
        return -1;
    }
    struct adt_function_smc_gpio dp2hdmi_pwr, hdmi_pwr;
    u32 pw8_size;
    const struct adt_function_smc_pw8 *pw8_dp2hdmi =
        adt_getprop(adt, node, "function-dp2hdmi_pwr_en", &pw8_size);

    if (pw8_dp2hdmi && pw8_size == sizeof(*pw8_dp2hdmi)) {
        u32 hdmi_size;
        const struct adt_function_smc_pw8 *pw8_hdmi =
            adt_getprop(adt, node, "function-hdmi_pwr_en", &hdmi_size);
        smc_dev_t *smc = smc_init();
        if (smc) {
            int r1 = smc_write_u64(smc, pw8_dp2hdmi->key,
                                   ((u64)pw8_dp2hdmi->hi << 32) | pw8_dp2hdmi->lo | 1);
            int r2 = -1;
            if (pw8_hdmi && hdmi_size == sizeof(*pw8_hdmi))
                r2 =
                    smc_write_u64(smc, pw8_hdmi->key, ((u64)pw8_hdmi->hi << 32) | pw8_hdmi->lo | 1);
            if (r1 || r2)
                printf("dcp: pcon power failed: dp2hdmi=%d hdmi=%d\n", r1, r2);
            smc_shutdown(smc);
        } else {
            printf("dcp: failed to initialize SMC for pcon power\n");
        }
    } else {
        int err = adt_getprop_copy(adt, node, "function-dp2hdmi_pwr_en", &dp2hdmi_pwr,
                                   sizeof(dp2hdmi_pwr));
        if (err < 0)
            printf("dcp: failed to get dp2hdmi_pwr_en gpio\n");
        else
            dcp->dp2hdmi_pwr_gpio = dp2hdmi_pwr.gpio;
        err = adt_getprop_copy(adt, node, "function-hdmi_pwr_en", &hdmi_pwr, sizeof(hdmi_pwr));
        if (err < 0)
            printf("dcp: failed to get hdmi_pwr_en gpio\n");
        else
            dcp->hdmi_pwr_gpio = hdmi_pwr.gpio;

        if (dcp->dp2hdmi_pwr_gpio && dcp->hdmi_pwr_gpio) {
            smc_dev_t *smc = smc_init();
            if (smc) {
                smc_write_u32(smc, dcp->dp2hdmi_pwr_gpio, 0x800001);
                smc_write_u32(smc, dcp->hdmi_pwr_gpio, 0x800001);
                smc_shutdown(smc);
            }
        }
    }

    dcp->die = cfg->die;

    dcp->phy = dptx_phy_init(cfg->dptx_phy, cfg->dcp_index);
    if (!dcp->phy) {
        printf("dcp: failed to init (lp)dptx-phy '%s'\n", cfg->dptx_phy);
        return -1;
    }

    dcp->dpav_ep = dcp_dpav_init(dcp);
    if (!dcp->dpav_ep) {
        printf("dcp: failed to initialize dpav endpoint\n");
        return -1;
    }

    dcp->dptx_ep = dcp_dptx_init(dcp, cfg->num_dptxports);
    if (!dcp->dptx_ep) {
        printf("dcp: failed to initialize dptx-port endpoint\n");
        dcp_dpav_shutdown(dcp->dpav_ep);
        return -1;
    }

#ifdef RTKIT_SYSLOG
    // start system endpoint when extended logging is requested
    dcp->system_ep = dcp_system_init(dcp);
    if (!dcp->system_ep) {
        printf("dcp: failed to initialize system endpoint\n");
        dcp_dptx_shutdown(dcp->dptx_ep);
        dcp_dpav_shutdown(dcp->dpav_ep);
        return -1;
    }

    dcp_system_set_property_u64(dcp->system_ep, "gAFKConfigLogMask", 0xffff);
#endif

    return 0;
}

int dcp_connect_dptx(dcp_dev_t *dcp)
{
    if (dcp->dptx_ep && dcp->phy) {
        return dcp_dptx_connect(dcp->dptx_ep, dcp->phy, dcp->die, 0);
    }

    return 0;
}

int dcp_work(dcp_dev_t *dcp)
{
    return afk_epic_work(dcp->afk, -1);
}

static int dcp_create_firmware_mappings(const display_config_t *cfg, dcp_dev_t *dcp)
{
    int created = 0;
    int dcp_node = adt_path_offset(adt, cfg->dcp);
    int node = adt_first_child_offset(adt, dcp_node);
    if (node < 0) {
        printf("dcp: iop-dcp*-nub not found!\n");
        return -1;
    }

    u64 asc_dram_mask;
    if (ADT_GETPROP(adt, node, "asc-dram-mask", &asc_dram_mask) < 0)
        asc_dram_mask = 0;

    const struct adt_segment_ranges *seg;
    u32 segments_len;

    seg = adt_getprop(adt, node, "segment-ranges", &segments_len);
    unsigned int count = segments_len / sizeof(*seg);

    for (unsigned int i = 0; i < count; i++) {
        u64 iova = seg[i].remap & ~asc_dram_mask;
        if (dart_translate_silent(dcp->dart_dcp, iova))
            continue;

        size_t len = ALIGN_UP(seg[i].size, SZ_16K);
        u32 flags = i == 0 ? 0b0100 : 0; // TEXT gets this bit set?
        printf("dcp: Mapping segment #%u %lx -> %lx [%lx]\n", i, iova, seg[i].phys, len);
        if (dart_map_flags(dcp->dart_dcp, iova, (void *)seg[i].phys, len, flags)) {
            printf("dcp: Failed to map segment\n");
            return -1;
        }
        created++;
    }

    return created;
}

dcp_dev_t *dcp_init(const display_config_t *cfg)
{
    u32 sid;

    if (cfg && cfg->dptx_phy[0]) {
        /* J873g reports mBoot-20457.1.29, which is not in the OS firmware
         * table, and the same dptx-port endpoint works on its USB-C route.
         * Keep the V13_5 restriction for the machines it was written for. */
        if (os_firmware.version != V13_5 && chip_id != T8152) {
            printf("dcp: dtpx-port is only supported with V13_5 OS firmware.\n");
            return NULL;
        }

        strncpy(dcp_pmgr_dev, cfg->pmgr_dev, sizeof(dcp_pmgr_dev));
        dcp_die = cfg->die;
        pmgr_adt_power_enable(cfg->dcp);
        int phy_node = adt_path_offset(adt, cfg->dptx_phy);
        if (phy_node >= 0 && adt_is_compatible(adt, phy_node, "dptx-phy,t8152")) {
            /* The T8152 dptx-phy node lists gate 5 in power-gates, which is
             * an E-core, not the phy's PMGR device. Enabling it leaves the
             * phy gated and the first register access aborts. */
            if (pmgr_power_on(dcp_die, "DPTX_PHY") < 0) {
                printf("dcp: failed to power DPTX_PHY\n");
                return NULL;
            }
            /* On PMGR2 that gate is a PSD service, as the GPU's sgx/gfx-asc
             * gates are. It idles at state 6 until requested; the register
             * interface works without it, the transmitter does not. */
            u32 gate;
            if (ADT_GETPROP(adt, phy_node, "power-gates", &gate) < 0 ||
                t8152_psd_power_on(gate) < 0) {
                printf("dcp: failed to power dptx-phy PSD service\n");
                return NULL;
            }
        } else {
            pmgr_adt_power_enable(cfg->dptx_phy);
        }
        mdelay(25);
    }

    int dart_node = adt_path_offset(adt, cfg->dcp_dart);
    int node = adt_first_child_offset(adt, dart_node);
    if (node < 0) {
        printf("dcp: mapper-dcp* not found!\n");
        return NULL;
    }
    if (ADT_GETPROP(adt, node, "reg", &sid) < 0) {
        printf("dcp: failed to read dart stream ID!\n");
        return NULL;
    }

    dcp_dev_t *dcp = calloc(1, sizeof(dcp_dev_t));
    if (!dcp)
        return NULL;

    dcp->dart_dcp = dart_init_adt_display(cfg->dcp_dart, 0, sid);
    if (!dcp->dart_dcp) {
        printf("dcp: failed to initialize DCP DART\n");
        goto out_free;
    }
    u64 vm_base = dart_vm_base(dcp->dart_dcp);
    dart_setup_pt_region(dcp->dart_dcp, cfg->dcp_dart, sid, vm_base);

    dcp->dart_disp = dart_init_adt(cfg->disp_dart, 0, 0, true);
    if (!dcp->dart_disp) {
        printf("dcp: failed to initialize DISP DART\n");
        goto out_dart_dcp;
    }
    // set disp0's page tables at dart-dcp's vm-base
    dart_setup_pt_region(dcp->dart_disp, cfg->disp_dart, 0, vm_base);

    dcp->iovad_dcp = iovad_init(vm_base + 0x10000000, vm_base + 0x20000000);

    int ret = dcp_create_firmware_mappings(cfg, dcp);
    if (ret < 0) {
        printf("dcp: failed to create firmware mappings\n");
        goto out_iovad;
    }
    if (ret > 0) {
        pmgr_reset(dcp_die, dcp_pmgr_dev);
    }

    dcp->asc = asc_init(cfg->dcp);
    if (!dcp->asc) {
        printf("dcp: failed to initialize ASC\n");
        goto out_iovad;
    }

    dcp->rtkit = rtkit_init("dcp", dcp->asc, dcp->dart_dcp, dcp->iovad_dcp, NULL, false);
    if (!dcp->rtkit) {
        printf("dcp: failed to initialize RTKit\n");
        goto out_iovad;
    }

    /* Keep unqualified ASCWrap-v8 firmware from trapping startup in a wait. */
    bool booted =
        asc_is_v8(dcp->asc) ? rtkit_boot_timed(dcp->rtkit, 2000000) : rtkit_boot(dcp->rtkit);
    if (!booted) {
        printf("dcp: failed to boot RTKit\n");
        if (rtkit_has_owned_buffers(dcp->rtkit))
            goto out_running;
        goto out_iovad;
    }

    dcp->afk = afk_epic_init(dcp->rtkit);
    if (!dcp->afk) {
        printf("dcp: failed to initialize AFK\n");
        goto out_rtkit;
    }

    if (cfg && cfg->dptx_phy[0]) {
        int ret = dcp_hdmi_dptx_init(dcp, cfg);
        if (ret < 0)
            goto out_afk;
    }

    return dcp;

out_afk:
    if (afk_epic_shutdown(dcp->afk) < 0)
        goto out_running;
out_rtkit:
    if (!rtkit_quiesce(dcp->rtkit))
        goto out_running;
    rtkit_free(dcp->rtkit);
out_iovad:
    iovad_shutdown(dcp->iovad_dcp, dcp->dart_dcp);
    dart_shutdown(dcp->dart_disp);
out_dart_dcp:
    dart_shutdown(dcp->dart_dcp);
out_free:
    free(dcp);
    return NULL;
out_running:
    dcp_mark_running();
    return NULL;
}

int dcp_shutdown(dcp_dev_t *dcp, bool sleep)
{
    /* The ASCWrap-v8 sleep path needs a PMGR2 reset, which is not implemented
     * yet. Quiescing has a working RTKit wake/renegotiation path on T8152. */
    if (sleep && asc_is_v8(dcp->asc)) {
        printf("dcp: using quiesce instead of sleep without PMGR2 reset\n");
        sleep = false;
    }
    /* dcp/dcp0 on desktop M2 and M2 Pro/Max devices do not wake from sleep */
    /* An interface whose endpoint did not stop stays allocated and attached,
     * since its callbacks can still run while another endpoint is polled. */
    int ret = 0;
    if (dcp_system_shutdown(dcp->system_ep) < 0)
        ret = -1;
    else
        dcp->system_ep = NULL;
    if (dcp_dptx_shutdown(dcp->dptx_ep) < 0)
        ret = -1;
    else
        dcp->dptx_ep = NULL;
    if (dcp_dpav_shutdown(dcp->dpav_ep) < 0)
        ret = -1;
    else
        dcp->dpav_ep = NULL;
    /* The DPTX ports use the PHY until their endpoint has stopped. */
    if (!dcp->dptx_ep) {
        free(dcp->phy);
        dcp->phy = NULL;
    }
    /* The firmware may still use every buffer and mapping below until it has
     * stopped, so keep them all and report the failure. */
    if (ret < 0 || afk_epic_shutdown(dcp->afk) < 0) {
        dcp_mark_running();
        return -1;
    }
    if (!(sleep ? rtkit_sleep(dcp->rtkit) : rtkit_quiesce(dcp->rtkit))) {
        dcp_mark_running();
        return -1;
    }
    if (sleep)
        pmgr_reset(dcp_die, dcp_pmgr_dev);
    rtkit_free(dcp->rtkit);
    dart_shutdown(dcp->dart_disp);
    iovad_shutdown(dcp->iovad_dcp, dcp->dart_dcp);
    dart_shutdown(dcp->dart_dcp);
    free(dcp);

    return 0;
}
