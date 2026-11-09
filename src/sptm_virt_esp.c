/* SPDX-License-Identifier: MIT */
/*
 * Boot macOS under SPTM-virt from the ESP, with no host (SPTM_VIRT_ESP, the macos= config line).
 *
 * This is what proxyclient/tools/run_guest_sptm.py does over the USB debug link for the M5 trace
 * runs (launch.sh, cp.env), from files tools/esp-macos/make_bundle.py wrote: read the
 * live values, lay the images out (sptm_virt_esp_plan.c), copy them to RAM, apply the patches and
 * enter SPTM. Every file's size and SHA-256 is checked against the manifest, and the manifest's
 * against the config line, before anything is written to the window SPTM will own. Any failure
 * returns with no guest started: the caller goes to the usual no-payload path, and a trial boot's
 * watchdog then brings the Mac back on its configured Stage 2.
 */

#ifdef ESP_BOOT_HOST
#include "esp_boot_host.h" /* tests/esp_boot_host: this file on a fake machine */
#else
#include "../build/build_cfg.h"

#include "adt.h"
#include "cpufreq.h"
#include "fb.h"
#include "iodev.h"
#include "malloc.h"
#include "memory.h"
#include "nvme.h"
#include "sptm_virt_esp_plan.h"
#include "string.h"
#include "uartproxy.h"
#include "utils.h"
#include "wdt.h"
#include "xnuboot.h"

#include "tinf/tinf.h"
#endif

#ifdef SPTM_VIRT_ESP

int rust_load_image(const char *spec, void **image, size_t *size, bool *proxy_request, char *trial,
                    size_t trial_capacity);
void rust_free_image(void *image, size_t size);
int rust_read_into(const char *spec, void *dst, size_t cap, size_t *size);

#define PAGE     0x4000UL
#define MIN_RAM  (2UL << 30) /* run_guest_sptm.py: "not enough managed RAM" */
#define ESP_SPEC 192

static struct {
    void *data[ESP_NIMG];
    u64 len[ESP_NIMG];
    u64 stage; /* the files sit below this, high in the managed window: aura's heap can't hold
                  them (the kernelcache alone is close to its size) */
} ef;

/* A failure message goes to the panel too: this is where a user with no USB host ends up. */
#define ESP_FAIL(...)                                                                              \
    do {                                                                                           \
        printf("macOS ESP: " __VA_ARGS__);                                                         \
        goto fail;                                                                                 \
    } while (0)

AURA_XTEXT static void esp_free_files(void)
{
    for (int i = 0; i < ESP_NIMG; i++)
        ef.data[i] = NULL;
}

AURA_XTEXT static bool esp_hex_eq(const u8 sha[32], const char *hex)
{
    static const char d[] = "0123456789abcdef";
    for (int i = 0; i < 32; i++)
        if (hex[2 * i] != d[sha[i] >> 4] || hex[2 * i + 1] != d[sha[i] & 15])
            return false;
    return true;
}

/* Read one file from the bundle directory, or refuse it: into dst (cap bytes) if given, else
 * into aura's heap. */
AURA_XTEXT static int esp_read(const char *uuid, const char *dir, const struct esp_file *f,
                               void **out, size_t *len, void *dst, u64 cap)
{
    char spec[ESP_SPEC];
    u8 sha[32];
    if (snprintf(spec, sizeof(spec), "%s;%s/%s", uuid, dir, f->name) >= (int)sizeof(spec))
        return -1;
    if (dst ? rust_read_into(spec, dst, cap, len)
            : rust_load_image(spec, out, len, NULL, NULL, 0)) {
        printf("macOS ESP: cannot read %s\n", spec);
        return -1;
    }
    if (dst)
        *out = dst;
    if (!dst)
        return 0;
    if (*len != f->size) {
        printf("macOS ESP: %s is 0x%lx bytes, the manifest says 0x%lx\n", f->name, (u64)*len,
               f->size);
        return -1;
    }
    sha256(*out, *len, sha);
    if (memcmp(sha, f->sha, 32)) {
        printf("macOS ESP: %s fails its SHA-256\n", f->name);
        return -1;
    }
    return 0;
}

