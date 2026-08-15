// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/main.c - entry point for the native tier. */

#include <stdio.h>

#include "harness.h"

void run_name_tests(void);

int main(int argc, char **argv)
{
	const char *corpus = argc > 1 ? argv[1] : "tests/data/frames.jsonl";

	printf("native tier - driver sources on a host build\n\n");
	run_regs_tests(corpus);
	putchar('\n');
	run_detect_tests();
	putchar('\n');
	run_name_tests();

	return astral_report();
}
