# SPDX-License-Identifier: MIT
"""Which PMGR power-state registers say whether a DART is powered (used by tools/dump_guest_mem.py; pure, no device).

A DART's registers may be touched only while its power domain is on: on T8142 a read of a powered-off block makes
the proxy fault in a loop. Many ADT clock-gates ids are virtual PMGR devices ("DCP0-V", "ANE-SYS-V":
flag no_ps, no register); their state is that of their nearest real ancestors. Some virtual devices have no parents
at all (DISP-MIE-DPB, AUDIO-LLT-V): for those DARTs a named real domain is given here (HINTS).

domains(adt, dart_name) -> ([(pmgr device name, PS register address)], why)   empty list = unknown, don't touch
A PS register's ACTUAL field (bits 7:4) is 0xf when the domain is active.
"""

# DART -> the real PMGR domain(s) it sits in, where the ADT's gates don't lead to one.
HINTS = {
    "dart-disp0": ["DISP_FE"],
    "dart-dispext0": ["DISPEXT0_FE"],
    "dart-dispext1": ["DISPEXT1_FE"],
}


def _u32s(v):
    if v is None:
        return []
    if isinstance(v, int):
        return [v]
    if isinstance(v, (list, tuple)):
        return [int(x) for x in v]
    b = bytes(v)
    return [int.from_bytes(b[i:i + 4], "little") for i in range(0, len(b) // 4 * 4, 4)]


def _node(adt, path):
    try:
        return adt[path]
    except Exception:
        return None


def _devices(adt):
    pm = adt[adt._pmgr_path]
    by_id = {adt.pmgr_dev_get_id(d): d for d in pm.devices}
    by_name = {str(d.name): d for d in pm.devices}
    return by_id, by_name


def _real(adt, by_id, d, depth=0):
    """The nearest real (has a PS register) devices at or above d."""
    if not d.flags.no_ps:
        return [d]
    if depth > 8:
        return []
    out = []
    for p in adt.pmgr_dev_get_parents(d):
        if p and p in by_id:
            out += _real(adt, by_id, by_id[p], depth + 1)
    return out


def domains(adt, name):
    try:
        by_id, by_name = _devices(adt)
    except Exception as e:
        return [], f"PMGR table unreadable ({e})"
    if name in HINTS:
        ds = [by_name[n] for n in HINTS[name] if n in by_name]
        if ds:
            return [(str(d.name), adt.pmgr_dev_get_addr(d)) for d in ds], f"hint {HINTS[name]}"
    base = name[len("dart-"):] if name.startswith("dart-") else name
    tried = []
    for src in (name, base, base + "0"):
        node = _node(adt, f"/arm-io/{src}")
        if node is None:
            continue
        gates = [g for g in _u32s(node.getprop("clock-gates")) if g]
        # The first gate that leads to a real domain is the block's own (ISP_SYS, VENC_SYS, ...); later ones are
        # sub-blocks (sensors, encoder pipes) that may be off while the block and its DART run.
        reals = []
        for g in gates:
            if g in by_id:
                reals = _real(adt, by_id, by_id[g])
                if reals:
                    break
        tried.append(f"{src} gates {[hex(g) for g in gates]}")
        if reals:
            seen, out = set(), []
            for d in reals:
                if id(d) not in seen:
                    seen.add(id(d))
                    out.append((str(d.name), adt.pmgr_dev_get_addr(d)))
            return out, f"{src} gates {[hex(g) for g in gates]} -> {[n for n, _ in out]}"
    return [], "no real power domain found (" + "; ".join(tried or ["no clock-gates"]) + ")"
