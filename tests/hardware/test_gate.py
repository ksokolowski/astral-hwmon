# SPDX-License-Identifier: MIT
"""Regression tests for the PCI gate and the all-zeros failure mode.

These guard the "never touch a foreign bus" invariant, so they must never pass
vacuously: each asserts the module is actually resident before concluding that
it declined to register. Without that, a module that failed to build, was
rejected by Secure Boot, or simply is not installed would make all three go
green while exercising nothing.
"""

import subprocess

import pytest

from astral_oracle.discover import find_chipless_nvidia_adapter, find_foreign_adapter
from astral_oracle.hwmon import MODULE_NAME, find_hwmon_dir, module_resident, unload_module

pytestmark = pytest.mark.hardware


def _modprobe(*params: str) -> None:
    subprocess.run(
        ["sudo", "modprobe", MODULE_NAME, *params],
        capture_output=True,
        text=True,
        check=False,
    )


def _assert_declined(context: str) -> None:
    assert module_resident(), (
        f"{context}: module is not resident, so this test proved nothing "
        "(not installed, build failed, or blocked by Secure Boot)"
    )
    assert find_hwmon_dir() is None, f"{context}: module registered sensors when it should not"


def test_declines_adapter_without_chip() -> None:
    adapter = find_chipless_nvidia_adapter()
    if adapter is None:
        pytest.skip("card exposes only one i2c adapter; no chipless one to test against")

    unload_module()
    _modprobe(f"bus={adapter}")
    try:
        _assert_declined(f"bus={adapter} (NVIDIA adapter with no chip at 0x2b)")
    finally:
        unload_module()


def test_bus_parameter_cannot_escape_the_pci_gate() -> None:
    """bus= chooses among the card's adapters; it must never reach a foreign bus.

    0x2b on the board's SMBus belongs to some other device entirely, and even
    the register-pointer write of a read is traffic we have no business making
    there. The PCI-id check therefore runs before bus= is considered.
    """
    adapter = find_foreign_adapter()
    if adapter is None:
        pytest.skip("no non-NVIDIA i2c adapter on this machine to test against")

    unload_module()
    _modprobe(f"bus={adapter}")
    try:
        _assert_declined(f"bus={adapter} (not an NVIDIA adapter)")
    finally:
        unload_module()


def test_allow_unknown_still_requires_an_nvidia_adapter() -> None:
    """allow_unknown skips the model allowlist, not the vendor check."""
    adapter = find_foreign_adapter()
    if adapter is None:
        pytest.skip("no non-NVIDIA i2c adapter on this machine to test against")

    unload_module()
    _modprobe(f"bus={adapter}", "allow_unknown=1")
    try:
        _assert_declined(f"bus={adapter} allow_unknown=1")
    finally:
        unload_module()
