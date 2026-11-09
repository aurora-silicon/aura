#!/bin/sh
# SPDX-License-Identifier: MIT
#
# Reproducible build evidence for the HvXfer port. No hardware is touched.
#
# Builds the Windows (WINDOWS=1) and default images twice -- from the
# committed base (git archive HEAD) and from the working tree with this port --
# in throwaway copies, with the same pinned version tag, then records:
#   * SHA-256 and size of aura.bin / aura.macho / aura-boot.macho per variant
#   * whether the default image is byte-identical to the base (it must be)
#   * whether each variant's warning set is identical to the base's
#   * the DCP file-part margin (_file_vital_end vs _file_limit, aura-raw.ld)
# The port's images are kept in build/hvxfer/<variant>/ (build/ is ignored).
#
# Writes tests/hv_xfer/out-build/build.json.

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
out="$here/out-build"
PATH=$HOME/.cargo/bin:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin
export PATH
AURA_VERSION_TAG=hvxfer-compare
export AURA_VERSION_TAG
make=${MAKE:-/opt/homebrew/bin/gmake}
nm=${NM:-/opt/homebrew/opt/llvm/bin/llvm-nm}

work=$(mktemp -d "${TMPDIR:-/tmp}/hvxfer-build.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

base_commit=$(git -C "$repo" rev-parse HEAD)
mkdir -p "$work/base" "$work/port"
git -C "$repo" archive "$base_commit" | tar -x -C "$work/base"
rsync -a --exclude .git --exclude /build --exclude 'tests/hv_xfer/out*' "$repo/" "$work/port/"
mkdir -p "$work/port/build"

build() { # tree variant
    cd "$work/$1"
    "$make" clean > /dev/null 2>&1 || true
    if [ "$2" = windows ]; then flags="WINDOWS=1"; else flags=""; fi
    # shellcheck disable=SC2086
    if "$make" -j8 $flags > "$work/$1-$2.log" 2>&1; then rc=0; else rc=$?; fi
    mkdir -p "$work/art/$1/$2"
    if [ "$rc" -eq 0 ]; then
        cp build/aura.bin build/aura.macho build/aura-boot.macho "$work/art/$1/$2/"
        "$nm" build/aura-raw.elf | awk '$3=="_file_vital_end"{print $1}' > "$work/art/$1/$2/vital_end"
    fi
    echo "$rc" > "$work/art/$1/$2/rc"
}

for tree in base port; do
    for variant in windows default; do
        echo "building $tree/$variant"
        build "$tree" "$variant"
    done
done

mkdir -p "$out" "$repo/build/hvxfer"
for variant in windows default; do
    rm -rf "$repo/build/hvxfer/$variant"
    cp -R "$work/art/port/$variant" "$repo/build/hvxfer/$variant"
done
cp "$work"/*.log "$out/"

python3 - "$work" "$out" "$repo" "$base_commit" "$make" <<'EOF'
import hashlib, json, re, sys
from pathlib import Path
work, out, repo, base_commit, make = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4], sys.argv[5]
art = work / "art"
FILE_LIMIT = 0x120000

def warnings(tree, variant):
    text = (work / f"{tree}-{variant}.log").read_text(errors="replace")
    root = str(work / tree) + "/"
    return sorted(line.replace(root, "") for line in text.splitlines() if "warning:" in line)

def images(tree, variant):
    d = art / tree / variant
    if (d / "rc").read_text().strip() != "0":
        return None
    res = {n: {"sha256": hashlib.sha256((d / n).read_bytes()).hexdigest(), "bytes": (d / n).stat().st_size}
           for n in ("aura.bin", "aura.macho", "aura-boot.macho")}
    vital = int((d / "vital_end").read_text().strip() or "0", 16)
    res["file_vital_end"] = hex(vital)
    res["dcp_margin_bytes"] = FILE_LIMIT - vital
    return res

report = {"schema": "aura.hvxfer.build.v1", "base_commit": base_commit, "hardware_accessed": False,
          "version_tag": "hvxfer-compare",
          "build_commands": [{"variant": "windows", "argv": [make, "-j8", "WINDOWS=1"]},
                             {"variant": "default", "argv": [make, "-j8"]}],
          "variants": {}}
for variant in ("windows", "default"):
    b, p = images("base", variant), images("port", variant)
    wb, wp = warnings("base", variant), warnings("port", variant)
    report["variants"][variant] = {"base": b, "port": p, "base_warnings": len(wb), "port_warnings": len(wp),
                                   "warning_set_identical": wb == wp,
                                   "new_warnings": sorted(set(wp) - set(wb))}
w, d = report["variants"]["windows"], report["variants"]["default"]
report["windows_link_ok"] = w["port"] is not None and w["port"]["dcp_margin_bytes"] >= 0
report["windows_warning_set_identical"] = w["warning_set_identical"]
report["default_identical"] = (d["base"] is not None and d["port"] is not None and
                               all(d["base"][n]["sha256"] == d["port"][n]["sha256"]
                                   for n in ("aura.bin", "aura.macho", "aura-boot.macho")))
report["kept_images"] = str(repo / "build/hvxfer")
(out / "build.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps({k: report[k] for k in ("default_identical", "windows_link_ok", "windows_warning_set_identical")}))
print("windows port aura.bin", w["port"]["aura.bin"]["sha256"] if w["port"] else None,
      "dcp margin", w["port"]["dcp_margin_bytes"] if w["port"] else None)
EOF
