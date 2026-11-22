# SPDX-License-Identifier: MIT
"""Explicit, hash-bound Aura -> Mu Windows launch admission.

Importing this module never opens a device or loads the allocating proxy helpers.
The ordinary Aura guest path is not selected by this launcher.
"""
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import struct
from types import SimpleNamespace
import zlib

CAPS_MAGIC = 0x4157494E00000000
CAPS_ABI = 1
REQUIRED_CAPS = 0x000F
FRAMEBUFFER_SHADOW = 0x0010
N1_HANDOFF = 0x0020
N1_ACQUIRE = 0x1000  # 0x0800 is reserved for CPUFREQ_OWNERSHIP (public PR93).
CONFIGURATION = 0x0040
MEMORY_PREFLIGHT = 0x0080
SERIAL_CAPTURE = 0x0100
SERIAL_CDC_ISOLATED = 0x0200
MTP_HANDOFF = 0x0400
CPUFREQ_OWNERSHIP = 0x0800
XFER = 0x2000  # HvXfer bulk channel: P_HV_MAP_XFER + HV_XFER events (src/hv/windows/hv_xfer.c)
ECPU_BOOT_STATE = 0x4000  # P_HV_WINDOWS_ECPU_BOOT_STATE (docs/t8142-ecpu-stepup.md)
# The T8152 carrier rules are P_HV_WINDOWS_CAPS2 bits 8-12 (they were CAPS bits 11-15, which
# j813-windows uses for CPU frequency ownership, N1 acquire, HvXfer and the E boot state).
# T8152 stage-2 allowlist policy, the guest RAM window op and the T8152 Fast IPI
# opt-in. A resident without it must never run a J804 guest.
CAPS2_T8152_POLICY = 0x0100
T8152_POLICY = CAPS2_T8152_POLICY
# The resident refuses a T8152 launch unless all 12 CPUs are live in its dispatch loop.
# A resident without it must never run one.
CAPS2_T8152_LIVE_CPUS = 0x0200
T8152_LIVE_CPUS = CAPS2_T8152_LIVE_CPUS
# The resident traps and completes guest WFE on T8152 (HCR_EL2.TWE) and keeps its code below
# the CTXR exec window and CTXR_D's last 32 MiB block. A resident without it must never run one.
CAPS2_T8152_WFE_PLACEMENT = 0x0400
T8152_WFE_PLACEMENT = CAPS2_T8152_WFE_PLACEMENT
# The resident reaches the guest's ACTLR_EL1 on T8152 only through the architectural ACTLR_EL12;
# a resident without it writes the implementation-defined alias, which UNDEFs at EL2 there.
CAPS2_T8152_ACTLR_ARCH = 0x0800
T8152_ACTLR_ARCH = CAPS2_T8152_ACTLR_ARCH
# The resident runs a T8152 guest OS with every ICC_* access trapped to EL2 (ICH_HCR_EL2
# En|TC|TALL0|TALL1) and a software GICv3 CPU interface (Phase 3 measurement (f):
# GIC_CPUIF_UNDEF in the hardware-interface configuration). Without it, never run one.
CAPS2_T8152_GIC_TRAPALL = 0x1000
T8152_GIC_TRAPALL = CAPS2_T8152_GIC_TRAPALL
T8152_RULES = (T8152_POLICY | T8152_LIVE_CPUS | T8152_WFE_PLACEMENT | T8152_ACTLR_ARCH |
               T8152_GIC_TRAPALL)
T8152_CPUS = 12
CONFIG_T8142_EXPERIMENTAL = 0x0001
# The same resident bit on T8152: the absent-PMUv3 UNDEF recovery only (M3 run 1: bootmgr's
# "mrs x9, PMCCNTR_EL0" (0xd53b9d09) UNDEFed at EL1 there, ESR_EL1 0x02000000, as on T8142).
CONFIG_GUEST_COMPAT = CONFIG_T8142_EXPERIMENTAL
T8152_PMU_UNDEF = "t8152-pmu-undef"
GUEST_COMPATIBILITY = ("none", "t8142-experimental", T8152_PMU_UNDEF)
CONFIG_FRAMEBUFFER_SHADOW = 0x0002
CONFIG_SERIAL_CAPTURE = 0x0004
CONFIG_SERIAL_CDC = 0x0008
CONFIG_MTP = 0x0010
CONFIG_CPUFREQ = 0x0020
CONFIG_T8152_FAST_IPI = 0x10000
CONFIG_T8152_SOFT_GIC = 0x20000
CONFIG_T8152_WFI = 0x40000  # real WFI at EL2, only after Phase 3 measurement (a)
MTP_DISABLED_PROPERTY = "aura,windows-mtp-disabled"
MTP_RESERVED_BASE = 0x10020000000
MTP_RESERVED_SIZE = 0x200000
MAX_IMAGE = 1024 * 1024 * 1024
N1_PROPERTY = "aura,windows-n1-v1"
N1_BYTES = 128
CPUFREQ_PROPERTY = "aura,windows-cpufreq-v1"
# Mu codex/j813-mu-quiet-unified Docs/j813-unified-display-marker.md: 4 bytes, LE 1 selects AURA8171.
UNIFIED_DISPLAY_PROPERTY = "aura,windows-unified-display-v1"
AURA_HEAP_RESERVE = 128 * 1024 * 1024
PROXY_HEAP_SIZE = 128 * 1024 * 1024
N1_RESOURCES = ((0x1cb0000000, 0x200000), (0x488000000, 0x20000),
                (0x488028000, 0x4000), (0x48f010000, 0x1000),
                (0x39a000300, 4), (0xb80000000, 0x600000))
# Retained J813 PMGR ADT frequencies, independently decoded from hardware.
# ADT records are {kHz, mV}; index 0 is command state 2 (state 1 is a 300 MHz floor).
# Preserve the P17/P18 ordering: this firmware ladder is not monotonic.
J813_CPU_OPP_KHZ = (
    (972000, 1152000, 1584000, 1992000, 2352000, 2700000, 2964000, 3048000),
    (1308000, 1608000, 1932000, 2220000, 2508000, 2820000, 3072000,
     3300000, 3516000, 3720000, 3876000, 4020000, 4176000, 4248000,
     4272000, 4332000, 4428000, 4416000, 4464000),
)
ECPU_HANDOFF_STATE = 2
ECPU_LAST_STATE = ECPU_HANDOFF_STATE + len(J813_CPU_OPP_KHZ[0]) - 1


class WindowsLaunchRefused(RuntimeError):
    """The launcher or the resident's configure step refused before hv_init(): nothing on the
    machine changed and the proxy keeps running.  Resident failures (ProxyRemoteError) and
    link failures (UartError) are different and are not this."""


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, label):
    require(isinstance(value, (str, int)) and not isinstance(value, bool), label + " must be an integer")
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError as error:
        raise ValueError(label + " must be an integer") from error


def validate_capabilities(value, framebuffer="native", guest_compatibility="none", n1="disabled",
                          reserved_memory=False, serial="disabled", mtp="disabled", cpufreq="disabled",
                          xfer=False, ecpu_boot_state=0, t8152=False, ipi="poll",
                          gic_cpuif=None, caps2=None):
    """CAPS (P_HV_WINDOWS_CAPS) against the launch; a T8152 launch also needs CAPS2
    (P_HV_WINDOWS_CAPS2) with the T8152 carrier rules (bits 8-12)."""
    require(isinstance(value, int) and value >> 32 == CAPS_MAGIC >> 32,
            "resident is not an explicit Windows Aura personality")
    require((value >> 16) & 0xffff == CAPS_ABI, "unsupported Windows Aura capability ABI")
    require(value & REQUIRED_CAPS == REQUIRED_CAPS, "resident lacks required Windows capabilities")
    if framebuffer == "bgra-shadow":
        require(value & FRAMEBUFFER_SHADOW, "resident lacks the framebuffer conversion backend")
    if (framebuffer != "native" or guest_compatibility != "none" or serial in ("capture", "cdc")
            or mtp != "disabled"):
        require(value & CONFIGURATION, "resident lacks explicit Windows configuration")
    if n1 != "disabled":
        require(value & N1_HANDOFF, "resident lacks the N1 handoff producer")
    if n1 == "j813-acquire-v1":
        require(value & N1_ACQUIRE, "resident lacks explicit whole-N1 acquisition")
    if reserved_memory:
        require(value & MEMORY_PREFLIGHT, "resident lacks read-only memory reservation preflight")
    if serial == "capture":
        require(value & SERIAL_CAPTURE, "resident lacks framed Windows serial capture")
    if serial == "cdc":
        require(value & SERIAL_CDC_ISOLATED, "resident lacks isolated Windows CDC serial mode")
    if mtp != "disabled":
        require(value & MTP_HANDOFF and value & MEMORY_PREFLIGHT,
                "resident lacks guarded Windows MTP handoff")
    if cpufreq != "disabled":
        require(value & CPUFREQ_OWNERSHIP and value & CONFIGURATION,
                "resident lacks exclusive Windows CPU frequency ownership")
    if xfer:
        require(value & XFER, "resident lacks the HvXfer bulk channel")
    if ecpu_boot_state:
        require(value & ECPU_BOOT_STATE and value & CONFIGURATION,
                "resident lacks the explicit E-cluster boot state")
    if gic_cpuif is None:
        gic_cpuif = "software" if t8152 else "carrier"
    if t8152:
        require(gic_cpuif == "software", "T8152: gic_cpuif carrier is refused (measurement (f): "
                "GIC_CPUIF_UNDEF); the software CPU interface is required")
    if t8152 or ipi != "poll" or gic_cpuif != "carrier":
        rules = caps2 if isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 else 0
        require(rules & T8152_POLICY, "resident lacks the T8152 stage-2 policy; "
                "never run a J804 guest on it")
        require(rules & T8152_LIVE_CPUS, "resident lacks the T8152 12-live-CPU refusal; "
                "never run a J804 guest on it")
        require(rules & T8152_WFE_PLACEMENT, "resident lacks the T8152 guest-WFE trap and code "
                "placement rule; never run a J804 guest on it")
        require(rules & T8152_ACTLR_ARCH, "resident lacks the T8152 architectural ACTLR_EL12 "
                "rule; never run a J804 guest on it")
        require(rules & T8152_GIC_TRAPALL, "resident lacks the T8152 trapped software GIC CPU "
                "interface; never run a J804 guest on it")
        require(value & CONFIGURATION, "resident lacks explicit Windows configuration")
    return value & 0xffff


@dataclass(frozen=True)
class LaunchPlan:
    profile_path: Path
    profile: dict
    profile_bytes: bytes
    profile_sha256: str
    image: bytes
    images: tuple
    entry_offset: int
    chip_id: int
    model: str
    framebuffer: str
    guest_compatibility: str
    usb: str
    n1: str
    reserved_memory: tuple
    boot_args: str
    serial: str = "disabled"
    mtp: str = "disabled"
    cpufreq: str = "disabled"
    xfer: object = None
    ecpu_boot_state: int = 0  # 0: keep the firmware handoff state (key absent)
    unified_display: str = "disabled"  # key absent: Mu keeps the legacy GPU and display pair
    guest_ram: tuple = None
    ipi: str = "poll"
    on_reset: str = "stay"
    gic_cpuif: str = "carrier"
    wfi: str = "poll"


XFER_FIELDS = {"doorbell", "window_bytes", "serve_dir", "inbox", "compress"}
XFER_DEFAULT_WINDOW = 1 << 20
# Mirrors of HV_XFER_REGS_SIZE/PAGE_SIZE and HV_XFER_MAX_WINDOW (src/hv/windows/hv_xfer.[ch]);
# kept here so profile validation never imports the aura.hv package.
XFER_PAGE = 0x4000
XFER_MAX_WINDOW = 64 << 20
XFER_IPA_LIMIT = 1 << 44  # hv_pt_init(): vaddr_bits = min(44, PARange)
# Guest-physical windows the Windows C personality hooks with no ADT node
# (vGIC distributor/redistributor/ITS, 36- and 42-bit layouts, hv_vgic.c).
# hv_map_xfer() also refuses any IPA with a stage-2 entry; this fails earlier.
WINDOWS_SYNTHETIC_MMIO = (
    ("vgic-36bit", 0xF00000000, 0xF30000000),
    ("vgic-42bit", 0x5000000000, 0x5300000000),
)


def doorbell_conflicts(base, size=XFER_PAGE, *, dram=None, devices=(), hooks=(),
                       synthetic=WINDOWS_SYNTHETIC_MMIO, ipa_limit=XFER_IPA_LIMIT):
    """Every reason `base` cannot host the HvXfer doorbell (empty = usable).

    ``dram`` is (start, stop); ``devices`` and ``hooks`` are (name, start, stop)
    triples: ADT register ranges, and host tracer zones other than the /arm-io
    HW passthrough the doorbell is meant to sit inside.
    """
    reasons = []
    stop = base + size
    if base <= 0 or base % XFER_PAGE:
        reasons.append(f"doorbell {base:#x} is not a nonzero 16 KiB-aligned address")
    if stop > ipa_limit:
        reasons.append(f"doorbell {base:#x} is beyond the {ipa_limit:#x} IPA limit")
    if dram is not None and base < dram[1] and dram[0] < stop:
        reasons.append(f"doorbell {base:#x} is inside DRAM {dram[0]:#x}..{dram[1]:#x}")
    for kind, ranges in (("ADT device", devices), ("hook", hooks), ("synthetic MMIO", synthetic)):
        for name, lo, hi in ranges:
            if base < hi and lo < stop:
                reasons.append(f"doorbell {base:#x} overlaps {kind} {name} {lo:#x}..{hi:#x}")
    return reasons


def _xfer_directory(value, label):
    require(isinstance(value, str) and os.path.isabs(value), f"xfer {label} must be an absolute path")
    path = Path(value)
    require(not path.is_symlink() and path.is_dir(), f"xfer {label} must be an existing, non-symlink directory")
    return os.path.realpath(path)


def plan_xfer(value):
    """Validate the optional "xfer" profile object; None means disabled.

    {"doorbell": "0x..." | "auto", "window_bytes": 1048576, "serve_dir": "/abs",
     "inbox": "/abs" (optional; PUT disabled without it), "compress": true}
    Hardware-dependent checks (DRAM, ADT devices, live hooks) run again at
    attach time; these are the ones that can be decided from the file alone.
    """
    if value is None:
        return None
    require(isinstance(value, dict) and set(value) <= XFER_FIELDS and {"doorbell", "serve_dir"} <= set(value),
            "xfer must bind doorbell and serve_dir (optional: window_bytes, inbox, compress)")
    doorbell = value["doorbell"]
    if doorbell != "auto":
        doorbell = number(doorbell, "xfer doorbell")
        problems = doorbell_conflicts(doorbell)
        require(not problems, "; ".join(problems))
    window = number(value.get("window_bytes", XFER_DEFAULT_WINDOW), "xfer window_bytes")
    require(0 < window <= XFER_MAX_WINDOW and window % XFER_PAGE == 0,
            "xfer window_bytes must be a nonzero 16 KiB multiple up to 64 MiB")
    serve_dir = _xfer_directory(value["serve_dir"], "serve_dir")
    inbox = value.get("inbox")
    if inbox is not None:
        inbox = _xfer_directory(inbox, "inbox")
        require(os.path.commonpath([serve_dir, inbox]) not in (serve_dir, inbox),
                "xfer inbox must be separate from serve_dir (neither inside the other)")
    compress = value.get("compress", True)
    require(isinstance(compress, bool), "xfer compress must be a boolean")
    return {"doorbell": doorbell, "window_bytes": window, "serve_dir": serve_dir, "inbox": inbox,
            "compress": compress}


def xfer_plan_summary(xfer):
    if xfer is None:
        return {"enabled": False}
    doorbell = xfer["doorbell"]
    return {"enabled": True, "doorbell": doorbell if doorbell == "auto" else hex(doorbell),
            "window_bytes": xfer["window_bytes"], "serve_dir": xfer["serve_dir"],
            "inbox": xfer["inbox"], "put_enabled": xfer["inbox"] is not None,
            "compress": xfer["compress"]}


def attach_windows_xfer(hv, xfer, log_dir):
    """Attach HvXfer before guest entry; returns the receipt for launch.json."""
    from .hv.xfer import XferHostDevice
    engine = XferHostDevice(xfer["serve_dir"], xfer["inbox"],
                            log_path=Path(log_dir) / "xfer-commands.jsonl")
    try:
        info = hv.attach_xfer(xfer["doorbell"], xfer["window_bytes"], engine,
                              compress=xfer["compress"])
    except BaseException:
        engine.close()
        raise
    receipt = xfer_plan_summary(xfer)
    receipt.update(doorbell=hex(info["doorbell"]), aura_window=hex(info["window"]),
                   aura_window_bytes=info["window_bytes"], log=str(engine.log_path),
                   guest_attach={"DoorbellBase": hex(info["doorbell"]),
                                 "note": "IOCTL_APPLE_HVXFER_ATTACH; guest-owned window "
                                         "(SET_WINDOW) is the driver default"})
    print(f"HvXfer: serving {xfer['serve_dir']} (GET), "
          f"inbox {xfer['inbox'] or 'disabled (PUT refused)'}; log {engine.log_path}", flush=True)
    return receipt


