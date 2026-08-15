// SPDX-License-Identifier: MIT
/* guard/guard_report.c - text and JSON output.
 *
 * Every number printed here is an integer. That is not incidental: a float in
 * the evaluator would make --json output depend on the locale's decimal
 * separator, and a report that parses differently under LC_NUMERIC=pl_PL is not
 * machine-readable.
 */

#include <stdio.h>

#include "astral_guard.h"

const char *guard_level_name(int level)
{
	switch (level) {
	case GUARD_OK:		return "OK";
	case GUARD_WARN:	return "WARNING";
	case GUARD_CRIT:	return "CRITICAL";
	case GUARD_UNKNOWN:	return "UNKNOWN";
	case GUARD_NOTE:	return "NOTE";
	default:		return "UNKNOWN";
	}
}

void guard_report_text(FILE *out, const struct guard_chip *c,
		       const struct guard_verdict *v)
{
	size_t i;

	fprintf(out, "%s 12VHPWR %s\n", guard_level_name((int)v->level), c->path);
	for (i = 0; i < v->n; i++)
		fprintf(out, "  [%s] %s\n",
			guard_level_name(v->f[i].level), v->f[i].detail);
}

void guard_report_json(FILE *out, const struct guard_chip *c,
		       const struct guard_verdict *v,
		       const struct guard_limits *lim, int first)
{
	size_t i;

	fprintf(out, "%s  {\n", first ? "" : ",\n");
	fprintf(out, "    \"chip\": \"%s\",\n", c->path);
	fprintf(out, "    \"update_interval_ms\": %d,\n", c->update_interval_ms);
	fprintf(out, "    \"level\": \"%s\",\n", guard_level_name((int)v->level));
	fprintf(out, "    \"exit_code\": %d,\n", (int)v->level);
	fprintf(out, "    \"findings\": [");
	for (i = 0; i < v->n; i++)
		fprintf(out,
			"%s\n      {\"rule\": \"%s\", \"level\": \"%s\", "
			"\"pin\": %d, \"value\": %d, \"detail\": \"%s\"}",
			i ? "," : "", v->f[i].rule,
			guard_level_name(v->f[i].level),
			v->f[i].pin, v->f[i].value, v->f[i].detail);
	fprintf(out, "%s],\n", v->n ? "\n    " : "");

	/* The thresholds travel with the verdict, so a stored report can be
	 * re-read later without guessing which limits produced it.
	 */
	fprintf(out, "    \"limits\": {\"pin_warn_ma\": %d, \"pin_crit_ma\": %d, "
		"\"open_pin_ma\": %d, \"ratio_note_pct\": %d, "
		"\"ratio_warn_pct\": %d, \"ratio_crit_pct\": %d, "
		"\"mv_warn\": %d, \"mv_crit\": %d}\n",
		lim->pin_warn_ma, lim->pin_crit_ma, lim->open_pin_ma,
		lim->ratio_note_pct, lim->ratio_warn_pct, lim->ratio_crit_pct,
		lim->mv_warn, lim->mv_crit);
	fprintf(out, "  }");
}
