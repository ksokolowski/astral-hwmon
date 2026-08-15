#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Map the chip's register space by reading it. Read-only, by construction.

The driver publishes one 32-byte block from 0x80 and nothing else, because that
is the only region whose meaning was established by measurement. This walks the
whole 0x00-0xFF space so the rest can be described rather than guessed at, and
so a specific question can be answered: does this chip carry threshold, limit or
status registers of its own? If it does, a hwmon-native alarm becomes possible
and `docs/GUARD-DESIGN.md` changes. If it does not, the guard stays in userspace.

**This script never writes.** It issues `I2C_SMBUS_I2C_BLOCK_DATA` reads and
nothing else. That matters more than usual here: this is a live GPU's I2C bus,
shared with the RGB controller and whatever else ASUS put on it, and a stray
write to an unknown register on an unknown device is how people brick hardware.
There is no write path in this file; keep it that way.

Two passes, spaced apart, so each 16-byte chunk can be classified:

  * changed between passes -> live measurement
  * static and non-zero    -> identity, configuration or calibration
  * static and all-zero    -> unimplemented, or a region that only fills under
                              conditions this run did not produce
  * no response            -> not backed by anything

Run it twice, once idle and once under load, and diff the classifications: a
register that is static at idle and moving under burn is a measurement channel
that idle simply cannot distinguish from a constant.
"""

from __future__ import annotations

import argparse
import sys
import time

from astral_oracle.i2c import read_block

CHUNK = 16
SPACE = 0x100


def scan(bus: int, addr: int) -> dict[int, bytes | None]:
    """One full pass. A chunk that errors is recorded as None, not fatal."""
    out: dict[int, bytes | None] = {}
    for base in range(0, SPACE, CHUNK):
        try:
            out[base] = read_block(bus, addr, base, CHUNK)
        except OSError:
            out[base] = None
    return out


def be16(buf: bytes, off: int) -> int:
    return (buf[off] << 8) | buf[off + 1]


def describe(base: int, buf: bytes) -> str:
    """The two readings worth trying on any 16 bytes off this chip."""
    words = [be16(buf, i) for i in range(0, CHUNK, 2)]
    pairs = [(be16(buf, i), be16(buf, i + 2)) for i in range(0, CHUNK, 4)]
    return (
        f"    as u16 BE : {' '.join(f'{w:5d}' for w in words)}\n"
        f"    as mV/mA  : {' '.join(f'{v}/{a}' for v, a in pairs)}"
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--bus", type=int, default=4)
    ap.add_argument("--addr", type=lambda s: int(s, 0), default=0x2B)
    ap.add_argument("--gap", type=float, default=3.0, help="seconds between passes")
    ap.add_argument("--decode", action="store_true", help="also print candidate decodes")
    args = ap.parse_args()

    first = scan(args.bus, args.addr)
    time.sleep(args.gap)
    second = scan(args.bus, args.addr)

    live, static_nz, static_zero, dead = [], [], [], []

    for base in sorted(first):
        a, b = first[base], second[base]
        if a is None or b is None:
            dead.append(base)
            print(f"0x{base:02x}  <no response>")
            continue
        tag = ""
        if a != b:
            live.append(base)
            tag = "  CHANGED"
        elif any(a):
            static_nz.append(base)
        else:
            static_zero.append(base)
        print(f"0x{base:02x}  {' '.join(f'{x:02x}' for x in a)}{tag}")
        if args.decode and any(a):
            print(describe(base, a))

    def fmt(xs: list[int]) -> str:
        return " ".join(f"0x{x:02x}" for x in xs) or "(none)"

    print()
    print(f"live (changed between passes) : {fmt(live)}")
    print(f"static, non-zero              : {fmt(static_nz)}")
    print(f"static, all-zero              : {fmt(static_zero)}")
    print(f"no response                   : {fmt(dead)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
