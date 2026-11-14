# T8122 GPU discovery from Linux

This personal fork supports AI-assisted M3 Air development. Its first step is
metadata discovery, not GPU activation. The M3 Pro resource and firmware
constants are not evidence for T8122.

Append this option when constructing a **stage-2** boot.bin:

```
chosen.asahi,t8122-gpu-diag=1
```

On T8122 only, after mandatory device-tree fixups, aura copies selected fields
from the iBoot ADT into `/chosen/asahi,t8122-gpu-diagnostics`. It reads ADT RAM
only: no GPU registers, power changes, firmware reads, memory reservations or
GPU enablement. The exported metadata is not a driver handoff.

The exporter accepts only the exact NUL-terminated string `1`. It creates a
private FDT copy, bounds each property to 32 KiB and the total payload to 48 KiB,
and publishes only a complete export. Missing SGX/ASC, exhausted FDT capacity,
allocation failure or oversized data leaves the original FDT unchanged and
allows boot to continue. Existing exports are left untouched. Other chips do
not run this exporter. It does not require a GPU alias in the kernel DTB.

The explicit property allowlist covers register/range metadata, interrupt and
power routing, PMGR devices, performance tables, and GPU/UAT memory-region
descriptions. It never copies arbitrary ADT contents or firmware images.
Missing optional properties stay absent. `complete` means the export finished;
it does not mean all information needed for bring-up exists.

`schema-version` is a big-endian FDT u32. Properties prefixed `adt,` preserve
their raw ADT encoding, normally little-endian. They are deliberately not named
`reg`, `ranges`, or `compatible`, so consumers do not mistake them for bindings.

After an approved installation and reboot:

```sh
python3 tools/read-t8122-gpu-diagnostics.py > /path/outside/repo/t8122-gpu.json
```

The reader verifies the schema and completion marker, preserves the raw bytes,
and labels numeric views by width and endianness without guessing their units.
Keep machine-derived output local. Use the ranges and PMGR data to ground the
T8122 DT before adding any hardware startup code.

Validation:

```sh
sh tests/run-t8122-gpu-diagnostics.sh
python3 tests/test_t8122_gpu_reader.py
make -j4 BUILDSTD=1
```

The host test uses synthetic ADT and real libfdt, including byte-for-byte
rollback across capacity boundaries. In a ptrace-based sandbox, LeakSanitizer
may require `ASAN_OPTIONS=detect_leaks=0`; ASan and UBSan remain enabled.

Before replacing boot.bin, preserve the current stage-2 image independently
and prepare restoration from a known-working environment. A fallback GRUB
kernel cannot recover a failure in aura that occurs before GRUB. Do not alter
macOS/recovery partitions, stage 1, or boot policy for this experiment.

## Decode captured resources locally

```sh
python3 tools/t8122_gpu_resources.py /path/outside/repo/t8122-gpu.json \
  > /path/outside/repo/t8122-gpu-resources.json
python3 tests/test_t8122_gpu_resources.py
```

The decoder uses the layouts in `proxyclient/aura/adt.py` and `src/pmgr.c`:
64-bit little-endian arm-io range triples, register pairs, 48-byte PMGR
records, and frequency/voltage pairs. It checks full register extents against
arm-io ranges, follows GPU power-gate parent chains, and resolves state-register
addresses only for nonvirtual devices. Truncation, conflicting translations,
cycles, missing parents, invalid indices, and inconsistent counts are rejected.
Performance values retain their raw units. IRQs retain ADT ordering and the
validity mask; this does not assign mailbox IRQ roles.

The decoder does not establish the mailbox layout, GPU core topology, thermal
model or firmware ABI. It does not turn ADT performance tables into a safe
power model, reserve runtime RAM or enable Linux GPU nodes. Keep both its input
and its machine-derived output outside git. Tests use synthetic records only.

## Exporter revision 2

