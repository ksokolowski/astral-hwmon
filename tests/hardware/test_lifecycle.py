# SPDX-License-Identifier: MIT
"""The hwmon node must come and go cleanly with the module."""

import pytest

from astral_oracle.hwmon import find_hwmon_dir, load_module, unload_module

pytestmark = pytest.mark.hardware


def test_load_unload_cycle() -> None:
    for _ in range(3):
        load_module()
        assert find_hwmon_dir() is not None
        unload_module()
        assert find_hwmon_dir() is None