/* The next staging slot of size bytes, or 0 below floor. */
AURA_XTEXT static u64 esp_stage(u64 size, u64 floor)
{
    u64 at = ALIGN_DOWN(ef.stage - size, PAGE);
    if (size > ef.stage || at < floor)
        return 0;
    ef.stage = at;
    return at;
}

AURA_XTEXT static int esp_load_file(const char *uuid, const char *dir, const struct esp_file *f,
                                    int id, u64 floor)
{
    void *d = NULL;
    size_t n = 0;
    u64 at = esp_stage(f->size, floor);
    if (!at || esp_read(uuid, dir, f, &d, &n, (void *)at, f->size)) {
        if (!at)
            printf("macOS ESP: no room to stage %s\n", f->name);
        return -1;
    }
    if (!f->gzip) {
        ef.data[id] = d;
        ef.len[id] = n;
        return 0;
    }
    u8 sha[32];
    unsigned int out = f->raw_size, src_len = n;
    u64 raw = esp_stage(f->raw_size, floor);
    if (!raw) {
        printf("macOS ESP: no room to unpack %s\n", f->name);
        return -1;
    }
    int r = tinf_gzip_uncompress((void *)raw, &out, d, &src_len);
    if (r != TINF_OK || out != f->raw_size) {
        printf("macOS ESP: %s does not unpack (%d, 0x%x bytes)\n", f->name, r, out);
        return -1;
    }
    sha256((void *)raw, out, sha);
    if (memcmp(sha, f->raw_sha, 32)) {
        printf("macOS ESP: %s unpacked fails its SHA-256\n", f->name);
        return -1;
    }
    ef.data[id] = (void *)raw;
    ef.len[id] = out;
    return 0;
}

/* aura's own footprint against a DCP __OS_LOG buffer (run_guest_sptm.py refuses it too). */
AURA_XTEXT static bool esp_oslog_overlap(void)
{
    static const char *const owner[] = {"dcp", "dcpext", "dcpext0", "dcpext1"};
    bool bad = false;
    for (unsigned o = 0; o < ARRAY_SIZE(owner); o++) {
        char path[64];
        snprintf(path, sizeof(path), "/arm-io/%s/iop-%s-nub", owner[o], owner[o]);
        int node = adt_path_offset(adt, path);
        u32 len = 0;
        const u8 *v = node >= 0 ? adt_getprop(adt, node, "segment-ranges", &len) : NULL;
        for (u32 i = 0; v && i + 32 <= len; i += 32) {
            u64 ph;
            u32 sz, fl;
            memcpy(&ph, v + i, 8);
            memcpy(&sz, v + i + 24, 4);
            memcpy(&fl, v + i + 28, 4);
            if (fl == 0xa && ph < (u64)_end && ph + sz > (u64)_base) {
                printf("macOS ESP: %s __OS_LOG 0x%lx overlaps aura [0x%lx, 0x%lx)\n", owner[o], ph,
                       (u64)_base, (u64)_end);
                bad = true;
            }
        }
    }
    return bad;
}

AURA_XTEXT static bool esp_is_tc_container(const u8 *b, u64 len)
{
    u32 num, first;
    if (len < 8)
        return false;
    memcpy(&num, b, 4);
    memcpy(&first, b + 4, 4);
    return num >= 1 && num <= 15 && len >= 4 + 4UL * num && first == 4 + 4 * num;
}

