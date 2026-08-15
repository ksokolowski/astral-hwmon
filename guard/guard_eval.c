// SPDX-License-Identifier: MIT
/* guard/guard_eval.c - the rules. Pure: no I/O, no allocation, no globals.
 *
 * Every threshold here comes from a connector specification, a vendor
 * statement, a shipping product's field-revised setting, or a physical model -
 * never from the reference card. The reference card's captures are used only to
 * confirm the rules stay quiet on a connector known to be healthy. guard/README.md
 * tabulates each threshold against the source it came from, and
 * docs/GUARD-DESIGN.md lists those sources in full.
 *
 * All arithmetic is integer. Ratios are compared as `lo * 100 < hi * pct`, so
 * there is no rounding behaviour to reason about in a safety tool and no
 * locale-dependent decimal separator in --json output.
 */

#include <stdio.h>
#include <string.h>

#include "astral_guard.h"

const struct guard_limits guard_limits_default = {
	.pin_warn_ma = 9200,	/* ASUS Power Detector+ warning point */
	.pin_crit_ma = 9500,	/* 12V-2x6 per-pin rating */
	.open_pin_ma = 500,	/* ASUS PD+ also warns on "or is at 0 amps" */
	.open_gate_ma = 10000,
	.ratio_gate_ma = 20000,
	.ratio_note_pct = 85,
	.ratio_warn_pct = 70,	/* WireView Pro II's original 30% difference */
	.ratio_crit_pct = 60,	/* WireView Pro II's field-revised 40% */
	.mv_warn = 11400,	/* ATX 12 V -5% */
	.mv_crit = 11000	/* -8%; no working rail sags this far */
};

int guard_severity(int level)
{
	switch (level) {
	case GUARD_NOTE:	return 1;
	case GUARD_OK:		return 0;
	case GUARD_WARN:	return 2;
	case GUARD_UNKNOWN:	return 3;
	case GUARD_CRIT:	return 4;
	default:		return 3;
	}
}

static int total_ma(const struct guard_sample *s)
{
	int t = 0;
	size_t pin;

	for (pin = 0; pin < GUARD_PINS; pin++)
		t += s->ma[pin];
	return t;
}

static void add(struct guard_verdict *v, int level, const char *rule,
		int pin, int value, const char *detail)
{
	struct guard_finding *f;

	if (v->n == GUARD_MAX_FINDINGS)
		return;
	f = &v->f[v->n++];
	f->level = level;
	f->rule = rule;
	f->pin = pin;
	f->value = value;
	snprintf(f->detail, sizeof(f->detail), "%s", detail);

	/* An advisory is recorded but can never become the verdict: it exists to
	 * be read by a human, not to page anyone. Everything else is compared by
	 * severity rank rather than by the raw value, so UNKNOWN cannot mask a
	 * CRITICAL on another chip.
	 */
	if (level != GUARD_NOTE &&
	    guard_severity(level) > guard_severity((int)v->level))
		v->level = (enum guard_level)level;
}

enum guard_level guard_eval(const struct guard_sample *s, size_t n,
			    const struct guard_limits *lim,
			    struct guard_verdict *out)
{
	int warn_hits[GUARD_PINS] = { 0 };
	int crit_hits[GUARD_PINS] = { 0 };
	int open_hits[GUARD_PINS] = { 0 };
	int worst_ma[GUARD_PINS] = { 0 };
	int note_hits = 0, ratio_warn_hits = 0, ratio_crit_hits = 0;
	int mv_warn_hits = 0, mv_crit_hits = 0;
	int worst_pct = 100;
	int worst_mv;
	size_t i, pin;

	memset(out, 0, sizeof(*out));
	out->level = GUARD_OK;
	if (n == 0)
		return out->level;

	worst_mv = s[0].mv[0];

