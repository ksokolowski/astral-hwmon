// SPDX-License-Identifier: MIT
/* tests/support/check.c - the assertion and reporting core.
 *
 * astral_report() treats a run that executed no tests, or made no checks, as a
 * failure. A tier that silently ran nothing would otherwise exit 0 and read as
 * green, which is the one outcome a test harness must never produce.
 */

#include <stdarg.h>
#include <stdio.h>

#include "check.h"

static int checks;
static int failures;
static int current_failures;
static int tests_run;
static int tests_failed;

void astral_check(bool ok, const char *file, int line, const char *fmt, ...)
{
	va_list ap;

	checks++;
	if (ok)
		return;

	failures++;
	current_failures++;
	fprintf(stderr, "    FAIL %s:%d: ", file, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

void astral_run(const char *name, void (*fn)(void))
{
	current_failures = 0;
	tests_run++;
	fn();
	if (current_failures) {
		tests_failed++;
		printf("  FAIL %s (%d)\n", name, current_failures);
	} else {
		printf("  ok   %s\n", name);
	}
}

int astral_report(void)
{
	printf("\n%d tests, %d checks, %d failed\n", tests_run, checks,
	       tests_failed);
	if (!tests_run || !checks) {
		fprintf(stderr, "no tests ran - treating as failure\n");
		return 1;
	}
	return failures ? 1 : 0;
}