/* Copy one load into RAM. The checks here repeat nothing the plan knows: they bound the copy. */
AURA_XTEXT static int esp_do_load(const struct esp_load *l)
{
    u8 *dst = (u8 *)l->pa;
    u64 at = 0;
    if (l->tc_header) {
        const u32 hdr[2] = {1, 8};
        memcpy(dst, hdr, 8);
        at = 8;
    }
    if (l->img == ESP_NIMG) {
        memcpy(dst + at, (const void *)l->fileoff, l->size);
    } else {
        if (!ef.data[l->img] || l->fileoff + l->size > ef.len[l->img]) {
            printf("macOS ESP: %s reads past its file\n", l->what);
            return -1;
        }
        memcpy(dst + at, (const u8 *)ef.data[l->img] + l->fileoff, l->size);
    }
    at += l->size;
    if (at > l->total) {
        printf("macOS ESP: %s is larger than its slot\n", l->what);
        return -1;
    }
    memset(dst + at, 0, l->total - at);
    return 0;
}

/* The word's address in RAM: the load of img that holds file offset off. */
AURA_XTEXT static u64 esp_patch_pa(const struct esp_plan *pl, u8 img, u64 off)
{
    for (u32 i = 0; i < pl->nload; i++) {
        const struct esp_load *l = &pl->load[i];
        if (l->img == img && off >= l->fileoff && off + 4 <= l->fileoff + l->size)
            return l->pa + (off - l->fileoff);
    }
    return 0;
}

/* The runner's configure() switches, as aura's own variables. */
extern u32 sptm_virt_s2r_dcp_pkt, sptm_virt_s2r_cp_skip_ans, sptm_virt_s2r_cp_ansx,
    sptm_virt_s2r_go_count, sptm_virt_s2r_wake_s, sptm_virt_s2r_quiesce, sptm_virt_ctrr_stash_off,
    sptm_virt_link_phys[2];
extern u64 sptm_virt_diag_pa;

/*
 * ANS's hand-off to XNU, after the last read from the ESP and before SPTM is entered. Its state is
 * the ANS power-off work's (aura#107 on T6050); this is the one place that decides it.
 */
AURA_XTEXT int sptm_virt_esp_ans_handoff(void)
{
    return nvme_shutdown() ? 0 : -1;
}

