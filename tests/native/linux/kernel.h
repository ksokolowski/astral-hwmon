/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <linux/kernel.h>. See tests/native/README.md. */
#ifndef ASTRAL_TEST_LINUX_KERNEL_H
#define ASTRAL_TEST_LINUX_KERNEL_H

#include <errno.h>
#include <stdio.h> /* sscanf */

#include <linux/types.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/*
 * The driver parses adapter names with sscanf(). The kernel's vsscanf() and
 * glibc's agree on %d and on literal matching, which is all this driver uses,
 * so the host build exercises the same behaviour. Anything fancier than %d
 * would need checking against lib/vsprintf.c before trusting it here.
 */

#endif /* ASTRAL_TEST_LINUX_KERNEL_H */
