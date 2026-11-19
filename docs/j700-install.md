# J700 Stage 1 configuration and installation

Stage 1 loads Stage 2 from an EFI System Partition (ESP). Stage 2 can load a
kernel image, device tree and optional initramfs from that partition using
`boot=`. Keep the previous Stage 1 and Stage 2 images for rollback.

## Fill the Stage 1 configuration

Use a bare Stage 1 image built with `RELEASE=1 CHAINLOADING=1
BUILTIN_LOGO=aurora`. The `LOGO=` build option appends a
payload and must not be used for the installed Stage 1.

Fill its versioned configuration block with the ESP's PARTUUID and the
relative Stage 2 path:

```sh
python3 tools/fill_stage1_config.py \
  --input aura-stage1.bin \
  --output aura-stage1-filled.bin \
  --esp-partuuid '<ESP PARTUUID>' \
  --stage2-path aurora/stage2-test.bin
```

The tool requires a canonical lowercase UUID, a relative FAT path and a
window from 0 to 99999 ms. It changes only the configuration block, keeping
the image length and `STACKBOT` tail. An empty configuration stays
proxy-only. A malformed configuration falls back to the proxy.

Use a separate Stage 2 file for testing. Select `aurora/stage2.bin` only
when filling the Stage 1 for the final installation.

## Compose the ESP Stage 2

Build the ESP-loaded image with `RELEASE=1 CHAINLOADING=1 ESP_STAGE2=1
BUILTIN_LOGO=aurora`. `ESP_STAGE2=1` omits the embedded Stage 1 configuration;
The option identifies the image's role; it is not a board-specific build.
Transport and firmware handoff special cases retain their platform gates.
Both stages include High-Speed and SuperSpeed USB implementations.
Use `aura-debug` to preserve DebugUSB and `aura-proxy` to enter the proxy.
Normal configured boots do not wait; `--window-ms` explicitly enables a
development interception window.

The `boot=` parser and proxy operation `0x705` are available on all chips in
`CHAINLOADING=1` builds. This does not establish hardware qualification of
the direct ESP boot route on M1–M3. The live-controller NVMe handoff uses the
`NVME_T8132` controller path; other controller types retain their existing
shutdown path. J700 qualification is in progress.

Copy the ESP-loaded image, then append newline-terminated variables:

```sh
cp aura-stage2.bin stage2-for-esp.bin
printf '%s\n' 'boot=<ESP PARTUUID>;aurora/<kernel>/Image;aurora/<kernel>/t8140-j700.dtb;aurora/<kernel>/initramfs.cpio.gz' >> stage2-for-esp.bin
printf '%s\n' 'chosen.bootargs=<kernel command line>' >> stage2-for-esp.bin
```

Put the resulting file at the path selected in Stage 1, initially
`aurora/stage2-test.bin`. The `boot=` value has exactly four nonempty fields.
Install the named Image or Image.gz, matching DTB, and gzip or cpio initramfs
on the ESP. Use kernel modules from the same kernel build.

Do not append a four-byte zero terminator to the ESP file. Stage 1 appends
its chosen variables and NVMe adoption setting, then adds the terminator in
RAM. An earlier terminator hides those appended variables from Stage 2.

For an EFI boot manager such as U-Boot, use `-` for the initramfs field:

```sh
printf '%s\n' 'boot=<ESP PARTUUID>;aurora/boot/u-boot.bin;aurora/boot/t8140-j700.dtb;-' >> stage2-for-esp.bin
```

This loads the image and DTB without publishing a loader initramfs. The EFI
OS image supplies its own initramfs. Do not substitute an empty cpio archive:
Linux prioritizes the device tree's `linux,initrd-*` over an EFI initramfs.

For an embedded-DTB U-Boot payload, follow [stage2-dtbs.md](stage2-dtbs.md)
to rebuild the DTB set from the intended kernel source. A kernel-only update
does not replace embedded Stage 2 DTBs.

## Test and install

Test every new build before replacing an installed image:

- Boot Stage 2 from its separate ESP file through a Stage 1 configured for
  that path. Keep the previous `aurora/stage2.bin` as a backup.
- Check the build tag in the Stage 2 console and
  `/chosen/aurora,aura-stage2-version`. Keep the kernel log of each test boot.
- Test a new Stage 1 with a host claiming the proxy window, with no host
  until the window expires, and with a missing Stage 2 file. Check proxy
  fallback, image length and the panel logo.
- Complete the platform's boot and suspend qualification before promoting
  the candidate to the normal Stage 2 path.

Final installation uses the filled **bare** Stage 1. From the Linux stub's
own recoveryOS, run `kmutil configure-boot` against that stub's System volume
with `--raw --entry-point 2048 --lowest-virtual-address 0`; the machine owner
enters the password. Keep the previous bare Stage 1 for rollback using the
same flags. Updating ESP Stage 2 or kernel files does not require 1TR.