def plan_profile(filename):
    filename = Path(filename).resolve()
    with filename.open("rb") as stream:
        raw = stream.read(65537)
    require(len(raw) <= 65536, "profile is too large")
    profile = json.loads(raw)
    require(isinstance(profile, dict), "profile must be an object")
    require(profile.get("schema") == "aura.windows.boot.v1", "unsupported Windows boot profile")
    require(set(profile) <= {"schema", "name", "target", "mu", "append", "framebuffer", "boot_args",
                             "guest_compatibility", "usb", "n1", "reserved_memory", "serial", "mtp", "cpufreq",
                             "xfer", "ecpu_boot_state", "unified_display", "guest_ram", "ipi",
                             "on_reset", "gic_cpuif", "wfi"},
            "unknown Windows profile field")
    require(isinstance(profile.get("name"), str) and 0 < len(profile["name"]) <= 80, "profile name is required")
    target = profile.get("target")
    require(isinstance(target, dict) and set(target) == {"chip_id", "model"}, "target must bind chip_id and model")
    chip = number(target["chip_id"], "chip_id")
    require(0 < chip <= 0xffff, "chip_id is out of range")
    model = target["model"]
    require(isinstance(model, str) and re.fullmatch(r"(?:Mac\d+,\d+|(?:MacBookAir|MacBookPro|Macmini|iMac|MacPro)\d+,\d+)", model),
            "target model must be an exact Apple model identifier")
    mu = profile.get("mu")
    require(isinstance(mu, dict) and set(mu) == {"path", "sha256", "entry_offset"}, "mu must bind path, sha256 and entry_offset")
    append = profile.get("append", [])
    require(isinstance(append, list) and len(append) <= 8, "append must be a bounded image list")
    images, parts = [], []
    size = 0
    for index, item in enumerate([mu, *append]):
        require(isinstance(item, dict), "image record must be an object")
        if index == 0:
            require(set(item) == {"path", "sha256", "entry_offset"}, "invalid Mu image record fields")
            offset = 0
        else:
            require({"path", "sha256"} <= set(item) <= {"path", "sha256", "offset"},
                    "invalid appended image record fields")
            offset = number(item.get("offset", size), "append offset")
        require(size <= offset < MAX_IMAGE, "append offset overlaps a preceding image or exceeds size limit")
        require(isinstance(item["path"], str) and item["path"], "image path is required")
        require(isinstance(item["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", item["sha256"]),
                "image SHA-256 must be 64 lowercase hex characters")
        path = (filename.parent / item["path"]).resolve()
        require(path.is_file(), "image does not exist: " + str(path))
        require(0 < path.stat().st_size <= MAX_IMAGE - offset, "image set exceeds size limit")
        with path.open("rb") as stream:
            content = stream.read(MAX_IMAGE - offset + 1)
        require(0 < len(content) <= MAX_IMAGE - offset, "image changed size or exceeds limit")
        digest = hashlib.sha256(content).hexdigest()
        require(digest == item["sha256"], "image SHA-256 mismatch: " + str(path))
        size = offset + len(content)
        parts.append((offset, content))
        images.append({"path": str(path), "size": len(content), "sha256": digest, "offset": offset})
    entry = number(mu["entry_offset"], "entry_offset")
    require(0 <= entry < len(parts[0][1]) and entry % 4 == 0, "entry point is outside/aligned incorrectly for Mu")
    framebuffer = profile.get("framebuffer", "native")
    require(framebuffer in ("native", "bgra-shadow"), "unsupported framebuffer policy")
    compatibility = profile.get("guest_compatibility", "none")
    require(compatibility in GUEST_COMPATIBILITY, "unsupported guest compatibility policy")
    require(compatibility != "t8142-experimental" or chip == 0x8142,
            "t8142-experimental compatibility requires chip 0x8142")
    require(compatibility != T8152_PMU_UNDEF or chip == T8152_CHIP,
            "t8152-pmu-undef compatibility requires chip 0x8152")
    usb = profile.get("usb", "disabled")
    require(usb in ("disabled", "off"), "USB passthrough (D4) is a run_guest.py option "
            "(--windows-usb on T8152); a profile takes usb disabled or off")
    usb = "disabled"
    serial = profile.get("serial", "disabled")
    require(serial in ("disabled", "capture", "cdc"), "unsupported serial capture policy")
    n1 = profile.get("n1", "disabled")
    require(n1 in ("disabled", "j813-v1", "j813-acquire-v1"), "unsupported N1 handoff profile")
    require(n1 == "disabled" or (chip == 0x8142 and model == "Mac17,3"),
            "J813 N1 handoff requires chip 0x8142 and model Mac17,3")
    mtp = profile.get("mtp", "disabled")
    require(mtp in ("disabled", "j813-v1"), "unsupported MTP handoff profile")
    require(mtp == "disabled" or (chip == 0x8142 and model == "Mac17,3"),
            "j813-v1 MTP handoff requires chip 0x8142 and model Mac17,3")
    cpufreq = profile.get("cpufreq", "disabled")
    require(cpufreq in ("disabled", "j813-manual-v1"), "unsupported CPU frequency policy")
    require(cpufreq == "disabled" or (chip == 0x8142 and model == "Mac17,3"),
            "j813-manual-v1 CPU frequency handoff requires chip 0x8142 and model Mac17,3")
    ecpu_boot_state = 0
    if "ecpu_boot_state" in profile:
        ecpu_boot_state = profile["ecpu_boot_state"]
        require(type(ecpu_boot_state) is int and
                ECPU_HANDOFF_STATE <= ecpu_boot_state <= ECPU_LAST_STATE,
                f"ecpu_boot_state must be an integer E command state "
                f"{ECPU_HANDOFF_STATE}..{ECPU_LAST_STATE}")
        require(chip == 0x8142 and model == "Mac17,3",
                "ecpu_boot_state requires chip 0x8142 and model Mac17,3")
        # Either cpufreq policy may accompany it. Mu 45087b59 double-frees the CPUF
        # SSDT when the j813-manual-v1 marker is absent (boot 145), so keep the marker;
        # AppleCpuFreq 0.2.0.3 then fails D0 on a raised E state after reads alone.
    unified_display = "disabled"
    if "unified_display" in profile:
        # One spelling for on, none for off: absent is the only way to keep the legacy pair.
        require(profile["unified_display"] == "j813-v1" and type(profile["unified_display"]) is str,
                "unsupported unified display selection")
        require(chip == 0x8142 and model == "Mac17,3",
                "j813-v1 unified display requires chip 0x8142 and model Mac17,3")
        unified_display = "j813-v1"
    declarations = profile.get("reserved_memory", [])
    require(isinstance(declarations, list) and len(declarations) <= 16,
            "reserved_memory must be a list of at most 16 ranges")
    reserved, names = [], set()
    for item in declarations:
        require(isinstance(item, dict) and (set(item) == {"name", "base", "size"} or
                set(item) == {"name", "from_ram_top", "size"}), "invalid reserved memory range fields")
        name = item["name"]
        require(isinstance(name, str) and re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]{0,63}", name)
                and name not in names, "reserved memory names must be unique identifiers")
        names.add(name)
        origin = "base" if "base" in item else "from_ram_top"
        start, size = number(item[origin], origin), number(item["size"], "reserved memory size")
        require(0 <= start < 1 << 64 and 0 < size < 1 << 64 and start % 4096 == 0 and size % 4096 == 0,
                "reserved memory ranges must be positive, 4-KiB aligned and fit u64")
        require((origin != "base" or start + size <= 1 << 64) and
                (origin != "from_ram_top" or start >= size), "reserved memory range wraps its address space")
        reserved.append({"name": name, origin: start, "size": size})
    if mtp != "disabled":
        # This reservation is owned by the device ABI, not an optional hint.
        require("aura-mtp-handoff" not in names, "aura-mtp-handoff is an automatic reservation")
        reserved.append({"name": "aura-mtp-handoff", "base": MTP_RESERVED_BASE,
                         "size": MTP_RESERVED_SIZE})
    guest_ram, ipi, on_reset, gic_cpuif = t8152_profile_fields(
        profile, chip, framebuffer, compatibility, n1, mtp, declarations)
    wfi = t8152_wfi_field(profile, chip)
    boot_args = profile.get("boot_args", "")
    require(isinstance(boot_args, str) and len(boot_args.encode("ascii")) <= 255 and "\0" not in boot_args,
            "boot_args must be at most 255 ASCII bytes without NUL")
    xfer = plan_xfer(profile.get("xfer"))
    # Hash every source file before introducing any padding. Offsets and the
    # final digest bind the zero-filled layout separately from source hashes.
    composed, cursor = [], 0
    for offset, content in parts:
        if offset != cursor:
            composed.append(bytes(offset - cursor))
        composed.append(content)
        cursor = offset + len(content)
    return LaunchPlan(filename, profile, raw, hashlib.sha256(raw).hexdigest(), b"".join(composed),
                      tuple(images), entry, chip, model, framebuffer, compatibility, usb, n1,
                      tuple(reserved), boot_args, serial, mtp, cpufreq, xfer,
                      ecpu_boot_state=ecpu_boot_state, unified_display=unified_display,
                      guest_ram=guest_ram, ipi=ipi, on_reset=on_reset, gic_cpuif=gic_cpuif,
                      wfi=wfi)


T8152_CHIP = 0x8152
T8152_GUEST_ALIGN = 0x10000
T8152_MIN_GUEST = 256 << 20
T8152_ENTRY_OFFSET = 16 << 20  # ADT and TrustCache copies sit below the image (HV.load_raw)
# ADT nodes the Windows guest never sees on T8152 (on top of the stage-2 denylist):
# the QSPI NOR with the NVRAM partition, the speaker amps and their i2c buses, audio,
# the AOP and ADMAC.  Matched against /arm-io children: prefix, or "audio" anywhere.
T8152_HIDE_PREFIXES = ("qspi", "aop", "admac", "i2c")


def t8152_profile_fields(profile, chip, framebuffer, compatibility, n1, mtp, reserved):
    """T8152 (J804) profile rules. Returns (guest_ram, ipi, on_reset, gic_cpuif).

    ipi "fast" is an opt-in for after Phase 3 measurement (c). gic_cpuif is "software" on
    T8152 (the default there; "carrier" is refused since measurement (f) found the hardware
    CPU interface path UNDEF) and "carrier" everywhere else."""
    ipi = profile.get("ipi", "poll")
    require(ipi in ("poll", "fast"), "ipi must be poll or fast")
    gic_cpuif = profile.get("gic_cpuif", "software" if chip == T8152_CHIP else "carrier")
    require(gic_cpuif in ("carrier", "software"), "gic_cpuif must be carrier or software")
    on_reset = profile.get("on_reset", "stay")
    require(on_reset in ("stay", "reboot"), "on_reset must be stay or reboot")
    if chip != T8152_CHIP:
        require("guest_ram" not in profile and ipi == "poll" and gic_cpuif == "carrier",
                "guest_ram, ipi and gic_cpuif are T8152 profile fields")
        return None, ipi, on_reset, gic_cpuif
    ram = profile.get("guest_ram")
    require(isinstance(ram, dict) and set(ram) == {"base", "top"},
            "T8152 profiles must bind guest_ram {base, top}")
    base, top = number(ram["base"], "guest_ram base"), number(ram["top"], "guest_ram top")
    validate_t8152_window(base, top)
    require(gic_cpuif == "software", "T8152 requires gic_cpuif software (measurement (f): "
            "GIC_CPUIF_UNDEF in the hardware CPU interface configuration)")
    require(framebuffer == "bgra-shadow", "T8152 requires framebuffer bgra-shadow")
    require(compatibility in ("none", T8152_PMU_UNDEF),
            "T8152 takes guest_compatibility none or t8152-pmu-undef")
    require(n1 == "disabled" and mtp == "disabled", "T8152 has no N1 or MTP handoff")
    require(not reserved, "T8152 profiles take no reserved_memory; guest_ram is the whole guest")
    return (base, top), ipi, on_reset, gic_cpuif


def t8152_wfi_field(profile, chip):
    """T8152 carrier idle: "poll" only (no WFI at EL2). Phase 3 measurement (a) found
    WFI_WIPES on T8152, so "wfi" is refused here and by the resident."""
    wfi = profile.get("wfi", "poll")
    require(wfi in ("poll", "wfi"), "wfi must be poll or wfi")
    require(chip == T8152_CHIP or wfi == "poll", "wfi is a T8152 profile field")
    require(wfi == "poll", "wfi: WFI does not preserve state on T8152 (WFI_WIPES); use poll")
    return wfi


def validate_t8152_window(base, top):
    require(0 <= base < top <= 1 << 64, "guest_ram must be a non-empty range")
    require(base % T8152_GUEST_ALIGN == 0, "guest_ram base must be 64 KiB aligned")
    require((top - base) % 0x4000 == 0, "guest_ram size must be a multiple of 16 KiB")
    require(top - base >= T8152_MIN_GUEST, "guest_ram must be at least 256 MiB")


def _pairs(node):
    """{name: (base, size)} of a /chosen map node's <u64 base, u64 size> properties."""
    out = {}
    for key, value in node._properties.items():
        if key == "name":
            continue
        if isinstance(value, (bytes, bytearray)) and len(value) == 16:
            value = struct.unpack("<QQ", value)
        if isinstance(value, (tuple, list)) and len(value) == 2 and all(isinstance(v, int) for v in value):
            out[key] = (value[0], value[1])
    return out


def t8152_window_conflicts(base, top, phys_base, mem_size, carveouts, memory_map, occupied=()):
    """Host-side mirror of the resident's hv_t8152_set_guest_ram() checks.

    carveouts and memory_map map names to (base, size); occupied is a list of
    (name, base, size) spans the host knows about (aura and its heaps, the live
    ADT, the scanout). Returns the list of conflicts; empty means acceptable.
    The resident re-checks all of it, plus CTXR_A-D and CTRR_C/D, which only it
    can read.
    """
    validate_t8152_window(base, top)
    problems = []
    if not (phys_base <= base and top <= phys_base + mem_size):
        problems.append(f"[{base:#x}, {top:#x}) is not inside boot_args RAM "
                        f"[{phys_base:#x}, {phys_base + mem_size:#x})")
    for name, (start, size) in sorted(carveouts.items()):
        if not size or (start == phys_base and size == mem_size):
            continue  # the usable-RAM carve-out itself (region-id-24 on J804)
        if start < top and base < start + size:
            problems.append(f"overlaps carve-out {name} [{start:#x}, {start + size:#x})")
    for name, (start, size) in sorted(memory_map.items()):
        if size and start < top and base < start + size:
            why = (" (the GPU pool of a --windows-gpu probe session earlier this boot: the GPU "
                   "firmware may still reach it; reboot the M6)") if name == GPU_POOL_SLOT else ""
            problems.append(f"overlaps memory-map {name} [{start:#x}, {start + size:#x}){why}")
    for name, start, size in occupied:
        if size and start < top and base < start + size:
            problems.append(f"overlaps {name} [{start:#x}, {start + size:#x})")
    return problems


def t8152_check_window(adt, args, base, top, occupied=()):
    """Raise unless [base, top) is a usable T8152 guest RAM window for this ADT."""
    problems = t8152_window_conflicts(
        base, top, args.phys_base, args.mem_size,
        _pairs(adt["/chosen/carveout-memory-map"]), _pairs(adt["/chosen/memory-map"]), occupied)
    require(not problems, "guest_ram refused: " + "; ".join(problems))


def t8152_hidden_nodes(adt):
    """/arm-io children hidden from a T8152 guest, plus every NVRAM store node."""
    paths = []
    for node in adt["/arm-io"]:
        name = node.name
        if name.startswith(T8152_HIDE_PREFIXES) or "audio" in name:
            paths.append("/arm-io/" + name)

    def walk(node, path):
        for child in node:
            here = path + "/" + child.name
            compatible = getattr(child, "compatible", None) or []
            if isinstance(compatible, str):
                compatible = [compatible]
            if "nvram,nor" in compatible and not any(here.startswith(p + "/") or here == p
                                                      for p in paths):
                paths.append(here)
            walk(child, here)
    walk(adt, "")
    return paths


def windows_system_reset(proxy):
    """Guest asked for SYSTEM_RESET (exit status 2): shut NVMe down, then reset by WD1.

    nvme_shutdown is a no-op when aura owns no NVMe session; a failure raises and the
    machine is not reset (hold power to exit; nothing was written in M2/M3)."""
    proxy.nvme_shutdown()
    proxy.reboot()



# ---- D1: the carrier-emulated AHCI disk (src/hv/windows/hv_ahci.h) -------------------------
#
# The carrier emulates one AHCI 1.3.1 HBA at a fixed guest IPA and keeps the disk in DRAM at
# the top of the --windows-guest-ram window.  The guest RAM becomes [BASE, store_base); the
# store, its slack and a 64 KiB descriptor fill [store_base, TOP).  Contract values must match
# the resident (P_HV_WINDOWS_AHCI QUERY), Mu's J804Windows.h and the FD marker at 0x110.
CAPS2_MAGIC = 0x4157493200000000          # 'AWI2'
CAPS2_AHCI = 0x0001                       # AHCI RAM store v1
CONFIG_T8152_AHCI = 0x80000                # hv_windows_configure BIT(19)
AHCI_VERSION = 1
AHCI_ABAR = 0xF40000000
AHCI_ABAR_SIZE = 0x4000
AHCI_GSIV = 990
AHCI_CAP = 0x81341F00
AHCI_PROPERTY = "aura,windows-ahci-v1"
AHCI_BOOT = {"disk": 0, "ramdisk": 1}
AHCI_DESC_SIZE = 0x10000
AHCI_STORE_ALIGN = 32 << 20
AHCI_MIN_GUEST = 4 << 30
AHCI_MIN_DISK = 1 << 20
AHCI_SIZE_ROUND = 256 << 20
AHCI_UEFI_TOP = 128 << 20                 # PrePi's UEFI region at the guest RAM top
J804_HANDOFF = (0x10220000000, 0x10220204000)   # Mu J804Windows.h J804_HANDOFF_BASE/TOP
AHCI_FD_MARKER_OFFSET = 0x110
AHCI_FD_MARKER = b"J804AHC1"
AHCI_CHUNK = 16 << 20
AHCI_OP_QUERY, AHCI_OP_ATTACH, AHCI_OP_SEAL_UPLOAD, AHCI_OP_SEAL_EXIT, AHCI_OP_STATUS = range(5)
AHCI_Q = {"version": 0, "abar": 1, "abar_size": 2, "intid": 3, "cap": 4, "min_guest": 5,
          "store_align": 6, "desc_size": 7}
