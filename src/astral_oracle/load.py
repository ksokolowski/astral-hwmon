# SPDX-License-Identifier: MIT
"""Controlled GPU load for verification, with a hard current abort.

The burn exists to prove the sensors track real current. It must never be able
to sit on a dangerous current while collecting data, hence the abort guard -
9.5 A per pin, which sits just above ASUS's own Power Detector+ warning point
of 9.2 A and well below the 11-15 A range documented in connector failures.
"""

from __future__ import annotations

import contextlib
import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

from .decode import PinReading
from .i2c import read_pins

ABORT_MA = 9500  # just above ASUS's 9.2 A warning point; see module docstring
BURN_BIN = Path(__file__).resolve().parents[2] / "tools" / "burn"


class LoadAbortedError(RuntimeError):
    """A pin reached the abort threshold; sampling stopped immediately."""


@dataclass(frozen=True)
class LoadSample:
    elapsed: float
    readings: list[PinReading]
    watts: float | None


def read_power() -> float | None:
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=power.draw", "--format=csv,noheader,nounits"],
            capture_output=True,
            text=True,
            timeout=5,
            check=True,
        )
        # IndexError matters: nvidia-smi can exit 0 with empty output in some
        # driver states, and this function's whole contract is to degrade to
        # None rather than abort a burn that is already under way.
        return float(out.stdout.strip().splitlines()[0])
    except (OSError, subprocess.SubprocessError, ValueError, IndexError):
        return None


def sample_until(
    stop: Callable[[], bool],
    sampler: Callable[[], list[PinReading]],
    power: Callable[[], float | None] | None = None,
    abort_ma: int = ABORT_MA,
    interval: float = 0.5,
) -> list[LoadSample]:
    """Sample until `stop()` is true, aborting if any pin reaches `abort_ma`."""
    started = time.monotonic()
    samples: list[LoadSample] = []
    while not stop():
        readings = sampler()
        samples.append(
            LoadSample(
                elapsed=round(time.monotonic() - started, 3),
                readings=readings,
                watts=power() if power else None,
            )
        )
        worst = max(r.milliamps for r in readings)
        if worst >= abort_ma:
            raise LoadAbortedError(f"pin at {worst} mA reached the {abort_ma} mA abort threshold")
        if interval:
            time.sleep(interval)
    return samples


def _stop_burn(proc: subprocess.Popen[bytes], immediate: bool) -> None:
    """Stop the load and reap the child.

    `immediate` means the burn must stop now rather than after a grace period:
    either a pin reached the abort threshold, or sampling failed and we have
    lost sight of the current entirely. Both are cases where letting the load
    run on for another 10 s would break this module's promise.
    Every path waits, so no zombie is left behind.
    """
    if proc.poll() is not None:
        return

    if immediate:
        proc.kill()
    else:
        proc.terminate()
        try:
            proc.wait(timeout=10)
            return
        except subprocess.TimeoutExpired:
            proc.kill()

    with contextlib.suppress(subprocess.TimeoutExpired):
        proc.wait(timeout=10)


def run_burn(
    bus: int,
    seconds: int = 45,
    sampler: Callable[[], list[PinReading]] | None = None,
) -> list[LoadSample]:
    """Run the CUDA burn and sample throughout. Requires tools/burn to be built.

    `sampler` defaults to reading the six pins over i2c-dev. A caller may pass
    its own to observe something alongside, but it must still return the pin
    readings the abort guard watches - keep that on the independent path rather
    than on whatever is under test.
    """
    if not BURN_BIN.exists():
        raise FileNotFoundError(f"{BURN_BIN} not built - run `make burn`")
    proc = subprocess.Popen(
        [str(BURN_BIN), str(seconds)],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    aborted = False
    try:
        return sample_until(
            stop=lambda: proc.poll() is not None,
            sampler=sampler or (lambda: read_pins(bus)),
            power=read_power,
        )
    except BaseException:
        # Deliberately every exception, not just LoadAbortedError. A FrameError
        # - which is what a sub-6 V droop or a failed transfer raises - means
        # the guard has gone blind while the load is still running, and a
        # KeyboardInterrupt means the operator wants it to stop. Neither is a
        # reason to be gentler with the burn than an over-current is.
        aborted = True
        raise
    finally:
        _stop_burn(proc, immediate=aborted)
