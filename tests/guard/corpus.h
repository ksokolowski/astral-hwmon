/* SPDX-License-Identifier: MIT */
/* tests/guard/corpus.h - the rule corpus.
 *
 * Each case names the measurement or public report it came from, so a threshold
 * change is judged against evidence rather than against numbers someone
 * invented. The same idea as tests/data/frames.jsonl for the decode.
 */
#ifndef ASTRAL_GUARD_CORPUS_H
#define ASTRAL_GUARD_CORPUS_H

#include <stdbool.h>
#include <stddef.h>

#include "astral_guard.h"

#define GUARD_CORPUS_MAX 128

struct guard_case {
	int line;
	char note[192];
	enum guard_level expect;
	bool expect_note;
	size_t samples;
	struct guard_sample s[GUARD_MAX_SAMPLES];
};

struct guard_corpus {
	struct guard_case cases[GUARD_CORPUS_MAX];
	size_t count;
};

/* Fatal on a malformed or missing corpus. A loader that quietly skipped a line
 * would turn this tier into a vacuous pass - the same reasoning as the native
 * tier's.
 */
void guard_corpus_load(struct guard_corpus *out, const char *path);

#endif /* ASTRAL_GUARD_CORPUS_H */
