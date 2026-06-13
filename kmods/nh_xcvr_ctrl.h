/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Nexthop Systems Inc. */

#ifndef _NH_XCVR_CTRL_H_
#define _NH_XCVR_CTRL_H_

#include <linux/auxiliary_bus.h>

struct nh_platform_cfg; /* forward declaration — full definition in platform/nh_platform.h */

/* One contiguous range of ports sharing a single 32-bit (reset, lp_mode,
 * present) register triple. Each group maps to one physical 32-bit
 * register per attribute; the port's bit position within the register is
 * (port_num - start_port). A platform with multiple register windows
 * (per port card / mezzanine / FPGA-internal half) declares one entry
 * per window, so end_port - start_port is at most 31.
 */
struct xcvr_port_group {
	u32 start_port; /* inclusive, 1-based */
	u32 end_port; /* inclusive, 1-based; end_port - start_port < 32 */
	u32 reset_reg_offset;
	u32 lp_mode_reg_offset;
	u32 present_reg_offset;
};

/* Per-device xcvr control structure */
struct nh_xcvr_ctrl {
	struct auxiliary_device *aux_dev;
	void __iomem *base;
	u32 port_num;
	u32 bit_pos; /* bit within the register, = port_num - group->start_port */
	const struct xcvr_port_group *group; /* cached group for this port */
	const struct nh_platform_cfg
		*platform_cfg; /* Per-device platform descriptor */
};

/* Per-platform xcvr register layout */
struct xcvr_ctrl_config {
	const struct xcvr_port_group *groups;
	u32 num_groups;
};

/* Look up the port group whose [start_port, end_port] range covers
 * @port_num. Returns NULL if no group matches.
 */
const struct xcvr_port_group *
xcvr_ctrl_find_port_group(const struct xcvr_ctrl_config *cfg, u32 port_num);

#endif /* _NH_XCVR_CTRL_H_ */
