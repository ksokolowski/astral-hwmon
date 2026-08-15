/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <linux/module.h>. See tests/native/README.md. */
#ifndef ASTRAL_TEST_LINUX_MODULE_H
#define ASTRAL_TEST_LINUX_MODULE_H

/*
 * MODULE_DEVICE_TABLE emits modalias metadata in a real build. Here it only has
 * to keep the table from looking unused, because tests/native builds with
 * -Werror and the table is otherwise referenced only by the tests.
 */
#define MODULE_DEVICE_TABLE(type, name) \
	static const void *astral_test_devtable_##type __attribute__((unused)) = &(name)

#endif /* ASTRAL_TEST_LINUX_MODULE_H */