	for (i = 0; i < n; i++) {
		for (pin = 0; pin < GUARD_PINS; pin++) {
			int ma = s[i].ma[pin];

			if (ma > worst_ma[pin])
				worst_ma[pin] = ma;
			/* Inclusive: a rating is a limit, not a target. */
			if (ma >= lim->pin_crit_ma)
				crit_hits[pin]++;
			if (ma >= lim->pin_warn_ma)
				warn_hits[pin]++;
		}

		/* R4 - the pin voltage floor. Ungated: a sagging rail is a fault
		 * at any load. A degrading contact raises resistance and so
		 * lowers the pin voltage under load, which is why the driver's
		 * plausibility floor is deliberately loose enough to let a
		 * sagging pin through as a low number rather than as silence.
		 */
		{
			int lo = s[i].mv[0];

			for (pin = 1; pin < GUARD_PINS; pin++)
				if (s[i].mv[pin] < lo)
					lo = s[i].mv[pin];
			if (lo < worst_mv)
				worst_mv = lo;
			if (lo < lim->mv_crit)
				mv_crit_hits++;
			if (lo < lim->mv_warn)
				mv_warn_hits++;
		}

		/* R2's gate. Without it every idle machine reports six open
		 * pins. It is lower than R3's because this test only asks
		 * whether one pin has stopped conducting while the others have
		 * not, which is unambiguous well below the load a pin-to-pin
		 * comparison needs: at 10 A total a healthy pin carries about
		 * 1.7 A, so a pin under 0.5 A is not a rounding artefact.
		 */
		if (total_ma(&s[i]) >= lim->open_gate_ma) {
			for (pin = 0; pin < GUARD_PINS; pin++)
				if (s[i].ma[pin] < lim->open_pin_ma)
					open_hits[pin]++;
		}

		/* R3's gate is higher: comparing every pin against every other
		 * needs the 20 mA quantisation step to be small relative to the
		 * spread. The reference card's worst gated sample is 86.1% at
		 * 20 A, against 76.3% if the gate were 10 A.
		 */
		if (total_ma(&s[i]) >= lim->ratio_gate_ma) {
			int lo = s[i].ma[0];
			int hi = s[i].ma[0];

			for (pin = 1; pin < GUARD_PINS; pin++) {
				if (s[i].ma[pin] < lo)
					lo = s[i].ma[pin];
				if (s[i].ma[pin] > hi)
					hi = s[i].ma[pin];
			}
			if (hi > 0) {
				int pct = lo * 100 / hi;

				if (pct < worst_pct)
					worst_pct = pct;
				if (lo * 100 < hi * lim->ratio_crit_pct)
					ratio_crit_hits++;
				if (lo * 100 < hi * lim->ratio_warn_pct)
					ratio_warn_hits++;
				if (lo * 100 < hi * lim->ratio_note_pct)
					note_hits++;
			}
		}
	}

	/* R2 - an open pin. ASUS Power Detector+ warns when a pin "exceeds 9.2
	 * amps or is at 0 amps"; a pin that has stopped conducting pushes its
	 * share onto its neighbours, which is the failure this tool exists for.
	 */
	for (pin = 0; pin < GUARD_PINS; pin++) {
		char detail[96];

		if (open_hits[pin] == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin %zu under %d mA while the connector is loaded",
				 pin + 1, lim->open_pin_ma);
			add(out, GUARD_CRIT, "open-pin", (int)pin + 1, 0, detail);
		}
	}

	/* R1 - absolute per-pin current. Never gated: it is absolute for a
	 * reason. Reported only when it held in every sample.
	 */
	for (pin = 0; pin < GUARD_PINS; pin++) {
		char detail[96];

		if (crit_hits[pin] == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin %zu at %d mA, at or above the %d mA rating",
				 pin + 1, worst_ma[pin], lim->pin_crit_ma);
			add(out, GUARD_CRIT, "pin-current", (int)pin + 1,
			    worst_ma[pin], detail);
		} else if (warn_hits[pin] == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin %zu at %d mA, above the %d mA warning point",
				 pin + 1, worst_ma[pin], lim->pin_warn_ma);
			add(out, GUARD_WARN, "pin-current", (int)pin + 1,
			    worst_ma[pin], detail);
		}
	}

	/* R3 - min/max imbalance. Not max/mean: that metric is inverted here.
	 * A pin at 1.5x contact resistance scores max/mean 1.059 against the
	 * healthy reference card's 1.064, because five pins absorb the starved
	 * pin's share evenly and the top pin barely moves. The signal is in the
	 * starved pin.
	 */
	{
		char detail[96];

		if (ratio_crit_hits == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin currents differ by more than %d%%: min/max %d%%",
				 100 - lim->ratio_crit_pct, worst_pct);
			add(out, GUARD_CRIT, "imbalance", 0, worst_pct, detail);
		} else if (ratio_warn_hits == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin currents differ by more than %d%%: min/max %d%%",
				 100 - lim->ratio_warn_pct, worst_pct);
			add(out, GUARD_WARN, "imbalance", 0, worst_pct, detail);
		} else if (note_hits == (int)n) {
			snprintf(detail, sizeof(detail),
				 "pin currents uneven: min/max %d%% - worth watching",
				 worst_pct);
			add(out, GUARD_NOTE, "imbalance", 0, worst_pct, detail);
		}

		if (mv_crit_hits == (int)n) {
			snprintf(detail, sizeof(detail),
				 "lowest pin at %d mV, below %d mV",
				 worst_mv, lim->mv_crit);
			add(out, GUARD_CRIT, "voltage", 0, worst_mv, detail);
		} else if (mv_warn_hits == (int)n) {
			snprintf(detail, sizeof(detail),
				 "lowest pin at %d mV, below %d mV",
				 worst_mv, lim->mv_warn);
			add(out, GUARD_WARN, "voltage", 0, worst_mv, detail);
		}
	}

	return out->level;
}
