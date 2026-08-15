// SPDX-License-Identifier: GPL-2.0-only
/* driver/astral_regs.c - block read and decode for the IT8915 pin block.
 *
 * This is one of two independent implementations of the same transformation;
 * the other is src/astral_oracle/decode.py. They exist to check each other on
 * real hardware, so neither may be derived from the other.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>

#include "astral.h"

static u16 astral_u16_be(const u8 *buf, int offset)
{
	return ((u16)buf[offset] << 8) | buf[offset + 1];
}

/*
 * Blocks are stored in REVERSE pin order: pin 1 at offset 20, pin 6 at 0.
 * Established by measurement on a ROG Astral RTX 5090 OC, 2026-08-13.
 *
 * Split out from the transfer so the shipped decoder itself can be run against
 * tests/data/frames.jsonl on a host build - see tests/native. `buf` must hold
 * ASTRAL_BLOCK_LEN bytes; validating that is the caller's job, because on the
 * wire a short read is a transport failure rather than a decode one.
 */
void astral_decode_frame(const u8 *buf, struct astral_frame *out)
{
	int pin, base;

	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		base = (ASTRAL_PIN_COUNT - 1 - pin) * 4;
		out->millivolts[pin] = astral_u16_be(buf, base);
		out->milliamps[pin] = astral_u16_be(buf, base + 2);
	}
}

int astral_read_frame(struct i2c_client *client, struct astral_frame *out)
{
	u8 buf[ASTRAL_BLOCK_LEN];
	int ret;

	/* I2C_SMBUS_I2C_BLOCK_DATA specifically: a repeated-START multi-byte
	 * read returns all zeros on this adapter.
	 */
	ret = i2c_smbus_read_i2c_block_data(client, ASTRAL_REG_BASE,
					    ASTRAL_BLOCK_LEN, buf);
	if (ret < 0)
		return ret;
	if (ret != ASTRAL_BLOCK_LEN)
		return -EIO;

	astral_decode_frame(buf, out);

	return 0;
}

bool astral_frame_plausible(const struct astral_frame *frame)
{
	int pin;

	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		if (frame->millivolts[pin] < ASTRAL_MV_MIN ||
		    frame->millivolts[pin] > ASTRAL_MV_MAX)
			return false;
		if (frame->milliamps[pin] > ASTRAL_MA_MAX)
			return false;
	}

	return true;
}
