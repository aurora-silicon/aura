/* SPDX-License-Identifier: MIT */
#ifndef T8122_25G83_H
#define T8122_25G83_H

#include <stdbool.h>

#include "libfdt/libfdt.h"

/* J615 display and GPU handoffs require their own explicit switch. */
#define T8122_25G83_J615_OPT_IN "aurora,j615-25g83-experimental"

/* A J613, or a J615 with the opt-in. `dt` has /chosen set already. */
static inline bool t8122_25g83_board(const void *dt)
{
    bool j613 = !fdt_node_check_compatible(dt, 0, "apple,j613");
    bool j615 = !fdt_node_check_compatible(dt, 0, "apple,j615");
    if (j613 == j615)
        return false;
    if (j613)
        return true;
    int chosen = fdt_path_offset(dt, "/chosen"), len = 0;
    const char *optin = chosen < 0 ? NULL : fdt_getprop(dt, chosen, T8122_25G83_J615_OPT_IN, &len);
    return optin && len == 2 && optin[0] == '1' && !optin[1];
}

#endif
