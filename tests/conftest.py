# SPDX-License-Identifier: MIT
"""Hardware tests skip rather than fail when the card is absent.

Borrowed from olduvai: a missing prerequisite must keep the suite green, so the
same command works on the rig and on a laptop.
"""

import pytest


@pytest.fixture(scope="session")
def astral_adapter() -> int:
    # Imported lazily so collection works before the module exists, and so a
    # machine with no card never pays for the import.
    from astral_oracle.discover import find_astral_adapter

    adapter = find_astral_adapter()
    if adapter is None:
        pytest.skip("no supported ASUS ROG Astral card found on this machine")
    return adapter
