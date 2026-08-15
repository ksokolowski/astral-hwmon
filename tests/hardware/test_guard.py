# SPDX-License-Identifier: MIT
"""The guard against the real card.

Everything else about the guard is proven without hardware. This asserts the
one thing a fake sysfs cannot: that the rules stay quiet on a connector known
to be healthy, reading the same node `sensors` reads.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from typing import Any

import pytest

from astral_oracle.hwmon import load_module

pytestmark = pytest.mark.hardware

GUARD = Path(__file__).resolve().parents[2] / "guard" / "astral-guard"


@pytest.fixture(scope="module")
def guard_json() -> list[dict[str, Any]]:
    if not GUARD.exists():
        pytest.skip(f"{GUARD} not built - run `make guard`")
    # test_gate.py deliberately leaves the module unloaded, so every test that
    # wants the sysfs node loads it itself. `modprobe && sensors` works without
    # a race by contract, so there is nothing to wait for here.
    load_module()
    out = subprocess.run(
        [str(GUARD), "--json", "--samples", "3"],
        capture_output=True,
        text=True,
        timeout=30,
    )
    # 3 is UNKNOWN - no chip found, or a read failed. On a machine that got
    # past the autouse card fixture that is a real failure, not a skip.
    assert out.returncode in (0, 1, 2), f"unexpected exit {out.returncode}: {out.stderr}"
    parsed: list[dict[str, Any]] = json.loads(out.stdout)
    return parsed


def test_guard_reports_ok_on_a_healthy_card(guard_json: list[dict[str, Any]]) -> None:
    assert len(guard_json) == 1, "expected exactly one chip"
    assert guard_json[0]["level"] == "OK", guard_json[0]["findings"]


def test_guard_reads_the_same_node_as_sysfs(guard_json: list[dict[str, Any]]) -> None:
    """The guard must find the driver's node, not some other hwmon chip."""
    chip = Path(guard_json[0]["chip"])
    assert (chip / "name").read_text().strip() == "astral12vhpwr"
