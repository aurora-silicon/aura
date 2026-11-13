# Third-party provenance

Aura preserves the licence and attribution of incorporated public code.

## Pavel Shago's m1n1

The T8122 DPTX PHY tables and sequencing in `src/dcp/dptx_phy.c` derive from
Pavel Shago's public m1n1 commit
[`453f0161`](https://github.com/shgpavel/m1n1/commit/453f0161). The retained-DCP
RTKit oslog-notify handling in `src/rtkit.c` derives from public commit
[`cf2e8187`](https://github.com/shgpavel/m1n1/commit/cf2e8187). That repository
is MIT licensed; these inherited portions remain MIT under `LICENSES/MIT.txt`,
with Pavel Shago and the Asahi Linux contributors attributed in the source.

The J773g series also depends on the ANS takeover introduced by commit
`f2a8b52b` (`payload: take over a live post-M4 ANS before booting a non-Linux
payload`). It is included in this series as commit `097f631c`; without it the
older Stage 1 leaves ANS live without a valid Stage 2 handoff and U-Boot cannot
reach NVMe.
