// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/test_regs.c - the shipped decoder against the captured corpus.
 *
 * tests/unit/test_decode.py runs the same frames through the Python mirror.
 * This runs them through driver/astral_regs.c - the code that actually ships -
 * so a decode change is caught without hardware on either side of the pair.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "astral.h"
#include "harness.h"

static struct astral_corpus corpus;

static void fill_frame(struct astral_frame *frame, u16 mv, u16 ma)
{
	int pin;

	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		frame->millivolts[pin] = mv;
		frame->milliamps[pin] = ma;
	}
}

/*
 * Guards the tier against passing vacuously. A corpus the scanner failed to
 * read, or one that lost its reject cases, would otherwise leave every other
 * test in this file iterating over nothing and reporting success.
 */
static void test_corpus_has_cases_of_both_kinds(void)
{
	CHECK(corpus.count > 0, "corpus is empty");
	CHECK(corpus.accepts > 0, "corpus has no frames that should decode");
	CHECK(corpus.rejects > 0, "corpus has no frames that should be rejected");
}

/* Captures are taken with `i2cget -y 4 0x2b 0x80 i 32`, so a frame carries the
 * 24-byte pin block plus whatever follows it. Both decoders read the first 24
 * and ignore the rest - decode.py requires len(raw) >= BLOCK_LEN for the same
 * reason. All that matters here is that a whole block is present.
 */
static void test_corpus_frames_carry_a_whole_block(void)
{
	size_t i;

	for (i = 0; i < corpus.count; i++)
		CHECK(corpus.cases[i].raw_len >= ASTRAL_BLOCK_LEN,
		      "line %d: raw is %zu bytes, need at least %d",
		      corpus.cases[i].line, corpus.cases[i].raw_len,
		      ASTRAL_BLOCK_LEN);
}

static void test_corpus_frames_decode(void)
{
	size_t i;
	int pin;

	for (i = 0; i < corpus.count; i++) {
		const struct astral_corpus_case *c = &corpus.cases[i];
		struct astral_frame frame;

		if (c->reject)
			continue;

		astral_decode_frame(c->raw, &frame);

		for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
			CHECK(frame.millivolts[pin] == c->expect_mv[pin],
			      "line %d pin %d: %u mV, expected %d",
			      c->line, pin + 1, frame.millivolts[pin],
			      c->expect_mv[pin]);
			CHECK(frame.milliamps[pin] == c->expect_ma[pin],
			      "line %d pin %d: %u mA, expected %d",
			      c->line, pin + 1, frame.milliamps[pin],
			      c->expect_ma[pin]);
		}

		CHECK(astral_frame_plausible(&frame),
		      "line %d: a captured healthy frame was called implausible",
		      c->line);
	}
}

static void test_corpus_rejects_are_implausible(void)
{
	size_t i;

	for (i = 0; i < corpus.count; i++) {
		const struct astral_corpus_case *c = &corpus.cases[i];
		struct astral_frame frame;

		if (!c->reject)
			continue;

		astral_decode_frame(c->raw, &frame);
		CHECK(!astral_frame_plausible(&frame),
		      "line %d: %s was accepted", c->line, c->note);
	}
}

/* Pin 1 lives at offset 20 and pin 6 at offset 0 - the mirror of
 * test_pin_order_is_reversed in tests/unit/test_decode.py.
 */
static void test_pin_order_is_reversed(void)
{
	u8 raw[ASTRAL_BLOCK_LEN];
	struct astral_frame frame;
	int pin;

	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		raw[pin * 4 + 0] = 0x2f;
		raw[pin * 4 + 1] = 0x88;
		raw[pin * 4 + 2] = 0x01;
		raw[pin * 4 + 3] = 0xcc;
	}
	raw[23] = 0xf4; /* distinct current in the pin 1 block */

	astral_decode_frame(raw, &frame);
	CHECK(frame.milliamps[0] == 500, "pin 1 read %u mA, expected 500",
	      frame.milliamps[0]);
	CHECK(frame.milliamps[5] == 460, "pin 6 read %u mA, expected 460",
	      frame.milliamps[5]);
}

static void test_big_endian_parsing(void)
{
	u8 raw[ASTRAL_BLOCK_LEN];
	struct astral_frame frame;
	int pin;

	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		raw[pin * 4 + 0] = 0x2f;
		raw[pin * 4 + 1] = 0x88;
		raw[pin * 4 + 2] = 0x01;
		raw[pin * 4 + 3] = 0xcc;
	}

	astral_decode_frame(raw, &frame);
	CHECK(frame.millivolts[0] == 0x2f88,
	      "big-endian voltage read as %#x", frame.millivolts[0]);
	CHECK(frame.milliamps[0] == 0x01cc,
	      "big-endian current read as %#x", frame.milliamps[0]);
}

/* The strictness the Python oracle currently lacks: a transfer that returns
 * fewer bytes than asked for must not be decoded from a part-filled buffer.
 */
