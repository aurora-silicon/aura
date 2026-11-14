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
  AURA_S2R_CP_ONLY=<a,b>   T6050 COPROC: hook only these /arm-io nodes (aop,smc; a name also
                           covers its dart-<name>); the default hooks every coprocessor
  AURA_S2R_CP_NODES=<a,b>  T6050 COPROC: also hook every reg window (up to 1 MiB) of these /arm-io
                           nodes, such as atc-phy0,acio0; "name:N" hooks only reg N, any size
  AURA_S2R_CP_RANGE=<b:s>  T6050 COPROC: also hook up to 4 comma-separated base:size ranges, such as
                           the AOP's SRAM (aligned out to 16 KiB pages)
  AURA_S2R_DROP_USB        tear down aura's USB gadget at the sleep commit
  AURA_S2R_WAKE_S=<s>      GO, timer-only sleep: arm the PMU RTC alarm s seconds after the commit
  AURA_S2R_GO_COUNT=<n>    GO, timer-only sleep: after n wakes, stop in the proxy at the next commit
  AURA_S2R_QUIESCE=<mask>  AMCC error-enable writes at the commit: 8 DSID_AGE_OUT,
                           16 UNEXP_RT_HIT_DIR
  AURA_S2R_USB_RETAKE=<n>  take the CDC link again after a wake; n > 1 also drops it n s in
  AURA_S2R_USB_RETAKE_MS   delay from XNU's return to the re-take (ms)
  AURA_XNU_BT              sample XNU backtraces every few seconds per core
  AURA_GPU_TRACE           hook the GPU (sgx reg 0) and PMP registers, record every access
  AURA_GPU_STOP_SECS=<n>   park in the proxy n s after the first GPU access (read the ring)
  AURA_GPU_STOP_MSG=<v>    park right after the guest writes v to the GPU (a mailbox message)
  AURA_GPU_PRINT_MAX=<n>   print at most n GPU accesses on aura's console (default 20000)
  AURA_GPU_STOP_BOOT_SECS=<n>  park in the proxy at the first SPTM trap or hooked S2R/GPU access n s
                           after entry, GPU trace or not; read the S2R ring with dump_s2r_ring.py
  AURA_SAMPLE              sample the guest's PC until each core's XNU entry (pre-XNU hangs)
  AURA_SAMPLE_HZ=<n>       samples per second (default 4)
  AURA_SAMPLE_MAX=<n>      stop printing after n samples (default 64)
  AURA_XNU_RO_RELAX        patch XNU's zalloc_ro_mut RO-zone panic out (tracing only; XNU_QUIRKS)
  AURA_XNU_TC_FORCE        accept a trust cache TXM refused, so the cryptex graft proceeds
  AURA_SHARE_NVRAM         let the guest reach the real NVRAM (a failed boot pollutes it)
  AURA_XNU_TASK_KEYS       keep XNU's per-task PAC keys (its kernel frames then fail, XNU_QUIRKS)
  AURA_DEBUG_PORT=<n>      with the proxy on KIS, hide only USB-C port n (port-number), not all
  AURA_SPTM_DIAG           give SPTM a diagnostic record (boot_args revision 3, SPTMDebug)
  AURA_OS_LONG_VERSION     the guest's Ap,OSLongVersion (--os-long-version)
  AURA_KEEP_BOOT_VIDEO     hand iBoot's boot framebuffer to XNU, aura's console off it
  AURA_TAIL_SECS, AURA_TAIL_IDLE   keep reading the console after entry, until idle
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
  XNU_BOOTARGS             XNU's command line

Example:
  export PYTHONPATH=proxyclient
  python3 proxyclient/tools/run_guest_sptm.py --sptm sptm.bin --txm txm.bin \\
      --xnu kernelcache.decompressed --trustcache iboot.tc --dry-run
