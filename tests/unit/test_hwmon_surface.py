# SPDX-License-Identifier: MIT
"""The few parts of hwmon.py that are logic rather than I/O.

Deliberately not a mirror of the whole module. read_hwmon_pins() and
load_module() exist to touch real sysfs and real modprobe; testing them against
files this suite wrote itself would prove only that the suite can write files,
and this project already has the scar - an over-simplified fake sysfs once made
a unit test agree with a bug real hardware rejected. Those stay in the hardware
tier, where they are covered against the actual driver.

What is here is the logic that decides things: the asymmetric attribute naming,
the search that has to skip entries it cannot read, the exclusion list that must
not silently drop a new attribute, and module_resident() - which is two lines
but which every anti-vacuity guard in test_gate.py leans on.
"""

from pathlib import Path

import pytest

from astral_oracle.hwmon import (
    CHIP_NAME,
    current_attr,
    find_hwmon_dir,
    module_resident,
    sensor_attributes,
    voltage_attr,
)


def test_voltage_and_current_numbering_is_asymmetric() -> None:
    """hwmon zero-indexes voltages and one-indexes currents, for the same pin.

    Assuming in1..in6 reads the wrong pin and then runs off the end. This is the
    single easiest thing to get wrong in the whole module.
    """
    assert [voltage_attr(pin) for pin in range(1, 7)] == [
        "in0_input",
        "in1_input",
        "in2_input",
        "in3_input",
        "in4_input",
        "in5_input",
    ]
    assert [current_attr(pin) for pin in range(1, 7)] == [
        "curr1_input",
        "curr2_input",
        "curr3_input",
        "curr4_input",
        "curr5_input",
        "curr6_input",
    ]


def _fake_hwmon(root: Path, chips: dict[str, str | None]) -> None:
    """Build a /sys/class/hwmon that looks like the real one.

    A `name` of None means the directory exists without a readable name, which
    is what an entry being torn down mid-scan looks like.
    """
    base = root / "class" / "hwmon"
    base.mkdir(parents=True)
    for entry, name in chips.items():
        (base / entry).mkdir()
        if name is not None:
            (base / entry / "name").write_text(name + "\n")


def test_find_hwmon_dir_picks_our_chip_out_of_a_populated_class(tmp_path: Path) -> None:
    # A real machine has several of these. Ours is not first, and not last.
    _fake_hwmon(
        tmp_path,
        {"hwmon0": "coretemp", "hwmon1": "nvme", "hwmon2": CHIP_NAME, "hwmon3": "iwlwifi_1"},
    )
    found = find_hwmon_dir(sysfs_root=str(tmp_path))
    assert found is not None
    assert found.name == "hwmon2"


def test_find_hwmon_dir_skips_entries_it_cannot_read(tmp_path: Path) -> None:
    _fake_hwmon(tmp_path, {"hwmon0": None, "hwmon1": CHIP_NAME})
    found = find_hwmon_dir(sysfs_root=str(tmp_path))
    assert found is not None
    assert found.name == "hwmon1"


def test_find_hwmon_dir_returns_none_when_absent(tmp_path: Path) -> None:
    _fake_hwmon(tmp_path, {"hwmon0": "coretemp"})
    assert find_hwmon_dir(sysfs_root=str(tmp_path)) is None


def test_find_hwmon_dir_returns_none_without_a_hwmon_class(tmp_path: Path) -> None:
    # A kernel with no hwmon at all must be None, not an exception.
    assert find_hwmon_dir(sysfs_root=str(tmp_path)) is None


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


def test_sensor_attributes_excludes_plumbing_but_keeps_new_attributes(
    tmp_path: Path,
) -> None:
    """An exclusion list, not an in*/curr* allowlist.

    The allowlist this replaced silently skipped update_interval when it was
    added, so the read-only sweep stopped covering it. Any attribute the driver
    grows must be picked up here without anyone remembering to widen a pattern.
    """
    for name in ("in0_input", "curr1_input", "name", "update_interval", "uevent"):
        (tmp_path / name).write_text("0\n")
    # uevent is 0644 on every sysfs device, so a blanket "nothing is writable"
    # sweep would flag the kernel's own plumbing rather than the driver.
    for name in ("device", "subsystem", "power"):
        (tmp_path / name).mkdir()
    # A hypothetical future attribute, to prove nothing needs teaching about it.
    (tmp_path / "temp1_input").write_text("0\n")

    names = {entry.name for entry in sensor_attributes(tmp_path)}
    assert names == {"in0_input", "curr1_input", "name", "update_interval", "temp1_input"}
