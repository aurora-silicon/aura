# J813 manual CPU-frequency handoff

## Failure model (before implementation)

An inherited marker must not enable control by default. Wrong chip, board, model,
policy type or marker length/value must not expose the CPU device. Exactly the
J813 controller pages must be published, with no fabricated PEP/CPPC support or
measured-frequency claims. Aura initialization may set a boot P-state; its live
Windows path must have no background DVFS writer. Host tooling must not issue
CPU-frequency writes while the guest owns the controllers. The guest driver
serializes requests, validates the public OPP ladder and keeps a conservative
boot-state ceiling until a separate thermal/PEP policy is qualified.

A source-bound handoff replay will exercise actual profile admission and marker
production, compile the actual Mu admission routine, compile the real SSDT, and
check the admitted resource bytes. This is offline protocol evidence; physical
CPU-frequency transitions and Windows PEP policy require separate qualification.

The source audit found the generic proxy CPUFREQ_INIT path can still write
T8142 state during a debug pause. The native personality must therefore latch
CPU ownership during configuration and reject proxy INIT/SET until a fresh
resident reset. Missing/changed firmware OPP mappings must reject admission;
matching chip/board alone is insufficient for a fixed level-to-Hz contract.

## Current implementation and validation

The profile key is `"cpufreq": "j813-manual-v1"`; omission means disabled. It
requires capability bit 11 and sets native configuration bit 5. Admission binds
chip `0x8142`, board `0x24`, model `Mac17,3`, and exact kHz entries in PMGR's
`voltage-states1-sram`/`voltage-states5-sram`. The parser consumes complete
`{u32 kHz, u32 mV}` records. Voltage fields are not used to program voltage.
The 8 E/19 P entries match public Linux commit
`08fdca6772eb6a27be6291bdec0ced7a937a4cf0`,
`arch/arm64/boot/dts/apple/t8142-j813-thermal.dtsi`, including the nonmonotonic
P17/P18 order. The retained real J813 ADT passed this admission offline.

Aura removes inherited markers. Enabled mode emits little-endian u32 one in
`/chosen/aura,windows-cpufreq-v1`. Mu's `J813CPUF` SSDT gate requires that exact
marker plus chip and board. Its `AURA8142` device owns only two 4 KiB pages:
`0x210e20000` and `0x211e20000`. Missing/malformed policy publishes no device.

After native configuration, host proxy CPUFREQ_INIT/SET fail without invoking
their write paths. Reconfiguration cannot revoke the grant, including before
HV initialization. A fresh resident reset is required to return ownership.
The normal Aura personality is unaffected by the Windows-only proxy guards.

Run `python3 tools/windows_cpufreq_e2e.py --mu /path/to/companion/mu --output
/new/receipt-directory` for the source-bound host/AML/native-proxy campaign.
It retains hashes and command logs. Separate production HV lifecycle and
launcher admission campaigns pass. Windows and default Aura builds pass.
These are source/build results; no CPU-frequency hardware transition is claimed.
The manual guest driver and its PEP lifetime/IRQL limitations are documented in
`drivers/AppleCpuFreq/J813-DVFS.md` in the companion Windows driver repository.
