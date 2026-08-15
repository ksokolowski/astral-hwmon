# Reference data, 2026-08-13

Captured on the reference machine (ROG Astral RTX 5090 OC `0x89E31043`, chip on `i2c-4`
at `0x2b`) during the session that established the register layout.

## `idle-raw-frames.txt`

`elapsed,watts,util|<32 raw bytes as 0xNN>`

Raw block reads, undecoded. This is the authoritative file: it carries the bytes exactly
as the chip returned them, so it can be re-decoded by either implementation at any time
and is immune to the column-order problem described below.

## `load-decoded.txt`

`elapsed|watts, sm_mhz, util|<6 currents mA>|<6 voltages mV>|sum_mA|min_mV`

**The six current and voltage columns are in RAW BLOCK ORDER — pin 6 first, pin 1 last.**
They are *not* pin 1..6. The register block stores pins in reverse (pin 1 at offset 20,
pin 6 at offset 0) and this file was written straight from that layout without reordering.

Reading it as pin 1..6 silently transposes the pins. It is not a decode that fails; it is
a decode that produces plausible numbers attributed to the wrong pins, which is precisely
the failure mode `test_pin_mapping_is_not_transposed` exists to catch in the driver.

Verified 2026-08-14 by decoding `idle-raw-frames.txt` with `astral_oracle.decode` and
comparing per-pin current share: the raw frames put pin 5 highest at 18.12% and pin 4
lowest at 15.66%, matching a fresh 40-minute capture on the same card (18.07% / 15.83%).
`load-decoded.txt` only agrees once its columns are reversed.

## `burn.cu.orig`

The CUDA load generator as it stood for this session; `tools/burn.cu` is its descendant.

## Known per-pin current share on this card

Stable across days, reboots and workloads, in the loaded regime:

| pin | share |
|-----|-------|
| 1 | 16.1% |
| 2 | 16.5% |
| 3 | 16.5% |
| 4 | 15.8% |
| 5 | 18.1% |
| 6 | 16.9% |

Even sharing would be 16.67%. The spread reflects contact and trace resistance and is a
property of this card and this cable seating, not a fault.

**Only meaningful under load.** At idle the currents are a few hundred mA and the 20 mA
quantisation step is ~4% of a pin's reading, so the shares scatter far more than the
underlying imbalance. Any check built on this must be gated on a minimum total current.
