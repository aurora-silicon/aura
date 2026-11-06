/* SPDX-License-Identifier: MIT AND GPL-2.0-only */

#ifndef DART_H
#define DART_H

#include "types.h"

#define DART_PTR_ERR     BIT(63)
#define DART_IS_ERR(val) FIELD_GET(DART_PTR_ERR, val)

typedef struct dart_dev dart_dev_t;

enum dart_type_t {
    DART_T8020,
    DART_T8110,
    DART_T6000,
    DART_GEN3,
};

dart_dev_t *dart_init(uintptr_t base, u8 device, bool keep_pts, enum dart_type_t type);
dart_dev_t *dart_init_adt(const char *path, int instance, int device, bool keep_pts);
dart_dev_t *dart_gen3_init_adt_cold(const char *path, int instance, int device);
int dart_set_bypass_fdt(void *dt, u32 phandle, u8 device);
dart_dev_t *dart_init_adt_display(const char *path, int instance, int device);
bool dart_adt_locked(const char *path, int instance);
void dart_lock_adt(const char *path, int instance);
dart_dev_t *dart_init_fdt(void *dt, u32 phandle, int device, bool keep_pts);
dart_dev_t *dart_init_fdt_locked(void *dt, u32 phandle, int device);
int dart_collect_locked_tables(dart_dev_t *dart, u64 dram_base, u64 dram_size, u64 *pages,
                               size_t capacity, size_t *count);
int dart_setup_pt_region(dart_dev_t *dart, const char *path, int device, u64 vm_base);
int dart_map(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len);
int dart_map_flags(dart_dev_t *dart, uintptr_t iova, void *bfr, size_t len, u32 flags);
void dart_unmap(dart_dev_t *dart, uintptr_t iova, size_t len);
void dart_free_l2(dart_dev_t *dart, uintptr_t iova);
void *dart_translate(dart_dev_t *dart, uintptr_t iova);
void *dart_translate_silent(dart_dev_t *dart, uintptr_t iova);
int dart_walk_locked_t8110(uintptr_t base, u64 size, u8 sid, u64 dram_base, u64 dram_size,
                           int (*visit)(u64 iova, u64 paddr, void *opaque), void *opaque);
bool dart_is_locked(dart_dev_t *dart);
u64 dart_search(dart_dev_t *dart, void *paddr);
u64 dart_find_iova(dart_dev_t *dart, s64 start, size_t len);
u64 dart_search_range(dart_dev_t *dart, u64 paddr, size_t len);
void dart_shutdown(dart_dev_t *dart);
int dart_reserve_legacy_tables(void *dt, u64 dram_base, u64 dram_size);
u64 dart_vm_base(dart_dev_t *dart);

int dart_collect_native_t8110(uintptr_t base, u8 sid, u64 dram_base, u64 dram_size, u64 *pages,
                              size_t capacity, size_t *count, char *detail, size_t detail_size);

#endif
