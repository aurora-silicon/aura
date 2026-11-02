#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-only
# mk-initrd.sh KBUILD OUT.cpio : the guest initrd, assembled on the user's own Linux on the J616s.
#
# Nothing private or proprietary lives in this repository. This script takes from the running system:
#   busybox (static), wpa_supplicant, sshd, ssh-keygen with their libraries, regulatory.db and the
#   Broadcom Wi-Fi firmware that the user's Asahi/Aurora install extracted from their own macOS.
# and from $MACHO_CONF (default ~/.config/macho-initrd):
#   wpa.conf          wpa_supplicant network block for the initrd Wi-Fi (optional)
#   authorized_keys   SSH public keys allowed to log in as root (optional)
# The SSH host key is generated fresh for every build. The resulting cpio holds firmware and your
# Wi-Fi key: keep it on your machine.
set -euo pipefail
D=$(dirname "$(readlink -f "$0")"); KB=$(readlink -f "$1"); OUT=$(readlink -f "$(dirname "$2")")/$(basename "$2")
C=${MACHO_CONF:-$HOME/.config/macho-initrd}
T=$(mktemp -d); X=$(mktemp -d); trap 'rm -rf -- "$T" "$X"' EXIT
mkdir -p $T/{bin,sbin,dev,proc,sys,run,tmp,mnt,new,lower,upper,usr/share/empty.sshd,root/.ssh,var/empty,lib/modules,lib/firmware/brcm,usr/bin,usr/lib/ssh,usr/local/bin,etc/ssh,etc/wpa}
install -m 755 "$(command -v busybox)" $T/bin/busybox
file -L $T/bin/busybox | grep -q 'statically linked' || { echo "busybox must be static" >&2; exit 1; }
cp -a "$D/rootfs/." $T/
install -m 755 "$D/initrd-init" $T/init
install -m 755 "$D/macho-eem-up" $T/macho-eem-up
install -m 755 "$D/macho-nvme-wt" $T/macho-nvme-wt
# dynamic tools and their library closure
for b in /usr/bin/wpa_supplicant /usr/bin/sshd /usr/bin/ssh-keygen /usr/lib/ssh/sshd-session /usr/lib/ssh/sshd-auth; do
  [ -e "$b" ] || continue
  install -D -m 755 "$b" "$T$b"
  ldd "$b" | grep -oE '/[^ ]+' | while read -r l; do install -D -m 755 "$(readlink -f "$l")" "$T/usr/lib/$(basename "$l")"; done
done
for l in libnss_files.so.2 libnss_dns.so.2; do [ -e /usr/lib/$l ] && install -m 755 "$(readlink -f /usr/lib/$l)" $T/usr/lib/$l; done
cp "$T/usr/lib/ld-linux-aarch64.so.1" "$T/lib/"
# users: every login is uid 0 (morpheus is the alias the author's log collectors use)
printf '%s\n' 'root:x:0:0:root:/root:/bin/sh' 'sshd:x:22:22:sshd:/var/empty:/sbin/nologin' \
  'nobody:x:65534:65534:nobody:/:/sbin/nologin' 'morpheus:x:0:0:initrd alias of root:/root:/bin/sh' > $T/etc/passwd
printf '%s\n' 'root:x:0:' 'sshd:x:22:' 'nobody:x:65534:' > $T/etc/group
printf '%s\n' 'root:*::::::' 'nobody:*::::::' 'morpheus:*::::::' > $T/etc/shadow; chmod 600 $T/etc/shadow
ssh-keygen -q -t ed25519 -N '' -f $T/etc/ssh/ssh_host_ed25519_key
[ -f "$C/authorized_keys" ] && install -m 600 "$C/authorized_keys" $T/root/.ssh/authorized_keys
[ -f "$C/wpa.conf" ] && install -m 600 "$C/wpa.conf" $T/etc/wpa/wpa.conf
# Wi-Fi firmware of this board (BCM4388, "hostomel"), from the user's own system
cp /usr/lib/firmware/brcm/brcmfmac4388*-pcie.apple,hostomel* $T/lib/firmware/brcm/ 2>/dev/null || echo "warning: no hostomel Wi-Fi firmware in /usr/lib/firmware/brcm" >&2
mkdir -p $T/usr/lib/firmware; cp -a $T/lib/firmware/brcm $T/usr/lib/firmware/
cp /usr/lib/firmware/regulatory.db /usr/lib/firmware/regulatory.db.p7s $T/lib/firmware/ 2>/dev/null || true
# test and probe programs
gcc -static -nostdlib -Os -fno-stack-protector -ffreestanding -o $T/tso-probe "$D/tso-probe.c"
gcc -O2 -static -pthread -s -o $T/tso_litmus "$D/tso_litmus.c"
gcc -O2 -static -pthread -s -o $T/macho-kvm-test "$D/test/macho-kvm-test.c"
gcc -O2 -static -s -o $T/macho-nvme-matrix "$D/macho-nvme-matrix.c"
gcc -O2 -static -pthread -s -o $T/tso-matrix "$D/tso-matrix.c"
gcc -O2 -static -pthread -s -o $T/macho-pinstep "$D/macho-pinstep.c"
# modules with their dependency closure
R=$(cat $KB/include/config/kernel.release); M=$X/lib/modules/$R; mkdir -p $M
want="overlay macsmc-hwmon macsmc-power nvme-apple apple-soc-cpufreq pcie-macho-init pcie-apple cfg80211 brcmutil brcmfmac brcmfmac-wcc hci_bcm4377 bluetooth apple-dart-apif apple-dockchannel apple-rtkit-helper dockchannel-hid asahi"
dcp_want="drm_dma_helper dcp-fabric-core appledrm"
usb_want="phy-apple-t6040-usb2 dwc3-apple sn201202x usb_f_acm usb_f_eem"
find $KB -name '*.ko' > $X/all
declare -A seen
add() { local n=${1//_/-}; [ -n "${seen[$n]:-}" ] && return 0; seen[$n]=1
  local f; f=$(grep -E "/(${n}|${n//-/_})\.ko$" $X/all | head -1) || true
  [ -n "$f" ] || { echo "builtin or absent: $n"; return 0; }
  llvm-strip --strip-debug -o $M/$(basename $f) "$f"
  for d in $(modinfo -F depends "$f" | tr , ' '); do add "$d"; done; }
for m in $want $usb_want $dcp_want; do add "$m"; done
cp $KB/modules.builtin $KB/modules.builtin.modinfo $KB/modules.order $M/ 2>/dev/null || true
depmod -b $X $R
cp $M/*.ko $M/modules.* $T/lib/modules/
cp -r "$D/../diag" $X/diag
make -s -C $KB M=$X/diag modules
for m in macho-diag macho-adt macho-wdt macho-ipi macho-tso; do
  [ -f $X/diag/$m.ko ] && llvm-strip --strip-debug -o $T/lib/modules/$m.ko $X/diag/$m.ko
done
declare -A ord
order() { local n=${1//_/-} f d; [ -n "${ord[$n]:-}" ] && return 0; ord[$n]=1
  f=$(grep -E "/(${n}|${n//-/_})\.ko$" $X/all | head -1) || true; [ -n "$f" ] || return 0
  for d in $(modinfo -F depends "$f" | tr , ' '); do order "$d"; done
  echo "$(basename "$f" .ko)" >> $T/lib/modules/usb.order; }
for m in $usb_want; do order "$m"; done
(cd $T; find . | LC_ALL=C sort | cpio -o -H newc --quiet --owner=0:0) > "$OUT"
ls -l "$OUT"