AURA_XTEXT int sptm_virt_esp_boot(const char *spec)
{
    char uuid[40], dir[ESP_PATH], err[128] = {0};
    const char *hash;
    struct esp_manifest *m = NULL;
    struct esp_plan *pl = NULL;
    struct esp_plan_in *in = NULL;
    u8 *adt_new = NULL;
    void *mtext = NULL;
    size_t mlen = 0;

    printf("macOS ESP: booting macOS from %s\n", spec);

    /* <PARTUUID>;<directory>;<manifest SHA-256> */
    const char *s1 = strchr(spec, ';'), *s2 = s1 ? strchr(s1 + 1, ';') : NULL;
    if (!s2 || (u64)(s1 - spec) >= sizeof(uuid) || (u64)(s2 - s1 - 1) >= sizeof(dir) ||
        strlen(s2 + 1) != 64)
        ESP_FAIL("bad macos= value\n");
    memcpy(uuid, spec, s1 - spec);
    uuid[s1 - spec] = 0;
    memcpy(dir, s1 + 1, s2 - s1 - 1);
    dir[s2 - s1 - 1] = 0;
    hash = s2 + 1;

    if (!sptm_virt_requested())
        ESP_FAIL("SPTM-virt can't run here\n");
    struct sptm_virt_placement place;
    if (sptm_virt_place(&place))
        ESP_FAIL("no valid SPTM layout\n");
    /* SPTM leaves a partial last 2 MiB block untyped and panics: the window ends on one. */
    place.managed_end &= ~((2UL << 20) - 1);
    if (esp_oslog_overlap())
        ESP_FAIL("aura overlaps a DCP log buffer\n");

    m = calloc(1, sizeof(*m));
    pl = calloc(1, sizeof(*pl));
    in = calloc(1, sizeof(*in));
    if (!m || !pl || !in)
        ESP_FAIL("no memory\n");

    if (!nvme_init())
        ESP_FAIL("NVMe init failed\n");
    cpufreq_payload_boost(true);

    /* The manifest, checked against this image's own config line. */
    struct esp_file mf;
    memset(&mf, 0, sizeof(mf));
    strcpy(mf.name, "manifest.txt");
    if (esp_read(uuid, dir, &mf, &mtext, &mlen, NULL, 0))
        goto fail;
    u8 msha[32];
    sha256(mtext, mlen, msha);
    if (!esp_hex_eq(msha, hash))
        ESP_FAIL("manifest.txt does not match this Stage 2's macos= line\n");
    if (esp_manifest_parse(m, mtext, mlen, err, sizeof(err)))
        ESP_FAIL("%s\n", err);
    rust_free_image(mtext, mlen);
    mtext = NULL;
    printf("macOS ESP: manifest for %s, %u patches, trace 0x%lx\n", m->build, m->npatch, m->trace);

    /* Everything is read and checked before the first write to RAM SPTM will own. */
    /* Staged downward from the window's top; the blob, laid out from its bottom, must stay below
     * (checked once the plan is known). A slot never reaches below the window's first 4 GiB. */
    ef.stage = place.managed_end;
    for (int i = 0; i < ESP_NIMG; i++)
        if (m->file[i].present &&
            esp_load_file(uuid, dir, &m->file[i], i, place.managed_base + (4UL << 30)))
            goto fail;

    /* The live values run_guest_sptm.py reads, in its native layout. */
    in->m = m;
    in->adt = (const u8 *)adt;
    in->adt_len = cur_boot_args.devtree_size;
    in->phys_base = place.managed_base;
    in->mem_size = place.managed_end - place.managed_base;
    in->mem_size_actual = mem_size_actual;
    in->dram_base = in->phys_base & ~((4UL << 30) - 1);
    if (in->mem_size < MIN_RAM)
        ESP_FAIL("not enough managed RAM (0x%lx)\n", in->mem_size);
    for (int i = 0; i < ESP_NIMG; i++)
        in->file_len[i] = ef.len[i];
    in->tc_container = esp_is_tc_container(ef.data[ESP_TC], ef.len[ESP_TC]);
    const struct boot_video *bv = &cur_boot_args.video;
    const u64 video[6] = {bv->base, bv->display, bv->stride, bv->width, bv->height, bv->depth};
    memcpy(in->video, video, sizeof(video));

    static const char *const copies[] = {"RTBuddySeg", "SEPFW", "SEPPatches", "uStuff", "preoslog"};
    int mmn = adt_path_offset(adt, "/chosen/memory-map");
    for (unsigned i = 0; mmn >= 0 && i < ARRAY_SIZE(copies); i++) {
        u32 len = 0;
        const void *v = adt_getprop(adt, mmn, copies[i], &len);
        u64 r[2];
        if (!v || len != 16)
            continue;
        memcpy(r, v, 16);
        if (r[1] && r[0] != ~0UL && !(r[0] & 0x3fff)) {
            strcpy(in->copies[in->ncopies].name, copies[i]);
            in->copies[in->ncopies].pa = r[0];
            in->copies[in->ncopies++].size = r[1];
        }
    }

    u64 adt_cap = ALIGN_UP(in->adt_len + 0x2000UL, PAGE);
    adt_new = malloc(adt_cap);
    if (!adt_new)
        ESP_FAIL("no memory for the device tree\n");
    if (esp_plan(in, pl, adt_new, adt_cap, err, sizeof(err)))
        ESP_FAIL("%s\n", err);
    printf("macOS ESP: blob [0x%lx, 0x%lx) in [0x%lx, 0x%lx), SPTM entry 0x%lx\n", pl->txm_pa,
           pl->blob_end, in->phys_base, in->phys_base + in->mem_size, pl->sptm_entry_pa);

    /* Nothing aura or iBoot's tree lives on may be written over, nor the staged files. */
    u64 lo = pl->txm_pa, hi = pl->blob_end;
    if (hi > ef.stage)
        ESP_FAIL("the blob reaches the staged files at 0x%lx\n", ef.stage);
    if (((u64)adt < hi && (u64)adt + in->adt_len > lo) || ((u64)_base < hi && (u64)_end > lo))
        ESP_FAIL("the blob overlaps aura or the device tree\n");
    /* iBoot's data regions first, as the runner does: refuse any source a load would overwrite. */
    for (u32 i = 0; i < pl->nload; i++) {
        const struct esp_load *c = &pl->load[i];
        if (c->img != ESP_NIMG)
            continue;
        for (u32 k = 0; k < pl->nload; k++)
            if (k != i && pl->load[k].pa < c->fileoff + c->size &&
                c->fileoff < pl->load[k].pa + pl->load[k].total)
                ESP_FAIL("%s overwrites the source of %s\n", pl->load[k].what, c->what);
    }
    for (u32 i = 0; i < pl->nload; i++)
        if (pl->load[i].img == ESP_NIMG && esp_do_load(&pl->load[i]))
            goto fail;
    for (u32 i = 0; i < pl->nload; i++)
        if (pl->load[i].img != ESP_NIMG && esp_do_load(&pl->load[i]))
            goto fail;
    memset((void *)pl->adt_pa, 0, pl->adt_space);
    memcpy((void *)pl->adt_pa, adt_new, pl->adt_len);
    memcpy((void *)pl->boot_args_pa, pl->boot_args, sizeof(pl->boot_args));

    /* SPTM's patches and its MTE sequence, each word checked first (the runner's patch_sptm). */
    for (u32 i = 0; i < m->npatch; i++) {
        const struct esp_patch *p = &m->patch[i];
        u64 pa = esp_patch_pa(pl, p->img, p->off);
        if (!pa || read32(pa) != p->old)
            ESP_FAIL("patch %s at file 0x%lx: found 0x%x, expected 0x%x\n", p->label, p->off,
                     pa ? read32(pa) : 0, p->old);
        write32(pa, p->new);
    }
    if (m->ctrr_stash)
        sptm_virt_ctrr_stash_off = m->ctrr_stash;
    for (int i = 0; i < 2; i++)
        sptm_virt_link_phys[i] = m->link_phys[i];

    for (u32 i = 0; i < pl->nload; i++)
        sptm_virt_flush(pl->load[i].pa, pl->load[i].total);
    sptm_virt_flush(pl->adt_pa, pl->adt_space);
    sptm_virt_flush(pl->boot_args_pa, sizeof(pl->boot_args));

    esp_free_files();
    cpufreq_payload_boost(false);

    /* configure(): the flags, then the switches; the stop's proxy is the debug console. */
    uartproxy_iodev = IODEV_DOCKCHANNEL_UART;
    sptm_virt_diag_pa = 0;
    sptm_virt_config(m->trace);
    u32 *const sw[ESP_S2R_N] = {&sptm_virt_s2r_dcp_pkt, &sptm_virt_s2r_cp_skip_ans,
                                &sptm_virt_s2r_cp_ansx, &sptm_virt_s2r_go_count,
                                &sptm_virt_s2r_wake_s,  &sptm_virt_s2r_quiesce};
    for (int i = 0; i < ESP_S2R_N; i++)
        if (m->s2r_set[i])
            *sw[i] = m->s2r[i];

    if (sptm_virt_esp_ans_handoff())
        ESP_FAIL("ANS hand-off failed\n");
    if (sptm_virt_load(pl->sptm_entry_pa, pl->boot_args_pa))
        ESP_FAIL("sptm_virt_load refused the plan\n");

    /* A trial boot's watchdog tops out at 178 s, less than macOS needs. From here a hang is held
     * by the power button; the marker is spent, so the next boot is the configured one. */
    wdt_disable();
    printf("macOS ESP: entering SPTM\n");
    iodev_console_flush();
    sptm_virt_enter();

fail:
    fb_set_active(true);
    printf("macOS ESP: not booting macOS.\n");
    cpufreq_payload_boost(false);
    esp_free_files();
    if (mtext)
        rust_free_image(mtext, mlen);
    free(adt_new);
    free(in);
    free(pl);
    free(m);
    return -1;
}

#endif
