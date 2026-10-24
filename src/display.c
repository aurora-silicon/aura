/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#include "../build/build_cfg.h"

#include "display.h"
#include "adt.h"
#include "assert.h"
#include "dcp.h"
#include "dcp_iboot.h"
#include "fb.h"
#include "firmware.h"
#include "memory.h"
#include "soc.h"
#include "string.h"
#include "utils.h"
#include "xnuboot.h"

#include "dcp/dptx_phy.h"

#define DISPLAY_STATUS_DELAY         100
#define DISPLAY_STATUS_RETRIES(dptx) ((dptx) ? 100 : 20)

#define COMPARE(a, b)                                                                              \
    if ((a) > (b)) {                                                                               \
        *best = modes[i];                                                                          \
        continue;                                                                                  \
    } else if ((a) < (b)) {                                                                        \
        continue;                                                                                  \
    }

static dcp_dev_t *dcp;
static dcp_iboot_if_t *iboot;
static u64 fb_dva;
static u64 fb_size;
bool has_dcp;
bool display_is_external;
bool display_is_dptx;
bool display_needs_power_cycle;

static const display_config_t display_config_m1 = {
    .dcp = "/arm-io/dcp",
    .dcp_dart = "/arm-io/dart-dcp",
    .disp_dart = "/arm-io/dart-disp0",
    .pmgr_dev = "DISP0_CPU0",
    .dcp_alias = "dcp",
};

/* J873g HDMI: as on macOS, the base DCP (dispint0) drives the dptx-phy in
 * display-crossbar0's lpdptx slot, through the PS190 DP-to-HDMI converter
 * powered from dp2hdmi-gpio0. */
static const display_config_t display_config_j873_hdmi = {
    .dcp = "/arm-io/dcp",
    .dcp_dart = "/arm-io/dart-dcp",
    .disp_dart = "/arm-io/dart-disp0",
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio0",
    .dptx_phy = "/arm-io/dptx-phy",
    .pmgr_dev = "DISP0_CPU0",
    .dcp_alias = "dcp",
    .dcp_index = 0,
    .num_dptxports = 2,
};

/* J873g HDMI next to a USB-C display: the base DCP drives USB-C, so HDMI goes
 * through DCPEXT0 (dispext0, source select 1). */
static const display_config_t display_config_j873_hdmi_ext = {
    .dcp = "/arm-io/dcpext0",
    .dcp_dart = "/arm-io/dart-dcpext0",
    .disp_dart = "/arm-io/dart-dispext0",
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio0",
    .dptx_phy = "/arm-io/dptx-phy",
    .pmgr_dev = "DISPEXT0_CPU0",
    .dcp_alias = "dcpext0",
    .dcp_index = 1,
    .num_dptxports = 2,
};

/* Config used by display_start_dcp(); the USB-C route keeps the base DCP. */
static const display_config_t *t8152_config = &display_config_m1;

/* J873g second display (HDMI while USB-C is up). Its state is kept here and
 * swapped with the globals while it is configured; the console stays on the
 * first display. The loader reads display_second_video. */
static struct {
    dcp_dev_t *dcp;
    dcp_iboot_if_t *iboot;
    u64 fb_dva, fb_size;
    const display_config_t *cfg;
    struct boot_video video;
} display_other;
struct boot_video display_second_video;
/* Framebuffer size display_start_dcp maps instead of /vram (the first
 * display's memory) when bringing up the second display. */
static u64 display_fb_override_size;
/* "max" mode config: the largest of the sink's own modes, without the
 * boot-console preference for modes up to 1920x1200 or for ones that fit the
 * current framebuffer. */
static bool display_mode_max;

#define USE_DCPEXT 1

static const display_config_t display_config_m2 = {
#if USE_DCPEXT
    .dcp = "/arm-io/dcpext",
    .dcp_dart = "/arm-io/dart-dcpext",
    .disp_dart = "/arm-io/dart-dispext0",
    .pmgr_dev = "DISPEXT_CPU0",
    .dcp_alias = "dcpext",
    .dcp_index = 1,
#else
    .dcp = "/arm-io/dcp",
    .dcp_dart = "/arm-io/dart-dcp",
    .disp_dart = "/arm-io/dart-disp0",
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio",
    .dptx_phy = "/arm-io/dptx-phy",
    .pmgr_dev = "DISP0_CPU0",
    .dcp_alias = "dcp",
    .dcp_index = 0,
#endif
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio",
    .dptx_phy = "/arm-io/dptx-phy",
    .num_dptxports = 2,
};