Revision 2 retains schema 1 and the same opt-in. It adds explicitly allowlisted
SGX thermal/control properties, ASC and optional firmware-nub segment names and
ranges, chosen DRAM bounds and root address-cell widths. Segment descriptions
are ADT metadata, not copied firmware bytes. Missing optional nodes/fields stay
absent; they do not establish firmware compatibility.

`exporter-revision` is a big-endian u32. `usable-memory` is a big-endian u64
base/size pair from the current kboot RAM bounds. All `adt,*` fields retain the
original raw encoding. No GPU MMIO, power transitions, firmware reads or Linux
GPU-node enablement are performed. Property/aggregate size limits and atomic
publication remain unchanged.

The reader supports the original export and revisions 2 and 3. When segment
metadata is present, the resource decoder checks physical extents against DRAM
and usable RAM, rejects overlaps with the four UAT regions, validates virtual
extents, and compares optional nub metadata. It does not validate TrustZone
carveouts or firmware contents, reserve memory, or identify a firmware ABI.
The 13-versus-14 pstate relationship is checked against the existing parser's
N-active-states-plus-one-off-state convention, without creating power limits.

## Exporter revision 3: loaded firmware identity

Revision 3 retains the same opt-in and all revision-2 fields. It additionally
reads the 0x38-byte image-info header at offset 0x4200 within the ADT-described
GFX `__TEXT` segment. The read uses the existing DRAM/carveout checks and
synchronous-exception guard. A missing or malformed header leaves the optional
`image-*` properties absent; it does not fail boot or enable the GPU.

When present, `image-uuid` is 16 bytes and `image-patchbay-offset`,
`image-patchbay-size`, and `image-text-size` are big-endian FDT u32 values.
The local reader reports them under `firmware_image` with
`abi_identified: false`. A structurally valid header identifies the loaded
image; it does not prove the firmware ABI or authorize GPU startup. Keep actual
UUIDs and other machine-derived values outside git.

## GPU setup: `chosen.asahi,t8122-gpu=1`

This switch runs the same fail-soft G15 GPU setup that `chosen.asahi,t6030-gpu=1`
runs on the M3 Pro (`dt_set_gpu_g15()` in `src/kboot_gpu.c`), with the T8122
entry of its SoC table. On the M3 MacBook Airs (J613, J615) and the 14" M3
MacBook Pro (J504), a normal build sets it by default, together with
`asahi,t8122-dcp`, the stand-in power model (`asahi,t8122-gpu-power-standin`)
and the leakage fuse (`asahi,t8122-gpu-fuse-leakage`), unless a config line set
it (`dt_set_m3_defaults()` in `src/kboot.c`). `chosen.asahi,t8122-gpu=0` keeps
the GPU off; `chosen.asahi,t8122-dcp=0` or `chosen.asahi,m3-defaults=0` keeps
the default GPU switches off too, while an owner's own GPU line still counts.
Other T8122 boards (the J433/J434 iMacs) get no default. Without the switch a
T8122 boot does not run the setup at all (there is no dry run), and only the
diagnostic switches above apply. With it, `asahi,t8122-gpu-handoff-diag` is
ignored.

The setup needs a `gpu` alias to an `apple,agx-t8122` node with the same inputs
as the T6030 node (`reg`/`reg-names` for asc and sgx, `operating-points-v2`,
`memory-region`/`memory-region-names`, `apple,firmware-*`, the four
`/reserved-memory/uat-*` nodes and a `mboxes` mailbox). In order, it

1. marks the gpu node disabled;
2. identifies the loaded GFX firmware against the T8122 entries of the
   known-image table (`firmware_gfx_version(T8122)`);
3. reads and checks the pstates, the four UAT regions and the firmware
   segments from the ADT, and the gpu node, OPP table and mailbox from the DT;