"""

import argparse
import os
import pathlib
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
}
KNOWN_ENV = {
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
    "AURA_S2R_CP_ONLY",
    "AURA_S2R_CP_NODES",
    "AURA_S2R_CP_RANGE",
    "AURA_S2R_DCP_PKT",
    "AURA_S2R_USB_RETAKE",
    "AURA_S2R_USB_RETAKE_MS",
    "AURA_TAIL_SECS",
    "AURA_TAIL_IDLE",
    "AURA_GPU_STOP_SECS",
    "AURA_GPU_STOP_BOOT_SECS",
    "AURA_GPU_STOP_MSG",
    "AURA_GPU_PRINT_MAX",
    "AURA_SAMPLE_HZ",
    "AURA_SAMPLE_MAX",
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


def patch_xnu(p, sym, plan, patches):
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


def set_xnu_rorgn(p, sym, plan, quirks):
    """Hand XNU the read-only region iBoot would have locked: the BootKC's text and const."""
    lo = plan.regions["BootKC-rx"][0]
    hi = plan.regions["BootKC-ro"][0] + plan.regions["BootKC-ro"][1] - 1
    p.write64(sym["sptm_virt_xnu_rorgn"], lo)
    p.write64(sym["sptm_virt_xnu_rorgn"] + 8, hi & ~0xfff)
    print(f"XNU read-only region 0x{lo:x}-0x{hi:x}")
    patch_xnu(p, sym, plan, quirks["rorgn"])
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


def configure(p, sym, plan, adt):
    p.write64(sym["sptm_virt_diag_pa"], plan.diag_pa)
    if plan.diag_pa:
        print(f"SPTM diagnostic record at 0x{plan.diag_pa:x}")
    flags = sum(bit for name, bit in CONFIG_FLAGS.items() if env_on(name))
    p.call(sym["sptm_virt_config"], flags)
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
    if os.environ.get("AURA_GPU_PRINT_MAX"):
        p.write32(sym["sptm_virt_gpu_print_max"], env_int("AURA_GPU_PRINT_MAX"))
    if env_on("AURA_S2R_DCP_PKT"):
        p.write32(sym["sptm_virt_s2r_dcp_pkt"], 1)
        print("coprocessor trace: DCP IOMFB packets copied into the ring")
    if env_on("AURA_S2R_CP_NO_ANS"):
        p.write32(sym["sptm_virt_s2r_cp_skip_ans"], 1)
        print("coprocessor trace: ANS's pages left unhooked")
    if os.environ.get("AURA_S2R_CP_ANSX"):
        p.write32(sym["sptm_virt_s2r_cp_ansx"], env_int("AURA_S2R_CP_ANSX"))
        print("coprocessor trace: ANS storage-side pages, mask", os.environ["AURA_S2R_CP_ANSX"])
    for var, symname, size in (("AURA_S2R_CP_ONLY", "sptm_virt_s2r_cp_only", 256),
                               ("AURA_S2R_CP_NODES", "sptm_virt_s2r_cp_nodes", 512)):
        if os.environ.get(var):
            val = os.environ[var].encode()
            if len(val) >= size:
                raise SystemExit(f"{var}: at most {size - 1} bytes")
            p.iface.writemem(sym[symname], val + b"\0")
            print("coprocessor trace:", var, os.environ[var])
    if os.environ.get("AURA_S2R_CP_RANGE"):
        ranges = [tuple(int(v, 0) for v in r.split(":")) for r in os.environ["AURA_S2R_CP_RANGE"].split(",")]
        if len(ranges) > 4:
            raise SystemExit("AURA_S2R_CP_RANGE: at most 4 base:size pairs")
        for i, (base, size) in enumerate(ranges):
            p.write64(sym["sptm_virt_s2r_cp_range"] + 16 * i, base)
            p.write64(sym["sptm_virt_s2r_cp_range"] + 16 * i + 8, size)
        print("coprocessor trace: extra ranges", os.environ["AURA_S2R_CP_RANGE"])
    if os.environ.get("AURA_S2R_GO_COUNT"):
        p.write32(sym["sptm_virt_s2r_go_count"], env_int("AURA_S2R_GO_COUNT"))
        print("stop at the commit after", os.environ["AURA_S2R_GO_COUNT"], "GO wakes")
    if os.environ.get("AURA_S2R_WAKE_S"):
        p.write32(sym["sptm_virt_s2r_wake_s"], env_int("AURA_S2R_WAKE_S"))
        print("PMU RTC wake after the GO commit:", os.environ["AURA_S2R_WAKE_S"], "s")
    if os.environ.get("AURA_S2R_QUIESCE"):
        p.write32(sym["sptm_virt_s2r_quiesce"], env_int("AURA_S2R_QUIESCE"))
        print("AMCC quiesce writes at the commit:", os.environ["AURA_S2R_QUIESCE"])
    if os.environ.get("AURA_S2R_USB_RETAKE"):
        p.write32(sym["sptm_virt_s2r_usb_retake"], env_int("AURA_S2R_USB_RETAKE"))
        p.write32(sym["sptm_virt_s2r_usb_delay_ms"], env_int("AURA_S2R_USB_RETAKE_MS"))
    if env_on("AURA_KEEP_ERRH") or env_on("AURA_AMCC_CLEAR"):
        report_amcc(p, adt, env_on("AURA_AMCC_CLEAR"))