static const display_config_t display_config_m2_pro_max = {
#if USE_DCPEXT
    .dcp = "/arm-io/dcpext0",
    .dcp_dart = "/arm-io/dart-dcpext0",
    .disp_dart = "/arm-io/dart-dispext0",
    .pmgr_dev = "DISPEXT0_CPU0",
    .dcp_alias = "dcpext0",
    .dcp_index = 1,
    .num_dptxports = 2,
#else
    .dcp = "/arm-io/dcp0",
    .dcp_dart = "/arm-io/dart-dcp0",
    .disp_dart = "/arm-io/dart-disp0",
    .pmgr_dev = "DISP0_CPU0",
    .dcp_alias = "dcp",
    .dcp_index = 0,
    .num_dptxports = 1,
#endif
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio0",
    .dptx_phy = "/arm-io/lpdptx-phy0",
};

static const display_config_t display_config_m2_ultra = {
    .dcp = "/arm-io/dcpext4",
    .dcp_dart = "/arm-io/dart-dcpext4",
    .disp_dart = "/arm-io/dart-dispext4",
    .dp2hdmi_gpio = "/arm-io/dp2hdmi-gpio1",
    .dptx_phy = "/arm-io/lpdptx-phy1",
    .pmgr_dev = "DISPEXT0_CPU0",
    .dcp_alias = "dcpext4",
    .dcp_index = 1,
    .num_dptxports = 2,
    .die = 1,
};

#define abs(x) ((x) >= 0 ? (x) : -(x))

u64 display_mode_fb_size(dcp_timing_mode_t *mode)
{
    // assume 4 byte per pixel (either BGRA x2r10b10g10)
    return mode->width * mode->height * 4;
}

static void display_choose_timing_mode(dcp_timing_mode_t *modes, int cnt, dcp_timing_mode_t *best,
                                       dcp_timing_mode_t *want)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        if (want && want->valid) {
            COMPARE(modes[i].width == want->width && modes[i].height == want->height,
                    best->width == want->width && best->height == want->height);
            COMPARE(-abs((long)modes[i].fps - (long)want->fps),
                    -abs((long)best->fps - (long)want->fps));
        } else if (!display_mode_max) {
            COMPARE(display_mode_fb_size(&modes[i]) <= fb_size,
                    display_mode_fb_size(best) <= fb_size);
        }

        if (!display_mode_max) {
            COMPARE(modes[i].width <= 1920, best->width <= 1920);
            COMPARE(modes[i].height <= 1200, best->height <= 1200);
        } else {
            /* Byte 0 bit 6 marks standard modes a converter adds on its
             * own (the J873g PS190 lists 3840x2160 for a 2560x1440 EIZO);
             * the sink's EDID modes have it clear. */
            COMPARE(!(modes[i].pad[0] & 0x40), !(best->pad[0] & 0x40));
        }
        COMPARE(modes[i].fps <= 60 << 16, best->fps <= 60 << 16);
        COMPARE(modes[i].width, best->width);
        COMPARE(modes[i].height, best->height);
        COMPARE(modes[i].fps, best->fps);
    }

    printf("display: timing mode: valid=%d %dx%d %d.%02d Hz\n", best->valid, best->width,
           best->height, best->fps >> 16, ((best->fps & 0xffff) * 100 + 0x7fff) >> 16);
}

static void display_choose_color_mode(dcp_color_mode_t *modes, int cnt, dcp_color_mode_t *best)
{
    *best = modes[0];

    for (int i = 1; i < cnt; i++) {
        COMPARE(modes[i].valid, best->valid);
        COMPARE(modes[i].bpp <= 32, best->bpp <= 32);
        COMPARE(modes[i].bpp, best->bpp);
        COMPARE(-modes[i].colorimetry, -best->colorimetry);
        COMPARE(-modes[i].encoding, -best->encoding);
        COMPARE(-modes[i].eotf, -best->eotf);
    }

    printf("display: color mode: valid=%d colorimetry=%d eotf=%d encoding=%d bpp=%d\n", best->valid,
           best->colorimetry, best->eotf, best->encoding, best->bpp);
}

