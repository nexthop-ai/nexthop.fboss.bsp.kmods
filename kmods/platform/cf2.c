// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2026 Nexthop Systems Inc.

#include <linux/types.h>
#include "nh_platform.h"

static const struct mux_config cf2_mux[] = {
	{ 0, 5, 0x0018, "MUX control" },
};

const struct nh_platform_cfg nh_platform_cf2 = {
	.device_id = NH_FPGA_DEVICE_CF2,
	.name = "CF2",
	.mux_cfg = cf2_mux,
	.mux_count = ARRAY_SIZE(cf2_mux),
	/* led, fan, port_led, xcvr: NULL/0 — not present on this platform */
};
