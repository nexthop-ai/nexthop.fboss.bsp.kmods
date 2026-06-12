/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2025 Nexthop Systems Inc. */

#ifndef _NH_PLATFORM_H_
#define _NH_PLATFORM_H_

#include <linux/types.h>
#include <linux/pci.h>

/* PCI Vendor/Device IDs for NH FPGA platforms */
#define NH_FPGA_VENDOR_XILINX 0x10ee
#define NH_FPGA_DEVICE_CF2 0x7016
#define NH_FPGA_DEVICE_M4062NHP_FPGA0 0x7018
#define NH_FPGA_DEVICE_M4062NHP_FPGA1 0x7019

static const struct pci_device_id nh_fpga_pci_ids[] = {
	{ PCI_DEVICE(NH_FPGA_VENDOR_XILINX, NH_FPGA_DEVICE_CF2) },
	{ PCI_DEVICE(NH_FPGA_VENDOR_XILINX, NH_FPGA_DEVICE_M4062NHP_FPGA0) },
	{ PCI_DEVICE(NH_FPGA_VENDOR_XILINX, NH_FPGA_DEVICE_M4062NHP_FPGA1) },
	{ 0 }
};

/* Pull in subsystem struct definitions. These headers contain only
 * type/enum/macro definitions — no static data. */
#include "../nh_fpga_led_core.h"
#include "../nh_fpga_fan.h"
#include "../nh_fpga_port_led.h"
#include "../nh_xcvr_ctrl.h"
#include "../nh_fpga_i2c_masters.h"
#include "../nh_fpga_asic_temp.h"

/* Common LED color indices for standard bicolor (red/green) system LEDs */
enum nh_led_bicolor {
	NH_LED_BICOLOR_FAIL = 0, /* Red - failure/fault state */
	NH_LED_BICOLOR_GOOD = 1, /* Green - good/operational state */
	NH_LED_BICOLOR_NUM_COLORS
};

/* Shared color names for red/green bicolor LEDs */
static const char *const nh_led_rg_color_names[NH_LED_BICOLOR_NUM_COLORS] = {
	[NH_LED_BICOLOR_FAIL] = "red",
	[NH_LED_BICOLOR_GOOD] = "green",
};

/**
 * struct nh_platform_cfg - All per-SKU data aggregated in one place.
 *
 * Fields are NULL/0 for subsystems not present on a given platform.
 * Drivers must check for NULL before dereferencing subsystem pointers.
 */
struct nh_platform_cfg {
	u16 device_id;
	const char *name;

	/* LED subsystem (PSU/FAN/SYS LEDs) */
	const struct nh_led_core_config
		*led_core_cfg; /* Core LED driver (all types) */

	/* Fan/PWM subsystem */
	const struct fan_card_status_addr *fan_cfg;

	/* Port LED subsystem */
	const struct port_led_color_addr
		*port_led_cfg; /* array[PORT_LED_NUM_COLORS] */

	u32 num_ports_per_led;

	/* Transceiver control subsystem */
	const struct xcvr_ctrl_config *xcvr_cfg;

	/* I2C mux subsystem */
	const struct mux_config *mux_cfg;
	int mux_count;

	/* ASIC temperature subsystem */
	const struct asic_temp_config *asic_temp_cfg;
};

/**
 * nh_get_platform - Look up the platform descriptor for a PCI device ID.
 * @device_id: PCI device ID from pdev->device
 *
 * Returns a pointer to the read-only platform descriptor, or NULL if the
 * device ID is not recognised. Callers must check for NULL.
 *
 * This function is compiled into each module that uses it (not exported
 * via EXPORT_SYMBOL).
 */
const struct nh_platform_cfg *nh_get_platform(u16 device_id);

#endif /* _NH_PLATFORM_H_ */
