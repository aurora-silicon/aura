// SPDX-License-Identifier: MIT
/*
 * The XNU guest entry: reach aura's control HVC with no kext, through a patch the host places in
 * the guest kernelcache's acct() syscall (proxyclient/aura/auractl_xnu.py, docs/auractl.md). Build
 * with `make ENTRY=xnu`.
 *
 * The host runner (proxyclient/tools/run_guest_sptm.py, AURA_XNU_ENTRY=1) patches acct(2) in the
 * BootKC it chainloads so that, past acct's own root check, the kernel loads uap->path into x1,
 * puts AURACTL_HVC_MAGIC in w0 and issues `hvc #AURACTL_HVC_IMM` on this thread, then branches to
 * acct's return-with-errno epilogue. So the tool, as root, calls acct() with its request block in
 * place of the path: aura reads and writes the request through this process's own EL0 translation,
 * exactly as the IOKit entry's kext would, and acct hands aura's result back as the syscall result
 * (0 = success, else -1 with the Darwin errno aura returned).
 *
 * A non-root caller never reaches the patch (acct's root check still sends it to EPERM), and a root
 * caller that is not auractl passes a real path whose first word is not AURACTL_REQ_MAGIC, so aura
 * answers EINVAL and acct fails as it would have: either way acct() looks like the acct() it was.
 */
#include "entry.h"

#include <errno.h>

/* acct(2), BSD syscall 51. Declared here because libc's prototype is guarded behind feature macros
 * on some SDKs; this is the BSD signature. The patched kernel never dereferences the argument as a
 * filename -- it takes it as the process's &struct auractl_req. */
extern int acct(const char *path);

int auractl_enter(struct auractl_req *req)
{
    errno = 0;
    if (acct((const char *)req) == 0)
        return 0; /* aura returned success: w0 was 0, so acct's epilogue returned 0 */
    /* acct failed: libc put aura's Darwin errno in errno. Fall back to EFAULT only if the syscall
     * layer left none (it always sets one on a -1 return). */
    return errno ? errno : AURACTL_EFAULT;
}

const char *auractl_entry_name(void)
{
    return "xnu";
}
