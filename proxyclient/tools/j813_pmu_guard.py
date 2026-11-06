#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Bounded J813 PMU guard; the shared implementation defaults to J873."""
from j873_pmu_guard import main

if __name__ == "__main__":
    raise SystemExit(main("j813"))