AHCI_ATTACH_KEEP = 0x1
AHCI_ATTACH_ACCEPT_UNSEALED = 0x2
AHCI_ATTACH_OVERWRITE = 0x4
AHCI_LIVE_STATES = ("READY", "RUNNING", "SEALED")   # a fresh attach refuses these by default
AHCI_ERROR = (1 << 64) - 1
AHCI_STATES = ("EMPTY", "LOADING", "READY", "RUNNING", "SEALED")
AHCI_DESC_MAGIC = b"AWAHCI01"
AHCI_DESC_FORMAT = "<8sIIQQQIIIiQ"        # up to desc_crc32 (0x40 bytes), then <I crc
AHCI_DESC_CRC_OFF = 0x40
AHCI_NOTE_OFF = 0x200
AHCI_NOTE_SIZE = 256


@dataclass(frozen=True)
class AhciPlan:
    window_base: int
    window_top: int
    store_base: int
    disk_bytes: int

    @property
    def desc_base(self):
        return self.window_top - AHCI_DESC_SIZE

    @property
    def guest_top(self):
        return self.store_base

    @property
    def guest_bytes(self):
        return self.store_base - self.window_base


def parse_size(text):
    """'12G', '12GiB', '512M', '0x300000000' or a byte count."""
    if isinstance(text, int):
        return text
    m = re.fullmatch(r"\s*(0x[0-9a-fA-F]+|\d+)\s*([KMGT]?)(?:i?B)?\s*", str(text))
    require(m is not None, f"cannot read the size {text!r} (examples: 12G, 13000M, 0x300000000)")
    value = int(m.group(1), 0)
    return value << {"": 0, "K": 10, "M": 20, "G": 30, "T": 40}[m.group(2)]


def ahci_default_disk_bytes(image_bytes):
    """The image size rounded up to 256 MiB."""
    return max(AHCI_SIZE_ROUND, _aligned(image_bytes, AHCI_SIZE_ROUND))


def ahci_max_disk_bytes(base, top):
    """The largest disk the window [base, top) can hold with 4 GiB of guest RAM left."""
    floor = _aligned(base + AHCI_MIN_GUEST, AHCI_STORE_ALIGN)
    desc = top - AHCI_DESC_SIZE
    return max(0, (desc - floor) & ~511)


def plan_t8152_ahci(base, top, disk_bytes, image_bytes=None):
    """Split the T8152 guest RAM window into guest RAM and the AHCI store (D1-AHCI-DESIGN 4.1).

    Raises ValueError with the reason.  The resident re-checks the store against every
    carve-out, CTXR range, aura, its heaps, the ADT and the scanout (hv_t8152_reserve_store)."""
    validate_t8152_window(base, top)
    require(top % AHCI_DESC_SIZE == 0, "guest_ram top must be 64 KiB aligned for the AHCI "
            "store descriptor")
    require(isinstance(disk_bytes, int) and disk_bytes % 512 == 0 and disk_bytes >= AHCI_MIN_DISK,
            f"AHCI disk size {disk_bytes} must be a multiple of 512 bytes and at least 1 MiB")
    if image_bytes is not None:
        require(image_bytes <= disk_bytes, f"the disk image ({image_bytes} bytes) is larger "
                f"than the AHCI store ({disk_bytes} bytes)")
    cap = ahci_max_disk_bytes(base, top)
    require(disk_bytes <= cap, f"an AHCI disk of {disk_bytes / 2**30:.2f} GiB leaves less than "
            f"4 GiB of guest RAM in [{base:#x}, {top:#x}); the largest is {cap} bytes "
            f"({cap / 2**30:.2f} GiB)")
    store_base = (top - AHCI_DESC_SIZE - disk_bytes) & ~(AHCI_STORE_ALIGN - 1)
    plan = AhciPlan(base, top, store_base, disk_bytes)
    require(plan.guest_bytes >= AHCI_MIN_GUEST, "guest RAM below 4 GiB")
    lo, hi = J804_HANDOFF
    require(base <= lo and hi <= store_base - AHCI_UEFI_TOP,
            f"the PrePi hand-off copies [{lo:#x}, {hi:#x}) must lie in guest RAM below the "
            f"128 MiB UEFI region at its top ({store_base - AHCI_UEFI_TOP:#x})")
    return plan


def check_t8152_ahci_layout(plan, adt_base, image_end):
    """After load_raw: the ADT/TrustCache/FD/blobs/boot_args span [adt_base, image_end) must lie
    in guest RAM, clear of the hand-off copies and below the UEFI region at the RAM top."""
    lo, hi = J804_HANDOFF
    uefi = plan.guest_top - AHCI_UEFI_TOP
    require(plan.window_base <= adt_base < image_end <= uefi,
            f"the loaded payload [{adt_base:#x}, {image_end:#x}) must end below the UEFI region "
            f"at {uefi:#x}")
    require(image_end <= lo or hi <= adt_base,
            f"the loaded payload [{adt_base:#x}, {image_end:#x}) overlaps the PrePi hand-off "
            f"copies [{lo:#x}, {hi:#x})")


def ahci_handshake(plan, boot):
    """The /chosen/aura,windows-ahci-v1 property (Mu J804Windows.h, 40-byte form)."""
    return struct.pack("<QQIIQQ", AHCI_ABAR, AHCI_ABAR_SIZE, AHCI_GSIV, AHCI_BOOT[boot],
                       plan.store_base, plan.window_top)


def resolve_ahci_boot(boot, appended_ramdisk):
    """--windows-ahci-boot auto: the appended RAM disk if there is one, else the AHCI disk."""
    require(boot in ("auto", "disk", "ramdisk"), "AHCI boot must be auto, disk or ramdisk")
    if boot == "auto":
        return "ramdisk" if appended_ramdisk else "disk"
    if boot == "ramdisk":
        require(appended_ramdisk, "--windows-ahci-boot ramdisk needs an appended RAM disk (-a)")
    return boot


def fd_ahci_marker(fd):
    """(ABAR, GSIV) from an AHCI FD's raw marker at 0x110, or None for any other FD."""
    block = bytes(fd[AHCI_FD_MARKER_OFFSET:AHCI_FD_MARKER_OFFSET + 16])
    if len(block) != 16 or block[:8] != AHCI_FD_MARKER:
        return None
    page, gsiv = struct.unpack("<II", block[8:])
    return page << 12, gsiv


def validate_ahci_contract(caps2, query, fd):
    """The resident's CAPS2 and QUERY answers and the FD marker must all be the D1 contract.

    query(name) returns the resident's QUERY value.  Raises ValueError with the reason."""
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and caps2 & CAPS2_AHCI,
            "the resident has no AHCI disk (P_HV_WINDOWS_CAPS2 bit 0): rebuild the carrier "
            "from j804-windows with D1")
    expect = {"version": AHCI_VERSION, "abar": AHCI_ABAR, "abar_size": AHCI_ABAR_SIZE,
              "intid": AHCI_GSIV, "cap": AHCI_CAP, "min_guest": AHCI_MIN_GUEST,
              "store_align": AHCI_STORE_ALIGN, "desc_size": AHCI_DESC_SIZE}
    for name, value in expect.items():
        got = query(name)
        require(got == value, f"resident AHCI {name} is {got:#x}, this launcher expects "
                f"{value:#x}")
    marker = fd_ahci_marker(fd)
    require(marker is not None, "the payload is not an AHCI FD (no J804AHC1 marker at 0x110): "
            "build Mu with J804_WINDOWS_AHCI=TRUE (out/j804-windows-ahci)")
    require(marker == (AHCI_ABAR, AHCI_GSIV), f"the FD's AHCI marker {marker[0]:#x}/{marker[1]} "
            f"differs from the carrier's {AHCI_ABAR:#x}/{AHCI_GSIV}")


def pack_ahci_desc(window_top, store_base, disk_bytes, state, generation=0, disk_crc=0,
                   last_exit=0, alive_cntpct=0):
    head = struct.pack(AHCI_DESC_FORMAT, AHCI_DESC_MAGIC, AHCI_VERSION, AHCI_DESC_CRC_OFF,
                       window_top, store_base, disk_bytes, state, generation, disk_crc,
                       last_exit, alive_cntpct)
    return head + struct.pack("<I", zlib.crc32(head))


def parse_ahci_desc(blob):
    """The store descriptor's header (see hv_ahci.h); ValueError if it is not a valid one."""
    blob = bytes(blob)
    require(len(blob) >= AHCI_DESC_CRC_OFF + 4, "AHCI store descriptor is truncated")
    fields = struct.unpack(AHCI_DESC_FORMAT, blob[:AHCI_DESC_CRC_OFF])
    (crc,) = struct.unpack("<I", blob[AHCI_DESC_CRC_OFF:AHCI_DESC_CRC_OFF + 4])
    require(fields[0] == AHCI_DESC_MAGIC and fields[1] == AHCI_VERSION and
            fields[2] == AHCI_DESC_CRC_OFF, "no AHCI store descriptor here (magic/version): "
            "nothing to keep")
    require(zlib.crc32(blob[:AHCI_DESC_CRC_OFF]) == crc, "AHCI store descriptor CRC mismatch")
    names = ("magic", "version", "header_size", "window_top", "store_base", "disk_bytes",
             "state", "generation", "disk_crc32", "last_exit", "alive_cntpct")
    desc = dict(zip(names, fields))
    desc["desc_crc32"] = crc
    desc["state_name"] = AHCI_STATES[desc["state"]] if desc["state"] < len(AHCI_STATES) else "?"
    return desc


def ahci_keep_plan(base, top, desc, accept_unsealed=False):
    """The plan of a kept store, from its descriptor (the resident re-verifies the disk CRC)."""
    require(desc["window_top"] == top, f"the kept store belongs to a window ending at "
            f"{desc['window_top']:#x}, not {top:#x}")
    ok = ("READY", "SEALED") + (("RUNNING",) if accept_unsealed else ())
    require(desc["state_name"] in ok, f"the kept store is {desc['state_name']}"
            + ("" if accept_unsealed else "; only a sealed store is kept "
               "(--windows-ahci-keep-unsealed accepts a RUNNING one)"))
    plan = plan_t8152_ahci(base, top, desc["disk_bytes"])
    require(plan.store_base == desc["store_base"], "the kept store's base does not match this "
            "window's plan")
    return plan


def parse_ahci_note(blob):
    """The host note at +0x200 of a descriptor blob: {"sha256", "bytes", "uploaded"} or None
    when there is none or it is not one this launcher wrote."""
    raw = bytes(blob[AHCI_NOTE_OFF:AHCI_NOTE_OFF + AHCI_NOTE_SIZE]).split(b"\0", 1)[0]
    m = re.fullmatch(rb"image sha256 ([0-9a-f]{64}) bytes (\d+) uploaded (\S+)", raw)
    if not m:
        return None
    return {"sha256": m.group(1).decode(), "bytes": int(m.group(2)),
            "uploaded": m.group(3).decode()}


def describe_ahci_note(note):
    if note is None:
        return "none (the store records no image)"
    return f"image sha256 {note['sha256']}, {note['bytes']} bytes, uploaded {note['uploaded']}"


def check_ahci_keep_expect(note, expect):
    """--windows-ahci-keep-expect: the kept store must come from the image with this SHA-256."""
    if not expect:
        return
    expect = expect.strip().lower()
    require(re.fullmatch(r"[0-9a-f]{64}", expect) is not None,
            "--windows-ahci-keep-expect takes a 64-digit SHA-256")
    require(note is not None, "the kept store has no host note, so its image cannot be "
            "checked against --windows-ahci-keep-expect")
    require(note["sha256"] == expect, f"the kept store holds image sha256 {note['sha256']}, "
            f"not {expect}")


def check_ahci_fresh_overwrite(blob, top, overwrite):
    """A fresh upload zeroes [store_base, top): refuse it over a live store at this window's
    top unless --windows-ahci-overwrite.  Returns the descriptor found (or None)."""
    try:
        desc = parse_ahci_desc(blob)
    except ValueError:
        return None
    if desc["window_top"] != top or desc["state_name"] not in AHCI_LIVE_STATES:
        return None
    what = (f"a {desc['state_name']} store, generation {desc['generation']}, "
            f"{desc['disk_bytes'] / 2**30:.2f} GiB; {describe_ahci_note(parse_ahci_note(blob))}")
    require(overwrite, f"the window top already holds {what}. Keep it with "
            "--windows-ahci-keep, or pass --windows-ahci-overwrite to zero it")
    print(f"T8152 AHCI: overwriting {what}")
    return desc


def ahci_image_info(path):
    """(size, warnings) of a raw GPT disk image; ValueError if it is not one."""
    path = Path(path)
    size = path.stat().st_size
    require(size >= 34 * 512 and size % 512 == 0, f"{path} is not a raw disk image (size "
            f"{size} is not a multiple of 512 bytes of at least 34 sectors)")
    warnings = []
    with path.open("rb") as f:
        mbr = f.read(512)
        gpt = f.read(512)
        f.seek(size - 512)
        backup = f.read(512)
    require(mbr[510:512] == b"\x55\xaa" and any(mbr[446 + 16 * i + 4] == 0xEE for i in range(4)),
            f"{path} has no protective MBR: pass a raw GPT disk (qemu-img convert -O raw)")
    require(gpt[:8] == b"EFI PART", f"{path} has no GPT header at LBA 1")
    if backup[:8] != b"EFI PART":
        warnings.append("no backup GPT header at the last LBA")
    return size, warnings


def ahci_image_chunks(f, size, disk_bytes, chunk=AHCI_CHUNK):
    """(offset, data or None) for every chunk of the disk; None for a chunk that is a hole or
    all zero in the image (or beyond it).  Holes are found with SEEK_DATA where the host has
    it, so a sparse image is never read in full."""
    zero = bytes(chunk)
    seek_data = getattr(os, "SEEK_DATA", None)
    fd = f.fileno()
    for off in range(0, disk_bytes, chunk):
        n = min(chunk, disk_bytes - off)
        if off >= size:
            yield off, n, None
            continue
        if seek_data is not None:
            try:
                data_at = os.lseek(fd, off, seek_data)
            except OSError:
                data_at = size  # ENXIO: only a hole follows
            if data_at >= off + n:
                yield off, n, None
                continue
        f.seek(off)
        data = f.read(min(n, size - off))
        if len(data) < n:
            data += bytes(n - len(data))
        yield off, n, (None if data == zero[:n] else data)


def upload_ahci_image(path, disk_bytes, write, chunk=AHCI_CHUNK, progress=None):
    """Stream the image into the store with write(offset, data), skipping zero chunks (the
    resident zero-filled the store).  Returns (crc32 over all disk_bytes, image sha256,
    uploaded bytes)."""
    crc, sha, sent = 0, hashlib.sha256(), 0
    zero = bytes(chunk)
    size = Path(path).stat().st_size
    with open(path, "rb") as f:
        for off, n, data in ahci_image_chunks(f, size, disk_bytes, chunk):
            block = data if data is not None else zero[:n]
            crc = zlib.crc32(block, crc)
            if off < size:
                sha.update(block[:min(n, size - off)])
            if data is not None:
                write(off, data)
                sent += n
            if progress:
                progress(off + n, disk_bytes, sent)
    return crc, sha.hexdigest(), sent


def ahci_host_note(sha256, image_bytes, when):
    note = f"image sha256 {sha256} bytes {image_bytes} uploaded {when}".encode()
    return note[:AHCI_NOTE_SIZE - 1].ljust(AHCI_NOTE_SIZE, b"\0")


# ---- D5: the AHCI disk on an internal NVMe partition (src/hv/windows/hv_ahci_nvme.h) --------
#
# Windows keeps seeing the same carrier-emulated AHCI disk; its backend is a dedicated GPT
# partition of nvme0n1, read and written by aura's EL2 NVMe driver.  The partition is chosen by
# the owner's private configuration (never by number): disk GUID, unique partition GUID, type
# GUID, name and first/last LBA, all re-checked by the resident against the live GPT.  The
# values below must agree with the resident (CAPS2 bit 5, P_HV_WINDOWS_AHCI ops 5-8).
CAPS2_AHCI_NVME = 0x0020                  # D5 v1
AHCI_OP_ATTACH_NVME, AHCI_OP_DETACH, AHCI_OP_NVME_STATUS, AHCI_OP_NVME_TEST = 5, 6, 7, 8
NVME_REQ_MAGIC = b"AWNVME01"
NVME_REQ_VERSION = 1
NVME_REQ_SIZE = 0xC0
NVME_REQ_FORMAT = "<8sII16s16s16sQQQQ72s24s"
NVME_NAME_UNITS = 36
NVME_RW, NVME_FLUSH_EVERY, NVME_GATE, NVME_SCRATCH = 0x1, 0x2, 0x4, 0x8
NVME_STATES = ("DETACHED", "ATTACHED", "RUNNING", "CLEAN", "FAILED")
NVME_STATUS = {"state": 0, "flags": 1, "commands": 2, "dev_commands": 3, "longest_us": 4,
               "flushes": 5, "rmw": 6, "guard_hits": 7, "first": 8, "last": 9, "wrote": 10,
               "failed_commands": 11, "blocks_read": 12, "blocks_written": 13,
               "inner_bytes": 14, "longest_flush_us": 15}
NVME_TEST = {"write": 1, "verify": 2, "flush": 3, "crc": 4}
NVME_LBA_BYTES = 4096
NVME_ALIGN = 1 << 20
NVME_CONFIG_NAME = "d5/windows-partition.env"
NVME_CONFIG_REQUIRED = ("J804_WIN_PART_TYPE", "J804_WIN_PART_UUID", "J804_WIN_PART_NAME",
                        "J804_WIN_DISK_GUID", "J804_WIN_PART_FIRST_LBA", "J804_WIN_PART_LAST_LBA",
                        "J804_WIN_LBA_BYTES", "J804_WIN_IMAGE_BYTES")
