// SPDX-License-Identifier: MIT
/* tests/guard/test_sysfs.c - discovery and reads, against fake sysfs trees. */

#include <stdio.h>
#include <string.h>

#include "astral_guard.h"
#include "check.h"
#include "fakesys.h"

static const int mv[GUARD_PINS] = { 11960, 11944, 11952, 11944, 11944, 11944 };
static const int ma[GUARD_PINS] = { 7640, 7840, 8300, 7920, 8560, 8020 };

static void test_finds_our_chip(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	int n;

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon7", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon7", mv, ma);
	fakesys_set_attr(&fs, "hwmon7", "update_interval", "200");

	n = guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS);
	CHECK(n == 1, "expected exactly one chip, got %d", n);
	CHECK(chips[0].update_interval_ms == 200,
	      "expected update_interval 200, got %d", chips[0].update_interval_ms);

	fakesys_destroy(&fs);
}

/* hwmon10 on the reference machine today, something else tomorrow. Matching on
 * the index rather than the name would find the wrong chip or none at all.
 */
static void test_ignores_other_chips(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	int n;

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon0", "nvme");
	fakesys_add_chip(&fs, "hwmon4", "nct6799");
	fakesys_add_chip(&fs, "hwmon10", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon10", mv, ma);

	n = guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS);
	CHECK(n == 1, "must match on name, not index; got %d chips", n);
	CHECK(strstr(chips[0].path, "hwmon10") != NULL,
	      "must have found hwmon10, got %s", chips[0].path);

	fakesys_destroy(&fs);
}

/* Absence of the chip is a fault state, not quiet success. */
static void test_no_chip_is_not_success(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	int n;

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon0", "nvme");

	n = guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS);
	CHECK(n == 0, "expected zero chips, got %d", n);

	fakesys_destroy(&fs);
}

static void test_unreadable_root_is_an_error(void)
{
	struct guard_chip chips[GUARD_MAX_CHIPS];

	CHECK(guard_find_chips("/nonexistent-sysfs-root", chips,
			       GUARD_MAX_CHIPS) == -1,
	      "an unreadable hwmon class must be -1, distinct from zero chips");
}

static void test_missing_update_interval_defaults(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon2", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon2", mv, ma);

	CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1,
	      "chip must still be found without update_interval");
	CHECK(chips[0].update_interval_ms == GUARD_DEFAULT_INTERVAL_MS,
	      "expected the default interval, got %d",
	      chips[0].update_interval_ms);

	fakesys_destroy(&fs);
}

/* Distinct values per channel, so a reader that mixed up the asymmetric
 * numbering cannot pass by coincidence. Reading in1..in6 instead of in0..in5 is
 * already in this project's table of bugs found the hard way.
 */
static void test_reads_pins_with_the_right_numbering(void)
{
	const int tag_mv[GUARD_PINS] = { 12001, 12002, 12003, 12004, 12005, 12006 };
	const int tag_ma[GUARD_PINS] = { 1001, 1002, 1003, 1004, 1005, 1006 };
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	struct guard_sample s;
	int pin;

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon3", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon3", tag_mv, tag_ma);

	CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1, "chip found");
	CHECK(guard_read_sample(&chips[0], &s) == 0, "sample must read");
	for (pin = 0; pin < GUARD_PINS; pin++) {
		CHECK(s.mv[pin] == tag_mv[pin],
		      "pin %d voltage: expected %d, got %d",
		      pin + 1, tag_mv[pin], s.mv[pin]);
		CHECK(s.ma[pin] == tag_ma[pin],
		      "pin %d current: expected %d, got %d",
		      pin + 1, tag_ma[pin], s.ma[pin]);
	}

	fakesys_destroy(&fs);
}

/* i2c_del_adapter() frees the client on rmmod nvidia, and the hwmon node goes
 * with it. A read after that must fail rather than return stale zeros.
 */
static void test_chip_vanishing_is_a_read_failure(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	struct guard_sample s;

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon3", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon3", mv, ma);
	CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1, "chip found");

	fakesys_destroy(&fs);

	CHECK(guard_read_sample(&chips[0], &s) == -1,
	      "a vanished chip must fail the read, not return stale zeros");
}

/* A half-filled sample would be evaluated as though the missing pin carried
 * nothing, which the open-pin rule would then report as a dead contact.
 */
