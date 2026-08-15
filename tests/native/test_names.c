// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/test_names.c - the driver and the guard must name the same chip.
 *
 * guard/astral_guard.h duplicates ASTRAL_CHIP_NAME because driver/astral.h
 * includes <linux/i2c.h> and cannot be used from userspace. A hand-edit to
 * either copy would leave the guard silently finding nothing, which reads as
 * "no card" rather than as a bug.
 *
 * This tier is the only place the duplication can be checked: it has the kernel
 * shims, so it can include both headers at once. The same pattern as
 * test_c_and_python_allowlists_agree.
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

void run_name_tests(void)
{
	RUN(test_guard_and_driver_name_the_same_chip);
	RUN(test_pin_counts_agree);
}
