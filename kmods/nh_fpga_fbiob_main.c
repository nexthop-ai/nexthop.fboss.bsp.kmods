// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/bitops.h>
#include <linux/auxiliary_bus.h>
#include <linux/list.h>
#include <linux/delay.h>
#include "nh_fpga_fbiob.h"
#include "platform/nh_platform.h"

#define DRIVER_NAME "fbiob_pci"
#define DRIVER_VERSION "1.0"

#define FBIOB_MAJOR 0 /* Dynamic allocation */
#define FBIOB_MAX_DEVICES 16

static dev_t fbiob_devt;
static struct class *nh_fpga_class;
static DEFINE_MUTEX(nh_fpga_devices_lock);
static DECLARE_BITMAP(minor_bitmap, FBIOB_MAX_DEVICES);

MODULE_DEVICE_TABLE(pci, nh_fpga_pci_ids);

/* Character device operations */
static int fbiob_open(struct inode *inode, struct file *file)
{
	struct nh_fpga_pci_dev *fbiob_dev;

	fbiob_dev = container_of(inode->i_cdev, struct nh_fpga_pci_dev, cdev);
	file->private_data = fbiob_dev;

	dev_dbg(fbiob_dev->dev, "Device opened\n");
	return 0;
}

static int fbiob_release(struct inode *inode, struct file *file)
{
	struct nh_fpga_pci_dev *fbiob_dev = file->private_data;

	dev_dbg(fbiob_dev->dev, "Device released\n");
	return 0;
}

static long fbiob_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct nh_fpga_pci_dev *fbiob_dev = file->private_data;
	struct fbiob_aux_data aux_data;
	int ret = 0;

	if (_IOC_TYPE(cmd) != FBIOB_IOC_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case FBIOB_IOC_NEW_DEVICE:
		if (copy_from_user(&aux_data, (void __user *)arg,
				   sizeof(aux_data))) {
			dev_err(fbiob_dev->dev,
				"Failed to copy device info from user\n");
			return -EFAULT;
		}

		/* Ensure null termination */
		aux_data.id.name[sizeof(aux_data.id.name) - 1] = '\0';

		/* Validate offsets are within MMIO region */
		if (aux_data.csr_offset >= fbiob_dev->mmio_len ||
		    (aux_data.iobuf_offset != FBIOB_INVALID_OFFSET &&
		     aux_data.iobuf_offset >= fbiob_dev->mmio_len)) {
			dev_err(fbiob_dev->dev,
				"Device offset exceeds MMIO region (csr=0x%x, iobuf=0x%x, mmio_len=0x%lx)\n",
				aux_data.csr_offset, aux_data.iobuf_offset,
				(unsigned long)fbiob_dev->mmio_len);
			return -EINVAL;
		}

		dev_info(fbiob_dev->dev,
			 "FBIOB_IOC_NEW_DEVICE: Creating device '%s'\n",
			 aux_data.id.name);

		/* Create auxiliary bus device */
		ret = nh_fpga_create_aux_device(fbiob_dev, &aux_data);
		break;

	case FBIOB_IOC_DEL_DEVICE:
		if (copy_from_user(&aux_data, (void __user *)arg,
				   sizeof(aux_data))) {
			dev_err(fbiob_dev->dev,
				"Failed to copy device info from user\n");
			return -EFAULT;
		}

		/* Ensure null termination */
		aux_data.id.name[sizeof(aux_data.id.name) - 1] = '\0';

		dev_info(fbiob_dev->dev,
			 "FBIOB_IOC_DEL_DEVICE: Deleting device '%s'\n",
			 aux_data.id.name);

		/* Delete auxiliary device */
		ret = nh_fpga_remove_aux_device(fbiob_dev, &aux_data.id);
		break;

	default:
		ret = -ENOTTY;
		break;
	}

	return ret;
}

static const struct file_operations fbiob_fops = {
	.owner = THIS_MODULE,
	.open = fbiob_open,
	.release = fbiob_release,
	.unlocked_ioctl = fbiob_ioctl,
	.llseek = noop_llseek,
};

