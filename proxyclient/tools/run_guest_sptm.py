#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Boot macOS's stock SPTM, TXM, secure kernel and XNU as a FEAT_NV2 guest under aura (SPTM_VIRT).

The host plays iBoot's SPTM loader, which a kcbz=0 boot object skips: it lays the images into
SPTM's managed window in the order SPTM expects, edits the device tree, builds the boot_args
SPTM's __start takes in x0, and asks aura to enter SPTM at virtual EL2 with its MMU off. The
layout is aura/sptm_virt_plan.py; see docs/sptm-nv2-virt.md.

Needs aura built with SPTM_VIRT=1 CHAINLOADING=1 and chainloaded, with its proxy up.

Environment (all optional; 1 enables a switch):
  AURA_KEEP_EXCLAVES       keep the exclave nodes; with AURA_EXCLAVE_FW, run cL4 and ExclaveOS
  AURA_EXCLAVE_FW=<dir>    extracted exclave firmware (exclavecore_bundle.<soc>.RELEASE and
                           <build>__MacOS/Firmware)
  AURA_EXCLAVE_BUNDLE=<f>  the IPSW's ExclaveCore bundle (Ap,cL4), instead of AURA_EXCLAVE_FW
  AURA_EXCLAVE_INTEGRITY=<f>, AURA_EXCLAVE_TRUSTCACHE=<f>   the ExclaveOS catalogs beside it
  AURA_XNU_SERIAL=<node>   XNU's console device under /arm-io, e.g. uart0 (aura's vuart)
  AURA_HIDE_DOCKCHANNEL    hide the DockChannel that carries aura's console and proxy
  AURA_KEEP_ERRH           keep the platform error handler visible to XNU
  AURA_AMCC_CLEAR          clear the AMCC error latches stage 1 left, before entry
  AURA_S2R_TRACE           sleep path: hooks on the power blocks and the trace ring
  AURA_S2R_GO              let the sleep proceed (otherwise stop in the proxy before it)
  AURA_S2R_FULLTRACE       also hook the high-volume blocks
  AURA_S2R_COPROC          also hook the coprocessor and exclave mailboxes
  AURA_S2R_MEM             T6050: also hook the MCC, AMCC, DCS and coprocessor DARTs
  AURA_S2R_CP_NO_ANS       T6050 COPROC: leave ANS's own pages unhooked
  AURA_S2R_DCP_PKT         T6050 COPROC: copy the DCPs' IOMFB packets into the ring
  AURA_S2R_CP_ANSX=<mask>  T6050 COPROC: also hook 1 the ANS SART, 2 the NVMe BAR page, 4 AUS5
  AURA_S2R_DROP_USB        tear down aura's USB gadget at the sleep commit
  AURA_S2R_WAKE_S=<s>      GO, timer-only sleep: arm the PMU RTC alarm s seconds after the commit
  AURA_S2R_GO_COUNT=<n>    GO: after n wakes, stop in the proxy at the next commit
  AURA_S2R_CRUMBS=<mask>   T8142 GO: note steps in PMU scratch (0x830d..0x8311), which a reset
                           keeps; bits as in sptm_virt_s2r.c (s2r_crumb)
  AURA_S2R_WIN=<b>,<a>     after a wake, dump b records before the first commit and a after
  AURA_S2R_ARM_S=<s>       arm the sleep rules s seconds after setup (default 45)
  AURA_S2R_EXTRA=<mask>    stop mode: also hook 1 the MCC, 2 aon-ptd, 4 other coprocessor mailboxes
  AURA_S2R_PROBE=<a,...>   read up to four registers at the wake (bit 63: 64-bit read)
  AURA_S2R_PROBE_AT=<a>    take the AURA_S2R_PROBE reads after the guest's first write to a
  AURA_S2R_PREWRITE=<a,v>  write v to a at the wake, before the probes
  AURA_AURA_HEAP_MIB=<n>   aura's own heap under the guest (112 MiB on J813, 128 elsewhere)
  AURA_S2R_QUIESCE=<mask>  AMCC error-enable writes at the commit: 8 DSID_AGE_OUT,
                           16 UNEXP_RT_HIT_DIR
  AURA_S2R_USB_RETAKE=<n>  take the CDC link again after a wake; n > 1 also drops it n s in
  AURA_S2R_USB_RETAKE_MS   delay from XNU's return to the re-take (ms)
  AURA_XNU_BT              sample XNU backtraces every few seconds per core
  AURA_GPU_TRACE           hook the GPU (sgx reg 0) and PMP registers, record every access
  AURA_GPU_STOP_SECS=<n>   park in the proxy n s after the first GPU access (read the ring)
  AURA_GPU_STOP_MSG=<v>    park right after the guest writes v to the GPU (a mailbox message)
  AURA_GPU_STOP_MSG_MASK=<m> compare only the bits in m (e.g. 0xffff000000000000 with
                           AURA_GPU_STOP_MSG=0x0081000000000000: the first InitData doorbell)
  AURA_GPU_STOP_EP=<ep>    and only a mailbox message to endpoint ep (e.g. 0x20)
  AURA_GPU_PRINT_MAX=<n>   print at most n GPU accesses on aura's console (default 20000)
  AURA_GPU_MSG_PARK=0      the write AURA_GPU_STOP_MSG names doesn't park (it snapshots, below)
  AURA_GPU_SNAP=<n>        copy the GPU firmware's memory (its page tables, the objects the
                           doorbell names, the chains) at every n-th such write, before it is
                           performed (tools/dump_snapshot.py reads it at a park)
  AURA_GPU_SNAP_MAX=<k>    k snapshots in all (default 1); AURA_GPU_SNAP_MIB=<m> buffer (32)
  AURA_GPU_SNAP_PARTS=<m>  what snapshots after the first take: 1 regions, 2 walk, 4 objects,
                           8 chains (default 12)
  AURA_GPU_SNAP_ROOTS=<va,...>  the objects' roots after all (default: the doorbell's address)
  AURA_GPU_SNAP_CHAINS=<s> pointer chains from the InitData root, copied whole: g16g-272 (HW
                           data, the main config, every channel ring's state, the device-control
                           ring, FWCTL), or name=step/step:len;... (aura/hvsnap.py)
  AURA_GPU_SNAP_WATCH=<name+off:bytes,...>  words of a chain kept in the snapshot's header
                           (g16g-272 brings HW data +0xfa0 and +0xfa8)
  AURA_GPU_MBOX=<n>        log n GPU-ASC mailbox messages, both ways, from the first such write
  AURA_GPU_MBOX_STOP=<msg>,<mask>,<ep>[,<after>]  and end the log at the after+1-th message to
                           the GPU that matches (the first work submission)
  AURA_GFXPMP=<ms>         the GPU/PMP record: from the write AURA_GPU_STOP_MSG names, which then
                           doesn't park, ms of every gfx-asc and pmp-asc mailbox message both ways,
                           sgx +0xd60000..+0xd60067, +0x1000104, +0x1000108 writes and the GFX PS
                           word's writes; the MMIO ring stops at its end (aura/hvgfxrec.py reads both
                           at a park). Needs AURA_GPU_TRACE; refuses anything that parks the guest
  AURA_GFXPMP_MAX=<n>      its records (default 262144, 32 bytes each)
  AURA_GFXPMP_BLK=<pa>,<len>  the block whose every access (SPTM's and XNU's, from the first trap
                           to the window's end) the record keeps apart (default sgx +0xe00000,
                           0x4000: MMIO_PROTECTED); AURA_GFXPMP_BLK_MAX=<n> its records (131072)
  AURA_SAMPLE              sample the guest's PC until each core's XNU entry (pre-XNU hangs)
  AURA_SAMPLE_HZ=<n>       samples per second (default 4)
  AURA_SAMPLE_MAX=<n>      stop printing after n samples (default 64)
  AURA_XNU_ENTRY           patch the guest kernelcache's acct(2) so a root process reaches aura's
                           control hvc with no kext (the auractl "xnu" entry; aura/auractl_xnu.py)
  AURA_XNU_RO_RELAX        patch XNU's zalloc_ro_mut RO-zone panic out (tracing only; XNU_QUIRKS)
  AURA_XNU_TC_FORCE        accept a trust cache TXM refused, so the cryptex graft proceeds
  AURA_SHARE_NVRAM         let the guest reach the real NVRAM (a failed boot pollutes it)
  AURA_XNU_TASK_KEYS       keep XNU's per-task PAC keys (its kernel frames then fail, XNU_QUIRKS)
  AURA_DEBUG_PORT=<n>      with the proxy on KIS, hide only USB-C port n (port-number), not all
  AURA_SPTM_DIAG           give SPTM a diagnostic record (boot_args revision 3, SPTMDebug)
  AURA_OS_LONG_VERSION     the guest's Ap,OSLongVersion (--os-long-version)
  AURA_KEEP_BOOT_VIDEO     hand iBoot's boot framebuffer to XNU, aura's console off it
  AURA_TAIL_SECS, AURA_TAIL_IDLE   keep reading the console after entry, until idle; a park
                           (the HV's START) ends the tail at once and says whether it resumes
  AURA_PARK_AFTER_SECS=<n> while tailing, ask the guest to park n s after entry (as SIGUSR1 to
                           this process does at any time); then tools/sptm_park.py --continue
  AURA_XNU_PATCH_LOG=<f>   record every XNU word patched (quirk, PA, original, patch) in f, as
                           JSON: sptm_park.py --restore-nvram-write --patch-log f takes it
  AURA_AMCC_NOLOCK=<mask>  which of SPTM's AMCC range locks aura shadows (bit 0 +0xacc, 1 +0xadc,
                           2 +0xaec; default 1, the one over aura's image; 0 lets SPTM lock all)
  AURA_WFI_WATCH=1|2       trap XNU's WFIs (2: and WFEs) and report every 10 s (idle vs. dead)
  AURA_HIDE_NODES=<a,b>    further ADT nodes to hide from the guest (comma-separated paths)
  AURA_RG_XWDT=0           leave the exclave watchdog's page unhooked (the reset guard skips it)
  AURA_KEEP_ANE            keep /arm-io/ane and dart-ane (the exclaves' manifests name its mappers)
  AURA_KEEP_WIFI           keep the kernelcache workaround's PCIe/Wi-Fi/Bluetooth nodes (not the
                           debug port's): WindowServer waits on TimeSync, which needs a network port
  AURA_TRAP_DELAY_NS=<n>   spin n ns in every GL trap (workaround: holds off the ~20 s stall)
  AURA_TRAP_DELAY_MASK=<m> which GL traps spin: 1 GEXIT, 2 g-ERET, 4 IMPDEF, 8 SPTM's entry
                           sysreg read, 16 HVC, 32 rest, 64 other sysregs
  AURA_SYSREG_CENSUS       print which sysregs the guarded world traps on, every 20 s
  AURA_S2R_LIVE            print the guest's writes to the hooked power blocks as they happen
  AURA_CPTR_LIVE=0         keep SPTM's CPTR_EL2 writes a shadow (default: XNU's CPACR_EL1)
  AURA_ANS_COLD=<mode>     ANS for XNU to start cold: sleep, idle or off (default: sleep on
                           T8140 and for an ANS an earlier stage powered off, else off)
  AURA_ANS_PROBE=0         skip the cold handoff's HELLO check
  AURA_ANS_SART=<v>        the nvme.sart= value tools/ans_sart.py printed on the stage that
                           powered ANS off (route: the host chainloads this image from its proxy)
  XNU_BOOTARGS             XNU's command line

Example:
  export PYTHONPATH=proxyclient
  python3 proxyclient/tools/run_guest_sptm.py --sptm sptm.bin --txm txm.bin \\
      --xnu kernelcache.decompressed --trustcache iboot.tc --dry-run
"""

import argparse
import os
import pathlib
import secrets
import struct
import subprocess
import sys
import time

# wdt=-1 keeps XNU from arming the SoC watchdog. -vm_compressor_wk_sw avoids the SVE WKdm
# compressor (an exception to EL2 clears PSTATE.SM). sprr_tpro*=0 drop the TPRO policy.
# -noprogress: no framebuffer is handed over, and the boot-progress layout loops on a zero
# scale. disable-uart-irq=1 keeps AppleSerialShim off uart0's interrupt; the polled console
# needs none.
DEFAULT_BOOTARGS = ("serial=3 disable-uart-irq=1 -v -nobsdmgroot wdt=-1 sprr_tpro=0 "
                    "sprr_tpro_pagers=0 msgbuf=1048576 -vm_compressor_wk_sw CAEnableMedina=0 "
                    "-noprogress")

# Configuration flags, sptm_virt.h SPTM_VIRT_*
CONFIG_FLAGS = {
    "AURA_S2R_TRACE": 1 << 0,
    "AURA_S2R_GO": 1 << 1,
    "AURA_S2R_FULLTRACE": 1 << 2,
    "AURA_S2R_COPROC": 1 << 3,
    "AURA_S2R_DROP_USB": 1 << 4,
    "AURA_XNU_BT": 1 << 5,
    "AURA_GPU_TRACE": 1 << 6,
    "AURA_SAMPLE": 1 << 7,
    "AURA_S2R_MEM": 1 << 8,
    # bit 9 is reserved for AURA_AURACTL in the feat/auractl runner
    "AURA_AURACTL_DCC": 1 << 10,  # arm auractl's kext-free DCC transport (off by default)
}
KNOWN_ENV = {
    "AURA_S2R_ARM_S",
    "AURA_S2R_WIN",
    "AURA_S2R_PROBE",
    "AURA_S2R_PREWRITE",
    "AURA_S2R_PROBE_AT",
    "AURA_S2R_CRUMBS",
    "AURA_S2R_EXTRA",
    "AURA_AURA_HEAP_MIB",
    "AURA_KEEP_EXCLAVES",
    "AURA_OS_LONG_VERSION",
    "AURA_KEEP_BOOT_VIDEO",
    "AURA_EXCLAVE_FW",
    "AURA_EXCLAVE_BUNDLE",
    "AURA_EXCLAVE_INTEGRITY",
    "AURA_EXCLAVE_TRUSTCACHE",
    "AURA_XNU_SERIAL",
    "AURA_HIDE_DOCKCHANNEL",
    "AURA_KEEP_ERRH",
    "AURA_AMCC_CLEAR",
    "AURA_S2R_QUIESCE",
    "AURA_S2R_WAKE_S",
    "AURA_S2R_GO_COUNT",
    "AURA_S2R_CP_NO_ANS",
    "AURA_S2R_CP_ANSX",
    "AURA_S2R_DCP_PKT",
    "AURA_S2R_USB_RETAKE",
    "AURA_S2R_USB_RETAKE_MS",
    "AURA_TAIL_SECS",
    "AURA_TAIL_IDLE",
    "AURA_PARK_AFTER_SECS",
    "AURA_XNU_PATCH_LOG",
    "AURA_GPU_STOP_SECS",
    "AURA_GPU_STOP_BOOT_SECS",
    "AURA_GPU_STOP_MSG",
    "AURA_GPU_STOP_MSG_MASK",
    "AURA_GPU_STOP_EP",
    "AURA_GPU_PRINT_MAX",
    "AURA_GPU_MSG_PARK",
    "AURA_GPU_SNAP",
    "AURA_GPU_SNAP_MAX",
    "AURA_GPU_SNAP_MIB",
    "AURA_GPU_SNAP_PARTS",
    "AURA_GPU_SNAP_ROOTS",
    "AURA_GPU_SNAP_CHAINS",
    "AURA_GPU_SNAP_WATCH",
    "AURA_GPU_MBOX",
    "AURA_GPU_MBOX_STOP",
    "AURA_GFXPMP",
    "AURA_GFXPMP_MAX",
    "AURA_GFXPMP_BLK",
    "AURA_GFXPMP_BLK_MAX",
    "AURA_SAMPLE_HZ",
    "AURA_SAMPLE_MAX",
    "AURA_XNU_ENTRY",
    "AURA_XNU_RO_RELAX",
    "AURA_SHARE_NVRAM",
    "AURA_XNU_TC_FORCE",
    "AURA_DEBUG_PORT",
    "AURA_SPTM_DIAG",
    "AURA_AMCC_NOLOCK",
    "AURA_WFI_WATCH",
    "AURA_HIDE_NODES",
    "AURA_RG_XWDT",
    "AURA_KEEP_ANE",
    "AURA_KEEP_WIFI",
    "AURA_TRAP_DELAY_NS",
    "AURA_TRAP_DELAY_MASK",
    "AURA_CPTR_LIVE",
    "AURA_SYSREG_CENSUS",
    "AURA_S2R_LIVE",
    "AURA_ANS_COLD",
    "AURA_ANS_PROBE",
    "AURA_ANS_DROP",
    "AURA_ANS_SART",
} | set(CONFIG_FLAGS)

# The debug link's USB-C port: XNU must never drive it (it resets the port about 3 s into IOKit
# and the link drops). From its PHY, the nodes that belong to the port follow from the ADT's
# phandles: forward from a node of the port through FWD, back to it through REV.
PORT_FWD = ("atc-phy-parent", "atc-phy", "dpxbar-parent", "acio-parent", "acio-cpu", "iommu-parent",
            "apciec-piodma", "usb-repeater")
PORT_REV = ("atc-phy-parent", "atc-phy", "acio-parent", "usb-port", "dpxbar-parent")
# Nothing drives the ANE yet, and the platform error handler panics on an AMCC DSID timeout.
STRIP_NODES = ("/arm-io/ane", "/arm-io/dart-ane")
# The guest has the real NVRAM, and macOS writes it: a boot that fails in launchd leaves
# recovery-boot-mode behind, after which iBoot ignores the enrolled boot object and the machine
# comes up in recovery until someone clears it by hand -- so one failed guest boot costs a trip
# through 1TR. Nothing here needs NVRAM (the boot arguments come through boot_args), so the device
# is hidden: macOS reports "platform does not support NVRAM" and its writes go nowhere.
# Hiding the device outright stalls the guest's IOKit at the display DART, so the writes are
# patched out in XNU instead (the nvram_ro quirk); this is kept for reference.
HOST_STATE_NODES = ("/arm-io/qspi/spinor/anvram",)
ERROR_HANDLER = "/arm-io/error-handler"
# Without cL4 the exclaves stay NOT_SUPPORTED, so their nodes and the RTBuddy routes that resolve
# through them would wait forever in IOKit. With cL4 they must stay: hidden, DCP never completes
# its sleep power state.
EXCLAVE_NODES = ("/arm-io/exdisplaypipe", "/arm-io/exdisplaypipe-s-proxy",
                 "/arm-io/dcp-exclave-ioreporting", "/arm-io/dcp-exclave-mailbox",
                 "/arm-io/aop-exclave-mailbox", "/arm-io/aop2-exclave-mailbox")
ROUTES_PROPS = (("/arm-io/dcp/iop-dcp-nub", "routes"), ("/arm-io/aop/iop-aop-nub", "routes"),
                ("/arm-io/aop2/iop-aop2-nub", "routes"))
# Without it XNU powers ANS on, maps its firmware and sets RUN itself, instead of waking a
# running session.
ANS_QUIESCED = (("/arm-io/ans/iop-ans-nub", "quiesced"),)
# iBoot's data regions copied to the blob tail, in iBoot's order (the native layout).
IBOOT_COPIES = ("RTBuddySeg", "SEPFW", "SEPPatches", "uStuff", "preoslog")

NOP = 0xd503201f

# Workarounds for one kernelcache, each keyed on its XNU release string and a fingerprint: an
# instruction at a place in its text. One release ships different kernelcaches per SoC, and only
# the one that matches gets the entry: what its guest must not see, and optional patches for
# tracing (AURA_XNU_RO_RELAX).
# J813's xnu-13432.1.9 (macOS 27.0, 26A428) panics in zalloc_ro_mut on AppleConvergedPCI's statics
# about a second into IOKit, and again in pid 1 a second into userspace. Nothing here needs PCIe
# or Wi-Fi, so their nodes are hidden. The RO-relax patch takes the branch that skips the second
# panic; it changes what the guest does, so it is never on by default. Its branch is the
# fingerprint. J714s's kernelcache of the same release has none of this and keeps Wi-Fi.
XNU_QUIRKS = (
    dict(release=b"xnu-13432.1.9~", fingerprint=("__TEXT_EXEC", 0x3b27e4c, 0x540002a9),
         hide=("/arm-io/apcie", "/arm-io/dart-apcie0", "/arm-io/apcie0-piodma",
               "/arm-io/apciec0", "/arm-io/dart-apciec0", "/arm-io/apciec0-piodma",
               "/arm-io/centauri", "/arm-io/bluetooth", "/arm-io/aop-spmi0/spmi-wifibt"),
         ro_relax=(("zalloc_ro_mut RO-zone check", "BootKC-rx", 0x3b27e4c, 0x540002a9,
                    0x14000015),),
         # XNU takes its read-only region from CTRR_D_LWR/UPR_EL2 (rorgn_begin = LWR,
         # rorgn_end = UPR | 0xfff), which iBoot locks over the kernelcache natively. Nothing in
         # the guest programs them, and a non-guarded EL1 read does not reach the bank aura can
         # write, so XNU would see an empty region and reject every zalloc_ro_mut whose source is
         # a kernel static. Skip its register read (the flag byte's branch, taken unconditionally)
         # and fill the two globals it would have written, with the BootKC's text and const span.
         rorgn=(("rorgn register read", "BootKC-rx", 0x336f1c8, 0x370001a8, 0x1400000d),),
         rorgn_globals=(0xefc020, 0xefc028),
         # AppleImage4's storageInit asks the registered expert whether mix-and-match is
         # allowed; the slot it calls through is zero here (and in the image), so it decides
         # "platform requires immutable NVRAM [Deconflicting]", which leaves its nonce
         # magazines without a slot. No cryptex1 boot nonce means TXM refuses the cryptex
         # trust cache and launchd's graft ends the boot. Skip the question (cbz w0 -> b) and
         # let the ordinary /chosen/boot-command decide, as it does on a native boot.
         # With the device hidden the guest still reads iBoot's /chosen/nvram-proxy-data, so the
         # question is worth skipping either way.
         nvram_mutable=(("Image4 storageInit mix-and-match question", "BootKC-rx",
                         0x2bc0a04, 0x34000080, 0x14000022),),
         # AURA_XNU_TC_FORCE. The cryptex graft loads cryptex1/current/os.dmg.trustcache through
         # TXM, which refuses it: the manifest is the reduced-security one and its chip's nonce
         # slot does not exist here (CHIP_CRYPTEX1_BOOT_AP_REDUCED, tag c1br), so launchd gives
         # up and the boot ends in recovery. The static trust cache this host passes is that same
         # cryptex's, so its binaries are trusted either way -- take the success path (cbz w0 -> b)
         # and let the graft proceed. The guest then accepts a trust cache its supervisor refused:
         # a tracing switch, never on by default.
         tc_force=(("trust-cache load result", "BootKC-rx",
                    0x37e1fd8, 0x340000e0, 0x14000007),),
         # The guest needs a working NVRAM -- hiding the device stalls its IOKit, and Image4's
         # nonce slots live there -- but nothing it writes should reach the flash, or a failed
         # boot leaves recovery-boot-mode behind and the machine stops booting aura. Make
         # AppleARMNORNVRAM::write return success without writing (keeping its BTI landing pad),
         # so the guest keeps its in-memory NVRAM and the host's copy is untouched.
         nvram_ro=(("AppleARMNORNVRAM::write", "BootKC-rx", 0x1c1bec, 0xaa0003e8, 0x52800000),
                   ("AppleARMNORNVRAM::write return", "BootKC-rx", 0x1c1bf0, 0x52805840,
                    0xd65f03c0)),
         # XNU reprogrammes APIBKey/APDBKey_EL1 (plus s3_4_c15_c13_6/7) at every context switch
         # to the incoming task's diversifier. On real hardware that is harmless to the kernel:
         # it runs at EL2, whose pointer-authentication keys are a different bank from the EL1
         # one those writes land in. Here the kernel runs at physical EL1, so the register it
         # rewrites for userspace is the one its own pacibsp/retab use, and any kernel frame
         # whose signing and authentication straddle a context switch fails -- which is what
         # "PAC failure from kernel with IB key while returning" in the SPTM call stub is: the
         # key, not the stack pointer or the return address, is what changed.
         # Each of the four sites guards the key block with "b.eq past it" for a task whose key
         # is already current; make that branch unconditional, so every task keeps the key the
         # kernel signs with. All of the guest's tasks then share one key: a weaker guest than
         # hardware gives, which is the price of the collapsed level.
         pac_keys=(("task PAC key reprogramming (switch)", "BootKC-rx", 0x314834c, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (2)", "BootKC-rx", 0x3148414, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (3)", "BootKC-rx", 0x3148614, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (4)", "BootKC-rx", 0x31488c4, 0x540001e0,
                    0x1400000f))),
    # T6041's xnu-13432.41.4 (macOS 27.2.1, 26B131, kernelcache.release.mac16j). Its guest needs
    # only nvram_ro: about 27 s in, while APFS mounts the volumes, AppleARMNORNVRAM sends the NOR a
    # write enable. The same two words as the J813's, at this kernelcache's offsets; the first is
    # the fingerprint. Without exclaves its single-user boot reaches the shell with no other patch.
    dict(release=b"xnu-13432.41.4~", fingerprint=("__TEXT_EXEC", 0x1bdf0c, 0xaa0003e8),
         hide=(), ro_relax=(),
         nvram_ro=(("AppleARMNORNVRAM::write", "BootKC-rx", 0x1bdf0c, 0xaa0003e8, 0x52800000),
                   ("AppleARMNORNVRAM::write return", "BootKC-rx", 0x1bdf10, 0x52805840,
                    0xd65f03c0))),
    # T8142's xnu-13432.41.4 (macOS 27.2.1 26B131, kernelcache.release.mac17g; 27.2's 26B123
    # ships the same XNU release but its kernelcache is not checked). Every site above, ported
    # from 26A428: each
    # one's 48-instruction window (PC-relative immediates, and where needed the adrp-paired
    # imm12, masked) matches exactly once here. The two twin PAC-key guards match twice in both
    # kernelcaches and are paired in order. The branch replacements are re-encoded for this
    # kernelcache, and the rorgn globals come from the matched function's own adrp/str pairs.
    # The fingerprint is the RO-zone branch: T6041's kernelcache of this release does not match it.
    dict(release=b"xnu-13432.41.4~", fingerprint=("__TEXT_EXEC", 0x3b53c20, 0x540002a9),
         hide=("/arm-io/apcie", "/arm-io/dart-apcie0", "/arm-io/apcie0-piodma",
               "/arm-io/apciec0", "/arm-io/dart-apciec0", "/arm-io/apciec0-piodma",
               "/arm-io/centauri", "/arm-io/bluetooth", "/arm-io/aop-spmi0/spmi-wifibt"),
         ro_relax=(("zalloc_ro_mut RO-zone check", "BootKC-rx", 0x3b53c20, 0x540002a9,
                    0x14000015),),
         rorgn=(("rorgn register read", "BootKC-rx", 0x339a4d0, 0x370001a8, 0x1400000d),),
         rorgn_globals=(0xf0c020, 0xf0c028),
         nvram_mutable=(("Image4 storageInit mix-and-match question", "BootKC-rx",
                         0x2be6254, 0x34000080, 0x14000022),),
         tc_force=(("trust-cache load result", "BootKC-rx",
                    0x380fae4, 0x340000e0, 0x14000007),),
         nvram_ro=(("AppleARMNORNVRAM::write", "BootKC-rx", 0x1c3d9c, 0xaa0003e8, 0x52800000),
                   ("AppleARMNORNVRAM::write return", "BootKC-rx", 0x1c3da0, 0x52805840,
                    0xd65f03c0)),
         pac_keys=(("task PAC key reprogramming (switch)", "BootKC-rx", 0x317034c, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (2)", "BootKC-rx", 0x3170414, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (3)", "BootKC-rx", 0x3170614, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (4)", "BootKC-rx", 0x31708c4, 0x540001e0,
                    0x1400000f))),
    # T8132's xnu-13432.41.4 (kernelcache.release.mac16g, the J713's and the J773g's), in macOS 27.2
    # (26B123) and 27.2.1 (26B131). 26B131's differs from 26B123's only in the code of a few file
    # system kexts (nfs to webdav, __TEXT_EXEC +0x3068000 to +0x3132000): every site below, the
    # 0x200 bytes around it, and the rorgn globals are the same in both, so one entry serves both.
    # Every site of the T8142 entry above, ported from 26B131's mac17g. The rorgn read,
    # the trust-cache result and the NVRAM write match their 48-instruction windows exactly once,
    # as above. T8132's structs are smaller and it has no MTE, so the mix-and-match question (48)
    # and the PAC-key guards (24, the twins paired in order) match only with the struct offsets
    # masked as well (ldr/str and ldp/stp immediates, add/sub imm12). The RO-zone branch, whose
    # source here is not tag-stripped, is the b.ls between the two panics of zalloc_ro_mut (lines
    # 6847 and 6730), with the same 21-instruction skip. The replacements are the T8142 words:
    # every branch distance is unchanged. The rorgn globals come from the function's own adrp/str
    # pairs. Nothing is hidden: with the read-only region in place the AppleConvergedPCI panic
    # doesn't come back (AURA_KEEP_WIFI's finding), WindowServer needs a network port, and a
    # Mac mini's only link to the host is its PCIe Ethernet.
    dict(release=b"xnu-13432.41.4~", fingerprint=("__TEXT_EXEC", 0x3acaa78, 0x540002a9),
         hide=(),
         ro_relax=(("zalloc_ro_mut RO-zone check", "BootKC-rx", 0x3acaa78, 0x540002a9,
                    0x14000015),),
         rorgn=(("rorgn register read", "BootKC-rx", 0x331ade8, 0x370001a8, 0x1400000d),),
         rorgn_globals=(0xedc020, 0xedc028),
         nvram_mutable=(("Image4 storageInit mix-and-match question", "BootKC-rx",
                         0x2bbaed4, 0x34000080, 0x14000022),),
         tc_force=(("trust-cache load result", "BootKC-rx",
                    0x378c1c8, 0x340000e0, 0x14000007),),
         nvram_ro=(("AppleARMNORNVRAM::write", "BootKC-rx", 0x1ab73c, 0xaa0003e8, 0x52800000),
                   ("AppleARMNORNVRAM::write return", "BootKC-rx", 0x1ab740, 0x52805840,
                    0xd65f03c0)),
         pac_keys=(("task PAC key reprogramming (switch)", "BootKC-rx", 0x3138284, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (2)", "BootKC-rx", 0x3138334, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (3)", "BootKC-rx", 0x3138520, 0x540001e0,
                    0x1400000f),
                   ("task PAC key reprogramming (4)", "BootKC-rx", 0x313877c, 0x540001e0,
                    0x1400000f))),
)


def kernelcache_word(kernelcache, seg, off):
    from aura.sptm_virt_plan import macho_info

    try:
        s = macho_info(kernelcache).segs[seg]
    except (KeyError, ValueError, struct.error):
        return None
    if off + 4 > min(s.vmsize, s.filesize):
        return None
    return struct.unpack_from("<I", kernelcache, s.fileoff + off)[0]


def xnu_quirks(kernelcache):
    for q in XNU_QUIRKS:
        seg, off, word = q["fingerprint"]
        if q["release"] in kernelcache and kernelcache_word(kernelcache, seg, off) == word:
            return q["release"].decode().rstrip("~"), q
    return None, dict(hide=(), ro_relax=())


# AMCC error status words: seven per plane, at +0x25004 of the AMCC the mcc node's
# amcc-reg-idx names, a plane every 0x40000.
AMCC_ERR_STATUS = 0x25004
AMCC_PLANE_STRIDE = 0x40000


def env_on(name):
    return os.environ.get(name) == "1"


def env_int(name, default=0):
    value = os.environ.get(name)
    return int(value, 0) if value else default


def aura_heap_size(adt):
    override = os.environ.get("AURA_AURA_HEAP_MIB")
    if override is not None:
        return int(override) * 1024 * 1024
    # The J813's padded EL2 boot object leaves a 160 MiB carve-out, with about
    # 33 MiB already used before the host reserves space for aura's own heap.
    j813 = adt["/chosen"].chip_id == 0x8142 and getattr(adt, "model", None) == "Mac17,3"
    return (112 if j813 else 128) * 1024 * 1024


def parse_args():
    ap = argparse.ArgumentParser(description="Boot the stock SPTM as an NV2 guest under aura")
    ap.add_argument("--sptm", type=pathlib.Path, required=True, help="sptm.bin (flat Mach-O)")
    ap.add_argument("--txm", type=pathlib.Path, required=True, help="txm.bin (flat Mach-O)")
    ap.add_argument("--xnu", type=pathlib.Path, required=True, help="decompressed kernelcache")
    ap.add_argument("--os-long-version", default=os.environ.get("AURA_OS_LONG_VERSION"),
                    help="the guest's Ap,OSLongVersion from its BuildIdentity, for TXM's cryptex "
                         "check when the live tree has none (AURA_OS_LONG_VERSION)")
    ap.add_argument("--trustcache", type=pathlib.Path, default=None,
                    help="iBoot's trust cache container, or one raw trust cache (default: empty)")
    ap.add_argument("--bootargs", default=os.environ.get("XNU_BOOTARGS", DEFAULT_BOOTARGS),
                    help="XNU's command line (env XNU_BOOTARGS)")
    ap.add_argument("--elf", type=pathlib.Path, default=pathlib.Path("build/aura-raw.elf"),
                    help="aura's raw ELF, for symbols")
    ap.add_argument("--nm", default="llvm-nm", help="an nm that reads the ELF")
    ap.add_argument("--dry-run", action="store_true", help="plan and print only; write nothing")
    ap.add_argument("--no-enter", action="store_true", help="load and record; do not enter")
    # Older launchers pass it; SPTM's window always stays mapped in aura.
    ap.add_argument("--no-unmap", action="store_true", help=argparse.SUPPRESS)
    return ap.parse_args()


class Symbols:
    """aura symbols at their run addresses: run base + (nm(sym) - nm(_base))."""

    def __init__(self, nm, elf, run_base):
        self.syms = {}
        for line in subprocess.check_output([nm, str(elf)], text=True).splitlines():
            parts = line.split()
            if len(parts) == 3:
                self.syms[parts[2]] = int(parts[0], 16)
        self.run_base = run_base
        self.link_base = self.syms["_base"]

    def __getitem__(self, name):
        return self.run_base + (self.syms[name] - self.link_base)


# Functions whose code the running image must share with --elf before the first call into it.
IMAGE_CHECK_FUNCS = ("sptm_virt_place", "sptm_virt_config", "sptm_virt_load")
IMAGE_CHECK_BYTES = 64


def elf_load_bytes(elf, vaddr, size):
    """size bytes at vaddr in the ELF's PT_LOAD segments (as linked, before relocation)."""
    data = pathlib.Path(elf).read_bytes()
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _flags, off, va, _pa, filesz, _memsz, _align = struct.unpack_from(
            "<IIQQQQQQ", data, phoff + i * phentsize)
        if p_type == 1 and va <= vaddr and vaddr + size <= va + filesz:
            return data[off + vaddr - va:off + vaddr - va + size]
    return None


def check_running_image(iface, sym, elf):
    """Refuse a proxy that isn't running the image in --elf. A host that chainloaded an image
    and then reached another proxy (the image reset the SoC and an earlier stage came back) would
    otherwise call that stage at this image's offsets. Code is position-independent, so the
    running bytes equal the ELF's. Nothing is called or written before this passes."""
    for name in IMAGE_CHECK_FUNCS:
        want = elf_load_bytes(elf, sym.syms[name], IMAGE_CHECK_BYTES)
        if want is None:
            raise SystemExit(f"{name} isn't in a loadable segment of {elf}")
        got = iface.readmem(sym[name], IMAGE_CHECK_BYTES)
        if got != want:
            raise SystemExit(f"the proxy at base 0x{sym.run_base:x} isn't running {elf}: the code "
                             f"at {name} differs. Stopping before any call into it.")
    print(f"running image matches {elf} at base 0x{sym.run_base:x}")


def ans_drop_props():
    """AURA_ANS_DROP=quiesced,running: ANS was powered off and back with its CPU stopped, so XNU
    starts it as after a sleep instead of waking iBoot's quiesced session."""
    names = [n for n in os.environ.get("AURA_ANS_DROP", "").split(",") if n]
    return tuple(("/arm-io/ans/iop-ans-nub", n) for n in names)


def iboot_copies(adt):
    mm = adt["/chosen/memory-map"]._properties
    copies = []
    for name in IBOOT_COPIES:
        try:
            pa, size = int(mm[name][0]), int(mm[name][1])
        except (KeyError, IndexError, TypeError, ValueError):
            continue
        if size and pa != 0xFFFFFFFFFFFFFFFF and not pa & 0x3fff:
            copies.append((name, pa, size))
            print(f"iBoot {name} 0x{pa:x}+0x{size:x} -> copied to the blob tail")
    return copies


def check_oslog(adt, aura_base, aura_end):
    """aura runs at stage 1's base, the only place a secondary core may execute, and the display
    coprocessors log into __OS_LOG buffers iBoot placed right after stage 1. aura's .bss and stacks
    must end below them, or the DCP overwrites aura's state."""
    overlap = False
    for owner in ("dcp", "dcpext", "dcpext0", "dcpext1"):
        try:
            raw = adt[f"/arm-io/{owner}/iop-{owner}-nub"]._properties.get("segment-ranges")
        except KeyError:
            continue
        for i in range(len(raw or b"") // 32):
            phys, _iova, _remap, size, flags = struct.unpack_from("<QQQII", raw, 32 * i)
            if flags != 0xA:  # __OS_LOG
                continue
            ov = phys < aura_end and phys + size > aura_base
            overlap |= ov
            print(f"{owner} __OS_LOG phys 0x{phys:x} +0x{size:x} (aura +0x{phys - aura_base:x})"
                  + (f"  OVERLAPS aura [0x{aura_base:x}, 0x{aura_end:x})" if ov else ""))
    if overlap:
        raise SystemExit("aura's .bss/stacks overlap a DCP __OS_LOG buffer: shrink .bss")


def exclave_cl4(plan_mod, bundle, kernel_bytes=None):
    """The cL4 kernel's entry and virtual base. The bundle carries its SYSTEM kernel's Mach-O
    header at the start of its read-only region, so an extracted copy of that file is optional."""
    if kernel_bytes is None:
        kernel_bytes = bundle.payload[bundle.regions["CL4-ro"][0]:]
    kernel = plan_mod.macho_info(kernel_bytes)
    return kernel.entry, kernel.virtlo


def exclave_report(bundle, entry, virt, integrity, trustcache):
    print(f"exclaves: cL4 bundle 0x{len(bundle.payload):x} entry 0x{entry:x} virt 0x{virt:x}; "
          f"exic 0x{len(integrity):x} extc 0x{len(trustcache):x}; regions "
          + " ".join(f"{k}=0x{v[1]:x}" for k, v in bundle.regions.items())
          + "; components " + " ".join(f"{c.name}({c.kind})" for c in bundle.components))
    return dict(cl4_regions=bundle.regions, cl4_entry=entry, cl4_virt=virt,
                exclave_integrity=integrity, exclave_trustcache=trustcache), bundle.payload


def exclave_firmware(fw_dir, plan_mod, chip):
    """The extracted exclave firmware of a macOS install: this SoC's ExclaveCore bundle directory
    and the build's Firmware directory beside it. An install carries one bundle per SoC."""
    from aura.exclave_bundle import parse as parse_bundle

    bdir = fw_dir / f"exclavecore_bundle.t{chip:x}.RELEASE"
    if not bdir.is_dir():
        raise SystemExit(f"AURA_EXCLAVE_FW: no {bdir.name} in {fw_dir}")
    bundle = parse_bundle((bdir / bdir.name).read_bytes())
    kernel_file = bdir / "SYSTEM" / "kernel"
    entry, virt = exclave_cl4(plan_mod, bundle,
                              kernel_file.read_bytes() if kernel_file.exists() else None)
    fw = next(fw_dir.glob("*__MacOS")) / "Firmware"
    return exclave_report(bundle, entry, virt,
                          (fw / "exclave.integrity_catalog.bin").read_bytes(),
                          (fw / "exclave.trustcache.bin").read_bytes())


def exclave_files(plan_mod):
    """The three IPSW members directly: the ExclaveCore bundle (Ap,cL4), the ExclaveOS integrity
    catalog and its trust cache, each an IM4P container as shipped."""
    from aura.exclave_bundle import im4p_payload, parse as parse_bundle

    bundle = parse_bundle(pathlib.Path(os.environ["AURA_EXCLAVE_BUNDLE"]).read_bytes())
    entry, virt = exclave_cl4(plan_mod, bundle)
    blobs = []
    for env in ("AURA_EXCLAVE_INTEGRITY", "AURA_EXCLAVE_TRUSTCACHE"):
        path = os.environ.get(env)
        blobs.append(im4p_payload(pathlib.Path(path).read_bytes()) if path else b"")
    return exclave_report(bundle, entry, virt, *blobs)


def write_plan(p, u, iface, sym, plan, images):
    # Device-side copies first (iBoot's data regions), refusing any whose source a load overwrites.
    copies = [ld for ld in plan.loads if ld.image == "copy"]
    for ld in copies:
        for other in plan.loads:
            if other is not ld and other.pa < ld.fileoff + ld.size and ld.fileoff < other.pa + other.total:
                raise SystemExit(f"{other.what} at 0x{other.pa:x} overwrites the source of {ld.what}")
    for ld in copies:
        print(f"copying {ld.what:22s} 0x{ld.fileoff:x} -> 0x{ld.pa:x} (0x{ld.size:x})")
        if not (ld.pa | ld.fileoff | ld.size) & 7:
            p.memcpy64(ld.pa, ld.fileoff, ld.size)
        else:
            p.memcpy8(ld.pa, ld.fileoff, ld.size)
        if ld.total > ld.size:
            iface.writemem(ld.pa + ld.size, bytes(ld.total - ld.size))
        p.call(sym["sptm_virt_flush"], ld.pa, ld.total)
    for ld in plan.loads:
        if ld.image == "copy":
            continue
        data = ld.payload if ld.image == "bytes" else images[ld.image][ld.fileoff:ld.fileoff + ld.size]
        data = data.ljust(ld.total, b"\0")
        print(f"writing {ld.what:22s} -> 0x{ld.pa:x} (0x{len(data):x})")
        u.compressed_writemem(ld.pa, data, True)
        p.call(sym["sptm_virt_flush"], ld.pa, len(data))

    print(f"writing device tree -> 0x{plan.adt_pa:x}")
    iface.writemem(plan.adt_pa, plan.adt)
    p.call(sym["sptm_virt_flush"], plan.adt_pa, len(plan.adt))
    print(f"writing boot_args   -> 0x{plan.boot_args_pa:x}")
    iface.writemem(plan.boot_args_pa, plan.boot_args)
    p.call(sym["sptm_virt_flush"], plan.boot_args_pa, len(plan.boot_args))


def rewrite_sptm_seq(p, sym, entry, seq):
    off, want, new = seq
    pa = entry + off
    have = tuple(p.read32(pa + 4 * i) for i in range(len(want)))
    if have != want:
        raise SystemExit(f"MTE tag-storage sequence at 0x{pa:x} is not the one found in the "
                         f"image:\n  have " + " ".join(f"{w:08x}" for w in have) + "\n"
                         f"  want " + " ".join(f"{w:08x}" for w in want))
    for i, w in enumerate(new):
        p.write32(pa + 4 * i, w)
    p.call(sym["sptm_virt_flush"], pa & ~0x3f, 0x80 + len(new) * 4)
    print(f"rewrote {len(new)} instructions at 0x{pa:x} (MTE tag storage from the window)")


# What patch_xnu wrote, for AURA_XNU_PATCH_LOG: the late NVRAM write enable puts nvram_ro's back.
XNU_PATCHES = []


def patch_xnu(p, sym, plan, patches, quirk=None):
    for what, region, off, expected, new in patches:
        if region not in plan.regions:
            raise SystemExit(f"{what}: the plan has no {region} region")
        pa = plan.regions[region][0] + off
        before = p.read32(pa)
        if before != expected:
            raise SystemExit(f"{what}: [0x{pa:x}] = 0x{before:08x}, expected 0x{expected:08x}: "
                             f"not the kernelcache this patch is for")
        p.write32(pa, new)
        p.call(sym["sptm_virt_flush"], pa & ~0x3f, 0x80)
        print(f"XNU {what}: [0x{pa:x}] 0x{before:08x} -> 0x{p.read32(pa):08x}")
        XNU_PATCHES.append(dict(quirk=quirk, what=what, region=region, off=off, pa=pa,
                                original=before, patched=new))


def write_patch_log():
    path = os.environ.get("AURA_XNU_PATCH_LOG")
    if path:
        import json

        pathlib.Path(path).write_text(json.dumps(XNU_PATCHES, indent=1) + "\n")
        print(f"XNU patches recorded in {path} ({len(XNU_PATCHES)} words)")


def patch_xnu_entry(p, sym, plan, kernelcache):
    """AURA_XNU_ENTRY: patch the guest kernelcache's acct(2) so a root process reaches aura's
    control hvc with no kext (the auractl "xnu" entry, aura/auractl_xnu.py). The site is located in
    the kernelcache file, build-independently and fail-closed; the words it overwrites are verified
    against the loaded BootKC before the write, so a mismatched kernelcache stops the boot."""
    from aura.auractl_xnu import AURACTL_HVC_IMM, XnuEntryError, find_acct_entry

    try:
        entry = find_acct_entry(kernelcache)
    except XnuEntryError as e:
        raise SystemExit(f"AURA_XNU_ENTRY: cannot place the acct() entry: {e}")
    base = plan.regions["BootKC-rx"][0]
    site = base + entry["text_off"]
    for i, want in enumerate(entry["expected"]):
        have = p.read32(site + 4 * i)
        if have != want:
            raise SystemExit(f"AURA_XNU_ENTRY: acct site [0x{site + 4 * i:x}] = 0x{have:08x}, the "
                             f"finder read 0x{want:08x} from the file: wrong kernelcache loaded")
    for i, new in enumerate(entry["replacement"]):
        p.write32(site + 4 * i, new)
    p.call(sym["sptm_virt_flush"], site & ~0x3f, 0x80 + len(entry["replacement"]) * 4)
    print(f"XNU auractl entry: acct() 0x{entry['acct_va']:x}, 5 instructions at 0x{site:x} "
          f"(uap in x{entry['uap_reg']}) -> hvc #0x{AURACTL_HVC_IMM:x}, b 0x{entry['epilogue_va']:x}")


def set_xnu_rorgn(p, sym, plan, quirks):
    """Hand XNU the read-only region iBoot would have locked: the BootKC's text and const."""
    lo = plan.regions["BootKC-rx"][0]
    hi = plan.regions["BootKC-ro"][0] + plan.regions["BootKC-ro"][1] - 1
    p.write64(sym["sptm_virt_xnu_rorgn"], lo)
    p.write64(sym["sptm_virt_xnu_rorgn"] + 8, hi & ~0xfff)
    print(f"XNU read-only region 0x{lo:x}-0x{hi:x}")
    patch_xnu(p, sym, plan, quirks["rorgn"], "rorgn")
    base = plan.regions["BootKC-ro"][0]
    for off, value in zip(quirks["rorgn_globals"], (lo, hi)):
        p.write64(base + off, value)
        p.call(sym["sptm_virt_flush"], (base + off) & ~0x3f, 0x80)


def patch_sptm(p, sym, entry, sp):
    for what, off, mask, expected, new in sp["patches"]:
        pa = entry + off
        before = p.read32(pa)
        if before & mask != expected:
            raise SystemExit(f"{what}: [0x{pa:x}] = 0x{before:08x}, expected 0x{expected:08x} "
                             f"under 0x{mask:08x}: not the SPTM these patches are for")
        p.write32(pa, new)
        p.call(sym["sptm_virt_flush"], pa & ~0x3f, 0x80)
        print(f"{what}: [0x{pa:x}] 0x{before:08x} -> 0x{p.read32(pa):08x}")
    if sp["mte"]:
        rewrite_sptm_seq(p, sym, entry, sp["mte"])
    p.write32(sym["sptm_virt_ctrr_stash_off"], sp["ctrr_stash_off"])
    print(f"SPTM: CTRR stash +0x{sp['ctrr_stash_off']:x}")


def amcc_status(adt):
    """(base, planes) of the AMCC error status words, from the mcc node; None without an AMCC."""
    from aura import sptm_virt_plan as plan_mod

    root, _ = plan_mod.adt_parse(adt)
    io = {k: v for k, v, _f in plan_mod.adt_find(root, "arm-io").props}
    child, parent, _ = struct.unpack_from("<QQQ", io["ranges"], 0)
    mcc = {k: v for k, v, _f in plan_mod.adt_find(root, "arm-io/mcc").props}
    if "amcc-reg-idx" not in mcc:
        return None
    u32 = lambda name, dflt: struct.unpack_from("<I", mcc[name])[0] if name in mcc else dflt
    base = struct.unpack_from("<Q", mcc["reg"], 16 * u32("amcc-reg-idx", 0))[0] + parent - child
    return base + AMCC_ERR_STATUS, u32("amcc-count", 1) * u32("dcs-count-per-amcc", 1)


def report_amcc(p, adt, clear):
    """The AMCC error latches stage 1 left: XNU's error handler panics on them at its start."""
    status = amcc_status(adt)
    if not status:
        print("no AMCC in the ADT (mcc amcc-reg-idx); nothing to report")
        return
    base, planes = status
    for plane in range(planes):
        for i in range(7):
            addr = base + plane * AMCC_PLANE_STRIDE + i * 8
            v = p.read32(addr)
            if v:
                print("AMCC plane %d INTSTS%d 0x%x = 0x%08x" % (plane, i, addr, v))
                if clear:
                    p.write32(addr, v)
                    print("  cleared, now 0x%08x" % p.read32(addr))


def _u32(v):
    return struct.unpack_from("<I", v)[0] if v and len(v) >= 4 else None


def debug_port(adt, proxy_phy=0, port=None):
    """The debug link's ports, from the ADT: the PHY aura's proxy runs on, or with the proxy on
    DockChannel (KIS, which any USB-C port with an ATC PHY may carry) every such PHY, or only the
    one whose port-number is `port`. Returns (PHY phandles, the ports' node paths)."""
    from aura import sptm_virt_plan as plan_mod

    root, _ = plan_mod.adt_parse(adt)
    nodes, parent = {}, {}

    def walk(n, path, par):
        here = f"{path}/{n.name()}"
        nodes[here] = {k: v for k, v, _f in n.props}
        parent[here] = par
        for kid in n.kids:
            walk(kid, here, here)

    for kid in root.kids:
        walk(kid, "", None)
    by_ph = {_u32(pr["AAPL,phandle"]): h for h, pr in nodes.items() if "AAPL,phandle" in pr}

    def device(h):  # the /arm-io device, or the device on an I2C or SPMI bus, a node belongs to
        # A port controller can sit on an SPMI bus with the PMU; the bus itself stays.
        bus_child = h.startswith(("/arm-io/i2c", "/arm-io/nub-spmi", "/arm-io/aop-spmi"))
        while h.count("/") > (3 if bus_child else 2):
            h = parent[h]
        return h

    phys = [h for h, pr in nodes.items() if h.count("/") == 2 and h.startswith("/arm-io/") and
            pr.get("device_type", b"").rstrip(b"\0") == b"atc-phy"]
    if proxy_phy:
        phys = [h for h in phys if _u32(nodes[h].get("AAPL,phandle")) == proxy_phy]
    elif port is not None:
        phys = [h for h in phys if _u32(nodes[h].get("port-number")) == port]
    out = set()
    for phy in phys:
        port_nodes = {phy}
        number, kind = _u32(nodes[phy].get("port-number")), _u32(nodes[phy].get("port-type"))
        for h, pr in nodes.items():  # the port's controller (HPM)
            if h.rpartition("/")[2].startswith("hpm") and _u32(pr.get("port-number")) == number \
               and _u32(pr.get("port-type")) == kind:
                port_nodes.add(h)
        # Its retimers name no port; they carry the PHY's index.
        index = phy.rpartition("atc-phy")[2]
        for h, pr in nodes.items():
            if pr.get("compatible", b"").rstrip(b"\0") == b"atcrt" and h.endswith(f"rt{index}"):
                port_nodes.add(h)
        while True:
            before = len(port_nodes)
            for h in list(port_nodes):
                for k in PORT_FWD:
                    t = by_ph.get(_u32(nodes[h].get(k)))
                    if t:
                        port_nodes.add(device(t))
            for h, pr in nodes.items():
                if h.startswith("/arm-io/") and device(h) not in port_nodes and any(
                        device(by_ph.get(_u32(pr.get(k)), "/x")) in port_nodes for k in PORT_REV):
                    port_nodes.add(device(h))
            if len(port_nodes) == before:
                break
        out |= port_nodes
    return [_u32(nodes[h]["AAPL,phandle"]) for h in phys], tuple(sorted(out))


def configure(p, sym, plan, adt, ctl_cookie=0):
    p.write64(sym["sptm_virt_diag_pa"], plan.diag_pa)
    if plan.diag_pa:
        print(f"SPTM diagnostic record at 0x{plan.diag_pa:x}")
    flags = sum(bit for name, bit in CONFIG_FLAGS.items() if env_on(name))
    p.call(sym["sptm_virt_config"], flags)
    if ctl_cookie:
        # The DCC transport's per-boot cookie; the same value is in the guest's boot-args, which
        # only root can read. aura refuses every gated DCC command without it.
        p.call(sym["sptm_virt_ctl_set_cookie"], ctl_cookie)
    for env, symbol in (("AURA_SAMPLE_HZ", "sptm_virt_sample_hz"),
                        ("AURA_SAMPLE_MAX", "sptm_virt_sample_max")):
        if os.environ.get(env):
            p.write32(sym[symbol], env_int(env))
    if os.environ.get("AURA_GPU_STOP_SECS"):
        p.write32(sym["sptm_virt_gpu_stop_secs"], env_int("AURA_GPU_STOP_SECS"))
    if os.environ.get("AURA_GPU_STOP_BOOT_SECS"):
        p.write32(sym["sptm_virt_gpu_stop_boot_secs"], env_int("AURA_GPU_STOP_BOOT_SECS"))
    if os.environ.get("AURA_GPU_STOP_MSG"):
        p.write64(sym["sptm_virt_gpu_stop_msg"], env_int("AURA_GPU_STOP_MSG"))
    if os.environ.get("AURA_GPU_STOP_MSG_MASK"):
        p.write64(sym["sptm_virt_gpu_stop_msg_mask"], env_int("AURA_GPU_STOP_MSG_MASK"))
    if os.environ.get("AURA_GPU_STOP_EP"):
        p.write32(sym["sptm_virt_gpu_stop_ep"], env_int("AURA_GPU_STOP_EP") + 1)
    if os.environ.get("AURA_GPU_PRINT_MAX"):
        p.write32(sym["sptm_virt_gpu_print_max"], env_int("AURA_GPU_PRINT_MAX"))
    from aura import hvsnap

    try:
        hvsnap.configure(p, sym, os.environ)
    except ValueError as e:
        raise SystemExit(f"AURA_GPU_SNAP_*: {e}")
    from aura import hvgfxrec

    hvgfxrec.configure(p, sym, os.environ)
    if os.environ.get("AURA_GFXPMP"):
        p.write32(sym["sptm_virt_gpu_msg_park"], 0)
        print(f"GPU/PMP record: {os.environ['AURA_GFXPMP']} ms from the named write, which doesn't "
              f"park")
    if env_on("AURA_S2R_DCP_PKT"):
        p.write32(sym["sptm_virt_s2r_dcp_pkt"], 1)
        print("coprocessor trace: DCP IOMFB packets copied into the ring")
    if env_on("AURA_S2R_CP_NO_ANS"):
        p.write32(sym["sptm_virt_s2r_cp_skip_ans"], 1)
        print("coprocessor trace: ANS's pages left unhooked")
    if os.environ.get("AURA_S2R_CP_ANSX"):
        p.write32(sym["sptm_virt_s2r_cp_ansx"], env_int("AURA_S2R_CP_ANSX"))
        print("coprocessor trace: ANS storage-side pages, mask", os.environ["AURA_S2R_CP_ANSX"])
    if os.environ.get("AURA_S2R_GO_COUNT"):
        p.write32(sym["sptm_virt_s2r_go_count"], env_int("AURA_S2R_GO_COUNT"))
        print("stop at the commit after", os.environ["AURA_S2R_GO_COUNT"], "GO wakes")
    if os.environ.get("AURA_S2R_WAKE_S"):
        p.write32(sym["sptm_virt_s2r_wake_s"], env_int("AURA_S2R_WAKE_S"))
        print("PMU RTC wake after the GO commit:", os.environ["AURA_S2R_WAKE_S"], "s")
    if os.environ.get("AURA_S2R_QUIESCE"):
        p.write32(sym["sptm_virt_s2r_quiesce"], env_int("AURA_S2R_QUIESCE"))
        print("AMCC quiesce writes at the commit:", os.environ["AURA_S2R_QUIESCE"])
    if os.environ.get("AURA_S2R_CRUMBS"):
        p.write32(sym["sptm_virt_s2r_crumbs"], env_int("AURA_S2R_CRUMBS"))
        print("S2R crumbs in PMU scratch, level", os.environ["AURA_S2R_CRUMBS"])
    if os.environ.get("AURA_S2R_PREWRITE"):
        a, v = (int(x, 0) for x in os.environ["AURA_S2R_PREWRITE"].split(","))
        p.write64(sym["sptm_virt_s2r_prewrite"], a)
        p.write64(sym["sptm_virt_s2r_prewrite"] + 8, v)
        print("wake-time pre-write: [0x%x] <- 0x%x" % (a, v))
    if os.environ.get("AURA_S2R_PROBE_AT"):
        p.write64(sym["sptm_virt_s2r_probe_at"], int(os.environ["AURA_S2R_PROBE_AT"], 0))
        print("wake-time probes at the guest's write to %s" % os.environ["AURA_S2R_PROBE_AT"])
    if os.environ.get("AURA_S2R_PROBE"):
        for i, a in enumerate(os.environ["AURA_S2R_PROBE"].split(",")[:4]):
            p.write64(sym["sptm_virt_s2r_probe"] + 8 * i, int(a, 0))
        print("wake-time probes: %s" % os.environ["AURA_S2R_PROBE"])
    if os.environ.get("AURA_S2R_WIN"):
        b, a = (int(x, 0) for x in os.environ["AURA_S2R_WIN"].split(","))
        p.write32(sym["sptm_virt_s2r_win"], b)
        p.write32(sym["sptm_virt_s2r_win"] + 4, a)
        print("after-wake dump window: %d records before the first commit, %d after" % (b, a))
    if os.environ.get("AURA_S2R_ARM_S"):
        p.write32(sym["sptm_virt_s2r_arm_s"], env_int("AURA_S2R_ARM_S"))
        print("S2R sleep rules armed", os.environ["AURA_S2R_ARM_S"], "s after setup")
    if os.environ.get("AURA_S2R_EXTRA"):
        p.write32(sym["sptm_virt_s2r_extra"], env_int("AURA_S2R_EXTRA"))
        print("extra stop-mode trace blocks, mask", os.environ["AURA_S2R_EXTRA"])
    if os.environ.get("AURA_S2R_USB_RETAKE"):
        p.write32(sym["sptm_virt_s2r_usb_retake"], env_int("AURA_S2R_USB_RETAKE"))
        p.write32(sym["sptm_virt_s2r_usb_delay_ms"], env_int("AURA_S2R_USB_RETAKE_MS"))
    if env_on("AURA_KEEP_ERRH") or env_on("AURA_AMCC_CLEAR"):
        report_amcc(p, adt, env_on("AURA_AMCC_CLEAR"))


def tail_console(iface, secs, idle):
    """Keep reading the console after the entry call: a serial read error reopens the device, and
    the tail stops after secs, after idle seconds of silence, or at once when the HV parks (its
    START, which says whether P_EXIT resumes the guest). AURA_PARK_AFTER_SECS, and SIGUSR1 at any
    time, ask the guest to park."""
    import signal

    import serial
    from aura import hvpark

    end = time.time() + secs
    dev = iface.dev
    dev.timeout = 0.5
    ask = {"at": time.time() + env_int("AURA_PARK_AFTER_SECS")
           if os.environ.get("AURA_PARK_AFTER_SECS") else None}
    signal.signal(signal.SIGUSR1, lambda *a: ask.update(at=time.time()))

    def poll():
        if ask["at"] is not None and time.time() >= ask["at"]:
            ask["at"] = None
            hvpark.request(dev)
            print("--- tail: park requested ---", flush=True)

    tail = hvpark.ConsoleTail(dev)
    print(f"--- tailing console for up to {secs}s (stop after {idle:g}s silent) ---", flush=True)
    while True:
        res = tail.run(max(0.0, end - time.time()), idle, poll)
        if res[0] != "gone":
            break
        print(f"--- tail: read error ({res[1]}); reopening ---", flush=True)
        try:
            dev.close()
        except Exception:
            pass
        for _ in range(100):
            try:
                dev.open()
                print("--- tail: reopened ---", flush=True)
                break
            except (serial.serialutil.SerialException, OSError):
                time.sleep(0.2)
        else:
            print("--- tail: device gone ---", flush=True)
            break
    tail.flush()
    if res[0] == "park":
        st = res[1]
        then = ("the guest waits for the host: tools/sptm_park.py --continue resumes it"
                if st.resumes else "a one-way park: the guest stays stopped")
        print(f"--- tail: the HV parked in its proxy ({st}); {then} ---", flush=True)
    elif res[0] == "idle":
        print(f"--- tail: console silent for {idle:g}s ---", flush=True)
    print("--- tail done ---", flush=True)


def main():
    args = parse_args()
    unknown = sorted(k for k in os.environ if k.startswith("AURA_") and k not in KNOWN_ENV)
    if unknown:
        print("not run_guest_sptm.py switches, ignored:", " ".join(unknown))
    from aura import hvgfxrec

    refused = hvgfxrec.check_env(os.environ)
    if refused:
        raise SystemExit(f"refused before anything was sent: {refused}")

    from aura import sptm_patch, sptm_virt_plan as plan_mod
    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    # Before anything else talks to it (ProxyUtils sets the proxy's heap up): is this the image?
    sym = Symbols(args.nm, args.elf, p.get_base())
    check_running_image(iface, sym, args.elf)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024, aura_heap=aura_heap_size)

    # SPTM's managed window, as aura computed it: above aura and everything iBoot left low,
    # below aura's carve-out.
    scratch = u.malloc(0x40)
    if p.call(sym["sptm_virt_place"], scratch) != 0:
        raise SystemExit("sptm_virt_place failed: not an SPTM_VIRT build, no FEAT_NV2 or GXF, or "
                         "no valid layout (see aura's console)")
    managed_base, managed_end = (p.read64(scratch + 8 * i) for i in range(2))
    print(f"managed window from aura [0x{managed_base:x}, 0x{managed_end:x})")

    # The native layout: physBase is the window's base, the images follow a 32 MiB phys slide
    # (which XNU frees to its VM, so nothing live may be in it), and iBoot's data regions are
    # copied to the blob tail, so nothing SPTM walks is below physBase.
    ba = u.ba
    phys_base = managed_base
    print(f"NATIVE layout: physBase = blob base = aura's floor 0x{phys_base:x} "
          f"(iBoot physBase 0x{ba.phys_base:x})")
    if managed_end - phys_base < (2 << 30):
        raise SystemExit("not enough managed RAM left above the blob base")
    print(f"SPTM managed window [0x{phys_base:x}, 0x{managed_end:x}), blob base 0x{phys_base:x}")
    dram_base = phys_base & ~((4 << 30) - 1)
    print(f"live boot_args: physBase 0x{ba.phys_base:x} memSize 0x{ba.mem_size:x} "
          f"memSizeActual 0x{ba.mem_size_actual:x}; dram base 0x{dram_base:x}")

    adt = u.adt
    copies = iboot_copies(adt)
    images = {"sptm": args.sptm.read_bytes(), "txm": args.txm.read_bytes(),
              "xnu": args.xnu.read_bytes()}
    check_oslog(adt, sym.run_base, sym["_end"])

    entry_off = plan_mod.macho_info(images["sptm"]).entry - plan_mod.macho_info(images["sptm"]).virtlo
    try:
        sp = sptm_patch.plan(images["sptm"], entry_off)
    except sptm_patch.SptmPatchError as e:
        raise SystemExit(f"this SPTM can't be patched: {e}")
    print(f"SPTM: {len(sp['patches'])} patch sites found"
          + (", MTE tag-storage sequence found" if sp["mte"] else ""))
    xnu_release, quirks = xnu_quirks(images["xnu"])
    if xnu_release:
        print(f"XNU {xnu_release}: hiding " + " ".join(quirks["hide"]))

    # Any stage that used NVMe left ANS in a session XNU can't take over: end it, stop and reset
    # ANS, and XNU starts it cold. AURA_ANS_COLD=idle (IDLE instead of SLEEP) is a diagnostic,
    # =off keeps the old handoff; AURA_ANS_PROBE=0 skips the HELLO check before XNU.
    # Measured on T8140 only: elsewhere it's opt-in, and XNU wakes iBoot's quiesced session.
    # A post-M4 ANS that an earlier stage powered off (Stage 1 1.0.0b13 on T8132, after it read
    # the ESP) has no session to wake and has lost its SART: XNU can only start it cold, and the
    # handoff powers it on again with the SART entries that stage passed on. Chainloaded by that
    # stage, this image has them (nvme.sart=); chainloaded by the host from that stage's proxy, the
    # host passes them (AURA_ANS_SART, from tools/ans_sart.py before the chainload).
    if os.environ.get("AURA_ANS_SART"):
        val = os.environ["AURA_ANS_SART"].removeprefix("nvme.sart=").encode()
        buf = u.malloc(len(val) + 1)
        iface.writemem(buf, val + b"\0")
        if not p.call(sym["nvme_set_sart_var"], buf):
            raise SystemExit("AURA_ANS_SART: not an nvme.sart= value")
    ans_state = p.call(sym["sptm_virt_ans_state"])
    ans_off = not ans_state & 1
    print(f"ANS {'powered off' if ans_off else 'on'}, "
          f"{'SART entries passed on' if ans_state & 2 else 'no SART entries passed on'}")
    t8140 = u.adt["/chosen"].chip_id == 0x8140
    ans_cold = os.environ.get("AURA_ANS_COLD", "sleep" if t8140 or ans_off else "off")
    if ans_cold not in ("sleep", "idle", "off"):
        raise SystemExit(f"AURA_ANS_COLD={ans_cold}: sleep, idle or off")
    if ans_off and ans_cold == "off":
        raise SystemExit("ANS is powered off: XNU can only start it cold (AURA_ANS_COLD=sleep)")
    if ans_off and not ans_state & 2:
        raise SystemExit("ANS is powered off and its SART entries weren't passed on: run "
                         "tools/ans_sart.py --start on the stage that powered it off, before the "
                         "chainload, and pass what it prints as AURA_ANS_SART")
    if ans_cold != "off":
        # An unanswered RTKit or controller step takes seconds, more than a read's timeout.
        to, iface.dev.timeout = iface.dev.timeout, 30
        try:
            rc = p.call(sym["sptm_virt_ans_cold"], (ans_cold == "idle") |
                        (2 if os.environ.get("AURA_ANS_PROBE", "1") == "0" else 0))
        finally:
            iface.dev.timeout = to
        if rc != 0:
            raise SystemExit("ANS didn't stop for XNU's cold start (see aura's console)")
        print(f"ANS stopped ({ans_cold.upper()}) and reset: XNU starts it cold")

    live_adt = u.get_adt()
    chip = u.adt["/chosen"].chip_id
    proxy_phy = p.call(sym["sptm_virt_proxy_phy"])
    phys, port_nodes = debug_port(live_adt, proxy_phy,
                                  env_int("AURA_DEBUG_PORT") if os.environ.get("AURA_DEBUG_PORT")
                                  else None)
    if not phys:
        raise SystemExit("no debug-link USB port found in the ADT")
    print("debug-link port" + ("s" if len(phys) > 1 else "") + " hidden:", " ".join(port_nodes))
    keep_exclaves = env_on("AURA_KEEP_EXCLAVES")
    # The NVRAM device stays visible (the guest's IOKit stalls without it and its Image4 nonce
    # slots live there); its writes are patched out instead, unless AURA_SHARE_NVRAM lets them
    # through at the price of a failed boot leaving recovery-boot-mode behind.
    quirk_hide = tuple(quirks["hide"])
    if env_on("AURA_KEEP_WIFI"):
        quirk_hide = tuple(n for n in quirk_hide if n in port_nodes)
    hide = port_nodes + (() if env_on("AURA_KEEP_ANE") else STRIP_NODES) + quirk_hide
    if not env_on("AURA_KEEP_ERRH"):
        hide += (ERROR_HANDLER,)
    if env_on("AURA_HIDE_DOCKCHANNEL"):
        hide += ("/arm-io/dockchannel-uart",)
    if os.environ.get("AURA_HIDE_NODES"):
        hide += tuple(n for n in os.environ["AURA_HIDE_NODES"].split(",") if n)
    if not keep_exclaves:
        hide += EXCLAVE_NODES
    cl4 = {}
    if keep_exclaves and os.environ.get("AURA_EXCLAVE_BUNDLE"):
        cl4, images["cl4"] = exclave_files(plan_mod)
    elif keep_exclaves and os.environ.get("AURA_EXCLAVE_FW"):
        cl4, images["cl4"] = exclave_firmware(pathlib.Path(os.environ["AURA_EXCLAVE_FW"]), plan_mod,
                                               chip)
    serial = os.environ.get("AURA_XNU_SERIAL")

    # auractl's DCC transport is gated behind a per-boot cookie: a random value that goes both into
    # the guest's boot-args (only root can read kern.bootargs) and into aura (configure() below).
    ctl_cookie = 0
    if env_on("AURA_AURACTL_DCC"):
        ctl_cookie = secrets.randbits(64) or 1
        args.bootargs = f"{args.bootargs} aura-actl-ck={ctl_cookie:016x}"

    plan = plan_mod.plan_boot(
        images["sptm"], images["txm"], images["xnu"], live_adt,
        dram_base=dram_base, phys_base=phys_base, mem_size=managed_end - phys_base,
        mem_size_actual=ba.mem_size_actual, cmdline=args.bootargs, blob_base=phys_base,
        trustcache=args.trustcache.read_bytes() if args.trustcache else b"",
        iboot_copies=copies, hide_nodes=hide, diag_record=env_on("AURA_SPTM_DIAG"),
        drop_props=(() if keep_exclaves else ROUTES_PROPS) + ans_drop_props() +
        (ANS_QUIESCED if ans_cold != "off" else ()),
        xnu_serial=f"/arm-io/{serial}" if serial else None,
        os_long_version=args.os_long_version,
        video=tuple(int(getattr(ba.video, f)) for f in
                    ("base", "display", "stride", "width", "height", "depth"))
        if env_on("AURA_KEEP_BOOT_VIDEO") else None, **cl4)

    entry, handoff = plan.sptm_entry_pa, plan.boot_args_pa
    print(f"bootargs: {args.bootargs}")
    print(f"SPTM load 0x{plan.sptm_pa:x}  entry 0x{entry:x}  boot_args 0x{handoff:x}  "
          f"ADT 0x{plan.adt_pa:x} ({len(plan.adt)} B)")
    print(f"virtBase 0x{plan.virt_base:x}  blob [0x{plan.blob_start:x}, 0x{plan.blob_end:x})")
    for name, (pa, size) in plan.regions.items():
        print(f"  {name:14s} 0x{pa:x} +0x{size:x}")
    for n in plan.notes:
        print("note:", n)
    if args.dry_run:
        return

    write_plan(p, u, iface, sym, plan, images)
    patch_sptm(p, sym, entry, sp)
    phys = phys[:2]
    for i, ph in enumerate(phys + [0] * (2 - len(phys))):
        p.write32(sym["sptm_virt_link_phys"] + 4 * i, ph)
    print("debug-link USB PHYs (phandles):", " ".join(f"0x{ph:x}" for ph in phys) or "none")
    if env_on("AURA_XNU_RO_RELAX"):
        if not quirks["ro_relax"]:
            raise SystemExit("AURA_XNU_RO_RELAX: no RO-zone patch matches this kernelcache")
        patch_xnu(p, sym, plan, quirks["ro_relax"], "ro_relax")
    if quirks.get("rorgn"):
        set_xnu_rorgn(p, sym, plan, quirks)
    if quirks.get("nvram_mutable"):
        patch_xnu(p, sym, plan, quirks["nvram_mutable"], "nvram_mutable")
    if quirks.get("pac_keys") and not env_on("AURA_XNU_TASK_KEYS"):
        patch_xnu(p, sym, plan, quirks["pac_keys"], "pac_keys")
    if quirks.get("nvram_ro") and not env_on("AURA_SHARE_NVRAM"):
        patch_xnu(p, sym, plan, quirks["nvram_ro"], "nvram_ro")
    if env_on("AURA_XNU_TC_FORCE") and quirks.get("tc_force"):
        patch_xnu(p, sym, plan, quirks["tc_force"], "tc_force")
    if env_on("AURA_XNU_ENTRY"):
        patch_xnu_entry(p, sym, plan, images["xnu"])
    write_patch_log()
    configure(p, sym, plan, live_adt, ctl_cookie)
    if p.call(sym["sptm_virt_load"], entry, handoff) != 0:
        raise SystemExit("sptm_virt_load rejected the entry/handoff (see aura's console)")
    if args.no_enter:
        print("loaded and recorded; not entering (--no-enter)")
        return

    if env_on("AURA_KEEP_BOOT_VIDEO"):
        from aura.proxy import IODEV
        p.iodev_set_usage(IODEV.FB, 0)  # the framebuffer is XNU's now
    if os.environ.get("AURA_TRAP_DELAY_NS"):
        p.write32(sym["sptm_virt_trap_delay_ns"], env_int("AURA_TRAP_DELAY_NS"))
    if env_on("AURA_S2R_LIVE"):
        p.write32(sym["sptm_virt_s2r_live"], 1)
    if env_on("AURA_SYSREG_CENSUS"):
        p.write32(sym["sptm_virt_census"], 1)
    if os.environ.get("AURA_CPTR_LIVE"):
        p.write32(sym["sptm_virt_cptr_live"], env_int("AURA_CPTR_LIVE"))
    if os.environ.get("AURA_TRAP_DELAY_MASK"):
        p.write32(sym["sptm_virt_trap_delay_mask"], env_int("AURA_TRAP_DELAY_MASK"))
    if os.environ.get("AURA_WFI_WATCH"):
        p.write32(sym["sptm_virt_wfi_watch"], env_int("AURA_WFI_WATCH"))
    if os.environ.get("AURA_AMCC_NOLOCK"):
        p.write32(sym["sptm_virt_amcc_nolock"], env_int("AURA_AMCC_NOLOCK"))
        print(f"AMCC range locks shadowed: mask {env_int('AURA_AMCC_NOLOCK'):#x}")
    if os.environ.get("AURA_RG_XWDT"):
        p.write32(sym["sptm_virt_rg_xwdt"], env_int("AURA_RG_XWDT"))
    print("entering SPTM at virtual EL2 (the result is on aura's console)")
    # sptm_virt_enter does not return. Waiting for a reply or a boot banner would eat the guest's
    # first console output and reopen the link.
    p.request(p.P_CALL, sym["sptm_virt_enter"], no_reply=True)
    iface.dev.flush()
    secs = env_int("AURA_TAIL_SECS")
    if secs > 0:
        tail_console(iface, secs, float(os.environ.get("AURA_TAIL_IDLE") or 10))


if __name__ == "__main__":
    sys.exit(main())
