# SPDX-License-Identifier: MIT
"""Parse an Apple ExclaveCore BUND ("DNUB") bundle and derive its cL4 regions.

The bundle is a plain container - signed via its IM4P wrapper, but not
encrypted - holding several Mach-O images plus a DER configuration describing
them. iBoot, not SPTM, parses this and hands SPTM pre-laid-out regions, so a
hosted implementation has to do this part itself.

The region rules follow the description of the bundle format in
ADevWithAnIdea/apple-silicon-re, docs/sptm/exclaves.md.
"""

import struct
from dataclasses import dataclass, field

BUND_MAGIC = b"DNUB"          # "BUND" as stored
PAGE = 0x4000

# DER tags we care about
_SEQUENCE = 0x30
_INTEGER = 0x02
_PRINTABLE = 0x13
_UTF8 = 0x0C
# 25G83's bundle encodes the configuration's keys and string values as PrintableString, 26A428's
# as UTF8String.
_STRINGS = (_PRINTABLE, _UTF8)

MACHO_KEYS = (
    "__MACHO__TEXTOFF", "__MACHO__TEXTSZ",
    "__MACHO__DATAOFF", "__MACHO__DATASZ",
    "__MACHO__LINKEDITOFF", "__MACHO__LINKEDITSZ",
    "__MACHO__PDATAOFF", "__MACHO__PDATASZ",
)


class BundleError(Exception):
    pass


def im4p_payload(data):
    """Return the OCTET STRING payload of an IM4P container."""
    def der_len(buf, i):
        if i >= len(buf):
            raise BundleError("truncated DER length")
        n = buf[i]
        i += 1
        if n < 0x80:
            return n, i
        k = n & 0x7F
        if not k or i + k > len(buf):
            raise BundleError("unsupported or truncated DER length")
        return int.from_bytes(buf[i:i + k], "big"), i + k

    if data[:4] == BUND_MAGIC or data[8:12] == BUND_MAGIC:
        return data                     # already unwrapped
    if data[0] != _SEQUENCE:
        raise BundleError("not a DER SEQUENCE and not a bare bundle")
    _, i = der_len(data, 1)
    while i < len(data):
        tag = data[i]
        i += 1
        ln, i = der_len(data, i)
        if tag == 0x04 and ln > 1024:
            return data[i:i + ln]
        i += ln
    raise BundleError("no IM4P payload found")


@dataclass
class Component:
    """One embedded Mach-O, as described by the bundle's DER configuration."""
    name: str = ""
    kind: str = ""
    text: tuple = (0, 0)
    data: tuple = (0, 0)
    linkedit: tuple = (0, 0)
    pdata: tuple = (0, 0)

    def tail(self):
        """Highest offset covered by this component's LinkEdit or PDATA."""
        return max(self.linkedit[0] + self.linkedit[1],
                   self.pdata[0] + self.pdata[1])


@dataclass
class Bundle:
    payload: bytes
    components: list = field(default_factory=list)
    regions: dict = field(default_factory=dict)


def _der_pairs(buf):
    """Yield (key, value) for a DER string key followed by an INTEGER or string value.

    The configuration encodes each attribute as SEQUENCE { PrintableString key,
    INTEGER|PrintableString value }. Walking the whole payload for that shape is
    more robust than assuming where the ``urst`` range begins, and the keys are
    distinctive enough that false positives do not occur in practice.
    """
    wanted = set(MACHO_KEYS) | {"__COMPONENTNAME", "__COMPONENTTYPE"}
    i = 0
    end = len(buf)
    while i < end - 3:
        if buf[i] not in _STRINGS:
            i += 1
            continue
        klen = buf[i + 1]
        if klen == 0 or klen > 0x30 or i + 2 + klen >= end:
            i += 1
            continue
        try:
            key = buf[i + 2:i + 2 + klen].decode("ascii")
        except UnicodeDecodeError:
            i += 1
            continue
        if key not in wanted:
            i += 1
            continue
        j = i + 2 + klen
        if j + 2 > end:
            break
        tag = buf[j]
        vlen = buf[j + 1]
        if vlen >= 0x80 or j + 2 + vlen > end:
            raise BundleError(f"{key}: value length not supported")
        raw = buf[j + 2:j + 2 + vlen]
        if tag == _INTEGER:
            yield key, int.from_bytes(raw, "big")
        elif tag in _STRINGS:
            yield key, raw.decode("ascii", "replace")
        i = j + 2 + vlen


