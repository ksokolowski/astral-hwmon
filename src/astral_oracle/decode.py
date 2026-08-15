# SPDX-License-Identifier: MIT
"""Pure decoder for the IT8915 pin block on ASUS ROG Astral cards.

This is one of two independent implementations of the same transformation; the
other is driver/astral_regs.c. They exist to check each other, so this module
must never import from, or be generated from, the driver source.

Layout established by direct measurement on a ROG Astral RTX 5090 OC
(0x89E31043) on 2026-08-13, and consistent with the publicly documented
behaviour of this chip (LibreHardwareMonitor's Astral pin sensors, and
Timic3/astral-power-monitoring):
  24 bytes = 6 blocks x (u16 BE millivolts, u16 BE milliamperes)
  Blocks are in REVERSE pin order - Pin1 at offset 20, Pin6 at offset 0.
"""

from __future__ import annotations

from dataclasses import dataclass

REG_BASE = 0x80
BLOCK_LEN = 24
PIN_COUNT = 6

# A garbage read must never reach a consumer as a safe-looking value. The
# all-zeros frame produced by a repeated-START read is the motivating case.
#
# The floor is 6 V rather than a tight 11 V because a degrading connector drops
# pin voltage under load: a tight floor would silence the reading precisely when
# it matters. The lowest pin measured under a 562 W burn was 11944 mV. Nothing
# below half of nominal is a working 12 V rail, so this still catches garbage.
PLAUSIBLE_MV = range(6000, 13001)
PLAUSIBLE_MA = range(0, 30001)


class FrameError(ValueError):
    """The frame was malformed or physically implausible."""


@dataclass(frozen=True)
class PinReading:
    pin: int
    millivolts: int
    milliamps: int


def _u16_be(raw: bytes, offset: int) -> int:
    return (raw[offset] << 8) | raw[offset + 1]


def decode_frame(raw: bytes) -> list[PinReading]:
    """Decode one register block into six readings, pin 1 first."""
    if len(raw) < BLOCK_LEN:
        raise FrameError(f"frame is {len(raw)} bytes, need at least {BLOCK_LEN}")

    readings = []
    for pin in range(1, PIN_COUNT + 1):
        base = (PIN_COUNT - pin) * 4
        millivolts = _u16_be(raw, base)
        milliamps = _u16_be(raw, base + 2)
        if millivolts not in PLAUSIBLE_MV or milliamps not in PLAUSIBLE_MA:
            raise FrameError(f"implausible sample on pin {pin}: {millivolts} mV, {milliamps} mA")
        readings.append(PinReading(pin=pin, millivolts=millivolts, milliamps=milliamps))
    return readings
