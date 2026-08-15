// SPDX-License-Identifier: MIT
/* tests/guard/corpus.c - scanner for tests/data/guard-cases.jsonl.
 *
 * A purpose-built scanner rather than a JSON library, to keep this tier
 * dependency-free. Every parse failure is fatal and loud: the one outcome that
 * must be impossible is a corpus that silently contributes no cases, because
 * that reads as a green run.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "corpus.h"

static void fatal(const char *path, int line, const char *what)
{
	fprintf(stderr, "corpus %s line %d: %s\n", path, line, what);
	exit(2);
}

/* Locate "key": and return the first character of its value, or NULL.
 *
 * A plain substring search, so a key name appearing inside the free-text "note"
 * field would mislead it. Tolerable only because every consequence is a loud
 * parse failure rather than a silently wrong case - and "note" is read first.
 */
static const char *find_key(const char *s, const char *key)
{
	char pat[64];
	const char *p;

	snprintf(pat, sizeof(pat), "\"%s\"", key);
	p = strstr(s, pat);
	if (!p)
		return NULL;
	p = strchr(p + strlen(pat), ':');
	return p ? p + 1 : NULL;
}

/* Reads a flat [n, n, n] array. Returns the count written. */
static size_t read_ints(const char *p, int *out, size_t max,
			const char *path, int line)
{
	size_t n = 0;

	p = strchr(p, '[');
	if (!p)
		fatal(path, line, "expected an array");
	p++;
	while (*p && *p != ']') {
		char *end;
		long v;

		while (*p == ' ' || *p == ',')
			p++;
		if (*p == ']')
			break;
		v = strtol(p, &end, 10);
		if (end == p)
			fatal(path, line, "malformed number in array");
		if (n == max)
			fatal(path, line, "array longer than the case can hold");
		out[n++] = (int)v;
		p = end;
	}
	if (*p != ']')
		fatal(path, line, "unterminated array");
	return n;
}

static void read_string(const char *p, char *out, size_t max)
{
	size_t n = 0;

	p = strchr(p, '"');
	if (!p) {
		out[0] = '\0';
		return;
	}
	p++;
	while (*p && *p != '"' && n + 1 < max)
		out[n++] = *p++;
	out[n] = '\0';
}

static enum guard_level parse_level(const char *p, const char *path, int line)
{
	char buf[16];

	read_string(p, buf, sizeof(buf));
	if (!strcmp(buf, "OK"))
		return GUARD_OK;
	if (!strcmp(buf, "WARN"))
		return GUARD_WARN;
	if (!strcmp(buf, "CRIT"))
		return GUARD_CRIT;
	if (!strcmp(buf, "UNKNOWN"))
		return GUARD_UNKNOWN;
	fatal(path, line, "expect must be OK, WARN, CRIT or UNKNOWN");
	return GUARD_UNKNOWN;
}

/* Broadcast: 6 values apply to every sample, 6*samples give one block each. */
static void spread(const int *flat, size_t got, size_t samples,
		   struct guard_case *c, bool voltage,
		   const char *path, int line)
{
	size_t i, pin;

	if (got != GUARD_PINS && got != GUARD_PINS * samples)
		fatal(path, line, "array must hold 6 or 6*samples values");

	for (i = 0; i < samples; i++) {
		for (pin = 0; pin < GUARD_PINS; pin++) {
			int v = (got == GUARD_PINS)
				? flat[pin]
				: flat[i * GUARD_PINS + pin];

			if (voltage)
				c->s[i].mv[pin] = v;
			else
				c->s[i].ma[pin] = v;
		}
	}
}

void guard_corpus_load(struct guard_corpus *out, const char *path)
{
	char buf[8192];
	FILE *fh = fopen(path, "r");
	int line = 0;

	memset(out, 0, sizeof(*out));
	if (!fh) {
		fprintf(stderr, "cannot open corpus %s\n", path);
		exit(2);
	}

	while (fgets(buf, sizeof(buf), fh)) {
		struct guard_case *c;
		const char *p;
		int flat[GUARD_PINS * GUARD_MAX_SAMPLES];
		size_t got;
		long samples;

		line++;
		if (buf[0] == '\n' || buf[0] == '#')
			continue;
		if (out->count == GUARD_CORPUS_MAX)
			fatal(path, line, "more cases than GUARD_CORPUS_MAX");

		c = &out->cases[out->count];
		memset(c, 0, sizeof(*c));
		c->line = line;

		p = find_key(buf, "note");
		if (p)
			read_string(p, c->note, sizeof(c->note));

		p = find_key(buf, "samples");
		if (!p)
			fatal(path, line, "missing \"samples\"");
		samples = strtol(p, NULL, 10);
		if (samples < 1 || samples > GUARD_MAX_SAMPLES)
			fatal(path, line, "samples out of range");
		c->samples = (size_t)samples;

		/* Compare the value token itself, not the rest of the line: a
		 * later "true" anywhere would otherwise mark every case as
		 * expecting a note.
		 */
		p = find_key(buf, "expect_note");
		if (p) {
			p += strspn(p, " \t");
			c->expect_note = !strncmp(p, "true", 4);
		}

		p = find_key(buf, "expect");
		if (!p)
			fatal(path, line, "missing \"expect\"");
		c->expect = parse_level(p, path, line);

		p = find_key(buf, "mv");
		if (!p)
			fatal(path, line, "missing \"mv\"");
		got = read_ints(p, flat, sizeof(flat) / sizeof(flat[0]), path, line);
		spread(flat, got, c->samples, c, true, path, line);

		p = find_key(buf, "ma");
		if (!p)
			fatal(path, line, "missing \"ma\"");
		got = read_ints(p, flat, sizeof(flat) / sizeof(flat[0]), path, line);
		spread(flat, got, c->samples, c, false, path, line);

		out->count++;
	}

	fclose(fh);
	if (out->count == 0)
		fatal(path, 0, "corpus is empty");
}
