/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <linux/i2c.h>. See tests/native/README.md. */
#ifndef ASTRAL_TEST_LINUX_I2C_H
#define ASTRAL_TEST_LINUX_I2C_H

#include <linux/device.h>
#include <linux/types.h>

/* 48 is the real kernel's size for this field; the reference machine's name,
 * "NVIDIA i2c adapter 1 at 1:00.0", is 31 bytes and must not be truncated.
 */
struct i2c_adapter {
	struct device dev;
	char name[48];
};

struct i2c_client {
	struct device dev;
	struct i2c_adapter *adapter;
	unsigned short addr;
};

/* Fake transfer, backed by astral_fake_i2c in harness.c. */
s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command,
				  u8 length, u8 *values);

#endif /* ASTRAL_TEST_LINUX_I2C_H */