# P_REBOOT args[0] that resets even after an unconfirmed close (src/proxy.h P_REBOOT_FORCE).
REBOOT_FORCE = int.from_bytes(b"FORCE", "little")
# Public type GUIDs another OS owns (gpt_check.c denied_types): never the Windows partition.
NVME_DENIED_TYPES = frozenset(t.upper() for t in (
    "C12A7328-F81F-11D2-BA4B-00A0C93EC93B", "024DEE41-33E7-11D3-9D69-0008C781F39F",
    "7C3457EF-0000-11AA-AA11-00306543ECAC", "48465300-0000-11AA-AA11-00306543ECAC",
    "426F6F74-0000-11AA-AA11-00306543ECAC", "69646961-6700-11AA-AA11-00306543ECAC",
    "52637672-7900-11AA-AA11-00306543ECAC", "0FC63DAF-8483-4772-8E79-3D69D8477DE4",
    "B921B045-1DF0-41C3-AF44-4C6F280D3FAE", "933AC7E1-2EB4-4F13-B844-0E14E2AEF915",
    "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F", "E6D6D379-F507-44C2-A23C-238F2A3DF928",
    "A19D880F-05FC-4D3B-A006-743F0F84911E", "CA7D7CCB-63ED-4C53-861C-1742536059CC",
    "BC13C2FF-59E6-4262-A352-B275FD6F7172", "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
    "E3C9E316-0B5C-4DB8-817D-F92DF00215AE", "DE94BBA4-06D1-4D40-A16A-BFD50179D6AC"))


def parse_env_file(text):
    """KEY=VALUE lines (shell style: comments, optional quotes, no expansion)."""
    values = {}
    for number_, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[7:].lstrip()
        key, sep, value = line.partition("=")
        require(sep and re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", key),
                f"line {number_} of the partition config is not KEY=VALUE")
        value = value.strip()
        if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
            value = value[1:-1]
        values[key] = value
    return values


def _guid(text, label):
    import uuid
    try:
        return uuid.UUID(text)
    except (ValueError, AttributeError, TypeError):
        raise ValueError(f"{label} {text!r} is not a GUID") from None


def _lba(text, label):
    require(isinstance(text, str) and re.fullmatch(r"[0-9]+", text.replace("_", "")),
            f"{label} {text!r} is not a decimal LBA")
    return int(text.replace("_", ""))


@dataclass(frozen=True)
class NvmePartition:
    disk_guid: object
    part_guid: object
    type_guid: object
    name: str
    first: int
    last: int
    lba_bytes: int
    image_bytes: int
    image_sha256: str = ""

    @property
    def blocks(self):
        return self.last - self.first + 1

    @property
    def size(self):
        return self.blocks * self.lba_bytes

    @property
    def sectors(self):
        return self.size // 512


def nvme_partition_config(values):
    """The private config's values -> NvmePartition, refusing anything incomplete or unsafe."""
    missing = [k for k in NVME_CONFIG_REQUIRED if not values.get(k)]
    require(not missing, f"the partition config lacks {', '.join(missing)} (written by "
            "j804-private-data/d5/install-windows-partition.sh at install)")
    name = values["J804_WIN_PART_NAME"]
    require(0 < len(name.encode("utf-16-le")) // 2 <= NVME_NAME_UNITS,
            f"the partition name must be 1-{NVME_NAME_UNITS} UTF-16 units")
    part = NvmePartition(
        disk_guid=_guid(values["J804_WIN_DISK_GUID"], "J804_WIN_DISK_GUID"),
        part_guid=_guid(values["J804_WIN_PART_UUID"], "J804_WIN_PART_UUID"),
        type_guid=_guid(values["J804_WIN_PART_TYPE"], "J804_WIN_PART_TYPE"),
        name=name,
        first=_lba(values["J804_WIN_PART_FIRST_LBA"], "J804_WIN_PART_FIRST_LBA"),
        last=_lba(values["J804_WIN_PART_LAST_LBA"], "J804_WIN_PART_LAST_LBA"),
        lba_bytes=_lba(values["J804_WIN_LBA_BYTES"], "J804_WIN_LBA_BYTES"),
        image_bytes=_lba(values["J804_WIN_IMAGE_BYTES"], "J804_WIN_IMAGE_BYTES"),
        image_sha256=values.get("J804_WIN_IMAGE_SHA256", ""))
    require(part.lba_bytes == NVME_LBA_BYTES, "the internal NVMe has 4096-byte LBAs; the config "
            f"says {part.lba_bytes}")
    require(part.first <= part.last, "the partition's first LBA is after its last")
    require(part.first * part.lba_bytes % NVME_ALIGN == 0,
            "the partition does not start on a 1 MiB boundary")
    require(str(part.type_guid).upper() not in NVME_DENIED_TYPES,
            "the partition type is a well-known type (EFI, APFS, Linux, Microsoft basic data, "
            "...): the Windows partition needs its own private type GUID")
    require(part.image_bytes > 0 and part.image_bytes % 512 == 0,
            "J804_WIN_IMAGE_BYTES must be a positive multiple of 512")
    require(part.size >= part.image_bytes,
            f"the partition ({part.size} bytes) is smaller than the image ({part.image_bytes})")
    for spec in filter(None, values.get("J804_WIN_OTHER_PARTS", "").replace(" ", "").split(",")):
        lo, sep, hi = spec.partition("-")
        require(sep, f"J804_WIN_OTHER_PARTS entry {spec!r} is not FIRST-LAST")
        lo, hi = _lba(lo, "J804_WIN_OTHER_PARTS"), _lba(hi, "J804_WIN_OTHER_PARTS")
        require(hi < part.first or lo > part.last,
                f"the Windows partition [{part.first}, {part.last}] overlaps another partition "
                f"[{lo}, {hi}] recorded in the config")
    return part


def load_nvme_config(path):
    path = Path(path)
    require(path.is_file(), f"no partition config at {path} (pass --windows-ahci-nvme-config, "
            "or set J804_PRIVATE_DATA)")
    return nvme_partition_config(parse_env_file(path.read_text()))


def default_nvme_config_path(environ=os.environ):
    root = environ.get("J804_PRIVATE_DATA")
    return Path(root) / NVME_CONFIG_NAME if root else None


def resolve_nvme_partition(part, key):
    """--windows-ahci-partition KEY: the partition's unique GUID or its GPT name, as the config
    records them.  A number is refused: GPT numbers change (macOS 27.2 renumbered this disk)."""
    require(isinstance(key, str) and key.strip(), "--windows-ahci-partition needs the partition's "
            "GPT GUID or GPT name")
    key = key.strip()
    require(not re.fullmatch(r"(p|nvme0n1p|/dev/nvme0n1p)?[0-9]+", key),
            f"--windows-ahci-partition {key!r} looks like a partition number: partitions are "
            "chosen by GUID or name only")
    try:
        import uuid
        if uuid.UUID(key) == part.part_guid:
            return part
    except ValueError:
        pass
    require(key == part.name, "--windows-ahci-partition matches neither the configured partition "
            "GUID nor its name")
    return part


def nvme_request(part, flags):
    """The 192-byte ATTACH_NVME request (hv_ahci_nvme.h struct hv_ahci_nvme_req)."""
    require(flags & ~(NVME_RW | NVME_FLUSH_EVERY | NVME_GATE | NVME_SCRATCH) == 0,
            "bad NVMe attach flags")
    require(not (flags & NVME_SCRATCH) or flags & NVME_GATE, "scratch is a gate-test attach only")
    name = part.name.encode("utf-16-le").ljust(2 * NVME_NAME_UNITS, b"\0")
    blob = struct.pack(NVME_REQ_FORMAT, NVME_REQ_MAGIC, NVME_REQ_VERSION, flags,
                       part.disk_guid.bytes_le, part.part_guid.bytes_le, part.type_guid.bytes_le,
                       part.first, part.last, part.image_bytes, 0, name, b"\0" * 24)
    assert len(blob) == NVME_REQ_SIZE
    return blob


def validate_nvme_contract(caps2):
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and
            caps2 & CAPS2_AHCI_NVME and caps2 & CAPS2_AHCI,
            "the resident has no NVMe-backed AHCI disk (P_HV_WINDOWS_CAPS2 bit 5): rebuild the "
            "carrier from j804-windows-nvme or later")


def nvme_ahci_plan(base, top):
    """With the NVMe backend there is no DRAM store: guest RAM is the whole window."""
    return AhciPlan(window_base=base, window_top=top, store_base=top, disk_bytes=0)


def ahci_nvme_handshake(boot):
    """The 24-byte /chosen/aura,windows-ahci-v1 (no store range; Mu J804AhciDxe accepts it)."""
    return struct.pack("<QQII", AHCI_ABAR, AHCI_ABAR_SIZE, AHCI_GSIV, AHCI_BOOT[boot])


def describe_nvme_state(state):
    return NVME_STATES[state] if 0 <= state < len(NVME_STATES) else f"unknown ({state})"


def nvme_test_pattern(seed, rel_block, word):
    """hv_ahci_nvme_pattern(): the W0 gate's 64-bit word `word` (0..511) of partition block
    `rel_block`, to check blocks from Linux after the carrier-closed reset."""
    m = (1 << 64) - 1
    z = seed ^ (rel_block * 0x9E3779B97F4A7C15 & m) ^ (word << 32 | word)
    z = (z + 0x9E3779B97F4A7C15) & m
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & m
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & m
    return z ^ (z >> 31)


# ---- D4: usb-drd1 (and drd3) xHCI passthrough (src/hv/windows/hv_usb.h) ---------------------
#
# The carrier passes only the xHCI registers through, publishes AIC 1692 as SPI 995 and gives
# the controller a DART identity map of guest RAM.  These values must agree with the resident
# (CAPS2 bit 1, P_HV_WINDOWS_USB) and with Mu's J804Windows.h: the FD marker at 0x120 and the
# 40-byte /chosen/aura,windows-usb-v1 handshake, without which Mu's J804XhciAcpiDxe installs no
# SSDT J804XHC1 (\_SB.XHC1) and Windows never sees the controller.
CAPS2_USB = 0x0002                        # USB passthrough v1
CONFIG_T8152_USB = 0x100000                 # hv_windows_configure BIT(20)
USB_OP_ATTACH, USB_OP_STATUS, USB_OP_DETACH = 1, 2, 3
USB_STATUS = {"attached": 0, "usbsts": 1, "portsc1": 2, "portsc2": 3, "irqs": 4,
              "dart0_exc": 5, "dart1_exc": 6, "gctl": 7, "unowned": 8, "storm": 9}
# Port-independent STATUS indices: the DART identity window of the live attach (2**64 - 1 when
# nothing is attached).  Read once after ATTACH for the handshake, not at exit.
USB_STATUS_DMA_BASE, USB_STATUS_DMA_TOP = 10, 11
USB_ERRORS = {-1: "not configured, not J804, no guest RAM, the guest already started or an "
                  "earlier ATTACH this session",
              -2: "bad port mask (drd0 or an unknown port)",
              -3: "the port is not in host mode as aura's j804_usb_prep leaves it",
              -4: "the ADT does not describe the expected controller, line or DARTs",
              -5: "the DART is protected, live or not the Gen3 USB DART",
              -6: "the xHCI is running, not ready, or outside its 0x8000 window",
              -7: "the DART tables do not fit or failed their self check",
              -8: "programming the DART or stage 2 failed (torn down again)"}
USB_PORTS = {"drd1": 1, "drd3": 3}
USB_PORT_BIT = {1: 0x2, 3: 0x8}
USB_XHCI = {1: 0x40A430000, 3: 0x412430000}
USB_XHCI_SIZE = 0x8000
USB_GSIV = {1: 995, 3: 997}
USB_FD_MARKER = {1: (0x120, b"J804USB1"), 3: (0x130, b"J804USB3")}
USB_CHOICES = ("off", "drd1", "drd1+drd3", "drd3")
USB_PROPERTY = "aura,windows-usb"          # diagnostic: the port mask asked for
USB_DISABLED_PROPERTY = "aura,windows-usb-disabled"
# Mu J804Windows.h J804_USB_ADT_PROPERTY / _SIZE / J804_USB_PORT_DRD1 (layout below).
USB_HANDSHAKE_PROPERTY = "aura,windows-usb-v1"
USB_HANDSHAKE_SIZE = 40
USB_HANDSHAKE_FORMAT = "<IIQQQQ"   # mask, GSIV, xHCI base, xHCI size, DMA base, DMA top
USB_DMA_ALIGN = 0x4000
J804_BOARD_ID = 0x22


def parse_windows_usb(value):
    """--windows-usb: "off" (or "disabled") -> (), else the sorted port numbers.  usb-drd0 is the
    proxy's own link and is refused in every spelling."""
    require(isinstance(value, str), "--windows-usb takes off, drd1, drd3 or drd1+drd3")
    text = value.strip().lower()
    if text in ("off", "disabled", "none", ""):
        return ()
    ports = []
    for part in re.split(r"[+,\s]+", text):
        name = re.sub(r"^usb-", "", part)
        if re.fullmatch(r"(?:usb|drd|port)?-?(?:0x)?0+", name):
            raise ValueError("usb-drd0 is the proxy's CDC link to this Mac: it is never passed "
                             "through (--windows-usb takes drd1 and/or drd3)")
        require(name in USB_PORTS, f"--windows-usb: unknown port {part!r} (off, drd1, drd3, "
                "drd1+drd3)")
        ports.append(USB_PORTS[name])
    require(len(set(ports)) == len(ports), "--windows-usb names a port twice")
    return tuple(sorted(ports))


def usb_port_mask(ports):
    mask = 0
    for port in ports:
        mask |= USB_PORT_BIT[port]
    return mask


def usb_spelling(ports):
    return "+".join(f"drd{p}" for p in ports) if ports else "disabled"


def fd_usb_markers(fd):
    """{port: (xHCI base, GSIV)} for every D4 marker in the FD (16 bytes each: magic, u32
    base >> 12, u32 GSIV); empty for an FD without USB."""
    found = {}
    for port, (offset, magic) in USB_FD_MARKER.items():
        block = bytes(fd[offset:offset + 16])
        if len(block) == 16 and block[:8] == magic:
            base, gsiv = struct.unpack_from("<II", block, 8)
            found[port] = (base << 12, gsiv)
    return found


def validate_usb_request(ports, chip, board, caps2, fd):
    """Before anything is uploaded: the chip and board, the resident's CAPS2 bit and the FD's
    XHC1/XHC2 markers must all agree with --windows-usb, in both directions."""
    markers = fd_usb_markers(fd)
    for port, (base, gsiv) in markers.items():
        require(port in ports, f"this FD describes usb-drd{port} to Windows (marker "
                f"{USB_FD_MARKER[port][1].decode()} at {USB_FD_MARKER[port][0]:#x}) but "
                f"--windows-usb does not pass it through: Windows would touch a denied page. "
                f"Pass --windows-usb {usb_spelling(tuple(sorted(markers)))} or use an FD built "
                f"without J804_WINDOWS_USB")
        require((base, gsiv) == (USB_XHCI[port], USB_GSIV[port]),
                f"the FD's usb-drd{port} marker {base:#x}/{gsiv} differs from the carrier's "
                f"{USB_XHCI[port]:#x}/{USB_GSIV[port]}")
    if not ports:
        return
    require(chip == T8152_CHIP, "--windows-usb is a T8152 (J804) option")
    require(board == J804_BOARD_ID, f"--windows-usb needs the J804 board (0x22), not {board:#x}")
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and caps2 & CAPS2_USB,
            "the resident has no USB passthrough (P_HV_WINDOWS_CAPS2 bit 1): rebuild the "
            "carrier at or after the D4 commit")
    for port in ports:
        require(port in markers, f"--windows-usb drd{port}: the FD has no "
                f"{USB_FD_MARKER[port][1].decode()} marker at {USB_FD_MARKER[port][0]:#x}, so "
                f"Windows has no XHC{1 if port == 1 else 2} to bind: build Mu with "
                f"J804_WINDOWS_USB=TRUE (out/j804-windows-usb)")


def usb_handshake(ports, dma_base, dma_top, guest_ram, ahci_plan=None):
    """/chosen/aura,windows-usb-v1 for Mu's J804XhciAcpiDxe, written only after ATTACH returned
    0.  dma_base/dma_top are the window the resident's DART actually maps (STATUS 10/11); they
    must equal the guest RAM window the launcher configured, be 16 KiB aligned, non-empty, clear
    of the xHCI window and of the AHCI store.  Mu re-checks all of it and fails closed."""
    require(tuple(ports) == (1,), "Mu's v1 USB handshake describes usb-drd1 only (mask 0x2); "
            f"--windows-usb {usb_spelling(tuple(ports))} has no Mu build")
    for name, value in (("base", dma_base), ("top", dma_top)):
        require(isinstance(value, int) and 0 <= value < 1 << 64 and value != (1 << 64) - 1,
                f"the resident reported no DART window {name} ({value!r}): nothing is attached")
        require(value % USB_DMA_ALIGN == 0, f"the DART window {name} {value:#x} is not 16 KiB "
                "aligned")
    require(dma_base < dma_top, f"the DART window [{dma_base:#x}, {dma_top:#x}) is empty")
    require((dma_base, dma_top) == tuple(guest_ram),
            f"the resident's DART window [{dma_base:#x}, {dma_top:#x}) is not the guest RAM "
            f"window [{guest_ram[0]:#x}, {guest_ram[1]:#x}) the launcher configured")
    xhci, size = USB_XHCI[1], USB_XHCI_SIZE
    require(dma_top <= xhci or xhci + size <= dma_base, "the DART window overlaps the xHCI "
            "registers")
    if ahci_plan is not None:
        store = (ahci_plan.store_base, ahci_plan.window_top)
        require(dma_top <= store[0] or store[1] <= dma_base,
                f"the DART window [{dma_base:#x}, {dma_top:#x}) reaches the AHCI store "
                f"[{store[0]:#x}, {store[1]:#x}): the store must never be DMA-reachable")
    value = struct.pack(USB_HANDSHAKE_FORMAT, usb_port_mask(ports), USB_GSIV[1], xhci, size,
                        dma_base, dma_top)
    assert len(value) == USB_HANDSHAKE_SIZE
    return value


