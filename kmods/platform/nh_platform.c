// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2025 Nexthop Systems Inc.

#include <linux/kernel.h>
#include "nh_platform.h"

/* Per-SKU descriptors are registered here by each platform/<sku>.c
 * integration. The table is empty in the core build (no platforms). */
static const struct nh_platform_cfg *const nh_platforms[] = {};

const struct nh_platform_cfg *nh_get_platform(u16 device_id)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(nh_platforms); i++)
		if (nh_platforms[i]->device_id == device_id)
			return nh_platforms[i];
	return NULL;
}
