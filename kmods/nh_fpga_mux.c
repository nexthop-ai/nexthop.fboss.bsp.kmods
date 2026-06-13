// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Nexthop Systems Inc.

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/sysfs.h>
#include "nh_fpga_i2c_mux.h"

#define DRIVER_NAME "nh_fpga_mux"
#define DRIVER_VERSION "1.0"

/* FPGA mux device structure */
struct fpga_mux {
	struct i2c_mux_core *muxc;
	void __iomem *mux_reg;
	int num_channels;
};

/* Mux select function - writes to FPGA register */
static int fpga_mux_select(struct i2c_mux_core *muxc, u32 chan)
{
	struct fpga_mux *mux = i2c_mux_priv(muxc);

	/* Write channel selection to the FPGA mux register. Probe allows up
	 * to 8 channels, so the select field is 3 bits; the old 2-bit mask
	 * silently aliased channel 4 onto channel 0 (cf2 has 5). */
	iowrite32(chan & 0x7, mux->mux_reg);

	dev_dbg(&muxc->parent->dev, "FPGA mux selected channel %u\n", chan);
	return 0;
}

/* Mux deselect function - optional, no action needed for this FPGA */
static int fpga_mux_deselect(struct i2c_mux_core *muxc, u32 chan)
{
	/* No deselect action needed for this FPGA */
	return 0;
}

/* I2C device probe function */
static int fpga_mux_probe(struct i2c_client *client)
{
	struct fpga_mux_platform_data *pdata = dev_get_platdata(&client->dev);
	struct fpga_mux *mux;
	int i, ret;

	if (!pdata) {
		dev_err(&client->dev, "No platform data provided\n");
		return -EINVAL;
	}

	if (!pdata->mux_reg) {
		dev_err(&client->dev, "No mux register provided\n");
		return -EINVAL;
	}

	if (pdata->num_channels <= 0 || pdata->num_channels > 8) {
		dev_err(&client->dev, "Invalid number of channels: %d\n",
			pdata->num_channels);
		return -EINVAL;
	}

	/* Allocate mux structure */
	mux = devm_kzalloc(&client->dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->mux_reg = pdata->mux_reg;
	mux->num_channels = pdata->num_channels;

	/* Create mux core - this creates the sysfs structure FBOSS expects */
	mux->muxc = i2c_mux_alloc(client->adapter, &client->dev,
				  pdata->num_channels, 0, 0, fpga_mux_select,
				  fpga_mux_deselect);
	if (!mux->muxc) {
		dev_err(&client->dev, "Failed to allocate mux core\n");
		return -ENOMEM;
	}

	mux->muxc->priv = mux;

	/* Add channels - this creates the channel-* symlinks for FBOSS discovery */
	for (i = 0; i < pdata->num_channels; i++) {
		ret = i2c_mux_add_adapter(mux->muxc, 0, i);
		if (ret) {
			dev_err(&client->dev,
				"Failed to add mux channel %d: %d\n", i, ret);
			/* Remove the symlinks created for earlier channels
			 * before their adapters are deleted. */
			while (--i >= 0) {
				char channel_name[16];

				snprintf(channel_name, sizeof(channel_name),
					 "channel-%d", i);
				sysfs_remove_link(&client->dev.kobj,
						  channel_name);
			}
			i2c_mux_del_adapters(mux->muxc);
			return ret;
		}

		/* Set FBOSS-compatible adapter name for the mux channel */
		if (i < mux->muxc->num_adapters && mux->muxc->adapter[i]) {
			struct i2c_adapter *chan_adapter =
				mux->muxc->adapter[i];
			char channel_name[16];

			/* Extract parent adapter name and create FBOSS format */
			/* Parent name format: "FPGA 7012 I2C Adapter #10" */
			/* Target format: "FPGA 7012 I2C Adapter #10 Channel #0" */
			snprintf(chan_adapter->name, sizeof(chan_adapter->name),
				 "%s Channel #%d", client->adapter->name, i);

			/* Create FBOSS-compatible channel symlink in client device directory */
			snprintf(channel_name, sizeof(channel_name),
				 "channel-%d", i);

			/* Remove existing symlink if it exists */
			sysfs_remove_link(&client->dev.kobj, channel_name);

			ret = sysfs_create_link(&client->dev.kobj,
						&chan_adapter->dev.kobj,
						channel_name);
			if (ret) {
				dev_warn(
					&client->dev,
					"Failed to create FBOSS channel symlink %s: %d\n",
					channel_name, ret);
			}

			dev_info(&client->dev,
				 "Set FBOSS name for channel %d: %s\n", i,
				 chan_adapter->name);
		}

		dev_dbg(&client->dev, "Added mux channel %d\n", i);
	}

	i2c_set_clientdata(client, mux);

	dev_info(&client->dev,
		 "FPGA mux with %d channels registered at 0x%02x\n",
		 pdata->num_channels, client->addr);

	return 0;
}

/* I2C device remove function */
static void fpga_mux_remove(struct i2c_client *client)
{
	struct fpga_mux *mux;
	int i;

	if (!client) {
		return;
	}

	mux = i2c_get_clientdata(client);
	if (!mux) {
		return;
	}

	/* Clear client data first to prevent further access */
	i2c_set_clientdata(client, NULL);

	if (mux->muxc) {
		/* Remove FBOSS channel symlinks first */
		for (i = 0; i < mux->num_channels; i++) {
			char channel_name[16];
			snprintf(channel_name, sizeof(channel_name),
				 "channel-%d", i);
			sysfs_remove_link(&client->dev.kobj, channel_name);
		}

		/* Remove mux adapters */
		i2c_mux_del_adapters(mux->muxc);
	}
}

/* I2C device ID table */
static const struct i2c_device_id fpga_mux_id[] = { { "fpga-mux", 0 }, {} };
MODULE_DEVICE_TABLE(i2c, fpga_mux_id);

/* I2C driver structure */
static struct i2c_driver fpga_mux_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = fpga_mux_probe,
	.remove = fpga_mux_remove,
	.id_table = fpga_mux_id,
};

/* Module initialization */
static int __init fpga_mux_init(void)
{
	int ret;

	pr_info("NH FPGA Mux driver v%s loading\n", DRIVER_VERSION);

	ret = i2c_add_driver(&fpga_mux_driver);
	if (ret) {
		pr_err("Failed to register I2C driver: %d\n", ret);
		return ret;
	}

	pr_info("NH FPGA Mux driver loaded successfully\n");
	return 0;
}

/* Module cleanup */
static void __exit fpga_mux_exit(void)
{
	pr_info("NH FPGA Mux driver unloading\n");
	i2c_del_driver(&fpga_mux_driver);
	pr_info("NH FPGA Mux driver unloaded\n");
}

module_init(fpga_mux_init);
module_exit(fpga_mux_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Arif Mohammad <marif@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FPGA I2C Mux Driver");
MODULE_VERSION(DRIVER_VERSION);
MODULE_SOFTDEP("pre: i2c-mux");
