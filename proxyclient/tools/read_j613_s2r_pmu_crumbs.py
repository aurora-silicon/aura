#!/usr/bin/env python3
"""Read Aura's retained J613 sleep crumbs from a fresh Stage 1 proxy."""
import argparse
import json


def decode(values):
    if len(values) != 5 or any(value < 0 or value > 0xff for value in values):
        raise ValueError("J613 S2R PMU record must be five bytes")
    return {
        "step": values[0],
        "wfi_mask": values[1] | values[2] << 8,
        "wake_step": values[3],
        "commit_count": values[4],
        "raw": values,
    }


def main():
    from aura.hw.pmu import PMU
    from aura.proxy import AuraProxy, UartInterface
    from aura.proxyutils import ProxyUtils, bootstrap_port

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reboot", action="store_true",
                        help="send one ordinary P_REBOOT after the read")
    args = parser.parse_args()

    iface = UartInterface()
    proxy = AuraProxy(iface)
    bootstrap_port(iface, proxy)
    if proxy.get_chipid() != 0x8122:
        raise RuntimeError("not T8122/J613")

    try:
        utils = ProxyUtils(proxy)
        pmu = PMU(utils)
        addresses = range(0x830d, 0x8312)
        values = [pmu.spmi.read8(pmu.reg, address) for address in addresses]
        print(json.dumps(decode(values), sort_keys=True), flush=True)
    finally:
        if args.reboot:
            proxy.reboot()


if __name__ == "__main__":
    main()
