/* SPDX-License-Identifier: MIT */
#ifndef PAYLOAD_PROFILE_H
#define PAYLOAD_PROFILE_H

#include "libfdt/libfdt.h"

/* Legacy trees are the default fallback (rank 1). Firmware-specific trees win
 * regardless of archive order when their OS firmware matches (rank 2) and are
 * never selected on other firmware (rank 0):
 * - J613/J615 25G83 (os_26_6_2): apple,j613-25g83-profile + 26.6.2 compat.
 *   Selecting J615 hands nothing over by itself; the display and GPU handoffs
 *   also need chosen.asahi,j615-25g83-experimental=1 (t8122_25g83.h), which
 *   aura.conf supplies only after the device trees have been ranked.
 * - T8112 macOS 27+ (os_27): apple,t8112-mtp with apple,rtkit-keep-running
 *   (e.g. t8112-j413-fw27.dtb vs t8112-j413.dtb).
 */
static inline int payload_profile_rank(const void *dt, bool os_26_6_2)
{
    int dcp = fdt_path_offset(dt, "dcp"), len = 0;
    const fdt32_t *profile =
        dcp < 0 ? NULL : fdt_getprop(dt, dcp, "apple,j613-25g83-profile", &len);
    if (!profile)
        return 1;
    bool j613 = !fdt_node_check_compatible(dt, 0, "apple,j613");
    bool j615 = !fdt_node_check_compatible(dt, 0, "apple,j615");
    if (len != 4 || fdt32_ld(profile) != 1 || !os_26_6_2 || j613 == j615)
        return 0;
    const fdt32_t *compat = fdt_getprop(dt, dcp, "apple,firmware-compat", &len);
    return compat && len == 12 && fdt32_ld(compat) == 26 && fdt32_ld(compat + 1) == 6 &&
                   fdt32_ld(compat + 2) == 2
               ? 2
               : 0;
}

static inline int payload_profile_rank_fw(const void *dt, bool os_26_6_2, bool os_27)
{
    int mtp = fdt_node_offset_by_compatible(dt, -1, "apple,t8112-mtp");
    if (mtp >= 0 && fdt_getprop(dt, mtp, "apple,rtkit-keep-running", NULL))
        return os_27 ? 2 : 0;

    return payload_profile_rank(dt, os_26_6_2);
}
#endif
