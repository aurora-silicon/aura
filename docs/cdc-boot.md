# Guarded CDC uploads on J813 and J873g

The boot tools can upload an Image, initramfs and device tree over USB2 CDC,
then return to KIS for Linux entry and console capture. KIS remains the default.
This supports J813 (T8142, Mac17,3) and the 26A428 J873g (T8152, Mac18,5), board
0x24. It does not change the J700 CDC policy or enable an automatic transition.

Use a matching built image and ELF pair. `boot_j813.py` checks `aura.macho`
against `aura.elf`; `boot_j873.py` checks `aura.bin` against `aura-raw.elf`.
J813 must already be running that public Aura image. Its first chainload from
an older resident must use a client matching the resident ABI, with a fresh
physical PMU guard. Do not use an old resident's heap opcode with public Aura.

## Procedure

Identify the physical serial, current KIS proxy/console endpoints and the host
HPM port before starting. Use normal Python, never `-O` or `PYTHONOPTIMIZE`.
Each hardware action runs a fresh physical PMU guard; a failed guard stops the
path. A port appearing is not a successful guard. Do not send proxy commands
to a running Linux console.

Choose `proxyclient/tools/boot_j813.py` or `proxyclient/tools/boot_j873.py` as
`BOOT_TOOL`, and select the matching `j813_pmu_guard.py` or
`j873_pmu_guard.py` as `PMU_TOOL`. Set `IMAGE_SHA256` to the SHA-256 of the corresponding image file
above. Use a fresh `RUN` directory and an explicit CDC primary endpoint; do
not guess from enumeration order. On macOS its name commonly includes the
physical serial, but the tool also verifies the USB descriptor and live ADT.

With the matching Aura running on `KIS_PROXY`:

```sh
python3 "$BOOT_TOOL" cdc --image "$BUILD" --image-sha256 "$IMAGE_SHA256" \
    --serial "$SERIAL" --device "$KIS_PROXY" --console "$KIS_CONSOLE" \
    --cdc-device "$CDC_PRIMARY" --output "$RUN"
```

This records the guarded KIS identity, schedules one transition with no reset
or lane-swap flags, and verifies the same image, base, ADT, serial and chip on
the selected CDC primary. A fully checked 1 MiB memory roundtrip must pass.
The tool neither scans other targets nor resets/retries an uncertain transition.

The artifact directory contains `Image`, `initramfs.cpio.gz`, the machine's
`t8142-j813.dtb` or `t8152-j873g.dtb`, and `SHA256SUMS` with local basenames.

```sh
python3 "$BOOT_TOOL" prepare --image "$BUILD" --image-sha256 "$IMAGE_SHA256" \
    --serial "$SERIAL" --device "$CDC_PRIMARY" --console "$KIS_CONSOLE" \
    --transport cdc --artifacts "$ARTIFACTS" --output "$RUN"
```

Preparation takes a new physical PMU receipt on CDC and requires the successful
CDC receipt to match the current session. All uploaded bytes are read back and
hashed. The tools return with the prepared device tree and receipts for review;
they do not enter Linux as part of the upload.

Before changing host routing, take another physical PMU receipt on CDC:

```sh
python3 "$PMU_TOOL" --device "$CDC_PRIMARY" --serial "$SERIAL" \
    --output "$RUN/restore-pmu"
```

Continue only when this command succeeds with `passed=true`,
`offline_only=false` and `after_raw=0`. Restore the previously verified host
HPM port to debug USB **without rebooting**.
For hosts using `macvdmctl`, supply the mapped `--rid N` explicitly and use its
`debugusb` action, not `reboot`. This is a host routing operation; the boot tools
do not choose a host port or issue it automatically. Do not change the cable.

```sh
python3 "$BOOT_TOOL" audit --image "$BUILD" --image-sha256 "$IMAGE_SHA256" \
    --serial "$SERIAL" --device "$KIS_PROXY" --console "$KIS_CONSOLE" --output "$RUN"
python3 "$BOOT_TOOL" boot --image "$BUILD" --image-sha256 "$IMAGE_SHA256" \
    --serial "$SERIAL" --device "$KIS_PROXY" --console "$KIS_CONSOLE" --output "$RUN"
```

The final action runs another PMU guard and rechecks the prepared state. Linux
entry requires the restored KIS proxy. The tools retain the already-open
proxy descriptor and save `aura-handoff-console.bin` alongside
`linux-console.bin`, preserving early native CPU-park messages. Retain both and the
workload's artifacts: a completed boot command or successful upload alone is
not a hardware test pass. The initial J813 path keeps secondary CPUs stopped;
J873 retains its existing native park and reserved-memory audit requirements.

Omitting `--transport cdc` retains KIS preparation, without a CDC receipt.
Each action has an exclusive start marker. On failure, preserve all receipts,
logs and target state; do not delete a marker to force a retry.

## Runtime boundaries

J813 reuses USB0 only if normal startup already initialized it. J873 validates
the USB0 ADT and cold power state, initializes only an unlocked unused Gen3 DART
stream, and bounds its power/PHY polls. Both use an explicit AP USB2 routing
command with no port-controller reset. KIS polling remains available so the
host can restore debug routing after the upload. Live/locked streams are not
reinitialized, and allocations are retained after uncertain setup failures.
M5/M6 USB2 console flushing is bounded so an opened but disconnected CDC pipe
cannot hold a later RAM handoff indefinitely. Upload readback remains mandatory.

## Hardware validation

The 2026-11-06 macOS-host run passed on both machines, using runtime commit
`d10758b1a9e0d69db5610f699ba1bb993df8a6d0` and the host-tool fixes through
`e6d7a2a8`. Runtime C sources did not change after that build. The exact binary
hashes, public Linux/Mesa dependencies and sanitized results are in
[the validation artifact](validation/cdc-j813-j873-20261106.json).

| Machine | Image + initramfs write/full readback | GPU smoke | Other checks |
| --- | --- | --- | --- |
| J813 | 10.15 s | 3 cases, 12,288 exact pixels | 16 checks, zero taint |
| J873g | 12.72 s | 3 cases, 12,288 exact pixels | 23 checks, 3 matched retirements, 125 thermal channels, 11 native park acknowledgements |

Both completed the 1 MiB CDC roundtrip, full device-tree upload/readback,
explicit KIS restoration, prepared-tree audit and final Linux health check.
Every physical PMU guard passed with a zero counter. Timings cover payload
write plus full readback, not the complete boot. The runtime build used
`RELEASE=1 CHAINLOADING=1 ESP_STAGE2=1 BUILDSTD=1 EXTRA_CFLAGS=-Oz`.

This is a single-CPU, RAM-filesystem rendering smoke. It does not qualify
SMP, suspend/resume, a full desktop or graphics conformance. Retained local
evidence also includes the earlier failed host/transport attempts; their
markers were preserved, and uncertain transitions were not treated as passes.
