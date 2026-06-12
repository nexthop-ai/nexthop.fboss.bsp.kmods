// SPDX-License-Identifier: GPL-2.0+
// Copyright (c) 2026 Nexthop Systems Inc.
// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/hwmon.h>
#include <linux/sysfs.h>
#include <linux/io.h>
#include <linux/pci.h>
#include "nh_fpga_fbiob.h"
#include "nh_fpga_asic_temp.h"
#include "platform/nh_platform.h"

#define DRIVER_NAME "nh_fpga_asic_temp"
#define DRIVER_VERSION "1.0"

/* ASIC Min/Max temperature status register.
 *
 * Single 32-bit register with two 13-bit fields:
 *
 *   bits[28:16]  Max temperature (13-bit unsigned)
 *   bits[12:0]   Min temperature (13-bit unsigned)
 *
 * The encoding from the 13-bit raw value to degC is not handled here;
 * the raw fields are surfaced as-is and any conversion is left to
 * userspace.
 *
 * base points at the device CSR block (the aux device is created with
 * the CSR offset already applied); the status register lives at
 * cfg->status_reg_offset within that block.
 */

struct nh_fpga_asic_temp {
	struct auxiliary_device *aux_dev;
	struct device *hwmon_dev;
	void __iomem *base;
	const struct asic_temp_config *cfg;
};

/* Sysfs attribute: asic_temp_max_raw — bits[28:16] of the status register. */
static ssize_t asic_temp_max_raw_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nh_fpga_asic_temp *t = dev_get_drvdata(dev);
	u32 reg = ioread32(t->base + t->cfg->status_reg_offset);

	return sysfs_emit(buf, "%u\n",
			  (reg & t->cfg->max_mask) >> t->cfg->max_shift);
}
static DEVICE_ATTR_RO(asic_temp_max_raw);

/* Sysfs attribute: asic_temp_min_raw — bits[12:0] of the status register. */
static ssize_t asic_temp_min_raw_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct nh_fpga_asic_temp *t = dev_get_drvdata(dev);
	u32 reg = ioread32(t->base + t->cfg->status_reg_offset);

	return sysfs_emit(buf, "%u\n",
			  (reg & t->cfg->min_mask) >> t->cfg->min_shift);
}
static DEVICE_ATTR_RO(asic_temp_min_raw);

static struct attribute *asic_temp_attrs[] = {
	&dev_attr_asic_temp_max_raw.attr,
	&dev_attr_asic_temp_min_raw.attr,
	NULL,
};

static const struct attribute_group asic_temp_group = {
	.attrs = asic_temp_attrs,
};

/* Register the attributes via the hwmon framework so they appear
 * under a hwmon[N] subdirectory of the aux device, which is the
 * convention userspace sensor-symlink machinery looks for.
 */
static const struct attribute_group *asic_temp_groups[] = {
	&asic_temp_group,
	NULL,
};

static int nh_fpga_asic_temp_probe(struct auxiliary_device *aux_dev,
				   const struct auxiliary_device_id *id)
{
	struct nh_fpga_aux_dev *fpga_aux_dev;
	const struct nh_platform_cfg *platform_cfg;
	struct nh_fpga_asic_temp *t;
	u16 device_id;

	fpga_aux_dev = container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);
	device_id = fpga_aux_dev->parent->pdev->device;

	platform_cfg = nh_get_platform(device_id);
	if (!platform_cfg || !platform_cfg->asic_temp_cfg) {
		dev_err(&aux_dev->dev,
			"No ASIC temp config for device ID: 0x%04x\n",
			device_id);
		return -ENODEV;
	}

	t = devm_kzalloc(&aux_dev->dev, sizeof(*t), GFP_KERNEL);
	if (!t)
		return -ENOMEM;

	t->aux_dev = aux_dev;
	t->base = fpga_aux_dev->csr_base;
	t->cfg = platform_cfg->asic_temp_cfg;

	t->hwmon_dev = devm_hwmon_device_register_with_groups(
		&aux_dev->dev, "nh_fpga_asic", t, asic_temp_groups);
	if (IS_ERR(t->hwmon_dev)) {
		dev_err(&aux_dev->dev, "Failed to register hwmon device: %ld\n",
			PTR_ERR(t->hwmon_dev));
		return PTR_ERR(t->hwmon_dev);
	}

	auxiliary_set_drvdata(aux_dev, t);

	dev_info(&aux_dev->dev, "NH FPGA ASIC temperature driver registered\n");
	return 0;
}

static void nh_fpga_asic_temp_remove(struct auxiliary_device *aux_dev)
{
	auxiliary_set_drvdata(aux_dev, NULL);
	dev_info(&aux_dev->dev, "NH FPGA ASIC temperature driver removed\n");
}

static const struct auxiliary_device_id nh_fpga_asic_temp_aux_id_table[] = {
	{
		.name = "fbiob_pci.asic_temp",
	},
	{},
};
MODULE_DEVICE_TABLE(auxiliary, nh_fpga_asic_temp_aux_id_table);

static struct auxiliary_driver nh_fpga_asic_temp_aux_driver = {
	.name = DRIVER_NAME,
	.probe = nh_fpga_asic_temp_probe,
	.remove = nh_fpga_asic_temp_remove,
	.id_table = nh_fpga_asic_temp_aux_id_table,
};

static int __init nh_fpga_asic_temp_init_module(void)
{
	int ret;

	pr_info("NH FPGA ASIC temp driver v%s loading\n", DRIVER_VERSION);

	ret = auxiliary_driver_register(&nh_fpga_asic_temp_aux_driver);
	if (ret) {
		pr_err("Failed to register auxiliary driver: %d\n", ret);
		return ret;
	}

	pr_info("NH FPGA ASIC temp driver loaded successfully\n");
	return 0;
}

static void __exit nh_fpga_asic_temp_exit_module(void)
{
	pr_info("NH FPGA ASIC temp driver unloading\n");
	auxiliary_driver_unregister(&nh_fpga_asic_temp_aux_driver);
}

module_init(nh_fpga_asic_temp_init_module);
module_exit(nh_fpga_asic_temp_exit_module);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Joy Wu <joy.wu@celestica.com>");
MODULE_AUTHOR("Anna Komarova <anna@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA ASIC Temperature Driver");
MODULE_VERSION(DRIVER_VERSION);
