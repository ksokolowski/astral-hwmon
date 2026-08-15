/* SPDX-License-Identifier: GPL-2.0-only */
/* tests/native/harness.h - assertions, corpus loader and programmable fakes. */
#ifndef ASTRAL_TEST_HARNESS_H
#define ASTRAL_TEST_HARNESS_H

#include <linux/i2c.h>
#include <linux/pci.h>
#include <linux/types.h>

/* CHECK, RUN and astral_report(). Kept in a kernel-type-free header so the
 * guard tier, which compiles against the real system headers, can share them.
 */
#include "check.h"

/* ---- the captured frame corpus ------------------------------------------- */

#define ASTRAL_CORPUS_MAX 512
#define ASTRAL_CORPUS_PINS 6

struct astral_corpus_case {
	int line;
	char note[256];
	u8 raw[64];
	size_t raw_len;
	bool reject;
	int expect_mv[ASTRAL_CORPUS_PINS];
	int expect_ma[ASTRAL_CORPUS_PINS];
};

struct astral_corpus {
	struct astral_corpus_case cases[ASTRAL_CORPUS_MAX];
	size_t count;
	size_t accepts;
	size_t rejects;
};

/* Fatal on a malformed corpus: a parser that quietly skipped lines would turn
 * this whole tier into a vacuous pass.
 */
void astral_corpus_load(struct astral_corpus *out, const char *path);

/* ---- fakes --------------------------------------------------------------- */

/* What the next i2c_smbus_read_i2c_block_data() call does. `result` is returned
 * verbatim, so a negative value simulates a transport error and a short count
 * simulates a truncated read.
 */
struct astral_fake_i2c {
	u8 data[64];
	s32 result;
	int calls;
};

extern struct astral_fake_i2c astral_fake_i2c;

void astral_fake_i2c_set(const u8 *data, size_t len, s32 result);

void astral_fake_pci_reset(void);
struct pci_dev *astral_fake_pci_add(u32 vendor, u32 subsystem_vendor,
				    u32 subsystem_device);
int astral_fake_pci_gets(void);
int astral_fake_pci_puts(void);

/* ---- suites -------------------------------------------------------------- */

void run_regs_tests(const char *corpus_path);
void run_detect_tests(void);

#endif /* ASTRAL_TEST_HARNESS_H */
