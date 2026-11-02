# j616s-sptm: Linux under the resident SPTM on the J616s (WIP)

Omarchy (Arch Linux ARM) running on a MacBook Pro M4 Pro (Mac16,7, J616s, T6040)
**with Apple's SPTM resident**, so the CPU's hardware TSO memory-ordering mode
can be switched on for Linux processes (FEX, x86 emulation). The ROVER path in
`docs/macho-boot-object.md` boots with `kcbz=0` and no SPTM. This path keeps
SPTM and puts a small EL2 monitor where XNU would run.

This is work in progress. We are asking for help.

## How it works

```
iBoot ─► SPTM (resident, Apple's, untouched) ─► BootKC entry ─► arm_init+0x88 ─► monitor (EL2, ours)
                                                                                 └─► Linux guest at EL1
```

- `tools/kc_patch.py` takes **your own** BootKC (from your Mac or Apple's IPSW
  for it) and writes into it: the monitor (`monitor/`), three branch hooks
  (`arm_init`, the secondary warm start, SPTM's panic reflect), and the Linux
  Image, guest DT and initrd as payload. Everything else in the kernelcache
  stays byte for byte as Apple shipped it. The result is enrolled with Apple's
  own `kmutil configure-boot` (without `--raw`) under Permissive Security.
- The monitor runs under SPTM as its XNU client. It builds the guest's stage 2
  through SPTM, forwards the guest's APIF calls (NVMe table 6, DART tables 5, 6
  and 7, UAT) to SPTM unchanged, and owns the WDT, the vGIC CPU interface,
  PSCI/SMP and the framebuffer console.
- The guest kernel is aurora-silicon/linux `integration/aurora` plus
  `linux/00*.patch`: `kvm-arm.mode=sptm`, `apple,dart-apif`, NVMe over APIF,
  `pcie-macho-init`, asahi GPU with a UAT backend over table 7, USB gadget.

## Status on hardware (2026-11-02)

| What | Result | Evidence |
|---|---|---|
| Omarchy desktop under the resident SPTM, 14 CPUs, Wi-Fi, USB ACM/EEM, NVMe root | runs | HW, boots of 2026-11-02 |
| **HW TSO** | `tso_litmus` cpu1 (E) × cpu9 (P), 40 M iterations: TSO off 15 MP reorderings, TSO bit on **0**, XNU's ACTLR state **0** | HW |
| Silent freezes after 60 to 330 s | the guest HVC `0x7505` path mapped a DART error page from the wrong state (SPTM panic). Fixed in F37 (`monitor/guest.c`); F37 ran 9 min without a freeze, a rarer freeze is still being chased. | HW |
| Left USB-C ports (xHCI) | IRQ 0, `hub_event` stuck in `xhci_alloc_dev`, blocks power-off. Disabled in the DT. | HW, open |
| Power-off | through SMC (`diag/`, WIP) | partly |
| DCP / external display, audio, camera, Bluetooth | not done or not verified | |
| KVM guests (`kvm-arm.mode=sptm`), FEX with HW TSO inside a muvm VM | runs on F37 (litmus in the VM: 0 reorderings with TSO) | HW |

The hardware-tested candidate is **F37** (boot object SHA-256 `4a90f7f0…`).
This tree is its source plus the F38 changes that are built but not yet
flashed: the KVM `KVM_MR_DELETE` fix (`linux/0019`), the left USB-C ports
held until their DART is bound (`initrd/initrd-init`, `guest/guest-usb01.dtsi`).
`tools/kc_patch.py` with the 26.7.1 25G241 `kernelcache.release.mac16j` from
Apple's IPSW and F37's monitor, Image, DT and initrd reproduces F37's boot
object byte for byte; the guest DT built here with your generated
`guest-platform.dtsi` is identical to F37's.

## Where we need help

1. Run F37 longer than 330 s and confirm the freeze is gone, or catch the next one.
2. Left USB-C xHCI under the monitor: why IRQ 0 (live ADT remaps SID 0→1 at DART init).
3. DCP and external displays under SPTM (table 7 path, `-DP_DCP`).
4. Other M4 boards: offsets in `tools/kc_patch.py` are for one BootKC build only.
5. A synthesized fileset with `kcbz > 0` (as `tools/macho_boot_object.py` does
   for `kcbz=0`), so no Apple kernelcache is needed at all.
6. Review of the monitor ↔ SPTM ABI use in `monitor/mm.c`, `iommu.c`, `apif.c`.

## Build

You need a J616s with macOS 26.7.1 (25G241) on its own APFS container, a
working raw-boot Linux on it (aurora kernel with m4-loader), clang/lld,
`dtc`, `lzfse`, Python 3, a static `busybox`, `wpa_supplicant` and `openssh`.

```sh
# 1. kernel: aurora-silicon/linux at 50ba71c552f1
git -C linux-src am ../j616s-sptm/linux/00{01..18}*.patch
git -C linux-src apply ../j616s-sptm/linux/0019-*.diff ../j616s-sptm/linux/0020-*.diff
j616s-sptm/linux/build-kernel.sh linux-src build-macho

# 2. per-unit values (Wi-Fi/BT calibration, BT address, OPPs) from the running raw-boot DT
python3 j616s-sptm/tools/gen-guest-platform.py j616s-sptm/guest/guest-platform.dtsi

# 3. optional initrd network: ~/.config/macho-initrd/wpa.conf and authorized_keys

# 4. your own kernelcache: from your Mac, or from Apple's IPSW for Mac16,7 26.7.1 (25G241)
#    (kernelcache.release.mac16j inside the IPSW zip)
j616s-sptm/build.sh kernelcache.release.mac16j build-macho/arch/arm64/boot/Image build-macho out
```

`out/rk.macho` contains Apple's code. Keep it on your machine.

## Install and roll back

From One True Recovery, as the owner of the Mac:

```sh
bputil -nc -v <volume group UUID>                        # Permissive Security for that macOS install
kmutil configure-boot -c rk.macho -v /Volumes/<volume>   # no --raw: Mach-O boot object
```

Roll back by enrolling your previous boot object (for example m4-loader with
`--raw`) the same way. Keep a second machine and your normal raw-boot entry
ready. With `macho.rw` the root file system is writable; NVMe writes are
limited to the ranges the boot DT publishes.

`initrd/initrd-init` and `guest/guest-usb.dtsi` still carry the author's
setup (user `morpheus`, hosts `hypnos` and `fefe` as log collectors, LAN
addresses). Adapt them.

## Layout

| Path | What |
|---|---|
| `monitor/` | EL2 monitor (C, asm); `vgic_cpuif.c` is generated from Linux `vgic-v3-sr.c` by `gen-vgic-cpuif.sh` |
| `tools/kc_patch.py` | writes the monitor and payload into your own BootKC, checks its SHA-256 |
| `tools/gen-guest-platform.py`, `guest-platform.dtsi.in` | per-unit DT values from your running DT |
| `tools/check-io.py` | checks monitor device pages against SPTM's io-range tables (optional, you dump them yourself) |
| `guest/` | guest DT |
| `initrd/` | init, sequencer, TSO litmus and probes, initrd builder |
| `diag/` | diagnostic modules (ADT, WDT, IPI, TSO knob) |
| `linux/` | kernel patches against aurora-silicon/linux `50ba71c552f1` (0010 to 0018 are aurora commits by Dj and Ryan Murray carried for the base, 0019 and 0020 our WIP), config |

See [LEGAL.md](LEGAL.md) for what this repository contains and what it never contains.
