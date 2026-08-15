# SPDX-License-Identifier: MIT
"""The 0444 claim, checked rather than trusted."""

import pytest

from astral_oracle.hwmon import find_hwmon_dir, load_module, sensor_attributes

pytestmark = pytest.mark.hardware


def test_no_writable_sensor_attribute_exists() -> None:
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None

    attributes = sensor_attributes(hwmon_dir)
    names = {entry.name for entry in attributes}
    expected = (
        {f"in{i}_input" for i in range(6)}
        | {f"in{i}_label" for i in range(6)}
        | {f"curr{i}_input" for i in range(1, 7)}
        | {f"curr{i}_label" for i in range(1, 7)}
        | {"name", "update_interval"}
    )
    assert expected <= names, f"missing attributes: {sorted(expected - names)}"

    writable = [entry.name for entry in attributes if entry.stat().st_mode & 0o222]
    assert writable == [], f"writable hwmon attributes present: {writable}"


def test_no_threshold_attributes_are_published() -> None:
    """v1 is telemetry only - a threshold nothing enforces must not appear."""
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None

    thresholds = [
        entry.name
        for entry in sensor_attributes(hwmon_dir)
        if entry.name.endswith(("_max", "_crit", "_min", "_alarm"))
    ]
    assert thresholds == [], f"unexpected threshold attributes: {thresholds}"
