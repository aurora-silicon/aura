#!/bin/sh
# Host check of auractl's pure register decode + ring (src/sptm_virt_auractl.c with AURACTL_HOST;
# the HV/EL2 glue is dropped). Asserts the ABI handshake, ARM range validation, the state machine,
# the DUMP/FIFO pop order and count, and DROPPED on ring wrap. See tests/auractl_decode_host.c.
#
#   sh tests/run-auractl-decode.sh          (CC=cc by default; CC=gcc and CC=clang both pass)
set -eu
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
${CC:-cc} -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -fno-sanitize-recover=undefined -Isrc \
    tests/auractl_decode_host.c -o "$tmp/auractl-decode-host"
"$tmp/auractl-decode-host"
