// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/sysfs.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/hwmon.h>
#include "nh_fpga_fbiob.h"
#include "nh_fpga_psu.h"
#include "platform/nh_platform.h"

#define DRIVER_NAME "nh_fpga_psu"
#define DRIVER_VERSION "0.1"

struct nh_fpga_psu {
	struct auxiliary_device *aux_dev;
	struct device *hwmon_dev;
	void __iomem *base;
	const struct psu_present_cfg *cfg;
	/* Per-PSU attrs are allocated via devm at probe time, sized to
	 * cfg->num_psus. No static cap array needed. */
	struct attribute_group group;
	const struct attribute_group *groups[2];
};

static ssize_t psu_present_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct nh_fpga_psu *p = dev_get_drvdata(dev);
	int psu_num;
	u32 reg;
	bool present;

	/* Extract PSU number from attribute name "psu<N>_present" */
	if (sscanf(attr->attr.name, "psu%d_present", &psu_num) != 1 ||
	    psu_num < 1 || psu_num > p->cfg->num_psus)
		return -EINVAL;

	reg = ioread32(p->base + p->cfg->present_reg_offset);
	present = !!(reg & p->cfg->present_masks[psu_num - 1]);
	if (p->cfg->active_low)
		present = !present;

	/* 1 for present, 0 for absent (FBOSS convention) */
	return sysfs_emit(buf, "%d\n", present ? 1 : 0);
}

static int nh_fpga_psu_probe(struct auxiliary_device *aux_dev,
			     const struct auxiliary_device_id *id)
{
	struct nh_fpga_aux_dev *fpga_aux_dev;
	const struct nh_platform_cfg *platform_cfg;
	struct nh_fpga_psu *p;
	u16 device_id;
	int i;

	fpga_aux_dev = container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);
	device_id = fpga_aux_dev->parent->pdev->device;

	platform_cfg = nh_get_platform(device_id);
	if (!platform_cfg || !platform_cfg->psu_present_cfg) {
		dev_err(&aux_dev->dev,
			"No PSU present config for device ID: 0x%04x\n",
			device_id);
		return -ENODEV;
	}

	if (platform_cfg->psu_present_cfg->present_reg_offset ==
	    NH_FPGA_PSU_OFFSET_UNSET) {
		dev_warn(
			&aux_dev->dev,
			"PSU-present register not yet characterized for 0x%04x; driver idle\n",
			device_id);
		return -ENODEV;
	}

	p = devm_kzalloc(&aux_dev->dev, sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	p->aux_dev = aux_dev;
	p->base = fpga_aux_dev->csr_base;
	p->cfg = platform_cfg->psu_present_cfg;

	if (p->cfg->num_psus == 0) {
		dev_err(&aux_dev->dev, "num_psus is zero in platform config\n");
		return -EINVAL;
	}

	/* Build per-PSU attrs dynamically, no static cap array needed.
	 * All allocations are devm. */
	{
		struct device_attribute *attrs;
		struct attribute **attr_ptrs;

		attrs = devm_kcalloc(&aux_dev->dev, p->cfg->num_psus,
				     sizeof(*attrs), GFP_KERNEL);
		attr_ptrs = devm_kcalloc(&aux_dev->dev, p->cfg->num_psus + 1,
					 sizeof(*attr_ptrs), GFP_KERNEL);
		if (!attrs || !attr_ptrs)
			return -ENOMEM;

		for (i = 0; i < p->cfg->num_psus; i++) {
			attrs[i].attr.name =
				devm_kasprintf(&aux_dev->dev, GFP_KERNEL,
					       "psu%d_present", i + 1);
			if (!attrs[i].attr.name)
				return -ENOMEM;
			sysfs_attr_init(&attrs[i].attr);
			attrs[i].attr.mode = 0444;
			attrs[i].show = psu_present_show;
			attr_ptrs[i] = &attrs[i].attr;
		}
		attr_ptrs[p->cfg->num_psus] = NULL;
		p->group.attrs = attr_ptrs;
	}
	p->groups[0] = &p->group;
	p->groups[1] = NULL;

	auxiliary_set_drvdata(aux_dev, p);

	/* Register via hwmon so the psu<N>_present attrs land under a hwmon[N]
	 * subdir, which is where platform_manager's /run/devmap/sensors
	 * resolver (resolveSensorPath) looks.
	 */
	p->hwmon_dev = devm_hwmon_device_register_with_groups(
		&aux_dev->dev, "nh_fpga_psu", p, p->groups);
	if (IS_ERR(p->hwmon_dev)) {
		dev_err(&aux_dev->dev, "Failed to register hwmon device: %ld\n",
			PTR_ERR(p->hwmon_dev));
		return PTR_ERR(p->hwmon_dev);
	}

	dev_info(&aux_dev->dev,
		 "NH FPGA PSU presence driver registered (%u PSUs)\n",
		 p->cfg->num_psus);
	return 0;
}

static void nh_fpga_psu_remove(struct auxiliary_device *aux_dev)
{
	auxiliary_set_drvdata(aux_dev, NULL);
	dev_info(&aux_dev->dev, "NH FPGA PSU presence driver removed\n");
}

static const struct auxiliary_device_id nh_fpga_psu_aux_id_table[] = {
	{
		.name = "fbiob_pci.psu_present",
	},
	{},
};
MODULE_DEVICE_TABLE(auxiliary, nh_fpga_psu_aux_id_table);

static struct auxiliary_driver nh_fpga_psu_aux_driver = {
	.name = DRIVER_NAME,
	.probe = nh_fpga_psu_probe,
	.remove = nh_fpga_psu_remove,
	.id_table = nh_fpga_psu_aux_id_table,
};

static int __init nh_fpga_psu_init_module(void)
{
	int ret;

	pr_info("NH FPGA PSU presence driver v%s loading\n", DRIVER_VERSION);

	ret = auxiliary_driver_register(&nh_fpga_psu_aux_driver);
	if (ret) {
		pr_err("Failed to register auxiliary driver: %d\n", ret);
		return ret;
	}

	pr_info("NH FPGA PSU presence driver loaded successfully\n");
	return 0;
}

static void __exit nh_fpga_psu_exit_module(void)
{
	pr_info("NH FPGA PSU presence driver unloading\n");
	auxiliary_driver_unregister(&nh_fpga_psu_aux_driver);
}

module_init(nh_fpga_psu_init_module);
module_exit(nh_fpga_psu_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Maisey Perelonia <maisey@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA PSU Presence Driver");
MODULE_VERSION(DRIVER_VERSION);
