// SPDX-License-Identifier: MIT
/* guard/guard_sysfs.c - hwmon discovery and reads.
 *
 * Read-only throughout. Every file is opened "r"; there is no write path in
 * this translation unit and there must never be one.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "astral_guard.h"

/* Reads one line from a sysfs attribute. Returns 0 on success. */
static int read_attr(const char *dir, const char *attr, char *out, size_t max)
{
	char path[sizeof(((struct guard_chip *)0)->path) + 64];
	FILE *fh;

	snprintf(path, sizeof(path), "%s/%s", dir, attr);
	fh = fopen(path, "r");
	if (!fh)
		return -1;
	/* fgets takes an int; `max` is a size_t. The narrowing is safe because
	 * every caller in this file passes sizeof() of a fixed stack buffer of
	 * a few dozen bytes. Written as an explicit cast rather than left for
	 * -Wconversion to flag, and this is the line to revisit if a caller
	 * ever passes a runtime length.
	 */
	if (!fgets(out, (int)max, fh)) {
		fclose(fh);
		return -1;
	}
	fclose(fh);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

/* Same standard as parse_int() in guard_main.c, and for the same reason: a
 * value that is not entirely a number in range is not a reading. Bounds are
 * mandatory rather than optional here, because strtol saturates to LONG_MAX and
 * the narrowing to int is silent - "4294975296" would otherwise arrive as a
 * perfectly healthy 8000 mA, and every rule would pass on it.
 *
 * Rejection returns -1, which guard_read_sample turns into a whole-sample
 * failure and main() into UNKNOWN. That is the right answer: losing sight of a
 * channel is a fault, not silence.
 */
static int read_int_attr(const char *dir, const char *attr, int lo, int hi,
			 int *out)
{
	char buf[32];
	char *end;
	long v;

	if (read_attr(dir, attr, buf, sizeof(buf)) != 0)
		return -1;
	v = strtol(buf, &end, 10);
	if (end == buf || *end != '\0' || v < lo || v > hi)
		return -1;
	*out = (int)v;
	return 0;
}

static int by_path(const void *a, const void *b)
{
	const struct guard_chip *x = a;
	const struct guard_chip *y = b;

	return strcmp(x->path, y->path);
}

int guard_find_chips(const char *sysfs_root, struct guard_chip *out, size_t max)
{
	char class_dir[2048];
	struct dirent *ent;
	DIR *dir;
	size_t n = 0;

	snprintf(class_dir, sizeof(class_dir), "%s/class/hwmon", sysfs_root);
	dir = opendir(class_dir);
	if (!dir)
		return -1;

	while ((ent = readdir(dir)) != NULL && n < max) {
		char path[sizeof(out->path)];
		char name[64];

		if (ent->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "%s/%s", class_dir, ent->d_name);
		if (read_attr(path, "name", name, sizeof(name)) != 0)
			continue;
		/* By name, never by the hwmonN index: it is hwmon10 on the
		 * reference machine today and was not yesterday.
		 */
		if (strcmp(name, GUARD_CHIP_NAME) != 0)
			continue;

		snprintf(out[n].path, sizeof(out[n].path), "%s", path);
		if (read_int_attr(path, "update_interval", 0,
				  GUARD_MAX_INTERVAL_MS,
				  &out[n].update_interval_ms) != 0)
			out[n].update_interval_ms = GUARD_DEFAULT_INTERVAL_MS;
		n++;
	}

	closedir(dir);
	/* readdir order is unspecified; sort so output is reproducible. */
	qsort(out, n, sizeof(*out), by_path);
	return (int)n;
}

int guard_read_sample(const struct guard_chip *c, struct guard_sample *out)
{
	char attr[32];
	int pin;

	for (pin = 0; pin < GUARD_PINS; pin++) {
		/* hwmon numbering is asymmetric: voltages are zero-indexed,
		 * currents one-indexed. in0 and curr1 are both pin 1.
		 */
		snprintf(attr, sizeof(attr), "in%d_input", pin);
		if (read_int_attr(c->path, attr, GUARD_MV_MIN, GUARD_MV_MAX,
				  &out->mv[pin]) != 0)
			return -1;

		/* A plain 0 rather than a GUARD_MA_MIN to match: the driver
		 * needs no such constant because it decodes a u16 and cannot
		 * produce a negative, but this parses text and can. A negative
		 * current is not something the driver could have published.
		 */
		snprintf(attr, sizeof(attr), "curr%d_input", pin + 1);
		if (read_int_attr(c->path, attr, 0, GUARD_MA_MAX,
				  &out->ma[pin]) != 0)
			return -1;
	}
	return 0;
}
