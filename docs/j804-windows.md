# J804 (T8152 / MacBook Pro 14-inch, M6) Windows carrier

This branch (`j804-windows`, based on `j813-windows`) carries the T8152/J804
Windows EL2 carrier on top of Aurora Silicon's Windows personality. Detailed
per-subsystem contracts live in [windows-t8152.md](windows-t8152.md).

## Scope

- **J804 boot support**: Stage 1/2 board admission, USB0 warm takeover, SMP,
  Gen3 DART quiescing, optional DCP/AOP/ISP/AVE/ACIO handoff, and the
  `aura-try` no-trial guard on T8152.
- **T8152 Windows carrier (`WINDOWS=1`)**:
  - strict stage-2 allowlist policy, guest RAM window, 12-live-CPU admission,
    trapped guest WFE, architectural `ACTLR_EL12`, software GICv3 CPU interface
    (every `ICC_*` trapped), and absent-PMUv3 UNDEF recovery;
  - carrier-emulated AHCI storage (RAM store and internal NVMe partition),
    USB-C xHCI passthrough (`drd1`/`drd3`), HID input service (host feed and
    internal T8152 MTP keyboard/trackpad), mediated G18G GPU forwarding (G0/G1/G2
    with per-doorbell UAT audit), G6-S scanout service (virtual 60 Hz vblank and
    fenced flips), and guest SError attribution;
  - per-CPU EL2 time accounting (`HV: el2 cpuN ...`, 1 Hz) and interrupt-aware
    idle scanout/framebuffer stepping.

## Resident ABI on `j813-windows`

To avoid colliding with `j813-windows`' own proxy opcodes, configure flags and
`P_HV_WINDOWS_CAPS` bits:

- `P_HV_WINDOWS_CAPS` stays at `0x4157494e00017fff`.
- The five T8152 carrier rule bits live in `P_HV_WINDOWS_CAPS2` bits 8–12
  (feature bits 0–7 remain AHCI, USB, GPU, input, GPU probe, AHCI NVMe, GPU G2,
  scanout).
- T8152 configure flags use `BIT(16)`–`BIT(22)`.
- `P_HV_WINDOWS_GUEST_RAM`, `P_HV_WINDOWS_CAPS2` and `P_HV_WINDOWS_AHCI` use
  `0xc8c`, `0xc8d` and `0xc8e`; USB/GPU/input/tick-CPU/scanout keep
  `0xc87`–`0xc8b`.

## Clean-room policy and private inputs (`J804_PRIVATE_DATA`)

No Apple firmware, kext code, hardware-derived binary recipes or machine-specific
identifiers are committed in this repository. Build-time and run-time inputs
lifted from the user's own macOS installation are supplied out-of-tree via
`J804_PRIVATE_DATA`:

- `aura/g18g-j804-gpu-profile.h` — optional J804 G18G firmware admission entry
  for `kboot_gpu_t8152.c`. Without it, no J804 G18G entry is built.
- `aura/g18g-j804-gpu-power.h` — optional GPU PSD power-on recipe for
  `gpu_t8152_power_recipe.c`. Without it, GPU power-on is disabled.
- `aura/g18g-j804-gpu-g2.h` — optional G2 boot-control and firmware window
  recipe for `gpu_t8152_g2_recipe.c`. Without it, G2 is disabled.
- `firmware/tpmtfw-j804.bin` — trackpad firmware loaded at launch time by
  `run_guest.py --windows-input-tp-fw`; never stored in git.

Register-range tables in `gpu_t8152_admit.c`, `gpu_t8152_probe.c` and
`gpu_t8152_power.c` document hardware register-access observations only.

## Licensing and third-party notices

Per [licensing.md](licensing.md):

- All new J804/T8152 carrier source files, tools and tests added on this branch
  are licensed under `GPL-2.0-only` (see [LICENSES/GPL-2.0-only.txt](../LICENSES/GPL-2.0-only.txt))
  with `Signed-off-by` commit trailers.
- Inherited m1n1/aura files retain their original `MIT` or `MIT AND GPL-2.0-only`
  terms.
- Inherited Windows hypervisor files retain their upstream notices:
  - `src/hv/windows/hv_psci.c` — based on Arm Trusted Firmware-A (TF-A),
    `BSD-3-Clause` / `MIT OR BSD-3-Clause`.
  - `src/hv/windows/hv_vgic.c` — inspired by the Linux KVM ARM vGIC driver
    (`GPL-2.0-only`), with original `BSD-2-Clause-Patent OR MIT` header preserved.

## Building and host verification

Windows carrier image (requires Clang/LLD):

```sh
gmake USE_CLANG=1 WINDOWS=1 FILE_LIMIT=0x200000 J804_PRIVATE_DATA=/path/to/private-data
```

Stage 1 / Stage 2 boot objects:

```sh
J804_PRIVATE_DATA=/path/to/private-data tools/j804-build-aura.sh /tmp/j804-aura-out
```

Host tests:

```sh
pytest tests/python/test_windows_t8152*.py
python3 tests/windows_el2_acct/run.py --output /tmp/el2-acct-out
python3 tests/windows_pmu_assist/run.py --out /tmp/pmu-assist-out --expect-fast
```
