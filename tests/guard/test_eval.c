// SPDX-License-Identifier: MIT
/* tests/guard/test_eval.c - the rule engine. */

#include <string.h>

#include "astral_guard.h"
#include "check.h"
#include "corpus.h"

static struct guard_corpus corpus;

/* A healthy full-load sample: docs/measurements/2026-08-14/burn-2hz.csv, the
 * highest-total row. 48.26 A total, min/max = 0.890.
 */
static const struct guard_sample healthy_full = {
	.mv = { 11960, 11944, 11952, 11944, 11944, 11944 },
	.ma = { 7640, 7840, 8300, 7920, 8560, 8020 }
};

static void test_healthy_full_load_is_ok(void)
{
	struct guard_verdict v;
	struct guard_sample s[5];
	size_t i;

	for (i = 0; i < 5; i++)
		s[i] = healthy_full;

	CHECK(guard_eval(s, 5, &guard_limits_default, &v) == GUARD_OK,
	      "healthy full-load sample must be OK");
	CHECK(v.n == 0, "healthy sample must produce no findings, got %zu", v.n);
}

/* The exit-code values are not in severity order. Nothing may compare them
 * raw, or an UNKNOWN on one card would mask a CRITICAL on another.
 */
static void test_severity_ranks_critical_above_unknown(void)
{
	CHECK(guard_severity(GUARD_CRIT) > guard_severity(GUARD_UNKNOWN),
	      "CRITICAL must outrank UNKNOWN despite 2 < 3");
	CHECK(guard_severity(GUARD_UNKNOWN) > guard_severity(GUARD_WARN),
	      "UNKNOWN must outrank WARNING");
	CHECK(guard_severity(GUARD_WARN) > guard_severity(GUARD_OK),
	      "WARNING must outrank OK");
	CHECK(guard_severity(GUARD_NOTE) > guard_severity(GUARD_OK),
	      "NOTE must outrank OK for display ordering");
	CHECK(guard_severity(GUARD_NOTE) < guard_severity(GUARD_WARN),
	      "NOTE must never reach WARNING");
}

/* The persistence requirement is the whole defence against the transient the
 * vendor's own tool alarms on: ASUS reproduced a false positive in which Power
 * Detector+ reports 0 A or >9.2 A after resume while the sensor reads fine.
 * Pinned directly, not only through the corpus.
 *
 * Both directions matter. Testing only that five-of-five alarms would still
 * pass with the persistence check deleted.
 */
static void test_a_single_bad_sample_does_not_alarm(void)
{
	struct guard_verdict v;
	struct guard_sample s[5];
	size_t i;

	for (i = 0; i < 5; i++)
		s[i] = healthy_full;
	s[2].ma[4] = 9800;	/* one sample, one pin, far over the rating */

	CHECK(guard_eval(s, 5, &guard_limits_default, &v) == GUARD_OK,
	      "one bad sample out of five must not raise the level");
}

static void test_every_sample_bad_does_alarm(void)
{
	struct guard_verdict v;
	struct guard_sample s[5];
	size_t i;

	for (i = 0; i < 5; i++) {
		s[i] = healthy_full;
		s[i].ma[4] = 9800;
	}

	CHECK(guard_eval(s, 5, &guard_limits_default, &v) == GUARD_CRIT,
	      "five bad samples out of five must be CRIT");
}

/* The advisory tier must be visible to a human and invisible to the alerting
 * layer. If it could raise the exit code it would page someone about a
 * connector that is merely uneven.
 */
static void test_note_never_changes_the_level(void)
{
	const int uneven[GUARD_PINS] = { 6900, 6900, 6900, 8200, 8200, 8200 };
	struct guard_verdict v;
	struct guard_sample s[5];
	size_t i, j;
	bool saw_note = false;

	for (i = 0; i < 5; i++) {
		s[i] = healthy_full;
		for (j = 0; j < GUARD_PINS; j++)
			s[i].ma[j] = uneven[j];
	}

	CHECK(guard_eval(s, 5, &guard_limits_default, &v) == GUARD_OK,
	      "a NOTE must leave the verdict at OK");
	for (i = 0; i < v.n; i++)
		if (v.f[i].level == GUARD_NOTE)
			saw_note = true;
	CHECK(saw_note, "the finding must still be recorded for a human");
}

static void test_corpus_loaded_cases(void)
{
	CHECK(corpus.count >= 3, "corpus must carry cases, got %zu", corpus.count);
}

static void test_corpus_cases_match_expectations(void)
{
	size_t i, j;

	for (i = 0; i < corpus.count; i++) {
		const struct guard_case *c = &corpus.cases[i];
		struct guard_verdict v;
		enum guard_level got = guard_eval(c->s, c->samples,
						  &guard_limits_default, &v);
		bool saw_note = false;

		for (j = 0; j < v.n; j++)
			if (v.f[j].level == GUARD_NOTE)
				saw_note = true;

		CHECK(got == c->expect,
		      "line %d (%s): expected level %d, got %d",
		      c->line, c->note, (int)c->expect, (int)got);
		CHECK(saw_note == c->expect_note,
		      "line %d (%s): expected note %d, got %d",
		      c->line, c->note, (int)c->expect_note, (int)saw_note);
	}
}

void run_eval_tests(const char *corpus_path)
{
	guard_corpus_load(&corpus, corpus_path);
	RUN(test_healthy_full_load_is_ok);
	RUN(test_severity_ranks_critical_above_unknown);
	RUN(test_a_single_bad_sample_does_not_alarm);
	RUN(test_every_sample_bad_does_alarm);
	RUN(test_note_never_changes_the_level);
	RUN(test_corpus_loaded_cases);
	RUN(test_corpus_cases_match_expectations);
}
