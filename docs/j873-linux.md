# J873g RAM-only Linux boot

The T8152/J873g minimal handoff supports the 26A428 firmware profile
`G18G-RTKit-3514.0.15.release`. It validates both GPU firmware TEXT images and
the boot-local memory layout before enabling their mailboxes. Other firmware
profiles are refused. This is a bring-up path, not a general desktop installer.

Build Aura with `make RELEASE=1 CHAINLOADING=1 ESP_STAGE2=1 BUILDSTD=1` (GNU make;
use `gmake` on macOS). Keep `build/aura.bin` and `build/aura-raw.elf` together.
Install the dependencies from `requirements.txt` in a Python virtual
environment. Do not run the controller with Python optimization (`-O`).

Use Linux `aurora-wip` with the J873 SMC hardware-monitoring change and its
standard `apple/t8152-j873g.dtb`. Both G18 and M5 GPU backends can be enabled.
The SMC change supplies the counted mailbox, read-only temperature discovery
and fan telemetry. Firmware retains fan control. A RAM filesystem needs
`mfd-core.ko`, `macsmc.ko` and `macsmc-hwmon.ko` if those drivers are modules.

## Preparing artifacts

Put `Image`, `t8152-j873g.dtb` and `initramfs.cpio.gz` in one artifact directory.
Generate its `SHA256SUMS` with those three filenames and no directory prefixes.
The initramfs must provide `/init`; use only RAM and virtual filesystems for this
procedure. The controller starts Linux with one CPU and the validated low clock
settings. Secondary CPUs are parked by Aura with their MMUs and caches disabled.

For the G18 graphics smoke, use Mesa `aurora-wip`. The GPU driver additionally
requests these locally obtained firmware inputs under `/lib/firmware/asahi/`:

- `g18-26a428-color-a-v2.bin`
- `g18-26a428-color-b-v2.bin`
- `g18-26a428-gears-g1-v2.bin`
- `g18-26a428-fragment-witness-v2.bin`
- `g18-26a428-compute-add.bin`
- `g18-26a428-compute-triangle.bin`

These inputs are not distributed by Aura. Their absence prevents the graphics
smoke; Linux boot and SMC telemetry do not require them. This patch does not
provide a firmware extraction workflow.

## Guarded host procedure

Identify the target serial number, Aura KIS proxy port and separate Linux console
port before running a command. The controller verifies the live serial, chip,
board and model. Never send proxy commands or the PMU guard to a running Linux
console. A fresh successful PMU receipt is required before every chainload,
prepare or boot. A failed or uncertain phase must be investigated without an
automatic reset or retry. Use a new output directory for a new boot attempt.

The following shell variables are examples to fill from the current connection;
do not infer a target from the host port number alone:

```sh
AURA_IMAGE=build
AURA_SHA=$(shasum -a 256 "$AURA_IMAGE/aura.bin" | cut -d ' ' -f 1)
J873_SERIAL=YOUR_TARGET_SERIAL
J873_PROXY=/dev/cu.kis-XXXXXXXX-ch-1
J873_CONSOLE=/dev/cu.kis-XXXXXXXX-ch-0
J873_ARTIFACTS=/path/to/artifacts
J873_RUN=/path/to/new-run
```

An installed, identified Aura proxy with a working framebuffer is the starting
point. If it is an older Aura, supply its matching binary/ELF pair and image hash
with `--source-image` and `--source-sha256` for the `chainload` action. This only
loads RAM; it does not replace the registered boot object.

```sh
python proxyclient/tools/boot_j873.py chainload \
  --image "$AURA_IMAGE" --image-sha256 "$AURA_SHA" \
  --source-image /path/to/installed-aura-pair --source-sha256 INSTALLED_SHA256 \
  --serial "$J873_SERIAL" --device "$J873_PROXY" --console "$J873_CONSOLE" \
  --output "$J873_RUN"

for phase in prepare audit boot; do
  python proxyclient/tools/boot_j873.py "$phase" \
    --image "$AURA_IMAGE" --image-sha256 "$AURA_SHA" \
    --serial "$J873_SERIAL" --device "$J873_PROXY" --console "$J873_CONSOLE" \
    --artifacts "$J873_ARTIFACTS" --output "$J873_RUN" || break
done
```

KIS uploads are slow. Each uploaded image is read back and hashed. Preparation
checks four known SMC temperature keys and the fan, saves the ADT and prepared
FDT, and reserves the retained Aura/display memory. The separate audit verifies
reservations, CPU release slots, framebuffer and initrd bounds. Boot requires that
audit, quiesces the host SMC session, and captures the Linux console for 60 seconds.
The console capture itself is not evidence of a successful Linux or GPU test.

Keep the image hashes, PMU receipts, prepared FDT, console capture and the
initramfs test's results together. A graphics result should include renderer
identity, saved pixels, matched GPU submissions/retirements, before/after SMC
readings, kernel taint and fault logs. Three basic rendering cases do not establish
CTS conformance, SMP, suspend, storage or networking support.

## J813

J813 uses Aura `main`, Linux `aurora-wip` (including the J813 GPU device-tree and
mailbox prerequisites), and Mesa `agx3c`. Use the J813 boot procedure and
`t8142-j813.dtb`; this J873-specific host controller deliberately refuses J813.