int display_get_vram(u64 *paddr, u64 *size)
{
    int ret = 0;
    int adt_path[4];
    int node = adt_path_offset_trace(adt, "/vram", adt_path);

    if (node < 0) {
        printf("display: '/vram' not found\n");
        return -1;
    }

    int pp = 0;
    while (adt_path[pp])
        pp++;
    adt_path[pp + 1] = 0;

    ret = adt_get_reg(adt, adt_path, "reg", 0, paddr, size);
    if (ret < 0) {
        printf("display: failed to read /vram/reg\n");
        return -1;
    }

    if (*paddr != cur_boot_args.video.base) {
        printf("display: vram does not match boot_args.video.base\n");
        return -1;
    }

    return 0;
}

static uintptr_t display_map_fb(uintptr_t iova, u64 paddr, u64 size)
{
    if (iova == 0) {
        u64 iova_disp0 = 0;
        u64 iova_dcp = 0;

        // start scanning for free iova space on vm-base
        iova_dcp = dart_find_iova(dcp->dart_dcp, dart_vm_base(dcp->dart_dcp) + SZ_16K, size);
        if (DART_IS_ERR(iova_dcp)) {
            printf("display: failed to find IOVA for fb of %06zx bytes (dcp)\n", size);
            return iova_dcp;
        }

        // try to map the fb to the same IOVA on disp0
        iova_disp0 = dart_find_iova(dcp->dart_disp, iova_dcp, size);
        if (DART_IS_ERR(iova_disp0)) {
            printf("display: failed to find IOVA for fb of %06zx bytes (disp0)\n", size);
            return iova_disp0;
        }

        // try to find the same IOVA on DCP again
        if (iova_disp0 != iova_dcp) {
            iova_dcp = dart_find_iova(dcp->dart_dcp, iova_disp0, size);
            if (DART_IS_ERR(iova_dcp)) {
                printf("display: failed to find IOVA for fb of %06zx bytes (dcp)\n", size);
                return iova_dcp;
            }
        }

        // assume this results in the same IOVA, not sure if this is required but matches what iboot
        // does on other models.
        if (iova_disp0 != iova_dcp) {
            printf("display: IOVA mismatch for fb between dcp (%08lx) and disp0 (%08lx)\n",
                   (u64)iova_dcp, (u64)iova_disp0);
            return DART_PTR_ERR;
        }

        iova = iova_dcp;
    }

    int ret = dart_map(dcp->dart_disp, iova, (void *)paddr, size);
    if (ret < 0) {
        printf("display: failed to map fb to dart-disp0\n");
        return DART_PTR_ERR;
    }

    ret = dart_map(dcp->dart_dcp, iova, (void *)paddr, size);
    if (ret < 0) {
        printf("display: failed to map fb to dart-dcp\n");
        dart_unmap(dcp->dart_disp, iova, size);
        return DART_PTR_ERR;
    }

    return iova;
}

const display_config_t *display_get_config(void)
{
    const display_config_t *conf = NULL;

    if (adt_is_compatible(adt, 0, "J473AP"))
        conf = &display_config_m2;
    else if (adt_is_compatible(adt, 0, "J474sAP") || adt_is_compatible(adt, 0, "J475cAP"))
        conf = &display_config_m2_pro_max;
    else if (adt_is_compatible(adt, 0, "J180dAP") || adt_is_compatible(adt, 0, "J475dAP"))
        conf = &display_config_m2_ultra;
    else if (chip_id == T8152)
        conf = t8152_config;
    else
        conf = &display_config_m1;

    has_dcp = adt_path_offset(adt, conf->dcp) > 0;
    if (!has_dcp) {
        return NULL;
    }

    return conf;
}

/*
 * On T6030 the internal DCP is left exactly as iBoot started it (see kboot_display_handoff.c). The
 * modeset below is untested there, and its fallback config names a power domain T6030 does not
 * have, so it must never run (display= variable, non-retina heuristic, proxy).
 */
static bool display_t6030_hands_off(const char *what)
{
    if (chip_id != T6030)
        return false;
    printf("display: T6030: %s skipped, keeping the display iBoot set up\n", what);
    return true;
}

