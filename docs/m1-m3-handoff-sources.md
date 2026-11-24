# M1–M3 handoff integration sources

This series starts at omacom/m1n1 integration/aurora
`d05ba17dfb771871d608225be42968bbb1e75a18`, with Scott Jones's existing
PR #4 (`bd5a2348806923e4139bc300f05b8df887bc06d3`) as its prerequisite.
The ISP firmware support is inherited from that PR rather than submitted a
second time. This series carries public source only.
The original authors and their supplied sign-offs remain on the carry commits.
No sign-off was added for an author who did not supply one.

## Source and license record

Every file in this series is available under m1n1's MIT license (`LICENSE`);
no existing file changes license. The T6031 device-tree test fixture is
`GPL-2.0+ OR MIT`, as kernel device trees are. The SiliconForest revisions
below are Chris Kearney's own work, which SiliconForest's GPL-2.0-or-later tree
carried; he contributes them here under MIT. No Asahi Linux GPL change is
carried.

| Contribution | Public source revision | Source terms / affected portions |
| --- | --- | --- |
| T6031 display/PMP facts, GPU identity, host tests | omacom/m1n1 `4b9dec74`, `ca47d588`, `03b72c71`, `a2b3a4ad` | MIT. Existing Max branch carried with authorship; it was absent from integration/aurora. |
| Locked DART walk | aurora-silicon/m1n1 `93602f0f3cb7e2825e58460ba452a5a4f3ef03c8` | MIT, explicit source-file header. |
| J514S panel and external-controller handoff | aurora-silicon/m1n1 `3f9f34d6f0cf8e85e247a5f8f9468b4457e63f0f`, `bf43e1e7ea265adbd8d39ec3f44fbc36b242b790` | MIT, explicit source-file headers. |
| J514S HDMI PHY calibration | aurora-silicon/m1n1 `f0623eaed55e612636520926db0ae75029443cf5` | MIT, source repository LICENSE; its kboot_atc.c has no SPDX header at that source. |
| T6030 SIO protocol-10 records | aurora-silicon/m1n1 `97820568035b8d7035c68e4126f203d86c76abe4` | MIT, explicit source-file header. |
| Atomic native handoff and exact Max GPU opt-in | siliconforest-m1n1 `654e1f68ced800f570a27c5b14a25509b715a0bf` | Chris Kearney's work, contributed here under MIT: native-display transactions and related bounds in src/kboot.c; Max intent admission in src/kboot_gpu.c; corresponding Python test. The mixed original commit's HV/SMP changes are excluded. |
| J514S window/identity hardening | siliconforest-m1n1 `b86594b7726de5b650ae2b707b5bb5fdee246dae` | Chris Kearney's work, contributed here under MIT: DART SID bound in src/dart.c; register/UUID checks in src/kboot.c; Python regression test. |
| T8122 26.6.2 ISP heap | omacom/m1n1 `bd5a2348806923e4139bc300f05b8df887bc06d3` (carried as SF `61999909`) | MIT at source; Scott Jones's authorship and supplied sign-off/coauthor retained. |
| ISP segment-list hardening | siliconforest-m1n1 `2f6117a3b2450c1cc3befb7afa9fcdecb5a0cf40` | Chris Kearney's work, contributed here under MIT: src/isp.c and its Python test. |
| T8122 external-firmware marker | GehrmannSolutions/omarchy-m3-air `5f0d61514cf2bf2ec598850bcca4e007c0c1f083`, boot/m1n1-aurora-t8122-dcpext.patch | MIT; Marius Gehrmann's additional notice is retained in 3rdparty_licenses/LICENSE.Marius-Gehrmann. No supplied Signed-off-by; none invented. |
| Transactional T8122 marker and ASC-list validation | siliconforest-m1n1 `1ecaa8c59d55e0b51275f6ebc063808c2c70bd21` | Chris Kearney's work, contributed here under MIT: src/kboot.c and its Python regression tests. |
| DART host-test extraction boundary | siliconforest-m1n1 `04fe540ef7fa58404145eb28ec40f1af0906d21e` | Chris Kearney's work, contributed here under MIT: the test fixture correction only. |
| T600x route admission and legacy fallback | siliconforest-m1n1 `24a133ffae06d2b8598173329953ba802a82ca46`, `99817310f70204e3dc469c3ec0c53f94a008a292`, `a28c2231045abc6695e6d8302bcdb5100805d202` | Chris Kearney's adaptation, contributed here under MIT, of Andreas Fernandez's (Ante, GitHub @Ante042) MIT proposal https://github.com/aurora-silicon/m1n1/pull/33: src/kboot.c and its Python regression tests. |