def describe_usb_attach(result):
    """P_HV_WINDOWS_USB ATTACH's answer as text (0 is success)."""
    value = result - (1 << 64) if result >= 1 << 63 else result
    return "attached" if value == 0 else USB_ERRORS.get(value, f"error {value}")


# ---- G0: GPU admission and logging hooks (src/hv/windows/hv_gpu.h) ----------------------------
#
# Off by default.  "admit" runs the resident's admission port once (both firmware identities
# against the live ADT, the range table, the firmware classes, the read-only handoff page);
# "log" also puts read-as-zero / write-ignored logging hooks on the sgx window and the
# post-office page.  Neither powers the GPU, starts its firmware, passes a register through or
# publishes an interrupt.  Mu's GPU0 node is described by the handoff page, which the 16-byte
# /chosen/aura,windows-gpu-v1 handshake points at; it is written only after ADMIT returned 0.
# Contract: windows-m6/G0-CARRIER-CONTRACT.md.
#
# G1 adds "probe": after ADMIT the resident powers the GPU on (PSD, its private recipe), once per
# boot (the boot marker /chosen/memory-map/MemoryMapReserved-15 in the live ADT, which an in-place
# carrier reload keeps and a reboot clears), then forwards exactly the measured registers through
# its hooks (GPU ID words, CPU_CONTROL with RUN guarded, CPU_STATUS, the GFX post-office windows),
# maps the UAT regions RW and the DATA regions RO, and logs every guest access by offset.  The
# firmware then runs for the rest of the boot; the pool stays reserved (MemoryMapReserved-14).
#
# G2 adds "g2": "probe" plus the resident's G2 guards (op 7, between ADMIT and POWER): whole
# 128-bit mailbox messages only, init data once per firmware role, the private recipe's refused
# message types (the recovery request) never forwarded, the guest's sgx and CPU_CONTROL/STATUS
# access locked once init data went out, every message counted by kind, and an audit of the GPU
# page tables (UAT) before every message on the two application endpoints and at teardown.  No
# register is forwarded beyond G1's.  Two owner decisions are separate, explicit grants
# (--windows-gpu-g2-grant): "fw-mmio" (G2-D1) declares the firmware register windows the GPU
# firmware's page tables may map in the handoff page (class 12), "boot-control" (G2-D2) lets
# POWER do the one checked sgx boot-control step (a pmgr-child register).  Needs a G2 Mu FD (the
# G1 FD refuses a page with state bit 7, word 0xec or class-12 ranges, so GPU0 would be missing).
CAPS2_GPU = 0x0004                        # G0 GPU admission v1
CAPS2_GPU_PROBE = 0x0010                  # G1 power-on, boot marker, mediated forwarding
CAPS2_GPU_G2 = 0x0040                     # G2 guards, grants, UAT audit (bit 5 on gpu3)
GPU_MODES = ("off", "admit", "log", "probe", "g2")
GPU_PROBE_MODES = ("probe", "g2")         # the modes that power the GPU and forward
GPU_OP_ADMIT, GPU_OP_HOOK, GPU_OP_STATUS = 1, 2, 3
GPU_OP_POWER, GPU_OP_PROBE, GPU_OP_STOP = 4, 5, 6
GPU_OP_G2 = 7
GPU_STATUS_STATE, GPU_STATUS_HANDOFF_IPA, GPU_STATUS_HANDOFF_SIZE = 0, 1, 2
GPU_STATUS_RANGES, GPU_STATUS_ACCESSES, GPU_STATUS_MAILBOX_GFX = 3, 4, 5
GPU_STATUS_BOOT, GPU_STATUS_FORWARDED, GPU_STATUS_DENIED = 6, 7, 8
GPU_STATUS_RUN_REFUSED, GPU_STATUS_FAULTS, GPU_STATUS_MAP_COUNT = 9, 10, 11
GPU_STATUS_MAP_DROPPED, GPU_STATUS_POWER_RECIPE = 12, 13
GPU_STATUS_G2_RECIPE, GPU_STATUS_G2, GPU_STATUS_DECLARED = 14, 15, 16
GPU_STATUS_BOOT_CONTROL = 0x48            # + 0 before, + 1 after the boot-control step
GPU_STATUS_MSG = 0x400                    # + 0x10 * (2 * role + tx) + kind
GPU_STATUS_GUARD = 0x460                  # + guard
GPU_STATUS_AUDIT = 0x480                  # + audit field
GPU_STATUS_AUDIT_FRESH = 0x4c0            # + 4 i + {0 entry, 1 descriptor, 2 kind}
GPU_STATUS_PSD_BEFORE, GPU_STATUS_PSD_AFTER, GPU_STATUS_GPU_ID, GPU_STATUS_CPU = \
    0x20, 0x28, 0x30, 0x40
GPU_STATUS_CLASS = 0x100
GPU_STATUS_MAP = 0x1000
GPU_BOOT_FOREIGN, GPU_BOOT_NO_SLOT = 1 << 31, 1 << 30
GPU_BOOT_BITS = ("claimed", "powered", "power failed", "found on", "forwarding", "ended",
                 "boot-control step done", "G2 armed", "G2 guard fired")
GPU_DISPOSITIONS = ("denied", "passed", "mediated", "refused", "fault", "locked", "blocked")
GPU_BOOT_MARKER_SLOT = "MemoryMapReserved-15"
GPU_POOL_SLOT = "MemoryMapReserved-14"
GPU_CLASSES = ("none", "sgx", "sgx-hole", "asc-ctrl", "asc", "iop-vbar", "mailbox", "fw-rw",
               "fw-ro", "fw-text", "pool", "handoff", "fw-mmio")
# G2 (hv_gpu.h, gpu_t8152_g2.h, gpu_t8152_uat.h)
GPU_G2_GRANTS = {"fw-mmio": 0x1, "boot-control": 0x2}
GPU_G2_FLAGS = ("armed", "fw-mmio declared", "boot-control granted", "boot-control done",
                "sgx locked")
GPU_G2_INIT_SHIFT = 8                     # bit 8 + role: init data forwarded to that role
GPU_MSG_KINDS = ("mgmt", "crashlog", "init-data", "init-ack", "event", "gfx-other", "post-init",
                 "doorbell", "fw-control", "ep21-other", "other-ep", "half")
GPU_GUARDS = ("second init-data refused", "refused types", "refused by the UAT audit",
              "sgx accesses locked", "mailbox halves denied")
GPU_UAT_CLASSES = ("pool", "uat", "data", "fw-mmio", "text", "dram", "mmio", "bad", "table")
GPU_AUDIT = {"audits": 0, "roots": 1, "tables": 2, "leaves": 3, "outliers": 12, "known": 13,
             "fresh": 14, "fresh_guest": 15, "capped": 16, "baseline": 17, "baseline_overflow": 18,
             "fresh_max": 19}
GPU_ERRORS = {-1: "not T8152, the policy is not armed, no guest RAM, the guest already entered, "
                  "ADMIT already ran this carrier session, or HOOK before ADMIT",
              -2: "the pool is not 32 MiB aligned inside guest RAM (nothing was spent)",
              -3: "the admission refused (firmware identity, ADT shape or layout)",
              -4: "the range table does not fit or breaks a rule",
              -5: "the stage-2 policy refused a firmware class, or a map or hook failed",
              -6: "no memory for the handoff page",
              -7: "this carrier has no GPU power recipe (built without the private file), or a "
                  "PSD block is not inside an ADT pmgr range (nothing was touched)",
              -8: "this boot's GPU firmware session is already used (boot marker), cannot be "
                  "claimed, or the GPU was found on: reboot the M6",
              -9: "a PSD power-on step failed; the GPU state is unknown: reboot the M6",
              -10: "G2: the sgx boot-control step refused the value it read or read back the "
                   "wrong one (the GPU is powered): reboot the M6",
              -11: "G2: no private G2 recipe in the resident, unknown grant bits, or a firmware "
                   "register window or the boot-control register failed its ADT check (nothing "
                   "was touched)"}
GPU_HANDSHAKE_PROPERTY = "aura,windows-gpu-v1"
GPU_HANDSHAKE_SIZE = 16
GPU_HANDSHAKE_FORMAT = "<QQ"     # handoff page IPA, handoff page size
GPU_HANDOFF_SIZE = 0x4000
GPU_POOL_ALIGN = 32 << 20


def parse_windows_gpu(value):
    """--windows-gpu: off (or disabled), admit, log, probe or g2."""
    require(isinstance(value, str), "--windows-gpu takes off, admit, log, probe or g2")
    text = value.strip().lower()
    if text in ("off", "disabled"):
        return "off"
    require(text in GPU_MODES,
            f"--windows-gpu: unknown mode {value!r} (off, admit, log, probe, g2)")
    return text


def parse_windows_gpu_g2_grant(value, mode):
    """--windows-gpu-g2-grant: "none" or a comma list of fw-mmio (G2-D1) and boot-control
    (G2-D2); only with --windows-gpu g2.  Returns the grant bits for op 7."""
    require(isinstance(value, str), "--windows-gpu-g2-grant takes none, fw-mmio, boot-control")
    names = [n.strip().lower() for n in value.split(",") if n.strip()]
    if names in ([], ["none"]):
        return 0
    require(mode == "g2", "--windows-gpu-g2-grant needs --windows-gpu g2")
    bits = 0
    for name in names:
        require(name in GPU_G2_GRANTS, f"--windows-gpu-g2-grant: unknown grant {name!r} "
                f"({', '.join(GPU_G2_GRANTS)})")
        require(not bits & GPU_G2_GRANTS[name], f"--windows-gpu-g2-grant: {name} given twice")
        bits |= GPU_G2_GRANTS[name]
    return bits


def describe_gpu_g2_grants(bits):
    names = [n for n, b in GPU_G2_GRANTS.items() if bits & b]
    return "+".join(names) if names else "none"


def validate_gpu_request(mode, chip, board, caps2, recipe=None, boot=None, g2_recipe=None):
    """Before anything is uploaded: the chip, the board and the resident's CAPS2 bit 2; for
    "probe" and "g2" also CAPS2 bit 4, the resident's power recipe (STATUS 13) and a free boot
    marker (STATUS 6), so a used boot is refused before the upload, not after hv_init(); for
    "g2" also CAPS2 bit 6 and the resident's G2 recipe (STATUS 14)."""
    if mode == "off":
        return
    require(chip == T8152_CHIP, "--windows-gpu is a T8152 (J804) option")
    require(board == J804_BOARD_ID, f"--windows-gpu needs the J804 board (0x22), not {board:#x}")
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and caps2 & CAPS2_GPU,
            "the resident has no G0 GPU admission (P_HV_WINDOWS_CAPS2 bit 2): rebuild the "
            "carrier at or after the G0 commit")
    if mode not in GPU_PROBE_MODES:
        return
    require(caps2 & CAPS2_GPU_PROBE, "the resident has no G1 GPU power-on (P_HV_WINDOWS_CAPS2 "
            "bit 4): rebuild the carrier at or after the G1 commit")
    if mode == "g2":
        require(caps2 & CAPS2_GPU_G2, "the resident has no G2 GPU guards (P_HV_WINDOWS_CAPS2 "
                "bit 6): rebuild the carrier at or after the G5b merge (gpu3 had G2 on bit 5)")
        require(g2_recipe == 1, "the resident was built without the private G2 recipe "
                "(J804_PRIVATE_DATA/gpu/carrier/g18g-j804-gpu-g2.h): --windows-gpu g2 is refused")
    require(recipe == 1, "the resident was built without the private GPU power recipe "
            "(J804_PRIVATE_DATA/aura/g18g-j804-gpu-power.h): --windows-gpu probe cannot power "
            "the GPU")
    require(boot == 0, f"this boot's GPU firmware session is already used: "
            f"{describe_gpu_boot(boot)}. Reboot the M6 before another --windows-gpu {mode}")


def describe_gpu_boot(value):
    """STATUS 6 (the boot marker) as text."""
    if not isinstance(value, int):
        return f"unknown ({value!r})"
    if value == 0:
        return "free"
    if value & GPU_BOOT_FOREIGN:
        return f"/chosen/memory-map/{GPU_BOOT_MARKER_SLOT} is set but holds no GPU marker"
    if value & GPU_BOOT_NO_SLOT:
        return f"this ADT has no /chosen/memory-map/{GPU_BOOT_MARKER_SLOT}"
    bits = [name for i, name in enumerate(GPU_BOOT_BITS) if value & (1 << i)]
    return "marker " + (", ".join(bits) if bits else f"{value:#x}")


def gpu_map_entry(addr, count, kind, first):
    """One access-map entry from STATUS 0x1000 + 4 i (+0 address, +1 count, +2 kind, +3 first)."""
    width, write, disp, cls = kind & 0xff, (kind >> 8) & 1, (kind >> 16) & 0xff, kind >> 24
    return {"addr": addr, "count": count, "bits": 8 << width, "write": bool(write),
            "disposition": GPU_DISPOSITIONS[disp] if disp < len(GPU_DISPOSITIONS) else str(disp),
            "class": GPU_CLASSES[cls] if cls < len(GPU_CLASSES) else str(cls), "first": first}


def gpu_pool(ram_base, ram_top, mib):
    """The GPU pool: the top `mib` MiB of guest RAM, 32 MiB aligned, at most half of it."""
    require(isinstance(mib, int) and mib > 0 and mib % 32 == 0,
            f"--windows-gpu-pool-mib {mib!r} must be a positive multiple of 32")
    size = mib << 20
    require(ram_top % GPU_POOL_ALIGN == 0, f"guest RAM top {ram_top:#x} is not 32 MiB aligned")
    require(size <= (ram_top - ram_base) // 2, f"a {mib} MiB pool is more than half of guest RAM "
            f"[{ram_base:#x}, {ram_top:#x})")
    return ram_top - size, size


def gpu_pool_check_layout(pool, ram_base, image_end, floor=None):
    """The pool against what the launcher loaded: everything from guest RAM base to image_end
    (ADT, trust cache, Mu FD, appended payloads, SEPFW, preoslog, boot_args) must end at or
    below the pool base, or Mu refuses the pool and the GPU0 node silently disappears.  With
    `floor` (bytes; the AHCI layout's 4 GiB), guest RAM below the pool must keep at least that."""
    pool_base, _ = pool
    require(isinstance(image_end, int) and ram_base <= image_end,
            f"the loaded image end {image_end!r} is not in guest RAM")
    require(image_end <= pool_base,
            f"the loaded image ends at {image_end:#x}, inside the GPU pool from {pool_base:#x}: "
            "use a smaller --windows-gpu-pool-mib or a smaller payload")
    if floor is not None:
        require(pool_base - ram_base >= floor,
                f"guest RAM below the GPU pool is {(pool_base - ram_base) >> 20} MiB, under the "
                f"{floor >> 20} MiB Windows floor of the AHCI layout")


def gpu_handshake(ipa, size):
    """/chosen/aura,windows-gpu-v1 for Mu's GPU0 node: where the read-only handoff page is."""
    require(isinstance(ipa, int) and 0 < ipa < (1 << 64) - 1 and ipa % GPU_HANDOFF_SIZE == 0,
            f"the resident reported no 16 KiB-aligned handoff page ({ipa!r})")
    require(size == GPU_HANDOFF_SIZE, f"the handoff page is {size!r} bytes, not 16 KiB")
    value = struct.pack(GPU_HANDSHAKE_FORMAT, ipa, size)
    assert len(value) == GPU_HANDSHAKE_SIZE
    return value


def describe_gpu(result):
    """P_HV_WINDOWS_GPU ADMIT/HOOK answer as text (0 is success)."""
    value = result - (1 << 64) if result >= 1 << 63 else result
    return "ok" if value == 0 else GPU_ERRORS.get(value, f"error {value}")
# ---- I0: Mac-fed keyboard and pointer (src/hv/windows/hv_input.h, input_model.h) -----------
#
# The carrier emulates one register page (0xF50000000, 16 KiB) and SPI 991 and parses host
# frames from the RX direction of the vUART CDC pipe.  These values must agree with the
# resident (CAPS2 bit 3, P_HV_WINDOWS_INPUT QUERY) and with Mu's J804Windows.h: the FD marker
# "J804INP1" at 0x140 and the 24-byte /chosen/aura,windows-input-v1 handshake, without which
# Mu installs no SSDT J804INP0 (\_SB.INP0, _HID NTAS2040) and Windows never sees the device.
CAPS2_INPUT = 0x0008                      # input v1
CONFIG_T8152_INPUT = 0x200000               # hv_windows_configure BIT(21)
INPUT_OP_QUERY, INPUT_OP_ATTACH, INPUT_OP_STATUS, INPUT_OP_DETACH, INPUT_OP_PROBE = 0, 1, 2, 3, 4
INPUT_Q = {"version": 0, "base": 1, "size": 2, "intid": 3, "features": 4, "sources": 5}
INPUT_VERSION = 1
INPUT_BASE = 0xF50000000
INPUT_SIZE = 0x4000
INPUT_GSIV = 991
INPUT_FEATURES_REPORTS = 0x000F           # keyboard, absolute, relative, consumer
INPUT_SRC = {"host": 0x1, "kbd": 0x2, "tp": 0x4}
INPUT_SRC_FEATURE_SHIFT = 16              # FEATURES bits 16-23 are the live sources
INPUT_CHOICES = ("off", "host", "host+kbd", "host+kbd+tp")
INPUT_CARRIER_SOURCES = ("host", "kbd", "tp")  # I0 host feed, I1 keyboard, I2 trackpad
INPUT_FD_MARKER_OFFSET = 0x140
INPUT_FD_MARKER = b"J804INP1"
# Mu J804Windows.h J804_INPUT_ADT_PROPERTY / _SIZE (layout below).
INPUT_HANDSHAKE_PROPERTY = "aura,windows-input-v1"
INPUT_HANDSHAKE_FORMAT = "<QIIII"         # base, size, GSIV, feature mask, version
INPUT_HANDSHAKE_SIZE = 24
INPUT_STATUS = {"frames_ok": 0, "bad_crc": 1, "bad_header": 2, "bad_payload": 3, "skipped": 4,
                "seq_gaps": 5, "watchdog": 6, "reports": 7, "high_water": 8, "mtp": 9,
                "out_ok": 10, "out_bad": 11, "hellos": 12, "bad_access": 13, "rx_bytes": 14,
                "dropped": 15, "attached": 16, "ctrl": 17, "depth": 18, "dropped_now": 19,
                "leds": 20, "line": 21, "owns_rx": 22}
