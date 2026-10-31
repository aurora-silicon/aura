# J700 NV2 and the T8140 fixed counters

NV2 remains explicitly selected with `aura-nv2` or `NV2_BOOT=1`. Ordinary
physical-EL1 handoff is unchanged. The monitor exposes VHE-only NV2, keeps
NV/NV2 together, and never sets NV1.

T8140 has working hardware performance counters. Proxy measurements on both
core types found PMUVer=0xf, ten 64-bit Apple counters, and fixed cycles on
PMC0/instructions on PMC1. Programmable event mappings are not established.
Linux integration must not advertise mappings inferred from another Apple CPU.

Two monitor details are required by Linux's existing Apple PMU/KVM support:

* With HACR_EL2[56], a guest PMUv3 access exits with EC=0x3f and its sysreg
  syndrome in AFSR1_EL2. Forward that syndrome into L1's AFSR1_EL2 view.
* L1 writes its guest's PMCR1 filters through the AVNCR-backed PMCR1_EL12
  view at +0x3c0. Load those filters into the physical guest bank at entry and
  restore L1's filters at exit. Redirection alone does not install them.

Bounded proxy tests on both clusters verified a guest PMCR read's syndrome
0x30e419, guest PMCR1=0x30000, and restored L1 PMCR1=0x300. Existing timer and
interrupt probes were also run before Linux. No private reference code was
used to derive these changes.

The companion Linux change registers one fixed-counter T8140 PMU over all six
CPUs. Two separate cluster PMUs caused default KVM backing events to stop when
vCPUs crossed clusters. One common interface fixes this without restricting
vCPU, emulator, or I/O-thread scheduling. Only cycles and instructions are
mapped; unsupported hardware events fail rather than using unverified codes.

Hardware validation on J700 with `7.3.0-nv2-pmu2` included all36 KVM init/run
CPU combinations,100 alternating P/E phases, host counting/sampling on all six
CPUs, and diskless Linux guests with1/2/4/6 vCPUs. Guest counters, overflow
interrupts, intentional UNDEF with x19/x29 preservation, timers and IPIs pass.
Windows11 Pro Arm64 build26300.9457 installed and booted its disk to the desktop,
then repeat-booted with six vCPUs, completed parallel arithmetic, flushed disk
write/readback and three benchmark rounds, and shut down cleanly. All six host
CPUs remained available; no PMU-disable or whole-QEMU affinity workaround was
used. This is not a claim that Windows's emulated TPM is healthy: its System
log reports TPM errors.

Recursive Linux still boots L2 at vEL2 and L3 to userspace. L3 fixed counting,
exceptions and PMU overflow delivery work, but a100k-instruction sampling
period triggers Linux perf throttling and undercounts. A diagnostic with
dynamic throttling disabled in the disposable guests counts20M+ instructions
and200 samples per vCPU without loss. A1M period passes under ordinary policy.
Neither diagnostic changes production policy; high-rate recursive sampling
cost remains an open limitation. Suspend/resume and EFI runtime services also
remain unverified or problematic in the tested configurations.

Test-kernel Image SHA-256:
`9abc0885d874d834ab56488b348bbb3be476e41f6a57643bfda7187d454e993e`.
DTB SHA-256:
`4c3670b5d3e4651e7cb8be483f97d849c8854a4ef5e7bd10b43dba81de844f29`.
Trace-disabled stage2 SHA-256:
`1a6753ea26a2f3e7092f181cd5327db05361c1cc17349e0aed1c5f3232b95cf1`.
Captured output and reproducible probe sources are retained in the Aurora
workspace under `work/aura-macho/neo/capture/continuation-20261031/` and its
adjacent scripts. No synchronous monitor output is used after Linux handoff.
