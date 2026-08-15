// SPDX-License-Identifier: MIT
/* tests/guard/fakesys.c - a sysfs tree on disk.
 *
 * Fidelity matters as much here as in the native tier: an over-simplified fake
 * sysfs once made a unit test agree with a bug real hardware rejected. This one
 * carries the driver's real asymmetric numbering - in0..in5 for voltages
 * against curr1..curr6 for currents - so a reader that assumed in1..in6 fails
 * here rather than on a card.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "astral_guard.h"
#include "fakesys.h"

static void write_file(const char *path, const char *value)
{
	FILE *fh = fopen(path, "w");

	if (!fh) {
		fprintf(stderr, "fakesys: cannot write %s\n", path);
		exit(2);
	}
	fprintf(fh, "%s\n", value);
	fclose(fh);
}

void fakesys_create(struct fakesys *fs)
{
	char path[320];

	snprintf(fs->root, sizeof(fs->root), "/tmp/astral-guard-test-XXXXXX");
	if (!mkdtemp(fs->root)) {
		fprintf(stderr, "fakesys: mkdtemp failed\n");
		exit(2);
	}
	snprintf(path, sizeof(path), "%s/class", fs->root);
	if (mkdir(path, 0755) != 0)
		exit(2);
	snprintf(path, sizeof(path), "%s/class/hwmon", fs->root);
	if (mkdir(path, 0755) != 0)
		exit(2);
}

void fakesys_add_chip(struct fakesys *fs, const char *hwmon, const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/class/hwmon/%s", fs->root, hwmon);
	if (mkdir(path, 0755) != 0)
		exit(2);
	snprintf(path, sizeof(path), "%s/class/hwmon/%s/name", fs->root, hwmon);
	write_file(path, name);
}

void fakesys_set_pins(struct fakesys *fs, const char *hwmon,
		      const int *mv, const int *ma)
{
	char path[512];
	char value[32];
	int pin;

	for (pin = 0; pin < GUARD_PINS; pin++) {
		/* Voltages zero-indexed, currents one-indexed. in0 and curr1
		 * are both pin 1. Assuming otherwise reads the wrong pin and
		 * runs off the end.
		 */
		snprintf(path, sizeof(path), "%s/class/hwmon/%s/in%d_input",
			 fs->root, hwmon, pin);
		snprintf(value, sizeof(value), "%d", mv[pin]);
		write_file(path, value);

		snprintf(path, sizeof(path), "%s/class/hwmon/%s/curr%d_input",
			 fs->root, hwmon, pin + 1);
		snprintf(value, sizeof(value), "%d", ma[pin]);
		write_file(path, value);
	}
}

void fakesys_set_attr(struct fakesys *fs, const char *hwmon,
		      const char *attr, const char *value)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/class/hwmon/%s/%s",
		 fs->root, hwmon, attr);
	write_file(path, value);
}

void fakesys_destroy(struct fakesys *fs)
{
	char cmd[320];

	if (!fs->root[0])
		return;
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'", fs->root);
	if (system(cmd) != 0)
		fprintf(stderr, "fakesys: cleanup of %s failed\n", fs->root);
	fs->root[0] = '\0';
}
