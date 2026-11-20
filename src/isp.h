/* SPDX-License-Identifier: MIT */

#ifndef ISP_H
#define ISP_H

#include "types.h"

void isp_reset(void);
bool isp_fdt_needs_init(const void *fdt);
int isp_init(void);
int isp_get_heap(u64 *phys, u64 *iova, u64 *size);
u64 isp_iova_base(void);
bool isp_is_isp17a(void);

#endif
