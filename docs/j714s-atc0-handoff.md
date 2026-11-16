# J714s ATC0 ownership handoff

The Linux left-back USB-C description starts disabled. Its
`/chosen/apple,atc0-handoff` property lists the 27 ATC0 power, PHY, USB, HPM,
router, tunnel and crossbar nodes that may be enabled together. Shared AUSS,
CIO reset and AOP GPIO providers are not members. GPIO77 remains reserved.
An older producer leaves this port disabled; the existing two ports still work.

Aura keeps port zero excluded from the ordinary PHY export. At the final
kernel handoff, after successful display shutdown, it may activate the group
only if USB shutdown succeeded, there are no retained DMA ranges or overflow,
and no console, DebugUSB host or USB0 controller-use reservation exists.
Debug ownership is sampled before CDC cleanup and checked again at publication.
A sticky USB0 bring-up marker includes failed or partially registered gadgets;
a later empty shutdown cannot erase this evidence. Debug/proxy boots keep the
port reserved for that boot even if their gadget stops successfully.

Only the known kernel DT buffer or registered hibernation copy is eligible.
All group handles must be valid, unique and explicitly disabled. The group
must contain the ATC0 PHY and router; its LSTX reference must identify pin77,
which must still be reserved. An already-enabled or malformed group refuses
the next-stage boot, including a repeated handoff against an activated tree.

The update uses a private DT copy. All 14 PHY tables are exported from this
machine's ADT with the existing controller-parent, register-bank, size and
alignment checks. The producer then sets `apple,guest-port-reserved`, releases
only GPIO77 and enables every group member. Allocation, export or property-space
failure discards the copy and leaves the original disabled group unchanged.
There are no register reads or writes in this DT operation. Other boards and
older DTs without the group are unchanged.

Run the production code against an external ADT and the matching Linux DTB:

```
python3 tests/atc0_handoff_policy.py /tmp/atc0-policy
sh tests/run-atc0-handoff.sh J714s.adt fixed-linux.dtb /tmp/atc0-evidence
```

The harness aborts on any MMIO. It retains before/after DTBs, failure logs,
result records and SHA256 manifests. The inputs are not copied into the source
tree. CPU execution and a successful build do not qualify a physical port;
normal-boot and debug-host hardware results are required before merging.
