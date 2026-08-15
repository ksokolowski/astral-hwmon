# SPDX-License-Identifier: MIT
"""Sensors must track real current, not just idle."""

import pytest

from astral_oracle.hwmon import find_hwmon_dir, load_module, read_hwmon_pins, unload_module
from astral_oracle.load import ABORT_MA, run_burn

pytestmark = pytest.mark.hardware


def test_oracle_tracks_burn(astral_adapter: int, burn_available: None) -> None:
    unload_module()
    samples = run_burn(astral_adapter, seconds=20)
    assert len(samples) > 10

    idle = min(sum(r.milliamps for r in s.readings) for s in samples)
    peak = max(sum(r.milliamps for r in s.readings) for s in samples)
    assert peak > idle * 3, f"burn did not load the card: {idle} -> {peak} mA"
    assert peak < ABORT_MA * 6


def test_driver_reports_plausible_values() -> None:
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None
    readings = read_hwmon_pins(hwmon_dir)
    # Deliberately tighter than the driver's own plausibility floor, which sits
    # at 6 V so that a sagging pin stays visible rather than silencing the
    # frame. This asserts the card in front of us is healthy, not what the gate
    # accepts: the lowest pin ever measured here under a 562 W burn was 11944 mV.
    # If this trips, suspect the connector before suspecting the test.
    assert all(11000 <= r.millivolts <= 13000 for r in readings)
    assert all(0 <= r.milliamps <= 30000 for r in readings)
