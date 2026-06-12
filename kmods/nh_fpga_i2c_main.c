// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/auxiliary_bus.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/stdarg.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/string.h>
#include "nh_fpga_i2c.h"
#include "nh_fpga_fbiob.h"
#include "nh_fpga_i2c_mux.h"
#include "fpga_axi_iic_common.h"
#include "nh_fpga_i2c_masters.h"
#include "platform/nh_platform.h"

#define DRIVER_NAME "nh_fpga_i2c"
#define DRIVER_VERSION "1.0"

/*
 * I2C master controllers occupy a contiguous block of the FPGA CSR space,
 * starting at NH_FPGA_I2C_BLOCK_BASE with one controller every
 * NH_FPGA_I2C_MASTER_STRIDE bytes. A controller's logical master index is its
 * position in that block, which is what mux_config.logical_master encodes.
 * These values are consistent across all supported SKUs.
 */
#define NH_FPGA_I2C_BLOCK_BASE 0x40000
#define NH_FPGA_I2C_MASTER_STRIDE 0x200

/* Configure mux support based on platform data and CSR offset */
static int configure_mux_support(struct nh_fpga_i2c *i2c,
				 struct nh_fpga_aux_dev *fpga_aux_dev,
				 struct auxiliary_device *aux_dev,
				 int master_id)
{
	u16 device_id = fpga_aux_dev->parent->pdev->device;
	u32 csr_offset = fpga_aux_dev->dev_info.csr_offset;
	int logical_master;
	const struct nh_platform_cfg *platform_cfg;
	int j;

	dev_dbg(&aux_dev->dev, "FPGA device ID: 0x%04x, Master ID: %d\n",
		device_id, master_id);

	/* Initialize mux support fields */
	i2c->num_mux_channels = 0;
	i2c->mux_reg = NULL;
	i2c->mux_client = NULL;

	/*
	 * Derive the logical master index from the controller's position in the
	 * FPGA CSR block, not from master_id. mux_config.logical_master is that
	 * positional index. The previous "master_id % 10" proxy aliased
	 * transceiver (OSFP) masters whose id shared a last digit with a
	 * mux-capable master onto the mux table, creating phantom muxes at 0x71
	 * and corrupting the i2c topology.
	 */
	if (csr_offset < NH_FPGA_I2C_BLOCK_BASE) {
		dev_dbg(&aux_dev->dev,
			"csr_offset 0x%x below i2c block base 0x%x — no mux\n",
			csr_offset, NH_FPGA_I2C_BLOCK_BASE);
		return 0;
	}
	if ((csr_offset - NH_FPGA_I2C_BLOCK_BASE) % NH_FPGA_I2C_MASTER_STRIDE) {
		dev_warn(
			&aux_dev->dev,
			"csr_offset 0x%x not aligned to master stride 0x%x — no mux\n",
			csr_offset, NH_FPGA_I2C_MASTER_STRIDE);
		return 0;
	}
	logical_master = (csr_offset - NH_FPGA_I2C_BLOCK_BASE) /
			 NH_FPGA_I2C_MASTER_STRIDE;

	platform_cfg = nh_get_platform(device_id);
	if (!platform_cfg) {
		dev_warn(&aux_dev->dev,
			 "Unknown device 0x%04x — no mux support\n", device_id);
		return 0;
	}

	if (!platform_cfg->mux_cfg) {
		dev_dbg(&aux_dev->dev, "%s: no mux support\n",
			platform_cfg->name);
		return 0;
	}

	/* Known platform with mux — search for this logical master */
	for (j = 0; j < platform_cfg->mux_count; j++) {
		if (platform_cfg->mux_cfg[j].logical_master == logical_master) {
			i2c->num_mux_channels =
				platform_cfg->mux_cfg[j].num_channels;
			i2c->mux_reg = fpga_aux_dev->parent->mmio_base +
				       platform_cfg->mux_cfg[j].reg_offset;
			dev_dbg(&aux_dev->dev,
				"%s master %d: %s, %d channels at offset 0x%04x\n",
				platform_cfg->name, logical_master,
				platform_cfg->mux_cfg[j].description,
				platform_cfg->mux_cfg[j].num_channels,
				platform_cfg->mux_cfg[j].reg_offset);
			return 0;
		}
	}

	dev_dbg(&aux_dev->dev, "%s master %d: no mux\n", platform_cfg->name,
		logical_master);
	return 0;
}

/* Forward declarations */
static int fpga_axi_iic_access(struct i2c_adapter *adap, struct i2c_msg *msgs,
			       int num);