int display_start_dcp(void)
{
    if (iboot)
        return 0;

    if (dcp_left_running()) {
        printf("display: a DCP did not stop; not starting it again\n");
        return -1;
    }

    if (display_t6030_hands_off("DCP start"))
        return 0;

#ifdef NO_DISPLAY
    printf("display: NO_DISPLAY!\n");
    return 0;
#endif

    const display_config_t *disp_cfg = display_get_config();

    if (!has_dcp) {
        printf("display: device has no DCP. Display will not be initialised.\n");
        return -1;
    }

    display_is_dptx = !!disp_cfg->dptx_phy[0];

    dcp = dcp_init(disp_cfg);
    if (!dcp) {
        printf("display: failed to initialize DCP\n");
        return -1;
    }

    // determine frame buffer PA and size from "/vram"
    u64 pa, size;
    if (display_fb_override_size) {
        pa = cur_boot_args.video.base;
        size = fb_size = display_fb_override_size;
    } else if (display_get_vram(&pa, &size)) {
        // use a safe fb_size
        fb_size = cur_boot_args.video.stride * cur_boot_args.video.height *
                  ((cur_boot_args.video.depth + 7) / 8);
    } else {
        fb_size = size;
    }

    // Find the framebuffer DVA
    fb_dva = dart_search(dcp->dart_disp, (void *)cur_boot_args.video.base);
    // framebuffer is not mapped on the M1 Ultra Mac Studio
    if (DART_IS_ERR(fb_dva) || !fb_dva)
        fb_dva = display_map_fb(0, pa, size);
    if (DART_IS_ERR(fb_dva)) {
        printf("display: failed to find display DVA\n");
        fb_dva = 0;
        dcp_shutdown(dcp, false);
        return -1;
    }

    iboot = dcp_ib_init(dcp);
    if (!iboot) {
        printf("display: failed to initialize DCP iBoot interface\n");
        dcp_shutdown(dcp, false);
        return -1;
    }

    return 0;
}

struct display_options {
    bool retina;
};

int display_parse_mode(const char *config, dcp_timing_mode_t *mode, struct display_options *opts)
{
    memset(mode, 0, sizeof(*mode));
    display_mode_max = config && !strcmp(config, "max");

    if (!config || !strcmp(config, "auto") || display_mode_max)
        return 0;

    const char *s_w = config;
    const char *s_h = strchr(config, 'x');
    const char *s_fps = strchr(config, '@');

    if (s_w && s_h) {
        mode->width = atol(s_w);
        mode->height = atol(s_h + 1);
        mode->valid = mode->width && mode->height;
    }

    if (s_fps) {
        mode->fps = atol(s_fps + 1) << 16;

        const char *s_fps_frac = strchr(s_fps + 1, '.');
        if (s_fps_frac) {
            // Assumes two decimals...
            mode->fps += (atol(s_fps_frac + 1) << 16) / 100;
        }
    }

    const char *option = config;
    while (option && opts) {
        if (!strncmp(option + 1, "retina", 6))
            opts->retina = true;
        option = strchr(option + 1, ',');
    }

    printf("display: want mode: valid=%d %dx%d %d.%02d Hz\n", mode->valid, mode->width,
           mode->height, mode->fps >> 16, ((mode->fps & 0xffff) * 100 + 0x7fff) >> 16);

    return mode->valid;
}

static int display_swap(u64 iova, u32 stride, u32 width, u32 height)
{
    int ret;

    dcp_layer_t layer = {
        .planes = {{
            .addr = iova,
            .stride = stride,
            .addr_format = ADDR_PLANAR,
        }},
        .plane_cnt = 1,
        .width = width,
        .height = height,
        .surface_fmt = chip_id == T8152 ? FMT_BGRA : FMT_w30r,
        .colorspace = 2,
        .eotf = EOTF_GAMMA_SDR,
        .transform = XFRM_NONE,
    };

    if ((ret = dcp_ib_set_surface(iboot, &layer)) < 0) {
        printf("display: failed to set surface\n");
        return -1;
    }

    return 0;
}

