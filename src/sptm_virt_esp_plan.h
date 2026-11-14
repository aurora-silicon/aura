/* SPDX-License-Identifier: MIT */
/*
 * The iBoot->SPTM handoff for an ESP boot of macOS (SPTM_VIRT_ESP): the manifest the bundle tool
 * writes (tools/esp-macos/make_bundle.py) and a C port of proxyclient/aura/sptm_virt_plan.py's
 * plan_boot, for the configuration boot_desktop.sh uses. Pure: no hardware, no aura state, so
 * tests/esp_plan_host.c runs it on the host against the Python planner.
 */

#ifndef SPTM_VIRT_ESP_PLAN_H
#define SPTM_VIRT_ESP_PLAN_H

#ifdef ESP_PLAN_HOST
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#ifndef AURA_XTEXT
#define AURA_XTEXT
#endif
#else
#include "../build/build_cfg.h"

#include "sptm_virt.h"
#include "types.h"
#endif

enum esp_img { ESP_SPTM, ESP_TXM, ESP_XNU, ESP_TC, ESP_CL4, ESP_EXIC, ESP_EXTC, ESP_NIMG };
/* run_guest_sptm.py configure()'s switches, as `s2r <name> <value>` lines */
enum esp_s2r {
    ESP_S2R_DCP_PKT,
    ESP_S2R_CP_SKIP_ANS,
    ESP_S2R_CP_ANSX,
    ESP_S2R_GO_COUNT,
    ESP_S2R_WAKE_S,
    ESP_S2R_QUIESCE,
    ESP_S2R_N
};

#define ESP_MAX_SEGS    24
#define ESP_MAX_PATCHES 40
#define ESP_MAX_HIDE    32
#define ESP_MAX_DROP    8
#define ESP_MAX_LOADS   48
#define ESP_MAX_REGIONS 64
#define ESP_PATH        64

struct esp_seg {
    char name[17];
    u64 vmaddr, vmsize, fileoff, filesize;
};

struct esp_image {
    u64 virtlo, virthi, entry;
    struct esp_seg seg[ESP_MAX_SEGS];
    u32 nseg;
};

struct esp_file {
    char name[ESP_PATH];
    u64 size, raw_size;
    u8 sha[32], raw_sha[32];
    bool present, gzip;
};

struct esp_patch {
    u8 img;
    u64 off;
    u32 old, new;
    char label[24];
};

struct esp_manifest {
    char build[32];
    struct esp_file file[ESP_NIMG];
    struct esp_image img[3]; /* sptm, txm, xnu */
    bool cl4;
    u64 cl4_entry, cl4_virt;
    u64 cl4r[4][2]; /* CL4-rx, CL4-ro, CL4-rw, CL4-le: bundle offset, size */
    struct esp_patch patch[ESP_MAX_PATCHES];
    u32 npatch;
    char hide[ESP_MAX_HIDE][ESP_PATH];
    u32 nhide;
    char drop[ESP_MAX_DROP][2][ESP_PATH];
    u32 ndrop;
    char serial[ESP_PATH];
    u64 trace;          /* sptm_virt_config's flags */
    u32 s2r[ESP_S2R_N]; /* the runner's symbol switches, by enum esp_s2r */
    bool s2r_set[ESP_S2R_N];
    u64 ctrr_stash;   /* sptm_virt_ctrr_stash_off */
    u32 link_phys[2]; /* sptm_virt_link_phys: the debug link's USB PHYs */
    char love[96];    /* /chosen/manifest-properties love, "" to leave it */
    bool video;       /* hand XNU the native boot video */
    char bootargs[1024];
};

/* The text manifest, edited in place (NULs). 0, or -1 with err set. */
int esp_manifest_parse(struct esp_manifest *m, char *text, u64 len, char *err, u64 errlen);

/* One copy into RAM: img's bytes [fileoff, fileoff + size), zero-filled to total. img
 * ESP_NIMG is a copy from physical address fileoff; with tc_header the trust cache gets the
 * 8-byte single-module container header first. */
struct esp_load {
    char what[32];
    u64 pa;
    u8 img;
    bool tc_header;
    u64 fileoff, size, total;
};

struct esp_region {
    char name[32]; /* an ADT property name's size */
    u64 pa, size;
};

struct esp_plan_in {
    const struct esp_manifest *m;
    const u8 *adt;
    u32 adt_len;
    u64 dram_base, phys_base, mem_size, mem_size_actual;
    u64 file_len[ESP_NIMG]; /* loaded (decompressed) lengths */
    bool tc_container;      /* the trust cache file already is a container */
    struct {
        char name[16];
        u64 pa, size;
    } copies[5]; /* iBoot's data regions, in iBoot's order */
    u32 ncopies;
    u64 reserved[4][2]; /* (pa, size) declared as carveouts */
    u32 nreserved;
    u64 video[6]; /* boot_args video: base, display, stride, width, height, depth */
};

struct esp_plan {
    struct esp_load load[ESP_MAX_LOADS];
    u32 nload;
    struct esp_region region[ESP_MAX_REGIONS];
    u32 nregion;
    u64 adt_pa, adt_len, adt_space; /* new tree, padded; its region's size */
    u64 sptm_pa, sptm_entry_pa, txm_pa, boot_args_pa, blob_end, virt_base;
    u8 boot_args[0x488];
};

/* Lay out the blob and write the edited device tree to adt_out (adt_cap bytes). */
int esp_plan(const struct esp_plan_in *in, struct esp_plan *out, u8 *adt_out, u64 adt_cap,
             char *err, u64 errlen);

/* The SHA-256 of a buffer (src/sha256.c). */
void sha256(const void *data, u64 len, u8 out[32]);

#endif
