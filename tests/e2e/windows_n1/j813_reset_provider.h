/* SPDX-License-Identifier: MIT */
/* Minimal non-personal fields extracted from a physical J813 ADT.
 * The bridge reset provider is the smc-pmu child, not the SMC controller.
 * Keep these independent of the production admission implementation. */
static const struct {
    const char *path;
    const char *compatible;
    u32 phandle;
} j813_reset_nodes[] = {
    {"/arm-io/smc", "iop,ascwrap-v6", 0xd7},
    {"/arm-io/smc/iop-smc-nub", "iop-nub,rtbuddy-v2", 0xd8},
    {"/arm-io/smc/iop-smc-nub/smc-pmu", "smc-pmu", 0xd9},
};
static const u32 j813_function_reset[] = {0xd9, 0x704b5734, 0x67503133, 0x800000};