int display_configure(const char *config)
{
    dcp_timing_mode_t want;
    struct display_options opts = {0};

#ifdef NO_DISPLAY
    printf("display: skip configuration (NO_DISPLAY)\n");
    return 0;
#endif

    if (display_t6030_hands_off("DCP modeset"))
        return 0;

    display_parse_mode(config, &want, &opts);

    u64 start_time = get_ticks();

    int ret = display_start_dcp();
    if (ret < 0)
        return ret;

    // connect dptx if necessary
    if (display_is_dptx) {
        ret = dcp_connect_dptx(dcp);
        if (ret < 0)
            return ret;
        /* The DCP only brings the port up once HPD is asserted. On the HDMI
         * path the sink's HPD reaches the AP gpio, so tell the firmware. */
        dcp_dptx_hpd(dcp->dptx_ep, 0, true);
    }

    if (!display_is_external) {
        // Sequoia bug workaround: Force power cycle
        if (display_needs_power_cycle) {
            if ((ret = dcp_ib_set_power(iboot, false)) < 0)
                printf("display: failed to set power off (continuing anyway)\n");
            mdelay(100);
        }
        // Sonoma bug workaround: Power on internal panel early
        if ((ret = dcp_ib_set_power(iboot, true)) < 0)
            printf("display: failed to set power on (continuing anyway)\n");
    }

    // Detect if display is connected
    int timing_cnt, color_cnt;
    int hpd = 0, retries = 0;

    /* After boot DCP does not immediately report a connected display. Retry getting display
     * information for 2 seconds.
     */
    while (retries++ < (DISPLAY_STATUS_RETRIES(display_is_dptx))) {
        dcp_work(dcp);
        hpd = dcp_ib_get_hpd(iboot, &timing_cnt, &color_cnt);
        /* Only the latest status query decides; the power requests above are
         * best effort and an earlier failed query may be followed by a good one. */
        ret = hpd < 0 ? hpd : 0;
        if (hpd > 0 && timing_cnt && color_cnt)
            break;
        if (retries < DISPLAY_STATUS_RETRIES(display_is_dptx))
            mdelay(DISPLAY_STATUS_DELAY);
    }
    printf("display: waited %d ms for display status\n", (retries - 1) * DISPLAY_STATUS_DELAY);
    if (ret < 0) {
        printf("display: failed to get display status\n");
        return 0;
    }

    printf("display: connected:%d timing_cnt:%d color_cnt:%d\n", hpd, timing_cnt, color_cnt);

    if (!hpd || !timing_cnt || !color_cnt)
        return 0;

    // Power on
    if ((ret = dcp_ib_set_power(iboot, true)) < 0) {
        printf("display: failed to set power\n");
        return ret;
    }

    // Sonoma bug workaround
    mdelay(100);

    // Find best modes
    dcp_timing_mode_t *tmodes, tbest;
    if ((ret = dcp_ib_get_timing_modes(iboot, &tmodes)) < 0) {
        printf("display: failed to get timing modes\n");
        return -1;
    }
    assert(ret == timing_cnt);
    display_choose_timing_mode(tmodes, timing_cnt, &tbest, &want);

    dcp_color_mode_t *cmodes, cbest;
    if ((ret = dcp_ib_get_color_modes(iboot, &cmodes)) < 0) {
        printf("display: failed to get color modes\n");
        return -1;
    }
    assert(ret == color_cnt);
    display_choose_color_mode(cmodes, color_cnt, &cbest);

    // Set mode
    if ((ret = dcp_ib_set_mode(iboot, &tbest, &cbest)) < 0) {
        printf("display: failed to set mode. trying again...\n");
        mdelay(500);
        if ((ret = dcp_ib_set_mode(iboot, &tbest, &cbest)) < 0) {
            printf("display: failed to set mode twice.\n");
            return ret;
        }
    }

    u64 fb_pa = cur_boot_args.video.base;
    u64 tmp_dva = 0;
    bool swapped = false;

    size_t size =
        ALIGN_UP(tbest.width * tbest.height * ((cbest.bpp + 7) / 8) + 24 * SZ_16K, SZ_16K);

    if (fb_size < size) {
        printf("display: current framebuffer is too small for new mode\n");

        /* rtkit uses 0x10000000 as DVA offset, FB starts in the first page */
        if ((s64)size > 7 * SZ_32M) {
            printf("display: not enough reserved L2 DVA space for fb size 0x%zx\n", size);
            return -1;
        }

        fb_pa = top_of_memory_alloc(size);
        memset((void *)fb_pa, 0, size);

        tmp_dva = iova_alloc(dcp->iovad_dcp, size);

        tmp_dva = display_map_fb(tmp_dva, fb_pa, size);
        if (DART_IS_ERR(tmp_dva)) {
            printf("display: failed to map new fb\n");
            return -1;
        }

        // Swap!
        u32 stride = tbest.width * 4;
        ret = display_swap(tmp_dva, stride, tbest.width, tbest.height);
        if (ret < 0)
            return ret;

        /* wait for swap durations + 1ms */
        u32 delay = (((1000 << 16) + tbest.fps - 1) / tbest.fps) + 1;
        mdelay(delay);
        dart_unmap(dcp->dart_disp, fb_dva, fb_size);
        dart_unmap(dcp->dart_dcp, fb_dva, fb_size);

        if (chip_id == T8152) {
            /* The DCP already scans out the new framebuffer at tmp_dva; a
             * second swap after moving it back to fb_dva times out here. */
            fb_dva = tmp_dva;
            swapped = true;
        } else {
            fb_dva = display_map_fb(fb_dva, fb_pa, size);
            if (DART_IS_ERR(fb_dva)) {
                printf("display: failed to map new fb\n");
                fb_dva = 0;
                return -1;
            }
        }

        fb_size = size;
        mmu_map_framebuffer(fb_pa, fb_size);

        /* update ADT with the physical address of the new framebuffer */
        u64 fb_reg[2] = {fb_pa, size};
        int node = adt_path_offset(adt, "vram");
        if (node >= 0) {
            // TODO: adt_set_reg(adt, node, "vram", fb_pa, size);?
            ret = adt_setprop(adt, node, "reg", &fb_reg, sizeof(fb_reg));
            if (ret < 0)
                printf("display: failed to update '/vram'\n");
        }
        node = adt_path_offset(adt, "/chosen/carveout-memory-map");
        if (node >= 0) {
            // TODO: adt_set_reg(adt, node, "vram", fb_pa, size);?
            ret = adt_setprop(adt, node, "region-id-14", &fb_reg, sizeof(fb_reg));
            if (ret < 0)
                printf("display: failed to update '/chosen/carveout-memory-map/region-id-14'\n");
        }
    }

    // Swap!
    u32 stride = tbest.width * 4;
    /* A reused framebuffer still holds the previous console at another
     * stride (the stage before left 640x1136 text there). */
    if (chip_id == T8152 && !tmp_dva)
        memset((void *)fb_pa, 0, (size_t)stride * tbest.height);
    if (!swapped) {
        ret = display_swap(fb_dva, stride, tbest.width, tbest.height);
        if (ret < 0)
            return ret;

        printf("display: swapped! (swap_id=%d)\n", ret);

        // Wait until the swap completes before powering down DCP
        // 50ms is too low, 100 works, 150 for good measure
        mdelay(150);
    }

    bool reinit = false;
    if (fb_pa != cur_boot_args.video.base || cur_boot_args.video.stride != stride ||
        cur_boot_args.video.width != tbest.width || cur_boot_args.video.height != tbest.height ||
        (cur_boot_args.video.depth & FB_DEPTH_MASK) != (chip_id == T8152 ? 32 : 30)) {
        cur_boot_args.video.base = fb_pa;
        cur_boot_args.video.stride = stride;
        cur_boot_args.video.width = tbest.width;
        cur_boot_args.video.height = tbest.height;
        cur_boot_args.video.depth =
            (chip_id == T8152 ? 32 : 30) | (opts.retina ? FB_DEPTH_FLAG_RETINA : 0);
        reinit = true;
    }

    if (!display_is_external && !(cur_boot_args.video.depth & FB_DEPTH_FLAG_RETINA)) {
        cur_boot_args.video.depth |= FB_DEPTH_FLAG_RETINA;
        reinit = true;
    }

    if (reinit)
        fb_reinit();

    /* Update for python / subsequent stages */
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));

    if (tmp_dva) {
        // unmap / free temporary dva
        dart_unmap(dcp->dart_disp, tmp_dva, size);
        dart_unmap(dcp->dart_dcp, tmp_dva, size);
        iova_free(dcp->iovad_dcp, tmp_dva, size);
    }

    u64 msecs = ticks_to_msecs(get_ticks() - start_time);
    printf("display: Modeset took %ld ms\n", msecs);

    return 1;
}

