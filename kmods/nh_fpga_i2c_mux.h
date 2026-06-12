/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Nexthop Systems Inc. */

#ifndef _NH_FPGA_I2C_MUX_H_
#define _NH_FPGA_I2C_MUX_H_

#include <linux/types.h>

/*
 * Platform data for FPGA mux devices
 * This is passed to the custom FPGA mux driver via platform_data
 */
struct fpga_mux_platform_data {
	void __iomem *mux_reg; /* FPGA mux control register */
	int num_channels; /* Number of mux channels */
};

#endif /* _NH_FPGA_I2C_MUX_H_ */
