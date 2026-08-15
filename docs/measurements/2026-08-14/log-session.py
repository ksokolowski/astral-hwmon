#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Log the astral-hwmon pins alongside GPU telemetry, for as long as it runs.

Written to survive the session that started it: no dependency on the launching
shell, no output under /tmp. Run it under `systemd-run --user` and it keeps
going across logouts.

Two things it deliberately records rather than hides:

  * a read returning -EIO. That is the driver's plausibility gate rejecting a
    frame, and it is a data point, not an error to swallow.
  * the pin spread. Absolute current says how hard the card is working; the
    imbalance between pins is what a degrading 12VHPWR connector shows first.
"""

from __future__ import annotations

import argparse
import csv
import os
import signal
import subprocess
import sys
import time
from pathlib import Path

HWMON_ROOT = Path("/sys/class/hwmon")
CHIP_NAME = "astral12vhpwr"
PINS = 6

NVIDIA_FIELDS = [
    "power.draw",
    "temperature.gpu",
    "utilization.gpu",
    "clocks.sm",
    "clocks.mem",
    "temperature.memory",
]


def find_chip() -> Path:
    for entry in sorted(HWMON_ROOT.iterdir()):
        try:
            if (entry / "name").read_text().strip() == CHIP_NAME:
                return entry
        except OSError:
            continue
    raise SystemExit(f"no {CHIP_NAME} hwmon chip - is astral-hwmon loaded?")


class PinReader:
    """Holds the sysfs files open and re-reads them by seeking to 0.

    Reopening twelve files twice a second for hours is pointless churn; sysfs
    attributes are re-evaluated on every read after a seek.
    """

    def __init__(self, chip: Path) -> None:
        self.chip = chip
        # in0..in5 are pins 1..6, curr1..curr6 are pins 1..6. hwmon numbers
        # voltages from zero and currents from one; this is not a typo.
        self._volt = [open(chip / f"in{p}_input", "rb") for p in range(PINS)]
        self._curr = [open(chip / f"curr{p + 1}_input", "rb") for p in range(PINS)]

    def read(self) -> tuple[list[int] | None, list[int] | None, str]:
        try:
            mv = [self._read_one(f) for f in self._volt]
            ma = [self._read_one(f) for f in self._curr]
        except OSError as exc:
            # EIO here is the driver refusing to publish an implausible frame.
            return None, None, f"{type(exc).__name__}:{exc.errno}"
        return mv, ma, ""

    @staticmethod
    def _read_one(handle) -> int:  # noqa: ANN001
        handle.seek(0)
        return int(handle.read().strip())


class NvidiaStream:
    """nvidia-smi in streaming mode, so we are not spawning it every sample."""

    def __init__(self, interval_ms: int) -> None:
        self.proc = subprocess.Popen(
            [
                "nvidia-smi",
                f"--query-gpu={','.join(NVIDIA_FIELDS)}",
                "--format=csv,noheader,nounits",
                f"-lms={interval_ms}",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        self.latest = [""] * len(NVIDIA_FIELDS)
        os.set_blocking(self.proc.stdout.fileno(), False)

    def poll(self) -> list[str]:
        while True:
            line = self.proc.stdout.readline()
            if not line:
                break
            parts = [p.strip() for p in line.split(",")]
            if len(parts) == len(NVIDIA_FIELDS):
                self.latest = parts
        return self.latest

    def stop(self) -> None:
        self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--hz", type=float, default=2.0)
    ap.add_argument("--max-hours", type=float, default=6.0)
    args = ap.parse_args()

    chip = find_chip()
    reader = PinReader(chip)
    nvidia = NvidiaStream(interval_ms=500)

    stopping = False

    def handle(signum, frame):  # noqa: ANN001, ARG001
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGTERM, handle)
    signal.signal(signal.SIGINT, handle)

    period = 1.0 / args.hz
    deadline = time.monotonic() + args.max_hours * 3600
    rows = 0
    errors = 0

    with args.out.open("w", newline="") as fh:
        # csv.writer defaults to RFC 4180's \r\n. That leaves a stray CR in the
        # last field of every row, so awk/cut analysis sees a trailing column
        # that is one character long instead of empty - which is exactly how a
        # "no error" column got misread as an error. Unix line endings here.
        writer = csv.writer(fh, lineterminator="\n")
        writer.writerow(
            ["epoch", "iso", "elapsed"]
            + [f"mv{p + 1}" for p in range(PINS)]
            + [f"ma{p + 1}" for p in range(PINS)]
            + ["ma_sum", "ma_max", "ma_min", "ma_spread", "mv_min"]
            + ["gpu_w", "gpu_c", "gpu_util", "sm_mhz", "mem_mhz", "mem_c"]
            + ["read_error"]
        )

        started = time.monotonic()
        while not stopping and time.monotonic() < deadline:
            tick = time.monotonic()
            mv, ma, err = reader.read()
            gpu = nvidia.poll()
            now = time.time()

            if ma is None or mv is None:
                errors += 1
                writer.writerow(
                    [f"{now:.3f}", time.strftime("%Y-%m-%dT%H:%M:%S"), f"{tick - started:.3f}"]
                    + [""] * (PINS * 2 + 5)
                    + gpu
                    + [err]
                )
            else:
                writer.writerow(
                    [f"{now:.3f}", time.strftime("%Y-%m-%dT%H:%M:%S"), f"{tick - started:.3f}"]
                    + mv
                    + ma
                    + [sum(ma), max(ma), min(ma), max(ma) - min(ma), min(mv)]
                    + gpu
                    + [""]
                )
            rows += 1
            # Flush every ~5 s so the file is useful while it is still running
            # and survives an abrupt end.
            if rows % (int(args.hz) * 5 or 1) == 0:
                fh.flush()
                os.fsync(fh.fileno())

            sleep = period - (time.monotonic() - tick)
            if sleep > 0:
                time.sleep(sleep)

        fh.flush()
        os.fsync(fh.fileno())

    nvidia.stop()
    print(f"wrote {rows} rows ({errors} read errors) to {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
