"""Bind alba serial operations in this checkout to rubra."""
import socket
import sys

ON_ALBA = socket.gethostname().split(".")[0].lower() == "alba"
if ON_ALBA:
    sys.path.insert(0, "/Users/satchlj/Projects/aurora-silicon/mac-aurora-omarchy/machines/tools/debug-cable")
    try:
        import debug_cable
    finally:
        sys.path.pop(0)


def default_path():
    return debug_cable.registered_path("rubra", "1")


def device(requested=None, channel="1", pattern=False, wait=False):
    if ON_ALBA:
        return debug_cable.serial_path("rubra", requested, channel, pattern, wait)
    return requested


def identify():
    if "--identify" in sys.argv[1:]:
        if not ON_ALBA:
            sys.exit("debug cable: run identification on alba")
        debug_cable.identify_serial("rubra", sys.argv[1:])
