/*
 *  Copyright (c) 2026 Nexthop Systems Inc.
 *  SPDX-License-Identifier: GPL-2.0-only
 *
 * Placeholder kernel module for the Nexthop FBOSS BSP kmods package.
 *
 * This repository is currently a skeleton: it carries the build
 * infrastructure (Makefile, RPM spec, manifest, build scripts) needed to
 * package the BSP kernel modules, but the real driver sources have not been
 * migrated here yet. This trivial module exists so the full
 * make -> .ko -> rpmbuild pipeline can be exercised end-to-end and produce a
 * valid, installable kmods RPM. It will be removed once the real drivers land.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>

static int __init nh_bsp_stub_init(void)
{
	pr_info("nh_bsp_stub: Nexthop BSP kmods placeholder loaded\n");
	return 0;
}

static void __exit nh_bsp_stub_exit(void)
{
	pr_info("nh_bsp_stub: Nexthop BSP kmods placeholder unloaded\n");
}

module_init(nh_bsp_stub_init);
module_exit(nh_bsp_stub_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Nexthop <support@nexthop.ai>");
MODULE_DESCRIPTION("Nexthop FBOSS BSP placeholder kernel module");
MODULE_VERSION("1.0.0");
