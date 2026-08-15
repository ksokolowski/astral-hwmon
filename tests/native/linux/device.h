/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <linux/device.h>. See tests/native/README.md. */
#ifndef ASTRAL_TEST_LINUX_DEVICE_H
#define ASTRAL_TEST_LINUX_DEVICE_H

#include <linux/types.h>

/*
 * Only the two fields the driver's detection path reads. `is_pci` stands in for
 * the real test, dev->bus == &pci_bus_type, which needs a bus type registry the
 * host build has no use for.
 */
struct device {
	struct device *parent;
	bool is_pci;
};

#endif /* ASTRAL_TEST_LINUX_DEVICE_H */
