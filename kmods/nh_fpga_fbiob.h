/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Nexthop Systems Inc. */

#ifndef _NH_FPGA_FBIOB_H_
#define _NH_FPGA_FBIOB_H_

#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/auxiliary_bus.h>

#include "platform_manager/uapi/fbiob-ioctl.h"

struct regmap;
struct regmap_irq_chip;
struct regmap_irq_chip_data;
struct clk_hw;
struct nh_fpga_irq_cfg;

/* Auxiliary device structure */
struct nh_fpga_aux_dev {
	struct auxiliary_device aux_dev;
	struct nh_fpga_pci_dev *parent;
	struct fbiob_aux_data dev_info;
	void __iomem *csr_base;
	void __iomem *iobuf_base;
	struct list_head node; /* For linking in the aux_dev_list */
};

/* PCI device structure */
struct nh_fpga_pci_dev {
	struct pci_dev *pdev;
	void __iomem *mmio_base; /**< Mapped MMIO base address */
	resource_size_t mmio_len; /**< Length of MMIO region */
	struct cdev cdev; /**< Character device structure */
	struct device *dev; /**< Device instance */
	struct mutex lock; /**< Device lock */
	int minor; /**< Minor number */
	struct list_head aux_dev_list; /**< List of auxiliary devices */

	/* Interrupt-driven I2C plumbing (see nh_fpga_irq.c); devm-managed. */
	const struct nh_fpga_irq_cfg *irq_cfg;
	struct regmap *regmap; /**< regmap over BAR0 for the irq chips */
	struct regmap_irq_chip *irq_chips; /**< one per MSI domain */
	struct regmap_irq_chip_data **irq_chip_data; /**< one per MSI domain */
	struct clk_hw *ref_clk_hw; /**< AXI IIC reference clock */
};

/* Function prototypes for auxiliary device management */
int nh_fpga_create_aux_device(struct nh_fpga_pci_dev *fbiob_dev,
			      struct fbiob_aux_data *dev_info);
int nh_fpga_remove_aux_device(struct nh_fpga_pci_dev *fbiob_dev,
			      struct fbiob_aux_id *dev_id);

#endif /* _NH_FPGA_FBIOB_H_ */
