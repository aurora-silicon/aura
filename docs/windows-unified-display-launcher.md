# Windows launcher: the unified display selection (host side)

The Mu image on codex/j813-mu-quiet-unified (quiet-02-unified, 396a2f8e…) publishes either the legacy
pair of devices (AURA8170 GPU, AURA8180 display) or the unified GPU+panel device (AURA8171), chosen by
one `/chosen` property in the ADT the host hands to the guest (Mu Docs/j813-unified-display-marker.md):
`aura,windows-unified-display-v1`, exactly 4 bytes, little-endian 1. Anything else, absent included,
keeps the legacy pair. This change lets a Windows boot profile ask for it.

## Contract

- Profile key `"unified_display": "j813-v1"`. Absent means off. Any other value, including
  `"disabled"`, any other string, a boolean, a number or null, refuses the profile (the Mu note
  proposes `"disabled"` as an alias of absent; the launcher keeps one spelling for off).
- Present with `"j813-v1"`: the profile's target must be chip 0x8142, model Mac17,3; the live ADT's
  `/chosen` must say chip-id 0x8142 and board-id 0x24 and the model Mac17,3, checked in `setup_adt`
  on the host-only preflight copy, which runs before the launcher writes anything to the target.
  Otherwise the launch refuses there.
- `setup_adt` removes any inherited copy of the property first, then writes it only when selected,
  next to `aura,windows-cpufreq-v1` and the same way (4-byte Bytes type).
- No resident C change, no capability bit, no re-enrolment: the property is ADT data the host already
  owns and the resident passes through.

## Failure model (written before the code)

1. A profile without the key changes the guest ADT. With the key absent the ADT the host builds
   must be byte-identical to the one 2e3a4e6b builds from the same input, for the default, the
   cpufreq and the cpufreq-plus-E-state cases.
2. An inherited marker survives: an input ADT that already carries the property (any value) must
   lose it when the key is absent, or have it replaced by exactly 01 00 00 00 when selected.
3. The marker is written on the wrong machine: chip other than 0x8142, board other than 0x24, a
   missing board-id, or a model other than Mac17,3 must refuse, never write.
4. A malformed selection is accepted: values other than the exact string, wrong types, or `"disabled"`
   must refuse at profile admission, before any device access.
5. Refusal after the target was touched: the board check needs the live ADT, so it must run in the
   host-only preflight `setup_adt`, which the launcher calls before `bootstrap_port` and the proxy
   heap reservation; the real `setup_adt` runs it again.
6. The marker has the wrong bytes, type or place: exactly 4 bytes 01 00 00 00 in `/chosen`, nothing
   else added, nothing else changed.
7. Other policies change: cpufreq and ecpu_boot_state admission, their markers and their errors stay
   as they are; the existing admission cases still pass.
8. The plan does not reach the second `setup_adt`: both the preflight HV and the launch HV must
   receive the selection.
9. The boot cannot bind: the launcher's host checks (host commit, host tree, host sources, freeze
   manifest source tree and tracked sources) must still be satisfiable for a boot that uses this
   tree; how is recorded in the report, not by loosening a check.

## Replay

`proxyclient/tools/windows_unified_display_e2e.py --out DIR` (host only, no hardware) runs this
tree and 2e3a4e6b side by side in separate processes on synthetic ADTs and covers 1 to 8: byte
equality with the key absent, the exact added property, inherited copies, every refusal, the
existing admission of boot 148's profile, and the real `HardwareSession.prepare()` over a model
transport (a wrong board refuses before `bootstrap_port`; both HVs receive the selection). It then
rejects mutations of the change and writes `DIR/result.json`.
