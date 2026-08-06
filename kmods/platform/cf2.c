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

const struct nh_platform_cfg nh_platform_cf2 = {
	.device_id = NH_FPGA_DEVICE_CF2,
	.name = "CF2",
	.mux_cfg = cf2_mux,
	.mux_count = ARRAY_SIZE(cf2_mux),
	.irq_cfg = &cf2_irq_cfg,
	/* led, fan, port_led, xcvr: NULL/0 — not present on this platform */
};
