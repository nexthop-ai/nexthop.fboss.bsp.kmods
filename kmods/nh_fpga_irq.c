// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#include <linux/bitops.h>
#include <linux/clk-provider.h>
#include <linux/device.h>
#include <linux/pci.h>
#include <linux/regmap.h>
#include <linux/slab.h>

#include "nh_fpga_fbiob.h"
#include "nh_fpga_irq.h"

/* One regmap_irq per status-register bit; unused bits never get unmasked. */
#define NH_FPGA_IRQS_PER_DOMAIN 32

static const struct regmap_config nh_fpga_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
};

static int nh_fpga_init_one_chip(struct nh_fpga_pci_dev *fbiob_dev, int idx)
{
	struct device *dev = &fbiob_dev->pdev->dev;
	const struct nh_fpga_msi_domain_cfg *dom =
		&fbiob_dev->irq_cfg->domains[idx];
	struct regmap_irq_chip *chip = &fbiob_dev->irq_chips[idx];
	struct regmap_irq *irqs;
	int linux_irq, i, ret;

	irqs = devm_kcalloc(dev, NH_FPGA_IRQS_PER_DOMAIN, sizeof(*irqs),
			    GFP_KERNEL);
	if (!irqs)
		return -ENOMEM;

	for (i = 0; i < NH_FPGA_IRQS_PER_DOMAIN; i++) {
		irqs[i].reg_offset = 0;
		irqs[i].mask = BIT(i);
	}

	chip->name = dom->chip_name;
	chip->status_base = dom->isr_offset;
	chip->unmask_base = dom->ier_offset;
	chip->ack_base = dom->isr_offset; /* write 1 to clear via status reg */
	chip->num_regs = 1;
	chip->irqs = irqs;
	chip->num_irqs = NH_FPGA_IRQS_PER_DOMAIN;

	linux_irq = pci_irq_vector(fbiob_dev->pdev, idx);
	if (linux_irq < 0) {
		dev_err(dev, "Failed to get Linux IRQ for MSI domain %d: %d\n",
			idx, linux_irq);
		return linux_irq;
	}

	ret = devm_regmap_add_irq_chip(dev, fbiob_dev->regmap, linux_irq, 0, 0,
				       chip, &fbiob_dev->irq_chip_data[idx]);
	if (ret) {
		dev_err(dev,
			"Failed to register regmap_irq_chip for domain %d: %d\n",
			idx, ret);
		return ret;
	}

	dev_info(
		dev,
		"Registered regmap_irq_chip '%s' for MSI domain %d on Linux IRQ %d\n",
		dom->chip_name, idx, linux_irq);
	return 0;
}

int nh_fpga_irq_init(struct nh_fpga_pci_dev *fbiob_dev)
{
	struct device *dev = &fbiob_dev->pdev->dev;
	const struct nh_fpga_irq_cfg *cfg = fbiob_dev->irq_cfg;
	int ret, i;

	/* Platform without interrupt support: nothing to set up. */
	if (!cfg)
		return 0;

	fbiob_dev->regmap = devm_regmap_init_mmio(dev, fbiob_dev->mmio_base,
						  &nh_fpga_regmap_config);
	if (IS_ERR(fbiob_dev->regmap)) {
		dev_err(dev, "Failed to initialize regmap: %ld\n",
			PTR_ERR(fbiob_dev->regmap));
		return PTR_ERR(fbiob_dev->regmap);
	}

	fbiob_dev->irq_chips = devm_kcalloc(dev, cfg->num_domains,
					    sizeof(*fbiob_dev->irq_chips),
					    GFP_KERNEL);
	if (!fbiob_dev->irq_chips)
		return -ENOMEM;

	fbiob_dev->irq_chip_data =
		devm_kcalloc(dev, cfg->num_domains,
			     sizeof(*fbiob_dev->irq_chip_data), GFP_KERNEL);
	if (!fbiob_dev->irq_chip_data)
		return -ENOMEM;

	for (i = 0; i < cfg->num_domains; i++) {
		ret = nh_fpga_init_one_chip(fbiob_dev, i);
		if (ret)
			return ret;
	}
	return 0;
}

int nh_fpga_resolve_master_irq_clk(struct nh_fpga_pci_dev *fbiob_dev,
				   u32 csr_offset, int *virq_out,
				   struct clk_hw **clk_hw_out)
{
	const struct nh_fpga_irq_cfg *cfg;
	int virq, master_index, i;
	u8 msi_domain = 0, hw_irq = 0;

	if (!fbiob_dev || !virq_out || !clk_hw_out)
		return -EINVAL;

	cfg = fbiob_dev->irq_cfg;
	if (!cfg)
		return -ENOENT;

	/* Master index from the CSR offset (ties the IRQ to the MMIO window). */
	if (csr_offset < cfg->i2c_csr_base || !cfg->i2c_csr_channel_size)
		return -ENOENT;
	master_index =
		(csr_offset - cfg->i2c_csr_base) / cfg->i2c_csr_channel_size;

	for (i = 0; i < cfg->num_domains; i++) {
		const struct nh_fpga_msi_domain_cfg *dom = &cfg->domains[i];

		if (master_index >= dom->first_master &&
		    master_index <= dom->last_master) {
			msi_domain = i;
			hw_irq = dom->first_hw_irq +
				 (master_index - dom->first_master);
			break;
		}
	}
	if (i == cfg->num_domains)
		return -ENOENT;

	if (msi_domain >= cfg->num_domains ||
	    !fbiob_dev->irq_chip_data[msi_domain])
		return -ENODEV;

	virq = regmap_irq_get_virq(fbiob_dev->irq_chip_data[msi_domain],
				   hw_irq);
	if (virq < 0)
		return virq;
	if (virq == 0)
		return -ENODEV;

	*virq_out = virq;
	*clk_hw_out = fbiob_dev->ref_clk_hw;
	return 0;
}
EXPORT_SYMBOL_GPL(nh_fpga_resolve_master_irq_clk);
