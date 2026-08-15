// SPDX-License-Identifier: MIT
/* tests/guard/main.c - entry point for the guard tier. */

#include <stdio.h>

#include "check.h"

void run_eval_tests(const char *corpus_path);
void run_sysfs_tests(void);
void run_cli_tests(void);

int main(int argc, char **argv)
{
	const char *corpus = argc > 1 ? argv[1] : "tests/data/guard-cases.jsonl";

	printf("guard tier - astral-guard on a host build\n\n");
	run_eval_tests(corpus);
	putchar('\n');
	run_sysfs_tests();
	putchar('\n');
	run_cli_tests();

	return astral_report();
}
