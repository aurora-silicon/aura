# J813 CPU ownership and N1 acquisition integration

## Failure model written before integration

Combine qualified CPU handoff `8758b165` with reviewed N1 acquisition `8b5264f2` in an
isolated source tree. Preserve the known-good Mu image and CPU/manual, MTP, CDC,
BGRA framebuffer, compatibility and reservation profile fields. Only the N1
profile changes to j813-acquire-v1. No G17 or firmware change is included.

- Capability `0x0800` means CPU ownership; `0x1000` means N1 acquisition. The combined
  resident must advertise both and retain the existing low capability bits.
  Missing either bit, or required MTP/display/CDC/configuration/reservation
  support, must reject the complete profile before target mutation.
- The native CPU configuration/ownership and legacy CPU initialization block
  must remain exactly the qualified implementation. N1 opcode `0xc84` cannot
  overlap CPU, heap or PSCI operations. Cold N1 behavior remains unchanged.
- CPU/manual and MTP markers, shadow framebuffer setup and N1 NW1H publication
  must coexist in the actual HV lifecycle. Missing CPU OPP data or rejected N1
  acquisition stops before guest loading/entry. No fallback to disabled N1,
  repeated acquisition or ordinary CPU initialization is permitted.
- Whole-N1 acquisition must retain its complete scope proof, guarded reset
  sequence, one-attempt/quarantine behavior, all-stream clean final state and
  exact descriptor bytes. CPU integration must not weaken these checks.
- Preserve the same hash-bound Mu bytes and every existing profile field except
  n1; verify the derived profile without connecting to hardware. CPU table
  admission must still pass against the unchanged qualified public Mu source.
- Build Windows and ordinary personalities separately from a committed source
  revision, preserving both images and complete replay receipts. Hardware
  diagnostic success is separate from combined-image/Windows qualification.

Replay the actual producer, CPU native/Mu gate, launcher and HV paths. Capture
source hashes, compiled binaries, traces, descriptor/profile bytes and build
commands in a repeatable frozen artifact. No hardware, push or merge here.

## Integrated source contract

This tree merges CPU source `8758b165` and the full N1 source history through
8b5264f2, including the prerequisite full-link-before-DART correction. The
only production merge conflict was the capability word; it is resolved to
`0x4157494e00011fff`, which advertises both CPU `0x0800` and N1 `0x1000`. CPU ownership,
MTP, framebuffer and N1 implementations retain their source behavior.

`windows_cpu_n1_e2e.py` covers the complete combined launcher profile and each
required capability rejection before mutation, using modeled endpoint and CDC
I/O boundaries. `windows_hv_e2e.py` additionally checks the serialized guest ADT
contains the CPU ownership marker, MTP preservation and NW1H descriptor while
BGRA/CDC/CPU/MTP configuration flags equal 59. A native N1 rejection prevents
payload upload and cannot be bypassed by changing its mode to disabled. Wrong
CPU OPP input stops after the read-only capability request.

The runtime profile artifact is derived from the existing CPU efficiency
profile by changing only `n1` to `j813-acquire-v1`. Mu remains source `35d5d4ac`
(code `113c98c3`) and image SHA-256
`de09007eec9031a1d788da7d63b865307f03973f38805c027a45c1054b0160fd`.
No firmware source or bytes are copied into this public repository.

Replay commands (each output must be a new directory):

```
python3 proxyclient/tools/windows_cpu_n1_e2e.py --output NEW_COMBINED_DIRECTORY
python3 proxyclient/tools/windows_launcher_e2e.py --out NEW_LAUNCHER_DIRECTORY
python3 proxyclient/tools/windows_hv_e2e.py --out NEW_HV_DIRECTORY
python3 tools/windows_n1_e2e.py --output NEW_N1_DIRECTORY
python3 tools/windows_cpufreq_e2e.py --mu QUALIFIED_PUBLIC_MU_SOURCE --output NEW_CPU_DIRECTORY
```

These checks establish source/model/build compatibility. The combined native
image still requires its own physical acquisition and Windows qualification.