static int display_t8152_work(u32 usec)
{
    u64 deadline = timeout_calculate(usec);
    do {
        if (dcp_work(dcp) < 0)
            return -1;
    } while (get_ticks() < deadline);
    return 0;
}

static int display_init_t8152_hdmi(void)
{
    printf("display: trying HDMI (dptx-phy)\n");
    t8152_config = &display_config_j873_hdmi;

    int ret = display_configure(NULL);

    if (ret <= 0) {
        printf("display: HDMI initialization failed\n");
        if (dcp)
            display_shutdown(DCP_QUIESCED);
        dcp = NULL;
        iboot = NULL;
        t8152_config = &display_config_m1;
        return ret < 0 ? ret : 0;
    }

    return 1;
}

static void display_swap_state(void)
{
#define SWAP(a, b)                                                                                 \
    do {                                                                                           \
        __typeof__(a) t = a;                                                                       \
        a = b;                                                                                     \
        b = t;                                                                                     \
    } while (0)
    SWAP(dcp, display_other.dcp);
    SWAP(iboot, display_other.iboot);
    SWAP(fb_dva, display_other.fb_dva);
    SWAP(fb_size, display_other.fb_size);
    SWAP(t8152_config, display_other.cfg);
    SWAP(cur_boot_args.video, display_other.video);
#undef SWAP
}

