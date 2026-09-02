# SPDX-License-Identifier: MIT
"""Read the driver's own sysfs surface.

Reading only: nothing here needs privileges, and nothing here can change the
machine. Loading and unloading the module is modctl.py's job.

Channel numbering follows hwmon convention, which is not uniform: voltage
inputs are ZERO-indexed (in0..in5 = pins 1..6) while current inputs are
ONE-indexed (curr1..curr6 = pins 1..6). Verified against the loaded driver
2026-08-13; assuming in1..in6 reads the wrong pin and then runs off the end.
"""

from __future__ import annotations

from pathlib import Path

from .decode import PIN_COUNT, PinReading

CHIP_NAME = "astral12vhpwr"
HWMON_ROOT = Path("/sys/class/hwmon")


def voltage_attr(pin: int) -> str:
    """sysfs attribute holding the voltage for a 1-based pin number."""
    return f"in{pin - 1}_input"


def current_attr(pin: int) -> str:
    """sysfs attribute holding the current for a 1-based pin number."""
    return f"curr{pin}_input"


def find_hwmon_dir(chip: str = CHIP_NAME, sysfs_root: str = "/sys") -> Path | None:
    """Locate the chip's hwmon directory, or None if it is not registered.

    `sysfs_root` is injectable for the same reason discover.py's is: it is the
    only way to test the search without a card. Real callers use the default.
    """
    root = Path(sysfs_root) / "class" / "hwmon"
    if not root.is_dir():
        return None
    for entry in sorted(root.iterdir()):
        try:
            if (entry / "name").read_text().strip() == chip:
                return entry
        except OSError:
            continue
    return None


def read_hwmon_pins(path: Path) -> list[PinReading]:
    return [
        PinReading(
            pin=pin,
            millivolts=int((path / voltage_attr(pin)).read_text().strip()),
            milliamps=int((path / current_attr(pin)).read_text().strip()),
        )
        for pin in range(1, PIN_COUNT + 1)
    ]


# uevent is 0644 on every sysfs device, so a blanket "nothing is writable"
# sweep would flag the kernel's plumbing rather than the driver.
SYSFS_PLUMBING = frozenset({"uevent", "device", "subsystem", "power"})


def sensor_attributes(path: Path) -> list[Path]:
    """Every attribute the driver owns, excluding sysfs plumbing.

    Deliberately an exclusion list rather than an in*/curr* allowlist: a new
    attribute must be covered by the read-only invariant automatically. An
    allowlist silently skipped update_interval when it was added.
    """
    return [
        entry
        for entry in sorted(path.iterdir())
        if entry.is_file() and entry.name not in SYSFS_PLUMBING
    ]
