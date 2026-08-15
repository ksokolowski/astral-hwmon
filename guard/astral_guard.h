/* SPDX-License-Identifier: MIT */
/* guard/astral_guard.h - types and limits for astral-guard. */
#ifndef ASTRAL_GUARD_H
#define ASTRAL_GUARD_H

#include <stddef.h>
#include <stdio.h>

#define GUARD_PINS 6
#define GUARD_MAX_CHIPS 4
#define GUARD_MAX_FINDINGS 16
#define GUARD_MAX_SAMPLES 32
#define GUARD_DEFAULT_SAMPLES 5
#define GUARD_DEFAULT_INTERVAL_MS 200

/* Duplicated from driver/astral.h, which cannot be included here because it
 * pulls in <linux/i2c.h>. tests/native/test_names.c asserts the two agree - a
 * hand-edit to either copy would leave the guard silently finding nothing,
 * which reads as "no card" rather than as a bug.
 */
#define GUARD_CHIP_NAME "astral12vhpwr"

/* The enum values are the process exit codes: the monitoring-plugin convention
 * understood by Nagios, Icinga, Zabbix and checkmk. Keep them in this order.
 *
 * Note that the numeric order is NOT severity order - UNKNOWN is 3 but is less
 * severe than CRITICAL. Use guard_severity() to compare, never the raw value.
 */
enum guard_level {
	GUARD_OK = 0,
	GUARD_WARN = 1,
	GUARD_CRIT = 2,
	GUARD_UNKNOWN = 3
};

/* Advisory. Recorded on a finding, printed for a human, and deliberately
 * incapable of changing the exit code.
 */
#define GUARD_NOTE (-1)

struct guard_sample {
	int mv[GUARD_PINS];	/* index 0 = pin 1 */
	int ma[GUARD_PINS];
};

struct guard_limits {
	int pin_warn_ma;	/* ASUS Power Detector+ warning point */
	int pin_crit_ma;	/* 12V-2x6 per-pin rating */
	int open_pin_ma;	/* below this, with load present, the pin is open */
	int open_gate_ma;	/* total current below which R2 is meaningless */
	int ratio_gate_ma;	/* total current below which R3 is meaningless */
	int ratio_note_pct;	/* min/max percentages, high to low */
	int ratio_warn_pct;
	int ratio_crit_pct;
	int mv_warn;
	int mv_crit;
};

extern const struct guard_limits guard_limits_default;

struct guard_finding {
	int level;		/* enum guard_level, or GUARD_NOTE */
	const char *rule;	/* stable id, e.g. "pin-current" */
	int pin;		/* 1..6, or 0 when not pin-specific */
	int value;		/* the measured quantity; units per rule */
	char detail[96];
};

struct guard_verdict {
	enum guard_level level;
	struct guard_finding f[GUARD_MAX_FINDINGS];
	size_t n;
};

struct guard_chip {
	char path[3072];
	int update_interval_ms;
};

/* Returns the number of matching chips written to `out`, or -1 if the hwmon
 * class directory cannot be read at all. Matching is on the `name` attribute:
 * hwmonN indices differ between machines and across boots. Results are sorted,
 * because readdir order is unspecified.
 */
int guard_find_chips(const char *sysfs_root, struct guard_chip *out, size_t max);

/* 0 on success, -1 if any channel could not be read. A partial read is a
 * failure: a half-filled sample would be evaluated as though the missing pins
 * were carrying nothing, which the open-pin rule would report as a dead contact.
 */
int guard_read_sample(const struct guard_chip *c, struct guard_sample *out);

/* Severity rank for comparison, highest is worst: OK < NOTE < WARNING <
 * UNKNOWN < CRITICAL. Exists because the exit-code values are not in severity
 * order, and comparing them raw would let an UNKNOWN on one card mask a
 * CRITICAL on another.
 */
int guard_severity(int level);

/* Pure: no I/O, no allocation, no globals, no clock. A finding is reported only
 * if it holds in every one of the n samples.
 */
enum guard_level guard_eval(const struct guard_sample *s, size_t n,
			    const struct guard_limits *lim,
			    struct guard_verdict *out);

/* "OK", "WARNING", "CRITICAL", "UNKNOWN", "NOTE" - the monitoring-plugin
 * spellings, so a check result reads the way operators expect.
 */
const char *guard_level_name(int level);

void guard_report_text(FILE *out, const struct guard_chip *c,
		       const struct guard_verdict *v);

/* `first` suppresses the separating comma for the first chip in the array. */
void guard_report_json(FILE *out, const struct guard_chip *c,
		       const struct guard_verdict *v,
		       const struct guard_limits *lim, int first);

#endif /* ASTRAL_GUARD_H */
