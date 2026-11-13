// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#ifndef _NH_FPGA_FLASH_SEL_H_
#define _NH_FPGA_FLASH_SEL_H_

#include <linux/types.h>

/* Flash select bits in SH0 for the switch card chain type in SH0 bits 3:0. */
struct nh_flash_sel_layout {
	u8 chain_type;
	u8 grab_bit;
	u8 sel_bit;
	const char *sel_name;
	u8 reload_bit;
};

struct nh_flash_sel_cfg {
	u32 spi_enable_reg;
	u8 spi_enable_bit;
	u32 sh0_reg;
	const struct nh_flash_sel_layout *layouts;
	u32 num_layouts;
};

#endif /* _NH_FPGA_FLASH_SEL_H_ */
