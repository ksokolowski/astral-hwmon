// SPDX-License-Identifier: GPL-2.0-only
/* tests/native/test_detect.c - the PCI gate and the adapter-name parser.
 *
 * The gate is the driver's most safety-critical decision and until now could
 * only be exercised on a machine with the card in it. Driven here against the
 * fake PCI registry in harness.c.
 *
 * astral_detect.c is #included rather than linked so the tests can reach
 * astral_models[] and astral_pci_ids[], which are static. It must therefore not
 * also be compiled as its own object - see the native tier's Makefile rules.
 */

#include <stdio.h>
#include <string.h>

#include "harness.h"

#include "astral_detect.c"

#define ASTRAL_TEST_NVIDIA 0x10de
#define ASTRAL_TEST_INTEL 0x8086

/* The reference machine: subsystem 0x89E31043, a ROG Astral RTX 5090 OC. */
#define REF_SUBVENDOR 0x1043
#define REF_SUBDEVICE 0x89e3
#define REF_MODEL "ROG Astral RTX 5090 OC"

#define SENSOR_ADAPTER_NAME "NVIDIA i2c adapter 1 at 1:00.0"

static struct i2c_adapter make_adapter(const char *name, struct pci_dev *parent)
{
	struct i2c_adapter adap;

	memset(&adap, 0, sizeof(adap));
	snprintf(adap.name, sizeof(adap.name), "%s", name);
	adap.dev.parent = parent ? &parent->dev : NULL;
	return adap;
}

/*
 * The name carries a PCI suffix, so equality never matches; and a prefix
 * compare would accept "adapter 10". Both were real mistakes.
 */
static void test_adapter_index_is_parsed_not_compared(void)
{
	static const struct {
		const char *name;
		bool want;
	} cases[] = {
		{ SENSOR_ADAPTER_NAME, true },
		{ "NVIDIA i2c adapter 1", true },
		{ "NVIDIA i2c adapter 0 at 1:00.0", false },
		{ "NVIDIA i2c adapter 2 at 1:00.0", false },
		{ "NVIDIA i2c adapter 10 at 1:00.0", false },
		{ "NVIDIA i2c adapter 11 at 1:00.0", false },
		{ "SMBus PIIX4 adapter port 0 at 0b00", false },
		{ "i2c-MSFT8000:01", false },
		{ "NVIDIA i2c adapter", false },
		{ "", false },
	};
	size_t i;

	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct i2c_adapter adap = make_adapter(cases[i].name, NULL);
		bool got = astral_is_sensor_adapter(&adap);

		CHECK(got == cases[i].want, "\"%s\": got %d, expected %d",
		      cases[i].name, got, cases[i].want);
	}
}

static void test_gate_requires_a_pci_parent(void)
{
	struct i2c_adapter orphan = make_adapter(SENSOR_ADAPTER_NAME, NULL);
	struct device non_pci;
	struct i2c_adapter adap = make_adapter(SENSOR_ADAPTER_NAME, NULL);

	CHECK(!astral_card_matches(&orphan, false, NULL),
	      "an adapter with no parent passed the gate");
	CHECK(!astral_card_matches(&orphan, true, NULL),
	      "allow_unknown let a parentless adapter through");

	memset(&non_pci, 0, sizeof(non_pci));
	non_pci.is_pci = false;
	adap.dev.parent = &non_pci;
	CHECK(!astral_card_matches(&adap, false, NULL),
	      "an adapter on a non-PCI parent passed the gate");
	CHECK(!astral_card_matches(&adap, true, NULL),
	      "allow_unknown let a non-PCI parent through");
}

/* This is the one that keeps the driver off the board's SMBus, where 0x2b is
 * somebody else's device entirely.
 */
static void test_gate_requires_nvidia_vendor(void)
{
	struct pci_dev *intel;
	struct i2c_adapter adap;

	astral_fake_pci_reset();
	intel = astral_fake_pci_add(ASTRAL_TEST_INTEL, REF_SUBVENDOR,
				    REF_SUBDEVICE);
	adap = make_adapter(SENSOR_ADAPTER_NAME, intel);

	CHECK(!astral_card_matches(&adap, false, NULL),
	      "a non-NVIDIA card passed the gate");
	CHECK(!astral_card_matches(&adap, true, NULL),
	      "allow_unknown widened the gate past the vendor check");
}

static void test_gate_accepts_every_listed_model(void)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(astral_models); i++) {
		struct pci_dev *pdev;
		struct i2c_adapter adap;
		const char *model = NULL;

		astral_fake_pci_reset();
		pdev = astral_fake_pci_add(ASTRAL_TEST_NVIDIA,
					   astral_models[i].subsystem_vendor,
					   astral_models[i].subsystem_device);
		adap = make_adapter(SENSOR_ADAPTER_NAME, pdev);

		CHECK(astral_card_matches(&adap, false, &model),
		      "%s was not matched", astral_models[i].name);
		CHECK(model && strcmp(model, astral_models[i].name) == 0,
		      "matched %s but reported \"%s\"", astral_models[i].name,
		      model ? model : "(null)");
	}
}

