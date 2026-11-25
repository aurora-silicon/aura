#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Host-side helper for running a macOS guest under aura SPTM-virt over the USB-C
# DFU proxy tether. Runs on a Linux host connected to the target Mac's DFU port.
#
# Performs:
#   1. Wait for Stage 1 aura proxy (*aura_uartproxy*-if00)
#   2. Read-only precheck that the target ELF (_end) does not overlap DCP __OS_LOG
#   3. Optional in-place Stage 2 chainload (--stage2 aura.bin)
#   4. Background live XNU vuart reader on CDC Pipe 1 (*-if02) with DTR=1
#   5. Launch run_guest_sptm.py and watch <out>.stop for graceful shutdown
#   6. Optional GPU trace ring dump (when parked and AURA_GPU_TRACE=1 or --dump-gpu-ring)
#   7. Optional clean proxy reboot back to the Mac's saved default OS
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
PROXYCLIENT_DIR="${PROXYCLIENT_DIR:-$REPO_ROOT/proxyclient}"

usage() {
    cat <<'EOF'
Usage: sptm_guest_session.sh --out PREFIX [options] [-- KEY=VAL ...]

Required:
  --out PREFIX           Output prefix for logs (<PREFIX>.{precheck,vuart,guest}.txt)
                         and stop-file control (<PREFIX>.stop).

Guest image inputs (either --img-dir or all four explicit paths):
  --img-dir DIR          Directory with sptm.bin, txm.bin, kernelcache*, trustcache.bin
  --sptm FILE            SPTM Mach-O image
  --txm FILE             TXM Mach-O image
  --xnu FILE             Decompressed XNU kernelcache
  --trustcache FILE      Static trustcache image

Aura image options:
  --elf FILE             Target aura-raw.elf for symbol lookup (default: build/aura-raw.elf)
  --stage2 FILE          Optional aura.bin to chainload in place before booting guest
  --os-long-version VER  Value for AURA_OS_LONG_VERSION (e.g. 26.2.123.0.0,0)

Session control:
  --wait SECS            Timeout waiting for Stage 1 proxy (default: 420)
  --guest-secs SECS      Hard timeout for guest session (default: 1800)
  --nm TOOL              Symbol tool for ELF inspection (default: llvm-nm, fallback nm)
  --dump-gpu-ring        Dump sptm_virt_gpu_ring if parked in proxy after guest exit
  --no-reboot            Do not send proxy p.reboot() at session end
  -h, --help             Show this help

Any remaining KEY=VAL arguments (after options or '--') are passed to run_guest_sptm.py
in the environment, e.g.:
  AURA_XNU_SERIAL=uart0 AURA_HIDE_DOCKCHANNEL=1 AURA_WFI_WATCH=1 AURA_TAIL_SECS=1700 AURA_TAIL_IDLE=0
EOF
}

OUT=""
IMG_DIR=""
SPTM=""
TXM=""
XNU=""
TRUSTCACHE=""
ELF="$REPO_ROOT/build/aura-raw.elf"
STAGE2=""
OS_LONG_VERSION="${AURA_OS_LONG_VERSION:-}"
WAIT_SECS="${WAIT:-420}"
GUEST_SECS="${GUEST_SECS:-1800}"
NM_TOOL="${NM:-}"
DUMP_GPU_RING="${DUMP_GPU_RING:-0}"
DO_REBOOT=1
EXTRA_ENV=()

