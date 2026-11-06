# SPDX-License-Identifier: MIT
"""Explicit, hash-bound Aura -> Mu Windows launch admission.

Importing this module never opens a device or loads the allocating proxy helpers.
The ordinary Aura guest path is not selected by this launcher.
"""
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
from types import SimpleNamespace

CAPS_MAGIC = 0x4157494E00000000
CAPS_ABI = 1
REQUIRED_CAPS = 0x000F
FRAMEBUFFER_SHADOW = 0x0010
N1_HANDOFF = 0x0020
CONFIGURATION = 0x0040
CONFIG_T8142_EXPERIMENTAL = 0x0001
CONFIG_FRAMEBUFFER_SHADOW = 0x0002
MAX_IMAGE = 1024 * 1024 * 1024
N1_PROPERTY = "aura,windows-n1-v1"
N1_BYTES = 128
N1_RESOURCES = ((0x1cb0000000, 0x200000), (0x488000000, 0x20000),
                (0x488028000, 0x4000), (0x48f010000, 0x1000),
                (0x39a000300, 4), (0xb80000000, 0x600000))


def require(condition, message):
    if not condition:
        raise ValueError(message)


def number(value, label):
    require(isinstance(value, (str, int)) and not isinstance(value, bool), label + " must be an integer")
    try:
        return int(value, 0) if isinstance(value, str) else value
    except ValueError as error:
        raise ValueError(label + " must be an integer") from error


def validate_capabilities(value, framebuffer="native", guest_compatibility="none", n1="disabled"):
    require(isinstance(value, int) and value >> 32 == CAPS_MAGIC >> 32,
            "resident is not an explicit Windows Aura personality")
    require((value >> 16) & 0xffff == CAPS_ABI, "unsupported Windows Aura capability ABI")
    require(value & REQUIRED_CAPS == REQUIRED_CAPS, "resident lacks required Windows capabilities")
    if framebuffer == "bgra-shadow":
        require(value & FRAMEBUFFER_SHADOW, "resident lacks the framebuffer conversion backend")
    if framebuffer != "native" or guest_compatibility != "none":
        require(value & CONFIGURATION, "resident lacks explicit Windows configuration")
    if n1 != "disabled":
        require(value & N1_HANDOFF, "resident lacks the N1 handoff producer")
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
    boot_args: str