/* Logging function for the common library */
static void nh_fpga_i2c_log(struct fpga_axi_iic *axi_iic, int level,
			    const char *fmt, ...)
{
	struct nh_fpga_i2c *i2c =
		container_of(axi_iic, struct nh_fpga_i2c, axi_iic);
	va_list args;
	char buf[256];

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	switch (level) {
	case AXI_IIC_LOG_ERR:
		dev_err(&i2c->adapter.dev, "%s", buf);
		break;
	case AXI_IIC_LOG_WARN:
		dev_warn(&i2c->adapter.dev, "%s", buf);
		break;
	case AXI_IIC_LOG_INFO:
		dev_info(&i2c->adapter.dev, "%s", buf);
		break;
	case AXI_IIC_LOG_DBG:
	default:
		dev_dbg(&i2c->adapter.dev, "%s", buf);
		break;
	}
}

static const struct i2c_algorithm axi_iic_algorithm = {
	.master_xfer = fpga_axi_iic_access,
	.functionality = fpga_axi_iic_func,
};

/* The AXI IIC dynamic mode read byte count is an 8 bit field; larger
 * reads truncate modulo 256 and bit 8 collides with the START flag.
 * Let the i2c core reject them before they reach the controller. */
static const struct i2c_adapter_quirks axi_iic_quirks = {
	.max_read_len = 255,
};

/* Create virtual I2C mux devices for FBOSS discovery */
static int create_fpga_mux_devices(struct nh_fpga_i2c *i2c, int master_id)
{
	struct i2c_board_info mux_info;
	struct i2c_client *mux_client;
	struct fpga_mux_platform_data *pdata;

	/* Only create mux for masters that have mux functionality */
	if (i2c->num_mux_channels == 0)
		return 0;

	/* Create a virtual mux device on this I2C bus */
	memset(&mux_info, 0, sizeof(mux_info));
	mux_info.addr = 0x71; /* Virtual i2c mux address */
	strscpy(mux_info.type, "fpga-mux",
		sizeof(mux_info.type)); /* Our custom mux type */

	/* Pass FPGA-specific data via platform_data */
	pdata = devm_kzalloc(&i2c->aux_dev->dev, sizeof(*pdata), GFP_KERNEL);
	if (!pdata)
		return -ENOMEM;

	pdata->mux_reg = i2c->mux_reg;
	pdata->num_channels = i2c->num_mux_channels;
	mux_info.platform_data = pdata;

	mux_client = i2c_new_client_device(&i2c->adapter, &mux_info);
	if (IS_ERR(mux_client)) {
		dev_err(&i2c->aux_dev->dev, "Failed to create mux device\n");
		return PTR_ERR(mux_client);
	}

	/* Store client for cleanup */
	i2c->mux_client = mux_client;

	dev_info(&i2c->aux_dev->dev,
		 "Created FPGA mux device at 0x%02x with %d channels\n",
		 mux_info.addr, i2c->num_mux_channels);

	return 0;
}

/* I2C master transfer function with retry logic */
static int fpga_axi_iic_access(struct i2c_adapter *adap, struct i2c_msg *msgs,
			       int num)
{
	struct nh_fpga_i2c *i2c = i2c_get_adapdata(adap);
	int ret;

	dev_dbg(&adap->dev,
		"I2C transfer: %d messages, first addr=0x%02x, len=%d, flags=0x%x\n",
		num, msgs[0].addr, msgs[0].len, msgs[0].flags);

	ret = fpga_axi_iic_xfer(&i2c->axi_iic, msgs, num);

	dev_dbg(&adap->dev, "I2C transfer result: %d\n", ret);
	return ret;
}

