# Aura

Aura is the Aurora Silicon bootloader for Apple Silicon Macs: Stage 1, the ESP
Stage 2 and the development proxy, for Aurora Silicon and Omarchy Mac.

## Build

Install an AArch64 GCC cross compiler, or LLVM with Clang and LLD, and Rust
with the `aarch64-unknown-none-softfloat` target.

```sh
git clone https://github.com/aurora-silicon/aura.git
cd aura
rustup target add aarch64-unknown-none-softfloat
make ARCH=aarch64-linux-gnu-
```

For Clang on an AArch64 Linux host:

```sh
make USE_CLANG=1 ARCH=aarch64-linux-gnu BUILDSTD=1
```

The outputs are `build/aura.bin` and `build/aura.macho`. Use `make V=1` for
verbose output.

That plain build is for development and RAM chainloading. The boot stages
need their own flags, and each overwrites `build/`:

| Image | Flags |
|---|---|
| Stage 1 | `RELEASE=1 CHAINLOADING=1` |
| ESP Stage 2 | `RELEASE=1 CHAINLOADING=1 ESP_STAGE2=1` |

Only an `ESP_STAGE2=1` image may be used as the ESP Stage 2;
`tools/compose_stage2.py` rejects any other. See
[boot options](docs/boot-options.md) and
[Stage 2 device trees](docs/stage2-dtbs.md). On macOS, install LLVM and LLD through Homebrew and run
`make`. A container build is also available through `podman-compose run
aura make` or `docker-compose run aura make`.

## Boot and host tools

See [the J700 installation guide](docs/j700-install.md) for Stage 1 and ESP
Stage 2 setup. Host tools are in `proxyclient`. The inherited Python package
name `aura`, USB identifiers, payload markers and device-tree properties
remain compatible with existing tools and kernels. New device rules provide
`/dev/aura` and `/dev/aura-sec`, with the original aliases retained.

The raw format supports concatenated kernel, device-tree and initramfs
payloads, including gzip and xz compression. Mach-O boot-object support
from Aurora's x1n1 development will be reviewed separately.

The built-in boot logo is Aurora at 48, 128, and 256 pixels. A fork or
distribution can link its own logo instead: make its assets with
`data/makelogo.py <image> --name <name> --output-dir <dir>` and build with
`make LOGO_DIR=<dir> BUILTIN_LOGO=<name>`. Every build checks that
`build/aura.bin` carries the selected logo at all three sizes.

## License

Aura combines inherited MIT code with Aurora Silicon contributions under
GPL-2.0-only. See [LICENSE](LICENSE) and [the provenance policy](docs/licensing.md).

The Aurora Silicon logo files have separate terms in
[LICENSE.AURORA-LOGO](3rdparty_licenses/LICENSE.AURORA-LOGO).

Please see the Git history for authorship information.

## Credits

Aura started from m1n1, by the Asahi Linux project. Code inherited from it
remains under its MIT licence and notice:

* Copyright The Asahi Linux Contributors

m1n1 in turn is based on mini:

* Copyright (C) 2008-2010 Hector Martin "marcan" <marcan@marcan.st>
* Copyright (C) 2008-2010 Sven Peter <sven@svenpeter.dev>
* Copyright (C) 2008-2010 Andre Heider <a.heider@gmail.com>

Aura embeds libfdt, which is dual [BSD](3rdparty_licenses/LICENSE.BSD-2.libfdt) and
[GPL-2](3rdparty_licenses/LICENSE.GPL-2) licensed and copyright:

* Copyright (C) 2014 David Gibson <david@gibson.dropbear.id.au>
* Copyright (C) 2018 embedded brains GmbH
* Copyright (C) 2006-2012 David Gibson, IBM Corporation.
* Copyright (C) 2012 David Gibson, IBM Corporation.
* Copyright 2012 Kim Phillips, Freescale Semiconductor.
* Copyright (C) 2016 Free Electrons
* Copyright (C) 2016 NextThing Co.

The ADT code in mini is also based on libfdt and subject to the same license.

Aura embeds [minlzma](https://github.com/ionescu007/minlzma), which is
[MIT](3rdparty_licenses/LICENSE.minlzma) licensed and copyright:

* Copyright (c) 2020 Alex Ionescu

Aura embeds a slightly modified version of [tinf](https://github.com/jibsen/tinf), which is
[ZLIB](3rdparty_licenses/LICENSE.tinf) licensed and copyright:

* Copyright (c) 2003-2019 Joergen Ibsen

Aura embeds portions taken from
[arm-trusted-firmware](https://github.com/ARM-software/arm-trusted-firmware), which is
[BSD](3rdparty_licenses/LICENSE.BSD-3.arm) licensed and copyright:

* Copyright (c) 2013-2020, ARM Limited and Contributors. All rights reserved.

Aura embeds [Doug Lea's malloc](ftp://gee.cs.oswego.edu/pub/misc/malloc.c) (dlmalloc), which is in
the public domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

Aura embeds portions of [PDCLib](https://github.com/DevSolar/pdclib), which is in the public
domain ([CC0](3rdparty_licenses/LICENSE.CC0)).

Aura embeds the [Source Code Pro](https://github.com/adobe-fonts/source-code-pro) font, which is
licensed under the [OFL-1.1](3rdparty_licenses/LICENSE.OFL-1.1) license and copyright:

* Copyright 2010-2019 Adobe (http://www.adobe.com/), with Reserved Font Name 'Source'. All Rights Reserved. Source is a trademark of Adobe in the United States and/or other countries.
* This Font Software is licensed under the SIL Open Font License, Version 1.1.

Aura embeds portions of the [dwc3 usb linux driver](https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/tree/drivers/usb/dwc3/core.h?id=7bc5a6ba369217e0137833f5955cf0b0f08b0712), which was [BSD-or-GPLv2 dual-licensed](3rdparty_licenses/LICENSE.BSD-3.dwc3) and copyright
* Copyright (C) 2010-2011 Texas Instruments Incorporated - http://www.ti.com

Aura embeds portions of [musl-libc](https://musl.libc.org/)'s floating point library, which are MIT licensed and copyright
* Copyright (c) 2017-2018, Arm Limited.

Aura embeds [rust-fatfs](https://github.com/aurora-silicon/fatfs), which is
[MIT licensed](3rdparty_licenses/LICENSE.MIT.fatfs). The pinned fork revision is
retained by the protected [`aurora-m1n1-v1` tag](https://github.com/aurora-silicon/fatfs/tree/aurora-m1n1-v1).
The fork's [AURORA.md](https://github.com/aurora-silicon/fatfs/blob/aurora-m1n1-v1/AURORA.md)
describes the changes from upstream.

Aura embeds other Rust crates. Licenses can be found in their vendor directories.

J700 Stage 1 configuration, ESP Stage 2 setup and installation are documented in
[docs/j700-install.md](docs/j700-install.md). Release packages include this guide
as `ESP-AND-INSTALL.md`.
