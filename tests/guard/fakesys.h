/* SPDX-License-Identifier: MIT */
/* tests/guard/fakesys.h - a sysfs tree on disk, for the reader's tests. */
#ifndef ASTRAL_GUARD_FAKESYS_H
#define ASTRAL_GUARD_FAKESYS_H

struct fakesys {
	char root[256];
};

void fakesys_create(struct fakesys *fs);
void fakesys_add_chip(struct fakesys *fs, const char *hwmon, const char *name);
void fakesys_set_pins(struct fakesys *fs, const char *hwmon,
		      const int *mv, const int *ma);
void fakesys_set_attr(struct fakesys *fs, const char *hwmon,
		      const char *attr, const char *value);
void fakesys_destroy(struct fakesys *fs);

#endif /* ASTRAL_GUARD_FAKESYS_H */
