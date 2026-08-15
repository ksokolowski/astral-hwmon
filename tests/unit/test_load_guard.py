# SPDX-License-Identifier: MIT
"""The abort guard must stop sampling before a dangerous current persists."""

import pytest

from astral_oracle.decode import PinReading
from astral_oracle.load import ABORT_MA, LoadAbortedError, sample_until


def frame(milliamps: int) -> list[PinReading]:
    return [PinReading(pin=p, millivolts=12000, milliamps=milliamps) for p in range(1, 7)]


def test_samples_until_stop_returns_true() -> None:
    calls = {"n": 0}

    def stop() -> bool:
        calls["n"] += 1
        return calls["n"] > 3

    samples = sample_until(stop, lambda: frame(500), interval=0)
    assert len(samples) == 3


def test_aborts_when_any_pin_reaches_threshold() -> None:
    readings = iter([frame(500), frame(500), frame(ABORT_MA), frame(500)])
    with pytest.raises(LoadAbortedError, match=str(ABORT_MA)):
        sample_until(lambda: False, lambda: next(readings), interval=0)


def test_does_not_abort_just_below_threshold() -> None:
    readings = iter([frame(ABORT_MA - 1), frame(ABORT_MA - 1)])
    stop = iter([False, False, True])
    samples = sample_until(lambda: next(stop), lambda: next(readings), interval=0)
    assert len(samples) == 2


def test_abort_threshold_matches_project_default() -> None:
    """9.5 A sits just above ASUS's own 9.2 A warning point; do not raise it."""
    assert ABORT_MA == 9500
