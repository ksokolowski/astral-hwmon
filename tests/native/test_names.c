// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/test_names.c - the driver and the guard must name the same chip.
 *
 * guard/astral_guard.h duplicates ASTRAL_CHIP_NAME because driver/astral.h
 * includes <linux/i2c.h> and cannot be used from userspace. A hand-edit to
 * either copy would leave the guard silently finding nothing, which reads as
 * "no card" rather than as a bug.
 *
 * This tier is the only place the C duplication can be checked: it has the kernel
 * shims, so it can include both headers at once. The same pattern as
 * test_c_and_python_allowlists_agree.
 *
 * There is a third copy of the chip name, in src/astral_oracle/hwmon.py, which a
 * C tier cannot see. It is checked by test_c_and_python_chip_names_agree in
 * tests/unit/test_hwmon_surface.py, which parses driver/astral.h the way the
 * allowlist test parses driver/astral_detect.c.
 */

#include <string.h>

#include "astral.h"
#include "astral_guard.h"
#include "check.h"

static void test_guard_and_driver_name_the_same_chip(void)
{
	CHECK(!strcmp(ASTRAL_CHIP_NAME, GUARD_CHIP_NAME),
	      "driver says \"%s\", guard says \"%s\" - the guard would find nothing",
	      ASTRAL_CHIP_NAME, GUARD_CHIP_NAME);
}

static void test_pin_counts_agree(void)
{
	CHECK(ASTRAL_PIN_COUNT == GUARD_PINS,
	      "driver has %d pins, guard has %d",
	      ASTRAL_PIN_COUNT, GUARD_PINS);
}

/* The guard refuses any reading outside these bounds, on the grounds that the
 * driver refuses to publish one. That reasoning only holds while the two copies
 * agree: if the driver's window were widened and the guard's were not, the guard
 * would start calling legitimate readings unreadable and report UNKNOWN on a
 * working card.
 */
static void test_plausibility_bounds_agree(void)
{
	CHECK(ASTRAL_MV_MIN == GUARD_MV_MIN,
	      "driver floor is %d mV, guard floor is %d mV",
	      ASTRAL_MV_MIN, GUARD_MV_MIN);
	CHECK(ASTRAL_MV_MAX == GUARD_MV_MAX,
	      "driver ceiling is %d mV, guard ceiling is %d mV",
	      ASTRAL_MV_MAX, GUARD_MV_MAX);
	CHECK(ASTRAL_MA_MAX == GUARD_MA_MAX,
	      "driver ceiling is %d mA, guard ceiling is %d mA",
	      ASTRAL_MA_MAX, GUARD_MA_MAX);
}

void run_name_tests(void)
{
	RUN(test_guard_and_driver_name_the_same_chip);
	RUN(test_pin_counts_agree);
	RUN(test_plausibility_bounds_agree);
}