# I1/I2: the MTP's STATUS indices (hv_input.h) and its reasons (mtp_bringup.h).
INPUT_STATUS_MTP = {"mtp_state": 23, "mtp_reason": 24, "mtp_kbd": 25, "mtp_tp": 26,
                    "mtp_rtkit": 27, "mtp_poll_us": 28, "mtp_errors": 29, "mtp_smc": 30,
                    "mtp_gpio": 31}
MTP_STATES = {0: "off", 1: "identity", 2: "power", 3: "dart", 4: "rtkit", 5: "scm", 6: "init",
              7: "keyboard ready", 8: "trackpad ready"}
MTP_REASONS = {0: "none", 1: "identity", 2: "area", 3: "MTP already running (reboot through iBoot)",
               4: "TTBR", 5: "TCR", 6: "DART root", 7: "stream-4 map", 8: "PMGR power",
               9: "RTKit", 10: "SCM send", 11: "SCM timeout", 12: "INIT timeout",
               13: "keyboard layout", 14: "TX ring", 15: "keyboard enable unacknowledged",
               16: "keyboard enable refused", 17: "keyboard READY", 18: "RTKit at run time",
               19: "ring", 20: "checksums", 21: "trackpad parameter page or firmware CRC",
               22: "no multi-touch INIT", 23: "multi-touch enable", 24: "SMC pcIO write",
               25: "firmware unacknowledged or refused", 26: "power method 2 step",
               27: "multi-touch READY", 28: "no multi-touch AFE function in the ADT"}
INPUT_OPT_FN_FKEYS = 0x100                # ATTACH a1 bit 8: F1-F12 without Fn
# The MTP's RUN bit and its 13 PMGR states (mtp_bringup.h), for the launch-time check.
MTP_ASC_CPU_CONTROL = 0x314600044
MTP_RUN = 0x10
MTP_PMGR_PS = 0x30828c000
MTP_PMGR_COUNT = 13
MTP_PMGR_STRIDE = 0x20


def t8152_mtp_running(read32):
    """Is the M6's MTP running (RUN set), read-only and without ever reading an unpowered ASC?

    `read32(addr)` reads one register (the proxy's).  The 13 PMGR states come first (PMGR is
    always powered): with any MTP domain off the MTP cannot be running and ASC_CPU_CONTROL is
    not read (an unpowered ASC faults); with all on it is read.  Returns (running, detail).
    Nothing is ever written."""
    states = [read32(MTP_PMGR_PS + i * MTP_PMGR_STRIDE) for i in range(MTP_PMGR_COUNT)]
    off = [i for i, v in enumerate(states) if (v >> 4) & 0xF != 0xF]
    if off:
        return False, (f"MTP domains off ({len(off)} of {MTP_PMGR_COUNT}): not started since "
                       "iBoot")
    ctrl = read32(MTP_ASC_CPU_CONTROL)
    return bool(ctrl & MTP_RUN), f"MTP domains on, ASC_CPU_CONTROL {ctrl:#x}"


T8152_MTP_RUNNING_REFUSAL = (
    "the M6's MTP is running (ASC_CPU_CONTROL RUN set): an earlier carrier session in this "
    "power cycle brought up the internal keyboard/trackpad, and the MTP keeps DMAing into that "
    "session's area through its stream-4 entries, which may be RAM this launch uses. Leave by a "
    "reboot through iBoot (the carrier's P_REBOOT: proxyclient/tools/reboot.py) and start again "
    "from the fresh proxy")
# The reserved MTP area (mtp_bringup.h): 8 MiB, 2 MiB aligned, carved off the top of the guest
# RAM window; the launcher writes the parameter page at +0 and, with the trackpad, the
# firmware payload at +0x700000 before ATTACH.
INPUT_AREA_SIZE = 0x800000
INPUT_AREA_ALIGN = 0x200000
INPUT_AREA_FW = 0x700000
INPUT_AREA_FW_SIZE = 0x20000
INPUT_PARAMS_MAGIC = b"AWMTPP01"
INPUT_PARAMS_VERSION = 1
INPUT_PARAMS_LEN = 64
INPUT_PARAMS_TAP, INPUT_PARAMS_NATURAL = 0x1, 0x2
# The trackpad firmware container (the owner's own file, lifted from their recoveryOS by
# tools/j804-mtfw.py in linux-j804; never in a repo): <IIIII magic "HIDF", version 1, header
# length, payload length, interface-byte offset>, the header, the payload.
TP_FW_MAGIC = 0x46444948
TP_FW_VERSION = 1
TP_FW_HEADER = "<IIIII"
TP_FW_DEFAULT_NAME = "firmware/tpmtfw-j804.bin"
INPUT_ERRORS = {-1: "not configured, not J804, the guest already started or an earlier ATTACH "
                    "this session",
                -2: "bad source mask, or a source this carrier does not run",
                -3: "a reserved area was given for the host source",
                -4: "the register page could not be hooked"}

# The host feed's wire protocol v1 (input_model.h): little-endian frames
#   'M' '6' type len seq:u16 payload[len] crc32:u32 (zlib, over type .. payload)
INPUT_SYNC = b"M6"
INPUT_MAX_PAYLOAD = 64
INPUT_PROTO = 1
INPUT_HELLO, INPUT_KEYS, INPUT_ABS, INPUT_REL, INPUT_CONSUMER, INPUT_KEEPALIVE, \
    INPUT_RELEASE_ALL = range(1, 8)
INPUT_MAX_KEYS = 16
INPUT_ABS_MAX = 32767
INPUT_WATCHDOG_S = 2.0                    # the carrier releases held host keys after this
INPUT_KEEPALIVE_S = 0.5                   # a feeder holding keys sends KEEPALIVE this often


def parse_windows_input(value):
    """--windows-input: "off" -> (), else the source names in order: host, host+kbd (I1, the
    M6's own keyboard) or host+kbd+tp (I2, its trackpad too).  The host feed is always in: the
    Mac-fed path stays the fallback.  Whether the resident runs them is QUERY Q_SOURCES."""
    require(isinstance(value, str), "--windows-input takes off, host, host+kbd or host+kbd+tp")
    text = value.strip().lower()
    if text in ("off", "disabled", "none", ""):
        return ()
    require(text in INPUT_CHOICES, f"--windows-input: unknown choice {value!r} "
            f"({', '.join(INPUT_CHOICES)}; the Mac-fed host source always stays on)")
    sources = tuple(text.split("+"))
    missing = [s for s in sources if s not in INPUT_CARRIER_SOURCES]
    require(not missing, f"--windows-input {text}: {'+'.join(missing)} is not in this launcher")
    return sources


def input_needs_area(sources):
    """The internal keyboard (and the trackpad with it) runs the MTP in the reserved area."""
    return "kbd" in sources


def plan_t8152_input_area(top):
    """(area base, new window top): the 8 MiB area at the top of the guest RAM window, 2 MiB
    aligned; everything else (guest RAM, the AHCI store, the GPU pool) plans below it."""
    end = top & ~(INPUT_AREA_ALIGN - 1)
    base = end - INPUT_AREA_SIZE
    require(base > 0, f"the guest RAM window top {top:#x} leaves no room for the input area")
    return base, base


def default_tp_firmware_path(environ=os.environ):
    root = environ.get("J804_PRIVATE_DATA")
    return Path(root) / TP_FW_DEFAULT_NAME if root else None


def parse_tp_firmware(blob):
    """(payload, interface-byte offset) from the owner's HIDF container.  The checks are the
    container's own (the Linux driver makes the same ones); the payload must fit the area's
    128 KiB slot."""
    head = struct.calcsize(TP_FW_HEADER)
    require(isinstance(blob, (bytes, bytearray)) and len(blob) >= head,
            "the trackpad firmware is shorter than its container header")
    magic, version, hdr_len, data_len, iface_off = struct.unpack_from(TP_FW_HEADER, blob)
    require(magic == TP_FW_MAGIC, "the trackpad firmware is not a HIDF container")
    require(version == TP_FW_VERSION, f"trackpad firmware container version {version}, not 1")
    require(head <= hdr_len <= len(blob) and hdr_len + data_len <= len(blob),
            "the trackpad firmware's header or payload length runs past the file")
    require(0 < data_len <= INPUT_AREA_FW_SIZE,
            f"the trackpad firmware payload ({data_len} bytes) does not fit 128 KiB")
    require(iface_off < data_len, "the trackpad firmware's interface offset is past its payload")
    return bytes(blob[hdr_len:hdr_len + data_len]), iface_off


@dataclass(frozen=True)
class TpOptions:
    """The trackpad's tunables (input_tp.h); the defaults are the carrier's."""
    tap: bool = True
    natural: bool = False
    gain: float = 6.0          # pointer counts per mm, slow
    accel: float = 3.0         # extra factor at accel_hi (0: none)
    accel_lo: int = 15         # mm/s
    accel_hi: int = 150        # mm/s
    scroll_mm: float = 5.0     # finger travel per wheel or pan detent
    tap_ms: int = 180
    tap_travel_mm: float = 1.5
    tap_key_quiet_ms: int = 300
    click_settle_ms: int = 40
    palm_major: int = 0        # 0: off


def input_params(payload, iface_off, opts=TpOptions()):
    """The area's 64-byte parameter page for mtp_params_parse() (mtp_bringup.h)."""
    require(0 < len(payload) <= INPUT_AREA_FW_SIZE and 0 <= iface_off < len(payload),
            "bad trackpad firmware payload")
    gain, accel = round(opts.gain * 256), round(opts.accel * 256)
    scroll, travel = round(opts.scroll_mm * 1000), round(opts.tap_travel_mm * 1000)
    require(16 <= gain <= 0xFFFF, "--windows-input-tp-gain must be between 1/16 and 255")
    require(0 <= accel <= 0xFFFF, "--windows-input-tp-accel must be between 0 and 255")
    require(not accel or 0 <= opts.accel_lo < opts.accel_hi <= 0xFFFF,
            "the acceleration needs accel_lo < accel_hi")
    require(500 <= scroll <= 0xFFFF, "--windows-input-tp-scroll-mm must be between 0.5 and 65")
    require(0 <= opts.tap_ms <= 1000 and 0 <= travel <= 0xFFFF and
            0 <= opts.tap_key_quiet_ms <= 5000 and 0 <= opts.click_settle_ms <= 500 and
            0 <= opts.palm_major <= 0xFFFF, "a trackpad tunable is out of range")
    flags = (INPUT_PARAMS_TAP if opts.tap else 0) | (INPUT_PARAMS_NATURAL if opts.natural else 0)
    body = INPUT_PARAMS_MAGIC + struct.pack(
        "<IIIIIIIHHHHHHHHHHI", INPUT_PARAMS_VERSION, INPUT_PARAMS_LEN, INPUT_AREA_FW, len(payload),
        zlib.crc32(payload) & 0xFFFFFFFF, iface_off, flags, gain, accel, opts.accel_lo,
        opts.accel_hi, scroll, opts.tap_ms, travel, opts.tap_key_quiet_ms, opts.click_settle_ms,
        opts.palm_major, 0)
    assert len(body) == INPUT_PARAMS_LEN - 4
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def input_attach_mask(sources, fn_default="media"):
    """ATTACH a1: the source bits, and bit 8 for the "fkeys" Fn default (keyboard only)."""
    require(fn_default in ("media", "fkeys"), "--windows-input-fn-default is media or fkeys")
    mask = input_source_mask(sources)
    if fn_default == "fkeys":
        require("kbd" in sources, "--windows-input-fn-default fkeys needs the internal keyboard")
        mask |= INPUT_OPT_FN_FKEYS
    return mask


def check_input_sources(sources, carrier_sources):
    """QUERY Q_SOURCES must cover what is asked (an I0-only carrier answers 1, I1 3, I2 7)."""
    want = input_source_mask(sources)
    require(isinstance(carrier_sources, int) and carrier_sources & want == want,
            f"the resident runs input sources {carrier_sources:#x}, this launch asks for "
            f"{want:#x}: rebuild the carrier at or after the I1 (keyboard) / I2 (trackpad) commit")


def input_live_sources(sources, features):
    """After ATTACH: the asked sources FEATURES says are live, and those that are not."""
    live = [s for s in sources if features >> INPUT_SRC_FEATURE_SHIFT & INPUT_SRC[s]]
    return live, [s for s in sources if s not in live]


def describe_mtp_status(st):
    """STATUS 23..31 as one line (the run log's exit summary and the probe tool)."""
    state = st["mtp_state"]
    reason = st["mtp_reason"]
    failed = state & 0x80
    text = (f"MTP {'stopped at ' if failed else ''}{MTP_STATES.get(state & 0x7f, state)}"
            f"{', reason ' + MTP_REASONS.get(reason & 0xff, str(reason & 0xff)) if reason & 0xff else ''}")
    if reason >> 8 & 0xff:
        text += (f"; trackpad not up: {MTP_REASONS.get(reason >> 8 & 0xff, reason >> 8 & 0xff)} "
                 f"(step {reason >> 16 & 0xff})")
    return (f"{text}; keyboard reports {st['mtp_kbd'] & 0xffffffff} (bad {st['mtp_kbd'] >> 32}), "
            f"trackpad frames {st['mtp_tp'] & 0xffffffff} (bad {st['mtp_tp'] >> 32}), longest "
            f"service {st['mtp_poll_us']} us, ring errors {st['mtp_errors']:#x}, SMC pcIO writes "
            f"{st['mtp_smc'] & 0xffff} (failed {st['mtp_smc'] >> 16}), GPIO refused "
            f"{st['mtp_gpio']}")


def input_source_mask(sources):
    mask = 0
    for name in sources:
        mask |= INPUT_SRC[name]
    return mask


def fd_input_marker(fd):
    """(page base, GSIV) from an input FD's raw marker at 0x140 (magic, u32 base >> 12, u32
    GSIV), or None for any other FD."""
    block = bytes(fd[INPUT_FD_MARKER_OFFSET:INPUT_FD_MARKER_OFFSET + 16])
    if len(block) != 16 or block[:8] != INPUT_FD_MARKER:
        return None
    page, gsiv = struct.unpack("<II", block[8:])
    return page << 12, gsiv


def validate_input_request(sources, chip, board, caps2, fd, serial):
    """Before anything is uploaded: --windows-input against the chip, the board, the
    resident's CAPS2 bit, the FD's INP0 marker (both directions) and the serial mode."""
    marker = fd_input_marker(fd)
    if marker is not None:
        require(sources, "this FD describes the input device to Windows (marker J804INP1 at "
                "0x140) but --windows-input is off: Windows would touch an unhooked page. Pass "
                "--windows-input host or use an FD built without J804_WINDOWS_INPUT")
        require(marker == (INPUT_BASE, INPUT_GSIV), f"the FD's input marker {marker[0]:#x}/"
                f"{marker[1]} differs from the carrier's {INPUT_BASE:#x}/{INPUT_GSIV}")
    if not sources:
        return
    require(chip == T8152_CHIP, "--windows-input is a T8152 (J804) option")
    require(board == J804_BOARD_ID, f"--windows-input needs the J804 board (0x22), not "
            f"{board:#x}")
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and caps2 & CAPS2_INPUT,
            "the resident has no input device (P_HV_WINDOWS_CAPS2 bit 3): rebuild the carrier "
            "at or after the I0 commit")
    require(marker is not None, "--windows-input: the FD has no J804INP1 marker at 0x140, so "
            "Windows has no INP0 to bind: use a Mu FD built with J804_WINDOWS_INPUT=TRUE "
            "(WM6_J804_INPUT=TRUE Tools/build-j804-windows.sh in Mu j804-windows-next; "
            "out/j804-windows-next/artifacts/J804WIN_EFI.fd)")
    if "host" in sources:
        require(serial == "cdc", "--windows-input host feeds input over the vUART CDC pipe: "
                "it needs --windows-serial cdc (the default)")


def validate_input_contract(query):
    """The resident's QUERY answers must be this launcher's contract.  query(name) returns
    the value; raises ValueError with the reason."""
    expect = {"version": INPUT_VERSION, "base": INPUT_BASE, "size": INPUT_SIZE,
              "intid": INPUT_GSIV}
    for name, value in expect.items():
        got = query(name)
        require(got == value, f"resident input {name} is {got:#x}, this launcher expects "
                f"{value:#x}")
    features = query("features")
    require(features & INPUT_FEATURES_REPORTS == INPUT_FEATURES_REPORTS,
            f"resident input features {features:#x} lack the v1 reports")
    return features


def input_handshake(sources, features):
    """/chosen/aura,windows-input-v1 for Mu's J804InputAcpiDxe, written only after ATTACH
    returned 0: page base, size, GSIV, the feature mask the guest will read (report bits and
    the attached sources in bits 16-23), version 1."""
    require(sources, "no input handshake without a source")
    # The sources the resident says are live after ATTACH (bit 17 only once the keyboard is
    # READY, 18 the trackpad), never more than were asked for.
    asked = input_source_mask(sources) << INPUT_SRC_FEATURE_SHIFT
    mask = (features & INPUT_FEATURES_REPORTS) | (features & asked)
    require(mask & INPUT_SRC["host"] << INPUT_SRC_FEATURE_SHIFT,
            "the resident's FEATURES lack the host source")
    value = struct.pack(INPUT_HANDSHAKE_FORMAT, INPUT_BASE, INPUT_SIZE, INPUT_GSIV, mask,
                        INPUT_VERSION)
    assert len(value) == INPUT_HANDSHAKE_SIZE
    return value


