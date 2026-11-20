/* SPDX-License-Identifier: MIT */

#ifndef S2R_DCP_PKT_H
#define S2R_DCP_PKT_H

#include "types.h"

extern u32 sptm_virt_s2r_dcp_pkt;

void s2r_dcp_add(const char *name, u64 mbox);
void s2r_dcp_watch(u64 addr, const u64 *val, int width);
/* sptm_virt_s2r.c: one ring record with a caller's tag in its pc field. */
void sptm_virt_s2r_rec_raw(u64 pa, u64 val, u64 tag, bool write);

#endif
