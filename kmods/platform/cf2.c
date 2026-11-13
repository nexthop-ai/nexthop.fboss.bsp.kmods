// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#include <linux/types.h>
#include "nh_platform.h"

static const struct mux_config cf2_mux[] = {
	{ 0, 5, 0x0018, "MUX control" },
};

/* Interrupt topology: 1 MSI vector, 11 masters */
static const struct nh_fpga_msi_domain_cfg cf2_msi_domains[] = {
	{ .chip_name = "cf2-msi-0",
	  .ier_offset = 0x20,
	  .isr_offset = 0x24,
	  .first_master = 0,
	  .last_master = 10,
	  .first_hw_irq = 8 },
};

static const struct nh_fpga_irq_cfg cf2_irq_cfg = {
	.domains = cf2_msi_domains,
	.num_domains = ARRAY_SIZE(cf2_msi_domains),
	.i2c_csr_base = 0x40000,
	.i2c_csr_channel_size = 0x200,
	.ref_clk_hz = 25000000,
	.min_fpga_version = 0x01,
};

static const struct nh_flash_sel_layout cf2_flash_sel_layouts[] = {
	/* 24-bit chain, dual-FPGA switch card: 0 = FPGA0 flash, 1 = FPGA1 */
	{ .chain_type = 0x1,
	  .grab_bit = 19,
	  .sel_bit = 18,
	  .sel_name = "flash_sel",
	  .reload_bit = 16 },
	/* 16-bit chain, single-FPGA switch card: 0 = SPI, 1 = JTAG */
	{ .chain_type = 0x2,
	  .grab_bit = 11,
	  .sel_bit = 10,
	  .sel_name = "jtag_spi_sel",
	  .reload_bit = 8 },
};

static const struct nh_flash_sel_cfg cf2_flash_sel_cfg = {
	.spi_enable_reg = 0x0004,
	.spi_enable_bit = 0,
	.sh0_reg = 0x0044,
	.layouts = cf2_flash_sel_layouts,
	.num_layouts = ARRAY_SIZE(cf2_flash_sel_layouts),
};

const struct nh_platform_cfg nh_platform_cf2 = {
	.device_id = NH_FPGA_DEVICE_CF2,
	.name = "CF2",
	.mux_cfg = cf2_mux,
	.mux_count = ARRAY_SIZE(cf2_mux),
	.irq_cfg = &cf2_irq_cfg,
	.flash_sel_cfg = &cf2_flash_sel_cfg,
	/* led, fan, port_led, xcvr: NULL/0 — not present on this platform */
};
