// SPDX-License-Identifier: GPL-2.0-only
/* driver/astral_detect.c - decide whether an adapter belongs to a card we may
 * talk to. Mirrors src/astral_oracle/discover.py.
 *
 * Nothing enumerates an I2C chip on a proprietary GPU's bus - no ACPI, no
 * device tree - and the NVIDIA adapters do not advertise I2C_CLASS_HWMON, so
 * the usual .detect() auto-probe never fires. Hence explicit matching.
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "astral.h"

#define PCI_VENDOR_NVIDIA	0x10de

struct astral_model {
	u16 subsystem_vendor;
	u16 subsystem_device;
	const char *name;
};

/*
 * Published ASUS Astral SKU ids (LibreHardwareMonitor's device list, the OpenRGB
 * device registry) plus owner-reported lspci output. Only 0x89E31043 has been
 * read on Linux here; the rest assume an identical sensor block across the family.
 *
 * One list, two consumers: the runtime allowlist below and the PCI device table
 * that makes udev autoload this module only on machines that actually have a
 * card. Generated from the same X-macro so they cannot drift apart.
 */
#define ASTRAL_MODEL_LIST(X)						\
	X(0x1043, 0x89ea, "ROG Astral RTX 5090D OC")			\
	X(0x1043, 0x8a61, "ROG Astral RTX 5090 Matrix")			\
	X(0x1043, 0x89ec, "ROG Astral RTX 5090 LC")			\
	X(0x1043, 0x89e3, "ROG Astral RTX 5090 OC")			\
	X(0x1043, 0x89de, "ROG Astral RTX 5080 OC")			\
	X(0x1043, 0x8a2e, "ROG Astral RTX 5090 OC White")		\
	X(0x1043, 0x8a2b, "ROG Astral RTX 5080 OC White")		\
	X(0x1043, 0x8a45, "ROG Astral RTX 5080 OC Hatsune Miku")

#define ASTRAL_MODEL_ROW(sv, sd, model)	{ (sv), (sd), (model) },
static const struct astral_model astral_models[] = {
	ASTRAL_MODEL_LIST(ASTRAL_MODEL_ROW)
};

/*
 * Makes the module's modalias match these cards, so udev loads it only where a
 * supported card exists. No pci_driver is registered - we bind over i2c, not
 * PCI - this table exists purely for autoload.
 */
#define ASTRAL_PCI_ROW(sv, sd, model)					\
	{ PCI_DEVICE_SUB(PCI_VENDOR_NVIDIA, PCI_ANY_ID, (sv), (sd)) },
static const struct pci_device_id astral_pci_ids[] = {
	ASTRAL_MODEL_LIST(ASTRAL_PCI_ROW)
	{ }
};
MODULE_DEVICE_TABLE(pci, astral_pci_ids);

static const char *astral_model_name(struct pci_dev *pdev)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(astral_models); i++) {
		if (astral_models[i].subsystem_vendor == pdev->subsystem_vendor &&
		    astral_models[i].subsystem_device == pdev->subsystem_device)
			return astral_models[i].name;
	}

	return NULL;
}

/*
 * Stage 1 of the load strategy: is a supported card physically in this machine?
 *
 * Answered from the PCI bus, which is enumerated long before the nvidia driver
 * loads, so this works even when no i2c adapters exist yet. That is what lets
 * the module tell "wrong machine, do not load" apart from "right machine, the
 * GPU driver just is not up yet".
 */
bool astral_card_present(bool allow_unknown, const char **model_out)
{
	struct pci_dev *pdev;
	size_t i;

	for (i = 0; i < ARRAY_SIZE(astral_models); i++) {
		pdev = pci_get_subsys(PCI_VENDOR_NVIDIA, PCI_ANY_ID,
				      astral_models[i].subsystem_vendor,
				      astral_models[i].subsystem_device, NULL);
		if (pdev) {
			if (model_out)
				*model_out = astral_models[i].name;
			pci_dev_put(pdev);
			return true;
		}
	}

	if (!allow_unknown)
		return false;

	pdev = pci_get_device(PCI_VENDOR_NVIDIA, PCI_ANY_ID, NULL);
	if (pdev) {
		if (model_out)
			*model_out = "unrecognised NVIDIA GPU (allow_unknown)";
		pci_dev_put(pdev);
		return true;
	}

	return false;
}

/*
 * The adapter name is "NVIDIA i2c adapter N at <pci>". Parsing N is deliberate:
 * an exact strcmp against "NVIDIA i2c adapter 1" matches nothing on a real
 * system, and a plain prefix compare would also accept "adapter 10".
 *
 * Restricting to adapter 1 also means 0x2b is never probed on the six other
 * buses the card exposes, where something unrelated could answer.
 */
bool astral_is_sensor_adapter(struct i2c_adapter *adap)
{
	int index = 0;

	if (sscanf(adap->name, ASTRAL_ADAPTER_FMT, &index) != 1)
		return false;

	return index == ASTRAL_ADAPTER_INDEX;
}

/*
 * Identify the card by PCI id before any I2C traffic happens. This is the
 * gate that keeps the driver off buses that are not ours: 0x2b on the board's
 * SMBus is somebody else's device entirely, and even a register-pointer write
 * to the wrong chip is a write we have no business making.
 *
 * Deliberately not overridable by bus=: that parameter chooses which of THIS
 * card's adapters to use, never which machine's bus to poke.
 */
bool astral_card_matches(struct i2c_adapter *adap, bool allow_unknown,
			 const char **model_out)
{
	struct device *parent = adap->dev.parent;
	struct pci_dev *pdev;
	const char *model;

	if (!parent || !dev_is_pci(parent))
		return false;

	pdev = to_pci_dev(parent);
	if (pdev->vendor != PCI_VENDOR_NVIDIA)
		return false;

	model = astral_model_name(pdev);
	if (!model && !allow_unknown)
		return false;

	if (model_out)
		*model_out = model ? model : "unknown Astral variant";

	return true;
}
