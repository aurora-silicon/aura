#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Package one Aura build with its configuration, provenance, and licenses."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", required=True)
    parser.add_argument("--variant", choices=("stage1", "stage2"), required=True)
    parser.add_argument("--output", type=Path, default=Path("dist"))
    parser.add_argument("--compiler", default="gcc")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    build = repo / "build"
    git = lambda *a: subprocess.check_output(["git", *a], cwd=repo, text=True).strip()
    if git("status", "--porcelain", "--untracked-files=no"):
        raise SystemExit("Refusing to package modified tracked sources")
    cfg = (build / "build_cfg.h").read_text()
    if "#define RELEASE\n" not in cfg or "#define CHAINLOADING\n" not in cfg:
        raise SystemExit("Expected a RELEASE=1 CHAINLOADING=1 build")
    if ("#define ESP_STAGE2\n" in cfg) != (args.variant == "stage2"):
        raise SystemExit("ESP_STAGE2 does not match the requested variant")
    tag = (build / "build_tag.h").read_text()
    if tag.strip() != f'#define BUILD_TAG "{args.version}"':
        raise SystemExit("Build version does not match the requested release")
    files = [build / n for n in ("aura.bin", "aura.macho", "build_cfg.h", "build_tag.h")]
    if args.variant == "stage1":
        files.append(build / "aura-boot.macho")
    files += [repo / "LICENSE", repo / "docs/releases/0.1.0.md"]
    files += [repo / "docs/boot-options.md", repo / "docs/j700-install.md",
              repo / "docs/stage2-dtbs.md", repo / "tools/compose_stage2.py"]
    # The one copy of which chips pad the Stage 1 boot object, for the installer and its tools.
    files += [repo / "data/execute-window.json", repo / "tools/execute_window.py"]
    files += [repo / "docs/macho-boot-object.md"]
    for folder in ("LICENSES", "3rdparty_licenses"):
        files += sorted(p for p in (repo / folder).rglob("*") if p.is_file())
    manifest = {
        "project": "aurora-silicon/aura", "version": args.version,
        "commit": git("rev-parse", "HEAD"), "variant": args.variant,
        "make_variables": f"RELEASE=1 CHAINLOADING=1 ESP_STAGE2={int(args.variant == 'stage2')}",
        "compiler": subprocess.check_output([args.compiler, "--version"], text=True).splitlines()[0],
        "rustc": subprocess.check_output(["rustc", "--version"], text=True).strip(),
        "hardware_tested": False,
    }
    args.output.mkdir(parents=True, exist_ok=True)
    name = f"aura-{args.version}-{args.variant}"
    with zipfile.ZipFile(args.output / f"{name}.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        hashes = []
        for path in files:
            relative = path.relative_to(build) if path.is_relative_to(build) else path.relative_to(repo)
            archive.write(path, str(relative))
            hashes.append(f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {relative}")
        archive.writestr("BUILD-INFO.json", json.dumps(manifest, indent=2) + "\n")
        archive.writestr("SHA256SUMS", "\n".join(hashes) + "\n")
        archive.writestr("README.txt", (
            f"Aura {args.version}, {args.variant}; source {manifest['commit']}\n"
            "aura.macho is the RAM-loadable Mach-O image. aura.bin is the raw image.\n"
            "Stage 1 also has aura-boot.macho, aura.bin as a boot object for\n"
            "kmutil configure-boot without --raw (docs/macho-boot-object.md).\n"
            "Stage 1 retains its embedded configuration block. Stage 2 is built\n"
            "with ESP_STAGE2=1 and omits that block. Neither archive is a complete\n"
            "UEFI boot.bin bundle: use matching board DTBs and U-Boot separately.\n"
            "This exact release build has not been boot-tested on hardware.\n"
        ))
    with zipfile.ZipFile(args.output / f"{name}-debug.zip", "w", zipfile.ZIP_DEFLATED) as archive:
        for n in ("aura.elf", "aura-raw.elf"):
            archive.write(build / n, n)
        archive.writestr("BUILD-INFO.json", json.dumps(manifest, indent=2) + "\n")
    sums = [f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.name}"
            for p in sorted(args.output.glob("*.zip"))]
    (args.output / "SHA256SUMS").write_text("\n".join(sums) + "\n")
    print(f"Packaged {name} from {manifest['commit']}")


if __name__ == "__main__":
    main()
