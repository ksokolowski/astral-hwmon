// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/harness.c - assertions, corpus loader and programmable fakes.
 *
 * The corpus reader is a purpose-built scanner rather than a JSON library, to
 * keep this tier dependency-free. Every parse failure is fatal and loud: the
 * one outcome that must be impossible is a corpus that silently contributes no
 * cases, because that reads as a green run.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

/* Assertions live in tests/support/check.c, shared with the guard tier. */

/* ---- corpus -------------------------------------------------------------- */

static void fatal(const char *path, int line, const char *what)
{
	fprintf(stderr, "%s:%d: cannot parse corpus: %s\n", path, line, what);
	exit(2);
}

static const char *skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t')
		p++;
	return p;
}

/*
 * Locate "key": and return the first character of its value, or NULL.
 *
 * A plain substring search, so a key name appearing inside the free-text "note"
 * field would mislead it. That is tolerable only because every consequence is a
 * hard failure - a bad hex string, a missing array, or a case whose expected
 * values do not match - never a skipped case.
 */
static const char *value_of(const char *line, const char *key)
{
	char pattern[64];
	const char *p;

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(line, pattern);
	if (!p)
		return NULL;

	p = skip_ws(p + strlen(pattern));
	if (*p != ':')
		return NULL;
	return skip_ws(p + 1);
}

static int hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static size_t parse_hex_string(const char *p, u8 *out, size_t max,
			       const char *path, int line)
{
	size_t len = 0;
	int hi, lo;

	if (*p != '"')
		fatal(path, line, "\"raw\" is not a string");
	p++;

	while (*p && *p != '"') {
		hi = hex_digit(p[0]);
		lo = hex_digit(p[1]);
		if (hi < 0 || lo < 0)
			fatal(path, line, "\"raw\" is not an even run of hex digits");
		if (len >= max)
			fatal(path, line, "\"raw\" is longer than the buffer");
		out[len++] = (u8)((hi << 4) | lo);
		p += 2;
	}

	if (*p != '"')
		fatal(path, line, "unterminated \"raw\" string");
	if (!len)
		fatal(path, line, "\"raw\" is empty");
	return len;
}

static void parse_int_array(const char *p, int *out, size_t n, const char *key,
			    const char *path, int line)
{
	char *end;
	size_t i;

	if (*p != '[')
		fatal(path, line, key);
	p++;

	for (i = 0; i < n; i++) {
		p = skip_ws(p);
		out[i] = (int)strtol(p, &end, 10);
		if (end == p)
			fatal(path, line, key);
		p = skip_ws(end);
		if (i + 1 < n) {
			if (*p != ',')
				fatal(path, line, key);
			p++;
		}
	}

	p = skip_ws(p);
	if (*p != ']')
		fatal(path, line, key);
}

static void parse_note(const char *line, char *out, size_t size)
{
	const char *p = value_of(line, "note");
	size_t len = 0;

	out[0] = '\0';
	if (!p || *p != '"')
		return;
	p++;
	while (*p && *p != '"' && len + 1 < size)
		out[len++] = *p++;
	out[len] = '\0';
}

void astral_corpus_load(struct astral_corpus *out, const char *path)
{
	char line[4096];
	FILE *f;
	int lineno = 0;

	memset(out, 0, sizeof(*out));

	f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "cannot open corpus %s\n", path);
		exit(2);
	}

	while (fgets(line, sizeof(line), f)) {
		struct astral_corpus_case *c;
		const char *raw, *reject, *ma, *mv;

		lineno++;
		if (!*skip_ws(line) || *skip_ws(line) == '\n')
			continue;

		if (out->count >= ASTRAL_CORPUS_MAX)
			fatal(path, lineno, "more cases than ASTRAL_CORPUS_MAX");

		c = &out->cases[out->count];
		memset(c, 0, sizeof(*c));
		c->line = lineno;
		parse_note(line, c->note, sizeof(c->note));

		raw = value_of(line, "raw");
		if (!raw)
			fatal(path, lineno, "no \"raw\" field");
		c->raw_len = parse_hex_string(raw, c->raw, sizeof(c->raw), path,
					      lineno);

		reject = value_of(line, "reject");
		c->reject = reject && strncmp(reject, "true", 4) == 0;

		if (c->reject) {
			out->rejects++;
		} else {
			ma = value_of(line, "expect_ma");
			mv = value_of(line, "expect_mv");
			if (!ma || !mv)
				fatal(path, lineno,
				      "an accepted case needs expect_ma and expect_mv");
			parse_int_array(ma, c->expect_ma, ASTRAL_CORPUS_PINS,
					"expect_ma", path, lineno);
			parse_int_array(mv, c->expect_mv, ASTRAL_CORPUS_PINS,
					"expect_mv", path, lineno);
			out->accepts++;
		}

		out->count++;
	}

	fclose(f);
}

