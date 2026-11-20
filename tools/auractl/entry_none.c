// SPDX-License-Identifier: MIT
/* No guest entry: every call is refused, and only `auractl print` works (entry.h). */
#include "entry.h"

int auractl_enter(struct auractl_req *req)
{
    (void)req;
    return AURACTL_ENTRY_NONE;
}

const char *auractl_entry_name(void)
{
    return "none";
}
