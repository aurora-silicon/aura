# Windows guest personality

This work adds an explicit `WINDOWS=1` build of Aura for the
Aura -> Project Mu -> Windows boot chain. The default Aura build retains its
existing hypervisor. A Windows build is not a claim that every Mac or peripheral
is supported: firmware, HAL, memory layout and device handoffs must match the
selected machine profile and pass physical acceptance.

## Failure model (before implementation)

- A default build must not acquire Windows register emulation, peripheral
  ownership, or an incompatible proxy ABI. Both build personalities must build from
  their own captured source trees.
- A launcher must reject an unrecognized or ordinary Aura resident before
  allocating guest memory or programming hardware. A positive versioned
  capability handshake is required; a matching executable filename is not proof.
- A firmware profile must bind the actual chip/model, firmware hash, entry
  offset, guest alignment and interrupt contract. Wrong-model firmware, changed
  files, truncated images and an entry outside the image must fail before launch.
- Mu must be loaded at 64-KiB alignment with valid boot arguments and an ADT
  copy. No guessed physical addresses or replacement of an installed boot object.
- Public Windows reference code must come from an immutable public commit,
  with original license notices retained and source hashes recorded. Dirty local
  reference files and proprietary code are excluded.
- Secondary CPU setup, start, watchdog and join errors must propagate. An
  acknowledgement timeout is not cancellation; pending dispatch storage must
  remain valid until the CPU is joined. A failed join forbids another guest.
- Native AIC timer/IPI delivery, early GIC discovery, PMU compatibility and
  orderly SYSTEM_OFF are distinct hardware gates. A successful link or UEFI
  banner does not establish Windows readiness.
- No profile may silently enable unqualified peripheral resets. N1's shared
  bridge and DART ownership require the matching exclusive Mu/Windows contract.

## Reference

The Windows personality is adapted from public
`aurora-silicon/m1n1` commit `69534d4899e20108d2c7a085565a7097509fa17f`.
Its imported-file manifest records original blob hashes and licenses. The
existing Windows lineage is a reference, not evidence that the port works on
the current Aura base. Hardware acceptance and cross-personality regressions
are required before merge.

## Build and current boundaries

Build with GNU make, Clang/LLD and the Rust target listed in the main build
instructions: `gmake WINDOWS=1`. The Windows build uses size optimization and
LTO, and retains the normal image/DCP overlap assertion. An ordinary build
selects the original hypervisor. Windows requires a Mac CPU with the current
Aura Fast-IPI capability and an initialized AIC; unsupported prerequisites
are rejected before peripheral handoff.

The guest lifecycle is deliberately one-shot per loaded Aura image. Reload
Aura before another guest, including after a failed device handoff. Guest
SYSTEM_OFF and SYSTEM_RESET drain guest CPUs and return status 1 or 2 to the
host proxy; they do not power off or restart the physical Mac. Unsupported
CPU_OFF, CPU_SUSPEND and memory-protection calls return NOT_SUPPORTED.

Use the explicit launcher and a matching Mu image described in
[windows-launcher.md](windows-launcher.md). USB is withheld pending its own
ownership backend; Mu must implement the matching no-USB ACPI gate. N1 is
optional and requires [the exclusive handoff](windows-n1.md). Neither a
successful build nor a microguest qualifies Windows, N1 DMA/reset, other
peripherals, or other Mac models.

## J813 hardware observations

The [2026-11-06 observation record](validation/windows-j813-20261106.json)
binds the tested resident images and probe artifacts. Windows on internal NVMe
reached `winlogon.exe`, `LogonUI.exe` and `dwm.exe` with MTP and N1 withheld;
an early capture now shows the Windows logo and spinner, followed by an opaque
black framebuffer at the later logon-stage observation. The display adapter and
monitor report Started/D0 with no PnP problem, but interactive login is unverified.
The AP-fault probe separately verified that a fault on CPU 0 drains all guest
CPUs while the boot CPU is CPU 6 and returns control to the host.

The CDC stress probe passed a sixty-second reader pause, a full TX queue,
closed-DTR discard, exact receive/reopen sequences and clean SYSTEM_OFF.
Its guest timing also verifies the T8142 counter conversion. This qualifies
the tested virtual UART transport behavior, not Windows kernel debugging.

The retained CDC text reaches Mu ExitBootServices. Windows logon-stage progress
was verified separately from memory; a continuous Windows kernel log or KD
session is not established. N1 cold admission rejects the inherited active
PCIe power state before reset or power changes; functional N1 support remains
unverified.

With MTP preparation enabled, Aura completed the seven handoff phases and Mu
published MTP0. Windows then started `AppleMtpHid`, its HID keyboard and
mouse/trackpad stacks, all in D0 without reported PnP problems. Logon processes
also started, but the later framebuffer remained black. Physical key/pointer
input and interactive login have not yet been verified.