/* ---- fake i2c ------------------------------------------------------------ */

struct astral_fake_i2c astral_fake_i2c;

void astral_fake_i2c_set(const u8 *data, size_t len, s32 result)
{
	memset(&astral_fake_i2c, 0, sizeof(astral_fake_i2c));
	if (data && len) {
		if (len > sizeof(astral_fake_i2c.data))
			len = sizeof(astral_fake_i2c.data);
		memcpy(astral_fake_i2c.data, data, len);
	}
	astral_fake_i2c.result = result;
}

s32 i2c_smbus_read_i2c_block_data(const struct i2c_client *client, u8 command,
				  u8 length, u8 *values)
{
	size_t copy;

	(void)client;
	(void)command;

	astral_fake_i2c.calls++;
	if (astral_fake_i2c.result < 0)
		return astral_fake_i2c.result;

	/* The real call fills only as many bytes as it returns. Copying more
	 * would hide a driver that reads past what the transfer produced.
	 */
	copy = (size_t)astral_fake_i2c.result;
	if (copy > length)
		copy = length;
	if (copy > sizeof(astral_fake_i2c.data))
		copy = sizeof(astral_fake_i2c.data);
	memcpy(values, astral_fake_i2c.data, copy);

	return astral_fake_i2c.result;
}

/* ---- fake pci ------------------------------------------------------------ */

#define ASTRAL_FAKE_PCI_MAX 8

static struct pci_dev fake_pci[ASTRAL_FAKE_PCI_MAX];
static size_t fake_pci_count;
static int pci_gets;
static int pci_puts;

void astral_fake_pci_reset(void)
{
	memset(fake_pci, 0, sizeof(fake_pci));
	fake_pci_count = 0;
	pci_gets = 0;
	pci_puts = 0;
}

struct pci_dev *astral_fake_pci_add(u32 vendor, u32 subsystem_vendor,
				    u32 subsystem_device)
{
	struct pci_dev *pdev;

	if (fake_pci_count >= ASTRAL_FAKE_PCI_MAX) {
		fprintf(stderr, "fake pci registry full\n");
		exit(2);
	}

	pdev = &fake_pci[fake_pci_count++];
	pdev->vendor = vendor;
	pdev->device = 0x2b85;
	pdev->subsystem_vendor = subsystem_vendor;
	pdev->subsystem_device = subsystem_device;
	pdev->dev.is_pci = true;
	pdev->dev.parent = NULL;
	return pdev;
}

int astral_fake_pci_gets(void)
{
	return pci_gets;
}

int astral_fake_pci_puts(void)
{
	return pci_puts;
}

static bool id_matches(u32 want, u32 have)
{
	return want == PCI_ANY_ID || want == have;
}

struct pci_dev *pci_get_subsys(u32 vendor, u32 device, u32 ss_vendor,
			       u32 ss_device, struct pci_dev *from)
{
	size_t i = from ? (size_t)(from - fake_pci) + 1 : 0;

	for (; i < fake_pci_count; i++) {
		if (id_matches(vendor, fake_pci[i].vendor) &&
		    id_matches(device, fake_pci[i].device) &&
		    id_matches(ss_vendor, fake_pci[i].subsystem_vendor) &&
		    id_matches(ss_device, fake_pci[i].subsystem_device)) {
			pci_gets++;
			return &fake_pci[i];
		}
	}

	return NULL;
}

struct pci_dev *pci_get_device(u32 vendor, u32 device, struct pci_dev *from)
{
	return pci_get_subsys(vendor, device, PCI_ANY_ID, PCI_ANY_ID, from);
}

void pci_dev_put(struct pci_dev *dev)
{
	if (dev)
		pci_puts++;
}
