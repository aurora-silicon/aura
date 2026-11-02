# What is in here, and what is not

This directory is independent free software (GPL-2.0-only, as the rest of
Aurora's contributions) written to make Linux interoperate with Apple Silicon
firmware on a Mac its user owns. It is not affiliated with or endorsed by
Apple.

## Never in this repository

- No Apple binaries or parts of them: no kernelcache, SPTM, TXM, iBoot, ADT,
  firmware images, and no byte sequences copied out of them.
- No Apple source text in any form. Apple's open-source XNU (APSL 2.0) is a
  reference for facts only, nothing is copied from it.
- No signing keys, no personalization data, nothing that bypasses Apple's
  boot security. Installation uses Apple's own Permissive Security setting and
  `kmutil configure-boot`, done by the owner in One True Recovery with their own
  credentials, exactly as for any other custom kernel on Apple Silicon.
- No per-unit calibration data, keys or network credentials.

## What is here

- Our own code: the monitor, kernel patches, device tree, init, tools.
- Interface facts needed for interoperability: SPTM call numbers and frame
  types, register addresses, and the file offsets inside one specific
  kernelcache build where the patcher writes.
- Third-party free software with its notices: parts of Linux (`vgic-v3-sr.c`,
  `font_8x16.c`, both GPL-2.0).

## What happens on the user's machine

- The user supplies their own copy of the kernelcache (from their Mac, or the
  IPSW Apple publishes for it). `tools/kc_patch.py` refuses anything but the
  one build it knows, by SHA-256, and writes our code into the user's copy.
- Firmware for Wi-Fi and Bluetooth comes from the user's own system, the same
  way Asahi and Aurora extract it from the user's macOS.
- Per-unit values (Wi-Fi and Bluetooth calibration, Bluetooth address) are read
  from the user's running device tree by `tools/gen-guest-platform.py`.

The patched boot object, the initrd and the generated `guest-platform.dtsi`
contain Apple's code, firmware or per-unit data. Do not share or publish them.
`.gitignore` keeps them out of commits.

`initrd/tso_litmus.c` is MIT licensed (the author's own file, MIT so FEX can
take it); everything else is GPL-2.0-only.