static void test_gate_handles_unknown_nvidia_cards(void)
{
	struct pci_dev *pdev;
	struct i2c_adapter adap;
	const char *model = NULL;

	astral_fake_pci_reset();
	pdev = astral_fake_pci_add(ASTRAL_TEST_NVIDIA, REF_SUBVENDOR, 0xffff);
	adap = make_adapter(SENSOR_ADAPTER_NAME, pdev);

	CHECK(!astral_card_matches(&adap, false, &model),
	      "an unlisted NVIDIA card passed without allow_unknown");
	CHECK(astral_card_matches(&adap, true, &model),
	      "allow_unknown did not admit an unlisted NVIDIA card");
	CHECK(model && strcmp(model, "unknown Astral variant") == 0,
	      "unlisted card reported as \"%s\"", model ? model : "(null)");
}

/*
 * The X-macro's whole purpose: the runtime allowlist and the modalias table are
 * generated from one list and cannot drift. A hand-edit to either would show up
 * here rather than as a card that autoloads but is then refused, or the reverse.
 */
static void test_model_table_matches_pci_table(void)
{
	size_t models = ARRAY_SIZE(astral_models);
	size_t ids = ARRAY_SIZE(astral_pci_ids);
	size_t i;

	CHECK(ids == models + 1,
	      "%zu pci ids for %zu models (expected one terminator)", ids,
	      models);
	if (ids != models + 1)
		return;

	for (i = 0; i < models; i++) {
		CHECK(astral_pci_ids[i].subvendor ==
			      astral_models[i].subsystem_vendor,
		      "row %zu (%s): subvendor %#x vs %#x", i,
		      astral_models[i].name, astral_pci_ids[i].subvendor,
		      astral_models[i].subsystem_vendor);
		CHECK(astral_pci_ids[i].subdevice ==
			      astral_models[i].subsystem_device,
		      "row %zu (%s): subdevice %#x vs %#x", i,
		      astral_models[i].name, astral_pci_ids[i].subdevice,
		      astral_models[i].subsystem_device);
		CHECK(astral_pci_ids[i].vendor == PCI_VENDOR_NVIDIA,
		      "row %zu (%s): vendor is %#x", i, astral_models[i].name,
		      astral_pci_ids[i].vendor);
		CHECK(astral_pci_ids[i].device == PCI_ANY_ID,
		      "row %zu (%s): device is not PCI_ANY_ID", i,
		      astral_models[i].name);
	}

	CHECK(astral_pci_ids[models].vendor == 0 &&
		      astral_pci_ids[models].subvendor == 0,
	      "the pci table is not zero-terminated");
}

static void test_card_present_reads_the_pci_bus(void)
{
	const char *model = NULL;

	astral_fake_pci_reset();
	CHECK(!astral_card_present(false, &model),
	      "reported a card on an empty bus");
	CHECK(!astral_card_present(true, &model),
	      "allow_unknown invented a card on an empty bus");

	astral_fake_pci_reset();
	astral_fake_pci_add(ASTRAL_TEST_INTEL, 0x1043, 0x1234);
	CHECK(!astral_card_present(false, &model),
	      "a non-NVIDIA device was reported as a card");
	CHECK(!astral_card_present(true, &model),
	      "allow_unknown accepted a non-NVIDIA device");

	astral_fake_pci_reset();
	astral_fake_pci_add(ASTRAL_TEST_NVIDIA, REF_SUBVENDOR, REF_SUBDEVICE);
	model = NULL;
	CHECK(astral_card_present(false, &model),
	      "the reference card was not detected");
	CHECK(model && strcmp(model, REF_MODEL) == 0,
	      "reference card reported as \"%s\"", model ? model : "(null)");

	astral_fake_pci_reset();
	astral_fake_pci_add(ASTRAL_TEST_NVIDIA, REF_SUBVENDOR, 0xffff);
	model = NULL;
	CHECK(!astral_card_present(false, &model),
	      "an unlisted NVIDIA card was reported without allow_unknown");
	CHECK(astral_card_present(true, &model),
	      "allow_unknown did not detect an unlisted NVIDIA card");
	CHECK(model && strcmp(model, "unrecognised NVIDIA GPU (allow_unknown)") == 0,
	      "unlisted card reported as \"%s\"", model ? model : "(null)");
}

/* Every pci_get_*() takes a reference the caller must drop. A leak here would
 * pin the GPU's PCI device for the lifetime of the machine.
 */
static void test_card_present_balances_pci_references(void)
{
	astral_fake_pci_reset();
	astral_fake_pci_add(ASTRAL_TEST_NVIDIA, REF_SUBVENDOR, REF_SUBDEVICE);
	astral_card_present(false, NULL);
	CHECK(astral_fake_pci_gets() == astral_fake_pci_puts(),
	      "matched card: %d gets vs %d puts", astral_fake_pci_gets(),
	      astral_fake_pci_puts());

	astral_fake_pci_reset();
	astral_fake_pci_add(ASTRAL_TEST_NVIDIA, REF_SUBVENDOR, 0xffff);
	astral_card_present(true, NULL);
	CHECK(astral_fake_pci_gets() == astral_fake_pci_puts(),
	      "allow_unknown path: %d gets vs %d puts", astral_fake_pci_gets(),
	      astral_fake_pci_puts());
}

void run_detect_tests(void)
{
	printf("astral_detect.c (%zu models in the allowlist)\n",
	       ARRAY_SIZE(astral_models));

	RUN(test_adapter_index_is_parsed_not_compared);
	RUN(test_gate_requires_a_pci_parent);
	RUN(test_gate_requires_nvidia_vendor);
	RUN(test_gate_accepts_every_listed_model);
	RUN(test_gate_handles_unknown_nvidia_cards);
	RUN(test_model_table_matches_pci_table);
	RUN(test_card_present_reads_the_pci_bus);
	RUN(test_card_present_balances_pci_references);
}
