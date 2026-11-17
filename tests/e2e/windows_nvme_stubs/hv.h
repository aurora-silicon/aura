#pragma once
#include "types.h"
#include "hv_ahci.h"
struct exc_info;
#define HV_MAX_RW_WORDS 8
typedef bool(hv_hook_t)(struct exc_info *ctx, u64 addr, u64 *val, bool write, int width);
int hv_map_hook(u64 from, hv_hook_t *hook, u64 size);
typedef bool(hv_pt_leaf_fn)(void *ctx, u64 ipa, u64 size, u64 pte, int level);
void hv_pt_for_each_leaf(hv_pt_leaf_fn *fn, void *ctx);
void hv_t8152_gic_spi_level(u32 intid, bool level);
void hv_wdt_pet(void);
