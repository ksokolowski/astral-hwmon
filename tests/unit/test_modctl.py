# SPDX-License-Identifier: MIT
"""The one part of modctl.py that is logic rather than a privileged call.

load_module() and unload_module() shell out to sudo modprobe. Testing them here
would mean either changing the machine from a unit test or asserting that a mock
was called, which proves nothing about the driver; they are covered in the
hardware tier against the real module. What is left is module_resident(), which
is two lines and which every anti-vacuity guard in test_gate.py leans on.
"""

from pathlib import Path

import pytest

from astral_oracle.modctl import module_resident


@pytest.mark.parametrize("resident", [True, False])
def test_module_resident_spells_the_name_with_an_underscore(tmp_path: Path, resident: bool) -> None:
    """Both outcomes, because a function stuck on True is the dangerous failure.

    test_gate.py asserts module_resident() before concluding the driver declined
    to register. If this could only ever return True, those guards would pass on
    a machine where the module was never installed.
    """
    modules = tmp_path / "module"
    modules.mkdir()
    if resident:
        (modules / "astral_hwmon").mkdir()
    # The dash spelling must never match: modprobe takes astral-hwmon, but
    # /sys/module and lsmod both report astral_hwmon.
    (modules / "unrelated").mkdir()

    assert module_resident(sysfs_root=str(tmp_path)) is resident
    assert not (modules / "astral-hwmon").exists()