static void test_a_missing_channel_fails_the_whole_sample(void)
{
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	struct guard_sample s;
	char path[512];

	fakesys_create(&fs);
	fakesys_add_chip(&fs, "hwmon3", GUARD_CHIP_NAME);
	fakesys_set_pins(&fs, "hwmon3", mv, ma);
	CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1, "chip found");

	snprintf(path, sizeof(path), "%s/class/hwmon/hwmon3/curr4_input", fs.root);
	CHECK(remove(path) == 0, "fixture: curr4_input removed");

	CHECK(guard_read_sample(&chips[0], &s) == -1,
	      "a partial read must fail: the missing pin would evaluate as zero");

	fakesys_destroy(&fs);
}

/* Every one of these once read as a healthy channel and the run exited OK.
 * "8000abc" parsed as 8000 because nothing looked past the digits; 4294975296
 * is 2^32 + 8000 and arrived as exactly 8000 because the narrowing from long to
 * int is silent; 9999999999 reached the rules and overflowed them, which UBSan
 * reports as undefined behaviour in guard_eval.
 *
 * A value the driver could not have published is not a measurement, so it has
 * to fail the sample. main() turns that into UNKNOWN, which is the honest
 * answer - and the one thing a guard must never do is report OK because it
 * could not understand what it read.
 */
static void test_an_impossible_reading_is_not_a_healthy_one(void)
{
	static const struct {
		const char *attr;
		const char *value;
	} bad[] = {
		{ "curr1_input", "8000abc" },	 /* trailing junk */
		{ "curr1_input", "4294975296" }, /* 2^32 + 8000 */
		{ "curr1_input", "9999999999" }, /* wider than int */
		{ "curr1_input", "-5000" },	 /* a u16 decode cannot emit this */
		{ "curr1_input", "30001" },	 /* one past the driver's ceiling */
		{ "in0_input", "5999" },	 /* one under the driver's floor */
		{ "in0_input", "13001" },	 /* one over its ceiling */
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		struct fakesys fs;
		struct guard_chip chips[GUARD_MAX_CHIPS];
		struct guard_sample s;

		fakesys_create(&fs);
		fakesys_add_chip(&fs, "hwmon3", GUARD_CHIP_NAME);
		fakesys_set_pins(&fs, "hwmon3", mv, ma);
		fakesys_set_attr(&fs, "hwmon3", bad[i].attr, bad[i].value);

		CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1,
		      "chip must still be found with %s = \"%s\"",
		      bad[i].attr, bad[i].value);
		CHECK(guard_read_sample(&chips[0], &s) != 0,
		      "%s = \"%s\" must fail the sample, not read as a value",
		      bad[i].attr, bad[i].value);

		fakesys_destroy(&fs);
	}
}

/* update_interval only paces the sampling, so an unreadable one falls back to
 * the default rather than failing the run. It still must not be believed: a
 * nonsense value would otherwise become the gap between samples.
 */
static void test_a_nonsense_update_interval_falls_back(void)
{
	static const char *const bad[] = { "abc", "200ms", "-1", "3600001" };
	struct fakesys fs;
	struct guard_chip chips[GUARD_MAX_CHIPS];
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		fakesys_create(&fs);
		fakesys_add_chip(&fs, "hwmon3", GUARD_CHIP_NAME);
		fakesys_set_attr(&fs, "hwmon3", "update_interval", bad[i]);

		CHECK(guard_find_chips(fs.root, chips, GUARD_MAX_CHIPS) == 1,
		      "chip must still be found with update_interval \"%s\"",
		      bad[i]);
		CHECK(chips[0].update_interval_ms == GUARD_DEFAULT_INTERVAL_MS,
		      "update_interval \"%s\": expected the %d ms default, got %d",
		      bad[i], GUARD_DEFAULT_INTERVAL_MS,
		      chips[0].update_interval_ms);

		fakesys_destroy(&fs);
	}
}

void run_sysfs_tests(void)
{
	RUN(test_finds_our_chip);
	RUN(test_ignores_other_chips);
	RUN(test_no_chip_is_not_success);
	RUN(test_unreadable_root_is_an_error);
	RUN(test_missing_update_interval_defaults);
	RUN(test_a_nonsense_update_interval_falls_back);
	RUN(test_reads_pins_with_the_right_numbering);
	RUN(test_chip_vanishing_is_a_read_failure);
	RUN(test_a_missing_channel_fails_the_whole_sample);
	RUN(test_an_impossible_reading_is_not_a_healthy_one);
}
