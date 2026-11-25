# Running macOS 27.2 as an SPTM-virt guest

This guide walks through booting a stock macOS 27.2 (`26B123`) installation as a
FEAT_NV2 guest under `aura` on an Apple Silicon Mac (proven on Mac mini M2 /
T8112, J473, and compatible with M4/M5 SPTM-class Macs described in
[sptm-nv2-virt.md](sptm-nv2-virt.md)).

## Prerequisites

1. **Stock macOS 27.2 installed on the target Mac** alongside your Aurora Linux
   installation. `aura`'s Stage 1 boot object must be enrolled on that macOS
   APFS volume group so iBoot initializes SEP and xART for it.
2. **A Linux tether host** connected to the target Mac's USB-C DFU port with a
   USB-C data cable. On the host, install `python3`, `pyserial`, `construct`,
   and `llvm-nm` (or `nm`).
3. **Guest images from your own macOS 27.2 installation or IPSW**, extracted
   into a local directory on the host (for example `img/`):
   - `sptm.bin` (SPTM Mach-O)
   - `txm.bin` (TXM Mach-O)
   - `kernelcache.release.<board>` (decompressed XNU kernelcache, e.g.
     `kernelcache.release.mac14g` for T8112)
   - `trustcache.bin` (static trust cache)
   Do not commit or redistribute Apple firmware or kernel binaries.

## 1. Build `aura` with `SPTM_VIRT=1`

Build an in-place SPTM-virt image (add `AURACTL=1` if you want kext-free
in-guest MMIO tracing via [auractl.md](auractl.md)):

```sh
make USE_CLANG=1 BUILDSTD=1 CHAINLOADING=1 SPTM_VIRT=1 AURACTL=1 EXTRA_CFLAGS=-Oz
```

This produces:
- `build/aura-boot.macho`: `kcbz=0` Mach-O boot object for one-time Stage 1
  enrollment (`kmutil configure-boot`).
- `build/aura.bin` and `build/aura-raw.elf`: raw Stage 2 image and ELF symbols
  used by `run_guest_sptm.py` (and optional in-place chainloading).

`SPTM_VIRT=1` omits the Linux `kboot` path so the entire image (`_end`) stays
well below the display coprocessor's `__OS_LOG` buffer that iBoot places after
Stage 1.

## 2. Enroll Stage 1 on the macOS volume group (one-time)

Follow [recovery-enroll.md](recovery-enroll.md) and
[macho-boot-object.md](macho-boot-object.md) in the macOS volume's paired One
True recoveryOS (1TR):

1. Verify `kmutil inspect -z -B aura-boot.macho --show-mach-boot-properties`
   reports `kcbz : 0`.
2. In paired 1TR, check `bputil -d -v <GROUP_UUID>` and disable Kernel CTRR if
   enabled (`bputil -nc -v <GROUP_UUID>`) so secondary cores do not fault on
   stores inside `aura`'s image.
3. Enroll `aura-boot.macho` on the macOS volume group (for example
   `Macintosh HD`) with `tools/recovery/enroll-stage1.sh`.

Leave your Aurora Linux volume as the persistent default startup disk.

## 3. Start the host-side tether and `vuart` console

On the Linux host connected to the target Mac's DFU port, start
`proxyclient/tools/sptm_guest_session.sh` before rebooting the Mac:

```sh
sudo proxyclient/tools/sptm_guest_session.sh \
    --out /tmp/macos-guest \
    --img-dir /path/to/img \
    --elf /path/to/aura/build/aura-raw.elf \
    --stage2 /path/to/aura/build/aura.bin \
    --os-long-version 26.2.123.0.0,0 \
    -- \
    AURA_S2R_TRACE=1 AURA_XNU_ENTRY=1 AURA_XNU_SERIAL=uart0 \
    AURA_HIDE_DOCKCHANNEL=1 AURA_WFI_WATCH=1 \
    AURA_TAIL_SECS=1700 AURA_TAIL_IDLE=0
```

Omit `--stage2` if the enrolled Stage 1 already matches the `aura` build you
want to run.

What the helper handles automatically:
- Waits for CDC Pipe 0 (`*aura_uartproxy*-if00`) when Stage 1 boots.
- Runs a read-only precheck verifying that `_end` of `--elf` does not overlap
  any `dcp`/`dcpext` `__OS_LOG` buffer.
