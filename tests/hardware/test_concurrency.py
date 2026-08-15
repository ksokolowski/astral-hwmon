# SPDX-License-Identifier: MIT
"""Hammer the driver from several readers at once.

What this DOES cover: the locking added for the cache. Two deadlocks were
introduced while writing that locking and caught by hand, so parallel readers
are worth exercising deliberately rather than trusting review.

What this does NOT cover: the cache-publication bug it grew out of. That one
needs a *failing* read to clobber the cache while another thread holds a cache
hit, and a healthy card never fails a read. Reproducing it would need
injectable I2C failures, which nothing here provides. This test would not have
caught it - said plainly so nobody reads a green run as proof that it would.
"""

import concurrent.futures
import time

import pytest

from astral_oracle.decode import PLAUSIBLE_MA, PLAUSIBLE_MV
from astral_oracle.hwmon import find_hwmon_dir, load_module, read_hwmon_pins

pytestmark = pytest.mark.hardware

READERS = 8
DURATION_SEC = 5.0


def test_parallel_readers_stay_consistent() -> None:
    load_module()
    hwmon_dir = find_hwmon_dir()
    assert hwmon_dir is not None

    deadline = time.monotonic() + DURATION_SEC

    def hammer() -> int:
        reads = 0
        while time.monotonic() < deadline:
            for reading in read_hwmon_pins(hwmon_dir):
                assert reading.millivolts in PLAUSIBLE_MV, (
                    f"pin {reading.pin} published {reading.millivolts} mV"
                )
                assert reading.milliamps in PLAUSIBLE_MA, (
                    f"pin {reading.pin} published {reading.milliamps} mA"
                )
            reads += 1
        return reads

    with concurrent.futures.ThreadPoolExecutor(max_workers=READERS) as pool:
        futures = [pool.submit(hammer) for _ in range(READERS)]
        total = sum(f.result(timeout=DURATION_SEC * 4) for f in futures)

    # A deadlock shows up as a timeout above; a livelock as a tiny count here.
    assert total > READERS * 10, f"only {total} reads across {READERS} threads"