def describe_input_attach(result):
    """P_HV_WINDOWS_INPUT ATTACH's answer as text (0 is success)."""
    value = result - (1 << 64) if result >= 1 << 63 else result
    return "attached" if value == 0 else INPUT_ERRORS.get(value, f"error {value}")


def input_frame(kind, payload=b"", seq=0):
    """One host-feed frame (wire protocol v1)."""
    require(isinstance(payload, (bytes, bytearray)) and len(payload) <= INPUT_MAX_PAYLOAD,
            "an input frame carries at most 64 payload bytes")
    body = struct.pack("<BBH", kind, len(payload), seq & 0xFFFF) + bytes(payload)
    return INPUT_SYNC + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def input_keys(modifiers, usages, seq=0):
    """KEYS: the full host keyboard state (modifier bits for usages 0xE0-0xE7, up to 16
    Keyboard-page usages 0x04-0x9F)."""
    usages = list(usages)
    require(0 <= modifiers <= 0xFF, "modifiers are one byte")
    require(len(usages) <= INPUT_MAX_KEYS, "at most 16 keys at once")
    require(all(0x04 <= u <= 0x9F for u in usages), "key usages are 0x04..0x9F")
    return input_frame(INPUT_KEYS, bytes([modifiers, len(usages), *usages]), seq)


def input_abs(buttons, x, y, wheel=0, pan=0, seq=0):
    require(0 <= buttons <= 0x1F and 0 <= x <= INPUT_ABS_MAX and 0 <= y <= INPUT_ABS_MAX and
            -127 <= wheel <= 127 and -127 <= pan <= 127, "ABS out of range")
    return input_frame(INPUT_ABS, struct.pack("<BHHbb", buttons, x, y, wheel, pan), seq)


def input_rel(buttons, dx, dy, wheel=0, pan=0, seq=0):
    require(0 <= buttons <= 0x1F and -32767 <= dx <= 32767 and -32767 <= dy <= 32767 and
            -127 <= wheel <= 127 and -127 <= pan <= 127, "REL out of range")
    return input_frame(INPUT_REL, struct.pack("<Bhhbb", buttons, dx, dy, wheel, pan), seq)


def input_consumer(usage, seq=0):
    require(0 <= usage <= 0x3FF, "consumer usages are 0..0x3FF")
    return input_frame(INPUT_CONSUMER, struct.pack("<H", usage), seq)


def input_hello(seq=0):
    return input_frame(INPUT_HELLO, struct.pack("<HH", INPUT_PROTO, 0), seq)


# ---- G6-S: the scanout service (src/hv/windows/hv_scanout.h, scanout_model.h) ---------------
#
# A register page (0xF60000000, 16 KiB) and SPI 992 for the WDDM driver: a virtual vblank at the
# panel rate from the system counter and FLIP, which copies a guest buffer into the iBoot
# scanout (the DCP is never touched).  The page needs the BGRA shadow.  Values must agree with
# the resident (CAPS2 bit 7, P_HV_WINDOWS_SCANOUT QUERY) and with Mu's J804Windows.h: the FD
# marker "J804SCN1" at 0x150 and the 32-byte /chosen/aura,windows-scanout-v1 handshake, without
# which Mu leaves the page and the SPI out of GPU0's resources.
CAPS2_SCANOUT = 0x0080                    # scanout v1
CONFIG_T8152_SCANOUT = 0x400000             # hv_windows_configure BIT(22)
SCANOUT_OP_QUERY, SCANOUT_OP_ATTACH, SCANOUT_OP_STATUS, SCANOUT_OP_DETACH = 0, 1, 2, 3
SCANOUT_Q = {"version": 0, "base": 1, "size": 2, "intid": 3, "features": 4, "refresh": 5}
SCANOUT_S = {"attached": 16, "ctrl": 17, "line": 18, "vblank_count": 19, "done_cookie": 20,
             "owns_panel": 21, "geometry": 22}
SCANOUT_STATS = {"vblanks": 0, "submitted": 1, "done": 2, "refused": 3, "busy": 4, "late": 5,
                 "rows": 6, "retired": 7, "bad_access": 8, "missed": 9, "stale_bands": 10}
SCANOUT_VERSION = 1
SCANOUT_BASE = 0xF60000000
SCANOUT_SIZE = 0x4000
SCANOUT_GSIV = 992
SCANOUT_FEATURES = 0x1F                   # timer vblank, copy flip, BGRA, A2R10G10B10, rect
SCANOUT_MIN_MHZ, SCANOUT_MAX_MHZ, SCANOUT_DEFAULT_MHZ = 1000, 240000, 60000
SCANOUT_FD_MARKER_OFFSET = 0x150
SCANOUT_FD_MARKER = b"J804SCN1"
# Mu J804Windows.h J804_SCANOUT_ADT_PROPERTY / _SIZE.
SCANOUT_HANDSHAKE_PROPERTY = "aura,windows-scanout-v1"
SCANOUT_HANDSHAKE_FORMAT = "<QIIIII4x"    # base, size, GSIV, features, refresh mHz, version
SCANOUT_HANDSHAKE_SIZE = 32
# The page's registers (scanout_model.h), for tools that read it from the host side.
SCANOUT_REGS = {"ID": 0x000, "VERSION_REG": 0x004, "FEATURES": 0x008, "CTRL": 0x00C,
                "STATUS": 0x010, "WIDTH": 0x014, "HEIGHT": 0x018, "PANEL_FORMAT": 0x01C,
                "REFRESH_MHZ": 0x020, "PERIOD_TICKS": 0x024, "SHADOW_LO": 0x028,
                "SHADOW_HI": 0x02C, "SHADOW_STRIDE": 0x030, "COUNTER_HZ": 0x034,
                "VBLANK_COUNT_LO": 0x038, "VBLANK_COUNT_HI": 0x03C, "VBLANK_TIME_LO": 0x040,
                "VBLANK_TIME_HI": 0x044, "FLIP_ADDR_LO": 0x080, "FLIP_ADDR_HI": 0x084,
                "FLIP_STRIDE": 0x088, "FLIP_FORMAT": 0x08C, "FLIP_RECT_TL": 0x090,
                "FLIP_RECT_BR": 0x094, "FLIP_SUBMIT": 0x098, "FLIP_ERROR": 0x09C,
                "PENDING_COOKIE": 0x0A0, "ACTIVE_COOKIE": 0x0A4, "DONE_COOKIE": 0x0A8,
                "ERROR_COOKIE": 0x0AC, "DONE_VBLANK_LO": 0x0B0, "DONE_VBLANK_HI": 0x0B4,
                "STATS": 0x100, "REG_END": 0x200}
SCANOUT_ERRORS = {-1: "not configured, not J804, the guest already started or an earlier ATTACH "
                      "this session", -2: "refresh outside 1..240 Hz",
                  -4: "the page could not be hooked"}


def parse_scanout_flag(value):
    """--windows-scanout: off, on (60 Hz), a rate in Hz (60, 120) or in mHz (59940).  Returns
    the refresh in mHz, 0 for off."""
    require(isinstance(value, str) and value, "--windows-scanout takes off, on, Hz or mHz")
    if value in ("off", "disabled"):
        return 0
    if value == "on":
        return SCANOUT_DEFAULT_MHZ
    require(value.isdigit(), f"--windows-scanout {value!r}: off, on, a rate in Hz or mHz")
    n = int(value)
    mhz = n * 1000 if n <= SCANOUT_MAX_MHZ // 1000 else n
    require(SCANOUT_MIN_MHZ <= mhz <= SCANOUT_MAX_MHZ and n,
            f"--windows-scanout {value}: the refresh must be 1..240 Hz")
    return mhz


def scanout_fd_marker_bytes():
    """The 16-byte FD marker a scanout build carries at 0x150 (Mu's FDF writes the same)."""
    return SCANOUT_FD_MARKER + struct.pack("<II", SCANOUT_BASE >> 12, SCANOUT_GSIV)


def fd_scanout_marker(fd):
    """(page base, GSIV) from a scanout FD's marker at 0x150, None for any other FD; a marker
    with other values is refused."""
    block = bytes(fd[SCANOUT_FD_MARKER_OFFSET:SCANOUT_FD_MARKER_OFFSET + 16])
    if len(block) != 16 or block[:8] != SCANOUT_FD_MARKER:
        return None
    page, gsiv = struct.unpack("<II", block[8:])
    require((page << 12, gsiv) == (SCANOUT_BASE, SCANOUT_GSIV),
            f"the FD's scanout marker {page << 12:#x}/{gsiv} differs from the carrier's "
            f"{SCANOUT_BASE:#x}/{SCANOUT_GSIV}")
    return page << 12, gsiv


def check_scanout_request(mhz, chip, board, caps2, fd_has_marker, fb_shadow):
    """Before anything is uploaded: --windows-scanout against the chip, the board, the
    resident's CAPS2 bit 7, the FD's marker (both directions) and the BGRA shadow."""
    if fd_has_marker:
        require(mhz, "this FD gives GPU0 the scanout page (marker J804SCN1 at 0x150) but "
                "--windows-scanout is off: Windows would touch an unhooked page. Pass "
                "--windows-scanout on or use an FD built without J804_WINDOWS_SCANOUT")
    if not mhz:
        return
    require(chip == T8152_CHIP, "--windows-scanout is a T8152 (J804) option")
    require(board == J804_BOARD_ID, f"--windows-scanout needs the J804 board (0x22), not "
            f"{board:#x}")
    require(isinstance(caps2, int) and caps2 >> 32 == CAPS2_MAGIC >> 32 and
            caps2 & CAPS2_SCANOUT, "the resident has no scanout service (P_HV_WINDOWS_CAPS2 "
            "bit 7): rebuild the carrier at or after the G6-S commit")
    require(fd_has_marker, "--windows-scanout: the FD has no J804SCN1 marker at 0x150, so GPU0 "
            "has no scanout resources: use a Mu FD built with WM6_J804_SCANOUT=TRUE")
    require(fb_shadow, "--windows-scanout copies flips into the panel next to the BGRA shadow: "
            "it needs --windows-fb-shadow (the T8152 default)")


def validate_scanout_contract(query):
    """The resident's QUERY answers must be this launcher's contract."""
    want = {"version": SCANOUT_VERSION, "base": SCANOUT_BASE, "size": SCANOUT_SIZE,
            "intid": SCANOUT_GSIV, "features": SCANOUT_FEATURES}
    for name, value in want.items():
        got = query(name)
        require(got == value, f"the resident's scanout {name} is {got:#x}, this launcher "
                f"expects {value:#x}")


def scanout_handshake(mhz):
    """/chosen/aura,windows-scanout-v1 for Mu's J804GpuAcpiDxe, written only after ATTACH
    returned 0: page base, size, GSIV, FEATURES, refresh in mHz, version 1, 4 bytes zero."""
    require(SCANOUT_MIN_MHZ <= mhz <= SCANOUT_MAX_MHZ, "no scanout handshake without a rate")
    value = struct.pack(SCANOUT_HANDSHAKE_FORMAT, SCANOUT_BASE, SCANOUT_SIZE, SCANOUT_GSIV,
                        SCANOUT_FEATURES, mhz, SCANOUT_VERSION)
    assert len(value) == SCANOUT_HANDSHAKE_SIZE
    return value


def describe_scanout_attach(result):
    """P_HV_WINDOWS_SCANOUT ATTACH's answer as text (0 is success)."""
    value = result - (1 << 64) if result >= 1 << 63 else result
    return "attached" if value == 0 else SCANOUT_ERRORS.get(value, f"error {value}")


def _aligned(value, alignment):
    return (value + alignment - 1) & -alignment


def predict_windows_layout(args, adt, heap_top, image_length):
    memory = adt["/chosen/memory-map"]
    _, sepfw_size = memory.SEPFW
    _, trust_size = memory.TrustCache
    preoslog_size = getattr(memory, "preoslog", (0, 0))[1]
    adt_base = heap_top + (16 << 20)
    trust_base = adt_base + _aligned(args.devtree_size, 0x4000)
    guest_base = _aligned(trust_base + _aligned(trust_size, 0x4000), 0x10000)
    bootargs_offset = _aligned(image_length, 0x4000) + _aligned(sepfw_size, 0x4000) + preoslog_size
    return {"heap_top": heap_top, "adt_base": adt_base, "trust_base": trust_base,
            "guest_base": guest_base, "bootargs": guest_base + bootargs_offset,
            "image_end": guest_base + bootargs_offset + 0x4000}


def validate_reserved_layout(guard, layout):
    require(layout == guard["layout"], "Windows memory layout changed after reservation preflight")
    for reservation in guard["reservations"]:
        start, end = reservation["base"], reservation["base"] + reservation["size"]
        require(layout["heap_top"] <= start < end <= guard["ram_end"],
                "reserved range is outside guest RAM: " + reservation["name"])
        for occupied in [*guard["occupied"],
                         {"name": "raw image and relocated metadata", "base": layout["heap_top"],
                          "size": layout["image_end"] - layout["heap_top"]}]:
            other_start, other_end = occupied["base"], occupied["base"] + occupied["size"]
            require(end <= other_start or other_end <= start,
                    "reserved range " + reservation["name"] + " overlaps " + occupied["name"])


def memory_reservation_preflight(plan, args, adt, high_water, resident_base,
                                 bootargs_address, bootargs_size, adt_address):
    ram_end = args.phys_base + args.mem_size
    require(not os.environ.get("AURAHEAP"), "reserved memory profiles cannot use AURAHEAP overrides")
    require(args.mem_size > 0 and ram_end <= 1 << 64 and
            args.phys_base <= resident_base <= high_water < ram_end,
            "resident or heap high-water mark is outside reported RAM")
    heap_base = _aligned(high_water, 64)
    proxy_base = heap_base + AURA_HEAP_RESERVE
    heap_top = proxy_base + PROXY_HEAP_SIZE
    layout = predict_windows_layout(args, adt, heap_top, len(plan.image))
    require(layout["image_end"] <= ram_end, "planned Windows image does not fit RAM")
    reservations = []
    for declaration in plan.reserved_memory:
        base = declaration.get("base", ram_end - declaration.get("from_ram_top", 0))
        reservations.append({"name": declaration["name"], "base": base, "size": declaration["size"]})
    for previous, current in zip(sorted(reservations, key=lambda r: r["base"]),
                                 sorted(reservations, key=lambda r: r["base"])[1:]):
        require(previous["base"] + previous["size"] <= current["base"],
                "reserved memory ranges overlap each other")
    occupied = [{"name": "Aura resident and C heap", "base": resident_base,
                 "size": proxy_base - resident_base},
                {"name": "proxy heap", "base": proxy_base, "size": PROXY_HEAP_SIZE},
                {"name": "source boot arguments", "base": bootargs_address, "size": bootargs_size},
                {"name": "source ADT", "base": adt_address, "size": args.devtree_size}]
    memory = adt["/chosen/memory-map"]
    for name in ("SEPFW", "TrustCache", "preoslog"):
        base, size = getattr(memory, name, (0, 0))
        if size:
            occupied.append({"name": "source " + name, "base": base, "size": size})
    if args.video.base and args.video.stride and args.video.height:
        occupied.append({"name": "framebuffer", "base": args.video.base,
                         "size": args.video.stride * args.video.height})
    for region in occupied:
        require(0 <= region["base"] < 1 << 64 and region["size"] > 0 and
                region["base"] + region["size"] <= 1 << 64, "invalid occupied memory span")
    guard = {"ram_end": ram_end, "heap_base": heap_base, "proxy_base": proxy_base,
             "reservations": reservations, "occupied": occupied, "layout": layout}
    validate_reserved_layout(guard, layout)
    return guard


def cpufreq_handoff_marker(policy, chip, board, model, opp_tables=None):
    """Transfer manual DVFS ownership only for the explicitly selected J813 ABI.

    This is not processor power policy or a frequency measurement. The Windows
    owner must bound changes to its validated boot-state/thermal limits.
    """
    require(policy in ("disabled", "j813-manual-v1"), "unsupported CPU frequency policy")
    if policy == "disabled":
        return None
    require((chip, board, model) == (0x8142, 0x24, "Mac17,3"),
            "CPU frequency ownership requires the exact J813 target")
    require(isinstance(opp_tables, (list, tuple)) and len(opp_tables) == 2,
            "CPU frequency handoff requires both firmware OPP tables")
    for table, expected in zip(opp_tables, J813_CPU_OPP_KHZ):
        _require_opp_table(table, expected)
    return struct.pack("<I", 1)


def unified_display_marker(selection, chip, board, model):
    """The /chosen marker that makes Mu publish the unified GPU+panel device, or None."""
    require(selection in ("disabled", "j813-v1"), "unsupported unified display selection")
    if selection == "disabled":
        return None
    require((chip, board, model) == (0x8142, 0x24, "Mac17,3"),
            "the unified display device requires the exact J813 target")
    return struct.pack("<I", 1)


def _require_opp_table(table, expected):
    require(isinstance(table, (list, tuple)) and len(table) == 2 * len(expected)
            and all(type(word) is int and 0 <= word <= 0xffffffff for word in table),
            "malformed CPU frequency OPP table")
    require(tuple(table[::2]) == expected, "firmware CPU OPP mapping differs from J813-v1")


def ecpu_boot_state_khz(state, chip, board, model, e_table):
    """kHz the hypervisor must find for the explicit E boot state, from the J813 E ladder.

    The C side re-reads the same ADT entry and refuses on any mismatch.
    """
    require(type(state) is int and ECPU_HANDOFF_STATE <= state <= ECPU_LAST_STATE,
            "unsupported E boot state")
    require((chip, board, model) == (0x8142, 0x24, "Mac17,3"),
            "the E boot state requires the exact J813 target")
    _require_opp_table(e_table, J813_CPU_OPP_KHZ[0])
    return J813_CPU_OPP_KHZ[0][state - ECPU_HANDOFF_STATE]


