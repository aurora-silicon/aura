# SPDX-License-Identifier: MIT
"""G5b launcher: input before GPU POWER, and the merged launcher's refusal guards
(G5B-BUILD 2.3 items 2 and 3, 3.4 "Launcher order test").

Failure modes written down before the code:
  L-1 the GPU is admitted or powered before the input device (the MTP bring-up) attaches, so an
      input refusal spends the boot's GPU firmware session with no guest;
  L-2 an input refusal (contract, ATTACH, --windows-input-require-mtp) reaches any GPU op;
  L-3 a GPU refusal (pool, ADMIT, G2, POWER, PROBE) leaves the input device attached, or does not
      say the MTP keeps running (only P_REBOOT leaves);
  L-4 --windows-input-probe-only with a GPU mode, --windows-gpu probe/g2 with the NVMe backend,
      or --windows-serror continue with the GPU is not refused before anything reaches the M6;
  L-5 the guards refuse what B0/B1 run (RAM store, g2, host+kbd+tp, tick CPU).
A recording proxy stands in for the M6.
"""
import ast
import pathlib
import sys
import types

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "proxyclient"))
RUN_GUEST = REPO / "proxyclient/tools/run_guest.py"


class _Exit(Exception):
    pass


class _RemoteError(Exception):
    pass


def _functions(*names):
    src = RUN_GUEST.read_text()
    fns = [n for n in ast.parse(src).body if isinstance(n, ast.FunctionDef) and n.name in names]
    assert {f.name for f in fns} == set(names)
    return ast.Module(body=fns, type_ignores=[])


def _launch_sequence():
    """The order run_windows_t8152() calls the attach steps in (from its own source)."""
    src = RUN_GUEST.read_text()
    fn = next(n for n in ast.parse(src).body
              if isinstance(n, ast.FunctionDef) and n.name == "run_windows_t8152")
    order = []
    for node in ast.walk(fn):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and \
                node.func.id.startswith("windows_t8152_") and node.func.id.endswith(
                    ("_attach", "_admit", "_probe")) and "nvme" not in node.func.id and \
                node.func.id != "windows_t8152_ahci_attach":
            order.append((node.lineno, node.func.id))
    return [name for _, name in sorted(order)]


def _harness(fail=None, features_live=0x7, require_mtp=True):
    """input_attach + gpu_admit in run_windows_t8152's order against one recording proxy.
    fail: the op that refuses ("query", "attach", "pool", "admit", "g2", "power", "probe")."""
    from aura import windows as w
    calls = []
    bad = -3 & ((1 << 64) - 1)

    class Proxy:
        def hv_windows_input(self, op, *a):
            calls.append(("input", op, *a))
            if op == w.INPUT_OP_QUERY:
                if fail == "query":
                    raise _RemoteError("Bad Command")
                return {w.INPUT_Q["version"]: w.INPUT_VERSION, w.INPUT_Q["base"]: w.INPUT_BASE,
                        w.INPUT_Q["size"]: w.INPUT_SIZE, w.INPUT_Q["intid"]: w.INPUT_GSIV,
                        w.INPUT_Q["features"]: w.INPUT_FEATURES_REPORTS |
                        features_live << w.INPUT_SRC_FEATURE_SHIFT,
                        w.INPUT_Q["sources"]: 7}[a[0]]
            if op == w.INPUT_OP_ATTACH:
                return 2 if fail == "attach" else 0
            if op == w.INPUT_OP_STATUS:
                return 0
            return 0

        def hv_windows_gpu(self, op, *a):
            calls.append(("gpu", op, *a))
            if op == w.GPU_OP_STATUS:
                return {w.GPU_STATUS_HANDOFF_IPA: 0x8_0600_0000,
                        w.GPU_STATUS_HANDOFF_SIZE: 0x4000, w.GPU_STATUS_RANGES: 40}.get(a[0], 0)
            return {w.GPU_OP_ADMIT: "admit", w.GPU_OP_G2: "g2", w.GPU_OP_POWER: "power",
                    w.GPU_OP_PROBE: "probe"}.get(op) == fail and bad or 0

        def hv_windows_usb(self, op, *a):
            calls.append(("usb", op, *a))
            return 0

    def sys_exit(message):
        calls.append(("exit", message))
        raise _Exit(message)

    base, top = 0x8_0000_0000, 0x9_2000_0000
    image_end = 0x9_1800_0000 if fail == "pool" else base + 0x200_0000
    hv = types.SimpleNamespace(
        windows_guest_ram=(base, top), tba=types.SimpleNamespace(top_of_kernel_data=image_end),
        set_windows_gpu_handshake=lambda v: calls.append(("hs-gpu", v)),
        set_windows_input_handshake=lambda v: calls.append(("hs-input", v)))
    env = {"p": Proxy(), "sys": types.SimpleNamespace(exit=sys_exit),
           "ProxyRemoteError": _RemoteError, "print": lambda *a: None,
           "iface": types.SimpleNamespace(writemem=lambda a, d: calls.append(("mem", a))),
           "no_timeout": lambda call, *a: call(*a),
           "args": types.SimpleNamespace(windows_input_fn_default="media",
                                         windows_input_require_mtp=require_mtp,
                                         windows_gpu_pool_mib=256,
                                         windows_gpu_g2_grant="fw-mmio,boot-control")}
    exec(compile(_functions("windows_t8152_input_attach", "windows_t8152_gpu_admit"),
                 "run_guest.py", "exec"), env)
    sources = ("host", "kbd", "tp")
    mtp = {"base": top - w.INPUT_AREA_SIZE, "size": w.INPUT_AREA_SIZE, "payload": b"\0" * 64,
           "params": b"\0" * 64}
    steps = {"windows_t8152_input_attach": lambda: env["windows_t8152_input_attach"](
                 hv, sources, (), mtp, "g2"),
             "windows_t8152_gpu_admit": lambda: env["windows_t8152_gpu_admit"](
                 hv, "g2", (), None, sources, mtp)}
    try:
        for name in _launch_sequence():
            if name in steps:
                steps[name]()
    except _Exit:
        pass
    return calls


