#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Lay out the ESP files that boot macOS under aura with no USB host (docs/m5-macos-esp.md).

Writes a staging tree on this machine and touches nothing else. With the inputs the M5 trace
runs use (/path/to/capture):

  FW=/path/to/macos-images; ST=$FW/stock/Firmware
  python3 tools/esp-macos/make_bundle.py --stage2 build/aura.bin --esp-partuuid <ESP-PARTUUID> \\
      --sptm $FW/sptm.macho --txm $FW/txm.macho --xnu $FW/kernelcache.macho \\
      --trustcache native-TrustCache.bin --adt live.adt --build-identity $FW/BuildIdentity.plist \\
      --exclave-bundle $ST/image4/exclavecore_bundle.t6050.RELEASE.im4p \\
      --exclave-integrity $ST/043-70822-678.dmg.aea.integrity_catalog \\
      --exclave-trustcache $ST/043-70822-678.dmg.aea.trustcache --out /tmp/m5-macos-esp

The Stage 2 must be built with
  make USE_CLANG=1 BUILDSTD=1 CHAINLOADING=1 ESP_STAGE2=1 SPTM_VIRT=1 SPTM_VIRT_ESP=1 DEBUGUSB_PROXY=1
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "proxyclient"))

from aura import esp_macos  # noqa: E402


def build_cfg(stage2, explicit):
    path = pathlib.Path(explicit) if explicit else stage2.with_name("build_cfg.h")
    if not path.exists():
        raise SystemExit(f"{path}: not found; say where this image's build_cfg.h is (--build-cfg)")
    return {ln.split()[1] for ln in path.read_text().splitlines() if ln.startswith("#define ")}


def kv(s):
    k, _, v = s.partition("=")
    return k, int(v, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--stage2", type=pathlib.Path, required=True, help="the aura.bin to install")
    ap.add_argument("--build-cfg", help="that image's build_cfg.h (default: next to it)")
    for f in ("sptm", "txm", "trustcache", "adt", "build-identity"):
        ap.add_argument(f"--{f}", type=pathlib.Path, required=True)
    ap.add_argument("--xnu", type=pathlib.Path, required=True, help="decompressed kernelcache")
    ap.add_argument("--exclave-bundle", type=pathlib.Path, required=True)
    ap.add_argument("--exclave-integrity", type=pathlib.Path, required=True)
    ap.add_argument("--exclave-trustcache", type=pathlib.Path, required=True)
    ap.add_argument("--out", type=pathlib.Path, required=True, help="staging directory")
    ap.add_argument("--esp-partuuid", required=True)
    ap.add_argument("--bootargs", help="XNU boot-args (default: run_guest_sptm.py's)")
    ap.add_argument("--flag", action="append", help="a run_guest_sptm.py CONFIG_FLAGS name "
                    f"(default: {' '.join(esp_macos.DEFAULT_FLAGS)})")
    ap.add_argument("--s2r", action="append", type=kv, help="NAME=VALUE, one of "
                    f"{', '.join(esp_macos.S2R_SWITCHES)} (default: {esp_macos.DEFAULT_S2R})")
    ap.add_argument("--ans-drop", action="append", default=[],
                    help="an /arm-io/ans/iop-ans-nub property XNU must not see (quiesced, running)")
    ap.add_argument("--no-video", action="store_true", help="don't hand XNU the boot video")
    ap.add_argument("--gzip-kernelcache", action="store_true", help="smaller on the ESP, slower to boot")
    ap.add_argument("--build-tag", default="26A428")
    a = ap.parse_args()

    if a.out.exists() and any(a.out.iterdir()):
        raise SystemExit(f"{a.out} is not empty; give a new directory")
    need = {"CHAINLOADING", "ESP_STAGE2", "SPTM_VIRT", "SPTM_VIRT_ESP"}
    cfg = build_cfg(a.stage2, a.build_cfg)
    if need - cfg:
        raise SystemExit(f"{a.stage2} lacks {sorted(need - cfg)}")
    stage2 = a.stage2.read_bytes()
    if not stage2.endswith(b"STACKBOT") or b"AURORA-S1-CFG01\0" in stage2:
        raise SystemExit(f"{a.stage2}: expected a bare ESP_STAGE2=1 aura image")

    try:
        ex = esp_macos.exclaves(a.exclave_bundle, a.exclave_integrity, a.exclave_trustcache)
        b = esp_macos.build(
            a.sptm.read_bytes(), a.txm.read_bytes(), a.xnu.read_bytes(), a.trustcache.read_bytes(),
            a.adt.read_bytes(), exclave=ex, love=esp_macos.os_long_version(a.build_identity),
            bootargs=a.bootargs, flags=tuple(a.flag) if a.flag else esp_macos.DEFAULT_FLAGS,
            s2r=dict(a.s2r) if a.s2r else None,
            ans_drop=[("/arm-io/ans/iop-ans-nub", p) for p in a.ans_drop],
            keep_video=not a.no_video, gzip_kc=a.gzip_kernelcache, build_tag=a.build_tag)
    except esp_macos.BundleError as e:
        raise SystemExit(f"cannot build the bundle: {e}")

    s2 = esp_macos.write_staging(b, stage2, a.out, partuuid=a.esp_partuuid)
    total = s2.stat().st_size + sum(len(d) for d in b.files.values())
    for n in b.notes:
        print("note:", n)
    print(f"staged {total} bytes ({total >> 20} MiB) under {a.out}")
    print("config line:", b.config_line(a.esp_partuuid), end="")
    print("next: copy the tree to the M5 and run tools/esp-macos/install_esp.sh there")


if __name__ == "__main__":
    main()
