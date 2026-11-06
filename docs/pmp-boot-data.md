# PMP boot data on T6000 and T8112

Stage 2 copies the current ADT's PMP inputs into the existing Linux PMP node
on these two SoCs, including when the `pmp` alias is absent. It reads board
and DRAM-vendor identifiers from `/chosen`, optional capacity from
`/arm-io/pmp/iop-pmp-nub`, and the same opaque tunable properties as the common
PMP handoff used on T6040 and T8140. It never substitutes another board's data.
Scalar inputs must be exactly four bytes; tunable bytes are copied unchanged.

The node's `status` is preserved. A disabled Linux node stays disabled, with
its inputs ready for a separately selected experimental profile. For a trial,
compile the whole board DT with `APPLE_USE_PMP`: the Linux profile enables PMP
and also selects its consumer report-domain topology. Do not expect a change
to `status` alone to select those dependencies. A DTB without a PMP node is
unchanged, and the T8122 dynamic handoff retains its existing gate.

Run `sh tests/run-pmp-boot-data.sh` for synthetic ADT/real libfdt coverage of
both SoCs, alias presence, activation policy, endian handling, optional
capacity, malformed and missing properties, and insufficient FDT space.
Passing a pre-change ref runs a regression negative control that should fail
because that code does not populate an alias-free PMP node.

Use [the Stage 2 composer](stage2-dtbs.md) with freshly compiled DTBs and a
qualified U-Boot payload. Stage 2 changes must first be tested by a RAM-only
chainload through the debugusb proxy. Keep the baseline ESP payload intact.
The Linux driver starts firmware already resident in SRAM; after ASC RUN,
failure recovery requires a reboot rather than live unbind or register repair.