Repository links for these source revisions are
[Omacom](https://github.com/omacom/m1n1),
[Aurora Silicon](https://github.com/aurora-silicon/m1n1),
[SiliconForest](https://github.com/iconidentify/siliconforest-m1n1), and
[Marius's public patch](https://github.com/GehrmannSolutions/omarchy-m3-air/blob/5f0d61514cf2bf2ec598850bcca4e007c0c1f083/boot/m1n1-aurora-t8122-dcpext.patch).

## Admission and compatibility

* M1/M2 retain their existing paths. T600x transitions only an exact
  `disabled\0` route status, together with its owner on a scratch FDT.
  Missing, enabled, reserved, failed and malformed route statuses are preserved.
* J514S native publication requires its matched DT's `apple,j514s-native-piodma`.
  The panel and each external-controller transaction are atomic. The optional
  native path is not exercised by a J516S boot using the legacy handoff.
* T6031 GPU probing requires the exact chosen string `asahi,t6031-gpu=1`.
  On J514C and J516C, `dt_set_m3_defaults()` sets it unless a config line set it.
  `chosen.asahi,t6031-gpu=0` keeps the handoff off. `chosen.asahi,m3-defaults=0`
  prevents the default and keeps the owner's explicit lines as written, an
  owner's `asahi,t6031-gpu=1` included. After a lost config line no T6031 line
  is passed on, the owner's `=1` included. No manual enable line is needed on
  either supported board. This
  does not qualify stand-in power data or widen the kernel's board gate.
* The T6031 display handoff requires the exact chosen string
  `asahi,t6031-dcp=1`. On J514C and J516C, `dt_set_m3_defaults()` sets it unless a
  config line set it, beside the GPU default, with the same `=0`,
  `m3-defaults=0` and lost-line rules. Each switch stands alone: `t6031-gpu=0`
  keeps the GPU off and still gets the display default, and `t6031-dcp=0` the
  other way round. No manual enable line is needed. Whether the kernel takes the
  display over is the kernel's decision; one that does not leaves the boot
  framebuffer. Its `__OS_LOG` reservations are made on every J514C or J516C
  boot without it.
* The T8122 external marker is u32 equal to 1, published last after reservations
  and firmware version. Suppliers and routes stay disabled for the kernel gate.
  The known external-display kernel contract requires firmware-compat 14,7,0.
  Camera support on 26.6.2 is not external-display support on that firmware.
* J613 DTBs at kernel `17a9930f20a0895b5980385b0b2d67edda8a24be` or later
  require a kernel at that pairing floor or later; rollback kernel and DTBs
  together. Source/build/host tests alone do not qualify a machine or firmware.

## Deliberate exclusions

No logo, branding, policy-file, README, stage-1 or firmware updates are part
of this series. The existing upstream test matrix now also triggers on both
integration branches, with read-only repository permissions. A common FDT
memory-map fix uses alignment-safe reads; its regression exercises the actual
function on 4-byte- and 8-byte-aligned properties with ASan/UBSan. The broad Asahi source/GPL transition and hypervisor
sync are not carried. In particular `80954151`'s unsupported-firmware display
replacement must not be applied wholesale when combining the pending Neo work:
Neo has native display paths for firmware outside that older display whitelist.
The independent Neo and pending upstream PR5/PR6 changes need their own
integration review; this series does not replace those branches.

## Submission dependencies and disclosure

This proposal depends on omacom/m1n1 #4 for Scott's ISP case and #3 for the
repository's pending AI-assisted contribution-policy clarification. Its source
comparison and adaptations were developed and adversarially reviewed with
Codex, Claude and Grok assistance, disclosed in the commit trailers and PR.
The original T600x proposal author is Andreas Fernandez (GitHub @Ante042,
display name Ante); his original commit and supplied sign-off are the source
record, and this adaptation does not claim he signed off the additional code.
