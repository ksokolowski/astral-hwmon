# SPDX-License-Identifier: MIT
"""Read the driver's own surface, and drive its module lifecycle.

Channel numbering follows hwmon convention, which is not uniform: voltage
inputs are ZERO-indexed (in0..in5 = pins 1..6) while current inputs are
ONE-indexed (curr1..curr6 = pins 1..6). Verified against the loaded driver
2026-08-13; assuming in1..in6 reads the wrong pin and then runs off the end.
"""

from __future__ import annotations

import subprocess
from pathlib import Path

from .decode import PIN_COUNT, PinReading

CHIP_NAME = "astral12vhpwr"
MODULE_NAME = "astral-hwmon"
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


def module_resident(sysfs_root: str = "/sys") -> bool:
    """Is the module loaded into the kernel?

    Deliberately distinct from find_hwmon_dir(): the module can be resident and
    have registered nothing (no card, foreign bus, implausible frame). Tests
    that assert "it declined" need both facts to mean anything - which is why
    this one is unit-tested despite being two lines. If it ever returned True
    unconditionally, every anti-vacuity guard in test_gate.py would still pass
    while proving nothing.

    Note the dash: the module is astral-hwmon but /sys/module spells it with an
    underscore, as does lsmod.
    """
    return Path(sysfs_root).joinpath("module", MODULE_NAME.replace("-", "_")).exists()


def load_module(**params: object) -> None:
    args = [f"{key}={value}" for key, value in params.items()]
    subprocess.run(["sudo", "modprobe", MODULE_NAME, *args], check=True)


def unload_module() -> None:
    subprocess.run(["sudo", "modprobe", "-r", MODULE_NAME], check=False)