def tail_console(iface, secs, idle):
    """Keep reading the console after the entry call: a serial read error reopens the device,
    and the tail stops after secs, or after idle seconds of silence."""
    import serial

    end = time.time() + secs
    last = time.time()
    dev = iface.dev
    buf = b""
    print(f"--- tailing console for up to {secs}s (stop after {idle:g}s silent) ---", flush=True)
    while time.time() < end:
        if idle and time.time() - last > idle:
            print(f"--- tail: console silent for {idle:g}s ---", flush=True)
            break
        try:
            chunk = dev.read(4096)
        except (serial.serialutil.SerialException, OSError) as e:
            print(f"--- tail: read error ({e}); reopening ---", flush=True)
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
            continue
        if not chunk:
            continue
        last = time.time()
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            print("TTY> " + line.decode("latin1", "replace").rstrip("\r"), flush=True)
    print("--- tail done ---", flush=True)


def main():
    args = parse_args()
    unknown = sorted(k for k in os.environ if k.startswith("AURA_") and k not in KNOWN_ENV)
    if unknown:
        print("not run_guest_sptm.py switches, ignored:", " ".join(unknown))

    from aura import sptm_patch, sptm_virt_plan as plan_mod
    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p, heap_size=128 * 1024 * 1024)
    sym = Symbols(args.nm, args.elf, p.get_base())

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
    t8140 = u.adt["/chosen"].chip_id == 0x8140
    ans_cold = os.environ.get("AURA_ANS_COLD", "sleep" if t8140 else "off")
    if ans_cold not in ("sleep", "idle", "off"):
        raise SystemExit(f"AURA_ANS_COLD={ans_cold}: sleep, idle or off")
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
        patch_xnu(p, sym, plan, quirks["ro_relax"])
    if quirks.get("rorgn"):
        set_xnu_rorgn(p, sym, plan, quirks)
    if quirks.get("nvram_mutable"):
        patch_xnu(p, sym, plan, quirks["nvram_mutable"])
    if quirks.get("pac_keys") and not env_on("AURA_XNU_TASK_KEYS"):
        patch_xnu(p, sym, plan, quirks["pac_keys"])
    if quirks.get("nvram_ro") and not env_on("AURA_SHARE_NVRAM"):
        patch_xnu(p, sym, plan, quirks["nvram_ro"])
    if env_on("AURA_XNU_TC_FORCE") and quirks.get("tc_force"):
        patch_xnu(p, sym, plan, quirks["tc_force"])
    configure(p, sym, plan, live_adt)
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