/* Auxiliary device probe function */
int nh_fpga_i2c_probe(struct auxiliary_device *aux_dev,
		      const struct auxiliary_device_id *id)
{
	struct nh_fpga_aux_dev *fpga_aux_dev;
	struct nh_fpga_i2c *i2c;
	int ret;
	int master_id;

	fpga_aux_dev = container_of(aux_dev, struct nh_fpga_aux_dev, aux_dev);

	/* Allocate I2C controller structure */
	i2c = devm_kzalloc(&aux_dev->dev, sizeof(*i2c), GFP_KERNEL);
	if (!i2c)
		return -ENOMEM;

	i2c->aux_dev = aux_dev;
	master_id = fpga_aux_dev->dev_info.id.id;

	dev_info(&aux_dev->dev,
		 "Initializing I2C Master %d at offset 0x%x, base=%p\n",
		 master_id, fpga_aux_dev->dev_info.csr_offset,
		 fpga_aux_dev->csr_base);

	i2c->axi_iic.base = fpga_aux_dev->csr_base;
	i2c->axi_iic.log_func = nh_fpga_i2c_log;
	i2c->axi_iic.log_data = i2c;

	ret = fpga_axi_iic_init(&i2c->axi_iic);
	if (ret) {
		dev_err(&aux_dev->dev,
			"Failed to initialize I2C controller: %d\n", ret);
		return ret;
	}

	/* Setup I2C adapter with FBOSS BSP compliant naming */
	i2c->adapter.owner = THIS_MODULE;
	i2c->adapter.algo = &axi_iic_algorithm;
	i2c->adapter.quirks = &axi_iic_quirks;
	i2c->adapter.dev.parent = &aux_dev->dev;
	i2c->adapter.dev.of_node = aux_dev->dev.of_node;
	snprintf(i2c->adapter.name, sizeof(i2c->adapter.name),
		 "FPGA %04x I2C Adapter #%u",
		 fpga_aux_dev->parent->pdev->device, master_id);

	i2c_set_adapdata(&i2c->adapter, i2c);

	/* Register I2C adapter */
	ret = devm_i2c_add_adapter(&aux_dev->dev, &i2c->adapter);
	if (ret) {
		dev_err(&aux_dev->dev, "Failed to add I2C adapter: %d\n", ret);
		return ret;
	}

	/* Configure mux support based on platform data and CSR offset */
	ret = configure_mux_support(i2c, fpga_aux_dev, aux_dev, master_id);
	if (ret) {
		dev_err(&aux_dev->dev, "Failed to configure mux support: %d\n",
			ret);
		return ret;
	}

	/* Create virtual mux devices for FBOSS discovery if mux is supported */
	if (i2c->num_mux_channels > 0) {
		ret = create_fpga_mux_devices(i2c, master_id);
		if (ret) {
			dev_err(&aux_dev->dev,
				"Failed to create mux devices: %d\n", ret);
			return ret;
		}
		dev_info(&aux_dev->dev,
			 "Created I2C adapter with %d mux channels\n",
			 i2c->num_mux_channels);
	} else {
		dev_info(&aux_dev->dev,
			 "Created I2C adapter without mux support\n");
	}

	/* Store I2C controller in auxiliary device data */
	auxiliary_set_drvdata(aux_dev, i2c);

	dev_info(&aux_dev->dev, "FPGA I2C controller registered as %s\n",
		 i2c->adapter.name);

	return 0;
}

/* Auxiliary device remove function */
void nh_fpga_i2c_remove(struct auxiliary_device *aux_dev)
{
	struct nh_fpga_i2c *i2c = auxiliary_get_drvdata(aux_dev);

	if (!i2c) {
		dev_warn(&aux_dev->dev, "I2C remove: controller is NULL\n");
		return;
	}

	/* Remove mux client device if present - do this BEFORE adapter removal */
	if (i2c->mux_client) {
		dev_info(&aux_dev->dev, "Removing mux client device\n");
		i2c_unregister_device(i2c->mux_client);
		i2c->mux_client = NULL;
	}

	/* Clear auxiliary device data to prevent further access */
	auxiliary_set_drvdata(aux_dev, NULL);

	/* With devm_i2c_add_adapter(), the adapter is automatically removed */
	dev_info(&aux_dev->dev, "FPGA I2C controller removed\n");
}

/* Auxiliary device ID table */
static const struct auxiliary_device_id nh_fpga_i2c_aux_id_table[] = {
	{
		.name = "fbiob_pci.i2c_master",
	},
	{
		.name = "fbiob_pci.i2c_master_ext",
	},
	{},
};
MODULE_DEVICE_TABLE(auxiliary, nh_fpga_i2c_aux_id_table);

/* Auxiliary driver structure */
static struct auxiliary_driver nh_fpga_i2c_aux_driver = {
	.name = DRIVER_NAME,
	.probe = nh_fpga_i2c_probe,
	.remove = nh_fpga_i2c_remove,
	.id_table = nh_fpga_i2c_aux_id_table,
};

/* Module initialization */
static int __init nh_fpga_i2c_init_module(void)
{
	int ret;

	pr_info("NH FPGA I2C driver v%s loading\n", DRIVER_VERSION);

	ret = auxiliary_driver_register(&nh_fpga_i2c_aux_driver);
	if (ret) {
		pr_err("Failed to register auxiliary driver: %d\n", ret);
		return ret;
	}

	pr_info("NH FPGA I2C driver loaded successfully\n");
	return 0;
}

/* Module cleanup */
static void __exit nh_fpga_i2c_exit_module(void)
{
	pr_info("NH FPGA I2C driver unloading\n");
	auxiliary_driver_unregister(&nh_fpga_i2c_aux_driver);
	pr_info("NH FPGA I2C driver unloaded\n");
}

module_init(nh_fpga_i2c_init_module);
module_exit(nh_fpga_i2c_exit_module);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arif Mohammad <marif@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA I2C Controller Driver");
MODULE_VERSION(DRIVER_VERSION);