/* Find available minor number */
static int nh_fpga_get_minor(void)
{
	int minor;

	mutex_lock(&nh_fpga_devices_lock);
	minor = find_first_zero_bit(minor_bitmap, FBIOB_MAX_DEVICES);
	if (minor >= FBIOB_MAX_DEVICES) {
		mutex_unlock(&nh_fpga_devices_lock);
		return -ENOSPC;
	}
	set_bit(minor, minor_bitmap);
	mutex_unlock(&nh_fpga_devices_lock);
	return minor;
}

/* Release minor number */
static void nh_fpga_put_minor(int minor)
{
	if (minor >= 0 && minor < FBIOB_MAX_DEVICES) {
		mutex_lock(&nh_fpga_devices_lock);
		clear_bit(minor, minor_bitmap);
		mutex_unlock(&nh_fpga_devices_lock);
	}
}

/* PCIe probe function */
static int nh_fpga_pci_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct nh_fpga_pci_dev *fbiob_dev;
	int ret, minor;
	char dev_name[32];

	dev_info(&pdev->dev, "Probing NH FPGA PCIe device %04x:%04x\n",
		 pdev->vendor, pdev->device);

	minor = nh_fpga_get_minor();
	if (minor < 0) {
		dev_err(&pdev->dev, "No available minor numbers\n");
		return minor;
	}

	/* Then allocate resources */
	fbiob_dev = devm_kzalloc(&pdev->dev, sizeof(*fbiob_dev), GFP_KERNEL);
	if (!fbiob_dev) {
		nh_fpga_put_minor(minor); /* Clean up minor on memory failure */
		return -ENOMEM;
	}

	fbiob_dev->minor = minor;
	fbiob_dev->pdev = pdev;
	mutex_init(&fbiob_dev->lock);

	/* Enable PCIe device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to enable PCIe device: %d\n", ret);
		goto err_put_minor;
	}

	/* Request memory regions */
	ret = pci_request_regions(pdev, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request PCIe regions: %d\n",
			ret);
		goto err_disable_device;
	}

	/* Map MMIO region */
	fbiob_dev->mmio_len = pci_resource_len(pdev, 0);
	fbiob_dev->mmio_base = pci_iomap(pdev, 0, fbiob_dev->mmio_len);
	if (!fbiob_dev->mmio_base) {
		dev_err(&pdev->dev, "Failed to map MMIO region\n");
		ret = -ENOMEM;
		goto err_release_regions;
	}

	/* Set up DMA */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "Failed to set DMA mask\n");
			goto err_unmap;
		}
	}

	pci_set_master(pdev);

	/* Initialize the auxiliary device list before the character device
	 * goes live; an early ioctl must not walk an uninitialized list. */
	INIT_LIST_HEAD(&fbiob_dev->aux_dev_list);

	/* Create character device */
	cdev_init(&fbiob_dev->cdev, &fbiob_fops);
	fbiob_dev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&fbiob_dev->cdev, MKDEV(MAJOR(fbiob_devt), minor), 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add character device: %d\n",
			ret);
		goto err_unmap;
	}

	/* Create device node */
	snprintf(dev_name, sizeof(dev_name), "fbiob_%04x.%04x.%04x.%04x",
		 pdev->vendor, pdev->device, pdev->subsystem_vendor,
		 pdev->subsystem_device);

	fbiob_dev->dev = device_create(nh_fpga_class, &pdev->dev,
				       MKDEV(MAJOR(fbiob_devt), minor),
				       fbiob_dev, "%s", dev_name);
	if (IS_ERR(fbiob_dev->dev)) {
		ret = PTR_ERR(fbiob_dev->dev);
		dev_err(&pdev->dev, "Failed to create device: %d\n", ret);
		goto err_del_cdev;
	}

	/* Set driver data */
	pci_set_drvdata(pdev, fbiob_dev);

	dev_info(&pdev->dev,
		 "NH FPGA fbiob device initialized successfully as /dev/%s\n",
		 dev_name);

	return 0;

err_del_cdev:
	cdev_del(&fbiob_dev->cdev);
err_unmap:
	pci_iounmap(pdev, fbiob_dev->mmio_base);
err_release_regions:
	pci_release_regions(pdev);
err_disable_device:
	pci_disable_device(pdev);
