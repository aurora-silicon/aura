#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Print ANS's SART entries as nvme.sart= (AURA_ANS_SART for run_guest_sptm.py).

A post-M4 stage that read NVMe powers ANS off when it is done (Stage 1 1.0.0b13 on T8132, after
its ESP read), and the SART loses its entries, iBoot's among them, with the power. That stage
keeps them and passes them to an image it chainloads itself (nvme.sart=). An image the host
chainloads from that stage's proxy gets nothing, so take them from the stage first:

    python3 proxyclient/tools/ans_sart.py --start

--start has the stage start ANS again (its nvme_init(), which puts its saved entries back), reads
them, and has it shut ANS down again (nvme_shutdown()). Without --start, ANS must already be on.
The value printed includes the stage's own RTKit buffers; the SPTM-virt cold handoff clears
those (src/nvme.c nvme_aura_memory()) and keeps iBoot's.
"""
import argparse
import pathlib
import sys

sys.path.append(str(pathlib.Path(__file__).resolve().parents[1]))

# SART register layouts by sart-version (src/sart.c): config, physical address and size word
# offsets per entry, and the shifts of the address and size fields.
LAYOUTS = {
    2: dict(cfg=0x00, pa=0x40, size=None),
    3: dict(cfg=0x00, pa=0x40, size=0x80),
    4: dict(cfg=0x00, pa=0x60, size=0xc0),
}
ENTRIES = 16


def read_entries(p, base, version):
    lay = LAYOUTS[version]
    out = []
    for i in range(ENTRIES):
        cfg = p.read32(base + lay["cfg"] + 4 * i)
        pa = p.read32(base + lay["pa"] + 4 * i) << 12
        if lay["size"] is None:  # v2: flags in [31:24], size in [23:0]
            flags, size = cfg >> 24, (cfg & 0xffffff) << 12
        else:
            flags, size = cfg & 0xff, p.read32(base + lay["size"] + 4 * i) << 12
        if flags or size:
            out.append((i, flags, pa, size))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--start", action="store_true",
                    help="have the stage start ANS (nvme_init) first and shut it down after")
    args = ap.parse_args()

    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    iface = UartInterface()
    p = AuraProxy(iface, debug=False)
    bootstrap_port(iface, p)
    u = ProxyUtils(p)
    adt = u.adt

    pm = adt[adt._pmgr_path]
    ans = [d for d in pm.devices if str(d.name) == "ANS"]
    if not ans:
        raise SystemExit("no ANS power domain in the PMGR table")
    ps = adt.pmgr_dev_get_addr(ans[0])

    def ans_on():
        return (p.read32(ps) >> 4) & 0xf == 0xf

    started = False
    if not ans_on():
        if not args.start:
            raise SystemExit("ANS is powered off: run again with --start")
        print("ANS is off: starting it on the stage (nvme_init)", file=sys.stderr)
        if not p.nvme_init():
            raise SystemExit("the stage's nvme_init failed: ANS stays off until a reboot")
        started = True
    if not ans_on():
        raise SystemExit("ANS is still off")

    node = adt["/arm-io/sart-ans"]
    version = int(getattr(node, "sart_version", 0))
    if version not in LAYOUTS:
        raise SystemExit(f"sart-version {version} isn't handled")
    base, _size = node.get_reg(0)
    entries = read_entries(p, base, version)
    if started and not p.nvme_shutdown():
        print("warning: the stage's nvme_shutdown failed", file=sys.stderr)
    for i, flags, pa, size in entries:
        print(f"SART {i:2d}: flags 0x{flags:02x} 0x{pa:x}+0x{size:x}", file=sys.stderr)
    if not entries:
        raise SystemExit("the SART is empty")
    print("nvme.sart=" + "".join(f"{i << 8 | flags:016x}{pa:016x}{size:016x}"
                                 for i, flags, pa, size in entries))


if __name__ == "__main__":
    main()