4. requires the GPU power closure to be off, powers it on, reads the GPU ID
   (family 7, variant 2), `ID_COUNTS_1` (one die; cores per cluster and
   clusters, as drm/asahi reads them for G15G) and the core masks, and
   rejects mask bits beyond the reported slots or more pstate tables than
   clusters;
5. reads the leakage value, computes the per-pstate maximum power and fills
   the OPPs (one microvolt per cluster), the leakage coefficients, the UAT
   regions, `apple,firmware-*` and the firmware segment description;
6. enables the UAT regions, the mailbox if the DT keeps it disabled, and then
   the gpu node.

Any failure leaves the gpu node, the UAT regions and the mailbox disabled,
zeroes `apple,firmware-compat`, releases the GPU power closure recursively
and checks that it is off, and lets the boot continue.

The T8122 table knows the 14.8.3 G15G image the Airs and the J504 load. Its
power model is a stand-in worked out on the J613, not measured: it sets power
targets, not clocks. The leakage fuse decode was also worked out on the J613.
A boot with an unknown image still stops at step 2, before any GPU register or
power access, and logs the loaded image (`FW: GFX: image uuid ...`). linux-aurora
starts the GPU only on the boards in its own T8122 list (J613, J615, J504), at
a conservative cap.

Host tests (synthetic ADT, simulated bus, real libfdt):

```sh
sh tests/run-kboot-gpu-g15.sh aurora7
```

The T8122 scenarios check the default (no change at all), the refusal with
the current tables, and the whole pipeline with the missing data supplied by
the test: power closure, GPU ID, die count, core mask, pstate table count, DT
capacity and power-release failures, and precedence over the diagnostic.

## J613 image metadata admission

The `air/t8122-g15g-image-admission` experiment can publish
`apple,firmware-compat = <14 8 3>` for one J613 boot. It first requires the
existing diagnostic opt-in, validated UAT and firmware reservations, a loaded
image-info header whose text size matches the ADT segment, and a successful
bounded powered identity read with this J613's measured G15G core mask and
counts. It then requires three strings in `/chosen`:

```dts
asahi,t8122-gpu-abi-ack = "14.8.3";
asahi,t8122-gpu-expected-uuid = "<32 lowercase or uppercase hex digits>";
asahi,t8122-gpu-expected-rtkit = "<exact version string>";
```

The expected values are owner-supplied and must stay outside source control.
The stage compares the UUID with the loaded image header and the RTKit string
with the loaded firmware version. These fields are metadata, not a
cryptographic signature. The `abi-ack` is the owner's explicit assertion that
the image in use carries the 14.8.3 ABI; aura cannot prove that ABI from the
header alone.

The stage clears any input `apple,firmware-compat` claim before checking these
conditions. On mismatch it leaves the claim absent. Even on success the GPU
and mailbox nodes stay disabled; no GPU firmware is started and no render node
can result from this test. A later Linux experiment must still validate power,
firmware startup and the J613 hardware configuration before enabling them.

## Separate J613 ASC register preflight

The `air/t8122-asc-preflight` experiment additionally accepts
`/chosen/asahi,t8122-gpu-asc-preflight = "1"`. Only after the exact J613
identity, loaded image metadata and UAT/firmware reservations pass does it
enable the GPU and mailbox nodes together in the FDT. The status edits are
published atomically from a copy, so an unsuccessful edit leaves both nodes
disabled. A matching Aurora diagnostic kernel can then read SGX identity and
pulse the ASC RUN control once before returning `ENODEV`. No UAT setup, RTKit
handshake, firmware initdata, GPU job or DRM render node is part of this test.
The boot opt-in is separate from image admission and has not been enabled or
booted on J613 yet.

When the full GPU setup is opted in with `chosen.asahi,t8122-gpu=1`, that path
owns the gpu and mailbox enable and the reservation-only diagnostic that hosts
this preflight does not run. The preflight additionally no-ops (and logs one
line) if it ever sees that opt-in, so the two paths can never both enable the
same two nodes in one boot.
