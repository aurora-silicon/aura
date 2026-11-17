# SPDX-License-Identifier: MIT
"""Select the host compiler consistently with the CI compiler environment."""

import os
import shlex


def host_cc():
    return shlex.split(os.environ.get("CC") or
                       ("clang" if os.environ.get("USE_CLANG") == "1" else "cc"))