err_put_minor:
	nh_fpga_put_minor(minor);
	return ret;
}

/* PCIe remove function */
static void nh_fpga_pci_remove(struct pci_dev *pdev)
{
	struct nh_fpga_pci_dev *fbiob_dev = pci_get_drvdata(pdev);

	if (!fbiob_dev)
		return;

	dev_info(&pdev->dev, "Removing NH FPGA fbiob device\n");

	/* Remove all auxiliary devices */
	{
		struct nh_fpga_aux_dev *aux_dev, *tmp;
		LIST_HEAD(devices_to_remove);

		/* First, move all devices to a temporary list while holding the lock */
		mutex_lock(&fbiob_dev->lock);
		list_splice_init(&fbiob_dev->aux_dev_list, &devices_to_remove);
		mutex_unlock(&fbiob_dev->lock);

		/* Now remove devices without holding the lock to avoid deadlocks */
		list_for_each_entry_safe(aux_dev, tmp, &devices_to_remove,
					 node) {
			list_del(&aux_dev->node);

			/* Delete and uninitialize the device */
			auxiliary_device_delete(&aux_dev->aux_dev);
			auxiliary_device_uninit(&aux_dev->aux_dev);
			/* Note: kfree(aux_dev) is handled by the release function */
		}

		/* Give time for all device cleanup to complete */
		msleep(50);
	}

	if (fbiob_dev->dev)
		device_destroy(nh_fpga_class,
			       MKDEV(MAJOR(fbiob_devt), fbiob_dev->minor));

	/* Remove character device */
	if (fbiob_dev->cdev.owner)
		cdev_del(&fbiob_dev->cdev);

	/* Unmap MMIO region */
	if (fbiob_dev->mmio_base)
		pci_iounmap(pdev, fbiob_dev->mmio_base);

	/* Release PCIe resources */
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	/* Release minor number last */
	nh_fpga_put_minor(fbiob_dev->minor);

	/* Clear driver data */
	pci_set_drvdata(pdev, NULL);
}

/* PCIe driver structure */
static struct pci_driver nh_fpga_pci_driver = {
	.name = DRIVER_NAME,
	.id_table = nh_fpga_pci_ids,
	.probe = nh_fpga_pci_probe,
	.remove = nh_fpga_pci_remove,
};

/* Module initialization */
static int __init nh_fpga_pci_init(void)
{
	int ret;

	pr_info("NH FPGA PCIe driver v%s loading\n", DRIVER_VERSION);

	/* Allocate character device numbers */
	ret = alloc_chrdev_region(&fbiob_devt, 0, FBIOB_MAX_DEVICES,
				  DRIVER_NAME);
	if (ret) {
		pr_err("Failed to allocate character device numbers: %d\n",
		       ret);
		return ret;
	}

	/* Create device class */
	nh_fpga_class = class_create(DRIVER_NAME);
	if (IS_ERR(nh_fpga_class)) {
		ret = PTR_ERR(nh_fpga_class);
		pr_err("Failed to create device class: %d\n", ret);
		goto err_unregister_chrdev;
	}

	/* Register PCIe driver */
	ret = pci_register_driver(&nh_fpga_pci_driver);
	if (ret) {
		pr_err("Failed to register PCIe driver: %d\n", ret);
		goto err_destroy_class;
	}

	pr_info("NH FPGA PCIe driver loaded successfully\n");
	return 0;

err_destroy_class:
	class_destroy(nh_fpga_class);
err_unregister_chrdev:
	unregister_chrdev_region(fbiob_devt, FBIOB_MAX_DEVICES);
	return ret;
}

/* Module cleanup */
static void __exit nh_fpga_pci_exit(void)
{
	pr_info("NH FPGA PCIe driver unloading\n");

	pci_unregister_driver(&nh_fpga_pci_driver);
	class_destroy(nh_fpga_class);
	unregister_chrdev_region(fbiob_devt, FBIOB_MAX_DEVICES);

	pr_info("NH FPGA PCIe driver unloaded\n");
}

module_init(nh_fpga_pci_init);
module_exit(nh_fpga_pci_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arif Mohammad <marif@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA PCIe Driver");
MODULE_VERSION(DRIVER_VERSION);
