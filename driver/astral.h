/* SPDX-License-Identifier: GPL-2.0-only */
/* driver/astral.h - shared declarations for the astral-hwmon driver. */
#ifndef ASTRAL_H
#define ASTRAL_H

#include <linux/i2c.h>
#include <linux/types.h>

#define ASTRAL_PIN_COUNT	6
#define ASTRAL_REG_BASE		0x80	/* IT8915FN pin block; verified vs HWiNFO */
#define ASTRAL_BLOCK_LEN	24	/* 6 blocks x (u16 BE mV, u16 BE mA) */
#define ASTRAL_DEFAULT_ADDR	0x2b

/* Plausibility bounds. The job of the floor is to reject the all-zeros frame a
 * repeated-START read produces on this bus (measured 2026-08-13), which decodes
 * to a perfectly healthy 0 A on every pin.
 *
 * 6000 mV, not a tight 11000: a failing 12VHPWR connector raises contact
 * resistance and so *lowers* the pin voltage under load, which means a tight
 * floor blinds this driver to the exact failure it exists to reveal. One pin
 * below the floor fails the whole frame and takes all twelve channels - the
 * currents included - out with it. Under a 562 W burn the lowest pin measured
 * 11944 mV (docs/measurements/2026-08-13/load-decoded.txt), only 944 mV of
 * headroom. Half of nominal is not a working 12 V rail by any reading, so 6000
 * still rejects garbage while leaving a sagging pin visible as a low number
 * rather than as silence.
 */
#define ASTRAL_MV_MIN		6000
#define ASTRAL_MV_MAX		13000
#define ASTRAL_MA_MAX		30000

/* Only adapter 1 carries the sensor chip. The kernel writes the name as
 * "NVIDIA i2c adapter 1 at 1:00.0", so this is matched by parsed index and
 * never by string equality.
 */
#define ASTRAL_ADAPTER_FMT	"NVIDIA i2c adapter %d"
#define ASTRAL_ADAPTER_INDEX	1

#define ASTRAL_CHIP_NAME	"astral12vhpwr"

struct astral_frame {
	u16 millivolts[ASTRAL_PIN_COUNT];	/* index 0 = pin 1 */
	u16 milliamps[ASTRAL_PIN_COUNT];
};

/* astral_regs.c
 *
 * astral_decode_frame() is pure: ASTRAL_BLOCK_LEN bytes in, a frame out. It is
 * separate from the transfer so tests/native can drive it on a host build.
 */
void astral_decode_frame(const u8 *buf, struct astral_frame *out);
int astral_read_frame(struct i2c_client *client, struct astral_frame *out);
bool astral_frame_plausible(const struct astral_frame *frame);

/* astral_detect.c
 *
 * astral_card_matches() is the PCI-id gate and must pass before ANY I2C
 * traffic. astral_is_sensor_adapter() then picks which of the card's adapters
 * carries the chip.
 */
bool astral_card_present(bool allow_unknown, const char **model_out);
bool astral_card_matches(struct i2c_adapter *adap, bool allow_unknown,
			 const char **model_out);
bool astral_is_sensor_adapter(struct i2c_adapter *adap);

/* Filled in by a scan so the caller can say WHY it has not attached yet. */
struct astral_scan {
	int card_adapters;	/* i2c adapters belonging to a matching card */
	int candidates;		/* those that passed every filter */
};

#endif /* ASTRAL_H */
