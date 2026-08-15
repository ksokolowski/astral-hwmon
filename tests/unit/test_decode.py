# SPDX-License-Identifier: MIT
import json
from pathlib import Path

import pytest

from astral_oracle.decode import BLOCK_LEN, FrameError, decode_frame

CORPUS = Path(__file__).resolve().parents[1] / "data" / "frames.jsonl"


def load_corpus() -> list[dict]:
    return [json.loads(line) for line in CORPUS.read_text().splitlines() if line.strip()]


@pytest.mark.parametrize("case", [c for c in load_corpus() if not c.get("reject")])
def test_decodes_measured_frames(case: dict) -> None:
    readings = decode_frame(bytes.fromhex(case["raw"]))
    assert [r.pin for r in readings] == [1, 2, 3, 4, 5, 6]
    assert [r.milliamps for r in readings] == case["expect_ma"]
    assert [r.millivolts for r in readings] == case["expect_mv"]


@pytest.mark.parametrize("case", [c for c in load_corpus() if c.get("reject")])
def test_rejects_implausible_frames(case: dict) -> None:
    with pytest.raises(FrameError):
        decode_frame(bytes.fromhex(case["raw"]))


def test_pin_order_is_reversed() -> None:
    """Pin1 lives at offset 20, Pin6 at offset 0 - verified against HWiNFO."""
    raw = bytearray(bytes.fromhex("2f8801cc" * 6))
    raw[20:24] = bytes.fromhex("2f8801f4")  # distinct value in the Pin1 block
    readings = decode_frame(bytes(raw))
    assert readings[0].milliamps == 500
    assert readings[5].milliamps == 460


def test_big_endian_parsing() -> None:
    raw = bytes.fromhex("2f8801cc" * 6)
    assert decode_frame(raw)[0].millivolts == 0x2F88


def test_rejects_short_frame() -> None:
    with pytest.raises(FrameError, match="24"):
        decode_frame(bytes(20))


def test_accepts_long_frame_and_ignores_tail() -> None:
    """A 32-byte read is fine; the 7th block at 0x98 is deliberately unused."""
    raw = bytes.fromhex("2f8801cc" * 6 + "2f80053c" + "00000000")
    assert len(decode_frame(raw)) == 6


def test_rejects_out_of_range_voltage() -> None:
    raw = bytearray(bytes.fromhex("2f8801cc" * 6))
    raw[0:2] = (5000).to_bytes(2, "big")
    with pytest.raises(FrameError, match="implausible"):
        decode_frame(bytes(raw))


def test_block_len_is_24() -> None:
    assert BLOCK_LEN == 24
