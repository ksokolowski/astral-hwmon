# SPDX-License-Identifier: MIT
"""Locate the Astral card and the one I2C adapter its sensor chip answers on.

Mirrors driver/astral_detect.c. Measured 2026-08-13: the NVIDIA driver exposes
seven adapters under the GPU's PCI device, and only "NVIDIA i2c adapter 1"
(i2c-4 on the reference machine) has a device that ACKs at 0x2b. Every other
adapter returns -EIO, which is how the bus was identified.
"""

from __future__ import annotations

import re
from collections.abc import Iterator
from pathlib import Path

NVIDIA_VENDOR = 0x10DE

# The sysfs name carries a PCI suffix - "NVIDIA i2c adapter 1 at 1:00.0" - so
# this is matched by index rather than by string equality. The \b matters:
# a prefix compare would also accept "adapter 10".
ADAPTER_INDEX = 1
_ADAPTER_RE = re.compile(r"^NVIDIA i2c adapter (\d+)\b")

# Subsystem ids in DWORD form (device << 16 | vendor), the form both NVAPI and
# NVML report. Sourced from published ASUS Astral SKU ids (LibreHardwareMonitor's
# device list and the OpenRGB device registry) plus owner-reported lspci output.
# Only 0x89E31043 has been read on Linux here; the rest are listed on the
# assumption that the sensor block is identical across the Astral family.
ASTRAL_SUBSYSTEM_IDS: dict[int, str] = {
    0x89EA1043: "ROG Astral RTX 5090D OC",
    0x8A611043: "ROG Astral RTX 5090 Matrix",
    0x89EC1043: "ROG Astral RTX 5090 LC",
    0x89E31043: "ROG Astral RTX 5090 OC",
    0x89DE1043: "ROG Astral RTX 5080 OC",
    0x8A2E1043: "ROG Astral RTX 5090 OC White",
    0x8A2B1043: "ROG Astral RTX 5080 OC White",
    0x8A451043: "ROG Astral RTX 5080 OC Hatsune Miku",
}


def adapter_index(name: str) -> int | None:
    """Extract N from an "NVIDIA i2c adapter N at <pci>" sysfs name."""
    match = _ADAPTER_RE.match(name)
    return int(match.group(1)) if match else None


def _read_hex(path: Path) -> int | None:
    try:
        return int(path.read_text().strip(), 16)
    except (OSError, ValueError):
        return None


def _matching_devices(sysfs_root: str, allow_unknown: bool) -> Iterator[tuple[Path, int, str]]:
    devices = Path(sysfs_root) / "bus" / "pci" / "devices"
    if not devices.is_dir():
        return
    for dev in sorted(devices.iterdir()):
        if _read_hex(dev / "vendor") != NVIDIA_VENDOR:
            continue
        sub_vendor = _read_hex(dev / "subsystem_vendor")
        sub_device = _read_hex(dev / "subsystem_device")
        if sub_vendor is None or sub_device is None:
            continue
        subsystem = (sub_device << 16) | sub_vendor
        model = ASTRAL_SUBSYSTEM_IDS.get(subsystem)
        if model is None and not allow_unknown:
            continue
        yield dev, subsystem, model or f"unknown ({subsystem:#010x})"


def find_astral_adapter(sysfs_root: str = "/sys", allow_unknown: bool = False) -> int | None:
    """Return the i2c bus number to talk to, or None if no supported card."""
    for dev, _subsystem, _model in _matching_devices(sysfs_root, allow_unknown):
        for adapter in sorted(dev.glob("i2c-*")):
            try:
                name = (adapter / "name").read_text().strip()
            except OSError:
                continue
            if adapter_index(name) == ADAPTER_INDEX:
                return int(adapter.name.removeprefix("i2c-"))
    return None


def _adapter_numbers(paths: Iterator[Path]) -> set[int]:
    """Bus numbers from i2c-N entries, ignoring non-numeric ones.

    /sys/bus/i2c/devices also lists ACPI-enumerated adapters whose id is not a
    number at all - i2c-MSFT8000:01, i2c-ITE8853:00 - so the suffix cannot be
    assumed to parse as an int.
    """
    numbers = set()
    for path in paths:
        suffix = path.name.removeprefix("i2c-")
        if suffix.isdigit():
            numbers.add(int(suffix))
    return numbers


def nvidia_adapters(sysfs_root: str = "/sys") -> set[int]:
    """Every i2c adapter belonging to an NVIDIA GPU, Astral or not."""
    found: set[int] = set()
    devices = Path(sysfs_root) / "bus" / "pci" / "devices"
    if not devices.is_dir():
        return found
    for dev in sorted(devices.iterdir()):
        if _read_hex(dev / "vendor") != NVIDIA_VENDOR:
            continue
        found |= _adapter_numbers(dev.glob("i2c-*"))
    return found


def all_adapters(sysfs_root: str = "/sys") -> set[int]:
    base = Path(sysfs_root) / "bus" / "i2c" / "devices"
    if not base.is_dir():
        return set()
    return _adapter_numbers(base.glob("i2c-*"))


def find_foreign_adapter(sysfs_root: str = "/sys") -> int | None:
    """An adapter that is not on any NVIDIA GPU - the board's SMBus, typically.

    Used to prove the driver's PCI gate refuses buses that are not ours. Derived
    rather than hardcoded: i2c numbering follows probe order, so a fixed number
    can silently become the card's own adapter on another machine and invert
    what the test claims to prove.
    """
    others = sorted(all_adapters(sysfs_root) - nvidia_adapters(sysfs_root))
    return others[0] if others else None


def find_chipless_nvidia_adapter(sysfs_root: str = "/sys") -> int | None:
    """An adapter on the card that does not carry the sensor chip."""
    sensor = find_astral_adapter(sysfs_root)
    candidates = nvidia_adapters(sysfs_root) - ({sensor} if sensor is not None else set())
    return min(candidates) if candidates else None
