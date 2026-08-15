/* SPDX-License-Identifier: GPL-2.0-only */
/* Host stand-in for <linux/pci.h>. See tests/native/README.md.
 *
 * Backed by a settable device list in harness.c, so the PCI gate - the driver's
 * most safety-critical decision - can be tested without the hardware it gates.
 */
#ifndef ASTRAL_TEST_LINUX_PCI_H
#define ASTRAL_TEST_LINUX_PCI_H

#include <linux/device.h>
#include <linux/types.h>

#define PCI_ANY_ID (~0u)

struct pci_dev {
	struct device dev;
	u32 vendor;
	u32 device;
	u32 subsystem_vendor;
	u32 subsystem_device;
};

struct pci_device_id {
	u32 vendor, device, subvendor, subdevice;
	u32 class, class_mask;
	unsigned long driver_data;
};

#define PCI_DEVICE_SUB(vend, dev, subvend, subdev)                  \
	.vendor = (vend), .device = (dev), .subvendor = (subvend),  \
	.subdevice = (subdev)

#define dev_is_pci(d) ((d)->is_pci)

/* Mirrors the kernel's container_of() rather than storing a back pointer, so a
 * struct layout mistake in the shim shows up here instead of silently working.
 */
#define to_pci_dev(d) \
	((struct pci_dev *)((char *)(d) - offsetof(struct pci_dev, dev)))

struct pci_dev *pci_get_subsys(u32 vendor, u32 device, u32 ss_vendor,
			       u32 ss_device, struct pci_dev *from);
struct pci_dev *pci_get_device(u32 vendor, u32 device, struct pci_dev *from);
void pci_dev_put(struct pci_dev *dev);

#endif /* ASTRAL_TEST_LINUX_PCI_H */
