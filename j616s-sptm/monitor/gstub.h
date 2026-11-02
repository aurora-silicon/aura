// SPDX-License-Identifier: GPL-2.0-only
#pragma once
/* first bytes of the guest stub's params page; the monitor's log text follows */
struct gstub_params {
	u64 fb, stride, width, height, entry, log_len, host_fb;
};