while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="${2:?missing --out value}"; shift 2 ;;
        --img-dir) IMG_DIR="${2:?missing --img-dir value}"; shift 2 ;;
        --sptm) SPTM="${2:?missing --sptm value}"; shift 2 ;;
        --txm) TXM="${2:?missing --txm value}"; shift 2 ;;
        --xnu) XNU="${2:?missing --xnu value}"; shift 2 ;;
        --trustcache) TRUSTCACHE="${2:?missing --trustcache value}"; shift 2 ;;
        --elf) ELF="${2:?missing --elf value}"; shift 2 ;;
        --stage2) STAGE2="${2:?missing --stage2 value}"; shift 2 ;;
        --os-long-version) OS_LONG_VERSION="${2:?missing --os-long-version value}"; shift 2 ;;
        --wait) WAIT_SECS="${2:?missing --wait value}"; shift 2 ;;
        --guest-secs) GUEST_SECS="${2:?missing --guest-secs value}"; shift 2 ;;
        --nm) NM_TOOL="${2:?missing --nm value}"; shift 2 ;;
        --dump-gpu-ring) DUMP_GPU_RING=1; shift ;;
        --no-reboot) DO_REBOOT=0; shift ;;
        -h|--help) usage; exit 0 ;;
        --) shift; EXTRA_ENV+=("$@"); break ;;
        *=*) EXTRA_ENV+=("$1"); shift ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

[ -n "$OUT" ] || { echo "Error: --out PREFIX is required" >&2; usage >&2; exit 2; }

if [ -n "$IMG_DIR" ]; then
    [ -n "$SPTM" ] || SPTM="$IMG_DIR/sptm.bin"
    [ -n "$TXM" ] || TXM="$IMG_DIR/txm.bin"
    [ -n "$TRUSTCACHE" ] || TRUSTCACHE="$IMG_DIR/trustcache.bin"
    if [ -z "$XNU" ]; then
        for cand in "$IMG_DIR"/kernelcache*; do
            if [ -f "$cand" ]; then
                XNU="$cand"
                break
            fi
        done
    fi
fi

for f in "$SPTM" "$TXM" "$XNU" "$TRUSTCACHE" "$ELF"; do
    [ -n "$f" ] && [ -f "$f" ] || { echo "Error: required file missing: ${f:-<unset>}" >&2; exit 2; }
done
if [ -n "$STAGE2" ] && [ ! -f "$STAGE2" ]; then
    echo "Error: --stage2 file missing: $STAGE2" >&2
    exit 2
fi

if [ -z "$NM_TOOL" ]; then
    if command -v llvm-nm >/dev/null 2>&1; then
        NM_TOOL="llvm-nm"
    else
        NM_TOOL="nm"
    fi
fi

if [ -n "${PYTHONPATH:-}" ]; then
    export PYTHONPATH="$PROXYCLIENT_DIR:$PYTHONPATH"
else
    export PYTHONPATH="$PROXYCLIENT_DIR"
fi