- Optionally chainloads `--stage2` in place (`chainload.py -r --vector-only`).
- Starts `proxyclient/tools/sptm_vuart_tail.py` on CDC Pipe 1 (`*-if02`,
  `IODEV_USB_VUART`) with `DTR=1` before entering the guest. Raising DTR on
  Pipe 1 streams XNU's verbose `-v` `uart0` boot log to `<out>.vuart.txt` and
  keeps DWC3 events draining Pipe 0 (`*-if00`) aura console output to
  `<out>.guest.txt`.
- Launches `proxyclient/tools/run_guest_sptm.py` and watches `<out>.stop` so
  you can stop the console tail cleanly at any time.

For bounded unattended runs, you can also pass
`AURA_GPU_STOP_BOOT_SECS=<n> AURA_GPU_STOP_REBOOT=1` so `aura` flushes console
output and triggers a hardware watchdog reboot after `<n>` seconds instead of
parking in the proxy.

## 4. Boot the Mac once into macOS from Linux

On the target Mac (running Linux), arm a one-shot boot to the macOS volume and
reboot:

```sh
sudo aurora-bless --set-boot-macos --next -y && sudo reboot
```

Because `--next` is a one-shot NVRAM selection, the persistent default stays on
Linux. On T8112 (Mac mini M2, J473):
- Stage 1 enumerates over the DFU USB-C cable within a few seconds.
- `run_guest_sptm.py` loads SPTM, TXM, and XNU (`nvram_ro` prevents a failed
  guest boot from writing `recovery-boot-mode` to NOR flash).
- All 8 CPU cores enter XNU and macOS 27.2 reaches userspace, `WindowServer`,
  and SSH in roughly 35-45 s.

If you are running an unattended session, mute the guest immediately after SSH
comes up and keep it awake for your workload:

```sh
osascript -e 'set volume output volume 0 output muted true'
caffeinate -dimsu &
```

## 5. Return cleanly to Linux

From inside the macOS guest:

```sh
osascript -e 'set volume output volume 0 output muted true'
sudo shutdown -r now
```

Then stop the host-side tailers by creating the stop file:

```sh
touch /tmp/macos-guest.stop
```

If the guest was parked in `aura`'s proxy (for example after a reset guard trap
or `AURA_GPU_STOP_BOOT_SECS` without `AURA_GPU_STOP_REBOOT=1`),
`sptm_guest_session.sh` sends `p.reboot()` automatically after the tail exits
(unless `--no-reboot` was passed).

Because `aurora-bless --set-boot-macos --next` only affected one boot, the Mac
reboots straight back into its saved Linux default. Verify on Linux:

```sh
uname -r
aurora-bless --get-boot
aurora-bless --get-boot --next
getenforce
systemctl --failed
```

## Known limits

- **Debug USB-C port hidden from the guest:** `run_guest_sptm.py` hides the
  debug tether's USB-C port (`atc-phy`, `usb-drd`, and its `hpm*` child) so XNU
  does not reset the proxy link. Sibling ports under `/arm-io/i2c*/hpmBusManager`
  stay visible to the guest.
- **In-place image ceiling:** under `SPTM_VIRT=1`, `aura` stays at Stage 1's
  base address so secondary cores can execute out of locked CTXR/CTRR windows.
  `_end` must remain below the board's `dcp`/`dcpext` `__OS_LOG` buffer; the
  precheck in `sptm_guest_session.sh` rejects overlapping images before
  chainloading.
- **Development-only build:** `SPTM_VIRT=1` compiles out the Linux `kboot` path
  and is not used for production Linux Stage 1/2 boots.
- **Collapsed-EL1 guest quirks:** `run_guest_sptm.py` applies per-kernelcache
  workarounds keyed on the XNU release string and an instruction fingerprint
  (`XNU_QUIRKS`), including `nvram_ro` (NOR writes return success in memory
  only) and shared EL1 PAC keys (`pac_keys`). Newer macOS builds whose
  kernelcache layout changes need a matching `XNU_QUIRKS` entry.
- **T8112 (M2) specifics:** macOS 27.2 on T8112 runs SPTM, TXM, and XNU without
  exclaves. External monitors over HDMI and USB-C (including HDR) work with
  discrete refresh rates; MCDP29xx/2900 HDMI bridges report no VRR support.
