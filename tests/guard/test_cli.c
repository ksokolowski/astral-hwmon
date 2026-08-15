// SPDX-License-Identifier: MIT
/* tests/guard/test_cli.c - the reporting surface. */

#include <stdio.h>
#include <string.h>

#include "astral_guard.h"
#include "check.h"

static const int healthy_mv[GUARD_PINS] = {
	11960, 11944, 11952, 11944, 11944, 11944
};
static const int healthy_ma[GUARD_PINS] = {
	7640, 7840, 8300, 7920, 8560, 8020
};

static void fill(struct guard_sample *s, size_t n, const int *ma)
{
	size_t i, pin;

	for (i = 0; i < n; i++)
		for (pin = 0; pin < GUARD_PINS; pin++) {
			s[i].mv[pin] = healthy_mv[pin];
			s[i].ma[pin] = ma[pin];
		}
}

/* The names are an interface: monitoring systems match on them. */
static void test_level_names_are_stable(void)
{
	CHECK(!strcmp(guard_level_name(GUARD_OK), "OK"), "OK");
	CHECK(!strcmp(guard_level_name(GUARD_WARN), "WARNING"), "WARNING");
	CHECK(!strcmp(guard_level_name(GUARD_CRIT), "CRITICAL"), "CRITICAL");
	CHECK(!strcmp(guard_level_name(GUARD_UNKNOWN), "UNKNOWN"), "UNKNOWN");
	CHECK(!strcmp(guard_level_name(GUARD_NOTE), "NOTE"), "NOTE");
}

static void test_json_carries_the_applied_thresholds(void)
{
	struct guard_chip c = { .path = "/sys/class/hwmon/hwmon7",
				.update_interval_ms = 200 };
	struct guard_verdict v;
	struct guard_sample s[5];
	char buf[4096];
	char want[64];
	FILE *fh;

	fill(s, 5, healthy_ma);
	guard_eval(s, 5, &guard_limits_default, &v);

	memset(buf, 0, sizeof(buf));
	fh = fmemopen(buf, sizeof(buf), "w");
	CHECK(fh != NULL, "fmemopen");
	guard_report_json(fh, &c, &v, &guard_limits_default, 1);
	fclose(fh);

	CHECK(strstr(buf, "\"level\": \"OK\"") != NULL,
	      "json must name the level, got: %s", buf);
	CHECK(strstr(buf, "\"exit_code\": 0") != NULL,
	      "json must carry the exit code, got: %s", buf);
	/* Built from the limits actually applied, not from a literal: retuning
	 * a threshold must not fail the suite. What is pinned is that the
	 * report echoes the numbers the verdict was reached with.
	 */
	snprintf(want, sizeof(want), "\"pin_crit_ma\": %d",
		 guard_limits_default.pin_crit_ma);
	CHECK(strstr(buf, want) != NULL,
	      "json must carry the applied thresholds, got: %s", buf);
}

/* An advisory must reach the operator's eyes even though it leaves the exit
 * code at zero - otherwise the tier is indistinguishable from silence.
 */
static void test_a_note_appears_in_both_outputs(void)
{
	const int uneven[GUARD_PINS] = { 6900, 6900, 6900, 8200, 8200, 8200 };
	struct guard_chip c = { .path = "/sys/class/hwmon/hwmon7",
				.update_interval_ms = 200 };
	struct guard_verdict v;
	struct guard_sample s[5];
	char buf[4096];
	FILE *fh;

	fill(s, 5, uneven);
	CHECK(guard_eval(s, 5, &guard_limits_default, &v) == GUARD_OK,
	      "the advisory case must stay OK");

	memset(buf, 0, sizeof(buf));
	fh = fmemopen(buf, sizeof(buf), "w");
	CHECK(fh != NULL, "fmemopen");
	guard_report_text(fh, &c, &v);
	fclose(fh);
	CHECK(strstr(buf, "[NOTE]") != NULL,
	      "text output must show the advisory, got: %s", buf);
	CHECK(strncmp(buf, "OK ", 3) == 0,
	      "the headline must still read OK, got: %s", buf);

	memset(buf, 0, sizeof(buf));
	fh = fmemopen(buf, sizeof(buf), "w");
	CHECK(fh != NULL, "fmemopen");
	guard_report_json(fh, &c, &v, &guard_limits_default, 1);
	fclose(fh);
	CHECK(strstr(buf, "\"level\": \"NOTE\"") != NULL,
	      "json findings must show the advisory, got: %s", buf);
}

void run_cli_tests(void)
{
	RUN(test_level_names_are_stable);
	RUN(test_json_carries_the_applied_thresholds);
	RUN(test_a_note_appears_in_both_outputs);
}
