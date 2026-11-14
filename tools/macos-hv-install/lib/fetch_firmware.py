#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Locate the firmware pieces make_bundle.py needs, from THIS Mac's installed macOS.

No Apple binary is downloaded or redistributed: every piece comes from the macOS already
installed on this machine (the same idea as the Aurora Linux installer taking firmware from
the installed macOS). This tool only reads the installed OS and copies the pieces into a
work directory; make_bundle.py then assembles the ESP staging tree from them.

    fetch_firmware.py --work <dir> [--volume /] [--no-exclave] [--dry-run]
                      [--sptm P] [--txm P] [--xnu P] [--trustcache P] [--adt P]
                      [--build-identity P] [--exclave-bundle P]
                      [--exclave-integrity P] [--exclave-trustcache P]

For each piece it prints where it found it (provenance). --dry-run finds and reports, copies
nothing. Any explicit --<piece> path overrides the search for that piece.

WHERE THESE LIVE (Apple Silicon, confirm-on-hardware): the active boot files live under the
Preboot volume for the target's APFS volume group:

    /System/Volumes/Preboot/<GROUP-UUID>/boot/<LP-HASH>/...

The exact sub-paths of SPTM/TXM/the trustcache inside that boot environment, and how to read
the live ADT without a kext, are the parts still being pinned on hardware (the J616s
acceptance run). This tool searches the documented candidates and, when a piece cannot be
found, STOPS with the exact --<piece> override to pass, rather than guessing. It never writes
outside --work.
"""
import argparse
import glob
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys

# Set by the host tests to a fixture tree standing in for "/".
FAKE_ROOT = os.environ.get("HVI_FAKE_ROOT")


def log(msg):
    print(msg, file=sys.stderr)


class FetchError(Exception):
    pass


def volroot(volume):
    """The filesystem root to search under (honours the test fixture root)."""
    if FAKE_ROOT:
        return pathlib.Path(FAKE_ROOT)
    return pathlib.Path(volume)


def group_uuid(volume):
    """The APFS volume group UUID of the target System volume (diskutil)."""
    if FAKE_ROOT:
        # The fixture encodes it in a file so tests need no diskutil.
        f = pathlib.Path(FAKE_ROOT) / ".hvi-group"
        return f.read_text().strip() if f.exists() else "GROUP-FIXTURE"
    try:
        out = subprocess.run(["diskutil", "info", volume], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError) as e:
        raise FetchError(f"diskutil info {volume} failed: {e}")
    for line in out.splitlines():
        if "APFS Volume Group:" in line:
            return line.split(":", 1)[1].strip()
    raise FetchError(f"no APFS volume group for {volume}")


def boot_envs(volume):
    """The candidate boot-environment directories, newest first."""
    root = volroot(volume)
    g = group_uuid(volume)
    base = root / "System/Volumes/Preboot" / g / "boot"
    if not base.is_dir():
        # Some layouts mount Preboot elsewhere; try a shallow glob.
        alts = list(root.glob(f"System/Volumes/Preboot/{g}/boot"))
        if not alts:
            raise FetchError(f"no Preboot boot environment under {base} (pass each piece explicitly)")
        base = alts[0]
    envs = [p for p in base.iterdir() if p.is_dir()]
    envs.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    if not envs:
        raise FetchError(f"{base} has no boot environment (pass each piece explicitly)")
    return envs


# Each piece: (make_bundle name, [candidate globs relative to a boot env], required-ness).
# The globs are the documented Apple-Silicon candidates; confirm-on-hardware. The first
# existing match wins. An explicit override skips the search.
PIECES = {
    "sptm": ["usr/standalone/firmware/**/sptm*.img4", "**/sptm*.macho", "**/sptm*"],
    "txm": ["usr/standalone/firmware/**/txm*.img4", "**/txm*.macho", "**/txm*"],
    "xnu": ["System/Library/Caches/com.apple.kernelcaches/kernelcache*",
            "**/kernelcache*"],
    "trustcache": ["**/StaticTrustCache*.img4", "**/*TrustCache*.bin", "**/*.trustcache"],
}
EXCLAVE_PIECES = {
    "exclave-bundle": ["**/exclavecore_bundle*.im4p", "**/exclavecore_bundle*"],
    "exclave-integrity": ["**/*.integrity_catalog", "**/*integrity*catalog*"],
    "exclave-trustcache": ["**/*.aea.trustcache", "**/*exclave*trustcache*"],
}


def find_in_envs(envs, globs):
    for env in envs:
        for pat in globs:
            hits = sorted(env.glob(pat))
            if hits:
                return hits[0]
    return None


def is_macho(data):
    return len(data) >= 4 and data[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf",
                                           b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca")


def decompress_kernelcache(src, dst, dry):
    """make_bundle wants a decompressed Mach-O kernelcache. Copy if already one; else try
    to unwrap/decompress with whatever the OS provides, and STOP clearly if we cannot."""
    data = src.read_bytes()
    if is_macho(data):
        log(f"    kernelcache is already a Mach-O ({len(data)} bytes)")
        if not dry:
            shutil.copyfile(src, dst)
        return
    # An Image4 (.img4/.im4p) payload, usually LZFSE/LZSS compressed. macOS ships no public
    # lzfse CLI, so we do not guess here: ask for a decompressed kernelcache explicitly.
    raise FetchError(
        f"{src} is not a decompressed Mach-O kernelcache (it looks wrapped/compressed). "
        "Supply a decompressed kernelcache with --xnu <path> (e.g. from pyimg4/img4 + lzfse). "
        "This is the one piece the installer will not guess at.")


def dump_adt(volume, dst, dry):
    """Write the live Apple Device Tree make_bundle needs for the debug port's nodes/PHYs.

    confirm-on-hardware: there is no supported kext-free macOS userspace way to read the raw
    ADT yet (the USB-tethered proxyclient/tools/boot_j873.py reads it from target memory).
    We try the documented candidates; if none work, STOP with --adt."""
    if FAKE_ROOT:
        f = pathlib.Path(FAKE_ROOT) / ".hvi-adt"
        if f.exists():
            log(f"    ADT (fixture): {f}")
            if not dry:
                shutil.copyfile(f, dst)
            return
    raise FetchError(
        "could not read the live ADT on this Mac without a kext. Supply it with --adt <path> "
        "(e.g. captured over the proxy with proxyclient/tools/boot_j873.py, or from the "
        "acceptance kit). This is a known gap being closed on hardware.")


def os_long_version_present(build_identity):
    try:
        with open(build_identity, "rb") as f:
            return "Ap,OSLongVersion" in plistlib.load(f)
    except (OSError, ValueError):
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--work", required=True, type=pathlib.Path, help="output directory")
    ap.add_argument("--volume", default="/", help="the target macOS System volume (default /)")
    ap.add_argument("--no-exclave", action="store_true", help="this Mac has no ExclaveOS (e.g. T6041)")
    ap.add_argument("--dry-run", action="store_true")
    for p in list(PIECES) + ["adt", "build-identity"] + list(EXCLAVE_PIECES):
        ap.add_argument(f"--{p}", type=pathlib.Path, help=f"use this file as {p} (skip the search)")
    a = ap.parse_args()

    work = a.work
    if not a.dry_run:
        work.mkdir(parents=True, exist_ok=True)

    overrides = {p.replace("-", "_"): getattr(a, p.replace("-", "_"))
                 for p in list(PIECES) + ["adt", "build-identity"] + list(EXCLAVE_PIECES)}

    try:
        envs = None
        need_search = any(overrides[p.replace("-", "_")] is None
                          for p in list(PIECES)) or overrides["adt"] is None
        if need_search:
            envs = boot_envs(a.volume)
            log(f"    boot environment: {envs[0]}")

        staged = {}

        def stage_copy(name, src):
            dst = work / f"{name}.bin"
            log(f"    {name}: {src}")
            if not a.dry_run:
                shutil.copyfile(src, dst)
            staged[name] = dst

        # SPTM / TXM / trustcache: copy verbatim.
        for name, globs in (("sptm", PIECES["sptm"]), ("txm", PIECES["txm"]),
                            ("trustcache", PIECES["trustcache"])):
            src = overrides[name] or find_in_envs(envs, globs)
            if not src:
                raise FetchError(f"could not find {name}; pass --{name} <path>")
            stage_copy(name, src)

        # kernelcache: must end up decompressed.
        xnu = overrides["xnu"] or find_in_envs(envs, PIECES["xnu"])
        if not xnu:
            raise FetchError("could not find the kernelcache; pass --xnu <path>")
        log(f"    xnu (kernelcache): {xnu}")
        decompress_kernelcache(xnu, work / "kernelcache.macho", a.dry_run)
        staged["xnu"] = work / "kernelcache.macho"

        # ADT.
        adt = overrides["adt"]
        if adt:
            log(f"    adt: {adt}")
            if not a.dry_run:
                shutil.copyfile(adt, work / "live.adt")
        else:
            dump_adt(a.volume, work / "live.adt", a.dry_run)
        staged["adt"] = work / "live.adt"

        # BuildIdentity: optional unless we have exclaves (needed for Ap,OSLongVersion).
        bi = overrides["build_identity"]
        if bi:
            log(f"    build-identity: {bi}")
            if not a.dry_run and bi.exists() and not os_long_version_present(bi):
                raise FetchError(f"{bi} has no Ap,OSLongVersion")
            if not a.dry_run:
                shutil.copyfile(bi, work / "BuildIdentity.plist")
            staged["build_identity"] = work / "BuildIdentity.plist"
        elif not a.no_exclave:
            raise FetchError("exclave Macs need the BuildIdentity (Ap,OSLongVersion); pass "
                             "--build-identity <path> or --no-exclave on a Mac with no ExclaveOS")

        # Exclaves.
        if not a.no_exclave:
            if envs is None:
                envs = boot_envs(a.volume)
            for name, globs in EXCLAVE_PIECES.items():
                key = name.replace("-", "_")
                src = overrides[key] or find_in_envs(envs, globs)
                if not src:
                    raise FetchError(f"could not find {name}; pass --{name} <path>, or "
                                     "--no-exclave if this Mac has no ExclaveOS")
                stage_copy(name.replace("-", "_"), src)
        else:
            log("    exclaves: skipped (--no-exclave)")

    except FetchError as e:
        sys.exit(f"fetch_firmware: {e}")

    log("    firmware located" + (" (dry run, nothing copied)" if a.dry_run else f"; staged under {work}"))


if __name__ == "__main__":
    main()
