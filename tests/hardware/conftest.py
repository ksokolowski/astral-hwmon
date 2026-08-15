# SPDX-License-Identifier: MIT
"""Hardware tests skip as a whole when the card is absent.

This is autouse on purpose. The previous arrangement skipped only tests that
happened to request the `astral_adapter` fixture, which left nine of fifteen
running `sudo modprobe` on machines with no card - failing rather than
skipping, and contradicting the rule this suite is built on.
"""

import pytest

from astral_oracle.discover import find_astral_adapter
from astral_oracle.load import BURN_BIN


@pytest.fixture(scope="session", autouse=True)
def require_astral_card() -> None:
    if find_astral_adapter() is None:
        pytest.skip(
            "no supported ASUS ROG Astral card on this machine",
            allow_module_level=False,
        )


@pytest.fixture(scope="session")
def burn_available() -> None:
    """Skip tests that need the CUDA load generator when it has not been built.

    `make burn` is a separate step from `make setup` because it needs nvcc, so
    a card-equipped machine can legitimately be missing it. Skipping is right
    here for the same reason the whole tier skips without a card: the test did
    not fail, it could not run.
    """
    if not BURN_BIN.exists():
        pytest.skip(f"{BURN_BIN} not built - run `make burn`")