log() { echo "$(date +%T) $*"; }
dev() { readlink -f /dev/serial/by-id/*aura_uartproxy*-if00 2>/dev/null | head -n 1; }
vdev() { readlink -f /dev/serial/by-id/*aura_uartproxy*-if02 2>/dev/null | head -n 1; }
usb_devnum() { lsusb -d 1209:316d 2>/dev/null | awk '{print $4}' | head -n 1; }
usb_product() { lsusb -d 1209:316d -v 2>/dev/null | sed -n 's/^ *iProduct *[0-9]* //p' | head -n 1; }

mkdir -p "$(dirname "$OUT")"

t0=$(date +%s)
log "waiting for Stage 1 aura proxy (wait=${WAIT_SECS}s stage2=${STAGE2:-none})"
while [ $(( $(date +%s) - t0 )) -lt "$WAIT_SECS" ]; do
    [ -n "$(dev)" ] && break
    sleep 0.3
done
PROXY_DEV="$(dev)"
[ -n "$PROXY_DEV" ] || { log "timed out waiting for aura proxy (*-if00)"; exit 3; }
log "+$(( $(date +%s) - t0 ))s Stage 1 proxy: $(usb_product) on $PROXY_DEV (vuart: $(vdev))"
dev0="$(usb_devnum)"
sleep 1

# 1. Read-only precheck: confirm target ELF _end does not overlap any DCP __OS_LOG buffer.
env PYTHONPATH="$PYTHONPATH" AURADEVICE="$PROXY_DEV" timeout 60 python3 - "$ELF" "$NM_TOOL" > "$OUT.precheck.txt" 2>&1 <<'EOF'
import struct
import subprocess
import sys
from aura.proxy import AuraProxy, UartInterface
from aura.proxyutils import ProxyUtils

iface = UartInterface()
p = AuraProxy(iface, debug=False)
u = ProxyUtils(p)
base = p.get_base()
nm_out = subprocess.check_output([sys.argv[2], sys.argv[1]], text=True)
syms = {}
for line in nm_out.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16)
end = base + syms["_end"] - syms["_base"]
print(f"stage1 base 0x{base:x} target end 0x{end:x} (+0x{end - base:x}) chip 0x{u.adt['/chosen'].chip_id:x}")
bad = False
for owner in ("dcp", "dcpext", "dcpext0", "dcpext1"):
    try:
        raw = u.adt[f"/arm-io/{owner}/iop-{owner}-nub"]._properties.get("segment-ranges")
    except KeyError:
        continue
    for i in range(len(raw or b"") // 32):
        phys, _iova, _remap, size, flags = struct.unpack_from("<QQQII", raw, 32 * i)
        if flags == 0xA:
            ov = phys < end and phys + size > base
            bad |= ov
            print(f"{owner} __OS_LOG 0x{phys:x}+0x{size:x} (+0x{phys - base:x}){' OVERLAP' if ov else ''}")
print(f"top_of_kernel_data 0x{u.ba.top_of_kernel_data:x} heap top 0x{p.heapblock_alloc(0):x}")
if bad:
    print("PRECHECK FAIL: OVERLAP")
    sys.exit(1)
print("PRECHECK OK MODE inplace")
EOF
rc=$?
cat "$OUT.precheck.txt"
if [ "$rc" -ne 0 ]; then
    log "precheck failed (rc=$rc); rebooting target from proxy"
    env PYTHONPATH="$PYTHONPATH" AURADEVICE="$PROXY_DEV" timeout 20 python3 -c \
        "from aura.proxy import UartInterface, AuraProxy; AuraProxy(UartInterface(), debug=False).reboot()" \
        2>&1 | tail -n 1
    exit 4
fi

# 2. Optional in-place Stage 2 chainload.
if [ -n "$STAGE2" ]; then
    log "chainloading $STAGE2 in place"
    env PYTHONPATH="$PYTHONPATH" AURADEVICE="$PROXY_DEV" timeout 120 \
        python3 "$SCRIPT_DIR/chainload.py" -r --vector-only "$STAGE2" 2>&1 | grep -v '^\.\+$' | tail -n 12
    t1=$(date +%s)
    while [ $(( $(date +%s) - t1 )) -lt 90 ]; do
        d="$(usb_devnum)"
        if [ -n "$d" ] && [ "$d" != "$dev0" ] && [ -n "$(dev)" ]; then
            break
        fi
        sleep 0.2
    done
    PROXY_DEV="$(dev)"
    [ -n "$PROXY_DEV" ] || { log "Stage 2 never re-enumerated"; exit 5; }
    log "Stage 2 up: $(usb_product) on $PROXY_DEV (vuart: $(vdev))"
    sleep 2
fi

# 3. Start CDC Pipe 1 (*-if02) vuart reader with DTR=1 before entering guest.
rm -f "$OUT.stop"
VDEV="$(vdev)"
VPID=""
if [ -n "$VDEV" ]; then
    log "starting live XNU vuart reader on $VDEV -> $OUT.vuart.txt"
    python3 -u "$SCRIPT_DIR/sptm_vuart_tail.py" \
        --device "$VDEV" --output "$OUT.vuart.txt" --stop-file "$OUT.stop" --timeout "$GUEST_SECS" &
    VPID=$!
    sleep 0.5
else
    log "warning: no *-if02 vuart CDC device found"
fi

# 4. Launch run_guest_sptm.py and watch <out>.stop for clean teardown.
RUN_ENV=("PYTHONPATH=$PYTHONPATH" "AURADEVICE=$PROXY_DEV")
if [ -n "$OS_LONG_VERSION" ]; then
    RUN_ENV+=("AURA_OS_LONG_VERSION=$OS_LONG_VERSION")
fi
RUN_ENV+=("${EXTRA_ENV[@]}")

log "launching run_guest_sptm.py (ELF=$ELF)"
env "${RUN_ENV[@]}" timeout -s INT "$GUEST_SECS" python3 -u "$SCRIPT_DIR/run_guest_sptm.py" \
    --sptm "$SPTM" --txm "$TXM" --xnu "$XNU" --trustcache "$TRUSTCACHE" \
    --elf "$ELF" > "$OUT.guest.txt" 2>&1 &
gpid=$!
echo "$gpid" > "$OUT.gpid"

while kill -0 "$gpid" 2>/dev/null; do
    if [ -f "$OUT.stop" ]; then
        log "stop file seen ($OUT.stop); ending console tail"
        kill -INT "$gpid" 2>/dev/null || true
        sleep 2
        kill -TERM "$gpid" 2>/dev/null || true
        break
    fi
    sleep 1
done
wait "$gpid" 2>/dev/null
log "run_guest_sptm rc=$?"
tail -n 40 "$OUT.guest.txt"

if [ -n "$VPID" ]; then
    kill -INT "$VPID" 2>/dev/null || true
    wait "$VPID" 2>/dev/null || true
    log "vuart log size: $(wc -c < "$OUT.vuart.txt" 2>/dev/null || echo 0) bytes"
    tail -n 30 "$OUT.vuart.txt" 2>/dev/null || true
fi

# 5. Optional GPU ring dump if parked in proxy.
sleep 2
want_gpu="$DUMP_GPU_RING"
for a in "${EXTRA_ENV[@]}"; do
    case "$a" in AURA_GPU_TRACE=1) want_gpu=1 ;; esac
done
PROXY_DEV="$(dev)"
if [ "$want_gpu" = "1" ] && [ -n "$PROXY_DEV" ]; then
    log "dumping GPU trace ring from parked proxy -> $OUT.gpu_ring.bin"
    env PYTHONPATH="$PYTHONPATH" AURADEVICE="$PROXY_DEV" timeout 180 \
        python3 - "$ELF" "$NM_TOOL" "$OUT.gpu_ring.bin" > "$OUT.gpu_ring.meta.txt" 2>&1 <<'EOF'
import subprocess
import sys
from aura.proxy import AuraProxy, UartInterface

iface = UartInterface()
p = AuraProxy(iface, debug=False)
base = p.get_base()
nm_out = subprocess.check_output([sys.argv[2], sys.argv[1]], text=True)
syms = {}
for line in nm_out.splitlines():
    parts = line.split()
    if len(parts) == 3:
        syms[parts[2]] = int(parts[0], 16)
sym = lambda n: base + (syms[n] - syms["_base"])
ring = p.read64(sym("sptm_virt_gpu_ring"))
nrec = p.read64(sym("sptm_virt_gpu_nrec"))
head = p.read64(sym("sptm_virt_gpu_head"))
print(f"base=0x{base:x} ring=0x{ring:x} nrec={nrec} head={head}")
if ring and nrec and head:
    count = min(head, nrec)
    nbytes = count * 32
    print(f"dumping {count} records ({nbytes} bytes) -> {sys.argv[3]}")
    data = iface.readmem(ring, nbytes)
    with open(sys.argv[3], "wb") as f:
        f.write(data)
    print(f"DUMP OK {len(data)} bytes")
else:
    print("RING EMPTY OR NOT ARMED")
EOF
    cat "$OUT.gpu_ring.meta.txt"
fi

# 6. Reboot target via proxy if still parked.
PROXY_DEV="$(dev)"
if [ "$DO_REBOOT" = "1" ] && [ -n "$PROXY_DEV" ]; then
    env PYTHONPATH="$PYTHONPATH" AURADEVICE="$PROXY_DEV" timeout 20 python3 -c \
        "from aura.proxy import UartInterface, AuraProxy; AuraProxy(UartInterface(), debug=False).reboot()" \
        2>&1 | tail -n 1
    log "reboot sent via proxy"
elif [ "$DO_REBOOT" = "1" ]; then
    log "no proxy device present (target already rebooted or disconnected)"
fi
