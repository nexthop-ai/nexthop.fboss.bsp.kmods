// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include "nh_fpga_fbiob.h"

/**
 * nh_fpga_aux_device_release - Release function for auxiliary device
 * @dev: Device being released
 *
 * This function is called when the auxiliary device is being freed.
 */
static void nh_fpga_aux_device_release(struct device *dev)
{
	struct auxiliary_device *aux_dev = to_auxiliary_dev(dev);
	struct nh_fpga_aux_dev *fpga_aux_dev;

	if (!dev || !aux_dev) {
		pr_err("nh_fpga_aux_device_release: NULL device\n");
		return;
	}

	fpga_aux_dev = container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);
	pr_debug("Releasing auxiliary device %s.%u\n",
		 fpga_aux_dev->dev_info.id.name, fpga_aux_dev->dev_info.id.id);
	kfree(fpga_aux_dev);
}

/**
 * nh_fpga_create_aux_device - Create an auxiliary device
 * @fbiob_dev: Parent FPGA device
 * @dev_info: Device information
 *
 * Creates and registers an auxiliary device with the provided information.
 */
int nh_fpga_create_aux_device(struct nh_fpga_pci_dev *fbiob_dev,
			      struct fbiob_aux_data *dev_info)
{
	struct nh_fpga_aux_dev *aux_dev = NULL;
	int ret = 0;

	/* Lock to protect auxiliary device list access */
	mutex_lock(&fbiob_dev->lock);

	/* Check if device already exists */
	list_for_each_entry(aux_dev, &fbiob_dev->aux_dev_list, node) {
		if (strcmp(aux_dev->dev_info.id.name, dev_info->id.name) == 0 &&
		    aux_dev->dev_info.id.id == dev_info->id.id) {
			dev_err(fbiob_dev->dev, "Device %s.%u already exists\n",
				dev_info->id.name, dev_info->id.id);
			ret = -EEXIST;
			goto err_unlock;
		}
	}

	/* Allocate auxiliary device */
	aux_dev = kzalloc(sizeof(*aux_dev), GFP_KERNEL);
	if (!aux_dev) {
		ret = -ENOMEM;
		goto err_unlock;
	}

	/* Initialize auxiliary device */
	aux_dev->parent = fbiob_dev;
	memcpy(&aux_dev->dev_info, dev_info, sizeof(*dev_info));

	/* Map device registers */
	aux_dev->csr_base = fbiob_dev->mmio_base + dev_info->csr_offset;

	/* Only map iobuf_base if offset is valid */
	if (dev_info->iobuf_offset != FBIOB_INVALID_OFFSET) {
		aux_dev->iobuf_base =
			fbiob_dev->mmio_base + dev_info->iobuf_offset;
	} else {
		aux_dev->iobuf_base = NULL;
	}

	/* Initialize auxiliary device structure. Point name at our own copy
	 * of dev_info; the caller's buffer goes away when the ioctl returns. */
	aux_dev->aux_dev.name = aux_dev->dev_info.id.name;
	aux_dev->aux_dev.id = dev_info->id.id;
	aux_dev->aux_dev.dev.parent = &fbiob_dev->pdev->dev;
	aux_dev->aux_dev.dev.release = nh_fpga_aux_device_release;

	ret = auxiliary_device_init(&aux_dev->aux_dev);
	if (ret) {
		dev_err(fbiob_dev->dev,
			"Failed to initialize auxiliary device: %d\n", ret);
		goto err_free;
	}

	/* Register auxiliary device */
	ret = auxiliary_device_add(&aux_dev->aux_dev);
	if (ret) {
		dev_err(fbiob_dev->dev, "Failed to add auxiliary device: %d\n",
			ret);
		goto err_uninit;
	}

	/* Add to device list */
	list_add_tail(&aux_dev->node, &fbiob_dev->aux_dev_list);

	mutex_unlock(&fbiob_dev->lock);

	dev_info(fbiob_dev->dev, "Created auxiliary device %s.%u\n",
		 dev_info->id.name, dev_info->id.id);
	return 0;

err_uninit:
	/* After a successful auxiliary_device_init(), uninit drops the
	 * refcount and the release callback frees aux_dev. Do not free it
	 * again here. */
	auxiliary_device_uninit(&aux_dev->aux_dev);
	goto err_unlock;
err_free:
	kfree(aux_dev);
err_unlock:
	mutex_unlock(&fbiob_dev->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(nh_fpga_create_aux_device);

/**
 * nh_fpga_remove_aux_device - Remove an auxiliary device
 * @fbiob_dev: Parent FPGA device
 * @dev_id: Device ID information
 *
 * Removes and unregisters the specified auxiliary device.
 */
int nh_fpga_remove_aux_device(struct nh_fpga_pci_dev *fbiob_dev,
			      struct fbiob_aux_id *dev_id)
{
	struct nh_fpga_aux_dev *aux_dev, *tmp;
	bool found = false;

	/* Lock to protect auxiliary device list access */
	mutex_lock(&fbiob_dev->lock);

	/* Find and remove the device */
	list_for_each_entry_safe(aux_dev, tmp, &fbiob_dev->aux_dev_list, node) {
		if (strcmp(aux_dev->dev_info.id.name, dev_id->name) == 0 &&
		    aux_dev->dev_info.id.id == dev_id->id) {
			found = true;

			/* Remove from list first to prevent new references */
			list_del(&aux_dev->node);

			/* Delete and uninitialize the device */
			auxiliary_device_delete(&aux_dev->aux_dev);
			auxiliary_device_uninit(&aux_dev->aux_dev);

			dev_info(fbiob_dev->dev,
				 "Removed auxiliary device %s.%u\n",
				 dev_id->name, dev_id->id);

			/* Note: kfree(aux_dev) is handled by the release function */
			break;
		}
	}

	mutex_unlock(&fbiob_dev->lock);

	if (!found) {
		dev_err(fbiob_dev->dev, "Device %s.%u not found\n",
			dev_id->name, dev_id->id);
		return -ENODEV;
	}

	return 0;
}
EXPORT_SYMBOL_GPL(nh_fpga_remove_aux_device);
