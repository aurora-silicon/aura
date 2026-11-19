/* SPDX-License-Identifier: MIT */
#ifndef PAYLOAD_PROFILE_H
#define PAYLOAD_PROFILE_H

#include "libfdt/libfdt.h"

/* Legacy is a framebuffer fallback on 26.x. An exact experimental DT wins
 * regardless of archive order; other firmware never selects that profile.
 * The J615 25G83 tree (experimental) ranks like the J613's: selecting it
 * hands nothing over by itself, the display and GPU handoffs also need
 * chosen.aurora,j615-25g83-experimental=1 (t8122_25g83.h), which aura.conf
 * supplies only after the device trees have been ranked.
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
#endif