def _ops(calls, w):
    names = {("input", w.INPUT_OP_ATTACH): "IN_ATTACH", ("input", w.INPUT_OP_DETACH): "IN_DETACH",
             ("gpu", w.GPU_OP_ADMIT): "ADMIT", ("gpu", w.GPU_OP_G2): "G2",
             ("gpu", w.GPU_OP_POWER): "POWER", ("gpu", w.GPU_OP_PROBE): "PROBE",
             ("gpu", w.GPU_OP_STOP): "STOP"}
    return [names[c[:2]] for c in calls if c[:2] in names]


def test_launch_sequence_is_input_before_gpu():
    """L-1, from run_windows_t8152's own source."""
    seq = _launch_sequence()
    assert seq.index("windows_t8152_usb_attach") < seq.index("windows_t8152_input_attach") < \
        seq.index("windows_t8152_input_probe") < seq.index("windows_t8152_gpu_admit")


def test_success_attaches_input_then_powers_the_gpu():
    from aura import windows as w
    calls = _harness()
    assert _ops(calls, w) == ["IN_ATTACH", "ADMIT", "G2", "POWER", "PROBE"]
    assert not any(c[0] == "exit" for c in calls)
    kinds = [c[0] for c in calls]
    assert kinds.index("hs-input") < kinds.index("hs-gpu")


@pytest.mark.parametrize("fail", ["query", "attach", "require-mtp"])
def test_input_refusal_reaches_no_gpu_op(fail):
    """L-2."""
    from aura import windows as w
    calls = _harness(fail=fail if fail != "require-mtp" else None,
                     features_live=0x1 if fail == "require-mtp" else 0x7)
    assert calls[-1][0] == "exit"
    assert not [o for o in _ops(calls, w) if o in ("ADMIT", "G2", "POWER", "PROBE")]


@pytest.mark.parametrize("fail", ["pool", "admit", "g2", "power", "probe"])
def test_gpu_refusal_detaches_input_and_names_the_reboot(fail):
    """L-3."""
    from aura import windows as w
    calls = _harness(fail=fail)
    ops = _ops(calls, w)
    assert calls[-1][0] == "exit", calls[-1]
    assert ops[0] == "IN_ATTACH" and ops[-1] == "IN_DETACH"
    if fail in ("pool", "admit", "g2"):
        assert "POWER" not in ops                      # the firmware session is not spent
    msg = calls[-1][1]
    assert "Input was detached again." in msg and "P_REBOOT" in msg


def _parser_and_runner():
    src = RUN_GUEST.read_text()
    head = src[:src.index("args = parser.parse_args()")]
    env = {"__file__": str(RUN_GUEST), "__name__": "run_guest_parser"}
    exec(compile(head, "run_guest.py", "exec"), env)
    return env


GUARD_CASES = [
    (["--windows-input", "host+kbd", "--windows-input-probe", "10", "--windows-input-probe-only",
      "--windows-gpu", "g2"], "--windows-input-probe-only starts no guest"),
    (["--windows-gpu", "g2", "--windows-ahci-backend", "nvme"], "is for the RAM store"),
    (["--windows-gpu", "probe", "--windows-ahci-partition", "Windows-M6"], "is for the RAM store"),
    (["--windows-gpu", "g2", "--windows-serror", "continue"], "--windows-serror continue"),
]
PASS_CASES = [
    ["--windows-gpu", "g2", "--windows-gpu-g2-grant", "fw-mmio,boot-control",
     "--windows-ahci-backend", "ram", "--windows-input", "host+kbd+tp", "--windows-tick-cpu", "7"],
    ["--windows-input", "host+kbd", "--windows-input-probe", "10", "--windows-input-probe-only"],
    ["--windows-gpu", "admit", "--windows-ahci-backend", "nvme"],
]


def _run_guards(argv):
    env = _parser_and_runner()
    args = env["parser"].parse_args(["-r", *argv, "payload.fd"])
    calls = []

    def sys_exit(message=None):
        calls.append(message)
        raise _Exit(message)

    ns = {"args": args, "sys": types.SimpleNamespace(exit=sys_exit)}
    exec(compile(_functions("run_windows_t8152", "ahci_nvme_requested"), "run_guest.py", "exec"),
         ns)
    with pytest.raises(_Exit):
        ns["run_windows_t8152"](None)
    return calls[-1]


@pytest.mark.parametrize("argv,why", GUARD_CASES)
def test_guards_refuse_before_anything_reaches_the_m6(argv, why):
    """L-4: each refuses at the top of run_windows_t8152 (no proxy is even given)."""
    msg = _run_guards(argv)
    assert msg.startswith("T8152 launch refused") and why in msg, msg


@pytest.mark.parametrize("argv", PASS_CASES)
def test_guards_let_the_b0_b1_launch_through(argv):
    """L-5: these get past every guard to the next check (the guest RAM window)."""
    msg = _run_guards(argv)
    assert msg.startswith("T8152 needs --windows-guest-ram"), msg
