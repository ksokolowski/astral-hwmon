# SPDX-License-Identifier: MIT
"""End-to-end tests for the astral-guard binary against a fake sysfs tree.

The guard's own C tier tests the rule engine and the sysfs reader as library
functions; `main()` is not linked into it. These cover the process boundary
instead: the exit code the caller actually sees, and above all that a guard
which cannot find the chip reports UNKNOWN rather than OK. Silent success is
the one failure mode this tool must never have - an operator reading "OK" from
a machine where the module never loaded is worse off than one reading nothing.

The binary is built by `make guard`, which `make qa` depends on. `make test`
does not, so this skips there and the unit tier stays dependency-free.
"""

from __future__ import annotations

import json
import subprocess
from pathlib import Path

import pytest

GUARD = Path(__file__).resolve().parents[2] / "guard" / "astral-guard"

# Idle currents measured on the reference card, docs/measurements/2026-08-14.
HEALTHY_MV = [11960, 11944, 11952, 11944, 11944, 11944]
HEALTHY_MA = [1180, 1220, 1260, 1200, 1240, 1210]


def _write_chip(root: Path, index: int, name: str, mv: list[int], ma: list[int]) -> Path:
    """Build one hwmon node the way the kernel lays it out.

    The asymmetric numbering is the point: voltages are in0-in5, currents are
    curr1-curr6. A reader that assumes in1-in6 finds nothing here.
    """
    chip = root / "class" / "hwmon" / f"hwmon{index}"
    chip.mkdir(parents=True)
    (chip / "name").write_text(f"{name}\n")
    (chip / "update_interval").write_text("200\n")
    for pin in range(6):
        (chip / f"in{pin}_input").write_text(f"{mv[pin]}\n")
        (chip / f"curr{pin + 1}_input").write_text(f"{ma[pin]}\n")
    return chip


@pytest.fixture(autouse=True)
def require_guard_binary() -> None:
    if not GUARD.exists():
        pytest.skip(f"{GUARD} not built - run `make guard`")


def _run(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(GUARD), "--sysfs-root", str(root), "--samples", "2", "--interval", "1", *args],
        capture_output=True,
        text=True,
        timeout=30,
    )


def test_no_chip_is_unknown_not_ok(tmp_path: Path) -> None:
    """The mutation this exists to catch: reporting OK when nothing was read."""
    (tmp_path / "class" / "hwmon").mkdir(parents=True)
    out = _run(tmp_path)
    assert out.returncode == 3, out.stdout + out.stderr
    assert "UNKNOWN" in out.stderr


def test_a_foreign_chip_is_not_mistaken_for_ours(tmp_path: Path) -> None:
    _write_chip(tmp_path, 3, "nct6799", HEALTHY_MV, HEALTHY_MA)
    out = _run(tmp_path)
    assert out.returncode == 3, out.stdout + out.stderr


def test_a_healthy_chip_is_ok(tmp_path: Path) -> None:
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, HEALTHY_MA)
    out = _run(tmp_path)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "OK" in out.stdout


def test_a_pin_over_its_rating_exits_critical(tmp_path: Path) -> None:
    ma = [7640, 7840, 8300, 7920, 9600, 8020]
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, ma)
    out = _run(tmp_path, "--json")
    assert out.returncode == 2, out.stdout + out.stderr

    report = json.loads(out.stdout)
    assert report[0]["level"] == "CRITICAL"
    assert any(f["rule"] == "pin-current" for f in report[0]["findings"]), report[0]


def test_quiet_prints_nothing_when_ok(tmp_path: Path) -> None:
    """cron's contract: mail only when something is wrong."""
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, HEALTHY_MA)
    out = _run(tmp_path, "--quiet")
    assert out.returncode == 0
    assert out.stdout == ""


def _run_raw(root: Path, *args: str) -> subprocess.CompletedProcess[str]:
    """Without the fixed --samples/--interval _run() adds, so args can be tested."""
    return subprocess.run(
        [str(GUARD), "--sysfs-root", str(root), *args],
        capture_output=True,
        text=True,
        timeout=30,
    )


@pytest.mark.parametrize("value", ["abc", "", "70%", "0", "101", "-1"])
def test_a_bad_ratio_never_silently_disarms_the_rule(tmp_path: Path, value: str) -> None:
    """The mutation this exists to catch: accepting a threshold that cannot fire.

    R3 tests `lo * 100 < hi * pct`, so pct 0 is false for every reading. Under
    the old atoi() parse, any non-numeric --warn-ratio became 0 and the
    imbalance warning was gone with no diagnostic and exit 0. Refusing the
    argument is the only outcome that cannot be mistaken for a clean run.
    """
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, HEALTHY_MA)
    out = _run_raw(tmp_path, "--samples", "1", "--warn-ratio", value)
    assert out.returncode == 3, out.stdout + out.stderr
    assert "--warn-ratio" in out.stderr
    assert value in out.stderr or value == ""


@pytest.mark.parametrize(
    ("option", "value"),
    [
        ("--samples", "abc"),
        ("--samples", "0"),
        ("--samples", "33"),
        ("--interval", "abc"),
        ("--interval", "-1"),
        ("--note-ratio", "abc"),
    ],
)
def test_a_malformed_option_is_unknown_not_a_default(
    tmp_path: Path, option: str, value: str
) -> None:
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, HEALTHY_MA)
    out = _run_raw(tmp_path, "--samples", "1", option, value)
    assert out.returncode == 3, out.stdout + out.stderr
    assert option in out.stderr


def test_the_json_limits_carry_every_threshold_the_verdict_used(tmp_path: Path) -> None:
    """The man page promises "the thresholds that were applied" - all of them.

    The two gates matter most to a reader: an absent imbalance finding means
    nothing unless the report shows whether the run ever cleared ratio_gate_ma.
    """
    _write_chip(tmp_path, 7, "astral12vhpwr", HEALTHY_MV, HEALTHY_MA)
    out = _run_raw(tmp_path, "--samples", "1", "--json")
    assert out.returncode == 0, out.stdout + out.stderr

    limits = json.loads(out.stdout)[0]["limits"]
    assert set(limits) == {
        "pin_warn_ma",
        "pin_crit_ma",
        "open_pin_ma",
        "open_gate_ma",
        "ratio_gate_ma",
        "ratio_note_pct",
        "ratio_warn_pct",
        "ratio_crit_pct",
        "mv_warn",
        "mv_crit",
    }, limits