/* Configure the second display: bring it up on first use with its own
 * framebuffer, or modeset it later (the loader calls this by symbol). */
int display_configure_second(const char *config)
{
    bool first = !display_other.dcp;

    if (first) {
        /* Its own framebuffer, mapped when the DCP starts, as /vram is for
         * the first display; sized so that any mode up to 3840x2160 fits.
         * Switching DCPEXT0 to a reallocated framebuffer later leaves it
         * scanning out nothing. */
        u64 size = ALIGN_UP(3840 * 2160 * 4 + 24 * SZ_16K, SZ_16K);
        u64 pa = top_of_memory_alloc(size);
        memset((void *)pa, 0, size);
        display_other.cfg = &display_config_j873_hdmi_ext;
        display_other.video = (struct boot_video){
            .base = pa, .stride = 3840 * 4, .width = 3840, .height = 2160, .depth = 32};
        display_fb_override_size = size;
    }
    display_swap_state();
    int ret = display_configure(config);
    display_fb_override_size = 0;
    if (ret > 0) {
        display_second_video = cur_boot_args.video;
    } else if (first && dcp) {
        display_shutdown(DCP_QUIESCED);
        dcp = NULL;
        iboot = NULL;
    }
    display_swap_state();

    /* display_configure moved the console and boot args to the second
     * framebuffer; put both back on the first display. */
    memcpy((void *)boot_args_addr, &cur_boot_args, sizeof(cur_boot_args));
    fb_reinit();
    return ret;
}

static int display_init_t8152(void)
{
    /* A real firmware framebuffer already has a working transport. Adopt it
     * unchanged, whether it came from HDMI, USB-C or another firmware route. */
    if (cur_boot_args.video.base && cur_boot_args.video.width && cur_boot_args.video.height &&
        !(cur_boot_args.video.width == 640 && cur_boot_args.video.height == 1136)) {
        printf("display: Using firmware framebuffer (%ldx%ld)\n", cur_boot_args.video.width,
               cur_boot_args.video.height);
        return 0;
    }

#ifdef NO_DISPLAY
    return 0;
#endif

    /* This read-only probe checks J873g's ADT, ACE3 connection, lane mode and
     * routing before any PHY writes. Other ports and USB4 docks stay untouched. */
    dptx_phy_t *phy = dptx_phy_init("/arm-io/atc-phy3", 0);
    if (!phy) {
        printf("display: No supported USB-C display connected\n");
        return display_init_t8152_hdmi();
    }

    if (display_start_dcp() < 0) {
        dptx_phy_shutdown(phy);
        return -1;
    }

    /* Keep the native route owned by DCP for modesets and later shutdown.
     * Do not use the older HDMI power/reset path on this PMGR2 platform. */
    dcp->phy = phy;
    dcp->dpav_ep = dcp_dpav_init(dcp);
    if (!dcp->dpav_ep)
        goto fail;
    dcp->dptx_ep = dcp_dptx_init(dcp, 2);
    if (!dcp->dptx_ep)
        goto fail;
    if (dcp_connect_dptx(dcp) || dcp_dptx_hpd(dcp->dptx_ep, 0, true))
        goto fail;

    /* Service firmware continuously while the qualified direct-DP route
     * trains. Host round trips and idle delays interrupt this exchange. */
    if (display_t8152_work(8000000) < 0)
        goto fail;
    int timing_cnt = 0, color_cnt = 0;
    if (dcp_ib_get_hpd(iboot, &timing_cnt, &color_cnt) != 1 || !timing_cnt || !color_cnt)
        goto fail;
    if (dcp_ib_set_power(iboot, true) < 0 || display_t8152_work(8000000) < 0)
        goto fail;

    /* Start with the mode qualified for this route. Subsequent modesets use
     * the common display path and retain the normal framebuffer console. */
    if (display_configure("1280x720@60") > 0) {
        printf("display: trying HDMI as second display (DCPEXT0)\n");
        if (display_configure_second("max") <= 0)
            printf("display: no second display\n");
        return 1;
    }

fail:
    printf("display: USB-C initialization failed\n");
    if (dcp->dptx_ep)
        dcp_dptx_disconnect(dcp->dptx_ep, 0);
    /* Stop a failed route, but leave successful scanout running when the
     * normal next-stage shutdown quiesces DCP. */
    dcp->phy = NULL;
    display_shutdown(DCP_QUIESCED);
    dptx_phy_shutdown(phy);
    dcp = NULL;
    return -1;
}

