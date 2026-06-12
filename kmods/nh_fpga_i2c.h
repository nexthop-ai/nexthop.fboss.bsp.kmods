/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Nexthop Systems Inc. */

#ifndef _NH_FPGA_I2C_H_
#define _NH_FPGA_I2C_H_

#include <linux/i2c.h>
#include <linux/auxiliary_bus.h>
#include "fpga_axi_iic_common.h"

/* I2C controller structure */
struct nh_fpga_i2c {
	struct auxiliary_device *aux_dev;
	struct i2c_adapter adapter;
	struct fpga_axi_iic axi_iic; /* Common AXI IIC controller */

	/* i2c mux support */
	struct i2c_client *mux_client; /* Virtual mux client device */
	void __iomem *mux_reg; /* FPGA mux control register */
	int num_mux_channels; /* Number of mux channels */
};

/* Function prototypes */
int nh_fpga_i2c_probe(struct auxiliary_device *aux_dev,
		      const struct auxiliary_device_id *id);
void nh_fpga_i2c_remove(struct auxiliary_device *aux_dev);

#endif /* _NH_FPGA_I2C_H_ */
