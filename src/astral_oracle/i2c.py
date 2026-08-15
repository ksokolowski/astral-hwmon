# SPDX-License-Identifier: MIT
"""Direct i2c-dev access, independent of the kernel driver under test.

Uses I2C_SMBUS_I2C_BLOCK_DATA. This matters: on the NVIDIA adapter a plain
repeated-START multi-byte read (I2C_RDWR with a write message followed by a
read message) returns all zeros silently. Measured 2026-08-13.
"""

from __future__ import annotations

import ctypes
import fcntl
import os

from .decode import BLOCK_LEN, REG_BASE, PinReading, decode_frame

I2C_SLAVE = 0x0703
I2C_SMBUS = 0x0720
I2C_SMBUS_READ = 1
I2C_SMBUS_I2C_BLOCK_DATA = 8
I2C_SMBUS_BLOCK_MAX = 32

DEFAULT_ADDR = 0x2B  # IT8915FN on Astral cards; verified against LibreHardwareMonitor


class I2cError(OSError):
    """A transaction on the I2C bus failed."""


class _SmbusData(ctypes.Union):
    _fields_ = (
        ("byte", ctypes.c_uint8),
        ("word", ctypes.c_uint16),
        ("block", ctypes.c_uint8 * (I2C_SMBUS_BLOCK_MAX + 2)),
    )


class _SmbusIoctlData(ctypes.Structure):
    _fields_ = (
        ("read_write", ctypes.c_uint8),
        ("command", ctypes.c_uint8),
        ("size", ctypes.c_uint32),
        ("data", ctypes.POINTER(_SmbusData)),
    )


def read_block(
    bus: int, addr: int = DEFAULT_ADDR, reg: int = REG_BASE, length: int = BLOCK_LEN
) -> bytes:
    """Read `length` bytes starting at `reg` in one atomic transaction."""
    if not 0 < length <= I2C_SMBUS_BLOCK_MAX:
        raise ValueError(f"length must be 1..{I2C_SMBUS_BLOCK_MAX}, got {length}")

    try:
        fd = os.open(f"/dev/i2c-{bus}", os.O_RDWR)
    except OSError as exc:
        raise I2cError(f"cannot open /dev/i2c-{bus}: {exc}") from exc

    try:
        try:
            fcntl.ioctl(fd, I2C_SLAVE, addr)
        except OSError as exc:
            raise I2cError(f"cannot select address {addr:#04x}: {exc}") from exc

        data = _SmbusData()
        data.block[0] = length
        msg = _SmbusIoctlData(
            read_write=I2C_SMBUS_READ,
            command=reg,
            size=I2C_SMBUS_I2C_BLOCK_DATA,
            data=ctypes.pointer(data),
        )
        try:
            fcntl.ioctl(fd, I2C_SMBUS, msg)
        except OSError as exc:
            raise I2cError(f"block read at {reg:#04x} failed: {exc}") from exc

        # block[0] is in/out: we set the requested count, the kernel writes back
        # what it actually produced. i2c_smbus_read_i2c_block_data() returns the
        # same number, which is what astral_regs.c compares against
        # ASTRAL_BLOCK_LEN. Without this check the oracle would decode a
        # part-filled buffer, leaving it laxer than the driver it exists to
        # judge - and the trailing bytes are whatever the last read left there.
        produced = data.block[0]
        if produced != length:
            raise I2cError(f"block read at {reg:#04x} returned {produced} bytes, expected {length}")

        return bytes(data.block[1 : length + 1])
    finally:
        os.close(fd)


def read_pins(bus: int, addr: int = DEFAULT_ADDR) -> list[PinReading]:
    """One validated sample of all six pins."""
    return decode_frame(read_block(bus, addr))
