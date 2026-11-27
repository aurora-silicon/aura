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

After enrollment that macOS volume always starts `aura`, never XNU directly. A
plain one-shot boot of it (for example `aurora-bless --set-boot-macos --next`)
with no tether host attached leaves the Mac in `aura`'s USB proxy: the screen
stays dark, the Mac does not join the network, and after a few minutes `aura`
reboots into the default startup disk. Booting that volume natively again needs
its original boot object restored from paired 1TR.

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
    AURA_XNU_ENTRY=1 AURA_XNU_SERIAL=uart0 \
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

### Check the displays

`WindowServer` takes a little longer than SSH to light the monitors. Wait about
a minute after SSH comes up, then check from the guest:

```sh
system_profiler SPDisplaysDataType
```

Each attached monitor should be listed with `Online: Yes` and its mode under
`UI Looks like`. Right after SSH the list can still be empty.

macOS shows the login window and the menu bar on its main display, which is not
always the monitor you are looking at. A monitor that is cabled to the Mac but
set to another input still counts. Pick the main display in System Settings >
Displays (drag the menu bar), or from a script with the public CoreGraphics
call `CGConfigureDisplayOrigin` (the display at origin 0,0 is the main display).
macOS stores the choice on the guest's disk, so later guest boots keep it.

### Saving NVRAM changes

`nvram_ro` drops every NVRAM write, including the ones macOS makes when you pair
a Bluetooth device. macOS keeps such a pairing on its own disk, but Linux tools
that read the pairing from NVRAM do not see it. For a session whose NVRAM writes
must persist, add `AURA_SHARE_NVRAM=1`. Back up the NVRAM partition first: if
that guest boot fails, XNU can write recovery or panic state into the real NVRAM.

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
- **USB-C monitors at boot:** with the proxy on a USB CDC port, a USB-C monitor
  can be offered only its 60 Hz modes after the guest boots. Unplugging and
  replugging it brings the full list back. HDMI is not affected. Tracked in
  https://github.com/aurora-silicon/aura/issues/522.
- **`AURA_S2R_TRACE` is a sleep-path debug tool:** leave it out when you only
  want the desktop. On J473 it adds the trace ring and nothing the guest needs.
  Two guest boots with it panicked in the GPU firmware (`GFX DATA ABORT ...
  agx_interrupt(4)`) right after a USB-C display power or mode change. The same
  unplugs without it did not panic. The sample is small (2 of 10 boots with it),
  so treat a GPU panic under the tracer as unproven, and rerun without it first.
- **`AURA_SEP_TRACE` records the SEP's traffic (debug only, `AURACTL=1`):** it
  hooks the SEP mailbox page and puts every message, both ways, into the
  auractl ring. It also copies each endpoint's shared in/out buffers (the ones
  the control endpoint's OOL messages register) at every send and reply,
  through dart-sep's live tables (T8110 layout; on another DART it says so and
  stays off). In the guest: `auractl clear`, `auractl mark <tag>`, run the
  command, `auractl read <file>`; decode with `tools/auractl/sep_decode.py
  <file> <dir>`. The capture holds device nonces and keys in transit: keep it
  private. `AURA_SEP_MAXB` caps the bytes copied per buffer (128 KiB).
- **T8112 (M2) specifics:** macOS 27.2 on T8112 runs SPTM, TXM, and XNU without
  exclaves. External monitors over HDMI and USB-C (including HDR) work with
  discrete refresh rates; MCDP29xx/2900 HDMI bridges report no VRR support.