def validate_n1_descriptor(descriptor):
    require(isinstance(descriptor, bytes) and len(descriptor) == N1_BYTES,
            "N1 descriptor must contain exactly 128 bytes")
    header = struct.unpack_from("<IHHIIII4H", descriptor)
    require(header == (0x4831574e, 1, N1_BYTES, 7, 0x8142, 0x24, 0x32354776,
                       0x100, 0x101, 0x102, 0), "N1 descriptor ABI or target identity mismatch")
    resources = tuple(struct.unpack_from("<QQ", descriptor, 32 + i * 16) for i in range(6))
    require(resources == N1_RESOURCES, "N1 descriptor resource mismatch")
    return {"profile": "j813-v1", "sha256": hashlib.sha256(descriptor).hexdigest(),
            "descriptor_hex": descriptor.hex(),
            "resources": [{"base": hex(base), "length": hex(size)} for base, size in resources]}


def prepare_n1_handoff(hv):
    if hv.windows_n1 == "disabled":
        return None
    require(hv.windows_n1 in ("j813-v1", "j813-acquire-v1") and hv.windows_initialized,
            "N1 handoff requires successful Windows HV initialization")
    require(not hv.windows_n1_attempted, "N1 handoff cannot be retried; load a fresh Aura resident")
    hv.windows_n1_attempted = True
    address = hv.u.memalign(16, N1_BYTES)
    try:
        hv.iface.writemem(address, bytes(N1_BYTES))
        producer = hv.p.hv_windows_acquire_n1 if hv.windows_n1 == "j813-acquire-v1" else hv.p.hv_windows_prepare_n1
        result = producer(address, N1_BYTES)
        require(result == 0, f"N1 handoff producer failed: {result}; load a fresh Aura resident")
        descriptor = hv.iface.readmem(address, N1_BYTES)
        receipt = validate_n1_descriptor(descriptor)
        receipt["acquisition"] = hv.windows_n1
        hv.adt["/chosen"]._properties[N1_PROPERTY] = descriptor
        hv._windows_adt_blob()
        hv.windows_n1_handoff = receipt
        hv.windows_n1_ready = True
        return receipt
    finally:
        hv.u.free(address)


def source_manifest():
    root = Path(__file__).resolve().parents[1]
    names = ("aura/windows.py", "aura/windows_serial.py", "aura/windows_cdc.py",
             "aura/hv/__init__.py", "aura/hv/xfer.py", "aura/proxy.py", "tools/boot_windows.py")
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}


class _GuardedHeapProxy:
    """Keep allocator drift from installing an unvalidated heap limit."""
    def __init__(self, proxy, guard):
        object.__setattr__(self, "_proxy", proxy)
        object.__setattr__(self, "_guard", guard)

    def __getattr__(self, name):
        return getattr(self._proxy, name)

    def __setattr__(self, name, value):
        setattr(self._proxy, name, value)

    def heapblock_alloc(self, size):
        require(size == 0, "unexpected allocation during reservation admission")
        # Re-query before even the allocator's alignment update.
        require(_aligned(self._proxy.hv_windows_heap_high_water(), 64) == self._guard["heap_base"],
                "resident heap changed after memory preflight")
        result = self._proxy.heapblock_alloc(size)
        require(result == self._guard["heap_base"], "resident allocator disagrees with memory preflight")
        return result

    def heapblock_set_limit(self, limit):
        require(limit == self._guard["proxy_base"], "refusing unvalidated heap limit")
        return self._proxy.heapblock_set_limit(limit)


class HardwareSession:
    """Transport boundary. Identity/capability methods are read-only."""
    def __init__(self, device, log):
        from .proxy import AuraProxy, UartInterface
        self.iface = UartInterface(device)
        self.iface.tty_log = log
        try:
            # Features belong to the resident, not to this serial handle.
            # Reopen can follow a CDC session that enabled checksum sentinels.
            self.iface.nop()
        except BaseException:
            self.iface.dev.close()
            raise
        self.proxy = AuraProxy(self.iface, debug=False)
        self.hv = None
        self.serial_capture = None
        self.xfer_receipt = None

    def capabilities(self):
        return self.proxy.hv_windows_capabilities()

    def capabilities2(self):
        return self.proxy.hv_windows_capabilities2()

    def validate_serial_transport(self, plan):
        if plan.serial != "cdc":
            return None
        from .proxy import IODEV
        iodev = self.proxy.iodev_whoami()
        require(int(IODEV.USB0) <= iodev < int(IODEV.USB0) + 8,
                "dedicated CDC logging requires a USB CDC proxy carrier")
        return {"proxy_iodev": int(iodev)}

    def identity(self):
        from .adt import load_adt
        from .tgtypes import BootArgs_r1, BootArgs_r2, BootArgs_r3
        address, revision = self.proxy.get_bootargs_rev()
        require(revision in (1, 2, 3), "unsupported resident boot-arguments revision")
        layout = {1: BootArgs_r1, 2: BootArgs_r2, 3: BootArgs_r3}[revision]
        args = layout.parse(self.iface.readmem(address, layout.sizeof()))
        require(0 < args.devtree_size <= 4 * 1024 * 1024, "resident ADT exceeds bounds")
        physical = (args.devtree - args.virt_base + args.phys_base) & ((1 << 64) - 1)
        ram_end = args.phys_base + args.mem_size
        require(args.mem_size > 0 and ram_end <= 1 << 64 and
                args.phys_base <= physical and physical + args.devtree_size <= ram_end,
                "resident ADT physical span is outside reported RAM")
        self._identity_bootargs = args
        self._identity_bootargs_address = address
        self._identity_bootargs_size = layout.sizeof()
        self._identity_adt_physical = physical
        self._identity_adt_blob = self.iface.readmem(physical, args.devtree_size)
        adt = load_adt(self._identity_adt_blob)
        self._identity_resident_base = self.proxy.get_base()
        return {"chip_id": adt["/chosen"].chip_id, "model": adt.model,
                "resident_base": hex(self._identity_resident_base), "bootargs_revision": revision,
                "framebuffer_depth": args.video.depth & 255}

    def prepare(self, plan):
        validate_capabilities(self.capabilities(), plan.framebuffer, plan.guest_compatibility, plan.n1,
                              bool(plan.reserved_memory), plan.serial, plan.mtp, plan.cpufreq,
                              xfer=plan.xfer is not None, ecpu_boot_state=plan.ecpu_boot_state,
                              t8152=plan.chip_id == T8152_CHIP, ipi=getattr(plan, "ipi", "poll"),
                              gic_cpuif=getattr(plan, "gic_cpuif", "carrier"),
                              caps2=self.capabilities2() if plan.chip_id == T8152_CHIP else None)
        self.validate_serial_transport(plan)
        if plan.serial == "capture":
            from .windows_serial import WindowsSerialRecorder
            self.serial_capture = WindowsSerialRecorder(Path(self.iface.tty_log.name).parent)
        from .adt import load_adt
        from .hv import HV
        from .proxyutils import ProxyUtils, bootstrap_port
        # Run the actual host-only ADT transformations before ProxyUtils can
        # reserve memory or bootstrap_port can change the resident baud rate.
        preflight = HV(self.iface, self.proxy, SimpleNamespace(ba=self._identity_bootargs), windows=True)
        preflight.adt = load_adt(self._identity_adt_blob)
        require(preflight.adt["/chosen"].chip_id == plan.chip_id and preflight.adt.model == plan.model,
                "target identity does not match the Windows profile")
        preflight.iodev = self.proxy.iodev_whoami()
        preflight.emulate_sprr = False
        preflight.windows_usb = plan.usb
        preflight.windows_n1 = plan.n1
        preflight.windows_mtp = plan.mtp
        preflight.windows_cpufreq = plan.cpufreq
        preflight.windows_unified_display = plan.unified_display
        guest_ram = getattr(plan, "guest_ram", None)
        preflight.windows_guest_ram = guest_ram
        preflight.windows_ipi = getattr(plan, "ipi", "poll")
        preflight.windows_gic_cpuif = getattr(plan, "gic_cpuif", "carrier")
        preflight.windows_wfi = getattr(plan, "wfi", "poll")
        preflight.setup_adt()
        if guest_ram is not None:
            args = self._identity_bootargs
            occupied = [("aura resident and heaps", self._identity_resident_base,
                         self.proxy.hv_windows_heap_high_water() + AURA_HEAP_RESERVE +
                         PROXY_HEAP_SIZE - self._identity_resident_base),
                        ("live ADT", self._identity_adt_physical, args.devtree_size)]
            if args.video.base and args.video.stride and args.video.height:
                occupied.append(("scanout", args.video.base, args.video.stride * args.video.height))
            t8152_check_window(preflight.adt, args, *guest_ram, occupied)
        preflight._windows_adt_blob()
        if plan.ecpu_boot_state:
            # Bind the E ladder before any target mutation; HV.init() binds it again.
            chosen = preflight.adt["/chosen"]
            ecpu_boot_state_khz(plan.ecpu_boot_state, chosen.chip_id, getattr(chosen, "board_id", None),
                                preflight.adt.model,
                                preflight.adt["/arm-io/pmgr"].getprop("voltage-states1-sram"))
        guard = None
        if plan.reserved_memory:
            guard = memory_reservation_preflight(
                plan, self._identity_bootargs, preflight.adt, self.proxy.hv_windows_heap_high_water(),
                self._identity_resident_base, self._identity_bootargs_address,
                self._identity_bootargs_size, self._identity_adt_physical)
        bootstrap_port(self.iface, self.proxy)
        allocator_proxy = _GuardedHeapProxy(self.proxy, guard) if guard is not None else self.proxy
        utils = ProxyUtils(allocator_proxy, heap_size=PROXY_HEAP_SIZE, aura_heap=AURA_HEAP_RESERVE)
        utils.proxy = self.proxy
        if guard is not None:
            require(utils.heap_base == guard["proxy_base"] and utils.heap_top == guard["layout"]["heap_top"],
                    "proxy heap disagrees with memory preflight")
        self.hv = HV(self.iface, self.proxy, utils, windows=True)
        self.hv.windows_memory_guard = guard
        self.hv.windows_framebuffer = plan.framebuffer
        self.hv.windows_guest_compatibility = plan.guest_compatibility
        self.hv.windows_usb = plan.usb
        self.hv.windows_n1 = plan.n1
        self.hv.windows_mtp = plan.mtp
        self.hv.windows_cpufreq = plan.cpufreq
        self.hv.windows_ecpu_boot_state = plan.ecpu_boot_state
        self.hv.windows_unified_display = plan.unified_display
        self.hv.windows_serial = plan.serial
        self.hv.windows_serial_recorder = self.serial_capture
        self.hv.windows_xfer = plan.xfer is not None
        self.hv.windows_guest_ram = guest_ram
        self.hv.windows_ipi = getattr(plan, "ipi", "poll")
        self.hv.windows_gic_cpuif = getattr(plan, "gic_cpuif", "carrier")
        self.hv.windows_wfi = getattr(plan, "wfi", "poll")
        self.hv.init()
        self.hv.set_logfile(self.iface.tty_log)
        prepare_n1_handoff(self.hv)
        if plan.xfer is not None:
            # Before load/start: start()'s pt_update() must see the RESERVED tracers.
            self.xfer_receipt = attach_windows_xfer(self.hv, plan.xfer,
                                                    Path(self.iface.tty_log.name).parent)

    def xfer_summary(self):
        """The attached channel's receipt plus engine statistics, or None."""
        if self.xfer_receipt is None:
            return None
        summary = dict(self.xfer_receipt)
        if self.hv is not None and self.hv.xfer_dev is not None:
            summary["engine"] = self.hv.xfer_dev.summary()
        return summary

    def load(self, plan):
        if plan.boot_args:
            self.hv.set_bootargs(plan.boot_args)
        self.hv.load_raw(plan.image, plan.entry_offset)
        require(self.hv.guest_base % 0x10000 == 0, "Mu payload base is not 64-KiB aligned")
        cpufreq = {"profile": plan.cpufreq, "automatic_policy": False}
        if plan.ecpu_boot_state:
            cpufreq["ecpu_boot_state"] = plan.ecpu_boot_state  # applied by hv_start, not yet
        record = {"guest_base": hex(self.hv.guest_base), "entry": hex(self.hv.entry),
                  "bootargs": hex(self.hv.guest_base + self.hv.bootargs_off),
                  "usb": plan.usb, "usb_removed": self.hv.windows_usb_removed,
                  "n1": self.hv.windows_n1_handoff, "memory_preflight": self.hv.windows_memory_guard,
                  "mtp": {"profile": plan.mtp, "prepared": self.hv.windows_mtp_ready},
                  "cpufreq": cpufreq}
        if plan.unified_display != "disabled":
            record["unified_display"] = plan.unified_display
        return record

    def start(self):
        return self.hv.start()

    def system_reset(self):
        windows_system_reset(self.proxy)

    def close(self):
        try:
            if self.serial_capture is not None:
                self.serial_capture.close()
            if self.hv is not None and self.hv.xfer_dev is not None:
                self.hv.xfer_dev.close()  # abandons open PUTs (their .part files stay)
        finally:
            self.iface.dev.close()

    def serial_summary(self):
        return self.serial_capture.summary() if self.serial_capture is not None else {"mode": "disabled"}


def _record_xfer(report, session, plan):
    """Keep the planned xfer config in the receipt; overlay the live channel once attached."""
    summary = session.xfer_summary() if hasattr(session, "xfer_summary") else None
    if summary is not None:
        report["xfer"] = dict(summary, attached=True)
    elif plan.xfer is not None:
        report["xfer"] = dict(xfer_plan_summary(plan.xfer), attached=False)


def launch_profile(filename, device, output, session_factory=HardwareSession, guest_serial_device=None):
    """Launch exactly the validated immutable image and preserve an honest receipt."""
    plan = plan_profile(filename)
    if plan.serial == "cdc":
        require(isinstance(guest_serial_device, str) and guest_serial_device,
                "CDC logging requires an explicit guest serial device")
        require(os.path.realpath(device) != os.path.realpath(guest_serial_device),
                "guest serial device must differ from the proxy device")
    else:
        require(guest_serial_device is None, "guest serial device requires serial=cdc in the profile")
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "profile.json").write_bytes(plan.profile_bytes)
    report = {"schema": "aura.windows.launch.v1", "result": "preflight",
              "profile_sha256": plan.profile_sha256, "images": plan.images,
              "host_sources": source_manifest(), "device": device,
              "combined_sha256": hashlib.sha256(plan.image).hexdigest(),
              "hardware_qualified": False, "target_mutated": False, "windows_ready": False,
              "serial": {"mode": plan.serial, "device": guest_serial_device},
              "xfer": xfer_plan_summary(plan.xfer)}
    session = None
    cdc_recorder = None
    try:
        with (output / "console.log").open("w") as log:
            session = session_factory(device, log)
            caps = session.capabilities()
            validate_capabilities(caps, plan.framebuffer, plan.guest_compatibility, plan.n1,
                                  bool(plan.reserved_memory), plan.serial, plan.mtp, plan.cpufreq,
                                  xfer=plan.xfer is not None,
                                  ecpu_boot_state=plan.ecpu_boot_state,
                                  t8152=plan.chip_id == T8152_CHIP,
                                  ipi=getattr(plan, "ipi", "poll"),
                                  gic_cpuif=getattr(plan, "gic_cpuif", "carrier"),
                                  caps2=(session.capabilities2()
                                         if plan.chip_id == T8152_CHIP and
                                         hasattr(session, "capabilities2") else None))
            identity = session.identity()
            require(identity["chip_id"] == plan.chip_id and identity["model"] == plan.model,
                    "target identity does not match the Windows profile")
            report.update(capabilities=hex(caps), identity=identity)
            if plan.serial == "cdc":
                from .windows_cdc import CdcSerialRecorder
                report["serial_transport"] = session.validate_serial_transport(plan)
                cdc_recorder = CdcSerialRecorder(output, guest_serial_device)
                cdc_recorder.wait_ready()
            report["target_mutated"] = True
            session.prepare(plan)
            _record_xfer(report, session, plan)
            report["loaded"] = session.load(plan)
            if cdc_recorder is not None:
                cdc_recorder.check()
                report["serial"] = cdc_recorder.summary()
            elif hasattr(session, "serial_summary"):
                report["serial"] = session.serial_summary()
            report["result"] = "guest-running"
            (output / "launch.json").write_text(json.dumps(report, indent=2) + "\n")
            result = session.start()
            if cdc_recorder is not None:
                cdc_recorder.check()
            require(result in (None, 0, 1, 2), "Windows guest returned an error")
            report["guest_exit_status"] = result
            report["result"] = {1: "guest-powered-off-unqualified",
                                2: "guest-reset-requested-unqualified"}.get(
                                    result, "guest-returned-unqualified")
            if result == 2 and getattr(plan, "on_reset", "stay") == "reboot":
                # The guest ran with no NVMe of its own (T8152 denylist), so this is
                # aura's session only; the resident refuses the reset if shutdown fails.
                report["result"] = "guest-reset-nvme-shutdown-then-reboot"
                (output / "launch.json").write_text(json.dumps(report, indent=2) + "\n")
                session.system_reset()
            return report
    except BaseException as error:
        report.update(result="failed", error=type(error).__name__ + ": " + str(error))
        if getattr(session, "serial_capture", None) is not None:
            session.serial_capture.incomplete("launch-or-transport-error")
        raise
    finally:
        if session is not None:
            try:
                _record_xfer(report, session, plan)  # final per-stream bytes and SHA-256
            except Exception as error:  # noqa: BLE001 - the receipt must still be written
                report["xfer_summary_error"] = repr(error)
        try:
            try:
                if cdc_recorder is not None:
                    cdc_recorder.close()
            finally:
                if session is not None:
                    session.close()
        finally:
            if cdc_recorder is not None:
                report["serial"] = cdc_recorder.summary()
            elif plan.serial != "cdc" and session is not None and hasattr(session, "serial_summary"):
                report["serial"] = session.serial_summary()
            (output / "launch.json").write_text(json.dumps(report, indent=2) + "\n")