def plan_profile(filename):
    filename = Path(filename).resolve()
    with filename.open("rb") as stream:
        raw = stream.read(65537)
    require(len(raw) <= 65536, "profile is too large")
    profile = json.loads(raw)
    require(isinstance(profile, dict), "profile must be an object")
    require(profile.get("schema") == "aura.windows.boot.v1", "unsupported Windows boot profile")
    require(set(profile) <= {"schema", "name", "target", "mu", "append", "framebuffer", "boot_args",
                             "guest_compatibility", "usb", "n1"},
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
    require(compatibility in ("none", "t8142-experimental"), "unsupported guest compatibility policy")
    require(compatibility == "none" or chip == 0x8142,
            "t8142-experimental compatibility requires chip 0x8142")
    usb = profile.get("usb", "disabled")
    require(usb == "disabled", "USB handoff is not implemented; usb must be disabled")
    n1 = profile.get("n1", "disabled")
    require(n1 in ("disabled", "j813-v1"), "unsupported N1 handoff profile")
    require(n1 == "disabled" or (chip == 0x8142 and model == "Mac17,3"),
            "j813-v1 N1 handoff requires chip 0x8142 and model Mac17,3")
    boot_args = profile.get("boot_args", "")
    require(isinstance(boot_args, str) and len(boot_args.encode("ascii")) <= 255 and "\0" not in boot_args,
            "boot_args must be at most 255 ASCII bytes without NUL")
    # Hash every source file before introducing any padding. Offsets and the
    # final digest bind the zero-filled layout separately from source hashes.
    composed, cursor = [], 0
    for offset, content in parts:
        if offset != cursor:
            composed.append(bytes(offset - cursor))
        composed.append(content)
        cursor = offset + len(content)
    return LaunchPlan(filename, profile, raw, hashlib.sha256(raw).hexdigest(), b"".join(composed),
                      tuple(images), entry, chip, model, framebuffer, compatibility, usb, n1, boot_args)


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
    require(hv.windows_n1 == "j813-v1" and hv.windows_initialized,
            "N1 handoff requires successful Windows HV initialization")
    require(not hv.windows_n1_attempted, "N1 handoff cannot be retried; load a fresh Aura resident")
    hv.windows_n1_attempted = True
    address = hv.u.memalign(16, N1_BYTES)
    try:
        hv.iface.writemem(address, bytes(N1_BYTES))
        result = hv.p.hv_windows_prepare_n1(address, N1_BYTES)
        require(result == 0, f"N1 handoff producer failed: {result}; load a fresh Aura resident")
        descriptor = hv.iface.readmem(address, N1_BYTES)
        receipt = validate_n1_descriptor(descriptor)
        hv.adt["/chosen"]._properties[N1_PROPERTY] = descriptor
        hv._windows_adt_blob()
        hv.windows_n1_handoff = receipt
        hv.windows_n1_ready = True
        return receipt
    finally:
        hv.u.free(address)


def source_manifest():
    root = Path(__file__).resolve().parents[1]
    names = ("aura/windows.py", "aura/hv/__init__.py", "aura/proxy.py", "tools/boot_windows.py")
    return {name: hashlib.sha256((root / name).read_bytes()).hexdigest() for name in names}


class HardwareSession:
    """Transport boundary. Identity/capability methods are read-only."""
    def __init__(self, device, log):
        from .proxy import AuraProxy, UartInterface
        self.iface = UartInterface(device)
        self.iface.tty_log = log
        self.proxy = AuraProxy(self.iface, debug=False)
        self.hv = None

    def capabilities(self):
        return self.proxy.hv_windows_capabilities()

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
        self._identity_adt_blob = self.iface.readmem(physical, args.devtree_size)
        adt = load_adt(self._identity_adt_blob)
        return {"chip_id": adt["/chosen"].chip_id, "model": adt.model,
                "resident_base": hex(self.proxy.get_base()), "bootargs_revision": revision,
                "framebuffer_depth": args.video.depth & 255}

    def prepare(self, plan):
        validate_capabilities(self.capabilities(), plan.framebuffer, plan.guest_compatibility, plan.n1)
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
        preflight.setup_adt()
        preflight._windows_adt_blob()
        bootstrap_port(self.iface, self.proxy)
        utils = ProxyUtils(self.proxy, heap_size=128 * 1024 * 1024, aura_heap=128 * 1024 * 1024)
        self.hv = HV(self.iface, self.proxy, utils, windows=True)
        self.hv.windows_framebuffer = plan.framebuffer
        self.hv.windows_guest_compatibility = plan.guest_compatibility
        self.hv.windows_usb = plan.usb
        self.hv.windows_n1 = plan.n1
        self.hv.init()
        self.hv.set_logfile(self.iface.tty_log)
        prepare_n1_handoff(self.hv)

    def load(self, plan):
        if plan.boot_args:
            self.hv.set_bootargs(plan.boot_args)
        self.hv.load_raw(plan.image, plan.entry_offset)
        require(self.hv.guest_base % 0x10000 == 0, "Mu payload base is not 64-KiB aligned")
        return {"guest_base": hex(self.hv.guest_base), "entry": hex(self.hv.entry),
                "bootargs": hex(self.hv.guest_base + self.hv.bootargs_off),
                "usb": plan.usb, "usb_removed": self.hv.windows_usb_removed,
                "n1": self.hv.windows_n1_handoff}

    def start(self):
        return self.hv.start()

    def close(self):
        self.iface.dev.close()


def launch_profile(filename, device, output, session_factory=HardwareSession):
    """Launch exactly the validated immutable image and preserve an honest receipt."""
    plan = plan_profile(filename)
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=False)
    (output / "profile.json").write_bytes(plan.profile_bytes)
    report = {"schema": "aura.windows.launch.v1", "result": "preflight",
              "profile_sha256": plan.profile_sha256, "images": plan.images,
              "host_sources": source_manifest(), "device": device,
              "combined_sha256": hashlib.sha256(plan.image).hexdigest(),
              "hardware_qualified": False, "target_mutated": False, "windows_ready": False}
    session = None
    try:
        with (output / "console.log").open("w") as log:
            session = session_factory(device, log)
            caps = session.capabilities()
            validate_capabilities(caps, plan.framebuffer, plan.guest_compatibility, plan.n1)
            identity = session.identity()
            require(identity["chip_id"] == plan.chip_id and identity["model"] == plan.model,
                    "target identity does not match the Windows profile")
            report.update(capabilities=hex(caps), identity=identity)
            report["target_mutated"] = True
            session.prepare(plan)
            report["loaded"] = session.load(plan)
            report["result"] = "guest-running"
            (output / "launch.json").write_text(json.dumps(report, indent=2) + "\n")
            result = session.start()
            require(result in (None, 0, 1, 2), "Windows guest returned an error")
            report["guest_exit_status"] = result
            report["result"] = {1: "guest-powered-off-unqualified",
                                2: "guest-reset-requested-unqualified"}.get(
                                    result, "guest-returned-unqualified")
            return report
    except BaseException as error:
        report.update(result="failed", error=type(error).__name__ + ": " + str(error))
        raise
    finally:
        try:
            if session is not None:
                session.close()
        finally:
            (output / "launch.json").write_text(json.dumps(report, indent=2) + "\n")
