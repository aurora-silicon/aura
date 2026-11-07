/* SPDX-License-Identifier: GPL-2.0-only */
/* Whole-controller initialization from public Linux apple-dart.c at
 * 3a0da31bcc027d4b0a28b0501e649522c7a232f6. Only after native scope proof and
 * complete Linux PCIe initialization. Never dereference inherited TTBRs. */
static int n1_dart_command(u32 command)
{
    for (unsigned phase = 0; phase < 2; phase++) {
        bool done = false;
        u64 deadline = timeout_calculate(2000);
        do {
            u32 status;
            if (n1_read(DART + 0x80, 4, &status))
                return -1;
            if (!(status & BIT(31))) {
                done = true;
                break;
            }
            udelay(1);
        } while (!timeout_expired(deadline));
        if (!done)
            return -1;
        if (!phase && n1_write(DART + 0x80, 4, command))
            return -1;
    }
    return 0;
}
static int n1_dart_all_clean(void)
{
    for (unsigned sid = 0; sid < 19; sid++) {
        u32 tcr, ttbr;
        if (n1_read(DART + 0x1000 + 4 * sid, 4, &tcr) ||
            n1_read(DART + 0x1400 + 4 * sid, 4, &ttbr) || tcr || ttbr)
            return -1;
    }
    u32 pending, block, mask;
    return n1_read(DART + 0x4000, 4, &pending) || pending || n1_read(DART + 0x704, 4, &block) ||
                   (block & 0x777) || n1_read(DART + 0x104, 4, &mask) || mask
               ? -1
               : 0;
}
static int n1_dart_acquire(void)
{
    u32 params1, params3, params4, protect, command, enable;
    if (n1_read(DART, 4, &params1) || n1_read(DART + 8, 4, &params3) ||
        n1_read(DART + 12, 4, &params4) || n1_read(DART + 0x200, 4, &protect) ||
        n1_read(DART + 0x80, 4, &command) || (params1 >> 24) != 14 || params3 != 0x2a2a0204 ||
        (params4 & 0x1ff) != 19 || (protect & 1) || (command & BIT(31)))
        return -1;
    /* Version2.4/19 streams is the qualified first acquisition profile. TCR1
     * with an invalid root is an observed reset default, not an active table. */
    for (unsigned sid = 0; sid < 19; sid++) {
        u32 tcr, ttbr;
        if (n1_read(DART + 0x1000 + 4 * sid, 4, &tcr) || n1_read(DART + 0x1400 + 4 * sid, 4, &ttbr))
            return -1;
        printf("Windows N1: initial SID%u TCR=%#x TTBR=%#x valid=%u\n", sid, tcr, ttbr, ttbr & 1);
        if (tcr > 1 || (ttbr & 1))
            return -1;
    }
    if (n1_read(DART + 0xc00, 4, &enable))
        return -1;
    printf("Windows N1: initial ENABLE observation=%#x; acquiring all19 N1 streams\n", enable);
    for (unsigned sid = 0; sid < 19; sid++)
        if (n1_checked(DART + 0x1000 + 4 * sid, 4, 0, ~0U))
            return -1;
    for (unsigned sid = 0; sid < 19; sid++)
        if (n1_checked(DART + 0x1400 + 4 * sid, 4, 0, ~0U))
            return -1;
    /* ENABLE is W1S. Linux writes all bits; a readback of zero is permitted. */
    if (n1_write(DART + 0xc00, 4, ~0U))
        return -1;
    u32 pending, block, diag;
    if (n1_read(DART + 0x4000, 4, &pending))
        return -1;
    printf("Windows N1: stale exceptions=%#x reserved=%#x\n", pending, pending & ~0x7ffffU);
    for (unsigned sid = 0; sid < 19; sid++)
        if (pending & BIT(sid)) {
            u32 status, low, high;
            if (n1_read(DART + 0x8000 + 0x40 * sid, 4, &status) ||
                n1_read(DART + 0x8008 + 0x40 * sid, 4, &low) ||
                n1_read(DART + 0x800c + 0x40 * sid, 4, &high))
                return -1;
            printf("Windows N1: stale SID%u status=%#x address=%#x:%08x\n", sid, status, high, low);
        }
    /* Whole-controller ownership permits Linux's exact observed-bitmap W1C,
     * including reserved bits. No guessed mask and no invented block clear. */
    if (pending && n1_write(DART + 0x4000, 4, pending))
        return -1;
    if (n1_read(DART + 0x4000, 4, &pending) || pending || n1_read(DART + 0x704, 4, &block) ||
        n1_read(DART + 0x210, 4, &diag))
        return -1;
    printf("Windows N1: post-ack BLOCK=%#x DIAG=%#x\n", block, diag);
    /* bit1 configures lock-on-error; it is not a completion bit to clear. */
    if ((diag & 2) && n1_dart_command(0x400))
        return -1;
    if (n1_checked(DART + 0x104, 4, 0, ~0U))
        return -1;
    for (unsigned sid = 0; sid < 19; sid++)
        if (n1_dart_command(0x8100 | sid))
            return -1;
    return n1_dart_all_clean();
}
