// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#ifndef _NH_FPGA_PSU_H_
#define _NH_FPGA_PSU_H_

#include <linux/types.h>

struct nh_platform_cfg; /* forward declaration */

/*
 * Sentinel for a not-yet-characterized PSU-present register: the driver
 * stays idle (probe returns -ENODEV) rather than read an unknown register
 * and report bogus presence. Set the real offset to enable.
 */
#define NH_FPGA_PSU_OFFSET_UNSET 0xFFFFFFFFU

/*
 * Per-platform layout of the PSU-present register.
 *
 * A single 32-bit register within the aux device's CSR block carries one
 * presence bit per PSU. The CSR block base is set by the fpgaIpBlockConfig
 * csrOffset in platform_manager.json; present_reg_offset is relative to
 * that base.
 *
 * num_psus: number of PSUs on this platform. Set to ARRAY_SIZE() of the
 * platform's present_masks array — no hardcoded cap needed.
 *
 * active_low: when true the hardware bit reads 0 for "present", so the driver
 * inverts it before reporting.
 */
struct psu_present_cfg {
	u32 present_reg_offset;
	u8 num_psus;
	bool active_low;
	const u32 *present_masks;
};

#endif /* _NH_FPGA_PSU_H_ */