def parse(data):
    """Parse an IM4P-wrapped or bare ExclaveCore bundle."""
    payload = im4p_payload(data)
    if payload[8:12] != BUND_MAGIC:
        raise BundleError(f"bad BUND magic: {payload[8:12]!r}")

    comps, cur = [], None
    for key, val in _der_pairs(payload):
        if key == "__COMPONENTNAME":
            if cur is not None:
                comps.append(cur)
            cur = Component(name=str(val))
            continue
        if cur is None:
            cur = Component()
        if key == "__COMPONENTTYPE":
            cur.kind = str(val)
        elif key == "__MACHO__TEXTOFF":
            cur.text = (val, cur.text[1])
        elif key == "__MACHO__TEXTSZ":
            cur.text = (cur.text[0], val)
        elif key == "__MACHO__DATAOFF":
            cur.data = (val, cur.data[1])
        elif key == "__MACHO__DATASZ":
            cur.data = (cur.data[0], val)
        elif key == "__MACHO__LINKEDITOFF":
            cur.linkedit = (val, cur.linkedit[1])
        elif key == "__MACHO__LINKEDITSZ":
            cur.linkedit = (cur.linkedit[0], val)
        elif key == "__MACHO__PDATAOFF":
            cur.pdata = (val, cur.pdata[1])
        elif key == "__MACHO__PDATASZ":
            cur.pdata = (cur.pdata[0], val)
    if cur is not None:
        comps.append(cur)

    if not comps:
        raise BundleError("no components found in bundle configuration")

    bundle = Bundle(payload=payload, components=comps)
    bundle.regions = derive_regions(comps, len(payload))
    return bundle


def derive_regions(components, payload_size):
    """Derive the four cL4 protection groups as BUND-relative (start, size).

    Content is packed executable first, then writable data, then LinkEdit and
    protected data, and finally the read-only Mach-O headers and the bundle
    configuration itself.
    """
    withdata = [c for c in components if c.data[0]]
    withle = [c for c in components if c.linkedit[0]]
    if not withdata or not withle:
        raise BundleError("components lack DATA/LINKEDIT offsets")

    rx_end = min(c.data[0] for c in withdata)
    rw_end = min(c.linkedit[0] for c in withle)
    le_end = max(c.tail() for c in components)
    ro_end = (payload_size + PAGE - 1) & ~(PAGE - 1)

    if not 0 < rx_end <= rw_end <= le_end <= ro_end:
        raise BundleError(
            f"implausible region ordering: rx_end={rx_end:#x} rw_end={rw_end:#x} "
            f"le_end={le_end:#x} ro_end={ro_end:#x}")

    regions = {
        "CL4-rx": (0, rx_end),
        "CL4-rw": (rx_end, rw_end - rx_end),
        "CL4-le": (rw_end, le_end - rw_end),
        "CL4-ro": (le_end, ro_end - le_end),
    }

    # The four groups must tile the payload exactly; a gap or overlap means the
    # derivation is wrong and the resulting load would be silently corrupt.
    total = sum(size for _, size in regions.values())
    if total != ro_end:
        raise BundleError(f"regions total {total:#x}, expected {ro_end:#x}")
    for name, (start, size) in regions.items():
        if size == 0:
            raise BundleError(f"{name} is empty")
        if start % PAGE or size % PAGE:
            raise BundleError(f"{name} is not page aligned: {start:#x}+{size:#x}")

    return regions
