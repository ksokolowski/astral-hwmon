# SPDX-License-Identifier: MIT
"""The driver and the oracle must agree. If they don't, one is wrong.

Both paths can be read at the same time. The i2c client is created without an
i2c_driver bound to it - hwmon is registered directly on the device - so
i2c-dev never marks 0x2b busy and userspace keeps its access. Measured
2026-08-13, contradicting the assumption in the original spec that a bound
driver would lock userspace out. Simultaneous sampling is the better test
anyway: currents can be compared, not just voltages across an unload gap.
"""

import time

import pytest

from astral_oracle.decode import PIN_COUNT, PinReading
from astral_oracle.hwmon import find_hwmon_dir, load_module, read_hwmon_pins, unload_module
from astral_oracle.i2c import read_pins
from astral_oracle.load import run_burn

pytestmark = pytest.mark.hardware

ROUNDS = 5
ROUND_INTERVAL = 0.3  # longer than the driver's 200 ms cache TTL
VOLTAGE_TOLERANCE_MV = 50
CURRENT_TOLERANCE_MA = 150

# The pin-to-pin current spread the mapping test needs before it can conclude
# anything. Measured 2026-08-13: 80 mA at idle, but 600-920 mA under the burn
# (docs/measurements/2026-08-13/load-decoded.txt). Idle is inside the agreement
# tolerance, so the comparison has to happen under load or not at all.
MIN_SPREAD_MA = 400
MAPPING_BURN_SECONDS = 20


def _mean_per_pin(rounds: list[list[PinReading]]) -> list[float]:
    return [sum(r[pin].milliamps for r in rounds) / len(rounds) for pin in range(PIN_COUNT)]


def test_driver_matches_oracle(astral_adapter: int) -> None:
    """Values agree pin-for-pin at idle.

    This checks the numbers, not the pin ORDER: at idle the spread across pins
    is smaller than the tolerance, so a transposition would pass here. That is
    test_pin_mapping_is_not_transposed's job, and it needs load to do it.
    """
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None, "driver loaded but registered no hwmon chip"

    oracle_mv = [0.0] * 6
    oracle_ma = [0.0] * 6
    driver_mv = [0.0] * 6
    driver_ma = [0.0] * 6

    for _ in range(ROUNDS):
        for pin, reading in enumerate(read_pins(astral_adapter)):
            oracle_mv[pin] += reading.millivolts / ROUNDS
            oracle_ma[pin] += reading.milliamps / ROUNDS
        for pin, reading in enumerate(read_hwmon_pins(hwmon_dir)):
            driver_mv[pin] += reading.millivolts / ROUNDS
            driver_ma[pin] += reading.milliamps / ROUNDS
        time.sleep(ROUND_INTERVAL)

    for pin in range(6):
        assert abs(driver_mv[pin] - oracle_mv[pin]) <= VOLTAGE_TOLERANCE_MV, (
            f"pin {pin + 1} voltage: driver {driver_mv[pin]:.0f} mV "
            f"vs oracle {oracle_mv[pin]:.0f} mV"
        )
        assert abs(driver_ma[pin] - oracle_ma[pin]) <= CURRENT_TOLERANCE_MA, (
            f"pin {pin + 1} current: driver {driver_ma[pin]:.0f} mA "
            f"vs oracle {oracle_ma[pin]:.0f} mA"
        )


def test_pin_mapping_is_not_transposed(astral_adapter: int, burn_available: None) -> None:
    """Both sides can agree pin-for-pin while the pin ORDER differs.

    This has to run under load. At idle the six pins sit within ~80 mA of each
    other, which is well inside CURRENT_TOLERANCE_MA, so a complete
    transposition passes test_driver_matches_oracle unnoticed - and picking the
    "busiest" pin from an 80 mA spread is really picking the winner of 20 mA
    quantisation noise, which is both blind to the bug and flaky. Under the burn
    the spread is 600-920 mA and the comparison can actually fail.

    The spread is asserted, not assumed: if the burn does not load the card, the
    test says so rather than passing on data that could not have caught anything.
    """
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None

    driver_rounds: list[list[PinReading]] = []

    def sample_both() -> list[PinReading]:
        # Returns the oracle reading, so the abort guard keeps watching the
        # independent path rather than the driver under test. The driver is
        # captured alongside, at effectively the same instant.
        driver_rounds.append(read_hwmon_pins(hwmon_dir))
        return read_pins(astral_adapter)

    samples = run_burn(astral_adapter, seconds=MAPPING_BURN_SECONDS, sampler=sample_both)
    assert len(samples) == len(driver_rounds)

    # Only the loaded part of the run. The first samples are still idle, and
    # averaging them in would shrink exactly the spread this test depends on.
    totals = [sum(r.milliamps for r in s.readings) for s in samples]
    threshold = max(totals) / 2
    loaded = [i for i, total in enumerate(totals) if total >= threshold]
    assert len(loaded) >= 5, f"burn produced only {len(loaded)} loaded samples"

    oracle_ma = _mean_per_pin([samples[i].readings for i in loaded])
    driver_ma = _mean_per_pin([driver_rounds[i] for i in loaded])

    spread = max(oracle_ma) - min(oracle_ma)
    assert spread >= MIN_SPREAD_MA, (
        f"only {spread:.0f} mA of spread across pins under load; below "
        f"{CURRENT_TOLERANCE_MA} mA this comparison cannot distinguish a "
        "transposition, so it would pass without proving anything"
    )

    for pin in range(PIN_COUNT):
        assert abs(driver_ma[pin] - oracle_ma[pin]) <= CURRENT_TOLERANCE_MA, (
            f"pin {pin + 1} under load: driver {driver_ma[pin]:.0f} mA "
            f"vs oracle {oracle_ma[pin]:.0f} mA"
        )

    assert oracle_ma.index(max(oracle_ma)) == driver_ma.index(max(driver_ma)), (
        f"busiest pin disagrees: oracle {oracle_ma}, driver {driver_ma}"
    )
    assert oracle_ma.index(min(oracle_ma)) == driver_ma.index(min(driver_ma)), (
        f"quietest pin disagrees: oracle {oracle_ma}, driver {driver_ma}"
    )


def test_labels_match_windows_names() -> None:
    """in0 is pin 1: hwmon zero-indexes voltages and one-indexes currents."""
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None
    assert (hwmon_dir / "in0_label").read_text().strip() == "12VHPWR Pin1 Voltage"
    assert (hwmon_dir / "in5_label").read_text().strip() == "12VHPWR Pin6 Voltage"
    assert (hwmon_dir / "curr1_label").read_text().strip() == "12VHPWR Pin1 Current"
    assert (hwmon_dir / "curr6_label").read_text().strip() == "12VHPWR Pin6 Current"


def test_hwmon_node_disappears_when_unloaded() -> None:
    load_module()
    assert find_hwmon_dir() is not None
    unload_module()
    assert find_hwmon_dir() is None
