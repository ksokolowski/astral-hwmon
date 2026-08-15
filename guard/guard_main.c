// SPDX-License-Identifier: MIT
/* guard/guard_main.c - CLI and orchestration.
 *
 * One-shot by design. Exit codes follow the monitoring-plugin convention, so
 * cron, a systemd timer, Icinga, Zabbix and checkmk all consume this without
 * the tool knowing any of them exist.
 *
 * Needs no privileges: every attribute it reads is world-readable, and it
 * opens all of them read-only.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "astral_guard.h"

#ifndef GUARD_VERSION
#define GUARD_VERSION "unknown"
#endif

static void usage(FILE *out)
{
	fprintf(out,
		"usage: astral-guard [options]\n"
		"  --json               machine-readable output\n"
		"  --samples N          samples per run (default %d)\n"
		"  --interval MS        ms between samples (default: the chip's)\n"
		"  --sysfs-root PATH    sysfs mount point (default /sys)\n"
		"  --warn-ratio PCT     min/max warning threshold (default %d)\n"
		"  --note-ratio PCT     min/max advisory threshold (default %d)\n"
		"  --quiet              print only when not OK\n"
		"  --version\n"
		"\nexit: 0 OK, 1 WARNING, 2 CRITICAL, 3 UNKNOWN\n",
		GUARD_DEFAULT_SAMPLES,
		guard_limits_default.ratio_warn_pct,
		guard_limits_default.ratio_note_pct);
}

static void nap(int ms)
{
	struct timespec ts;

	if (ms <= 0)
		return;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	struct guard_limits lim = guard_limits_default;
	const char *sysfs_root = "/sys";
	struct guard_chip chips[GUARD_MAX_CHIPS];
	int samples = GUARD_DEFAULT_SAMPLES;
	int interval = 0;	/* 0 means "use the chip's update_interval" */
	int json = 0, quiet = 0;
	enum guard_level worst = GUARD_OK;
	int found, i, printed = 0;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (!strcmp(a, "--json")) {
			json = 1;
		} else if (!strcmp(a, "--quiet")) {
			quiet = 1;
		} else if (!strcmp(a, "--version")) {
			printf("astral-guard %s\n", GUARD_VERSION);
			return GUARD_OK;
		} else if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
			usage(stdout);
			return GUARD_OK;
		} else if (!strcmp(a, "--samples") && i + 1 < argc) {
			samples = atoi(argv[++i]);
		} else if (!strcmp(a, "--interval") && i + 1 < argc) {
			interval = atoi(argv[++i]);
		} else if (!strcmp(a, "--sysfs-root") && i + 1 < argc) {
			sysfs_root = argv[++i];
		} else if (!strcmp(a, "--warn-ratio") && i + 1 < argc) {
			lim.ratio_warn_pct = atoi(argv[++i]);
		} else if (!strcmp(a, "--note-ratio") && i + 1 < argc) {
			lim.ratio_note_pct = atoi(argv[++i]);
		} else {
			fprintf(stderr, "unknown or incomplete option: %s\n", a);
			usage(stderr);
			return GUARD_UNKNOWN;
		}
	}

	if (samples < 1 || samples > GUARD_MAX_SAMPLES) {
		fprintf(stderr, "--samples must be 1..%d\n", GUARD_MAX_SAMPLES);
		return GUARD_UNKNOWN;
	}

	found = guard_find_chips(sysfs_root, chips, GUARD_MAX_CHIPS);
	if (found < 0) {
		fprintf(stderr, "UNKNOWN cannot read %s/class/hwmon\n", sysfs_root);
		return GUARD_UNKNOWN;
	}
	if (found == 0) {
		fprintf(stderr,
			"UNKNOWN no %s chip found - is the astral-hwmon module loaded?\n",
			GUARD_CHIP_NAME);
		return GUARD_UNKNOWN;
	}

	if (json)
		printf("[\n");

	for (i = 0; i < found; i++) {
		struct guard_sample s[GUARD_MAX_SAMPLES];
		struct guard_verdict v;
		int gap = interval > 0 ? interval : chips[i].update_interval_ms;
		int k, ok = 1;

		for (k = 0; k < samples; k++) {
			if (guard_read_sample(&chips[i], &s[k]) != 0) {
				ok = 0;
				break;
			}
			if (k + 1 < samples)
				nap(gap);
		}

		memset(&v, 0, sizeof(v));
		if (!ok) {
			/* Losing sight of the current is a fault, not silence.
			 * A guard that treated "no reading" as "no alarm" would
			 * be worse than none.
			 */
			v.level = GUARD_UNKNOWN;
			v.n = 1;
			v.f[0].level = GUARD_UNKNOWN;
			v.f[0].rule = "read";
			v.f[0].pin = 0;
			v.f[0].value = 0;
			/* Explicit precision: the path can be far longer than a
			 * finding's detail field, and truncating a human
			 * message is fine as long as it is deliberate rather
			 * than a warning the build had to be told to ignore.
			 */
			snprintf(v.f[0].detail, sizeof(v.f[0].detail),
				 "could not read all channels from %.56s",
				 chips[i].path);
		} else {
			guard_eval(s, (size_t)samples, &lim, &v);
		}

		/* By severity, never by the raw value: UNKNOWN is 3 and
		 * CRITICAL is 2, so comparing them directly would let one
		 * card's unreadable sensor hide another card's overcurrent.
		 */
		if (guard_severity((int)v.level) > guard_severity((int)worst))
			worst = v.level;

		if (json) {
			guard_report_json(stdout, &chips[i], &v, &lim, !printed);
			printed++;
		} else if (!quiet || v.level != GUARD_OK) {
			guard_report_text(stdout, &chips[i], &v);
		}
	}

	if (json)
		printf("\n]\n");

	return (int)worst;
}