int display_init(void)
{
    if (chip_id == T8152) {
        display_is_external = true;
        return display_init_t8152();
    }

    const char *disp_path;
    if (adt_is_compatible(adt, 0, "J180dAP") || adt_is_compatible(adt, 0, "J475dAP"))
        disp_path = "/arm-io/dispext4";
    else
        disp_path = "/arm-io/disp0";

    bool has_notch = false;
    UNUSED(has_notch);

    int product = adt_path_offset(adt, "/product");
    if (product < 0) {
        printf("/product node not found!\n");
    } else {
        u32 val = 0;
        ADT_GETPROP(adt, product, "partially-occluded-display", &val);
        has_notch = !!val;
    }

    int node = adt_path_offset(adt, disp_path);
    if (node < 0) {
        printf("%s node not found!\n", disp_path);
        return -1;
    }

    display_is_external = adt_getprop(adt, node, "external", NULL);
    if (display_is_external)
        printf("display: Display is external\n");
    else
        printf("display: Display is internal\n");

    if (chip_id == T8140) {
        printf("display: preserving inherited T8140 display (%ldx%ld)\n", cur_boot_args.video.width,
               cur_boot_args.video.height);
        return 0;
    }

    if ((cur_boot_args.video.width == 640 && cur_boot_args.video.height == 1136) &&
        chip_id != S5L8960X) {
        printf("display: Dummy framebuffer found, initializing display\n");
        return display_configure(NULL);
    } else if (display_is_external && is_mac) {
        printf("display: External display found, reconfiguring\n");
        return display_configure(NULL);
    } else if ((!(cur_boot_args.video.depth & FB_DEPTH_FLAG_RETINA)) && is_mac) {
        printf("display: Internal display with non-retina flag, assuming Sonoma bug and "
               "reconfiguring\n");
        fb_clear_direct(); // Old m1n1 stage1 ends up with an ugly logo situation, clear it.
        return display_configure(NULL);
#ifndef CHAINLOADING
    } else if (!has_notch && firmware_sfw_in_range(V15_0B1, FW_MAX) &&
               os_firmware.version < V15_0B1) {
        printf("display: Internal display on t8103 or t8112 with Sequoia SFW, power cycling\n");
        display_needs_power_cycle = true;
        return display_configure(NULL);
#endif
    } else {
        printf("display: Display is already initialized (%ldx%ld)\n", cur_boot_args.video.width,
               cur_boot_args.video.height);
        return 0;
    }
}

int display_shutdown(dcp_shutdown_mode mode)
{
    /* Linux owns the live J700 controllers. Keep the inherited ASC session
     * and scanout intact at both the Stage 1 and Linux boundaries. */
    if (chip_id == T8140)
        return 0;

    /* We have no DCP, so just exit */
    if (!has_dcp)
        return 0;

    if (iboot) {
        /* An endpoint that does not stop stays registered, so dcp_shutdown()
         * retries it with the rest and reports the DCP as still running. */
        dcp_ib_shutdown(iboot);
        switch (mode) {
            case DCP_QUIESCED:
                printf("display: Quiescing DCP (unconditional)\n");
                dcp_shutdown(dcp, false);
                break;
            case DCP_SLEEP_IF_EXTERNAL:
                if (!display_is_external)
                    printf("display: Quiescing DCP (internal)\n");
                else
                    printf("display: Sleeping DCP (external)\n");
                dcp_shutdown(dcp, display_is_external);
                break;
            case DCP_SLEEP:
                printf("display: Sleeping DCP (unconditional)\n");
                dcp_shutdown(dcp, true);
                break;
        }
        iboot = NULL;
    }

    /* A DCP that did not stop at any point may still use its buffers. */
    return dcp_left_running() ? -1 : 0;
}
