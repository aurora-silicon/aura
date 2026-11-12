# Signed payloads and the boot record

aura can check signatures on what it boots and tells the OS what it checked.
Both are build options; a default build only writes the record.

## Build

- `SECBOOT=audit`: check every signature, record the result, boot anyway.
- `SECBOOT=enforce`: also refuse anything that is not verified: payloads
  without a valid `aura_sig` record, a next stage without a valid trailer,
  and `boot=` files on the ESP (they cannot carry a signature).
- `SECBOOT_PUBKEY=<file>`: the raw 32-byte Ed25519 public key built into the
  stage. The default, `data/secboot/test-ed25519.pub`, is a published test
  key (its private half is in the same directory), so a build with it proves
  nothing about who signed an image.
- `SECBOOT_MIN_SECURITY_VERSION` (a `-D` define): the lowest security version
  a signature may carry. Signatures below it are refused as rollbacks.
- `SECBOOT_FLAVOUR=dev|secure` (default `dev`): `dev` keeps the USB host
  windows a lab needs (the recovery window after a failed boot, `aura-proxy`,
  DebugUSB) while the stage does not enforce. `secure` never gives a USB host
  the proxy, whatever the policy: no window, and a failed hand-off or a boot
  with no payload reboots instead of falling back to the proxy. `secure` needs
  `SECBOOT=audit` or `enforce` and refuses `DEBUGUSB_PROXY=1`.

The usual pair is a `secure` Stage 1 built with `SECBOOT=audit` for users and a
`dev` one for development Macs. Both report and boot by default; enforcing is
chosen when the key block is filled (below), so one build serves both.

## Keys in a Stage 1 image

A Stage 1 built with `SECBOOT=audit` or `enforce` also carries an
`AURORA-S1-KEY01` block: up to four Ed25519 public keys and a minimum
security version, filled before `kmutil` enrols the image, so the
LocalPolicy's `coih` covers them like the rest of Stage 1:
`secboot_sign.py fill-keys --pub aurora.pub [--pub owner.pub] --min-secver N [--enforce] in out`.
A record is checked with the key whose id it names. An unfilled block leaves
the built-in key in charge; a damaged one (bad CRC, unknown flags) is reported
and ignored.

- Keys: the distribution's release key by default; an owner who wants to sign
  their own images puts their key in instead of, or next to, it.
- `--min-secver`: the rollback floor. It lives only here, so it moves only when
  a new Stage 1 is enrolled; signatures below it are refused as rollbacks in
  every policy.
- `--enforce`: an `audit` Stage 1 then refuses what it cannot verify, as an
  `enforce` build does. Without it the Stage 1 checks, records and boots.

The record carries the policy that was in force (`policy`), the Stage 1
flavour (`stage1-flavour`, passed on as `chosen.aurora,stage1-flavour`) and
this stage's own (`flavour`).

While a stage enforces, or in the `secure` flavour, no aura stage gives a USB
host the proxy: not the configured or `aura-proxy` window, not the recovery
window after a failed boot, not the DebugUSB window, and not the fallbacks when
no payload starts or a hand-off fails (it leaves a message on the panel and
reboots).

## Signature forms

Both are made and checked with `tools/secboot_sign.py`.

- Payload record, for the stage that runs the payloads: an `aura_sig` record
  right after the aura head covers every byte after it (DTBs, U-Boot or a
  kernel, initramfs, config lines). The stage checks it before it parses
  anything else. Under `enforce`, it then reads nothing after the covered
  bytes except the lines an earlier aura stage appends (`chosen.aurora,*`,
  the ESP UUID, NVMe hand-over). Older stages skip the record.
  `secboot_sign.py sign-payloads --key K --head-len <aura head size> in out`
- Image trailer, for the stage that chainloads the image: an `aura_sig`
  record and a 16-byte footer (`u64` record offset, `AURSIGT1`) at the end of
  the image cover every byte before the record. The loading stage checks it,
  cuts it off and starts the rest. An aura-try trial image is checked the
  same way. `secboot_sign.py sign-image --key K in out`

The record body is `AURSIG01`, version, algorithm (1 = Ed25519), security
version, flags, covered length and an 8-byte key id (SHA-256 of the public
key), then an Ed25519 signature over `AURA-SIG-V1` (padded to 16 bytes), the
header, and SHA-256 of the covered bytes. The tool refuses to sign an image
that carries `chosen.aurora,measure-` lines, which only a loading stage may
add. In an aura image it searches only what follows the image (from the load
header's footprint, where `_payload_start` is), since aura's own code holds
the line's format string.

## The record

Every stage measures (SHA-256) what it hands over: the selected DTB, the
kernel or U-Boot payload, ESP `boot=` files and the next stage. A loading
stage passes its entry to the next as `chosen.aurora,measure-N`. The stage
that builds the OS device tree writes `/chosen/aurora,boot-security`:

- `compatible = "aurora,boot-security-v1"`; `stage`, `policy` of that stage;
- `state`: `verified` when every stage enforced and verified the next,
  `partial` when this stage verified what it hands over but an earlier link
  was not verified, `unverified` otherwise;
- `apple,lp-*`: the LocalPolicy facts iBoot reports in ADT `/chosen/asmb`
  (`lp-coih` is the SHA-384 of the Stage 1 Image4 manifest the policy pins),
  and `apple,secure-boot`, `apple,effective-security-mode-ap` and related
  `/chosen` properties, copied as they are;
- `measurement@N`: `stage`, `policy`, `component`, `verdict` (`verified`,
  `unsigned`, `measured`, `bad-signature`, `unknown-key`, `rollback`,
  `bad-format`), `sha256`, and for signed items `key-id` and
  `security-version`.

U-Boot (`EFI_AURORA_MEASURE`) adds an entry for every EFI image it loads.
The record is only as good as the first stage that enforces: a stage nobody
verified can write anything here.
