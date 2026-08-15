/* SPDX-License-Identifier: MIT */
/* tests/support/check.h - assertions shared by every C test tier.
 *
 * Deliberately free of kernel types: the native tier compiles against the shims
 * in tests/native/linux/, the guard tier against the real system headers, and
 * both need these. A header that pulled in <linux/i2c.h> could only serve one
 * of them.
 */
#ifndef ASTRAL_TEST_CHECK_H
#define ASTRAL_TEST_CHECK_H

#include <stdbool.h>

void astral_check(bool ok, const char *file, int line, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void astral_run(const char *name, void (*fn)(void));
int astral_report(void);

#define CHECK(cond, ...) astral_check((cond), __FILE__, __LINE__, __VA_ARGS__)
#define RUN(fn) astral_run(#fn, fn)

#endif /* ASTRAL_TEST_CHECK_H */