static void test_short_read_is_rejected(void)
{
	u8 raw[ASTRAL_BLOCK_LEN];
	struct astral_frame frame;
	struct i2c_client client;
	int ret;

	memset(&client, 0, sizeof(client));
	memset(raw, 0x2f, sizeof(raw));

	astral_fake_i2c_set(raw, sizeof(raw), ASTRAL_BLOCK_LEN - 1);
	ret = astral_read_frame(&client, &frame);
	CHECK(ret == -EIO, "short read returned %d, expected -EIO", ret);
}

static void test_transport_error_propagates(void)
{
	struct astral_frame frame;
	struct i2c_client client;
	int ret;

	memset(&client, 0, sizeof(client));

	astral_fake_i2c_set(NULL, 0, -ENXIO);
	ret = astral_read_frame(&client, &frame);
	CHECK(ret == -ENXIO, "transport error became %d, expected -ENXIO", ret);
}

static void test_full_read_decodes(void)
{
	const struct astral_corpus_case *c = NULL;
	struct astral_frame frame;
	struct i2c_client client;
	size_t i;
	int ret;

	for (i = 0; i < corpus.count && !c; i++) {
		if (!corpus.cases[i].reject)
			c = &corpus.cases[i];
	}

	CHECK(c != NULL, "no accepted case to drive the read path with");
	if (!c)
		return;

	memset(&client, 0, sizeof(client));
	memset(&frame, 0, sizeof(frame));

	astral_fake_i2c_set(c->raw, c->raw_len, ASTRAL_BLOCK_LEN);
	ret = astral_read_frame(&client, &frame);
	CHECK(ret == 0, "full read returned %d, expected 0", ret);
	CHECK(astral_fake_i2c.calls == 1, "expected exactly one transfer, saw %d",
	      astral_fake_i2c.calls);
	CHECK(frame.millivolts[0] == c->expect_mv[0],
	      "read path decoded pin 1 as %u mV, expected %d",
	      frame.millivolts[0], c->expect_mv[0]);
}

static void test_plausibility_bounds(void)
{
	struct astral_frame frame;
	int pin;

	fill_frame(&frame, 12168, 460);
	CHECK(astral_frame_plausible(&frame), "a nominal idle frame was rejected");

	fill_frame(&frame, 0, 0);
	CHECK(!astral_frame_plausible(&frame),
	      "the all-zeros frame was accepted");

	fill_frame(&frame, ASTRAL_MV_MIN, 0);
	CHECK(astral_frame_plausible(&frame), "ASTRAL_MV_MIN is not inclusive");

	fill_frame(&frame, ASTRAL_MV_MAX, 0);
	CHECK(astral_frame_plausible(&frame), "ASTRAL_MV_MAX is not inclusive");

	fill_frame(&frame, 12168, ASTRAL_MA_MAX);
	CHECK(astral_frame_plausible(&frame), "ASTRAL_MA_MAX is not inclusive");

	/* The point of the floor being loose. A connector on its way out drops
	 * pin voltage under load; if that reading silences the frame, it takes
	 * all six currents with it and the driver goes quiet exactly when it
	 * should be shouting. A deeply sagging pin must still be publishable.
	 */
	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		fill_frame(&frame, 12168, 8000);
		frame.millivolts[pin] = 9000;
		CHECK(astral_frame_plausible(&frame),
		      "pin %d sagging to 9.0 V under load silenced the frame",
		      pin + 1);
	}

	/* Every pin, not just pin 0: a loop that stopped early would pass an
	 * out-of-range reading on any later pin.
	 */
	for (pin = 0; pin < ASTRAL_PIN_COUNT; pin++) {
		fill_frame(&frame, 12168, 460);
		frame.millivolts[pin] = ASTRAL_MV_MIN - 1;
		CHECK(!astral_frame_plausible(&frame),
		      "pin %d under ASTRAL_MV_MIN was accepted", pin + 1);

		fill_frame(&frame, 12168, 460);
		frame.millivolts[pin] = ASTRAL_MV_MAX + 1;
		CHECK(!astral_frame_plausible(&frame),
		      "pin %d over ASTRAL_MV_MAX was accepted", pin + 1);

		fill_frame(&frame, 12168, 460);
		frame.milliamps[pin] = ASTRAL_MA_MAX + 1;
		CHECK(!astral_frame_plausible(&frame),
		      "pin %d over ASTRAL_MA_MAX was accepted", pin + 1);
	}
}

void run_regs_tests(const char *corpus_path)
{
	astral_corpus_load(&corpus, corpus_path);
	printf("astral_regs.c (%zu corpus frames: %zu decode, %zu reject)\n",
	       corpus.count, corpus.accepts, corpus.rejects);

	RUN(test_corpus_has_cases_of_both_kinds);
	RUN(test_corpus_frames_carry_a_whole_block);
	RUN(test_corpus_frames_decode);
	RUN(test_corpus_rejects_are_implausible);
	RUN(test_pin_order_is_reversed);
	RUN(test_big_endian_parsing);
	RUN(test_short_read_is_rejected);
	RUN(test_transport_error_propagates);
	RUN(test_full_read_decodes);
	RUN(test_plausibility_bounds);
}
