# SPDX-License-Identifier: MIT
"""The oracle reads the chip by a path fully independent of the driver."""

import subprocess

import pytest

from astral_oracle.decode import PLAUSIBLE_MA, PLAUSIBLE_MV
from astral_oracle.i2c import I2cError, read_block, read_pins

pytestmark = pytest.mark.hardware


def test_reads_six_plausible_pins(astral_adapter: int) -> None:
    readings = read_pins(astral_adapter)
    assert len(readings) == 6
    for r in readings:
        assert r.millivolts in PLAUSIBLE_MV
        assert r.milliamps in PLAUSIBLE_MA


def test_agrees_with_i2c_tools(astral_adapter: int) -> None:
    """Cross-check our ioctl path against a wholly separate codebase."""
    ours = read_block(astral_adapter)
    out = subprocess.run(
        ["i2cget", "-y", str(astral_adapter), "0x2b", "0x80", "i", "24"],
        capture_output=True,
        text=True,
        check=True,
    )
    theirs = bytes(int(tok, 16) for tok in out.stdout.split())
    # Currents move between the two reads; voltages are stable to within a few mV.
    for pin in range(6):
        ours_mv = (ours[pin * 4] << 8) | ours[pin * 4 + 1]
        theirs_mv = (theirs[pin * 4] << 8) | theirs[pin * 4 + 1]
        assert abs(ours_mv - theirs_mv) <= 100


def test_rejects_bad_address(astral_adapter: int) -> None:
    with pytest.raises(I2cError):
        read_block(astral_adapter, addr=0x7F)
